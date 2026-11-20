#include "workstation_settings.h"
#include "workstation_plcpanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace {
const char* kDefaultAddr = "127.0.0.1:502";
}

PlcJobPanel::PlcJobPanel(QWidget* parent)
    : QWidget(parent) {
    buildUi();

    connect(&client, &PlcJobClient::linkChanged, this, [this](bool up, const QString& detail) {
        linked = up;
        linkLabel->setText(up ? "已连接：" + detail : "未连接" + (detail.isEmpty() ? QString() : "（" + detail + "）"));
        linkButton->setText(up ? "断开" : "连接");
        log(up ? "PLC 链路建立：" + detail : "PLC 链路关闭：" + detail);
        refreshButtons();
    });
    connect(&client, &PlcJobClient::statusUpdated, this, &PlcJobPanel::onStatus);
    connect(&client, &PlcJobClient::rejected, this, [this](const QString& message) {
        log("⚠ " + message);
        cancelAuto(message);
    });

    auto settings = workstationSettings();
    addrEdit->setText(settings.value("plc/address", kDefaultAddr).toString());
    refreshButtons();
}

void PlcJobPanel::buildUi() {
    auto* root = new QVBoxLayout(this);

    // —— 连接区 ——
    auto* linkRow = new QHBoxLayout;
    addrEdit = new QLineEdit;
    addrEdit->setPlaceholderText(kDefaultAddr);
    addrEdit->setMaximumWidth(220);
    linkButton = new QPushButton("连接");
    linkLabel = new QLabel("未连接");
    linkRow->addWidget(new QLabel("AC802 地址"));
    linkRow->addWidget(addrEdit);
    linkRow->addWidget(linkButton);
    linkRow->addWidget(linkLabel);
    linkRow->addStretch(1);
    root->addLayout(linkRow);

    connect(linkButton, &QPushButton::clicked, this, [this] {
        workstationSettings().setValue("plc/address", addrEdit->text());
        if (linked) {
            cancelAuto("链路断开");
            client.disconnectFromPlc("手动断开");
        } else {
            const QString text = addrEdit->text();
            const int colon = text.lastIndexOf(':');
            const QString host = colon > 0 ? text.left(colon) : text;
            const quint16 port = colon > 0 ? quint16(text.mid(colon + 1).toInt()) : 502;
            log(QString("连接 %1:%2 …").arg(host).arg(port));
            client.connectToPlc(host, port, 1);
        }
    });

    // —— 参数区 ——
    auto* paramForm = new QFormLayout;
    recipeBox = new QComboBox;
    recipeBox->addItem("1 · 直线单道");
    recipeBox->addItem("2 · 直线空跑");
    recipeBox->addItem("3 · 圆弧");
    jobBox = new QSpinBox;
    jobBox->setRange(1, 999999);
    jobBox->setValue(1);
    dryBox = new QCheckBox("空跑（禁止工艺输出）");
    powerBox = new QSpinBox;
    powerBox->setRange(0, 2500);
    powerBox->setSuffix(" W");
    powerBox->setSpecialValueText("配方默认");
    wireBox = new QSpinBox;
    wireBox->setRange(0, 100);
    wireBox->setSuffix(" ×0.1mm/s");
    wireBox->setSpecialValueText("配方默认");
    gasBox = new QSpinBox;
    gasBox->setRange(0, 30000);
    gasBox->setSuffix(" ms");
    gasBox->setSpecialValueText("配方默认");
    dutyBox = new QSpinBox;
    dutyBox->setRange(0, 100);
    dutyBox->setSuffix(" %");
    dutyBox->setSpecialValueText("配方默认");
    dutyBox->setToolTip("脉冲激光占空比，0–100%；0 用配方默认值。");
    freqBox = new QSpinBox;
    freqBox->setRange(0, 500);
    freqBox->setSuffix(" Hz");
    freqBox->setSpecialValueText("配方默认");
    freqBox->setToolTip("脉冲频率，0–500 Hz；0 用配方默认值。");
    pregasBox = new QSpinBox;
    pregasBox->setRange(0, 30000);
    pregasBox->setSuffix(" ms");
    pregasBox->setSpecialValueText("配方默认");
    pregasBox->setToolTip("出光前保护气预吹时长；0 用配方默认值。");
    paramForm->addRow("配方", recipeBox);
    paramForm->addRow("作业号", jobBox);
    paramForm->addRow("", dryBox);
    paramForm->addRow("激光功率", powerBox);
    paramForm->addRow("送丝速度", wireBox);
    paramForm->addRow("后送气", gasBox);
    paramForm->addRow("占空比", dutyBox);
    paramForm->addRow("脉冲频率", freqBox);
    paramForm->addRow("预送气", pregasBox);

    auto* paramGroup = new QGroupBox("作业参数");
    paramGroup->setLayout(paramForm);

    // —— 会话与流程按钮 ——
    registerButton = new QPushButton("① 登记");
    logoutButton = new QPushButton("撤销会话");
    prepareButton = new QPushButton("② 准备");
    commitButton = new QPushButton("③ 提交");
    startButton = new QPushButton("④ 开始");
    abortButton = new QPushButton("中止");
    resetButton = new QPushButton("⑤ 复位");
    autoButton = new QPushButton("一键单道（登记→复位）");

    auto* flowRow1 = new QHBoxLayout;
    flowRow1->addWidget(registerButton);
    flowRow1->addWidget(logoutButton);
    flowRow1->addStretch(1);
    auto* flowRow2 = new QHBoxLayout;
    flowRow2->addWidget(prepareButton);
    flowRow2->addWidget(commitButton);
    flowRow2->addWidget(startButton);
    flowRow2->addWidget(abortButton);
    flowRow2->addWidget(resetButton);
    flowRow2->addStretch(1);
    auto* flowRow3 = new QHBoxLayout;
    flowRow3->addWidget(autoButton);
    flowRow3->addStretch(1);

    auto* flowGroup = new QGroupBox("作业流程");
    auto* flowCol = new QVBoxLayout(flowGroup);
    flowCol->addLayout(flowRow1);
    flowCol->addLayout(flowRow2);
    flowCol->addLayout(flowRow3);

    auto* topRow = new QHBoxLayout;
    topRow->addWidget(paramGroup);
    topRow->addWidget(flowGroup, 1);
    root->addLayout(topRow);

    connect(registerButton, &QPushButton::clicked, this, [this] { client.registerSession(); log("→ 登记会话"); });
    connect(logoutButton, &QPushButton::clicked, this, [this] { client.logoutSession(); log("→ 撤销会话"); });
    connect(prepareButton, &QPushButton::clicked, this, [this] {
        client.prepareJob(quint16(recipeBox->currentIndex() + 1), quint32(jobBox->value()), dryBox->isChecked(),
                          quint16(powerBox->value()), quint16(wireBox->value()), quint16(gasBox->value()),
                          quint16(dutyBox->value()), quint16(freqBox->value()), quint16(pregasBox->value()));
        log(QString("→ 准备：配方 %1 作业 %2%3 功率 %4 丝速 %5 后送气 %6 占空比 %7% 频率 %8Hz 预送气 %9ms")
                .arg(recipeBox->currentIndex() + 1).arg(jobBox->value())
                .arg(dryBox->isChecked() ? "（空跑）" : "")
                .arg(powerBox->value()).arg(wireBox->value()).arg(gasBox->value())
                .arg(dutyBox->value()).arg(freqBox->value()).arg(pregasBox->value()));
    });
    connect(commitButton, &QPushButton::clicked, this, [this] { client.commitPrepared(); log("→ 提交（等 AcceptedSeq）"); });
    connect(startButton, &QPushButton::clicked, this, [this] { client.startJob(); log("→ 开始"); });
    connect(abortButton, &QPushButton::clicked, this, [this] { client.abortJob(); log("→ 中止"); });
    connect(resetButton, &QPushButton::clicked, this, [this] { client.faultReset(); log("→ 复位"); });
    connect(autoButton, &QPushButton::clicked, this, [this] { startAuto(); log("→ 一键单道开始"); });

    // —— 状态显示 ——
    stateLabel = new QLabel("单元状态：—");
    seqLabel = new QLabel("回执序号：Prepared — / Accepted — / Completed —");
    faultLabel = new QLabel("故障：无");
    robotLabel = new QLabel("机器人：—");
    deviceLabel = new QLabel("设备：激光 — / 送丝 — / 气 —");
    hbLabel = new QLabel("PLC 心跳：—");
    auto* statusCol = new QVBoxLayout;
    statusCol->addWidget(stateLabel);
    statusCol->addWidget(seqLabel);
    statusCol->addWidget(faultLabel);
    statusCol->addWidget(robotLabel);
    statusCol->addWidget(deviceLabel);
    statusCol->addWidget(hbLabel);
    auto* statusGroup = new QGroupBox("单元状态");
    statusGroup->setLayout(statusCol);
    root->addWidget(statusGroup);

    eventLog = new QPlainTextEdit;
    eventLog->setReadOnly(true);
    eventLog->setMaximumHeight(180);
    root->addWidget(eventLog, 1);
}

void PlcJobPanel::onStatus(const PlcProto::PlcStatus& s) {
    last = s;
    stateLabel->setText(QString("单元状态：%1%2")
                            .arg(PlcProto::cellName(s.cellState))
                            .arg(s.dryRun() ? "（空跑激活）" : ""));
    seqLabel->setText(QString("回执序号：Prepared %1 / Accepted %2 / Completed %3")
                          .arg(s.preparedSeq).arg(s.acceptedSeq).arg(s.completedSeq));
    faultLabel->setText(s.faultCode == 0
        ? QStringLiteral("故障：无")
        : QStringLiteral("故障：[%1] %2%3")
              .arg(s.faultCode)
              .arg(PlcProto::faultText(s.faultCode))
              .arg(s.faultDetail ? QString("（详情 [%1] %2）").arg(s.faultDetail).arg(PlcProto::faultText(s.faultDetail)) : QString()));
    robotLabel->setText(QString("机器人：状态 %1 · 程序 %2 · XYZ (%3, %4, %5) mm · 标志 %6")
                            .arg(s.robotState).arg(s.robotProgram)
                            .arg(s.robotX, 0, 'f', 2).arg(s.robotY, 0, 'f', 2).arg(s.robotZ, 0, 'f', 2)
                            .arg(s.robotFlags, 0, 2));
    deviceLabel->setText(QString("设备：激光 %1%2 / 送丝 %3%4 / 气 %5%6")
                             .arg(s.laserFlags & PlcProto::lfReady ? "就绪" : "—")
                             .arg(s.laserFlags & PlcProto::lfEmitting ? "·出光" : "")
                             .arg(s.wireFlags & PlcProto::wfReady ? "就绪" : "—")
                             .arg(s.wireFlags & PlcProto::wfFeeding ? "·送丝" : "")
                             .arg(s.auxFlags & PlcProto::afShieldGas ? "保护气开" : "气关")
                             .arg(s.auxFlags & PlcProto::afCoolingOk ? "·冷却正常" : "·冷却异常"));
    hbLabel->setText(QString("PLC 心跳：%1 · 会话 %2%3")
                         .arg(s.plcHeartbeat)
                         .arg(s.plcSession, 0, 16)
                         .arg(s.sessionRegistered() ? " · 已登记" : ""));

    // 自动编排推进：以回执序号为准，不靠时间猜（协议 §5）
    if (autoPhase == AutoPhase::WaitPrepared && s.cellState == PlcProto::Prepared && s.preparedSeq != 0) {
        log("✔ 暂存通过（PreparedSeq=" + QString::number(s.preparedSeq) + "），自动提交");
        client.commitPrepared();
        autoPhase = AutoPhase::WaitAccepted;
    } else if (autoPhase == AutoPhase::WaitAccepted && s.cellState == PlcProto::Accepted && s.acceptedSeq != 0) {
        log("✔ 提交生效（AcceptedSeq=" + QString::number(s.acceptedSeq) + "），自动开始");
        client.startJob();
        autoPhase = AutoPhase::WaitDone;
    } else if (autoPhase == AutoPhase::WaitDone &&
               (s.cellState == PlcProto::Done || s.cellState == PlcProto::Aborted || s.cellState == PlcProto::Fault) &&
               s.completedSeq != 0) {
        log(s.cellState == PlcProto::Done
            ? "✔ 作业完成（CompletedSeq=" + QString::number(s.completedSeq) + "），自动复位"
            : QString("✖ 作业未完成（%1，CompletedSeq=%2），复位")
                  .arg(PlcProto::cellName(s.cellState)).arg(s.completedSeq));
        client.faultReset();
        autoPhase = AutoPhase::WaitIdle;
    } else if (autoPhase == AutoPhase::WaitIdle && s.cellState == PlcProto::Idle) {
        log("✔ 一键单道结束，单元回 Idle");
        autoPhase = AutoPhase::None;
    }
    refreshButtons();
}

void PlcJobPanel::refreshButtons() {
    const bool sessionOk = linked && last.sessionRegistered();
    const bool idleStates = last.cellState == PlcProto::Idle || last.cellState == PlcProto::Done ||
                            last.cellState == PlcProto::Aborted;
    const bool faultOrIdle = idleStates || last.cellState == PlcProto::Fault;
    registerButton->setEnabled(linked && faultOrIdle && !last.sessionRegistered());
    logoutButton->setEnabled(sessionOk && !last.jobActive());
    const bool paramEditable = sessionOk && (last.cellState == PlcProto::Idle || last.cellState == PlcProto::Prepared ||
                                             last.cellState == PlcProto::Aborted);
    recipeBox->setEnabled(paramEditable);
    jobBox->setEnabled(paramEditable);
    dryBox->setEnabled(paramEditable);
    powerBox->setEnabled(paramEditable);
    wireBox->setEnabled(paramEditable);
    gasBox->setEnabled(paramEditable);
    dutyBox->setEnabled(paramEditable);
    freqBox->setEnabled(paramEditable);
    pregasBox->setEnabled(paramEditable);
    prepareButton->setEnabled(paramEditable && autoPhase == AutoPhase::None);
    commitButton->setEnabled(sessionOk && last.cellState == PlcProto::Prepared && last.preparedSeq != 0 &&
                             autoPhase == AutoPhase::None);
    startButton->setEnabled(sessionOk && last.cellState == PlcProto::Accepted && autoPhase == AutoPhase::None);
    abortButton->setEnabled(sessionOk && (last.jobActive() || last.cellState == PlcProto::Fault));
    resetButton->setEnabled(sessionOk && idleStates);
    autoButton->setEnabled(linked && faultOrIdle && autoPhase == AutoPhase::None);
}

void PlcJobPanel::startAuto() {
    if (!last.sessionRegistered()) {
        client.registerSession();
        log("① 自动登记");
    }
    client.prepareJob(quint16(recipeBox->currentIndex() + 1), quint32(jobBox->value()), dryBox->isChecked(),
                      quint16(powerBox->value()), quint16(wireBox->value()), quint16(gasBox->value()),
                      quint16(dutyBox->value()), quint16(freqBox->value()), quint16(pregasBox->value()));
    log(QString("② 自动准备：配方 %1，等待 PreparedSeq").arg(recipeBox->currentIndex() + 1));
    autoPhase = AutoPhase::WaitPrepared;
    // 整体超时守卫：60 s 内未走完即取消编排（作业本身可能仍在进行，按钮仍可手动接管）
    auto* guard = new QTimer(this);
    guard->setSingleShot(true);
    connect(guard, &QTimer::timeout, this, [this, guard] {
        if (autoPhase != AutoPhase::None) cancelAuto("自动编排 60 s 超时");
        guard->deleteLater();
    });
    guard->start(60000);
    refreshButtons();
}

void PlcJobPanel::cancelAuto(const QString& reason) {
    if (autoPhase == AutoPhase::None) return;
    autoPhase = AutoPhase::None;
    log("自动编排取消：" + reason);
    refreshButtons();
}

void PlcJobPanel::log(const QString& text) {
    eventLog->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss ") + text);
}
