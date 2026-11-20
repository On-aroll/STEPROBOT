#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include "workstation_modbus.h"

// PC↔PLC 作业协议 V1.2（docs/实施设计/20261114_V1/protocol_v1.md）上位机侧协议层。
// 职责：登记/暂存/提交/开始/中止/复位/撤销的原子命令发送、状态区解析、心跳维持、
// PlcSessionID 学习。作业编排（流程推进与按钮联动）由 Workstation UI 层组合本类信号完成。
namespace PlcProto {

constexpr int kCmdBase = 0;
constexpr int kCmdWords = 40;
constexpr int oVersion = 0, oCommand = 1, oReqSeq = 2, oJobId = 4, oRecipe = 6, oFlags = 7;
constexpr int oPower = 8, oWire = 9, oPostGas = 10, oHostSess = 11, oExpected = 13;
constexpr int oDuty = 15, oFreq = 16, oPreGas = 17;   // V1.2：占空比 % / 脉冲频率 Hz / 预送气 ms，0=配方默认
constexpr int oReserved = 18;                          // 18–35 预留（波形、工艺号选取、渐升渐降等）
constexpr int oCommit = 36, oHeartbeat = 38;

constexpr int kStatusBase = 100;
constexpr int kStatusWords = 40;
constexpr int sVersion = 0, sCellState = 1, sPrepared = 2, sAccepted = 4, sCompleted = 6;
constexpr int sFault = 8, sStatusFlags = 9, sPlcSess = 10, sPlcHb = 12, sLastCmd = 14, sFaultDetail = 15;
constexpr int sRobotProg = 16, sRobotState = 17, sRobotFlags = 18, sRobotAlarm = 19, sRobotPos = 20;
constexpr int sLaserFlags = 26, sLaserFb = 27, sWireFlags = 28, sWireFb = 29, sAuxFlags = 30;

constexpr quint16 kProtocolVersion = 2;
constexpr quint16 kFlagDryRun = 1 << 0;

enum Command : quint16 {
    CmdNone = 0, CmdRegister = 1, CmdPrepare = 2, CmdCommit = 3,
    CmdStart = 4, CmdAbort = 5, CmdReset = 6, CmdLogout = 7,
};

enum CellState : quint8 {
    Unconfigured = 0, Idle, Prepared, Accepted, Arming, Deposit, Finishing, Done, Aborted, Fault,
};
inline const char* cellName(quint8 state) {
    static const char* names[] = {"Unconfigured", "Idle", "Prepared", "Accepted",
                                  "Arming", "Deposit", "Finishing", "Done", "Aborted", "Fault"};
    return state <= Fault ? names[state] : "?";
}

enum FaultCode : quint8 {
    FaultNone = 0, FaultSession = 1, FaultSeq = 2, FaultRecipe = 3, FaultRange = 4, FaultState = 5,
    FaultStaging = 6, FaultConfig = 7, FaultRobot = 8, FaultLaser = 9, FaultWire = 10, FaultGas = 11,
    FaultHeartbeat = 12, FaultAborting = 13, FaultNotArrived = 14, FaultPermit = 15,
};
inline QString faultText(quint8 code) {
    switch (code) {
    case FaultSession:   return QStringLiteral("会话冲突/来源不符");
    case FaultSeq:       return QStringLiteral("序号重复或回退");
    case FaultRecipe:    return QStringLiteral("配方号无效");
    case FaultRange:     return QStringLiteral("参数越界");
    case FaultState:     return QStringLiteral("当前状态不允许该命令");
    case FaultStaging:   return QStringLiteral("暂存校验超时");
    case FaultConfig:    return QStringLiteral("PLC 组态/IO 缺失");
    case FaultRobot:     return QStringLiteral("机器人通讯中断或报警");
    case FaultLaser:     return QStringLiteral("激光焊机故障或非遥控模式");
    case FaultWire:      return QStringLiteral("送丝机故障");
    case FaultGas:       return QStringLiteral("气/冷却异常");
    case FaultHeartbeat: return QStringLiteral("上位机心跳失联");
    case FaultAborting:  return QStringLiteral("中止流程执行中");
    case FaultNotArrived:return QStringLiteral("机器人未到位/轨迹事件超时");
    case FaultPermit:    return QStringLiteral("工艺许可失效");
    default:             return QStringLiteral("未知故障 %1").arg(code);
    }
}

// StatusFlags 位
constexpr quint32 sfValid = 1 << 0, sfReady = 1 << 1, sfPermit = 1 << 2, sfActive = 1 << 3;
constexpr quint32 sfSession = 1 << 4, sfDryRun = 1 << 5, sfRobotLink = 1 << 6;
constexpr quint32 sfStaging = 1 << 7, sfDonePending = 1 << 8;

// RobotFlags 位
constexpr quint32 rfArmReq = 1 << 0, rfDepositReq = 1 << 1, rfPathDone = 1 << 2;
constexpr quint32 rfProcessArmed = 1 << 3, rfProcessActive = 1 << 4, rfProcessDone = 1 << 5;
constexpr quint32 rfAlarmLatch = 1 << 6;

// 激光/送丝/辅助 Flags 位（状态区 §3）
constexpr quint8 lfReady = 1 << 0, lfEmitting = 1 << 1, lfFault = 1 << 2, lfRemoteMode = 1 << 3;
constexpr quint8 wfReady = 1 << 0, wfFeeding = 1 << 1, wfFault = 1 << 2, wfRetract = 1 << 3;
constexpr quint8 afShieldGas = 1 << 0, afCoolingOk = 1 << 1;

inline quint32 u32At(const QVector<quint16>& regs, int offset) {
    return (quint32(regs.value(offset)) << 16) | quint32(regs.value(offset + 1));
}

struct PlcStatus {
    bool valid = false;
    quint8 cellState = Unconfigured;
    quint32 preparedSeq = 0;
    quint32 acceptedSeq = 0;
    quint32 completedSeq = 0;
    quint8 faultCode = 0;
    quint8 faultDetail = 0;
    quint32 statusFlags = 0;
    quint32 plcSession = 0;
    quint32 plcHeartbeat = 0;
    quint8 lastCommand = 0;
    quint16 robotProgram = 0;
    quint8 robotState = 0;      // 0 未知 1 停止 2 运行 3 暂停 4 故障
    quint32 robotFlags = 0;
    quint16 robotAlarm = 0;
    double robotX = 0.0, robotY = 0.0, robotZ = 0.0;   // mm（定点 mm×100）
    quint8 laserFlags = 0, wireFlags = 0, auxFlags = 0;
    quint16 laserPowerFb = 0;
    quint16 wireSpeedFb = 0;

    bool sessionRegistered() const { return statusFlags & sfSession; }
    bool jobActive() const { return statusFlags & sfActive; }
    bool dryRun() const { return statusFlags & sfDryRun; }
    bool robotOnline() const { return statusFlags & sfRobotLink; }
};

}  // namespace PlcProto

Q_DECLARE_METATYPE(PlcProto::PlcStatus)

class PlcJobClient : public QObject {
    Q_OBJECT
public:
    explicit PlcJobClient(QObject* parent = nullptr);

    void connectToPlc(const QString& host, quint16 port, quint8 unitId = 1);
    void disconnectFromPlc(const QString& reason = {});
    bool isLinked() const { return modbus.isConnected(); }
    PlcProto::PlcStatus status() const { return last; }

    // 协议原子操作（异步发出；回执通过 statusUpdated 观察，瞬态拒绝经 rejected 提示）
    void registerSession();
    void prepareJob(quint16 recipeId, quint32 jobId, bool dryRun,
                    quint16 powerW = 0, quint16 wireSpeed = 0, quint16 postGasMs = 0,
                    quint16 dutyPct = 0, quint16 pulseFreqHz = 0, quint16 preGasMs = 0);
    void commitPrepared();   // FC16 整块写 36–39：CommitSeq + 心跳（协议 V1.2 推荐方式）
    void startJob();
    void abortJob();
    void faultReset();
    void logoutSession();

signals:
    void linkChanged(bool up, const QString& detail);
    void statusUpdated(const PlcProto::PlcStatus& status);
    void rejected(const QString& message);

private:
    void onRegisters(int functionCode, int startAddress, const QVector<quint16>& values, qint64);
    void sendCommandBlock(quint16 command, quint16 recipe, quint16 flags,
                          quint16 power, quint16 wire, quint16 postGas, quint32 jobId,
                          quint16 duty = 0, quint16 freq = 0, quint16 preGas = 0);
    void notifyTransientFaults(quint8 faultDetail);

    ModbusClient modbus;
    PlcProto::PlcStatus last;
    quint32 hostSession = 0;
    quint32 expectedPlcSession = 0;
    quint32 reqSeq = 0;
    quint32 heartbeat = 0;
    quint32 pendingCommitSeq = 0;   // 最近状态区见到的 PreparedSeq
    quint8 lastNotifiedDetail = 0;
};
