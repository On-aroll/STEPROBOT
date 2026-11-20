#include "workstation_plcbridge.h"
#include <QHostAddress>
#include <QRandomGenerator>
#include <cmath>
#include <cstring>
#include <limits>

namespace BridgeProto {
namespace {
quint32 u32(const QVector<quint16>& w, int at) { return (quint32(w[at]) << 16) | w[at + 1]; }
}
Status decode(const QVector<quint16>& w) {
    Status s;
    if (w.size() != StatusWords || w[0] != Magic || (w[1] != LegacyVersion && w[1] != Version)) return s;
    s.version = w[1];
    if (s.version == Version) {
        if (w[57] != 1 || w[58] < 1 || w[58] > MaxPrograms || w[60] != 49 || w[61] != 56 || w[63] != 2) return {};
        s.selectionMode = w[57]; s.maxPrograms = w[58]; s.desiredProgramWord = w[59];
        s.gdiFirst = w[60]; s.gdiLast = w[61]; s.robotWriteWords = w[63];
    } else { s.maxPrograms = 4; s.robotWriteWords = 1; }
    s.valid = true; s.flags = w[2]; s.stage = w[3]; s.ackSeq = u32(w, 4);
    for (int i = 0; i < StatusWords; ++i) s.rawWords[i] = w[i];
    s.ackCommand = w[6]; s.result = w[7]; s.error = w[8]; s.hostHeartbeat = w[9];
    s.boot = u32(w, 10); s.plcHeartbeat = w[12]; s.selectedSlot = w[13];
    s.desiredWord = w[14]; s.owner = u32(w, 15); s.rawStatus = w[17]; s.robotState = w[18];
    for (int i = 0; i < 32; ++i) s.robotWords[i] = w[20 + i];
    s.rejectedSeq = u32(w, 52); s.rejectReason = w[54]; s.interruptedSeq = u32(w, 55);
    s.robotHeartbeatEdges = w[62];
    return s;
}
std::optional<float> Status::coordinate(int axis) const {
    if (!valid || !has(RobotOnline) || !has(CoordinatesRequested) || axis < 0 || axis >= 6) return {};
    const quint32 bits = (quint32(robotWords[21 + 2 * axis]) << 16) | robotWords[20 + 2 * axis];
    float value = 0; static_assert(sizeof(value) == sizeof(bits)); std::memcpy(&value, &bits, sizeof(value));
    if (!std::isfinite(value)) return {};
    return value;
}
std::optional<bool> Status::digitalOutput(int number) const {
    if (!valid || !has(RobotOnline) || number < 33 || number > 96) return {};
    const int bit = number - 33;
    return (robotWords[bit / 16] & (quint16(1) << (bit % 16))) != 0;
}
std::optional<bool> Status::desiredInput(int number) const {
    if (!valid || number < 33 || number > (version == Version ? 64 : 48)) return {};
    const auto word = number <= 48 ? desiredWord : desiredProgramWord;
    return (word & (quint16(1) << ((number - 33) % 16))) != 0;
}
std::optional<quint16> Status::analogRaw(int number) const {
    if (!valid || !has(RobotOnline) || number < 5 || number > 32) return {};
    return robotWords[number - 1];
}
QString stageText(quint16 s) {
    switch (s) {
    case 0: return "空闲"; case 10: return "脉冲前置低"; case 20: return "脉冲输出";
    case 30: return "脉冲后置低"; case 40: return "等待机器人反馈";
    default: return QString("未知阶段%1").arg(s);
    }
}
QString inputMeaning(int n, quint16 version) {
    if (version == Version) {
        if (n >= 33 && n <= 36) return "旧四槽选择（V2固定0）";
        if (n >= 49 && n <= 56) return QString("GDI程序号 bit%1（权重%2）").arg(n-49).arg(1 << (n-49));
        if (n >= 57 && n <= 64) return "保留（V2固定0）";
    }
    static const char* names[] = {"选择主程序槽1", "选择主程序槽2", "选择主程序槽3", "选择主程序槽4",
        "使能切换", "加载", "一键运行（未启用）", "再启动（未启用）", "运行/继续", "清错", "暂停", "停止",
        "1=XYZABC请求 / 0=AO21～32", "自定义（未启用）", "自定义（未启用）", "自定义（未启用）"};
    return n >= 33 && n <= 48 ? QString::fromUtf8(names[n - 33]) : QString("未映射DI%1").arg(n);
}
QString outputMeaning(int n) {
    static const char* names[] = {"运行", "故障", "使能", "暂停", "停止", "手动模式", "自动模式", "外部自动",
        "急停", "机器人心跳DO42", "自定义", "自定义", "自定义", "自定义", "自定义", "自定义"};
    return n >= 33 && n <= 48 ? QString::fromUtf8(names[n - 33]) : QString("自定义DO%1（语义未定义）").arg(n);
}
QString statusFieldName(int i) {
    static const char* names[] = {"接口签名", "接口版本", "状态标志字", "命令阶段", "回执序号·高字", "回执序号·低字",
        "回执命令", "回执结果", "回执错误", "PLC已接收上位机心跳", "PLC启动号·高字", "PLC启动号·低字", "PLC扫描心跳",
        "选中槽（不是加载身份）", "期望系统字（非写成功回执）", "会话所有者·高字", "会话所有者·低字", "机器人原始状态",
        "机器人状态码", "保留=0"};
    if (i >= 0 && i < 20) return QString::fromUtf8(names[i]);
    if (i >= 20 && i < 52) {
        const int r = i - 20;
        return r < 4 ? QString("机器人DO%1～%2").arg(33 + 16 * r).arg(48 + 16 * r) :
            QString("AO%1%2").arg(r + 1).arg(r >= 20 ? " / XYZABC复用区" : "");
    }
    switch (i) {
    case 52: return "拒绝序号·高字"; case 53: return "拒绝序号·低字"; case 54: return "最近拒绝原因";
    case 55: return "STOP抢占序号·高字"; case 56: return "STOP抢占序号·低字";
    case 57: return "V2选程序方式：1=GDI8（V1保留）";
    case 58: return "V2最大程序号（V1保留）";
    case 59: return "V2期望GDI字，非回读（V1保留）";
    case 60: return "V2 GDI起始DI（V1保留）";
    case 61: return "V2 GDI结束DI（V1保留）";
    case 62: return "机器人DO42跳变计数";
    case 63: return "V2机器人FC16写字数（V1保留）";
    default: return i >= 0 && i < StatusWords ? "保留=0" : "无效偏移";
    }
}
QString commandText(Command c) {
    switch (c) {
    case Register: return "登记"; case Enable: return "上使能"; case Disable: return "断使能";
    case Load: return "选择并加载"; case Run: return "运行/继续"; case Pause: return "暂停";
    case Stop: return "停止"; case ClearError: return "确认报警 / 清错"; case Logout: return "撤销会话";
    case Coordinates: return "切换坐标输出"; case Rearm: return "重置通讯停止锁存";
    case ConfirmLoad: return "人工确认加载";
    }
    return "未知命令";
}
QString resultText(quint16 r) {
    switch (r) {
    case None: return "无回执"; case Busy: return "执行中"; case Confirmed: return "反馈确认完成";
    case SentUnconfirmed: return "已发送，未确认"; case Rejected: return "拒绝";
    case Timeout: return "反馈超时"; case Interrupted: return "中断";
    default: return "未知结果";
    }
}
QString errorText(quint16 e) {
    static const char* messages[] = {"无", "协议版本不符", "会话所有者不符", "PLC启动标识不符",
        "请求序号无效", "正在执行其他命令", "无本地远程许可", "机器人离线", "命令或参数无效",
        "当前状态不允许", "动作反馈超时", "上位机心跳失联", "机器人故障或急停",
        "PLC映射未确认", "保留字段非零", "通讯停止锁存未复位"};
    return e < 16 ? QString::fromUtf8(messages[e]) : QString("未知错误 %1").arg(e);
}
QString robotStateText(const Status& s) {
    if (!s.valid || !s.has(RobotOnline)) return "离线/未知";
    if (s.emergency()) return "急停"; if (s.alarm()) return "故障";
    if (s.paused()) return "暂停"; if (s.running()) return "运行"; if (s.stopped()) return "停止";
    return "状态待确认";
}
}

PlcBridgeClient::PlcBridgeClient(QObject* parent) : QObject(parent) {
    transport.setPollRange(3, BridgeProto::StatusBase, BridgeProto::StatusWords);
    connect(&transport, &ModbusClient::registersRead, this, &PlcBridgeClient::onRegisters);
    connect(&transport, &ModbusClient::linkChanged, this, [this](bool up, const QString& text) {
        if (!up) resetSession();
        emit linkChanged(up, text); emit changed();
    });
    connect(&transport, &ModbusClient::writeAcknowledged, this, [this](int fc, int at, int count) {
        if (fc == 16 && at == BridgeProto::CommandBase && count == BridgeProto::CommandWords && pending()) {
            writeAccepted = true; // Transport receipt is NOT action completion.
        }
    });
    connect(&transport, &ModbusClient::failure, this, [this](const QString& text) {
        abandon(text + " 不确定命令不自动重发；需重新连接并显式登记。");
    });
    heartbeatTimer.setInterval(250);
    connect(&heartbeatTimer, &QTimer::timeout, this, [this] {
        if (ownsSession() && fresh()) transport.writeRegister(215, ++heartbeat);
    });
    healthTimer.setInterval(100); connect(&healthTimer, &QTimer::timeout, this, &PlcBridgeClient::tick);
}
void PlcBridgeClient::resetSession() {
    heartbeatTimer.stop(); healthTimer.stop(); registeredHere = false; pendingSeq = 0;
    loadSentSlot = 0; scanObserved = scanAdvanced = false; scanChangedAt.invalidate(); last = {};
}
void PlcBridgeClient::connectTo(const QString& host, quint16 port, quint8 unit) {
    if (isActive()) return;
    QHostAddress address(host);
    const auto ipv4 = address.toIPv4Address();
    const bool privateIp = (ipv4 >> 24) == 10 || (ipv4 >> 20) == 0xAC1 ||
                           (ipv4 >> 16) == 0xC0A8 || (ipv4 >> 24) == 127;
    if (address.protocol() != QAbstractSocket::IPv4Protocol || !privateIp || !port || unit == 0) {
        emit rejected("请填写 PLC LAN A 的私有 IPv4 地址和有效端口；Unit ID 不能为0。"); return;
    }
    endpointHost_ = address.toString(); endpointPort_ = port; endpointUnit_ = unit;
    resetSession(); sequence = 0; heartbeat = 0; session = 0;
    while (!session) session = QRandomGenerator::global()->generate();
    healthTimer.start(); transport.connectTo(address.toString(), port, unit); emit changed();
}
void PlcBridgeClient::disconnectFrom(const QString& reason) {
    if (pending()) emit notice("连接断开，在途命令结果可能未知，未自动重发。");
    resetSession(); transport.disconnectFrom(reason); emit changed();
}
bool PlcBridgeClient::fresh() const {
    return isConnected() && last.valid && last.has(BridgeProto::ScanValid) && scanAdvanced &&
           scanChangedAt.isValid() && scanChangedAt.elapsed() < 1500;
}
bool PlcBridgeClient::ownsSession() const {
    return registeredHere && last.has(BridgeProto::Registered) && last.has(BridgeProto::HostAlive) && last.owner == session;
}
bool PlcBridgeClient::canIssue(BridgeProto::Command c, quint16 p, QString* reason) const {
    using namespace BridgeProto;
    if (reason) reason->clear();
    const auto no = [reason](const char* message) { if (reason) *reason = message; return false; };
    if (!fresh()) return no("PLC签名、版本和动态扫描心跳尚未通过，或状态已失效");
    if (operationInhibited_ && c != Stop && c != Disable) return no("正在切换机器人模式，请等待实际模式确认");
    if (c < Register || c > ConfirmLoad) return no("命令无效");
    if ((c == Load || c == ConfirmLoad) ? (p < 1 || p > last.maxPrograms) : (c == Coordinates ? p > 1 : p != 0)) {
        if (reason) *reason = QString("参数无效；当前PLC V%1支持程序号1～%2").arg(last.version).arg(last.maxPrograms);
        return false;
    }
    if ((pending() || last.has(ActionBusy)) && c != Stop) return no("有在途命令，请等待PLC回执");
    if (c == Register) {
        if (last.has(Registered) && last.owner != session) return no("PLC已被其他会话占用");
        return true;
    }
    if (!ownsSession()) return no("请先显式登记本机会话");
    if (c == Logout) return last.has(RobotOnline) && last.stopped() && !last.enabled() ? true : no("撤销会话前须在线、停止并断使能");
    if (!last.has(Commissioned)) return no("PLC的xCommissioned尚未放行");
    if (!last.has(RobotOnline)) return no("PLC与机器人未建立有效动态通信");
    if (c == Stop) return true; // Ordinary STOP remains available after remote permit is revoked.
    if (!last.has(RemotePermit)) return no("PLC的xRemotePermit尚未放行");
    if (last.version == Version && !last.has(GdiMappingConfirmed) &&
        (c == Enable || c == Load || c == Run || c == ConfirmLoad || c == Rearm))
        return no("PLC本地尚未确认GDI DI49～56与程序绑定，需先完成V2现场配置");
    if (c != ClearError && c != Rearm && c != Coordinates && last.has(StopLatch)) return no("请先确认报警，再重置通讯停止锁存");
    switch (c) {
    case Enable: return last.stopped() && last.externalAuto() && !last.alarm() && !last.emergency() ? true : no("使能前须停止、外部自动且无故障/急停");
    case Disable: return last.stopped() ? true : no("断使能前须完全停止");
    case Load: return last.stopped() && last.externalAuto() && !last.alarm() && !last.emergency() ? true : no("加载前须停止、外部自动且无故障/急停");
    case Run: return last.enabled() && last.externalAuto() && !last.alarm() && !last.emergency() &&
        last.has(LoadConfirmed) && (last.stopped() || last.paused()) ? true : no("运行需使能、外部自动、无故障/急停并人工确认已加载程序");
    case Pause: return last.running() ? true : no("机器人未处于运行状态");
    case ClearError: return !last.running() && !last.paused() && !last.emergency() ? true : no("确认报警前须停止运行和暂停，并解除急停");
    case Coordinates: return last.stopped() ? true : no("切换坐标输出前须停止");
    case Rearm: return last.stopped() && !last.emergency() && !last.alarm() ? true : no("先确认报警；重置停止锁存前须停稳、无故障并解除急停");
    case ConfirmLoad: return loadSentSlot == p && last.selectedSlot == p && last.stopped() &&
        last.externalAuto() && !last.alarm() && !last.emergency() ? true : no("尚无本次加载已发送回执，或所选槽/机器人状态不符");
    default: return no("命令无效");
    }
}
bool PlcBridgeClient::issue(BridgeProto::Command c, quint16 parameter) {
    QString why;
    if (!canIssue(c, parameter, &why)) { emit rejected(why); return false; }
    if (sequence == std::numeric_limits<quint32>::max()) { emit rejected("请求序号已用尽，请断开后重新登记"); return false; }
    if (pending()) emit notice(QString("停止抢占请求%1；原命令结果以PLC回执为准。").arg(pendingSeq));
    pendingSeq = ++sequence; pendingCommand = c; pendingParameter = parameter;
    rejectBaseline = last.rejectedSeq; rejectReasonBaseline = last.rejectReason;
    writeAccepted = false; commandStarted.restart();
    if (c == BridgeProto::Load || c == BridgeProto::Stop || c == BridgeProto::Rearm) loadSentSlot = 0;
    QVector<quint16> frame(BridgeProto::CommandWords, 0);
    frame[0] = last.version; frame[1] = c; frame[2] = quint16(pendingSeq >> 16); frame[3] = quint16(pendingSeq);
    frame[4] = parameter; frame[6] = quint16(session >> 16); frame[7] = quint16(session);
    frame[8] = quint16(last.boot >> 16); frame[9] = quint16(last.boot); frame[15] = ++heartbeat;
    emit notice(QString("发送请求%1：%2，参数%3；等待PLC应用回执。").arg(pendingSeq).arg(BridgeProto::commandText(c)).arg(parameter));
    transport.writeRegisters(BridgeProto::CommandBase, frame); emit changed(); return true;
}
void PlcBridgeClient::onRegisters(int fc, int at, const QVector<quint16>& words, qint64) {
    using namespace BridgeProto;
    if (fc != 3 || at != StatusBase) return;
    const auto next = decode(words);
    if (!next.valid) { abandon("PLC签名/版本/能力块不匹配：须为0x4D42、V1或V2，V2须声明有效GDI8映射。"); return; }
    if (scanObserved && (next.version != last.version || next.maxPrograms != last.maxPrograms || next.selectionMode != last.selectionMode)) {
        abandon("PLC协议或选程序能力已改变；当前会话失效，请重新连接并显式登记。"); return;
    }
    if (scanObserved && next.boot != last.boot) { abandon("PLC启动标识已改变；当前会话失效，不自动登记或恢复动作。"); return; }
    if (!scanObserved || next.plcHeartbeat != last.plcHeartbeat) {
        if (scanObserved) scanAdvanced = true;
        scanChangedAt.restart();
    }
    scanObserved = true;
    last = next;
    if (!last.externalAuto() || last.alarm() || last.emergency() || !last.has(RobotOnline) || last.has(StopLatch)) loadSentSlot = 0;
    const bool logoutConfirmed = pending() && pendingCommand == Logout && last.ackSeq == pendingSeq &&
        last.ackCommand == Logout && last.result == Confirmed;
    if (registeredHere && !logoutConfirmed && (!last.has(Registered) || last.owner != session || !last.has(HostAlive))) {
        abandon("PLC会话所有权或心跳许可已失效；需要重新连接并显式登记。"); return;
    }
    if (pending() && writeAccepted) {
        quint16 result = None, error = 0;
        if (last.ackSeq == pendingSeq && last.ackCommand == pendingCommand &&
            (pendingCommand != Register || last.owner == session) && last.result >= Confirmed && last.result <= Interrupted) {
            result = last.result; error = last.error;
        } else if (last.rejectedSeq == pendingSeq &&
                   (last.rejectedSeq != rejectBaseline || last.rejectReason != rejectReasonBaseline)) {
            result = Rejected; error = last.rejectReason;
        }
        if (result != None) {
            const auto doneSeq = pendingSeq; const auto doneCmd = pendingCommand; const auto parameter = pendingParameter;
            pendingSeq = 0;
            if (doneCmd == Register && result == Confirmed && last.has(Registered) && last.owner == session) {
                registeredHere = true; heartbeatTimer.start();
            }
            if (doneCmd == Logout && result == Confirmed) { registeredHere = false; heartbeatTimer.stop(); loadSentSlot = 0; }
            if (doneCmd == Load && result == SentUnconfirmed) loadSentSlot = parameter;
            emit notice(QString("请求%1 %2：%3（%4）").arg(doneSeq).arg(commandText(doneCmd)).arg(resultText(result)).arg(errorText(error)));
            emit commandFinished(doneSeq, doneCmd, result, error);
        }
    }
    emit changed();
}
void PlcBridgeClient::tick() {
    if (scanObserved && scanChangedAt.isValid() && scanChangedAt.elapsed() >= 1500) {
        abandon("PLC扫描心跳超过1.5秒未变化，状态已失效。"); return;
    }
    if (pending() && commandStarted.elapsed() >= 13000) {
        abandon("13秒内未取得匹配的PLC命令回执；结果未知，未自动重发。"); return;
    }
    emit changed();
}
void PlcBridgeClient::abandon(const QString& message) {
    if (ending) return;
    ending = true; emit rejected(message); disconnectFrom(message); ending = false;
}
