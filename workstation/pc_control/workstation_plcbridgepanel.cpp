#include "workstation_plcbridgepanel.h"
#include "workstation_ui.h"
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QGridLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace {
const BridgeProto::Command Commands[] = {BridgeProto::Register, BridgeProto::Logout, BridgeProto::Enable, BridgeProto::Disable,
    BridgeProto::Load, BridgeProto::ConfirmLoad, BridgeProto::Run, BridgeProto::Pause, BridgeProto::Stop,
    BridgeProto::ClearError, BridgeProto::Rearm, BridgeProto::Coordinates};
QString hexWord(quint16 word) { return QString("0x%1").arg(word, 4, 16, QLatin1Char('0')).toUpper(); }
QString yesNo(bool value) { return value ? "是" : "否"; }
void cell(QTableWidget* table, int row, int column, const QString& text) {
    auto* item = table->item(row, column);
    if (item->text() != text) { item->setText(text); item->setToolTip(text); }
}
QTableWidget* table(int rows, const QStringList& headers, const QString& name) {
    auto* result = new QTableWidget(rows, headers.size()); result->setObjectName(name);
    result->setHorizontalHeaderLabels(headers); result->setEditTriggers(QAbstractItemView::NoEditTriggers);
    result->setSelectionBehavior(QAbstractItemView::SelectRows); result->verticalHeader()->setVisible(false);
    result->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    result->horizontalHeader()->setStretchLastSection(true); result->verticalHeader()->setDefaultSectionSize(29);
    for (int r = 0; r < rows; ++r) for (int c = 0; c < headers.size(); ++c) result->setItem(r, c, new QTableWidgetItem);
    result->setMinimumHeight(120); return result;
}
QLabel* label(QVBoxLayout* layout, const QString& text = {}) {
    auto* result = new QLabel(text); result->setWordWrap(true);
    result->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(result); return result;
}
QString parameterText(BridgeProto::Command c, int slot, int selection) {
    return c == BridgeProto::Load || c == BridgeProto::ConfirmLoad ? QString("程序号%1").arg(slot) :
        c == BridgeProto::Coordinates ? (selection ? "1：XYZABC请求" : "0：AO21～32请求") : "0";
}
}
PlcBridgePanel::PlcBridgePanel(PlcBridgeClient& value, QWidget* parent) : QWidget(parent), client(value) {
    using namespace BridgeProto;
    setObjectName("plcBridgePanel");
    auto* layout=WorkstationUi::page(this,"plcControlScroll");
    pageLayout_=layout;
    layout->parentWidget()->setMaximumWidth(1780);
    summary_=new QWidget;summary_->setObjectName("plcSummary");auto* badges=new QHBoxLayout(summary_);badges->setContentsMargins(0,0,0,0);auto* heading=new QLabel("自动运行");heading_=heading;heading->setProperty("heading",true);badges->addWidget(heading);badges->addSpacing(16);
    connectionBadge=WorkstationUi::badge("plcConnectionBadge"); sessionBadge=WorkstationUi::badge("plcSessionBadge");
    robotBadge=WorkstationUi::badge("plcRobotBadge"); enableBadge=WorkstationUi::badge("plcEnableBadge"); loadBadge=WorkstationUi::badge("plcLoadBadge");
    for(auto* badge:{connectionBadge,sessionBadge,robotBadge,enableBadge,loadBadge})badges->addWidget(badge,1);
    layout->addWidget(summary_);
    nextAction=label(layout); nextAction->setObjectName("plcNextAction"); nextAction->setProperty("banner",true);
    slot = new QSpinBox; slot->setRange(1, MaxPrograms); slot->setObjectName("plcBridgeSlot");
    coordinates = new QComboBox; coordinates->setObjectName("plcBridgeCoordinates");
    coordinates->addItems({"0：请求 AO21～32", "1：请求 XYZABC 坐标"});
    for (int i = 0; i < 12; ++i) {
        const auto command = Commands[i]; auto* button = new QPushButton(commandText(command)); actions.insert(command, button);
        button->setObjectName(QString("plcBridgeCommand%1").arg(command));
        button->setMinimumHeight(38);
        connect(button, &QPushButton::clicked, this, [this, command] { send(command); });
    }
    actions[Stop]->setProperty("danger",true);
    workspace=new QGridLayout;workspace->setSpacing(12);layout->addLayout(workspace);
    auto* controls=new QWidget;controls_=controls;controls->setObjectName("plcPrimaryControls");auto* controlLayout=new QVBoxLayout(controls);controlLayout->setContentsMargins(0,0,0,0);controlLayout->setSpacing(10);workspace->addWidget(controls,0,0,Qt::AlignTop);
    poseHost_=new QWidget;poseHost_->setObjectName("controlPoseHost");auto* poseLayout=new QVBoxLayout(poseHost_);poseLayout->setContentsMargins(0,12,0,0);workspace->addWidget(poseHost_,0,1,Qt::AlignTop);
    monitorHost_=new QWidget;monitorHost_->setObjectName("controlMonitorHost");auto* monitorLayout=new QVBoxLayout(monitorHost_);monitorLayout->setContentsMargins(0,0,0,0);workspace->addWidget(monitorHost_,0,2,Qt::AlignTop);
    workspace->setColumnStretch(0,1);workspace->setColumnStretch(1,2);workspace->setColumnStretch(2,1);
    auto* flow=new QGridLayout;flow_=flow;flow->setSpacing(10);int cardIndex=0;
    auto card=[this,flow,&cardIndex](const QString& title){auto* box=new QGroupBox(title);flowCards_<<box;auto* col=new QVBoxLayout(box);col->setSpacing(6);flow->addWidget(box,cardIndex++,0);return col;};
    auto* registration=card("1  登记控制权");
    registration->addWidget(WorkstationUi::hint("连接后登记本机控制权。"));
    auto* registrationRow=new QHBoxLayout;registrationRow->addWidget(actions[Register]);registrationRow->addWidget(actions[Logout]);registration->addLayout(registrationRow);
    registerHint=WorkstationUi::hint();registerHint->setObjectName("plcRegisterHint");registration->addWidget(registerHint);registration->addStretch();
    auto* loading=card("2  选择并确认程序");
    slot->setMaximumWidth(160);
    auto* selectionRow=new QHBoxLayout;selectionRow->addWidget(new QLabel("程序号"));selectionRow->addWidget(slot,1);selectionRow->addStretch();loading->addLayout(selectionRow);
    loading->addWidget(actions[Load]); loading->addWidget(actions[ConfirmLoad]);
    loadHint=WorkstationUi::hint();loadHint->setObjectName("plcLoadHint");loading->addWidget(loadHint);loading->addStretch();
    auto* running=card("3  使能与运行");
    auto* servoRow=new QHBoxLayout;servoRow->addWidget(actions[Enable]);servoRow->addWidget(actions[Disable]);running->addLayout(servoRow);
    auto* motionRow=new QHBoxLayout;motionRow->addWidget(actions[Run]);motionRow->addWidget(actions[Pause]);running->addLayout(motionRow);
    running->addWidget(actions[Stop]);runHint=WorkstationUi::hint();runHint->setObjectName("plcRunHint");running->addWidget(runHint);running->addStretch();
    controlLayout->addLayout(flow);
    auto* recovery=new QHBoxLayout;recovery->addWidget(actions[ClearError]);recovery->addWidget(actions[Rearm]);
    controlLayout->addLayout(recovery);
    receipt=label(controlLayout);receipt->setObjectName("plcBridgeReceipt");receipt->setProperty("hint",true);
    controlLayout->addWidget(WorkstationUi::hint("程序号须在示教器预先绑定。人工确认后单独点击运行；切换程序时请保留使能。"));
    auto* advanced=WorkstationUi::disclosure(layout,"I/O、坐标与接口诊断","plcAdvanced");auto* advancedLayout=new QVBoxLayout(advanced);advancedLayout->setContentsMargins(0,0,0,0);
    pages = new QTabWidget; pages->setObjectName("plcBridgeDetailTabs");pages->setMinimumHeight(310); advancedLayout->addWidget(pages, 1);
    auto* ioPage = new QWidget; auto* ioLayout = new QVBoxLayout(ioPage);
    label(ioLayout, "DI33～64显示PLC期望输出（V1仅DI33～48），不能证明机器人已收到；DO33～96来自机器人32字镜像。这里只监视，不单独强制I/O。");
    auto* ioFilter = new QComboBox; ioFilter->setObjectName("plcBridgeIoFilter");
    ioFilter->addItems({"机器人 DO33～96 回读", "PLC期望 DI33～64", "全部 I/O"}); ioLayout->addWidget(ioFilter);
    ioTable = table(96, {"信号", "PLC报文偏移/位", "含义", "当前值"}, "plcBridgeIoTable");
    connect(ioFilter, &QComboBox::currentIndexChanged, this, [this](int index) {
        for (int r = 0; r < 96; ++r) ioTable->setRowHidden(r, index == 0 ? r < 32 : index == 1 ? r >= 32 : false);
    });
    for (int r = 0; r < 32; ++r) ioTable->setRowHidden(r, true);
    ioLayout->addWidget(ioTable); pages->addTab(ioPage, "I/O 信号");
    auto* aoPage = new QWidget; auto* aoLayout = new QVBoxLayout(aoPage);
    auto* coordinateRow=new QHBoxLayout;coordinateRow->addWidget(new QLabel("监视输出"));coordinateRow->addWidget(coordinates,1);coordinateRow->addWidget(actions[Coordinates]);aoLayout->addLayout(coordinateRow);
    position = label(aoLayout); position->setObjectName("plcBridgeCoordinatesReadout");
    label(aoLayout, "AO显示原始WORD，无单位和比例换算。AO21～32与XYZABC复用，REAL低字在前；输出模式没有独立反馈，坐标仅供监视。");
    analog = table(28, {"信号", "PLC/机器人偏移", "HEX", "WORD十进制", "当前解释"}, "plcBridgeAnalogTable");
    aoLayout->addWidget(analog); pages->addTab(aoPage, "模拟量/坐标");
    auto* diagPage = new QWidget; auto* diagLayout = new QVBoxLayout(diagPage);
    connection = label(diagLayout); robot = label(diagLayout); permissions = label(diagLayout);
    connection->setObjectName("plcBridgeConnection");
    selectionInfo = label(diagLayout); selectionInfo->setObjectName("plcBridgeProgramCapability");
    label(diagLayout,"FC03 读 HR300～363；FC16 写 HR200～215；心跳 FC06 写 HR215；地址从0开始。");
    traffic = label(diagLayout); traffic->setObjectName("plcBridgeTraffic"); flags = label(diagLayout);
    pendingLabel = label(diagLayout); pendingLabel->setObjectName("plcBridgePending");
    diagnostics = table(64, {"HR偏移", "4xxxx参考号", "字段", "HEX", "十进制"}, "plcBridgeDiagnosticTable");
    diagLayout->addWidget(diagnostics);
    auto* save = new QPushButton("导出诊断快照（只读JSON）"); save->setObjectName("plcBridgeExport");
    diagLayout->addWidget(save); connect(save, &QPushButton::clicked, this, &PlcBridgePanel::exportDiagnostics);
    pages->addTab(diagPage, "接口诊断");
    auto* coveragePage = new QWidget; auto* coverageLayout = new QVBoxLayout(coveragePage);
    label(coverageLayout, "12条PLC命令均在上方提供按钮。下表实时显示参数和不可用原因；PLC本地许可及未开放功能另列说明。");
    coverage = table(17, {"命令", "功能", "参数", "可用状态/原因"}, "plcBridgeCoverageTable");
    coverage->setWordWrap(true); coverage->verticalHeader()->setDefaultSectionSize(48);
    coverageLayout->addWidget(coverage); pages->addTab(coveragePage, "功能与操作条件");
    events = new QPlainTextEdit; events->setObjectName("plcBridgeEvents"); events->setReadOnly(true); events->setMaximumBlockCount(500);events->setPlaceholderText("这里保留本次 PLC 设备操作的完整记录。右侧操作记录同时显示微调与工程操作。");
    pages->addTab(events, "事件记录");
    connect(&client, &PlcBridgeClient::changed, this, &PlcBridgePanel::refresh);
    const auto log = [this](const QString& text) { events->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss ") + text); };
    connect(&client, &PlcBridgeClient::notice, this, log); connect(&client, &PlcBridgeClient::rejected, this, log);
    connect(slot, &QSpinBox::valueChanged, this, [this](int value) { emit programSlotChanged(value); refresh(); });
    connect(coordinates, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    refresh();
    layout->addStretch(1);
}
void PlcBridgePanel::resizeEvent(QResizeEvent* event){
    QWidget::resizeEvent(event);if(!workspace)return;
    const bool compact=width()<1380;workspace->removeWidget(monitorHost_);
    workspace->addWidget(monitorHost_,compact?1:0,compact?0:2,1,compact?2:1,Qt::AlignTop);
    workspace->setColumnStretch(0,1);workspace->setColumnStretch(1,compact?1:2);
    workspace->setColumnStretch(2,compact||monitorHost_->isHidden()?0:1);
    if(!printWorkspace_)layoutFlow(compact);
}
void PlcBridgePanel::layoutFlow(bool compact){
    for(int i=0;i<flowCards_.size();++i){flow_->removeWidget(flowCards_[i]);flow_->addWidget(flowCards_[i],compact?i/2:i,compact?i%2:0,1,compact&&i==2?2:1);}
}
void PlcBridgePanel::mountPrintControls(){
    using namespace BridgeProto;
    auto findLayout=[](auto&& self,QLayout* layout,QWidget* widget)->QBoxLayout*{
        if(layout->indexOf(widget)>=0)return qobject_cast<QBoxLayout*>(layout);
        for(int i=0;i<layout->count();++i)if(auto* child=layout->itemAt(i)->layout())if(auto* found=self(self,child,widget))return found;
        for(int i=0;i<layout->count();++i)if(auto* child=layout->itemAt(i)->widget())if(child->layout())if(auto* found=self(self,child->layout(),widget))return found;
        return nullptr;
    };
    QList<QWidget*> moving{slot,receipt};for(auto it=actions.cbegin();it!=actions.cend();++it)if(it.key()!=Coordinates)moving<<it.value();
    for(auto* widget:moving){auto* layout=findLayout(findLayout,controls_->layout(),widget);Q_ASSERT(layout);printReturns_.append({widget,layout,layout->indexOf(widget)});}
    for(auto* card:flowCards_){printHidden_<<card;card->hide();}
    for(int i=0;i<controls_->layout()->count();++i)if(auto* widget=controls_->layout()->itemAt(i)->widget())if(widget!=receipt&&widget!=printRun_&&!widget->isHidden()){printHidden_<<widget;widget->hide();}
    if(!printPreparation_){
        printPreparation_=new QWidget;printPreparation_->setObjectName("printPreparationBar");auto* row=new QHBoxLayout(printPreparation_);row->setContentsMargins(0,0,0,0);row->setSpacing(6);
        printRun_=new QGroupBox("机器人运行");printRun_->setObjectName("printRunControls");auto* run=new QVBoxLayout(printRun_);run->setContentsMargins(8,6,8,6);run->setSpacing(3);
    }
    auto* row=qobject_cast<QHBoxLayout*>(printPreparation_->layout());
    while(row->count())delete row->takeAt(0);
    row->addWidget(actions[Register]);row->addWidget(actions[Logout]);row->addSpacing(8);row->addWidget(slot);row->addWidget(actions[Load]);row->addWidget(actions[ConfirmLoad]);row->addStretch();row->addWidget(actions[ClearError]);row->addWidget(actions[Rearm]);
    auto* run=qobject_cast<QVBoxLayout*>(printRun_->layout());
    for(auto command:{Enable,Disable,Run,Pause,Stop})run->addWidget(actions[command]);
    run->addWidget(receipt);
    slot->setPrefix("程序号 ");slot->setMinimumWidth(110);
    qobject_cast<QVBoxLayout*>(controls_->layout())->insertWidget(0,printRun_);
    const QString style="QGroupBox {padding:7px 6px 6px;margin-top:10px;} QPushButton {font-size:10pt;padding:4px 7px;min-height:19px;} QPushButton:focus {padding:3px 6px;} QSpinBox {font-size:10pt;padding:3px 24px 3px 5px;} QLabel {font-size:9pt;}";
    printPreparation_->setStyleSheet(style);printRun_->setStyleSheet(style);
    for(auto it=actions.cbegin();it!=actions.cend();++it)if(it.key()!=Coordinates){it.value()->setMinimumHeight(28);it.value()->show();}
    receipt->show();slot->show();printRun_->show();printPreparation_->show();
}
void PlcBridgePanel::restorePrintControls(){
    controls_->layout()->removeWidget(printRun_);
    std::stable_sort(printReturns_.begin(),printReturns_.end(),[](const PrintReturn& a,const PrintReturn& b){return a.index<b.index;});
    for(const auto& item:printReturns_){item.layout->insertWidget(item.index,item.widget);item.widget->show();}
    printReturns_.clear();for(auto* widget:printHidden_)widget->show();printHidden_.clear();
    slot->setPrefix("");slot->setMinimumWidth(0);for(auto* action:actions)action->setMinimumHeight(38);
    printPreparation_->hide();printRun_->hide();refresh();
}
void PlcBridgePanel::setPrintHosts(QVBoxLayout* summary,QVBoxLayout* controls,QVBoxLayout* preparation){
    const bool active=summary&&controls&&preparation;
    if(active){if(!printWorkspace_)mountPrintControls();summary->addWidget(summary_);summary->addWidget(nextAction);preparation->addWidget(printPreparation_);controls->addWidget(controls_);}
    else if(printWorkspace_){restorePrintControls();pageLayout_->insertWidget(0,summary_);pageLayout_->insertWidget(1,nextAction);workspace->addWidget(controls_,0,0,Qt::AlignTop);}
    printWorkspace_=active;heading_->setText(active?"联动打印":"自动运行");if(!active)layoutFlow(width()<1380);
    for(auto* widget:{summary_,static_cast<QWidget*>(nextAction),controls_})widget->show();
}
void PlcBridgePanel::setPrintRowHeight(int height){
    if(!printWorkspace_)return;
    for(auto command:{BridgeProto::Enable,BridgeProto::Disable,BridgeProto::Run,BridgeProto::Pause,BridgeProto::Stop})actions[command]->setMinimumHeight(height);
}
void PlcBridgePanel::setMonitorVisible(bool visible){monitorHost_->setVisible(visible);workspace->setColumnStretch(2,visible&&width()>=1380?1:0);}
void PlcBridgePanel::setModeEnabled(bool enabled) { modeEnabled = enabled; refresh(); }
void PlcBridgePanel::setProgramSlot(int value) { slot->setValue(value); }
void PlcBridgePanel::showDiagnostics(){if(auto* toggle=findChild<QToolButton*>("plcAdvancedToggle"))toggle->setChecked(true);pages->setCurrentIndex(2);if(auto* scroll=findChild<QScrollArea*>("plcControlScroll"))scroll->ensureWidgetVisible(pages,0,0);}
void PlcBridgePanel::send(BridgeProto::Command command) {
    using namespace BridgeProto;
    if (!modeEnabled) return;
    const quint16 parameter = (command == Load || command == ConfirmLoad) ? quint16(slot->value()) :
                              (command == Coordinates ? quint16(coordinates->currentIndex()) : 0);
    if (command == ConfirmLoad && QMessageBox::question(controls_->window(), "人工核对示教器",
        QString("请实际查看示教器：当前加载的程序是否确实对应程序号 %1 的绑定？\n"
                "此确认不会自动运行。请勿仅凭「加载已发送」判断。").arg(parameter),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    if (command == Run && QMessageBox::question(controls_->window(), "运行机器人程序",
        "将运行/继续示教器上已加载并人工确认的程序。本地打开的G代码不会自动下发。确认运行？",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    client.issue(command, parameter);
}
void PlcBridgePanel::refresh() {
    using namespace BridgeProto;
    const auto s = client.status(); const bool live = client.fresh();
    const bool online=live&&s.has(RobotOnline),owned=live&&client.ownsSession();
    const bool confirmed=online&&s.has(LoadConfirmed);
    const bool chosenConfirmed=confirmed&&s.selectedSlot==slot->value();
    WorkstationUi::badgeState(connectionBadge,live?"PLC · 状态有效":client.isConnected()?"PLC · 等待验证":"PLC · 未连接",live?"success":client.isConnected()?"warn":"idle");
    WorkstationUi::badgeState(sessionBadge,owned?"会话 · 已登记":"会话 · 未登记",owned?"success":"idle");
    WorkstationUi::badgeState(robotBadge,online?"机器人 · "+robotStateText(s):"机器人 · 未知",!online?"idle":s.alarm()||s.emergency()?"error":s.running()?"busy":"success");
    WorkstationUi::badgeState(enableBadge,online?(s.enabled()?"伺服 · 已使能":"伺服 · 未使能"):"伺服 · 未知",online&&s.enabled()?"success":"idle");
    WorkstationUi::badgeState(loadBadge,!online?"程序 · 未知":confirmed?QString("已人工确认 · %1号").arg(s.selectedSlot):"程序 · 未确认",!online?"idle":confirmed?"success":"warn");
    const int recommended=!owned?Register:s.alarm()?ClearError:s.has(StopLatch)?Rearm:!chosenConfirmed?
        (client.canIssue(ConfirmLoad,quint16(slot->value()))?ConfirmLoad:Load):!s.enabled()?Enable:Run;
    QString instruction,tone="idle",why;
    if(!modeEnabled)instruction="选择顶部的 AC802 PLC 连接方式。";
    else if(!live)instruction=client.isConnected()?"已建立 TCP 连接，正在等待 PLC 签名与动态心跳验证。":"先填写顶部 PLC 地址并连接，再登记控制权。";
    else if(client.pending()){instruction="正在等待 PLC 回执："+commandText(client.currentCommand())+"。";tone="warn";}
    else if(!owned){client.canIssue(Register,0,&why);instruction=why.isEmpty()?"下一步：点击「登记」，取得本机控制权。":why;}
    else if(!online){instruction="PLC 已连接；等待机器人通信恢复。";tone="warn";}
    else if(s.emergency()||s.alarm()){instruction="机器人反馈："+robotStateText(s)+"。先记录报警并处理原因，再点击「确认报警 / 清错」，随后复位停止锁存。";tone="error";}
    else if(s.has(StopLatch)){instruction="已停止锁存：核对停稳后，点击「重置通讯停止锁存」。";tone="warn";}
    else if(s.running()){instruction="机器人正在运行；可暂停或停止。";tone="success";}
    else if(!s.has(Commissioned)||!s.has(RemotePermit)||!s.externalAuto()||(s.version==Version&&!s.has(GdiMappingConfirmed))){client.canIssue(Load,quint16(slot->value()),&why);instruction=why.isEmpty()?"等待设备操作条件满足。":why;tone="warn";}
    else if(!chosenConfirmed){
        instruction=client.canIssue(ConfirmLoad,quint16(slot->value()))?QString("%1号加载请求已发送。请核对示教器，再点击「人工确认加载」。").arg(slot->value()):QString("下一步：选择程序号，再点击「选择并加载」。当前输入为 %1 号。").arg(slot->value());
    }else if(!s.enabled())instruction="程序已人工确认。下一步：点击「使能」。";
    else if(client.canIssue(Run)){instruction=s.paused()?"程序已暂停。点击「继续运行」可继续。":"准备就绪。核对现场后，点击「运行」。";tone="success";}
    else{client.canIssue(Run,0,&why);instruction=why;tone="warn";}
    nextAction->setText(instruction);WorkstationUi::tone(nextAction,tone);
    const int maximum = s.valid ? s.maxPrograms : MaxPrograms;
    if (slot->maximum() != maximum) slot->setMaximum(maximum);
    selectionInfo->setText(!s.valid ? "未验证PLC版本：V2支持GDI程序号1～255；连接V1时自动限制为原四槽。" :
        s.version == LegacyVersion ? "当前PLC V1：仅支持原四槽。启用1～255须导入V2并完成机器人GDI绑定。" :
        QString("当前PLC V2：程序号1～%1，GDI DI%2～%3；本地GDI映射确认：%4。")
            .arg(s.maxPrograms).arg(s.gdiFirst).arg(s.gdiLast).arg(yesNo(s.has(GdiMappingConfirmed))));
    connection->setText(QString("TCP：%1  协议：%2  动态状态：%3  PLC扫描心跳：%4")
        .arg(client.isConnected() ? "已连接" : "未连接").arg(s.valid ? QString("V%1匹配").arg(s.version) : "未验证")
        .arg(live ? "有效" : "无效").arg(live ? QString::number(s.plcHeartbeat) : "—"));
    robot->setText(live && s.has(RobotOnline) ? QString("机器人：%1  使能：%2  外部自动：%3  故障：%4  急停：%5")
        .arg(robotStateText(s)).arg(yesNo(s.enabled())).arg(yesNo(s.externalAuto())).arg(yesNo(s.alarm())).arg(yesNo(s.emergency())) : "机器人：无有效实时状态（含离线，旧数据不作实时反馈）");
    permissions->setText(!live ? "许可/会话/加载确认：无有效状态" : QString("映射放行：%1  远程许可：%2  本机会话：%3  停止锁存：%4  加载人工确认：%5")
        .arg(yesNo(s.has(Commissioned))).arg(yesNo(s.has(RemotePermit))).arg(yesNo(client.ownsSession()))
        .arg(yesNo(s.has(StopLatch))).arg(yesNo(s.has(LoadConfirmed))));
    const auto receiptDetail=live ? QString("回执%1：%2 / %3 / %4；%5。选中程序号%6（非加载身份）")
        .arg(s.ackSeq).arg(commandText(Command(s.ackCommand))).arg(resultText(s.result)).arg(errorText(s.error))
        .arg(stageText(s.stage)).arg(s.selectedSlot) : QString("PLC 回执：暂无有效状态。按钮不可用原因见上方提示，详细信息在下方诊断标签。");
    receipt->setText(live?QString("回执 %1 · %2 · %3").arg(s.ackSeq).arg(commandText(Command(s.ackCommand))).arg(resultText(s.result)):"PLC 回执：暂无有效状态。");receipt->setToolTip(receiptDetail);
    QStringList xyz; const QStringList axes{"X", "Y", "Z", "A", "B", "C"};
    for (int axis = 0; axis < 6; ++axis) {
        const auto v = live ? s.coordinate(axis) : std::optional<float>{};
        xyz << axes[axis] + "=" + (v ? QString::number(*v, 'f', 3) : "—");
    }
    position->setText(QString("PLC当前输出请求：%1（模式未独立确认）\n%2")
        .arg(!live ? "无有效状态" : s.has(CoordinatesRequested) ? "XYZABC" : "AO21～32").arg(xyz.join("  ")));
    for (auto it = actions.cbegin(); it != actions.cend(); ++it) {
        const auto c = Command(it.key()); const int row = int(c) - 1;
        const quint16 p = (c == Load || c == ConfirmLoad) ? quint16(slot->value()) :
                          (c == Coordinates ? quint16(coordinates->currentIndex()) : 0);
        QString why; const bool allowed = client.canIssue(c, p, &why);
        const QString availability = !modeEnabled ? "先在顶部选择AC802 PLC模式" : allowed ? "可操作（仍以PLC回执为准）" : why;
        const bool busy=live&&client.pending()&&client.currentCommand()==c;
        const bool done=live&&((c==Register&&owned)||(c==Enable&&online&&s.enabled())||(c==ConfirmLoad&&chosenConfirmed));
        QString state=busy?"busy":done?"done":modeEnabled&&allowed&&c==recommended?"next":"";
        it.value()->setText(busy?commandText(c)+" · 等待回执":done?commandText(c)+" ✓":c==Run&&confirmed?
            QString(s.paused()?"继续 %1 号":"运行 %1 号").arg(s.selectedSlot):commandText(c));
        WorkstationUi::button(it.value(),modeEnabled&&allowed,availability,state);
        cell(coverage, row, 0, QString::number(c)); cell(coverage, row, 1, commandText(c));
        cell(coverage, row, 2, parameterText(c, slot->value(), coordinates->currentIndex())); cell(coverage, row, 3, availability);
    }
    const auto reason=[this](Command command,quint16 parameter=0){QString text;client.canIssue(command,parameter,&text);return !modeEnabled?QString("请选择 PLC 连接方式"):text;};
    registerHint->setText(owned?"已登记；断开连接后需要重新登记。":reason(Register).isEmpty()?"可以登记本机会话。":reason(Register));
    loadHint->setText(chosenConfirmed?QString("已人工确认 %1 号。需要重载时可再次选择并加载。").arg(slot->value()):actions[ConfirmLoad]->isEnabled()?"加载已发送，请人工核对；此步骤不会运行。":actions[Load]->isEnabled()?"加载后人工核对示教器，再确认加载。":reason(Load,quint16(slot->value())));
    runHint->setText(actions[Run]->isEnabled()?QString("运行已人工确认的 %1 号；修改输入编号后须重新加载。").arg(s.selectedSlot):s.running()&&online?"正在运行；可暂停或停止。":reason(Run));
    for(auto* hint:{registerHint,loadHint,runHint})hint->setVisible(live);
    const QStringList localNames{"映射放行 / GDI本地确认", "远程许可 xRemotePermit", "原生总线故障输入", "自定义DI / AI写入", "点动/本地轨迹/工艺参数"};
    const QStringList localReasons{"PLC本地xCommissioned；V2还需xGdiProgramMappingConfirmed，上位机仅显示", "PLC本地授权与模式联锁；上位机仅显示", "由PLC总线通道提供，当前接口没有单独故障码", "V2仅开放DI49～56作GDI程序号；其余自定义写入未开放", "当前PLC设备接口未开放，不能由按钮替代实现"};
    for (int i = 0; i < localNames.size(); ++i) {
        cell(coverage, 12+i, 0, "—"); cell(coverage, 12+i, 1, localNames[i]); cell(coverage, 12+i, 2, "—"); cell(coverage, 12+i, 3, localReasons[i]);
    }
    slot->setEnabled(modeEnabled && !client.pending()); coordinates->setEnabled(modeEnabled && !client.pending());
    for (int r = 0; r < 96; ++r) {
        const bool input = r < 32; const int number = input ? 33 + r : 33 + r - 32;
        const auto value = !live ? std::optional<bool>{} : input ? s.desiredInput(number) : s.digitalOutput(number);
        cell(ioTable, r, 0, QString(input ? "DI%1·期望" : "DO%1·回读").arg(number));
        cell(ioTable, r, 1, input ? QString("HR%1.%2").arg(r < 16 ? 314 : 359).arg(r % 16) : QString("HR%1.%2").arg(320 + (number-33)/16).arg((number-33)%16));
        cell(ioTable, r, 2, input ? inputMeaning(number, s.version) : outputMeaning(number));
        cell(ioTable, r, 3, value ? (*value ? "1 / ON" : "0 / OFF") : (input && s.valid && s.version == LegacyVersion && number > 48 ? "V1未映射" : "无有效回读"));
    }
    for (int r = 0; r < 28; ++r) {
        const int number = r + 5; const auto v = live ? s.analogRaw(number) : std::optional<quint16>{};
        cell(analog, r, 0, QString("AO%1").arg(number)); cell(analog, r, 1, QString("HR%1 / FC04:%2").arg(324+r).arg(4+r));
        cell(analog, r, 2, v ? hexWord(*v) : "—"); cell(analog, r, 3, v ? QString::number(*v) : "—");
        QString meaning = "原始WORD；未定义比例";
        if (number >= 21) meaning = !live ? "复用区；无有效状态" : s.has(CoordinatesRequested) ?
            QString("%1·%2字（坐标请求，未独立确认）").arg(axes[(number-21)/2]).arg((number-21)%2 ? "高" : "低") : "AO原始WORD（AO请求，未独立确认）";
        cell(analog, r, 4, meaning);
    }
    for (int i = 0; i < StatusWords; ++i) {
        const bool valid = live && (i < 17 || i >= 52 || s.has(RobotOnline));
        cell(diagnostics, i, 0, QString::number(StatusBase+i)); cell(diagnostics, i, 1, QString::number(40001+StatusBase+i));
        cell(diagnostics, i, 2, statusFieldName(i)); cell(diagnostics, i, 3, valid ? hexWord(s.rawWords[i]) : "—");
        cell(diagnostics, i, 4, valid ? QString::number(s.rawWords[i]) : "—");
    }
    traffic->setText(QString("Modbus 发%1 / 收%2 / 错%3；往返%4ms；扫描变化距今%5ms\n"
        "启动号%6；本机会话%7；PLC所有者%8；上位机心跳发%9 / PLC收%10；DO42跳变%11")
        .arg(client.sentFrames()).arg(client.receivedFrames()).arg(client.errorCount()).arg(client.lastRoundTripMs())
        .arg(client.scanAgeMs() >= 0 ? QString::number(client.scanAgeMs()) : "—")
        .arg(live ? QString::number(s.boot) : "—").arg(client.sessionId()).arg(live ? QString::number(s.owner) : "—")
        .arg(client.sentHeartbeat()).arg(live ? QString::number(s.hostHeartbeat) : "—").arg(live ? QString::number(s.robotHeartbeatEdges) : "—"));
    QStringList flagText;
    const Flag flagBits[]{ScanValid, RobotOnline, RemotePermit, Registered, HostAlive, ActionBusy, Commissioned, CoordinatesRequested, StopLatch, LoadConfirmed, GdiMappingConfirmed};
    const QStringList names{"扫描有效", "机器人在线", "远程许可", "会话登记", "上位机存活", "动作忙", "映射放行", "坐标请求", "停止锁存", "加载人工确认", "GDI本地映射确认"};
    for (int i = 0; i < 11; ++i) flagText << names[i] + "=" +
        (live ? (i == 10 && s.version == LegacyVersion ? "V1不适用" : yesNo(s.has(flagBits[i]))) : "—");
    flags->setText(flagText.join("  "));
    pendingLabel->setText((client.pending() ? QString("在途请求%1：%2 参数%3，已等待%4ms；TCP写应答不等于动作完成。\n")
        .arg(client.pendingSequence()).arg(commandText(client.currentCommand())).arg(client.currentParameter()).arg(client.commandAgeMs()) : "在途请求：无。\n") +
        (live ? QString("最近拒绝%1：%2；STOP抢占请求%3；阶段%4=%5")
            .arg(s.rejectedSeq).arg(errorText(s.rejectReason)).arg(s.interruptedSeq).arg(s.stage).arg(stageText(s.stage)) : "当前状态无效；不把旧数据作为实时反馈。"));
}
void PlcBridgePanel::exportDiagnostics() {
    const auto path = QFileDialog::getSaveFileName(this, "导出PLC诊断快照", "plc_bridge_snapshot.json", "JSON (*.json)");
    if (path.isEmpty()) return;
    const bool live = client.fresh(); const auto s = client.status();
    QJsonObject root{{"timestamp", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)}, {"interfaceVersion", s.version}, {"maxPrograms", s.maxPrograms},
        {"selectionMode", s.selectionMode}, {"gdiMappingConfirmed", s.has(BridgeProto::GdiMappingConfirmed)},
        {"fresh", live}, {"tcpConnected", client.isConnected()}, {"ownsSession", client.ownsSession()},
        {"session", double(client.sessionId())}, {"sentFrames", double(client.sentFrames())},
        {"receivedFrames", double(client.receivedFrames())}, {"errors", double(client.errorCount())},
        {"scanAgeMs", double(client.scanAgeMs())}, {"pendingSequence", double(client.pendingSequence())},
        {"notice", "zero-based offsets; HR314/HR359 are desired outputs, not robot readback or write confirmation; coordinates are monitoring only"}};
    QJsonArray words;
    if (live) for (int i = 0; i < BridgeProto::StatusWords; ++i) words.append(QJsonObject{{"offset", 300+i},
        {"field", BridgeProto::statusFieldName(i)}, {"word", s.rawWords[i]},
        {"robotDataValid", i < 17 || i >= 52 || s.has(BridgeProto::RobotOnline)}});
    root.insert("statusWords", words); root.insert("eventLog", events->toPlainText());
    QSaveFile file(path);
    const auto bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        QMessageBox::warning(this, "导出失败", "无法保存诊断快照，请检查路径及写入权限。");
    else events->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss ") + "已导出只读诊断快照：" + path);
}
