#pragma once
#include "workstation_modbus.h"
#include <array>
#include <optional>

// AC802 equipment bridge V1 (4 one-hot slots) / V2 (GDI8 1..255).
namespace BridgeProto {
constexpr int CommandBase = 200, CommandWords = 16, StatusBase = 300, StatusWords = 64;
constexpr quint16 Magic = 0x4D42, LegacyVersion = 1, Version = 2, MaxPrograms = 255;
enum Command : quint16 {
    Register = 1, Enable, Disable, Load, Run, Pause, Stop, ClearError,
    Logout, Coordinates, Rearm, ConfirmLoad
};
enum Result : quint16 { None = 0, Busy, Confirmed, SentUnconfirmed, Rejected, Timeout, Interrupted };
enum Flag : quint16 {
    ScanValid = 1, RobotOnline = 2, RemotePermit = 4, Registered = 8, HostAlive = 16,
    ActionBusy = 32, Commissioned = 64, CoordinatesRequested = 128, StopLatch = 256, LoadConfirmed = 512, GdiMappingConfirmed = 1024
};
struct Status {
    bool valid = false;
    quint16 version = 0, maxPrograms = 0, selectionMode = 0, desiredProgramWord = 0;
    quint16 gdiFirst = 0, gdiLast = 0, robotWriteWords = 0;
    quint16 flags = 0, stage = 0, ackCommand = 0, result = 0, error = 0;
    quint32 ackSeq = 0, boot = 0, owner = 0, rejectedSeq = 0, interruptedSeq = 0;
    quint16 hostHeartbeat = 0, plcHeartbeat = 0, selectedSlot = 0, desiredWord = 0;
    quint16 rawStatus = 0, robotState = 0, rejectReason = 0, robotHeartbeatEdges = 0;
    std::array<quint16, 32> robotWords{};
    std::array<quint16, StatusWords> rawWords{};
    bool has(Flag flag) const { return (flags & flag) != 0; }
    bool running() const { return (rawStatus & 0x0001) != 0; }
    bool alarm() const { return (rawStatus & 0x0002) != 0; }
    bool enabled() const { return (rawStatus & 0x0004) != 0; }
    bool paused() const { return (rawStatus & 0x0008) != 0; }
    bool stopped() const { return (rawStatus & 0x0010) && !running() && !paused(); }
    bool externalAuto() const { return (rawStatus & 0x0080) != 0; }
    bool emergency() const { return (rawStatus & 0x0100) != 0; }
    std::optional<float> coordinate(int axis) const;
    std::optional<bool> digitalOutput(int number) const;
    std::optional<bool> desiredInput(int number) const;
    std::optional<quint16> analogRaw(int number) const;
};
Status decode(const QVector<quint16>& words);
QString commandText(Command command);
QString resultText(quint16 result);
QString errorText(quint16 error);
QString robotStateText(const Status& status);
QString stageText(quint16 stage);
QString inputMeaning(int number, quint16 version = LegacyVersion);
QString outputMeaning(int number);
QString statusFieldName(int index);
}

class PlcBridgeClient : public QObject {
    Q_OBJECT
public:
    explicit PlcBridgeClient(QObject* parent = nullptr);
    void connectTo(const QString& host, quint16 port = 502, quint8 unit = 1);
    void disconnectFrom(const QString& reason = {});
    bool isConnected() const { return transport.isConnected(); }
    bool isActive() const { return transport.isActive(); }
    QString endpointHost() const { return endpointHost_; }
    quint16 endpointPort() const { return endpointPort_; }
    quint8 endpointUnit() const { return endpointUnit_; }
    bool fresh() const;
    bool ownsSession() const;
    bool pending() const { return pendingSeq != 0; }
    bool operationInhibited() const { return operationInhibited_; }
    void setOperationInhibited(bool value) { operationInhibited_=value; emit changed(); }
    quint32 sessionId() const { return session; }
    quint32 pendingSequence() const { return pendingSeq; }
    BridgeProto::Command currentCommand() const { return pendingCommand; }
    quint16 currentParameter() const { return pendingParameter; }
    qint64 commandAgeMs() const { return pending() && commandStarted.isValid() ? commandStarted.elapsed() : -1; }
    qint64 scanAgeMs() const { return scanChangedAt.isValid() ? scanChangedAt.elapsed() : -1; }
    qint64 sentFrames() const { return transport.sentFrames(); }
    qint64 receivedFrames() const { return transport.receivedFrames(); }
    qint64 errorCount() const { return transport.errorCount(); }
    qint64 lastRoundTripMs() const { return transport.lastRoundTripMs(); }
    quint16 sentHeartbeat() const { return heartbeat; }
    BridgeProto::Status status() const { return last; }
    bool canIssue(BridgeProto::Command command, quint16 parameter = 0, QString* reason = nullptr) const;
    bool issue(BridgeProto::Command command, quint16 parameter = 0);
signals:
    void linkChanged(bool connected, const QString& detail);
    void changed();
    void notice(const QString& message);
    void rejected(const QString& message);
    void commandFinished(quint32 sequence, quint16 command, quint16 result, quint16 error);
private:
    void onRegisters(int fc, int start, const QVector<quint16>& words, qint64);
    void tick();
    void resetSession();
    void abandon(const QString& message);
    ModbusClient transport;
    QTimer heartbeatTimer, healthTimer;
    QElapsedTimer scanChangedAt, commandStarted;
    BridgeProto::Status last;
    quint32 session = 0, sequence = 0, pendingSeq = 0;
    BridgeProto::Command pendingCommand = BridgeProto::Register;
    quint16 pendingParameter = 0, heartbeat = 0, loadSentSlot = 0;
    quint32 rejectBaseline = 0;
    quint16 rejectReasonBaseline = 0;
    bool scanObserved = false, scanAdvanced = false, registeredHere = false, writeAccepted = false;
    bool ending = false;
    bool operationInhibited_ = false;
    QString endpointHost_;
    quint16 endpointPort_ = 502;
    quint8 endpointUnit_ = 1;
};
