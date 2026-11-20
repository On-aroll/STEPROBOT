#include "workstation_plcjob.h"

#include <QRandomGenerator>

namespace PlcProto {
namespace {
// 状态区 RobotPos：定点 mm×100 → double mm
double posMm(const QVector<quint16>& regs, int offset) {
    const qint32 raw = qint32(u32At(regs, offset));
    return raw / 100.0;
}
}  // namespace
}  // namespace PlcProto

PlcJobClient::PlcJobClient(QObject* parent)
    : QObject(parent) {
    while (hostSession == 0)
        hostSession = QRandomGenerator::global()->generate();
    qRegisterMetaType<PlcProto::PlcStatus>();
    connect(&modbus, &ModbusClient::linkChanged, this, &PlcJobClient::linkChanged);
    connect(&modbus, &ModbusClient::registersRead, this, &PlcJobClient::onRegisters);
    modbus.setPollRange(3, PlcProto::kStatusBase, PlcProto::kStatusWords);
}

void PlcJobClient::connectToPlc(const QString& host, quint16 port, quint8 unitId) {
    modbus.connectTo(host, port, unitId);
}

void PlcJobClient::disconnectFromPlc(const QString& reason) {
    modbus.disconnectFrom(reason);
}

void PlcJobClient::registerSession() {
    sendCommandBlock(PlcProto::CmdRegister, 0, 0, 0, 0, 0, 0);
}

void PlcJobClient::prepareJob(quint16 recipeId, quint32 jobId, bool dryRun,
                              quint16 powerW, quint16 wireSpeed, quint16 postGasMs,
                              quint16 dutyPct, quint16 pulseFreqHz, quint16 preGasMs) {
    sendCommandBlock(PlcProto::CmdPrepare, recipeId, dryRun ? PlcProto::kFlagDryRun : 0,
                     powerW, wireSpeed, postGasMs, jobId, dutyPct, pulseFreqHz, preGasMs);
}

void PlcJobClient::commitPrepared() {
    if (!modbus.isConnected()) {
        emit rejected(QStringLiteral("PLC 未连接，提交未发送"));
        return;
    }
    if (pendingCommitSeq == 0) {
        emit rejected(QStringLiteral("尚无 PreparedSeq 回执，不能提交"));
        return;
    }
    // FC16 整块写 36–39：提交 + 心跳一次原子完成（协议 V1.2 §2 写入规则）
    heartbeat = (heartbeat + 1) & 0xFFFFu;
    const QVector<quint16> tail{
        quint16(pendingCommitSeq >> 16), quint16(pendingCommitSeq & 0xFFFFu),
        quint16(heartbeat >> 16), quint16(heartbeat & 0xFFFFu)};
    modbus.writeRegisters(PlcProto::oCommit, tail);
}

void PlcJobClient::startJob() {
    sendCommandBlock(PlcProto::CmdStart, 0, 0, 0, 0, 0, 0);
}

void PlcJobClient::abortJob() {
    sendCommandBlock(PlcProto::CmdAbort, 0, 0, 0, 0, 0, 0);
}

void PlcJobClient::faultReset() {
    sendCommandBlock(PlcProto::CmdReset, 0, 0, 0, 0, 0, 0);
}

void PlcJobClient::logoutSession() {
    sendCommandBlock(PlcProto::CmdLogout, 0, 0, 0, 0, 0, 0);
}

void PlcJobClient::sendCommandBlock(quint16 command, quint16 recipe, quint16 flags,
                                    quint16 power, quint16 wire, quint16 postGas, quint32 jobId,
                                    quint16 duty, quint16 freq, quint16 preGas) {
    if (!modbus.isConnected()) {
        emit rejected(QStringLiteral("PLC 未连接，命令未发送"));
        return;
    }
    // 协议 §2：RequestSeq 接近 2^32 前必须撤销会话重新登记
    if (reqSeq >= 0xFFFFFFFEu) {
        emit rejected(QStringLiteral("RequestSeq 接近回绕，请撤销会话后重新登记"));
        return;
    }
    ++reqSeq;   // 只有真正发出的命令消耗序号，未连接时不递增
    QVector<quint16> block(PlcProto::kCmdWords, 0);
    block[PlcProto::oVersion] = PlcProto::kProtocolVersion;
    block[PlcProto::oCommand] = command;
    block[PlcProto::oReqSeq] = quint16(reqSeq >> 16);
    block[PlcProto::oReqSeq + 1] = quint16(reqSeq & 0xFFFFu);
    block[PlcProto::oJobId] = quint16(jobId >> 16);
    block[PlcProto::oJobId + 1] = quint16(jobId & 0xFFFFu);
    block[PlcProto::oRecipe] = recipe;
    block[PlcProto::oFlags] = flags;
    block[PlcProto::oPower] = power;
    block[PlcProto::oWire] = wire;
    block[PlcProto::oPostGas] = postGas;
    block[PlcProto::oDuty] = duty;
    block[PlcProto::oFreq] = freq;
    block[PlcProto::oPreGas] = preGas;
    block[PlcProto::oHostSess] = quint16(hostSession >> 16);
    block[PlcProto::oHostSess + 1] = quint16(hostSession & 0xFFFFu);
    block[PlcProto::oExpected] = quint16(expectedPlcSession >> 16);
    block[PlcProto::oExpected + 1] = quint16(expectedPlcSession & 0xFFFFu);
    block[PlcProto::oHeartbeat] = quint16(heartbeat >> 16);
    block[PlcProto::oHeartbeat + 1] = quint16(heartbeat & 0xFFFFu);
    modbus.writeRegisters(PlcProto::kCmdBase, block);
}

void PlcJobClient::onRegisters(int functionCode, int startAddress,
                               const QVector<quint16>& values, qint64) {
    using namespace PlcProto;
    if (functionCode != 3 || startAddress != kStatusBase || values.size() < kStatusWords)
        return;

    PlcStatus next;
    next.valid = values.value(sVersion) == kProtocolVersion;
    next.cellState = quint8(values.value(sCellState));
    next.preparedSeq = u32At(values, sPrepared);
    next.acceptedSeq = u32At(values, sAccepted);
    next.completedSeq = u32At(values, sCompleted);
    next.faultCode = quint8(values.value(sFault));
    next.faultDetail = quint8(values.value(sFaultDetail));
    // StatusFlags（偏移 109）与 RobotFlags（偏移 118）按协议 §3 是 16 位单字
    next.statusFlags = values.value(sStatusFlags);
    next.plcSession = u32At(values, sPlcSess);
    next.plcHeartbeat = u32At(values, sPlcHb);
    next.lastCommand = quint8(values.value(sLastCmd));
    next.robotProgram = values.value(sRobotProg);
    next.robotState = quint8(values.value(sRobotState));
    next.robotFlags = values.value(sRobotFlags);
    next.robotAlarm = values.value(sRobotAlarm);
    next.robotX = posMm(values, sRobotPos);
    next.robotY = posMm(values, sRobotPos + 2);
    next.robotZ = posMm(values, sRobotPos + 4);
    next.laserFlags = quint8(values.value(sLaserFlags));
    next.laserPowerFb = values.value(sLaserFb);
    next.wireFlags = quint8(values.value(sWireFlags));
    next.wireSpeedFb = values.value(sWireFb);
    next.auxFlags = quint8(values.value(sAuxFlags));

    // 学习 PLC 会话标识：重启后下次命令自动携带新 ExpectedPlcSessionID
    if (next.plcSession != 0)
        expectedPlcSession = next.plcSession;
    if (next.preparedSeq != 0)
        pendingCommitSeq = next.preparedSeq;

    last = next;
    notifyTransientFaults(next.faultDetail);
    emit statusUpdated(last);

    // 心跳：每次状态读回后 FC06 写低字（协议 V1.2 §2）
    heartbeat = (heartbeat + 1) & 0xFFFFu;
    modbus.writeRegister(kCmdBase + oHeartbeat + 1, quint16(heartbeat & 0xFFFFu));
}

void PlcJobClient::notifyTransientFaults(quint8 faultDetail) {
    // 瞬态拒绝（2/3/4/5/6 等 FaultDetail）只提示一次，同值不重复刷
    if (faultDetail == 0) {
        lastNotifiedDetail = 0;
        return;
    }
    if (faultDetail == lastNotifiedDetail)
        return;
    lastNotifiedDetail = faultDetail;
    if (last.faultCode == 0 || last.faultCode == faultDetail)
        emit rejected(PlcProto::faultText(faultDetail));
}
