#pragma once
#include "workstation_plcbridge.h"
#include <QWidget>
#include <QJsonObject>
#include <array>

namespace ManualProto {
constexpr int CommandBase = 220, StatusBase = 400, CommandWords = 16, StatusWords = 32;
constexpr int PositionBase = 450, PositionWords = 40;
constexpr quint16 Magic = 0x4D35, Version = 2;
// Scaled degrees per held segment. Single-step displacement is independent.
constexpr int jointJogLimit(int speed) { return speed<=500?100:speed<=1000?250:speed<=1500?500:1000; }
constexpr int worldJogLimit(int speed) { return speed<=50?100:speed<=250?500:speed<=1000?1000:speed<=5000?5000:10000; }
enum Op : quint16 { Enter = 1, Step = 2, Capture = 3, Return = 4, Exit = 5, Cancel = 6,
    Stage = 7, Move = 8, Jog = 9 };
enum Flag : quint16 { Installed = 1, Binding = 2, Entered = 4, Busy = 8, ServiceAlive = 16,
    HostValid = 32, HomeSaved = 64, AtHome = 128, Stopping = 256 };
struct Status {
    bool valid = false;
    std::array<quint16, StatusWords> raw{};
    quint32 ack = 0, boot = 0, owner = 0;
    quint16 flags = 0, phase = 0, op = 0, result = 0, error = 0, scan = 0, slot = 0, origin = 0;
    bool has(Flag f) const { return (flags & f) != 0; }
};
Status decode(const QVector<quint16>& words);
struct Position {
    bool valid = false;
    quint16 flags = 0, frame = 0, age = 0;
    quint32 boot = 0, counter = 0;
    std::array<double,12> values{}; // J1..J6 degrees, WORLD/Default XYZ mm + ABC degrees.
};
Position decodePosition(const QVector<quint16>& words);
QString errorText(quint16 error);
QVector<quint16> frame(Op op, quint32 request, quint32 owner, quint32 boot, quint16 slot,
    quint16 action, qint16 argument, quint16 speed, quint16 heartbeat);
}

// A second connection to the SAME PLC. It borrows the explicitly registered main-session identity.
// It never opens a robot/SDK connection and never registers or reconnects automatically.
class ManualBridgeClient : public QObject {
    Q_OBJECT
public:
    explicit ManualBridgeClient(PlcBridgeClient& bridge, QObject* parent = nullptr);
    bool fresh() const;
    bool positionFresh() const;
    ManualProto::Position position() const { return position_; }
    bool pending() const { return pending_ != 0; }
    bool uncertain() const { return uncertain_; }
    quint32 pendingSequence() const { return pending_; }
    ManualProto::Status status() const { return status_; }
    bool ready(QString* why = nullptr, bool requireEntered = true) const;
    bool issue(ManualProto::Op op, quint16 action = 0, qint16 argument = 0, quint16 speed = 100);
    bool moveTarget(bool cartesian, const std::array<std::optional<double>,6>& values, quint16 speed);
    bool targetActive() const { return targetFlow_; }
    int targetProgress() const { return targetIndex_; }
    void cancel(bool leave = false);
    void setModeEnabled(bool enabled);
    void reconnect();
signals:
    void changed();
    void notice(const QString& message);
    void finished(quint32 sequence, quint16 op, quint16 result, quint16 error);
private:
    void tick();
    void receive(int function, int start, const QVector<quint16>& words, qint64);
    void reset();
    void advanceTarget();
    PlcBridgeClient& bridge;
    ModbusClient wire;
    QTimer timer;
    QElapsedTimer scanAge, commandAge, heartbeatAge;
    QElapsedTimer positionAge;
    ManualProto::Status status_;
    ManualProto::Position position_;
    quint32 sequence_ = 0, pending_ = 0;
    ManualProto::Op op_ = ManualProto::Enter;
    quint16 heartbeat_ = 0;
    bool advanced_ = false, mode_ = false, cancelling_ = false, notifying_ = false, uncertain_ = false;
    struct TargetField { quint16 action; qint16 whole; quint16 fraction; };
    QList<TargetField> targetFields_;
    bool targetFlow_ = false, advancingTarget_ = false;
    int targetIndex_ = 0;
    quint16 targetId_ = 0, targetSpeed_ = 0;
};

class QLabel;
class QPushButton;
class QComboBox;
class QDoubleSpinBox;
class QCheckBox;
class QPlainTextEdit;
class QLineEdit;
class QGridLayout;
class QBoxLayout;
class QResizeEvent;
class ManualBridgePanel : public QWidget {
    Q_OBJECT
public:
    explicit ManualBridgePanel(PlcBridgeClient& bridge, QWidget* parent = nullptr);
    void setModeEnabled(bool enabled);
    void halt(bool leave = false);
    bool engaged() const;
    bool safeToClose() const;
    ManualBridgeClient& client() { return manual; }
    QWidget* poseHost() const { return poseHost_; }
    QWidget* monitorHost() const { return monitorHost_; }
signals:
    void notice(const QString& message);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
private:
    void refresh();
    void nextStep();
    void capture();
    void returnHome();
    void saveOrigin();
    void moveTarget();
    void fillCurrentTarget();
    PlcBridgeClient& bridge;
    ManualBridgeClient manual;
    QComboBox *frame, *multiplier, *speedPreset;
    QDoubleSpinBox *distance, *speed, *returnSpeed;
    QComboBox* targetFrame;
    QDoubleSpinBox* targetSpeed;
    QList<QDoubleSpinBox*> targetValues;
    QList<QCheckBox*> targetSelected;
    QList<QLabel*> targetLabels;
    QList<QLabel*> targetCurrent, jointCurrent, worldCurrent;
    QPushButton* targetMove;
    QPushButton* targetFillCurrent;
    QLabel* targetHint;
    QLabel *positionState, *speedHint;
    QLineEdit* experiment;
    QLabel *health, *receipt, *origin, *units, *coordinates;
    QLabel *channelBadge, *serviceBadge, *originBadge, *setupHint;
    QPlainTextEdit* log;
    QList<QPushButton*> directions;
    QPushButton *enter, *stop, *exit, *captureButton, *returnButton, *single;
    QPushButton *reconnect, *load, *confirm, *enable;
    QPushButton *acknowledge, *rearm, *registerSession, *logoutSession;
    QLabel* sessionState;
    QLabel *recoveryStatus;
    QGridLayout* setupGrid = nullptr;
    QGridLayout* manualBody = nullptr;
    QWidget *poseHost_=nullptr,*monitorHost_=nullptr,*homeBox_=nullptr;
    bool narrowLayout = false;
    QTimer repeat;
    bool mode_ = false, held_ = false, returning_ = false, submitting_ = false;
    int axis_ = 0, direction_ = 1;
    quint16 newOrigin_ = 0;
    QJsonObject originRecord;
};
