#define NOMINMAX
#include <winsock2.h>
#include "workstation_session.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QStandardPaths>

#include <windows.h>

namespace {
bool virtualPortsFree() {
    WSADATA startup{};
    if (WSAStartup(MAKEWORD(2, 2), &startup)) return false;
    bool available = true;
    for (const unsigned short port : {30316, 30317}) {
        const auto probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        const BOOL exclusive = TRUE;
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = htons(port);
        if (probe == INVALID_SOCKET || setsockopt(probe, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive), sizeof exclusive) ||
            bind(probe, reinterpret_cast<const sockaddr*>(&endpoint), sizeof endpoint)) available = false;
        if (probe != INVALID_SOCKET) closesocket(probe);
        if (!available) break;
    }
    WSACleanup();
    return available;
}

bool copyTree(const QString& from, const QString& to, const QStringList& skip = {}) {
    QDir source(from);
    if (!source.exists() || !QDir().mkpath(to)) return false;
    for (const auto& item : source.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        if (skip.contains(item.fileName())) continue;
        const auto target = QDir(to).filePath(item.fileName());
        if (item.isDir() ? !copyTree(item.filePath(), target) : !QFile::copy(item.filePath(), target)) return false;
    }
    return true;
}

void hideConsole(QProcess& process) {
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    process.setProcessChannelMode(QProcess::MergedChannels);
}

void cleanOldRuns(const QString& logs) {
    const QDir folder(logs);
    const auto root = QFileInfo(logs).canonicalFilePath();
    if (root.isEmpty()) return;
    const auto now = QDateTime::currentDateTime();
    for (const auto& entry : folder.entryInfoList({"cpp_*"}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        const auto canonical = entry.canonicalFilePath();
        if (QFileInfo(canonical).dir().canonicalPath() == root && entry.lastModified().daysTo(now) >= 30)
            QDir(canonical).removeRecursively();
    }
}
}

Session::Session(QString workspace, QString client, QString studioRoot, QString simulationSeed)
    : workspace(std::move(workspace)), client(std::move(client)), studioRoot(std::move(studioRoot)), simulationSeed(std::move(simulationSeed)) {
    for (auto* process : {&kernel, &motion, &bootstrap, &worker}) hideConsole(*process);
    clock.start();
    timer.setInterval(60);
    connect(&timer, &QTimer::timeout, this, &Session::poll);
    connect(&worker, &QProcess::readyReadStandardOutput, this, &Session::receive);
    connect(&worker, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        receive();
        if (!closing && note) note("控制端已退出。实机状态需要现场确认。");
        lost = !closing;
        feedback.clear();
        pending.clear();
        waitingStop = control = ownedRun = waitingEstop = false;
        cleanVirtual();
        closing = false;
        if (ended) ended(lost);
        if (changed) changed();
    });
    connect(&worker, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) fail("无法启动现有 helix_pc.exe。");
    });
    timer.start();
}

Session::~Session() {
    timer.stop();
    // QProcess destruction can emit finished after Session's feedback buffers are destroyed.
    for (auto* process : {&worker, &bootstrap, &motion, &kernel})
        QObject::disconnect(process, nullptr, this, nullptr);
    disconnect();
    cleanVirtual();
}

bool Session::connected() const {
    return worker.state() != QProcess::NotRunning;
}

bool Session::fresh() const {
    return connected() && lastStatus >= 0 && clock.elapsed() - lastStatus < 3000;
}

bool Session::canClose() const {
    if (preparing || waitingStop || waitingEstop || !pending.isEmpty()) return false;
    if (value("home_returning") == "1" || (value("home_return_result") == "unconfirmed" && value("motion_rest") != "1")) return false;
    return !control || !connected() || (fresh() && value("state") != "0" && value("state") != "3");
}

void Session::prepare(const Job& job) {
    if (active() || (!job.name.isEmpty() && !job.runnable())) return;
    preparing = simulated = true;
    lost = false;
    if (changed) changed();
    waitForVirtualPorts(job, clock.elapsed() + 3000);
}

void Session::waitForVirtualPorts(const Job& job, qint64 deadline) {
    if (!preparing) return;
    if (!virtualPortsFree()) {
        if (clock.elapsed() >= deadline) fail("隔离仿真端口 30316 / 30317 已占用。");
        else QTimer::singleShot(150, this, [this, job, deadline] { waitForVirtualPorts(job, deadline); });
        return;
    }
    const QDir studio(studioRoot);
    const auto seed = simulationSeed;
    const auto env = studio.filePath("RobotController_PC/Environment");
    if (studioRoot.isEmpty() || seed.isEmpty() || !QFileInfo::exists(QDir(env).filePath("1-NonReal.exe")) ||
        !QFileInfo::exists(QDir(env).filePath("2-RobotMoveControl.exe")) || !QDir(seed).exists("SRSystemFile") || !QDir(seed).exists("home")) {
        fail("未配置完整 STEPStudio 仿真环境。请先运行 Configure-STEPStudio.cmd，预览与 G 代码转换可独立使用。");
        return;
    }
    auto logs = qEnvironmentVariable("STEP_SIMULATION_RUN_ROOT");
    if (logs.isEmpty()) logs = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath("STEPWS");
    if (!QDir().mkpath(logs)) { fail("无法创建厂家仿真临时目录。请设置 STEP_SIMULATION_RUN_ROOT 到可写的短路径。"); return; }
    wchar_t shortPath[32768]{};
    const auto nativeLogs = QDir::toNativeSeparators(QDir(logs).absolutePath()).toStdWString();
    const DWORD shortLength = GetShortPathNameW(nativeLogs.c_str(), shortPath, 32768);
    logs = shortLength > 0 && shortLength < 32768 ? QString::fromWCharArray(shortPath) : QDir(logs).absolutePath();
    if (logs.size() > 64) { fail("厂家仿真目录过长。请将 STEP_SIMULATION_RUN_ROOT 设置到较短的可写路径后重启工作台。"); return; }
    cleanOldRuns(logs);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("STEP_WORKSTATION_RUN_ROOT", QDir::toNativeSeparators(logs));
    environment.insert("STEP_VIRTUAL_KERNEL", QDir::toNativeSeparators(QDir(env).filePath("1-NonReal.exe")));
    bootstrap.setProcessEnvironment(environment);
    worker.setProcessEnvironment(environment);
    QTemporaryDir run(QDir(logs).filePath("cpp_XXXXXX"));
    if (!run.isValid()) { fail("无法创建隔离仿真目录。"); return; }
    run.setAutoRemove(false);
    logFolder = run.path();
    const auto station = QDir(run.path()).filePath("station");
    const auto system = QDir(station).filePath("SRSystemFile");
    const auto programs = QDir(system).filePath("UserPrograms");
    if (!copyTree(QDir(seed).filePath("home"), QDir(station).filePath("home")) ||
        !copyTree(QDir(seed).filePath("SRSystemFile"), system,
                  {"Backup", "Log", "ErrReport", "UserPrograms", "UserProgramsBackup", "UserProgramsRecyclebin"}) ||
        !copyTree(QDir(seed).filePath("SRSystemFile/UserPrograms/_global.sr"), QDir(programs).filePath("_global.sr"))) {
        fail("无法复制独立仿真工作站。");
        return;
    }
    const auto stationJob = job.project.isEmpty() ? readNativeJob(QDir(QFileInfo(client).absolutePath()).absoluteFilePath("../SR175_3200_Helix_Ready_25mms_v4.zip")) : job;
    if (!stationJob.project.isEmpty()) {
        const QDir project(QDir(programs).filePath(stationJob.project));
        if (!QDir().mkpath(project.path())) { fail("无法创建仿真作业目录。"); return; }
        for (auto file = stationJob.files.cbegin(); file != stationJob.files.cend(); ++file) {
            QFile output(project.filePath(file.key()));
            if (!output.open(QIODevice::WriteOnly) || output.write(file.value()) != file.value().size()) {
                fail("无法写入隔离仿真作业。");
                return;
            }
        }
    }
    const auto suffix = job.project == "GcodePreview.sr" ? "-gcode" : (job.project.isEmpty() ? "-manual" : "");
    kernel.setWorkingDirectory(env);
    kernel.setStandardOutputFile(QDir(logFolder).filePath("kernel.log"));
    kernel.start(QDir(env).filePath("1-NonReal.exe"), {station, "4"});
    QTimer::singleShot(2200, this, [this, env, station, suffix] {
        if (kernel.state() != QProcess::Running) { fail("虚拟控制器未启动，请检查 kernel.log。"); return; }
        motion.setWorkingDirectory(env);
        motion.setStandardOutputFile(QDir(logFolder).filePath("motion.log"));
        motion.start(QDir(env).filePath("2-RobotMoveControl.exe"), {station, "4"});
        QTimer::singleShot(2200, this, [this, suffix, station] {
            if (motion.state() != QProcess::Running) { fail("仿真运动进程未启动，请检查 motion.log。"); return; }
            const auto pid = QString::number(kernel.processId());
            const auto manualProfile = QDir(station).filePath("SRSystemFile/ConfigureFiles/RobotParameter.xml");
            bootstrap.setStandardOutputFile(QDir(logFolder).filePath("prepare.log"), QIODevice::Append);
            QObject::disconnect(&bootstrap, nullptr, this, nullptr);
            const auto prepareLog = QDir(logFolder).filePath("prepare.log");
            const auto prepareLogPos = QFileInfo(prepareLog).size();
            connect(&bootstrap, &QProcess::finished, this, [this, suffix, pid, manualProfile, prepareLog, prepareLogPos, attempts = 0](int code, QProcess::ExitStatus status) mutable {
                if (!preparing) return;
                const auto restart = [this, suffix, pid, manualProfile] {
                    QTimer::singleShot(250, this, [this, suffix, pid, manualProfile] {
                        if (preparing && QString::number(kernel.processId()) == pid)
                            bootstrap.start(client, {QString("virtual-prepare") + suffix, pid, manualProfile});
                    });
                };
                if (code == 3 && status == QProcess::NormalExit && kernel.state() == QProcess::Running && motion.state() == QProcess::Running) {
                    // Exit 3 preserves the helper's PID/path/port checks. A newly started vendor
                    // controller can expose its listeners later than its process and motion service.
                    restart();
                    return;
                }
                if (code || status != QProcess::NormalExit) {
                    // SDK 后台线程收尾时可能访问违规，把已完成的准备变成崩溃退出。以 prepare.log 中
                    // 本次尝试的 virtual_prepared= 为准：读到成功标记就照常接入，否则重试一次。
                    const auto tries = attempts++;
                    QTimer::singleShot(150, this, [this, restart, suffix, pid, manualProfile, prepareLog, prepareLogPos, tries, code, status] {
                        if (!preparing) return;
                        QFile record(prepareLog);
                        QString tail;
                        if (record.open(QIODevice::ReadOnly)) {
                            record.seek(qMin<qint64>(prepareLogPos, record.size()));
                            tail = QString::fromLocal8Bit(record.readAll());
                        }
                        if (tail.contains("virtual_prepared=1")) {
                            if (note) note("SDK 控制端收尾崩溃，但本次准备已确认成功，按就绪接入。");
                            preparing = false;
                            attach(QString("virtual") + suffix, {pid, manualProfile});
                            return;
                        }
                        if (tries < 1) { restart(); return; }
                        fail(QString("仿真准备失败（退出码 %1，%2），请检查 prepare.log。")
                                 .arg(code)
                                 .arg(status == QProcess::NormalExit ? "正常退出" : "进程崩溃"));
                    });
                    return;
                }
                preparing = false;
                attach(QString("virtual") + suffix, {pid, manualProfile});
            });
            bootstrap.start(client, {QString("virtual-prepare") + suffix, pid, manualProfile});
            QTimer::singleShot(20000, this, [this, pid] {
                if (preparing && QString::number(kernel.processId()) == pid) fail("仿真准备超时。");
            });
        });
    });
}

void Session::connectReal(QString ip) {
    if (active()) return;
    simulated = false;
    logFolder.clear();
    QStringList params{ip};
    const auto seedProfile = simulationSeed.isEmpty() ? QString() :
        QDir(simulationSeed).filePath("SRSystemFile/ConfigureFiles/RobotParameter.xml");
    const auto bundled = QDir(QFileInfo(client).absolutePath()).filePath("RobotParameter.xml");
    const auto profile = !seedProfile.isEmpty() && QFileInfo::exists(seedProfile) ? seedProfile :
        QFileInfo::exists(bundled) ? bundled : QString();
    auto environment = QProcessEnvironment::systemEnvironment();
    if (!profile.isEmpty()) {
        environment.insert("STEP_WORKSTATION_RUN_ROOT", QDir::toNativeSeparators(QFileInfo(profile).absolutePath()));
        params << QDir::toNativeSeparators(profile);
    }
    worker.setProcessEnvironment(environment);
    attach("real", params);
}

void Session::attach(QString mode, QStringList parameters) {
    feedback.clear();
    incoming.clear();
    pending.clear();
    model.clear();
    identity.clear();
    address.clear();
    lastStatus = -1;
    lost = control = waitingStop = waitingEstop = ownedRun = timeoutStop = commandSent = closing = false;
    outstanding = true;
    parameters.prepend(mode);
    parameters << "--panel";
    worker.start(client, parameters);
    if (changed) changed();
}

bool Session::send(const QString& command) {
    return worker.state() == QProcess::Running && worker.write((command + '\n').toUtf8()) >= 0;
}

bool Session::request(QString command) {
    if (!control || !fresh() || waitingStop || waitingEstop || !pending.isEmpty()) return false;
    pending = std::move(command);
    queuedAt = clock.elapsed();
    if (changed) changed();
    return true;
}

void Session::stop() {
    if (!control || !connected() || waitingStop) return;
    const bool cancelled = !pending.isEmpty() && !commandSent;
    const bool start = cancelled && (pending == "start" || pending.startsWith("sendrun ") || pending.startsWith("runproj "));
    pending.clear();
    if (cancelled && note) note("已取消排队中的操作。");
    waitingStop = send("stop");
    if (note) note(waitingStop ? "停止发送请求已提交。" : "停止请求未送达控制端。");
    if (start && event) event("start_cancelled");
    if (changed) changed();
}

void Session::disconnect() {
    if (!connected() || !canClose()) return;
    closing = true;
    send("quit");
    if (!worker.waitForFinished(1300)) worker.kill();
}

// 急停绕过回读队列直发：点动/回程循环和启动预检都会因管道里出现 estop 而立即中断。
bool Session::emergencyStop() {
    if (!control || !connected()) return false;
    pending.clear();
    waitingEstop = true;
    const bool ok = send("estop");
    if (note) note(ok ? "急停指令已直发（STOP+MSTOP）。" : "急停指令未送达控制端。");
    if (changed) changed();
    return ok;
}

void Session::cleanVirtual() {
    preparing = false;
    for (auto* process : {&bootstrap, &motion, &kernel}) {
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(3000);
        }
    }
}

void Session::fail(QString why) {
    cleanVirtual();
    if (note) note(why + (logFolder.isEmpty() ? "" : " 日志：" + logFolder));
    if (changed) changed();
}

void Session::receive() {
    incoming.append(worker.readAllStandardOutput());
    for (auto newline = incoming.indexOf('\n'); newline >= 0; newline = incoming.indexOf('\n')) {
        const auto line = QString::fromLocal8Bit(incoming.left(newline)).trimmed();
        incoming.remove(0, newline + 1);
        handle(line);
    }
    if (changed) changed();
}

void Session::handle(const QString& line) {
    static const QRegularExpression keyValue(R"((\w+)=([^\s]*))");
    auto fields = [&] {
        QMap<QString, QString> result;
        auto items = keyValue.globalMatch(line);
        while (items.hasNext()) {
            const auto match = items.next();
            result[match.captured(1)] = match.captured(2);
        }
        return result;
    };
    if (line.startsWith("home_plan=")) {
        const auto parts = line.mid(10).split('|');
        if (parts.size() == 5 && !parts[0].isEmpty() && !parts[4].isEmpty()) {
            feedback["home_plan_id"] = parts[0];
            feedback["home_plan_speed"] = parts[1];
            feedback["home_plan_seconds"] = parts[2];
            feedback["home_plan_tool"] = parts[3];
            feedback["home_path"] = parts[4];
            feedback["home_plan_ready"] = "1";
            feedback.remove("home_plan_error");
            if (note) note("回程路径预览已生成，等待界面确认。");
        } else if (note) note("回程路径预览数据无效，运动未开始。");
        if (changed) changed();
    } else if (line.startsWith("home_plan_error=")) {
        feedback["home_plan_ready"] = "0";
        feedback["home_plan_error"] = line.mid(16);
        if (note) note("回程规划失败：" + line.mid(16) + "。机器人未移动。");
        if (event) event("home_plan_failed");
        if (changed) changed();
    } else if (line.startsWith("robot=")) {
        const auto data = fields();
        control = data.value("access") == "start_stop";
        model = data.value("robot") + " / " + data.value("cabinet");
        address = data.value("ip");
        identity = data.value("serial");
        if (note) note(model + (control ? " · 已授权" : " · 只读"));
    } else if (line.startsWith("status=connected")) {
        const auto previous = value("state");
        const auto priorPlanId = feedback.value("home_plan_id");
        const auto priorPath = feedback.value("home_path");
        const auto priorTool = feedback.value("home_plan_tool");
        feedback = fields();
        if (!priorPlanId.isEmpty() && feedback.value("home_plan_id") == priorPlanId && !priorPath.isEmpty()) {
            feedback["home_path"] = priorPath;
            feedback["home_plan_tool"] = priorTool;
        }
        if (value("home_returning") == "1") ownedRun = true;
        else if (value("home_return_result") == "arrived" || (value("home_return_result") == "unconfirmed" && value("motion_rest") == "1")) ownedRun = false;
        if (value("state") != "0" && (previous == "0" || value("outcome") == "stopped" || value("outcome") == "completed")) ownedRun = false;
        lastStatus = clock.elapsed();
        if (value("command_busy") != "1") {
            outstanding = false;
            if (commandSent) {
                pending.clear();
                commandSent = false;
            }
        }
        if (sampled) sampled();
    } else if (line.startsWith("status=unknown")) {
        lastStatus = -1;
        feedback.clear();
        outstanding = commandSent = false;
        pending.clear();
        waitingEstop = false;
    } else if (line == "start_request_rc=0" || line.startsWith("home_return_request_rc=0")) {
        ownedRun = true;
        timeoutStop = false;
    } else if (line == "command_done=stop") {
        waitingStop = false;
    } else if (line == "command_done=estop") {
        waitingEstop = false;
    } else if (!line.isEmpty() && note) {
        if (line.startsWith("stop_confirmed=")) ownedRun = false;
        note(line);
    }
}

void Session::poll() {
    if (connected()) {
        if (!pending.isEmpty() && !commandSent && clock.elapsed() - queuedAt >= 3000) {
            pending.clear();
            if (note) note("等待回读超时，操作未发送。");
            if (event) event("queue_timeout");
        }
        if (control && ownedRun && (!fresh() || (value("home_return_result") == "unconfirmed" && value("motion_rest") != "1")) && !waitingStop && !timeoutStop) {
            timeoutStop = true;
            waitingStop = send("stop");
            if (note) note(waitingStop ? "回读超时，已尝试停止发送。" : "回读超时，停止未送达。");
        }
        if (!outstanding && !waitingStop && !waitingEstop && (lastStatus < 0 || clock.elapsed() - lastStatus >= 230)) {
            commandSent = !pending.isEmpty();
            outstanding = send(commandSent ? pending : "status");
            if (commandSent && !outstanding) {
                commandSent = false;
                pending.clear();
                if (note) note("命令未送达控制端。");
                if (event) event("send_failed");
            }
        }
    }
    if (changed) changed();
}
