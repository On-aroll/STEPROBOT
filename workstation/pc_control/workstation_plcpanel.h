#pragma once

#include <QWidget>

#include "workstation_plcjob.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

// PLC 作业面板：登记→准备→提交→开始→（中止/复位）的手动按钮编排，
// 外加「一键单道」自动流（观察三个回执序号逐步推进，不靠时间猜）。
// 全部机器人/工艺控制经 PlcJobClient 发给 AC802，上位机不直控设备。
class PlcJobPanel : public QWidget {
    Q_OBJECT
public:
    explicit PlcJobPanel(QWidget* parent = nullptr);

private:
    void buildUi();
    void onStatus(const PlcProto::PlcStatus& s);
    void refreshButtons();
    void log(const QString& text);
    void startAuto();
    void stepAuto();
    void cancelAuto(const QString& reason);

    // 连接区
    QLineEdit* addrEdit = nullptr;
    QPushButton* linkButton = nullptr;
    QLabel* linkLabel = nullptr;
    // 会话区
    QPushButton* registerButton = nullptr;
    QPushButton* logoutButton = nullptr;
    // 参数区
    QComboBox* recipeBox = nullptr;
    QSpinBox* jobBox = nullptr;
    QCheckBox* dryBox = nullptr;
    QSpinBox* powerBox = nullptr;
    QSpinBox* wireBox = nullptr;
    QSpinBox* gasBox = nullptr;
    QSpinBox* dutyBox = nullptr;     // V1.2：占空比 %，0=配方默认
    QSpinBox* freqBox = nullptr;     // V1.2：脉冲频率 Hz，0=配方默认
    QSpinBox* pregasBox = nullptr;   // V1.2：预送气 ms，0=配方默认
    // 流程按钮
    QPushButton* prepareButton = nullptr;
    QPushButton* commitButton = nullptr;
    QPushButton* startButton = nullptr;
    QPushButton* abortButton = nullptr;
    QPushButton* resetButton = nullptr;
    QPushButton* autoButton = nullptr;
    // 状态显示
    QLabel* stateLabel = nullptr;
    QLabel* seqLabel = nullptr;
    QLabel* faultLabel = nullptr;
    QLabel* robotLabel = nullptr;
    QLabel* deviceLabel = nullptr;
    QLabel* hbLabel = nullptr;
    QPlainTextEdit* eventLog = nullptr;

    PlcJobClient client;
    PlcProto::PlcStatus last;
    bool linked = false;

    // 自动编排阶段：发准备后等 PreparedSeq、提交后等 AcceptedSeq、开始后等 CompletedSeq
    enum class AutoPhase { None, WaitPrepared, WaitAccepted, WaitDone, WaitIdle };
    AutoPhase autoPhase = AutoPhase::None;
};
