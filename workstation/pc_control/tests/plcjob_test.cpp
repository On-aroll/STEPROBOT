#include "../workstation_plcjob.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>
#include <QVector>

#include <cmath>
#include <iostream>

namespace {

int failures = 0;
void check(bool ok, const char* reason) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << reason << '\n';
    }
}

template <typename Pred>
bool waitFor(Pred pred, int ms) {
    if (pred()) return true;
    QEventLoop loop;
    QTimer deadline, tick;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&tick, &QTimer::timeout, &loop, &QEventLoop::quit);
    tick.start(15);
    deadline.start(ms);
    while (!pred() && deadline.isActive()) loop.exec();
    return pred();
}

quint32 u32(const QVector<quint16>& v, int off) {
    return (quint32(v.value(off)) << 16) | quint32(v.value(off + 1));
}

// AC802 最小从站：协议 V1.2 状态区（100–139）+ 登记/暂存/提交/开始/中止/复位/撤销
// 的最小回执路径，验证上位机客户端的帧格式、序号、来源校验字段与状态解析。
// 完整 PLC 状态机（心跳失联、超时、设备故障锁存）由 plc_sim.py 自测覆盖。
class PlcSimulator : public QObject {
public:
    bool start() {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            client = server.nextPendingConnection();
            connect(client, &QTcpSocket::readyRead, this, [this] {
                rx += client->readAll();
                process();
            });
        });
        return server.listen(QHostAddress::LocalHost);
    }
    quint16 port() const { return server.serverPort(); }

    // —— 观测面 ——
    QVector<quint16> status = QVector<quint16>(PlcProto::kStatusWords, 0);
    QVector<quint16> lastCmdBlock;      // 最近一次 FC16 整块写 0–39
    QVector<quint16> lastCommitBlock;   // 最近一次 FC16 写 36–39
    QVector<quint16> hostHeartbeats;    // 上位机心跳写入序列
    quint32 plcSession = 0x12345678;
    quint32 lastCommandEcho = 0;

    void restartAs(quint32 newSession) {
        registered = false;
        registeredHost = 0;
        preparedSeq = acceptedSeq = completedSeq = 0;
        cellState = PlcProto::Idle;
        statusFlags = 0;
        lastCommandEcho = 0;
        plcSession = newSession;
        lastCmdBlock.clear();
        status[PlcProto::sAuxFlags] = PlcProto::afCoolingOk;
        publish();
    }

    void setPos(double x, double y, double z) {
        const auto put = [this](int off, double mm) {
            const quint32 raw = quint32(qint32(std::lround(mm * 100.0)));
            status[off] = quint16(raw >> 16);
            status[off + 1] = quint16(raw & 0xFFFFu);
        };
        put(PlcProto::sRobotPos, x);
        put(PlcProto::sRobotPos + 2, y);
        put(PlcProto::sRobotPos + 4, z);
    }

private:
    void publish() {
        const auto put32 = [this](int off, quint32 v) {
            status[off] = quint16(v >> 16);
            status[off + 1] = quint16(v & 0xFFFFu);
        };
        status[PlcProto::sVersion] = PlcProto::kProtocolVersion;
        status[PlcProto::sCellState] = quint16(cellState);
        // 模拟冷却常态正常（afShieldGas 由工艺输出驱动，简化不置）
        status[PlcProto::sAuxFlags] = PlcProto::afCoolingOk;
        put32(PlcProto::sPrepared, preparedSeq);
        put32(PlcProto::sAccepted, acceptedSeq);
        put32(PlcProto::sCompleted, completedSeq);
        put32(PlcProto::sPlcSess, plcSession);
        status[PlcProto::sStatusFlags] = quint16(statusFlags & 0xFFFFu);
        status[PlcProto::sLastCmd] = quint8(lastCommandEcho);
    }

    void applyCommand() {
        const quint32 cmd = lastCmdBlock.value(PlcProto::oCommand);
        const quint32 reqSeq = u32(lastCmdBlock, PlcProto::oReqSeq);
        const quint32 host = u32(lastCmdBlock, PlcProto::oHostSess);
        const quint32 expected = u32(lastCmdBlock, PlcProto::oExpected);
        lastCommandEcho = cmd;
        if (lastCmdBlock.value(PlcProto::oVersion) != PlcProto::kProtocolVersion || reqSeq == 0) {
            publish();
            return;
        }
        if (cmd == PlcProto::CmdRegister) {
            // 登记只在未登记时接受；Expected 不参与登记校验（协议 §4.1）
            if (registered) {
                publish();
                return;
            }
            registeredHost = host;
            registered = true;
            statusFlags |= PlcProto::sfSession;
            publish();
            return;
        }
        // 来源校验：不符按未处理命令静默忽略（协议 §4.2）
        if (!registered || host != registeredHost || expected != plcSession) {
            publish();
            return;
        }
        switch (cmd) {
        case PlcProto::CmdPrepare: {
            const quint16 recipe = lastCmdBlock.value(PlcProto::oRecipe);
            if (recipe >= 1 && recipe <= 3) {
                preparedSeq = reqSeq;
                cellState = PlcProto::Prepared;
                statusFlags |= PlcProto::sfStaging;
                // 空跑模式跟随命令块 bit0（协议 §3 StatusFlags.bit5）
                if (lastCmdBlock.value(PlcProto::oFlags) & PlcProto::kFlagDryRun)
                    statusFlags |= PlcProto::sfDryRun;
                else
                    statusFlags &= ~PlcProto::sfDryRun;
            }
            break;
        }
        case PlcProto::CmdStart:
            if (cellState == PlcProto::Accepted) cellState = PlcProto::Arming;
            break;
        case PlcProto::CmdAbort:
            if (cellState >= PlcProto::Accepted && cellState <= PlcProto::Finishing) {
                completedSeq = acceptedSeq;
                cellState = PlcProto::Aborted;
                statusFlags |= PlcProto::sfDonePending;
                statusFlags &= ~PlcProto::sfActive;
            }
            break;
        case PlcProto::CmdReset:
            if (cellState == PlcProto::Aborted || cellState == PlcProto::Done || cellState == PlcProto::Fault) {
                cellState = PlcProto::Idle;
                statusFlags &= ~PlcProto::sfDonePending;
            }
            break;
        case PlcProto::CmdLogout:
            registered = false;
            registeredHost = 0;
            statusFlags &= ~PlcProto::sfSession;
            cellState = PlcProto::Idle;
            break;
        default:
            break;
        }
        publish();
    }

    void applyCommit() {
        const quint32 commitSeq = u32(lastCommitBlock, 0);
        if (cellState == PlcProto::Prepared && commitSeq == preparedSeq) {
            acceptedSeq = commitSeq;
            cellState = PlcProto::Accepted;
            statusFlags |= PlcProto::sfActive;
            statusFlags &= ~PlcProto::sfStaging;
        }
        publish();
    }

    void process() {
        while (rx.size() >= 8) {
            const int length = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(rx.constData() + 4));
            const int total = 6 + length;
            if (rx.size() < total) return;
            const quint16 id = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(rx.constData()));
            const quint8 unitId = static_cast<quint8>(rx[6]);
            const quint8 fc = static_cast<quint8>(rx[7]);
            const QByteArray pdu = rx.mid(8, total - 8);
            rx.remove(0, total);
            if (unitId != 1) continue;
            if (fc == 3 || fc == 4) {
                const auto start = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(pdu.constData()));
                const auto count = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(pdu.constData() + 2));
                QByteArray out;
                if (start == PlcProto::kStatusBase && count == PlcProto::kStatusWords) {
                    // 模拟 PLC 任务周期：每次读回 PlcHeartbeat 低字 +1
                    ++plcTicks;
                    status[PlcProto::sPlcHb + 1] = quint16(plcTicks & 0xFFFFu);
                    publish();
                    out.append(char(fc)).append(char(2 * count));
                    for (int i = 0; i < count; ++i) {
                        const quint16 big = qToBigEndian<quint16>(status[i]);
                        out.append(reinterpret_cast<const char*>(&big), 2);
                    }
                } else {
                    out.append(char(fc | 0x80)).append(char(2));
                }
                send(id, unitId, out);
            } else if (fc == 6) {
                const auto address = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(pdu.constData()));
                const auto value = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(pdu.constData() + 2));
                if (address < PlcProto::kCmdWords) {
                    cmdRegs[address] = value;
                    if (address == PlcProto::oHeartbeat + 1)
                        hostHeartbeats.append(value);
                }
                send(id, unitId, QByteArray(1, char(fc)) + pdu.left(4));
            } else if (fc == 16) {
                const auto address = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(pdu.constData()));
                const auto count = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(pdu.constData() + 2));
                for (int i = 0; i < count; ++i) {
                    const auto value = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(pdu.constData() + 5 + 2 * i));
                    if (address + i < PlcProto::kCmdWords) cmdRegs[address + i] = value;
                }
                if (address == PlcProto::kCmdBase && count == PlcProto::kCmdWords) {
                    lastCmdBlock = QVector<quint16>(cmdRegs, cmdRegs + PlcProto::kCmdWords);
                    applyCommand();
                } else if (address == PlcProto::oCommit && count == 4) {
                    // 提交块的心跳一并记录：验证 CommitSeq+心跳原子到达（FC16 写 36–39）
                    lastCommitBlock = QVector<quint16>(cmdRegs + PlcProto::oCommit, cmdRegs + PlcProto::oCommit + 4);
                    hostHeartbeats.append(cmdRegs[PlcProto::oHeartbeat + 1]);
                    applyCommit();
                }
                send(id, unitId, QByteArray(1, char(fc)) + pdu.left(4));
            } else {
                QByteArray out;
                out.append(char(fc | 0x80)).append(char(1));
                send(id, unitId, out);
            }
        }
    }

    void send(quint16 id, quint8 unitId, const QByteArray& pdu) {
        QByteArray frame;
        const quint16 bigId = qToBigEndian<quint16>(id);
        frame.append(reinterpret_cast<const char*>(&bigId), 2);
        frame.append('\0').append('\0');
        const quint16 length = qToBigEndian<quint16>(static_cast<quint16>(1 + pdu.size()));
        frame.append(reinterpret_cast<const char*>(&length), 2);
        frame.append(char(unitId));
        frame += pdu;
        client->write(frame);
    }

    QTcpServer server;
    QTcpSocket* client = nullptr;
    QByteArray rx;
    quint16 cmdRegs[PlcProto::kCmdWords] = {};
    quint32 plcTicks = 0;
    bool registered = false;
    quint32 registeredHost = 0;
    quint32 cellState = PlcProto::Idle;
    quint32 statusFlags = 0;
    quint32 preparedSeq = 0, acceptedSeq = 0, completedSeq = 0;
};

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    PlcSimulator sim;
    if (!sim.start()) {
        std::cerr << "FAIL: simulator listen\n";
        return 1;
    }

    PlcJobClient client;
    PlcProto::PlcStatus st;
    int statusCount = 0, rejectCount = 0;
    bool linkUp = false;
    QString rejectText;
    QObject::connect(&client, &PlcJobClient::statusUpdated, [&](const PlcProto::PlcStatus& s) {
        st = s;
        ++statusCount;
    });
    QObject::connect(&client, &PlcJobClient::rejected, [&](const QString& m) {
        rejectText = m;
        ++rejectCount;
    });
    QObject::connect(&client, &PlcJobClient::linkChanged, [&](bool up, const QString&) {
        linkUp = up;
    });

    // 未连接：提交与登记直接拒绝，不产生任何 Modbus 流量
    client.commitPrepared();
    check(rejectCount == 1 && rejectText.contains("未连接"), "未连接时提交被拒绝");
    client.registerSession();
    check(rejectCount == 2, "未连接时登记被拒绝");

    client.connectToPlc("127.0.0.1", sim.port());
    check(waitFor([&] { return linkUp; }, 3000), "连接建立");
    check(waitFor([&] { return statusCount > 0 && st.valid; }, 3000), "状态区首读有效");
    check(st.cellState == PlcProto::Idle, "初始单元状态 Idle");
    check(st.plcSession == 0x12345678, "PlcSessionID 解析正确");
    check(st.plcHeartbeat != 0, "PLC 心跳递增中");
    check((st.auxFlags & PlcProto::afCoolingOk) != 0, "辅助标志解析");

    // 已连接但尚无 PreparedSeq 回执：提交被拒
    client.commitPrepared();
    check(rejectCount == 3 && rejectText.contains("PreparedSeq"), "无 PreparedSeq 时不提交");

    // 登记：命令块格式 + 登记回执
    client.registerSession();
    check(waitFor([&] { return st.sessionRegistered(); }, 3000), "登记回执 sfSession");
    check(sim.lastCmdBlock.value(PlcProto::oVersion) == 2, "块版本=2");
    check(sim.lastCmdBlock.value(PlcProto::oCommand) == PlcProto::CmdRegister, "登记命令=1");
    check(u32(sim.lastCmdBlock, PlcProto::oReqSeq) == 1, "首个 RequestSeq=1");
    const quint32 hostAtRegister = u32(sim.lastCmdBlock, PlcProto::oHostSess);
    check(hostAtRegister != 0, "HostSessionID 非零");
    check(u32(sim.lastCmdBlock, PlcProto::oExpected) == 0x12345678, "登记携带已学习的 PlcSessionID");
    check(sim.lastCmdBlock.value(PlcProto::oHeartbeat + 1) != 0, "心跳字段已写");
    check(sim.lastCommandEcho == PlcProto::CmdRegister, "LastCommandEcho 回显");

    // 准备（配方 1，完整参数含 V1.2 工艺参数，32 位 JobID）
    client.prepareJob(1, 0x00030002, false, 1500, 300, 3000, 50, 20, 1000);
    check(waitFor([&] { return st.cellState == PlcProto::Prepared && st.preparedSeq == 2; }, 3000),
          "PreparedSeq=2 回执");
    check(sim.lastCmdBlock.value(PlcProto::oCommand) == PlcProto::CmdPrepare, "准备命令=2");
    check(sim.lastCmdBlock.value(PlcProto::oRecipe) == 1, "配方号写入");
    check(sim.lastCmdBlock.value(PlcProto::oFlags) == 0, "空跑位未置");
    check(sim.lastCmdBlock.value(PlcProto::oPower) == 1500, "功率写入");
    check(sim.lastCmdBlock.value(PlcProto::oWire) == 300, "丝速写入");
    check(sim.lastCmdBlock.value(PlcProto::oPostGas) == 3000, "后送气写入");
    check(sim.lastCmdBlock.value(PlcProto::oDuty) == 50, "V1.2 占空比写入偏移 15");
    check(sim.lastCmdBlock.value(PlcProto::oFreq) == 20, "V1.2 脉冲频率写入偏移 16");
    check(sim.lastCmdBlock.value(PlcProto::oPreGas) == 1000, "V1.2 预送气写入偏移 17");
    check(sim.lastCmdBlock.size() == 40, "命令块整块 40 字");
    check(u32(sim.lastCmdBlock, PlcProto::oJobId) == 0x00030002, "JobID 32 位写入");
    check(u32(sim.lastCmdBlock, PlcProto::oHostSess) == hostAtRegister, "HostSessionID 登记后不变");
    check(u32(sim.lastCmdBlock, PlcProto::oExpected) == 0x12345678, "Expected 携带 PlcSessionID");
    check(u32(sim.lastCmdBlock, PlcProto::oReqSeq) == 2, "RequestSeq 递增");

    // 重暂存（配方 2，空跑，默认参数）
    client.prepareJob(2, 7, true);
    check(waitFor([&] { return st.preparedSeq == 3 && st.dryRun(); }, 3000), "空跑重暂存回执");
    check(sim.lastCmdBlock.value(PlcProto::oRecipe) == 2, "重暂存配方号");
    check((sim.lastCmdBlock.value(PlcProto::oFlags) & PlcProto::kFlagDryRun) != 0, "空跑位置位");
    check(sim.lastCmdBlock.value(PlcProto::oPower) == 0, "空跑默认参数");
    check(sim.lastCmdBlock.value(PlcProto::oDuty) == 0 && sim.lastCmdBlock.value(PlcProto::oFreq) == 0 &&
          sim.lastCmdBlock.value(PlcProto::oPreGas) == 0, "V1.2 参数缺省全 0=配方默认");
    check(u32(sim.lastCmdBlock, PlcProto::oJobId) == 7, "重暂存 JobID");

    // 提交：FC16 整块写 36–39（CommitSeq + 心跳原子完成）
    sim.lastCommitBlock.clear();
    const quint32 preparedSeq = st.preparedSeq;
    client.commitPrepared();
    check(waitFor([&] { return sim.lastCommitBlock.size() == 4; }, 3000), "提交块经 FC16 写 36–39 发出");
    check(u32(sim.lastCommitBlock, 0) == preparedSeq, "CommitSeq=最近 PreparedSeq");
    check(waitFor([&] { return st.cellState == PlcProto::Accepted && st.acceptedSeq == preparedSeq; }, 3000),
          "AcceptedSeq 回执");

    // 开始
    client.startJob();
    check(waitFor([&] { return st.cellState == PlcProto::Arming; }, 3000), "开始后进入 Arming");
    check(sim.lastCmdBlock.value(PlcProto::oCommand) == PlcProto::CmdStart, "开始命令=4");

    // 中途中止：CompletedSeq 记作业序号，而非中止命令自身的序号（协议 V1.1 §5）
    client.abortJob();
    check(waitFor([&] { return st.cellState == PlcProto::Aborted; }, 3000), "中止流程走完进 Aborted");
    check(st.completedSeq == preparedSeq, "CompletedSeq=作业序号");
    check(u32(sim.lastCmdBlock, PlcProto::oReqSeq) > preparedSeq, "中止命令序号独立递增");

    // 复位 → Idle
    client.faultReset();
    check(waitFor([&] { return st.cellState == PlcProto::Idle; }, 3000), "复位回 Idle");
    check(sim.lastCmdBlock.value(PlcProto::oCommand) == PlcProto::CmdReset, "复位命令=6");

    // 心跳严格递增（提交块与每轮读回后的 FC06 都应递增）
    check(sim.hostHeartbeats.size() >= 5, "心跳写入次数充足");
    bool monotonic = true;
    for (int i = 1; i < sim.hostHeartbeats.size(); ++i)
        if (sim.hostHeartbeats[i] <= sim.hostHeartbeats[i - 1]) {
            monotonic = false;
            break;
        }
    check(monotonic, "上位机心跳严格递增");

    // PLC 重启：PlcSessionID 变化 → 客户端读到新会话标识，重新登记自动携带
    sim.restartAs(0x778899AA);
    check(waitFor([&] { return !st.sessionRegistered() && st.plcSession == 0x778899AA; }, 3000),
          "重启后读到新会话标识");
    client.registerSession();
    check(waitFor([&] { return u32(sim.lastCmdBlock, PlcProto::oExpected) == 0x778899AA; }, 3000),
          "重新登记携带新 Expected");
    check(waitFor([&] { return st.sessionRegistered(); }, 3000), "新会话登记成功");

    // 状态区解析：位置定点 mm×100、设备标志、反馈值
    sim.setPos(1234.56, 987.65, 12.34);
    sim.status[PlcProto::sRobotFlags] = PlcProto::rfArmReq | PlcProto::rfProcessActive;
    sim.status[PlcProto::sLaserFlags] = PlcProto::lfReady | PlcProto::lfRemoteMode;
    sim.status[PlcProto::sWireFlags] = PlcProto::wfFeeding;
    sim.status[PlcProto::sLaserFb] = 1480;
    sim.status[PlcProto::sWireFb] = 295;
    check(waitFor([&] {
        return (st.robotFlags & PlcProto::rfProcessActive) != 0 && std::fabs(st.robotX - 1234.56) < 0.01;
    }, 3000), "机器人标志与位置读回");
    check(std::fabs(st.robotY - 987.65) < 0.01 && std::fabs(st.robotZ - 12.34) < 0.01, "Y/Z 定点解析");
    check((st.robotFlags & PlcProto::rfArmReq) != 0, "ArmReq 标志");
    check((st.laserFlags & PlcProto::lfRemoteMode) != 0 && (st.laserFlags & PlcProto::lfFault) == 0, "激光标志");
    check((st.wireFlags & PlcProto::wfFeeding) != 0, "送丝标志");
    check(st.laserPowerFb == 1480 && st.wireSpeedFb == 295, "功率/丝速反馈");
    check(PlcProto::faultText(PlcProto::FaultLaser).contains("激光"), "故障文案可读");

    std::cout << "plcjob_test: " << (failures ? "FAILURES" : "all checks passed") << " (" << failures << ")\n";
    return failures ? 1 : 0;
}
