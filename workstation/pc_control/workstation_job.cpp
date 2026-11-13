#include "workstation_job.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QtCore/private/qzipwriter_p.h>
#if __has_include(<QtCore/private/qzipreader_p.h>)
#include <QtCore/private/qzipreader_p.h>
#else
#include <QtGui/private/qzipreader_p.h>
#endif

#include <algorithm>
#include <cmath>
#include <numbers>

Job readNativeJob(const QString& path) {
    Job job;
    job.nativeInput = true;
    job.sourceIsGcode = false;
    job.name = QFileInfo(path).fileName();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 5'000'000) {
        job.error = "ZIP 无法读取或超过 5 MB。";
        return job;
    }
    QByteArray raw = file.readAll();
    QBuffer buffer(&raw);
    buffer.open(QIODevice::ReadOnly);
    QZipReader archive(&buffer);
    const auto entries = archive.fileInfoList();
    const QRegularExpression member(R"(^([A-Za-z0-9_-]{1,27}\.sr)/([A-Za-z0-9_-]{1,27}\.sr[pd])$)");
    qint64 size = 0;
    QSet<QString> seen;
    QStringList directories;
    if (entries.isEmpty() || entries.size() > 80 || archive.status() != QZipReader::NoError) {
        job.error = "ZIP 无效或条目过多。";
        return job;
    }
    for (const auto& entry : entries) {
        if (entry.isDir) {
            directories << entry.filePath;
            continue;
        }
        const auto match = member.match(entry.filePath);
        size += entry.size;
        const QString key = entry.filePath.toCaseFolded();
        if (!entry.isFile || entry.isSymLink || !match.hasMatch() || entry.size < 0 || size > 5'000'000 ||
            seen.contains(key) || (!job.project.isEmpty() && job.project != match.captured(1))) {
            job.error = "ZIP 需要一层英文 .sr 工程目录，成员必须为同一工程的 .srp / .srd。";
            return job;
        }
        job.project = match.captured(1);
        seen.insert(key);
    }
    for (const auto& directory : directories) {
        if (directory != job.project && directory != job.project + '/') {
            job.error = "ZIP 包含多余目录。";
            return job;
        }
    }
    for (const auto& entry : entries) {
        if (entry.isDir) continue;
        auto bytes = archive.fileData(entry.filePath);
        QStringDecoder decoder(QStringDecoder::Utf8);
        decoder(bytes);
        if (bytes.size() != entry.size || bytes.contains('\0') || decoder.hasError()) {
            job.error = "ZIP 文件内容损坏或不是有效 UTF-8 文本。";
            return job;
        }
        job.files.insert(entry.filePath.section('/', -1), bytes);
    }
    int programs = 0;
    for (const auto& name : job.files.keys()) {
        if (name.endsWith(".srp")) {
            ++programs;
            if (!job.files.contains(name.chopped(1) + 'd')) {
                job.error = "缺少同名程序变量文件。";
                return job;
            }
        }
    }
    if (!programs || !job.files.contains("_project.srd")) {
        job.error = "缺少程序或工程变量。";
        return job;
    }
    job.reviewed = QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex() ==
        "e2c0746c66cb9045e71fabedc2c58b15add4dc6a929e7f38ce9378a611677c87";
    job.inputBytes = raw;
    job.program = job.reviewed ? "helix_25mms" : QString();
    if (job.program.isEmpty()) {
        for (auto it = job.files.cbegin(); it != job.files.cend(); ++it)
            if (it.key().endsWith(".srp") && it.value().size() > job.files.value(job.program + ".srp").size())
                job.program = it.key().chopped(4);
    }
    job.summary = job.reviewed ? "原生螺旋 v4 · 4 圈 · X +100 / Z +332 mm" : "原生 STEP 工程 · 仅查阅";
    if (job.reviewed) {
        job.points = {{}, {100, 0, 0}, {100, 0, 2}, {100, 0, 12}};
        const auto radius = std::sqrt(800.0);
        for (int i = 1; i <= 288; ++i) {
            const auto angle = (-135 - 5 * i) * std::numbers::pi / 180;
            job.points << Point{120 + radius * std::cos(angle), 20 + radius * std::sin(angle), 12 + 80.0 * i / 72};
        }
    }
    showNativeSource(job, job.program);
    if (QFileInfo::exists(path + ".mapping.json")) job.mappingWarning = associateNativeGcode(job, path + ".mapping.json");
    return job;
}

void showNativeSource(Job& job, const QString& program) {
    showNativeFile(job, program + ".srp");
}

void showNativeFile(Job& job, const QString& name) {
    if (!job.nativeInput || !job.files.contains(name)) return;
    const auto bytes = job.files.value(name);
    QString text = QString::fromUtf8(bytes);
    text.replace("\r\n", "\n").replace('\r', '\n');
    if (text.endsWith('\n')) text.chop(1);
    job.source = text.isEmpty() ? QStringList{} : text.split('\n');
    job.sourceFile = name;
    job.sourceProgram = name.chopped(4);
    job.sourceIsGcode = false;
    job.steps.clear();
    job.start = 1;
}

QString updateNativeFile(Job& job, const QString& name, const QByteArray& bytes) {
    if (!job.nativeInput || !job.files.contains(name)) return "当前工作副本中没有这个文件。";
    QStringDecoder decoder(QStringDecoder::Utf8);
    decoder(bytes);
    if (bytes.size() > 1'000'000 || bytes.contains('\0') || decoder.hasError()) return "文件必须为不超过 1 MB 的 UTF-8 文本。";
    if (job.files.value(name) == bytes) return {};
    job.files[name] = bytes;
    job.modified = true;
    job.localEdits = true;
    job.reviewed = false;
    job.trustedGcode = false;
    job.mappedSource.clear();
    job.mappedBytes.clear();
    job.mappedSteps.clear();
    job.mappedProgram.clear();
    job.points.clear();
    job.mappingWarning = "工作副本已修改，旧验证状态和 G 代码映射已清除。请另存 ZIP 并在 STEPStudio 重新验证。";
    job.summary = "STEP 工作副本已修改 · 待 STEPStudio 验证";
    showNativeFile(job, name);
    return {};
}

void showMappedSource(Job& job) {
    if (job.mappedSource.isEmpty()) return;
    job.source = job.mappedSource;
    job.sourceProgram = job.mappedProgram;
    job.steps = job.mappedSteps;
    job.sourceIsGcode = true;
    job.sourceFile.clear();
    job.start = job.mappedStart;
}

int sourceRowForNativeLine(const Job& job, const QString& project, const QString& program, int nativeLine) {
    if (job.localEdits || job.sourceFile.endsWith(".srd") || (project != job.project && project + ".sr" != job.project) || program != job.sourceProgram || nativeLine < 1) return 0;
    if (!job.sourceIsGcode) return nativeLine <= job.source.size() ? nativeLine : 0;
    for (const auto& step : job.steps)
        if (!step.skipped && nativeLine >= step.firstLine && nativeLine <= step.lastLine) return step.row;
    return 0;
}

QString associateNativeGcode(Job& job, const QString& mappingPath) {
    if (!job.nativeInput || !job.error.isEmpty()) return "请先打开有效的 STEP ZIP。";
    const bool separateSource = !mappingPath.endsWith(".mapping.json", Qt::CaseInsensitive);
    QFile file(separateSource ? mappingPath + ".mapping.json" : mappingPath);
    if (separateSource && !file.exists()) return "缺少同名 .mapping.json。请让转换工具同时输出 SRP 与 G 代码行映射；仅凭 G 代码无法确定执行行。";
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1'000'000) return "G 代码映射无法读取或超过 1 MB。";
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const auto map = document.object();
    const auto digest = [](const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    if (parseError.error != QJsonParseError::NoError || map.value("schema").toInt() != 1 ||
        map.value("source_format").toString("gcode") != "gcode" || map.value("project").toString() != job.project ||
        !QRegularExpression("^[a-f0-9]{64}$").match(map.value("zip_sha256").toString()).hasMatch()) return "映射与当前 ZIP 的版本或 SHA-256 不匹配，已保留 SRP 原文。";
    const auto hashes = map.value("program_sha256").toObject();
    if (hashes.size() != job.files.size()) return "映射缺少原生程序的版本校验。";
    for (auto it = job.files.cbegin(); it != job.files.cend(); ++it)
        if (hashes.value(it.key()).toString() != digest(it.value())) return "映射中的原生文件 SHA-256 不匹配。";
    const auto program = map.value("program").toString();
    if (!job.files.contains(program + ".srp")) return "映射指定的 SRP 程序不在当前 ZIP 中。";
    const auto decoded = QByteArray::fromBase64Encoding(map.value("source_bytes_base64").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    const auto bytes = decoded.decoded;
    if (!decoded || bytes.isEmpty() || bytes.size() > 32768 || digest(bytes) != map.value("source_sha256").toString())
        return "G 代码原文缺失或 SHA-256 不匹配。";
    if (separateSource) {
        QFile sourceFile(mappingPath);
        if (!sourceFile.open(QIODevice::ReadOnly) || sourceFile.size() > 32768 || sourceFile.readAll() != bytes)
            return "所选 G 代码与映射中的原文不一致。";
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString text = stripUtf8Bom(decoder(bytes));
    if (decoder.hasError()) return "映射中的 G 代码不是有效 UTF-8。";
    text.replace("\r\n", "\n").replace('\r', '\n');
    if (text.endsWith('\n')) text.chop(1);
    const auto source = text.split('\n');
    const int start = map.value("source_start_line").toInt();
    QString nativeText = QString::fromUtf8(job.files.value(program + ".srp"));
    if (nativeText.endsWith('\n')) nativeText.chop(1);
    const auto nativeLines = nativeText.split('\n').size();
    if (start < 1 || start > source.size()) return "映射起始行无效。";
    QVector<Step> steps;
    QSet<int> covered, rows;
    for (const auto value : map.value("source_map").toArray()) {
        const auto entry = value.toObject();
        const int row = entry.value("source_line").toInt(), first = entry.value("srp_first_line").toInt(), last = entry.value("srp_last_line").toInt();
        if (row < start || row > source.size() || rows.contains(row) || first < 1 || last < first || last > nativeLines)
            return "映射行号越界或源行重复。";
        rows.insert(row);
        const bool skipped = entry.value("skipped_output").toBool();
        for (int line = first; line <= last; ++line) {
            if (covered.contains(line)) return "一条 SRP 行映射到多条 G 代码，无法唯一高亮。";
            covered.insert(line);
        }
        steps << Step{row, first, last, 0, 0, 0, 0, skipped};
    }
    if (steps.isEmpty()) return "映射没有可用的执行行。";
    job.mappedSource = source;
    job.mappedBytes = bytes;
    job.mappedProgram = program;
    job.mappedSteps = steps;
    job.mappedStart = start;
    const auto options = map.value("conversion").toObject();
    job.conversion = {options.value("rapid_mm_s").toDouble(5), options.value("arc_tolerance_mm").toDouble(.05), options.value("omit_process_outputs").toBool()};
    job.mappingWarning.clear();
    showMappedSource(job);
    return {};
}

bool restoreMappedPreview(Job& job, const Job& nativeTemplate) {
    if (!job.nativeInput || job.localEdits || job.mappedSource.isEmpty() || !nativeTemplate.reviewed) return false;
    const auto rebuilt = compileGcodeJob(job.name, job.mappedSource, job.mappedStart, nativeTemplate, job.conversion);
    if (!rebuilt.runnable() || rebuilt.project != job.project || rebuilt.program != job.mappedProgram || rebuilt.files != job.files) return false;
    job.points = rebuilt.points;
    job.program = rebuilt.program;
    job.trustedGcode = true;
    job.mappedSteps = rebuilt.steps;
    if (job.sourceIsGcode) job.steps = job.mappedSteps;
    job.summary = "转换版本已核对 · 可本机仿真 · " + rebuilt.summary;
    return true;
}

Job readGcodeJob(const QString& path, const Job& nativeTemplate, ConversionSettings settings) {
    Job job;
    job.name = QFileInfo(path).fileName();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 32768) {
        job.error = "G 代码无法读取或超过 32 KiB。";
        return job;
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QByteArray raw = file.readAll();
    QString text = stripUtf8Bom(decoder(raw));
    text.replace("\r\n", "\n").replace('\r', '\n');
    if (text.endsWith('\n')) text.chop(1);
    job.source = text.split('\n');
    if (decoder.hasError()) {
        job.error = "文件包含无效的 UTF-8 字节。";
        return job;
    }
    auto compiled = compileGcodeJob(job.name, job.source, 1, nativeTemplate, settings);
    compiled.inputBytes = raw;
    return compiled;
}

Job compileGcodeJob(QString name, const QStringList& source, int start, const Job& nativeTemplate, ConversionSettings settings) {
    Job job;
    job.name = std::move(name);
    job.source = source;
    job.inputBytes = (source.join('\n') + '\n').toUtf8();
    job.start = start;
    job.project = "GcodePreview.sr";
    job.program = "gcode_path";
    job.sourceProgram = job.program;
    job.conversion = settings;
    if (job.inputBytes.size() > 32768) { job.error = "G 代码超过 32 KiB。"; return job; }
    if (!nativeTemplate.reviewed || start < 1 || start > source.size() ||
        !std::isfinite(settings.rapidMmS) || settings.rapidMmS < .001 || settings.rapidMmS > 25 ||
        !std::isfinite(settings.arcToleranceMm) || settings.arcToleranceMm < .001 || settings.arcToleranceMm > .1) {
        job.error = "仿真起点模板或起始行无效。";
        return job;
    }
    QStringList code{"# Generated dry-run path. Validate in STEPStudio before use.", "simDone := FALSE;", "IF(homeSaved==TRUE)THEN", "Tool(Default);", "RefSys(WORLD);"};
    QStringList data{"BOOL simDone := FALSE", "OVERLAPREL ovlPath := 50", "CARTPOS cpTarget := {0,0,0,0,0,0,0,0,0,0,0,0,0}"};
    // 同一份转换结果的 SDK 逐行下发版本：行号与 SRP 严格一致，供 helix_pc sendprog 直发实机。
    QStringList sdk{"HOME", "TOOL 4 Default", "REFSYS 5 WORLD"};
    const QRegularExpression comment(R"(\([^()]*\))");
    const QRegularExpression word(R"(([A-Z])[ \t]*([+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)))");
    QMap<double, QString> speeds;
    Point position, origin;
    job.points << position;
    bool relative = false;
    int modalMotion = -1;
    double feed = 0;
    double sdkFeed = 0;
    bool ended = false;
    int turns = 0;
    double dwell = 0;
    auto error = [&](int row, const QString& reason) { job.diagnostics.insert(row, reason); };
    for (int row = 1; row <= source.size(); ++row) {
        if (row == start) origin = position;
        const QString line = source[row - 1].section(';', 0, 0).toUpper().replace(comment, " ").trimmed();
        if (line.isEmpty() || line == "%") continue;
        if (ended || !QString(line).remove(word).trimmed().isEmpty()) {
            error(row, ended ? "M30 后还有指令。" : "无法识别的字符或参数。");
            continue;
        }
        QList<QPair<QChar, double>> words;
        auto matches = word.globalMatch(line);
        while (matches.hasNext()) {
            const auto match = matches.next();
            words.append({match.captured(1)[0], match.captured(2).toDouble()});
        }
        QMap<QChar, double> args;
        QSet<int> groups;
        int explicitMotion = -1, mcode = -1;
        bool bad = false;
        for (const auto& [letter, value] : words) {
            if (!std::isfinite(value)) { bad = true; break; }
            if (letter == 'G') {
                int group = 0;
                if (value == 0 || value == 1 || value == 2 || value == 3 || value == 4) { group = 1; explicitMotion = static_cast<int>(value); }
                else if (value == 90 || value == 91) { group = 2; relative = value == 91; }
                else if (value == 17) group = 3;
                else if (value == 21) group = 4;
                else if (value == 94) group = 5;
                else if (value == 91.1) group = 6;
                else { error(row, "不支持该 G 指令；仅开放毫米、XY 平面、相对 I/J 圆心和每分钟进给。"); bad = true; break; }
                if (groups.contains(group)) { bad = true; break; }
                groups.insert(group);
            } else if (letter == 'M') {
                if (mcode >= 0 || value < 0 || value > 999 || value != std::floor(value)) { bad = true; break; }
                mcode = static_cast<int>(value);
            } else {
                if (args.contains(letter)) { bad = true; break; }
                args.insert(letter, value);
            }
        }
        if (bad) { if (!job.diagnostics.contains(row)) error(row, "参数重复、同组指令冲突或数值无效。"); continue; }
        if (args.contains('N')) {
            const auto n = args.take('N');
            if (n < 0 || n != std::floor(n)) { error(row, "N 行号需要非负整数。"); continue; }
        }
        if (args.size() == 1 && args.contains('O') && explicitMotion < 0 && mcode < 0 && args['O'] >= 0 && args['O'] == std::floor(args['O'])) continue;
        const int firstPoint = static_cast<int>(job.points.size()) - 1;
        if (mcode >= 0) {
            const bool end = mcode == 2 || mcode == 30;
            const bool process = mcode == 3 || mcode == 4 || mcode == 5 || mcode == 10 || mcode == 11;
            const bool arguments = args.isEmpty() || ((mcode == 3 || mcode == 4) && args.size() == 1 && args.contains('S') && args['S'] >= 0);
            if (explicitMotion >= 0 || !arguments || (!end && !(process && (settings.omitProcessOutputs || mcode == 10)))) {
                error(row, "该 M 指令未接入。M3/M4/M5/M11 仅在明确选择忽略工艺输出时可生成空跑工程。"); continue;
            }
            ended = end;
            if (!ended && row >= start) {
                code << QString("# Source line %1: M%2 omitted. No process output.").arg(row).arg(mcode);
                const int n = static_cast<int>(code.size());
                job.steps << Step{row, n, n, firstPoint, firstPoint, 0, 0, true};
            }
            continue;
        }
        if (args.contains('F')) {
            feed = args.take('F') / 60;
            if (feed < .001 || feed > 25) { error(row, "F 需要 0.06–1500 mm/min（0.001–25 mm/s）。"); continue; }
        }
        if (explicitMotion == 4) {
            if (args.size() != 1 || (!args.contains('T') && !args.contains('P'))) { error(row, "G4 需要 P 秒，或本项目兼容的 T 秒。"); continue; }
            const auto seconds = args.first();
            if (seconds < 0 || seconds > 60 || dwell + seconds > 300) {
                error(row, "等待须为 0–60 秒，累计不超过 300 秒。");
                continue;
            }
            dwell += seconds;
            if (row < start) continue;
            code << QString("WaitTime(%1);").arg(static_cast<int>(std::nearbyint(seconds * 1000)));
            const int n = static_cast<int>(code.size());
            sdk << QString("WAIT %1 %2").arg(n).arg(static_cast<int>(std::nearbyint(seconds * 1000)));
            job.steps << Step{row, n, n, firstPoint, firstPoint, 0, seconds};
            continue;
        }
        if (explicitMotion >= 0) modalMotion = explicitMotion;
        if (args.isEmpty()) continue;
        const auto op = modalMotion;
        if (op < 0) { error(row, "坐标前需要 G0/G1/G2/G3 运动模式。"); continue; }
        const double speed = op == 0 ? settings.rapidMmS : feed;
        if (speed < .001 || speed > 25) { error(row, "第一次进给运动前需要有效 F，单位 mm/min。"); continue; }
        const QString allowed = op <= 1 ? "XYZ" : "XYZIJ";
        const auto keys = args.keys();
        if (std::any_of(keys.cbegin(), keys.cend(), [&](QChar axis) { return !allowed.contains(axis); })) {
            error(row, "该行含有不支持的轴或圆弧参数。");
            continue;
        }
        QVector<Point> targets;
        const auto coordinate = [&](QChar axis, double current) { return args.contains(axis) ? (relative ? current + args.value(axis) : args.value(axis)) : current; };
        const Point target{coordinate('X', position.x), coordinate('Y', position.y), coordinate('Z', position.z)};
        if (op <= 1) {
            if (std::hypot(target.x - position.x, target.y - position.y, target.z - position.z) < 1e-9) continue;
            targets << target;
        } else {
            const auto i = args.value('I'), j = args.value('J');
            const auto radius = std::hypot(i, j);
            if ((!args.contains('I') && !args.contains('J')) || radius < .01 || radius > 100) {
                error(row, "圆弧需要相对起点的 I/J 圆心，半径为 0.01–100 mm；R 格式尚未开放。");
                continue;
            }
            const Point center{position.x + i, position.y + j, 0};
            const auto endRadius = std::hypot(target.x - center.x, target.y - center.y);
            if (std::abs(endRadius - radius) > std::max(.01, radius * .001)) { error(row, "圆弧起终点半径不一致，请检查坐标模式与 I/J。"); continue; }
            const auto phase = std::atan2(-j, -i);
            const bool circle = std::hypot(target.x - position.x, target.y - position.y) < 1e-7;
            auto sweep = std::atan2(target.y - center.y, target.x - center.x) - phase;
            if (op == 2) { while (sweep >= -1e-12) sweep -= 2 * std::numbers::pi; }
            else { while (sweep <= 1e-12) sweep += 2 * std::numbers::pi; }
            if (circle) { sweep = (op == 2 ? -2 : 2) * std::numbers::pi; if (row >= start) ++turns; }
            const double angleLimit = std::min(5 * std::numbers::pi / 180, 2 * std::acos(std::clamp(1 - settings.arcToleranceMm / radius, -1., 1.)));
            const int segments = std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / angleLimit - 1e-10)));
            if (segments > 1000) { error(row, "圆弧离散点超过 1000，请检查半径与弦高误差。"); continue; }
            for (int n = 1; n < segments; ++n) {
                const auto angle = phase + sweep * n / segments;
                targets << Point{center.x + radius * std::cos(angle), center.y + radius * std::sin(angle), position.z + (target.z - position.z) * n / segments};
            }
            targets << target;
        }
        if (row < start) { position = target; continue; }
        if (firstPoint + targets.size() > 1000 || std::any_of(targets.cbegin(), targets.cend(), [&](Point p) {
            return std::abs(p.x - origin.x) > 1000 || std::abs(p.y - origin.y) > 1000 || std::abs(p.z - origin.z) > 1000;
        })) {
            error(row, "超出本版 1000 个点或 ±1000 mm 增量范围。");
            continue;
        }
        if (!speeds.contains(speed)) {
            const auto key = QString("dyn%1").arg(speeds.size() + 1);
            speeds.insert(speed, key);
            data << QString("DYNAMIC %1 := {%2,200,200,1000,30,80,80,300,10,20,20,20}").arg(key, QString::number(speed, 'f', 9));
        }
        if (sdkFeed != speed) {
            // 新速度组的首条 Lin 位于 code.size()+3（注释、RelRef 之后），Dyn 指令与它同行生效。
            sdk << QString("DYN %1 %2 200 200 1000 30 80 80 300 10 20 20 20").arg(static_cast<int>(code.size()) + 3).arg(speed, 0, 'f', 9);
            sdkFeed = speed;
        }
        code << QString("# Source line %1").arg(row);
        const int firstLine = static_cast<int>(code.size()) + 1;
        double seconds = 0;
        for (qsizetype index = 0; index < targets.size(); ++index) {
            const auto pathTarget = targets[index];
            const auto point = QString("p%1").arg(job.points.size());
            seconds += std::hypot(pathTarget.x - position.x, pathTarget.y - position.y, pathTarget.z - position.z) / speed;
            position = pathTarget;
            const Point offset{pathTarget.x - origin.x, pathTarget.y - origin.y, pathTarget.z - origin.z};
            job.points << offset;
            data << QString("POSOFFSET %1 := {%2,%3,%4,0,0,0}").arg(point, QString::number(offset.x, 'f', 9),
                     QString::number(offset.y, 'f', 9), QString::number(offset.z, 'f', 9));
            sdk << QString("LIN %1 %2 %3 %4 %5 %6").arg(static_cast<int>(code.size()) + 2, 5).arg(offset.x, 0, 'f', 6)
                    .arg(offset.y, 0, 'f', 6).arg(offset.z, 0, 'f', 6)
                    .arg(speeds.value(speed).mid(3).toInt()).arg(index + 1 == targets.size() ? 0 : 1);
            code << QString("cpTarget := RelRef(pHomeCart,%1);").arg(point);
            code << QString("Lin(cpTarget,%1,%2,NULL,Default,WORLD);").arg(speeds.value(speed), index + 1 == targets.size() ? "NULL" : "ovlPath");
        }
        code << "WaitIsFinished(100);";
        job.steps << Step{row, firstLine, static_cast<int>(code.size()), firstPoint, static_cast<int>(job.points.size()) - 1, seconds, 0};
    }
    if (!job.diagnostics.isEmpty() || !ended || job.points.size() < 2) {
        job.error = job.diagnostics.isEmpty() ? "需要至少一条运动指令，并以 M2 / M30 结束。" :
            QString("第 %1 行：%2").arg(job.diagnostics.firstKey()).arg(job.diagnostics.first());
        job.points.clear();
        job.steps.clear();
        return job;
    }
    code << "simDone := TRUE;" << "SaveData(simDone);" << "ELSE" << "Message(\"Run capture_home before gcode_path.\",eInfo);" << "END_IF";
    sdk << QString("EOF %1").arg(static_cast<int>(code.size()) + 1);
    job.files.insert("gcode_path.srp", (code.join('\n') + '\n').toLatin1());
    job.files.insert("gcode_path.srd", (data.join('\n') + '\n').toLatin1());
    for (const auto* member : {"_project.srd", "capture_home.srd", "capture_home.srp"}) {
        job.files.insert(member, nativeTemplate.files.value(member));
    }
    job.sdkProgram = (sdk.join('\n') + '\n').toLatin1();
    const auto endpoint = job.points.last();
    job.summary = QString("%1 个点 · %2 整圈 · 终点 (%3, %4, %5) mm · 等待 %6 s")
        .arg(job.points.size() - 1).arg(turns).arg(endpoint.x).arg(endpoint.y).arg(endpoint.z).arg(dwell);
    return job;
}

QString exportStepJob(const Job& job, const QString& zipPath) {
    if (!job.error.isEmpty() || job.files.isEmpty()) return "当前作业没有可导出的原生程序。";
    qint64 totalSize = 0;
    for (auto it = job.files.cbegin(); it != job.files.cend(); ++it) {
        totalSize += it.value().size();
        if (it.key().endsWith(".srp") && !job.files.contains(it.key().chopped(1) + 'd')) return "缺少同名 SRD，无法导出完整工程。";
    }
    if (totalSize > 5'000'000 || !job.files.contains("_project.srd")) return "工程超过 5 MB 或缺少工程变量文件。";
    QByteArray archive;
    QBuffer output(&archive);
    if (!output.open(QIODevice::WriteOnly)) return "无法创建 STEP 工程包。";
    QZipWriter zip(&output);
    QJsonObject programHashes;
    for (auto file = job.files.cbegin(); file != job.files.cend(); ++file) {
        zip.addFile(job.project + '/' + file.key(), file.value());
        programHashes.insert(file.key(), QString::fromLatin1(QCryptographicHash::hash(file.value(), QCryptographicHash::Sha256).toHex()));
    }
    zip.close();
    if (zip.status() != QZipWriter::NoError) return "STEP ZIP 生成失败。";
    const auto digest = [](const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    const bool hasGcode = !job.nativeInput || !job.mappedSource.isEmpty();
    const auto& mappedSteps = job.nativeInput ? job.mappedSteps : job.steps;
    QJsonArray mapping;
    for (const auto& step : mappedSteps) mapping << QJsonObject{
        {"source_line", step.row}, {"srp_first_line", step.firstLine}, {"srp_last_line", step.lastLine},
        {"first_path_point", step.firstPoint}, {"last_path_point", step.lastPoint}, {"skipped_output", step.skipped}};
    const auto sourceBytes = job.nativeInput ? job.mappedBytes : job.inputBytes;
    const QJsonObject manifest{
        {"schema", 1}, {"source_format", hasGcode ? "gcode" : "srp"}, {"source_file", job.name}, {"source_sha256", digest(sourceBytes)},
        {"source_bytes_base64", QString::fromLatin1(sourceBytes.toBase64())}, {"source_start_line", job.nativeInput ? job.mappedStart : job.start},
        {"zip_sha256", digest(archive)}, {"project", job.project}, {"program", job.nativeInput ? job.mappedProgram : job.sourceProgram},
        {"program_sha256", programHashes}, {"source_map", mapping}, {"tool", "Default"}, {"reference", "WORLD"},
        {"dry_run", true}, {"process_output", "not_configured"}, {"origin", "captured_pHomeCart"},
        {"conversion", QJsonObject{{"rapid_mm_s", job.conversion.rapidMmS}, {"arc_tolerance_mm", job.conversion.arcToleranceMm},
                                     {"omit_process_outputs", job.conversion.omitProcessOutputs}}}
    };
    QSaveFile package(zipPath);
    if (!package.open(QIODevice::WriteOnly) || package.write(archive) != archive.size() || !package.commit()) return "STEP ZIP 写入失败。";
    if (!hasGcode) return {};
    QSaveFile sidecar(zipPath + ".mapping.json");
    const auto bytes = QJsonDocument(manifest).toJson();
    if (!sidecar.open(QIODevice::WriteOnly) || sidecar.write(bytes) != bytes.size() || !sidecar.commit())
        return "ZIP 已保存，源行映射写入失败。请重新导出。";
    QSaveFile source(zipPath + ".source.gcode"), report(zipPath + ".conversion.txt");
    const auto reportBytes = conversionReport(job).toUtf8();
    if (!source.open(QIODevice::WriteOnly) || source.write(sourceBytes) != sourceBytes.size() || !source.commit() ||
        !report.open(QIODevice::WriteOnly) || report.write(reportBytes) != reportBytes.size() || !report.commit())
        return "ZIP 和映射已保存，但原始 G 代码或转换报告写入失败。请重新导出。";
    return {};
}

QString conversionReport(const Job& job) {
    QStringList lines{"STEP G 代码后置转换报告", "结果：" + (job.error.isEmpty() ? QString("转换通过，待 STEPStudio 验证") : job.error),
        "源文件：" + job.name, "工程：" + job.project, "主程序：" + job.program,
        "坐标：毫米；G90/G91 均以 capture_home 记录的 TCP 为工件零点，轴向平行 WORLD；固定 Default 工具及记录时姿态。",
        "F：mm/min，转换为 STEP DYNAMIC 的 mm/s；限 0.001–25 mm/s。",
        QString("G0：受限直线，%1 mm/s；圆弧最大角步长 5°、弦高误差 %2 mm。").arg(job.conversion.rapidMmS).arg(job.conversion.arcToleranceMm),
        "工艺输出：未接入。此包为空跑运动工程；被忽略的工艺指令会留在源行映射中。",
        "概况：" + job.summary, "", "工程文件："};
    for (auto it = job.files.cbegin(); it != job.files.cend(); ++it) lines << QString("  %1 (%2 字节)").arg(it.key()).arg(it.value().size());
    lines << "" << "STEPStudio 操作顺序：" << "1. 在 SR175-3200 工作站中导入 ZIP；核对 Default 工具、WORLD 轴向、实际工件零点和初始姿态。"
          << "2. 在经确认的初始姿态运行 capture_home（只记录起点）。然后加载 gcode_path 仿真。"
          << "3. 检查可达性、关节限位、工具/机器人/工件碰撞、路径连续性、进给、端点和工艺顺序。"
          << "4. 如需返回记录的设备起点，使用上位机独立的路径规划功能；本转换包不生成 go_home 返回程序。"
          << "5. 核对同版本 ZIP 后导入上位机；保留 mapping.json 以联动 G 代码。STEPStudio 若修改程序，旧映射不能继续使用。"
          << "转换通过仅证明受支持语法已生成 SRP/SRD，不代表已完成 STEPStudio 场景或实机验证。";
    for (auto it = job.diagnostics.cbegin(); it != job.diagnostics.cend(); ++it) lines << QString("L%1: %2").arg(it.key()).arg(it.value());
    return lines.join('\n') + '\n';
}
