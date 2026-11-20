#include "workstation_delivery.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QRandomGenerator>
#include <QBuffer>
#include <QtCore/private/qzipwriter_p.h>
#include <QUuid>

namespace {
QString digest(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QString writeJson(const QString& path, const QJsonObject& object) {
    QSaveFile file(path);
    const auto bytes = QJsonDocument(object).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return "无法保存本地交接记录：" + path;
    return {};
}
}

Job Delivery::compile(const QString& source, const Job& nativeTemplate, ConversionSettings settings, bool includeReturn) {
    auto job = readGcodeJob(source, nativeTemplate, settings);
    if (!job.error.isEmpty() || !includeReturn) return job;
    // Copy the exact reviewed return pair; never infer a collision-free return path.
    for (const auto* member : {"go_home.srp", "go_home.srd"}) {
        if (!nativeTemplate.reviewed || !nativeTemplate.files.contains(member) || nativeTemplate.files.value(member).isEmpty()) {
            job.error = "已核对模板中缺少完整 go_home 程序，未生成三程序工程。";
            job.files.clear(); return job;
        }
        job.files.insert(member, nativeTemplate.files.value(member));
    }
    return job;
}

QJsonObject Delivery::manifest(const Job& job, const QString& source) {
    QJsonObject hashes;
    QJsonArray programs;
    for (auto it = job.files.cbegin(); it != job.files.cend(); ++it) {
        hashes.insert(it.key(), digest(it.value()));
        if (it.key().endsWith(".srp")) programs.append(QJsonObject{
            {"program", it.key().chopped(4)}, {"gdi_number", QJsonValue::Null}, {"binding_confirmed", false}});
    }
    return {{"schema", 1}, {"status", "PREPARED_NOT_UPLOADED"}, {"source", source},
        {"source_sha256", digest(job.inputBytes)}, {"local_project", job.project}, {"main_program", job.program},
        {"native_sha256", hashes}, {"programs", programs}, {"remote_directory", QJsonValue::Null},
        {"dry_run", true}, {"process_outputs_configured", false}, {"scene_verified_automatically", false},
        {"gdi_binding_required", true}, {"robot_loaded", false}, {"robot_enabled", false}, {"robot_started", false}};
}

Delivery::Package Delivery::save(const Job& job, const QString& source, const QString& root) {
    Package package;
    if (!job.runnable() || job.nativeInput || job.files.isEmpty()) { package.error = "转换未通过，未保存交付工程。"; return package; }
    QDir parent(root);
    if (!parent.mkpath(".")) { package.error = "无法创建本地作业目录：" + root; return package; }
    const auto id = "gcode_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz_") +
        QUuid::createUuid().toString(QUuid::Id128).left(8);
    if (!parent.mkdir(id)) { package.error = "无法创建独立作业目录，未覆盖既有文件。"; return package; }
    package.directory = parent.filePath(id);
    package.zip = QDir(package.directory).filePath("GcodePreview.zip");
    package.manifest = QDir(package.directory).filePath("delivery.json");
    package.error = exportStepJob(job, package.zip);
    if (!package.error.isEmpty()) return package;
    QFile archive(package.zip);
    if (!archive.open(QIODevice::ReadOnly)) { package.error = "无法读取已保存的工程。"; return package; }
    auto object = manifest(job, source);
    object.insert("zip", package.zip); object.insert("zip_sha256", digest(archive.readAll()));
    package.error = writeJson(package.manifest, object);
    return package;
}

QString Delivery::record(const Package& package, const Job& job, const QJsonObject& transfer) {
    QFile file(package.manifest);
    if (!file.open(QIODevice::ReadOnly)) return "无法读取本地交接记录；请保留文件桥原始回执。";
    QJsonParseError error;
    auto object = QJsonDocument::fromJson(file.readAll(), &error).object(); file.close();
    if (error.error != QJsonParseError::NoError || object.value("schema").toInt() != 1)
        return "本地交接记录无效；请保留文件桥原始回执。";
    const bool complete = transfer.value("status").toString() == "COMPLETE";
    object.insert("status", complete ? "UPLOADED_GDI_BINDING_PENDING" : "UPLOAD_NOT_COMPLETE");
    object.insert("transfer", transfer);
    object.insert("remote_directory", complete ? transfer.value("remote_directory") : QJsonValue(QJsonValue::Null));
    object.insert("operator_handoff", handoff(job, transfer));
    // A successful transfer never establishes a cabinet program number or motion state.
    const auto recordError = writeJson(package.manifest, object);
    if (!recordError.isEmpty()) return recordError;
    QSaveFile instructions(QDir(package.directory).filePath("handoff.txt"));
    const auto text = handoff(job, transfer).toUtf8();
    if (!instructions.open(QIODevice::WriteOnly) || instructions.write(text) != text.size() || !instructions.commit())
        return "上传回执及delivery.json已保留，但交接文本保存失败。";
    return {};
}

QString Delivery::handoff(const Job& job, const QJsonObject& transfer) {
    if (transfer.value("status").toString() != "COMPLETE")
        return "上传尚未整组验证成功，不能把本次工程视为柜内可用作业。保留回执核对；不自动重试。";
    QStringList text{"整组上传和逐字节回读已完成。", "新工程：" + transfer.value("remote_directory").toString(),
        "", "请在示教器将下列程序绑定到现场确认可用的 GDI 编号（1～255）："};
    if (job.files.contains("capture_home.srp")) text << "• capture_home：记录本工程起点，不发运动指令。";
    text << "• " + job.program + "：本次 G 代码轨迹（空跑）。";
    if (job.files.contains("go_home.srp")) text << "• go_home：原模板关节 PTP 返回；回程路径须单独核对。";
    text << "" << "编号目前未分配。旧工程的 1、2、3 号不会自动指向新目录。"
         << "绑定保存并核对工程/程序后，再回到「PLC设备控制」页填写实际编号：登记 → 加载 → 人工确认加载 → 核对使能 → 单独运行。"
         << "每次换程先确认停止。运行前仍须验证本次轨迹、工具/坐标、起点与现场条件。"
         << "文件上传没有加载、使能或启动机器人，也没有修改 GDI 绑定。";
    return text.join('\n');
}

Delivery::Source Delivery::readSource(const QString& path) {
    Source source; source.path = path;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 32768) source.error = "G 代码无法读取或超过 32 KiB。";
    else source.bytes = file.readAll();
    return source;
}

Delivery::Source Delivery::currentSource(const Job& job) {
    if (!job.nativeInput && !job.inputPath.isEmpty()) return {job.inputPath, job.inputBytes, job.start, {}};
    if (job.automaticPatch) {
        auto program = job.sourceProgram;
        if (!job.programGcodes.contains(program)) program = job.mappedProgram;
        if (!job.programGcodes.contains(program) && !job.programGcodes.isEmpty()) program = job.programGcodes.firstKey();
        if (job.programGcodes.contains(program)) {
            const auto& snapshot = job.programGcodes[program];
            return {job.programSources.value(program), snapshot.bytes, snapshot.start, {}};
        }
        if (job.sourceIsGcode && job.programSources.contains(job.sourceProgram))
            return {job.programSources.value(job.sourceProgram), job.mappedBytes, job.start, {}};
    }
    if (!job.mappedBytes.isEmpty() && !job.mappedProgram.isEmpty())
        return {job.mappedProgram + ".nc", job.mappedBytes, job.mappedStart, {}};
    return {};
}

Delivery::AutoJob Delivery::compileAuto(const QStringList& sources, const Job& base, ConversionSettings settings) {
    QList<Source> snapshots;
    for (const auto& path : sources) snapshots.append(readSource(path));
    return compileAutoSnapshots(snapshots, base, settings);
}

Delivery::AutoJob Delivery::compileAutoSnapshots(const QList<Source>& snapshots, const Job& base, ConversionSettings settings) {
    QStringList sources;
    for (const auto& source : snapshots) sources.append(source.path);
    AutoJob result; result.sources = sources;
    if (sources.isEmpty() || sources.size() > 15) { result.error = "请选择1～15个G代码文件，按显示顺序执行。"; return result; }
    for (auto& tag : result.tag) tag = quint16(QRandomGenerator::global()->bounded(1,32768));
    result.revision = QString("%1-%2-%3").arg(result.tag[0]).arg(result.tag[1]).arg(result.tag[2]);
    // Literal tags live in SRP, so cached SRP cannot borrow new SRD initial values.
    // STEP AIRead is an assignment instruction, not an expression callable in IF.
    const auto guard = QString("ajRead1 := AIRead(pb_in5);\najRead2 := AIRead(pb_in6);\najRead3 := AIRead(pb_in7);\n"
        "IF(ajRead1!=%1 || ajRead2!=%2 || ajRead3!=%3 || ajTag1!=%1 || ajTag2!=%2 || ajTag3!=%3)THEN\n"
        "Message(\"AUTO REVISION MISMATCH. Reload the uploaded job.\",eError);\nEXIT();\nEND_IF\n")
        .arg(result.tag[0]).arg(result.tag[1]).arg(result.tag[2]).toLatin1();
    result.patchLineOffset = guard.count('\n');
    const auto tagData=QString("INT ajTag1 := %1\nINT ajTag2 := %2\nINT ajTag3 := %3\n"
        "INT ajRead1 := 0\nINT ajRead2 := 0\nINT ajRead3 := 0\n")
        .arg(result.tag[0]).arg(result.tag[1]).arg(result.tag[2]).toLatin1();
    QByteArray main = "# R6.4 fixed job entry; revision " + result.revision.toLatin1() + "\n" + guard +
        "IF(msHomeSaved!=TRUE || msOrigin<=0)THEN\nMessage(\"Capture the experiment origin in manual service first.\",eError);\nEXIT();\nEND_IF\n"
        "Tool(Default);\nRefSys(WORLD);\n";
    for (int index = 0; index < sources.size(); ++index) {
        const auto& snapshot = snapshots[index];
        auto part = readGcodeBytes(snapshot.path, snapshot.bytes, base, settings, snapshot.start);
        if (!snapshot.error.isEmpty()) part.error = snapshot.error;
        result.parts.append(part);
        if (!part.error.isEmpty()) { result.error = QString("第%1个文件：%2").arg(index+1).arg(part.error); result.files.clear(); return result; }
        const auto name = QString("auto_%1").arg(index+1,2,10,QLatin1Char('0'));
        auto code = part.files.value("gcode_path.srp");
        code.replace("homeSaved", "msHomeSaved"); code.replace("pHomeCart", "msHomeCart");
        code.replace("Message(\"Run capture_home before gcode_path.\",eInfo);", "Message(\"Experiment origin is missing.\",eError);\nEXIT();");
        auto data = part.files.value("gcode_path.srd");
        data.replace("CARTPOS cpTarget := {0,0,0,0,0,0,0,0,0,0,0,0,0}", "ROBOTCARTPOS cpTarget := {0,0,0,0,0,0,0}");
        result.files.insert(name+".srp",guard+code);
        result.files.insert(name+".srd",tagData+data);
        main += "ProgCall(" + name.toLatin1() + ");\n";
    }
    main += "Message(\"AUTO DONE " + result.revision.toLatin1() + "\",eInfo);\n";
    result.files.insert("auto_main.srp",main); result.files.insert("auto_main.srd",tagData);
    return result;
}

Job Delivery::autoPreview(const AutoJob& automatic, const QJsonObject& transfer) {
    Job preview;
    preview.nativeInput = true;
    preview.automaticPatch = true;
    preview.project = automatic.project;
    preview.program = "auto_main";
    preview.error = automatic.error;
    if (!preview.error.isEmpty() || automatic.parts.isEmpty() || automatic.files.isEmpty()) {
        if (preview.error.isEmpty()) preview.error = "尚无转换通过的自动作业。";
        preview.name = "自动作业 · " + QFileInfo(automatic.sources.value(automatic.parts.size() - 1)).fileName();
        if (!automatic.parts.isEmpty()) {
            const auto& failed = automatic.parts.last();
            preview.source = failed.source;
            preview.mappedBytes = failed.inputBytes;
            preview.diagnostics = failed.diagnostics;
            preview.sourceProgram = QString("auto_%1").arg(automatic.parts.size(), 2, 10, QLatin1Char('0'));
            preview.programSources.insert(preview.sourceProgram, automatic.sources.value(automatic.parts.size() - 1));
        }
        preview.summary = "转换失败 · 本次未上传";
        preview.previewNote = preview.summary + "。" + preview.error;
        return preview;
    }
    preview.files = automatic.files;
    QStringList names;
    for (int index = 0; index < automatic.parts.size(); ++index) {
        const auto program = QString("auto_%1").arg(index + 1, 2, 10, QLatin1Char('0'));
        preview.programPaths.insert(program, automatic.parts[index].points);
        const auto source = automatic.sources.value(index);
        preview.programSources.insert(program, source);
        const auto& part = automatic.parts[index];
        GcodeSnapshot snapshot{part.source, part.inputBytes, part.steps, part.conversion, part.start};
        for (auto& step : snapshot.steps) {
            step.firstLine += automatic.patchLineOffset;
            step.lastLine += automatic.patchLineOffset;
        }
        preview.programGcodes.insert(program, snapshot);
        names << QFileInfo(source).fileName();
    }
    if (automatic.parts.size() == 1) preview.programPaths.insert("auto_main", automatic.parts.first().points);
    preview.name = "自动作业 · " + names.join("；");
    const auto state = transfer.value("status").toString();
    const auto status = state == "COMPLETE" ? QString("上传及回读通过 · 待重新加载") :
        state.isEmpty() ? QString("本地转换通过 · 尚未上传") :
        state == "UPLOADING" ? QString("正在上传 · 尚未回读确认") : QString("上传未完成 · 本次作业未放行");
    preview.summary = QString("自动作业 · %1 个轨迹 · 版本 %2 · %3")
        .arg(automatic.parts.size()).arg(automatic.revision, status);
    preview.previewNote = QString("本地生成文件，非控制柜实时目录。%1。11号入口：%2 / auto_main。"
        "原文、轨迹及行映射均来自本次转换；可在工程树切换各段G代码及生成文件。示教器重新加载后须核对本次版本 %3。")
        .arg(status, automatic.project, automatic.revision);
    showProgramGcode(preview, "auto_01");
    return preview;
}

Delivery::Package Delivery::saveAuto(const AutoJob& job, const QString& root) {
    Package p;
    if (!job.error.isEmpty() || job.parts.isEmpty()) { p.error = "自动作业转换未通过。"; return p; }
    QDir parent(root);
    const auto id = "auto_"+QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz_")+QUuid::createUuid().toString(QUuid::Id128).left(8);
    if (!parent.mkpath(".") || !parent.mkdir(id)) { p.error = "无法创建独立作业目录。"; return p; }
    p.directory=parent.filePath(id); p.zip=QDir(p.directory).filePath("AutoJobPatch.zip"); p.manifest=QDir(p.directory).filePath("delivery.json");
    QByteArray archive;QBuffer buffer(&archive);
    if (!buffer.open(QIODevice::WriteOnly)) { p.error="无法创建自动作业包。"; return p; }
    QJsonObject hashes;
    { QZipWriter zip(&buffer);
      for(auto it=job.files.cbegin();it!=job.files.cend();++it) { zip.addFile(job.project+'/'+it.key(),it.value());hashes.insert(it.key(),digest(it.value())); }
      zip.close(); if(zip.status()!=QZipWriter::NoError) { p.error="无法生成自动作业包。"; return p; }
    }
    QSaveFile output(p.zip);
    if(!output.open(QIODevice::WriteOnly) || output.write(archive)!=archive.size() || !output.commit()) { p.error="无法封存自动作业包。"; return p; }
    QJsonArray parts;
    for(int index=0;index<job.parts.size();++index) {
        const auto dir=QDir(p.directory).filePath(QString("source_%1").arg(index+1,2,10,QLatin1Char('0')));QDir().mkpath(dir);
        // Keep each source and its original line map alongside the actual guard offset.
        const auto error=exportStepJob(job.parts[index],QDir(dir).filePath("ConvertedSource.zip"));
        if(!error.isEmpty()){p.error=error;return p;}
        parts.append(QJsonObject{{"order",index+1},{"source",job.sources[index]},
            {"source_sha256",digest(job.parts[index].inputBytes)},{"program",QString("auto_%1").arg(index+1,2,10,QLatin1Char('0'))},
            {"patch_line_offset",job.patchLineOffset}});
    }
    p.error=writeJson(p.manifest,{{"schema",2},{"status","PREPARED_NOT_UPLOADED"},{"project",job.project},
        {"program","auto_main"},{"program_number",job.number},{"revision",job.revision},
        {"tag",QJsonArray{job.tag[0],job.tag[1],job.tag[2]}},{"parts",parts},{"native_sha256",hashes},
        {"orientation","captured_manual_tcp_fixed"},{"joint_lock",false},{"guard_syntax_revision",2},
        {"patch_only",true},{"origin_file_written",false},{"robot_loaded",false},{"robot_started",false},{"dry_run",true}});
    return p;
}

QString Delivery::autoHandoff(const AutoJob& job,const QJsonObject& transfer) {
    if(transfer.value("status").toString()!="COMPLETE")return "上传尚未完整通过，本次作业号未放行。请保留回执。";
    return QString("上传及逐字节回读通过。\n可用作业号：%1 → %2 / auto_main\n本次版本：%3\n轨迹程序：%4个，按导入顺序执行。\n\n"
        "登记 → 加载%1号 → 确认加载 → 使能 → 运行。\n每次上传后都要重新加载；入口和每个轨迹先校验版本，缓存旧代码会退出并提示版本不匹配。\n"
        "实验起点沿用10号手动服务已捕捉的数据；本次上传未写起点文件。\n同名程序是否重读新内容仍须现场验证；FTP回读通过不等于柜内重载已验收。")
        .arg(job.number).arg(job.project,job.revision).arg(job.parts.size());
}

QString Delivery::recordAuto(const Package& p,const AutoJob& job,const QJsonObject& transfer) {
    QFile file(p.manifest);if(!file.open(QIODevice::ReadOnly))return "无法读取自动作业交接记录。";
    auto object=QJsonDocument::fromJson(file.readAll()).object();file.close();
    if(object.value("schema").toInt()!=2)return "自动作业交接记录格式无效。";
    object.insert("status",transfer.value("status").toString()=="COMPLETE"?"UPLOADED_RELOAD_REQUIRED":"UPLOAD_NOT_COMPLETE");
    object.insert("transfer",transfer);object.insert("operator_handoff",autoHandoff(job,transfer));
    return writeJson(p.manifest,object);
}
