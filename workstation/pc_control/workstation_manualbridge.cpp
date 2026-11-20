#include "workstation_manualbridge.h"
#include "workstation_ui.h"
#include <QtWidgets>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <cmath>
#include <limits>

namespace ManualProto {
static quint32 u32(const QVector<quint16>& w, int i) { return (quint32(w[i]) << 16) | w[i+1]; }
Status decode(const QVector<quint16>& w) {
    Status s;
    if (w.size() != StatusWords || w[0] != Magic || w[1] != Version) return s;
    std::copy(w.cbegin(), w.cend(), s.raw.begin()); s.valid = true;
    s.flags=w[2];s.phase=w[3];s.ack=u32(w,4);s.op=w[6];s.result=w[7];s.error=w[8];s.scan=w[9];
    s.slot=w[14];s.boot=u32(w,20);s.owner=u32(w,22);s.origin=w[27];
    return s;
}
Position decodePosition(const QVector<quint16>& w){
    Position p;
    if(w.size()!=PositionWords||w[0]!=0x5053||w[1]!=1||w[36]!=0x0603||w[37]!=12||w[38]!=1000)return p;
    p.flags=w[2];p.frame=w[3];p.boot=u32(w,4);p.counter=u32(w,6);p.age=w[8];
    if(!p.counter||p.frame<1||p.frame>99)return {};
    for(int i=0;i<12;++i){
        qint64 value=u32(w,10+2*i);if(value>=0x80000000LL)value-=0x100000000LL;
        if(std::abs(value)>10000000LL)return {};
        p.values[i]=double(value)/1000.0;
    }
    p.valid=true;return p;
}
QVector<quint16> frame(Op op, quint32 request, quint32 owner, quint32 boot, quint16 slot,
                     quint16 action, qint16 argument, quint16 speed, quint16 heartbeat) {
    QVector<quint16> w(CommandWords,0);
    w[0]=Magic;w[1]=op;w[2]=quint16(request>>16);w[3]=quint16(request);w[4]=Version;
    w[5]=action;w[6]=quint16(argument);w[7]=speed;w[8]=quint16(owner>>16);w[9]=quint16(owner);
    w[10]=quint16(boot>>16);w[11]=quint16(boot);w[12]=slot;
    quint16 sum=0xA55A;for(int i=0;i<13;++i) sum=quint16(sum^w[i]); w[13]=sum;
    w[14]=(op==Step || op==Capture || op==Return || op==Stage || op==Move || op==Jog)?1:0;w[15]=heartbeat;
    return w;
}
QString errorText(quint16 e) {
    switch(e) {
    case 0:return "无";case 1:return "扩展协议/校验不符";case 2:return "主会话或PLC启动号不符";
    case 3:return "请求序号重复或过期";case 4:return "有在途动作";case 5:return "本地安装或服务绑定未确认";
    case 6:return "机器人条件不满足或主桥正在操作";case 7:return "参数超出柜内限幅";
    case 8:return "微调心跳/操作许可失效";case 9:return "服务启动或动作回执超时";
    case 10:return "机器人服务回报拒绝";case 11:return "尚未进入微调";case 12:return "起点编号不符或尚未捕捉";
    case 13:return "停止尚未确认";case 14:return "指定位置参数不完整、顺序错误或事务编号不符";default:return QString("错误%1").arg(e);
    }
}
}

ManualBridgeClient::ManualBridgeClient(PlcBridgeClient& b,QObject* parent):QObject(parent),bridge(b) {
    wire.setPollRange(3,ManualProto::StatusBase,ManualProto::StatusWords);wire.setPollInterval(50);
    connect(&bridge,&PlcBridgeClient::linkChanged,this,[this](bool up,const QString&) {
        if(!up) {wire.disconnectFrom();reset();}
        else if(mode_&&!wire.isActive())wire.connectTo(bridge.endpointHost(),bridge.endpointPort(),bridge.endpointUnit());
    });
    connect(&wire,&ModbusClient::registersRead,this,&ManualBridgeClient::receive);
    connect(&wire,&ModbusClient::linkChanged,this,[this](bool up,const QString&) {if(!up)reset();emit changed();});
    connect(&wire,&ModbusClient::failure,this,[this](const QString& message) {
        emit notice("微调通信中断："+message+"；结果待确认，不自动重发。PLC租约将撤销执行。");
        wire.disconnectFrom();reset();emit changed();
    });
    timer.setInterval(50);connect(&timer,&QTimer::timeout,this,&ManualBridgeClient::tick);timer.start();
}
void ManualBridgeClient::reset() {
    if(pending_||status_.has(ManualProto::Entered)||status_.has(ManualProto::Busy)||status_.has(ManualProto::Stopping))uncertain_=true;
    targetFlow_=false;targetFields_.clear();targetIndex_=0;
    position_={};positionAge.invalidate();wire.setPollRange(3,ManualProto::StatusBase,ManualProto::StatusWords);
    pending_=0;status_={};advanced_=false;cancelling_=false;scanAge.invalidate();commandAge.invalidate();heartbeatAge.invalidate();
}
void ManualBridgeClient::reconnect() {
    if(mode_&&bridge.isConnected()&&!wire.isActive())wire.connectTo(bridge.endpointHost(),bridge.endpointPort(),bridge.endpointUnit());
}
void ManualBridgeClient::setModeEnabled(bool e) {
    if(mode_==e)return;mode_=e;
    if(!e) {cancel(true);wire.disconnectFrom();reset();}
    else if(bridge.isConnected()&&!wire.isActive())wire.connectTo(bridge.endpointHost(),bridge.endpointPort(),bridge.endpointUnit());
}
bool ManualBridgeClient::fresh() const {
    return mode_&&wire.isConnected()&&status_.valid&&advanced_&&scanAge.isValid()&&scanAge.elapsed()<1000;
}
bool ManualBridgeClient::positionFresh() const {
    return fresh()&&bridge.fresh()&&bridge.status().has(BridgeProto::RobotOnline)&&!bridge.status().alarm()&&!bridge.status().emergency()&&position_.valid&&(position_.flags&13)==13&&position_.age<8000&&
        positionAge.isValid()&&positionAge.elapsed()<8000&&position_.boot==bridge.status().boot&&
        status_.has(ManualProto::Entered)&&!status_.has(ManualProto::Busy)&&!status_.has(ManualProto::Stopping)&&!pending()&&!targetFlow_;
}
bool ManualBridgeClient::ready(QString* why,bool entered) const {
    const auto no=[why](const char* text){if(why)*why=text;return false;};
    if(bridge.operationInhibited())return no("机器人模式正在切换，请等待回读确认");
    if(!fresh())return no("PLC微调扩展尚未安装、未连接或扫描心跳失效");
    if((status_.raw[28]&31)!=31||status_.raw[29]!=0x0603)return no("需要配套的 R6.3 PLC 微调程序与柜内服务，速度编码需要一致");
    if(uncertain_)return no("上次动作结果待确认；请用原PLC停止并等候新鲜停稳状态");
    if(!bridge.fresh()||!bridge.ownsSession())return no("先在PLC设备控制页登记本机会话");
    const auto b=bridge.status();
    if(status_.boot!=b.boot)return no("PLC启动标识已改变，重新连接并登记");
    if(!status_.has(ManualProto::Installed)||!status_.has(ManualProto::Binding))return no("PLC本地微调安装/固定程序绑定未确认");
    if(!b.has(BridgeProto::RobotOnline))return no("PLC与机器人动态通信无效");
    if(b.emergency())return no("机器人急停未解除，请检查现场急停状态");
    if(b.alarm())return no("机器人反馈报警：先记录示教器报警，再点击「确认报警 / 清错」；3400需修正变量文件");
    if(!b.has(BridgeProto::Commissioned))return no("PLC本地映射尚未放行（xCommissioned）");
    if(!b.has(BridgeProto::GdiMappingConfirmed))return no("PLC本地GDI程序绑定尚未确认");
    if(!b.has(BridgeProto::RemotePermit))return no("PLC本地远程许可未放行");
    if(b.has(BridgeProto::StopLatch))return no("通讯停止已锁存：停稳并确认报警后，点击「复位停止锁存」");
    if(!b.externalAuto())return no("机器人尚未切到外部自动模式");
    if(!b.enabled())return no("机器人未使能；确认报警并复位后，重新加载、确认和使能");
    if(!b.has(BridgeProto::LoadConfirmed)||b.selectedSlot!=status_.slot)return no("先加载微调固定程序号，并人工核对确认加载");
    if(bridge.pending()||b.has(BridgeProto::ActionBusy))return no("原PLC控制命令仍在执行");
    if(entered&&!status_.has(ManualProto::Entered))return no("请先点击进入微调");
    if(why)why->clear();return true;
}
bool ManualBridgeClient::issue(ManualProto::Op op,quint16 action,qint16 arg,quint16 speed) {
    QString why;
    if(targetFlow_&&!advancingTarget_){emit notice("指定位置事务尚未结束，请等待或停止。");return false;}
    if(!ready(&why,op!=ManualProto::Enter)||pending()||status_.has(ManualProto::Busy)||status_.has(ManualProto::Stopping)) {
        emit notice(why.isEmpty()?"等待当前动作或停止回执":why);return false;
    }
    if((op==ManualProto::Step||op==ManualProto::Jog)&&(!(action>=1&&action<=6)&&!(action>=11&&action<=16)))return false;
    const bool joint=action>=1&&action<=6;
    const int segment=joint?ManualProto::jointJogLimit(speed):ManualProto::worldJogLimit(speed);
    if((op==ManualProto::Step||op==ManualProto::Jog)&&(arg==0||std::abs(int(arg))>(op==ManualProto::Jog?segment:10000)||speed<(joint?10:1)||speed>(joint?10000:32767))) {
        emit notice("速度：关节0.10～100.00%；WORLD 0.1～3276.7 mm/s或°/s。请输入有效距离和速度。");return false;
    }
    if(op==ManualProto::Return&&(speed<10||speed>10000))return false;
    if(sequence_==std::numeric_limits<quint32>::max()){emit notice("微调请求序号已用尽，重新连接。");return false;}
    pending_=++sequence_;op_=op;cancelling_=false;commandAge.restart();heartbeatAge.restart();
    wire.writeRegisters(ManualProto::CommandBase,ManualProto::frame(op,pending_,bridge.sessionId(),bridge.status().boot,
        status_.slot,action,arg,speed,++heartbeat_));
    emit changed();return true;
}
bool ManualBridgeClient::moveTarget(bool cartesian,const std::array<std::optional<double>,6>& values,quint16 speed) {
    if(targetFlow_||pending()||!ready()||status_.has(ManualProto::Busy)||status_.has(ManualProto::Stopping)||speed<(cartesian?1:10)||speed>(cartesian?32767:10000))return false;
    QList<TargetField> fields;
    const quint16 id=quint16(targetId_>=999?1:targetId_+1);
    fields.append({23,qint16(cartesian?2:1),id});
    bool selected=false;
    for(int i=0;i<6;++i){
        if(!values[i]){fields.append({quint16(24+i),0,2000});continue;}
        selected=true;const auto value=*values[i];const double limit=cartesian&&i<3?10000:360;
        if(!std::isfinite(value)||std::abs(value)>limit){emit notice("指定位置超出输入范围。");return false;}
        const qint64 scaled=std::llround(value*1000);
        // Euclidean division preserves negative sub-unit values: -0.001=-1+999/1000.
        const int whole=int(std::floor(double(scaled)/1000.0));
        fields.append({quint16(24+i),qint16(whole),quint16(scaled-qint64(whole)*1000)});
    }
    if(!selected){emit notice("请至少勾选一个要修改的关节或坐标分量。");return false;}
    targetFields_=fields;targetFlow_=true;targetIndex_=0;targetId_=id;targetSpeed_=speed;
    advanceTarget();return targetFlow_||pending();
}
void ManualBridgeClient::advanceTarget(){
    if(!targetFlow_||pending())return;
    advancingTarget_=true;bool sent=false;
    if(targetIndex_<targetFields_.size()){
        const auto field=targetFields_[targetIndex_];
        sent=issue(ManualProto::Stage,field.action,field.whole,field.fraction);
    }else sent=issue(ManualProto::Move,30,qint16(targetId_),targetSpeed_);
    advancingTarget_=false;
    if(!sent){targetFlow_=false;targetFields_.clear();emit notice("指定位置事务已撤销，未自动重试。");cancel();}
}
void ManualBridgeClient::cancel(bool leave) {
    targetFlow_=false;targetFields_.clear();targetIndex_=0;
    if(!wire.isConnected()||!bridge.ownsSession()||!status_.valid)return;
    if(cancelling_)return;
    if(!pending()&&!status_.has(ManualProto::Entered)&&!status_.has(ManualProto::Busy))return;
    if(sequence_==std::numeric_limits<quint32>::max()){wire.disconnectFrom();return;}
    pending_=++sequence_;op_=leave?ManualProto::Exit:ManualProto::Cancel;cancelling_=true;commandAge.restart();
    wire.writePriorityRegisters(ManualProto::CommandBase,ManualProto::frame(op_,pending_,bridge.sessionId(),bridge.status().boot,
        status_.slot,0,0,0,++heartbeat_),ManualProto::CommandBase,ManualProto::CommandWords);
    emit notice("已撤销后续请求，等待PLC确认停止。");emit changed();
}
void ManualBridgeClient::receive(int fc,int start,const QVector<quint16>& words,qint64) {
    if(fc==3&&start==ManualProto::PositionBase){
        auto p=ManualProto::decodePosition(words);
        if(p.valid&&(!position_.valid||p.boot!=position_.boot||p.counter!=position_.counter))positionAge.restart();
        position_=p;wire.setPollRange(3,ManualProto::StatusBase,ManualProto::StatusWords);emit changed();return;
    }
    if(fc!=3||start!=ManualProto::StatusBase)return;
    auto s=ManualProto::decode(words);
    if(s.valid) {
        if(status_.valid&&s.boot!=status_.boot) {reset();emit notice("PLC重启，微调身份失效，不恢复动作。");}
        if(status_.valid&&s.scan!=status_.scan){advanced_=true;scanAge.restart();}
        else if(!status_.valid)scanAge.restart();
    }
    status_=s;
    if(s.valid&&(s.raw[28]&4)&&s.raw[29]==0x0603)wire.setPollRange(3,ManualProto::PositionBase,ManualProto::PositionWords);
    else{position_={};positionAge.invalidate();}
    if(fresh()&&bridge.fresh()&&bridge.status().stopped()&&!s.has(ManualProto::Entered)&&!s.has(ManualProto::Busy)&&!s.has(ManualProto::Stopping))uncertain_=false;
    if(pending()&&s.valid&&s.ack==pending_&&s.op==op_&&s.result>=2) {
        const auto seq=pending_;const auto op=op_;pending_=0;cancelling_=false;
        const bool next=targetFlow_&&op==ManualProto::Stage&&s.result==2;
        if(next)++targetIndex_;
        else if(targetFlow_){targetFlow_=false;targetFields_.clear();}
        emit finished(seq,op,s.result,s.error);
        if(next)QTimer::singleShot(0,this,&ManualBridgeClient::advanceTarget);
    }
    emit changed();
}
void ManualBridgeClient::tick() {
    if(wire.isConnected()&&bridge.fresh()&&bridge.ownsSession()&&fresh()) {
        if(!heartbeatAge.isValid()||heartbeatAge.elapsed()>=150){
            heartbeatAge.restart();wire.writeRegister(ManualProto::CommandBase+15,++heartbeat_);
        }
        if((pending()||status_.has(ManualProto::Entered))&&!ready(nullptr,false)&&!cancelling_)cancel(true);
    }
    if(pending()&&op_!=ManualProto::Jog&&commandAge.isValid()&&commandAge.elapsed()>((op_==ManualProto::Move||op_==ManualProto::Return)?123000:23000)&&!notifying_) {
        notifying_=true;emit notice("微调回执超时，结果未知；已断开扩展连接，由PLC租约撤销，不自动重发。");
        wire.disconnectFrom();reset();notifying_=false;
    }
    if(status_.valid&&scanAge.isValid()&&scanAge.elapsed()>=1000&&wire.isConnected()) {
        emit notice("微调PLC扫描心跳失效，停止保持请求；结果待确认。");wire.disconnectFrom();reset();
    }
    emit changed();
}

ManualBridgePanel::ManualBridgePanel(PlcBridgeClient& b,QWidget* parent):QWidget(parent),bridge(b),manual(b,this) {
    setObjectName("plcManualPanel");
    auto* layout=WorkstationUi::page(this,"manualControlScroll");
    layout->parentWidget()->setMaximumWidth(1780);
    auto* heading=new QHBoxLayout;auto* title=new QLabel("微调与实验起点");title->setProperty("heading",true);heading->addWidget(title);heading->addStretch();
    channelBadge=WorkstationUi::badge("manualChannelBadge");serviceBadge=WorkstationUi::badge("manualServiceBadge");originBadge=WorkstationUi::badge("manualOriginBadge");
    layout->addLayout(heading);
    layout->setSpacing(8);
    auto* registration=new QWidget;registration->setObjectName("manualRegistration");
    auto* registrationRow=new QHBoxLayout(registration);registrationRow->setContentsMargins(0,0,0,0);registrationRow->setSpacing(8);
    registrationRow->addWidget(new QLabel("本机控制权"));
    registerSession=new QPushButton("登记");registerSession->setObjectName("manualRegister");
    logoutSession=new QPushButton("撤销会话");logoutSession->setObjectName("manualLogout");
    sessionState=WorkstationUi::hint();sessionState->setObjectName("manualSessionState");
    registrationRow->addWidget(registerSession);registrationRow->addWidget(logoutSession);registrationRow->addWidget(sessionState,1);
    for(auto* badge:{channelBadge,serviceBadge,originBadge})registrationRow->addWidget(badge);
    layout->addWidget(registration);
    connect(registerSession,&QPushButton::clicked,this,[this]{bridge.issue(BridgeProto::Register);});
    connect(logoutSession,&QPushButton::clicked,this,[this]{if(!engaged())bridge.issue(BridgeProto::Logout);});
    auto* setup=new QGridLayout;setupGrid=setup;
    load=new QPushButton("1  加载微调程序");load->setObjectName("manualLoad");
    confirm=new QPushButton("2  人工确认加载…");confirm->setObjectName("manualConfirm");
    enable=new QPushButton("3  使能");enable->setObjectName("manualEnable");
    enter=new QPushButton("4  进入微调");enter->setObjectName("manualEnter");
    int setupColumn=0;for(auto* button:{load,confirm,enable,enter}){button->setMinimumHeight(40);setup->addWidget(button,0,setupColumn++);}layout->addLayout(setup);
    connect(load,&QPushButton::clicked,this,[this]{const auto slot=manual.status().slot;if(slot)bridge.issue(BridgeProto::Load,slot);});
    connect(confirm,&QPushButton::clicked,this,[this]{
        const auto slot=manual.status().slot;
        if(slot&&QMessageBox::question(this,"核对常驻服务",QString("请核对示教器实际加载的是常驻微调工程中的 manual_service 程序，GDI编号%1。\n确认实际工程与程序一致？").arg(slot),
            QMessageBox::Yes|QMessageBox::No,QMessageBox::No)==QMessageBox::Yes)bridge.issue(BridgeProto::ConfirmLoad,slot);
    });
    connect(enable,&QPushButton::clicked,this,[this]{bridge.issue(BridgeProto::Enable);});
    connect(enter,&QPushButton::clicked,this,[this]{manual.issue(ManualProto::Enter);});
    auto* recovery=new QHBoxLayout;health=new QLabel;health->setObjectName("manualNextAction");health->setProperty("banner",true);health->setWordWrap(true);recovery->addWidget(health,1);
    acknowledge=new QPushButton("确认报警 / 清错");acknowledge->setObjectName("manualAcknowledge");
    rearm=new QPushButton("复位停止锁存");rearm->setObjectName("manualRearm");
    recoveryStatus=WorkstationUi::hint();recoveryStatus->setObjectName("manualRecoveryStatus");recoveryStatus->setParent(this);
    recoveryStatus->hide();layout->addLayout(recovery);
    connect(acknowledge,&QPushButton::clicked,this,[this]{bridge.issue(BridgeProto::ClearError);});
    connect(rearm,&QPushButton::clicked,this,[this]{bridge.issue(BridgeProto::Rearm);});
    auto* body=new QGridLayout;manualBody=body;body->setSpacing(12);layout->addLayout(body);
    auto* movement=new QGroupBox("关节与末端控制");movement->setObjectName("manualPrimaryControls");auto* movementLayout=new QVBoxLayout(movement);
    auto* motionTabs=new QTabWidget;motionTabs->setObjectName("manualMotionTabs");movementLayout->addWidget(motionTabs);
    auto* relativePage=new QWidget;auto* motionLayout=new QVBoxLayout(relativePage);motionLayout->setSpacing(8);
    auto* targetPage=new QWidget;auto* targetLayout=new QVBoxLayout(targetPage);
    motionTabs->addTab(relativePage,"按住 / 单步");motionTabs->addTab(targetPage,"指定位置");body->addWidget(movement,0,0,Qt::AlignTop);
    poseHost_=new QWidget;poseHost_->setObjectName("manualPoseHost");auto* poseLayout=new QVBoxLayout(poseHost_);poseLayout->setContentsMargins(0,12,0,0);body->addWidget(poseHost_,0,1,Qt::AlignTop);
    body->setColumnStretch(0,1);body->setColumnStretch(1,2);body->setColumnStretch(2,1);
    auto fitMotionPage=[motionTabs]{for(int i=0;i<motionTabs->count();++i)motionTabs->widget(i)->setSizePolicy(QSizePolicy::Preferred,i==motionTabs->currentIndex()?QSizePolicy::Preferred:QSizePolicy::Ignored);motionTabs->updateGeometry();};
    fitMotionPage();connect(motionTabs,&QTabWidget::currentChanged,this,[this,fitMotionPage]{if(held_||returning_||manual.pending()||manual.targetActive())halt(true);fitMotionPage();});
    auto* homeBox=new QGroupBox("当前位置与实验起点");homeBox->setObjectName("manualOriginControls");homeBox_=homeBox;auto* homeLayout=new QVBoxLayout(homeBox);homeLayout->setSpacing(8);body->addWidget(homeBox,0,2,Qt::AlignTop);
    positionState=WorkstationUi::hint();positionState->setObjectName("manualPositionState");homeLayout->addWidget(positionState);
    homeLayout->addWidget(recoveryStatus);recoveryStatus->show();
    auto* liveGrid=new QGridLayout;liveGrid->setVerticalSpacing(4);liveGrid->setColumnStretch(1,1);liveGrid->setColumnStretch(3,1);
    for(int i=0;i<6;++i){
        auto* joint=new QLabel("—");joint->setObjectName(QString("manualJointCurrent%1").arg(i));
        auto* cart=new QLabel("—");cart->setObjectName(QString("manualWorldCurrent%1").arg(i));
        joint->setTextInteractionFlags(Qt::TextSelectableByMouse);cart->setTextInteractionFlags(Qt::TextSelectableByMouse);
        jointCurrent<<joint;worldCurrent<<cart;
        liveGrid->addWidget(new QLabel(QString("J%1").arg(i+1)),i,0);liveGrid->addWidget(joint,i,1);
        liveGrid->addWidget(new QLabel(QStringList{"X","Y","Z","A","B","C"}.at(i)),i,2);liveGrid->addWidget(cart,i,3);
    }
    homeLayout->addLayout(liveGrid);homeLayout->addWidget(WorkstationUi::hint("右列：Default 工具 · WORLD。XYZ 为 mm，ABC 为 °。停稳后自动更新完整快照，运动时暂停参考。"));
    auto* divider=new QFrame;divider->setFrameShape(QFrame::HLine);homeLayout->addWidget(divider);
    auto* settings=new QGridLayout;settings->setVerticalSpacing(8);settings->setHorizontalSpacing(10);
    settings->setColumnStretch(0,1);settings->setColumnStretch(1,1);
    frame=new QComboBox;frame->setObjectName("manualFrame");frame->addItems({"关节 J1～J6","WORLD · XYZ / ABC"});frame->setToolTip("关节角，或 Default 工具下的 WORLD 平移 XYZ / 欧拉角 ABC");
    multiplier=new QComboBox;multiplier->setObjectName("manualMultiplier");
    const QStringList distanceNames{"极细","很细","细调","细调","常用","中步","粗调","大步","大步"};int presetIndex=0;
    for(double value:{.001,.005,.01,.05,.1,.5,1.,5.,10.})multiplier->addItem(QString("%1 · %2").arg(distanceNames[presetIndex++]).arg(value,0,'f',3),value);multiplier->setCurrentIndex(4);
    multiplier->setPlaceholderText("自定义距离");
    distance=new QDoubleSpinBox;distance->setObjectName("manualDistance");distance->setRange(.001,10);distance->setDecimals(3);distance->setValue(.1);distance->setSingleStep(.01);
    speed=new QDoubleSpinBox;speed->setObjectName("manualSpeed");speed->setRange(.1,100);speed->setDecimals(2);speed->setValue(1);
    speedPreset=new QComboBox;speedPreset->setObjectName("manualSpeedPreset");
    const QStringList speedNames{"最慢","慢速","常用","较快","原最高","快速","快速","快速","高速","高速","满速"};int speedIndex=0;
    for(double value:{.1,.5,1.,2.,5.,10.,15.,25.,50.,75.,100.})speedPreset->addItem(QString("%1 · %2").arg(speedNames[speedIndex++]).arg(value,0,'f',2),value);speedPreset->setCurrentIndex(2);speedPreset->setPlaceholderText("自定义速度");
    returnSpeed=new QDoubleSpinBox;returnSpeed->setObjectName("manualReturnSpeed");returnSpeed->setRange(.1,100);returnSpeed->setDecimals(2);returnSpeed->setValue(1);
    units=WorkstationUi::hint();units->setParent(this);
    for(auto* combo:{frame,multiplier,speedPreset}){combo->setMinimumContentsLength(8);combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);}
    settings->addWidget(frame,0,0,1,2);
    settings->addWidget(new QLabel("单步距离 · 预设 / 数值"),1,0,1,2);settings->addWidget(multiplier,2,0);settings->addWidget(distance,2,1);
    settings->addWidget(new QLabel("移动速度 · 预设 / 数值"),3,0,1,2);settings->addWidget(speedPreset,4,0);settings->addWidget(speed,4,1);
    speedHint=WorkstationUi::hint();speedHint->setObjectName("manualSpeedHint");settings->addWidget(speedHint,5,0,1,2);
    units->hide();
    motionLayout->addLayout(settings);
    connect(multiplier,&QComboBox::currentIndexChanged,this,[this]{if(multiplier->currentIndex()>=0)distance->setValue(multiplier->currentData().toDouble());});
    connect(speedPreset,&QComboBox::currentIndexChanged,this,[this]{if(speedPreset->currentIndex()>=0)speed->setValue(speedPreset->currentData().toDouble());});
    connect(distance,&QDoubleSpinBox::valueChanged,this,[this]{QSignalBlocker guard(multiplier);int found=-1;for(int i=0;i<multiplier->count();++i)if(std::abs(multiplier->itemData(i).toDouble()-distance->value())<.0001)found=i;multiplier->setCurrentIndex(found);refresh();});
    connect(speed,&QDoubleSpinBox::valueChanged,this,[this]{QSignalBlocker guard(speedPreset);int found=-1;for(int i=0;i<speedPreset->count();++i)if(std::abs(speedPreset->itemData(i).toDouble()-speed->value())<.0001)found=i;speedPreset->setCurrentIndex(found);refresh();});
    connect(frame,&QComboBox::currentIndexChanged,this,[this]{
        halt();
        { QSignalBlocker blockSpeed(speed),blockPreset(speedPreset);
          speed->setMaximum(frame->currentIndex()==0?100:3276.7);speed->setDecimals(frame->currentIndex()==0?2:1);speedPreset->clear();
          const QStringList names{"最慢","慢速","常用","较快","原最高","快速","快速","快速","高速","高速","满速"};int index=0,selected=-1;
          const QList<double> values=frame->currentIndex()==0?QList<double>{.1,.5,1.,2.,5.,10.,15.,25.,50.,75.,100.}:QList<double>{.1,.5,1.,5.,10.,25.,50.,100.,250.,500.,1000.,2000.,3276.7};
          for(double value:values){
              speedPreset->addItem(frame->currentIndex()==0?QString("%1 · %2").arg(names[index]).arg(value,0,'f',2):QString::number(value,'f',1),value);
              if(std::abs(speed->value()-value)<.0001)selected=index;++index;
          }
          speedPreset->setCurrentIndex(selected);
        }
        refresh();
    });
    auto* grid=new QGridLayout;grid->setVerticalSpacing(5);grid->setHorizontalSpacing(6);
    for(int axis=0;axis<6;++axis){
        const int row=axis%3,column=(axis/3)*3;
        auto* label=new QLabel(QString("J%1").arg(axis+1));label->setObjectName(QString("manualAxis%1").arg(axis));label->setFixedWidth(28);grid->addWidget(label,row,column);
        for(int d=0;d<2;++d){
            auto* button=new QPushButton(d?"+":"−");button->setObjectName(QString("manualDir%1").arg(axis*2+d));button->setProperty("motion",true);button->setAccessibleName(QString("J%1 %2方向按住移动").arg(axis+1).arg(d?"正":"负"));
            button->setMinimumSize(38,34);button->setStyleSheet("QPushButton { padding:4px 8px; font-size:12pt; }");button->setAutoRepeat(false);directions<<button;grid->addWidget(button,row,column+d+1);
            button->installEventFilter(this);
            connect(button,&QPushButton::pressed,this,[this,axis,d]{
                if(held_||returning_||manual.pending()||manual.targetActive())return;
                axis_=axis;direction_=d?1:-1;held_=true;
                const int action=(frame->currentIndex()==0?1:11)+axis_;
                const int scaledSpeed=int(std::lround(speed->value()*(frame->currentIndex()==0?100:10)));
                const auto segment=qint16(direction_*(frame->currentIndex()==0?ManualProto::jointJogLimit(scaledSpeed):ManualProto::worldJogLimit(scaledSpeed)));
                if(!manual.issue(ManualProto::Jog,quint16(action),segment,quint16(scaledSpeed)))held_=false;
                refresh();
            });
            connect(button,&QPushButton::released,this,[this,axis,d]{if(held_&&axis_==axis&&direction_==(d?1:-1))halt();});
        }
    }
    for(int c:{1,2,4,5})grid->setColumnStretch(c,1);motionLayout->addLayout(grid);
    auto* once=new QHBoxLayout;
    auto* axisChoice=new QComboBox;axisChoice->setObjectName("manualOnceAxis");axisChoice->addItems({"J1 / X","J2 / Y","J3 / Z","J4 / A","J5 / B","J6 / C"});
    auto* sign=new QComboBox;sign->addItems({"正方向 +","负方向 −"});
    single=new QPushButton("单步移动");single->setObjectName("manualSingle");once->addWidget(axisChoice,1);once->addWidget(sign,1);motionLayout->addLayout(once);motionLayout->addWidget(single);
    motionLayout->addStretch(1);
    connect(single,&QPushButton::clicked,this,[this,axisChoice,sign]{axis_=axisChoice->currentIndex();direction_=sign->currentIndex()? -1:1;nextStep();});
    targetLayout->addWidget(WorkstationUi::hint("勾选要修改的分量，其余保持本次请求开始时的位置。填写和传输参数不会移动；点击下方按钮并确认后执行。"));
    targetFrame=new QComboBox;targetFrame->setObjectName("manualTargetFrame");targetFrame->addItems({"绝对关节角 J1～J6 · PTP","绝对 WORLD XYZ / ABC · 直线 Lin"});targetFrame->setMinimumContentsLength(10);targetFrame->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);targetLayout->addWidget(targetFrame);
    targetFillCurrent=new QPushButton("用当前位置填入目标");targetFillCurrent->setObjectName("manualFillCurrent");targetLayout->addWidget(targetFillCurrent);
    connect(targetFillCurrent,&QPushButton::clicked,this,&ManualBridgePanel::fillCurrentTarget);
    auto* targetGrid=new QGridLayout;targetGrid->addWidget(new QLabel("修改"),0,0);targetGrid->addWidget(new QLabel("分量"),0,1);targetGrid->addWidget(new QLabel("当前位置"),0,2);targetGrid->addWidget(new QLabel("目标值"),0,3);
    for(int i=0;i<6;++i){
        auto* use=new QCheckBox("修改");use->setObjectName(QString("manualTargetUse%1").arg(i));
        auto* label=new QLabel;auto* value=new QDoubleSpinBox;value->setDecimals(3);value->setRange(-360,360);value->setSingleStep(1);value->setEnabled(false);value->setObjectName(QString("manualTargetValue%1").arg(i));
        auto* current=new QLabel("—");current->setObjectName(QString("manualTargetCurrent%1").arg(i));targetCurrent<<current;
        targetSelected<<use;targetLabels<<label;targetValues<<value;
        targetGrid->addWidget(use,i+1,0);targetGrid->addWidget(label,i+1,1);targetGrid->addWidget(current,i+1,2);targetGrid->addWidget(value,i+1,3);
        connect(use,&QCheckBox::toggled,this,&ManualBridgePanel::refresh);
    }
    targetGrid->setColumnStretch(2,1);targetGrid->setColumnStretch(3,2);targetLayout->addLayout(targetGrid);
    auto* targetSpeedRow=new QHBoxLayout;targetSpeedRow->addWidget(new QLabel("目标速度"));
    targetSpeed=new QDoubleSpinBox;targetSpeed->setObjectName("manualTargetSpeed");targetSpeed->setRange(.1,100);targetSpeed->setDecimals(2);targetSpeed->setValue(1);targetSpeedRow->addWidget(targetSpeed,1);targetLayout->addLayout(targetSpeedRow);
    targetHint=WorkstationUi::hint();targetLayout->addWidget(targetHint);
    targetMove=new QPushButton("移动到指定位置…");targetMove->setObjectName("manualTargetMove");targetMove->setMinimumHeight(40);targetLayout->addWidget(targetMove);targetLayout->addStretch(1);
    connect(targetMove,&QPushButton::clicked,this,&ManualBridgePanel::moveTarget);
    connect(targetFrame,&QComboBox::currentIndexChanged,this,[this]{
        // Changing coordinate systems requires explicit selection again.
        for(auto* selected:targetSelected){QSignalBlocker guard(selected);selected->setChecked(false);}
        refresh();
    });
    experiment=new QLineEdit;experiment->setPlaceholderText("实验起点名称，例如试次A");experiment->setMaxLength(80);experiment->setToolTip("先微调到需要的位置，再捕捉起点。每次实验可使用不同起点。");
    captureButton=new QPushButton("捕捉起点…");captureButton->setObjectName("manualCapture");
    returnButton=new QPushButton("返回起点…");returnButton->setObjectName("manualReturn");
    homeLayout->addWidget(experiment);homeLayout->addWidget(captureButton);
    connect(captureButton,&QPushButton::clicked,this,&ManualBridgePanel::capture);connect(returnButton,&QPushButton::clicked,this,&ManualBridgePanel::returnHome);
    origin=new QLabel;origin->setWordWrap(true);homeLayout->addWidget(origin);
    auto* returnRow=new QHBoxLayout;returnRow->addWidget(new QLabel("回程关节速度 %"));returnRow->addWidget(returnSpeed,1);homeLayout->addLayout(returnRow);homeLayout->addWidget(returnButton);
    homeLayout->addWidget(WorkstationUi::hint("回程按设定速度执行一次关节 PTP，直接返回保存的起点。"));
    coordinates=WorkstationUi::hint();coordinates->setParent(this);coordinates->hide();homeLayout->addStretch(1);
    stop=new QPushButton("停止运动");stop->setObjectName("manualStop");stop->setProperty("danger",true);stop->setMinimumHeight(32);
    exit=new QPushButton("退出微调服务");exit->setObjectName("manualExit");
    connect(stop,&QPushButton::clicked,this,[this]{halt(true);});connect(exit,&QPushButton::clicked,this,[this]{halt(true);});
    layout->addWidget(WorkstationUi::hint("按住：段长随速度变化，关节最多1°，WORLD最多10mm/°；松开完成当前段后停。单步距离单独设置。停止键、失焦或通信异常会请求普通停止。"));
    monitorHost_=new QWidget;monitorHost_->setObjectName("manualMonitorHost");auto* monitorLayout=new QVBoxLayout(monitorHost_);monitorLayout->setContentsMargins(0,0,0,0);layout->addWidget(monitorHost_);
    auto* details=WorkstationUi::disclosure(layout,"微调回执与操作记录","manualAdvanced");auto* diagnosticLayout=new QVBoxLayout(details);diagnosticLayout->setContentsMargins(0,0,0,0);
    auto* channelRow=new QHBoxLayout;setupHint=WorkstationUi::hint();channelRow->addWidget(setupHint,1);
    reconnect=new QPushButton("重连微调通道");reconnect->setObjectName("manualReconnect");channelRow->addWidget(reconnect);diagnosticLayout->addLayout(channelRow);
    connect(reconnect,&QPushButton::clicked,this,[this]{manual.reconnect();});
    receipt=new QLabel;receipt->setWordWrap(true);diagnosticLayout->addWidget(receipt);
    log=new QPlainTextEdit;log->setObjectName("manualEventLog");log->setReadOnly(true);log->setMaximumBlockCount(100);log->setMinimumHeight(70);log->setMaximumHeight(100);log->setPlaceholderText("微调请求、完成及停止回执显示在这里。进入服务不会自动发出运动增量。");diagnosticLayout->addWidget(log);
    heading->insertWidget(1,stop);heading->insertWidget(2,exit);heading->insertWidget(3,acknowledge);heading->insertWidget(4,rearm);
    layout->addStretch(1);
    connect(&manual,&ManualBridgeClient::notice,this,[this](const QString& text){log->appendPlainText(text);emit notice(text);});
    connect(&manual,&ManualBridgeClient::changed,this,&ManualBridgePanel::refresh);
    connect(&bridge,&PlcBridgeClient::changed,this,&ManualBridgePanel::refresh);
    connect(&manual,&ManualBridgeClient::finished,this,[this](quint32 sequence,quint16 op,quint16 result,quint16 error){
        const auto text=QString("请求%1 操作%2：%3，%4").arg(sequence).arg(op).arg(result==2?"完成":result==6?"已中断":result==4?"拒绝":"未完成").arg(ManualProto::errorText(error));
        log->appendPlainText(text);emit notice(text);
        if(op==ManualProto::Capture&&result==2)saveOrigin();
        if(result!=2){
            held_=false;returning_=false;
            const auto b=bridge.status();
            const QString detail=QString("最新主桥反馈：报警=%1，急停=%2，使能=%3，外部自动=%4，机器人在线=%5，停止锁存=%6，加载确认=%7。")
                .arg(b.alarm()).arg(b.emergency()).arg(b.enabled()).arg(b.externalAuto())
                .arg(b.has(BridgeProto::RobotOnline)).arg(b.has(BridgeProto::StopLatch)).arg(b.has(BridgeProto::LoadConfirmed));
            log->appendPlainText(detail);emit notice(detail);
        }
        if(op==ManualProto::Return){returning_=false;if(result==2&&!manual.status().has(ManualProto::AtHome))emit notice("回程指令已完成，起点误差尚未确认；未自动重复执行。");}
        refresh();
    });
    repeat.setInterval(75);connect(&repeat,&QTimer::timeout,this,[this]{
        if(held_&&!directions[axis_*2+(direction_>0?1:0)]->isDown()){halt();return;}
    });repeat.start();
    connect(qApp,&QGuiApplication::applicationStateChanged,this,[this](Qt::ApplicationState state){if(state!=Qt::ApplicationActive)halt(true);});
    qApp->installEventFilter(this);
    QFile record(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/plc_manual_origin.json");
    if(record.open(QIODevice::ReadOnly))originRecord=QJsonDocument::fromJson(record.readAll()).object();
    refresh();
}
void ManualBridgePanel::setModeEnabled(bool e){if(mode_==e)return;if(!e)halt(true);mode_=e;manual.setModeEnabled(e);refresh();}
bool ManualBridgePanel::engaged() const{return manual.uncertain()||manual.pending()||manual.targetActive()||manual.status().has(ManualProto::Entered)||manual.status().has(ManualProto::Busy)||manual.status().has(ManualProto::Stopping)||held_||returning_;}
bool ManualBridgePanel::safeToClose() const{return !engaged()||(manual.fresh()&&bridge.fresh()&&bridge.status().stopped()&&!manual.pending()&&!manual.status().has(ManualProto::Stopping));}
void ManualBridgePanel::resizeEvent(QResizeEvent* event){
    QWidget::resizeEvent(event);const bool narrow=event->size().width()<1380;
    if(!manualBody||!setupGrid||narrow==narrowLayout)return;narrowLayout=narrow;
    manualBody->removeWidget(homeBox_);manualBody->addWidget(homeBox_,narrow?1:0,narrow?0:2,1,narrow?2:1,Qt::AlignTop);
    manualBody->setColumnStretch(0,1);manualBody->setColumnStretch(1,narrow?1:2);manualBody->setColumnStretch(2,narrow?0:1);
    const QList<QPushButton*> buttons{load,confirm,enable,enter};
    for(auto* button:buttons)setupGrid->removeWidget(button);
    for(int i=0;i<buttons.size();++i)setupGrid->addWidget(buttons[i],0,i);
    for(int i=0;i<4;++i)setupGrid->setColumnStretch(i,1);
}
void ManualBridgePanel::halt(bool leave){held_=false;returning_=false;manual.cancel(leave);}
bool ManualBridgePanel::eventFilter(QObject* watched,QEvent* event){
    if((held_||returning_||manual.pending()||manual.targetActive())&&
       ((watched==window()&&event->type()==QEvent::WindowDeactivate)||
        (watched==this&&event->type()==QEvent::Hide)||
        (held_&&watched==directions[axis_*2+(direction_>0?1:0)]&&
         (event->type()==QEvent::UngrabMouse||event->type()==QEvent::FocusOut||event->type()==QEvent::Leave))))halt(true);
    return QWidget::eventFilter(watched,event);
}
void ManualBridgePanel::nextStep(){
    if(submitting_)return;submitting_=true;
    bool sent=false;
    if(returning_){
        if(manual.status().has(ManualProto::AtHome)){returning_=false;submitting_=false;return;}
        sent=manual.issue(ManualProto::Return,21,qint16(manual.status().origin),quint16(std::lround(returnSpeed->value()*100)));
    }else{
        const int action=(frame->currentIndex()==0?1:11)+axis_;
        sent=manual.issue(ManualProto::Step,quint16(action),qint16(direction_*std::lround(distance->value()*1000)),quint16(std::lround(speed->value()*(frame->currentIndex()==0?100:10))));
    }
    if(!sent){held_=false;returning_=false;}submitting_=false;
}
void ManualBridgePanel::moveTarget(){
    if(!manual.ready()||manual.pending()||manual.targetActive())return;
    std::array<std::optional<double>,6> values;
    QStringList fields;const bool cartesian=targetFrame->currentIndex()==1;
    for(int i=0;i<6;++i)if(targetSelected[i]->isChecked()){
        values[i]=targetValues[i]->value();
        fields<<QString("%1 = %2").arg(cartesian?QStringList{"X","Y","Z","A","B","C"}.at(i):QString("J%1").arg(i+1)).arg(*values[i],0,'f',3);
    }
    if(fields.isEmpty())return;
    const auto text=QString("%1\n%2\n未勾选分量保持本次请求开始时的位置。\n速度 %3 %4；工具 Default。\n确认目标与整段路径后开始？")
        .arg(cartesian?"WORLD 绝对坐标与欧拉角，TCP 直线运动":"绝对关节角，PTP 关节运动")
        .arg(fields.join("\n")).arg(targetSpeed->value(),0,'f',2).arg(cartesian?"mm/s（姿态 °/s）":"%");
    if(QMessageBox::question(this,"移动到指定位置",text,QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes)return;
    manual.moveTarget(cartesian,values,quint16(std::lround(targetSpeed->value()*(cartesian?10:100))));refresh();
}
void ManualBridgePanel::fillCurrentTarget(){
    if(!manual.positionFresh()||manual.pending()||manual.targetActive()||held_||returning_)return;
    const auto position=manual.position();const int offset=targetFrame->currentIndex()==1?6:0;
    for(int i=0;i<6;++i){
        const double value=position.values[offset+i];
        if(value<targetValues[i]->minimum()||value>targetValues[i]->maximum()){
            emit notice("当前位置超出目标输入范围，本次未填入。");return;
        }
    }
    for(int i=0;i<6;++i){
        targetValues[i]->setValue(position.values[offset+i]);
        QSignalBlocker guard(targetSelected[i]);targetSelected[i]->setChecked(false);
    }
    emit notice("已填入当前停稳位置；勾选并修改要移动的分量，再点击移动。填入操作不会运动。");refresh();
}
void ManualBridgePanel::capture(){
    if(QMessageBox::question(this,"捕捉实验起点","将当前关节及Default/WORLD位姿保存为新的实验起点，替换本服务此前保存的起点。确认？",QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes)return;
    newOrigin_=quint16(QRandomGenerator::global()->bounded(1,1000));
    if(newOrigin_==manual.status().origin)newOrigin_=quint16(newOrigin_==999?1:newOrigin_+1);
    manual.issue(ManualProto::Capture,20,qint16(newOrigin_),0);
}
void ManualBridgePanel::returnHome(){
    if(!manual.status().has(ManualProto::HomeSaved)||!manual.status().origin)return;
    if(QMessageBox::question(this,"返回实验起点",QString("将以 %1% 关节速度执行一次 PTP，返回保存的起点。确认回程路径并开始？").arg(returnSpeed->value(),0,'f',2),QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes)return;
    returning_=true;nextStep();
}
void ManualBridgePanel::saveOrigin(){
    originRecord={{"id",newOrigin_},{"name",experiment->text().trimmed()},{"plc",bridge.endpointHost()},{"saved_at",QDateTime::currentDateTime().toString(Qt::ISODate)}};
    const auto folder=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);QDir().mkpath(folder);
    QSaveFile file(folder+"/plc_manual_origin.json");
    if(!file.open(QIODevice::WriteOnly)||file.write(QJsonDocument(originRecord).toJson())<0||!file.commit())emit notice("柜内起点已完成，但本机试次备注保存失败。");
}
void ManualBridgePanel::refresh(){
    const auto s=manual.status();QString why;const bool ready=mode_&&manual.ready(&why);const bool idle=!manual.pending()&&!s.has(ManualProto::Busy)&&!s.has(ManualProto::Stopping);
    if((held_||returning_)&&(!ready||!isVisible())){held_=false;returning_=false;manual.cancel(true);}
    const bool moving=held_||returning_||manual.pending()||manual.targetActive()||s.has(ManualProto::Busy);
    const bool live=manual.fresh(),mainLive=bridge.fresh(),entered=live&&s.has(ManualProto::Entered);
    const auto b=bridge.status();const bool selected=live&&mainLive&&b.selectedSlot==s.slot&&b.has(BridgeProto::LoadConfirmed);
    const bool powered=mainLive&&b.has(BridgeProto::RobotOnline)&&b.enabled();
    QString sessionWhy;
    WorkstationUi::button(registerSession,mode_&&bridge.canIssue(BridgeProto::Register,0,&sessionWhy),sessionWhy,bridge.ownsSession()?"done":"next");
    registerSession->setText(bridge.ownsSession()?"登记 ✓":"登记");
    WorkstationUi::button(logoutSession,mode_&&!engaged()&&bridge.canIssue(BridgeProto::Logout,0,&sessionWhy),engaged()?"先停止并退出微调服务":sessionWhy);
    sessionState->setText(bridge.ownsSession()?"已登记 · 与自动运行共用本机会话":mainLive?"连接后在此登记本机控制权":"请先连接 PLC");
    QString recoveryWhy;
    const bool canAcknowledge=mode_&&bridge.canIssue(BridgeProto::ClearError,0,&recoveryWhy);
    WorkstationUi::button(acknowledge,canAcknowledge,recoveryWhy,mainLive&&b.alarm()?"next":"");
    const bool canRearm=mode_&&bridge.canIssue(BridgeProto::Rearm,0,&recoveryWhy);
    WorkstationUi::button(rearm,canRearm,recoveryWhy,mainLive&&b.has(BridgeProto::StopLatch)&&!b.alarm()?"next":"");
    recoveryStatus->setText(!mainLive?"连接并登记后可确认报警、复位锁存。":
        QString("报警：%1 · 急停：%2 · 使能：%3 · 锁存：%4")
            .arg(b.alarm()?"有":"无").arg(b.emergency()?"有":"无").arg(b.enabled()?"开":"关").arg(b.has(BridgeProto::StopLatch)?"有":"无"));
    recoveryStatus->setToolTip("恢复顺序：确认报警 → 复位锁存 → 重新加载、确认、使能。");
    const int nextSetup=!selected?(bridge.canIssue(BridgeProto::ConfirmLoad,s.slot)?BridgeProto::ConfirmLoad:BridgeProto::Load):!powered?BridgeProto::Enable:0;
    const auto setupButton=[&](QPushButton* button,BridgeProto::Command command,const QString& title,bool done,quint16 parameter=0){
        QString reason;const bool allowed=bridge.canIssue(command,parameter,&reason);
        const bool busy=mainLive&&bridge.pending()&&bridge.currentCommand()==command;
        const bool usable=mode_&&live&&s.slot&&idle&&!engaged()&&allowed;
        if(!mode_)reason="请选择 PLC 连接方式";else if(!mainLive)reason="请先连接 PLC";else if(!live||!s.slot)reason="等待有效微调服务状态与固定程序号";else if(engaged())reason="请先停止并退出当前微调服务";else if(allowed)reason="可操作，等待 PLC 应用回执";
        button->setText(busy?title+" · 等待":done?title+" ✓":title);
        WorkstationUi::button(button,usable,reason,busy?"busy":done?"done":usable&&command==nextSetup?"next":"");
    };
    setupButton(load,BridgeProto::Load,live?QString("1  加载 %1 号微调程序").arg(s.slot):"1  加载微调程序",selected,s.slot);
    setupButton(confirm,BridgeProto::ConfirmLoad,"2  人工确认加载…",selected,s.slot);
    setupButton(enable,BridgeProto::Enable,"3  使能",powered);
    enter->setText(entered?"4  已进入微调 ✓":manual.pending()&&!entered?"4  进入微调 · 等待":"4  进入微调");
    WorkstationUi::button(enter,mode_&&manual.ready(nullptr,false)&&idle&&!entered,why,entered?"done":manual.pending()?"busy":"next");
    WorkstationUi::button(reconnect,mode_&&bridge.isConnected()&&!live,"连接 PLC 后，可手动重连微调扩展通道");
    // Keep the held button enabled: disabling a pressed QPushButton loses release delivery.
    for(int i=0;i<directions.size();++i)directions[i]->setEnabled(ready&&(!moving||(held_&&i==axis_*2+(direction_>0?1:0))));
    single->setEnabled(ready&&idle&&!moving);captureButton->setEnabled(ready&&idle&&!moving);returnButton->setEnabled(ready&&idle&&!moving&&s.has(ManualProto::HomeSaved)&&s.origin);
    frame->setEnabled(!moving);distance->setEnabled(!moving);multiplier->setEnabled(!moving);speed->setEnabled(!moving);speedPreset->setEnabled(!moving);returnSpeed->setEnabled(!moving);
    targetFrame->setEnabled(!moving);targetSpeed->setEnabled(!moving);
    bool anyTarget=false;const bool cartesian=targetFrame->currentIndex()==1;
    targetSpeed->setMaximum(cartesian?3276.7:100);targetSpeed->setDecimals(cartesian?1:2);targetSpeed->setSuffix(cartesian?" mm/s / °/s":" %");
    const bool positionLive=manual.positionFresh();const auto position=manual.position();
    for(int i=0;i<6;++i){
        anyTarget|=targetSelected[i]->isChecked();targetSelected[i]->setEnabled(!moving);
        const double limit=cartesian&&i<3?10000:360;targetValues[i]->setRange(-limit,limit);
        targetValues[i]->setEnabled(!moving&&targetSelected[i]->isChecked());
        targetLabels[i]->setText(cartesian?QStringList{"X · mm","Y · mm","Z · mm","A · °","B · °","C · °"}.at(i):QString("J%1 · °").arg(i+1));
        targetCurrent[i]->setText(positionLive?QString::number(position.values[(cartesian?6:0)+i],'f',3):"—");
        jointCurrent[i]->setText(positionLive?QString::number(position.values[i],'f',3)+" °":"—");
        worldCurrent[i]->setText(positionLive?QString::number(position.values[i+6],'f',3)+(i<3?" mm":" °"):"—");
    }
    positionState->setText(positionLive?QString("停稳位置 · %1 秒前更新").arg(position.age/1000.0,0,'f',1):
        !live?"连接并进入微调后，自动读取当前位置。":s.raw[29]!=0x0603?"需要配套 R6.3 PLC 和柜内工程。":
        moving?"运动或请求处理中；当前位置参考暂不可用。":"等待完整位置快照；进入微调并停稳后约 1～3 秒更新。");
    WorkstationUi::button(targetFillCurrent,positionLive&&!moving,"只填入目标输入框，不会运动；填入后再勾选要修改的分量");
    WorkstationUi::button(targetMove,ready&&idle&&!moving&&anyTarget,anyTarget?why:"先勾选要修改的分量");
    targetHint->setText(cartesian?"工具 Default · WORLD。XYZ 单位 mm；ABC 为欧拉角 °；TCP 直线路径。速度 mm/s，姿态速度 °/s。":"关节角单位 °，速度为额定关节速度 %。只勾选一个关节即可单独设定该轴的绝对角度。");
    WorkstationUi::button(stop,mode_&&engaged(),"撤销后续微调或回程，等待 PLC 确认停止");
    WorkstationUi::button(exit,mode_&&engaged(),"停止并退出微调服务");
    for(int i=0;i<6;++i){if(auto* l=findChild<QLabel*>(QString("manualAxis%1").arg(i))){const auto axis=frame->currentIndex()==0?QString("J%1").arg(i+1):QStringList{"X","Y","Z","A","B","C"}.at(i);l->setText(axis);l->setToolTip(axis+(frame->currentIndex()==0||i>=3?" · °":" · mm"));for(int d=0;d<2;++d)directions[i*2+d]->setAccessibleName(axis+QString(d?" 正方向按住移动":" 负方向按住移动"));}}
    distance->setSuffix(frame->currentIndex()==0?" °":" mm / °");speed->setSuffix(frame->currentIndex()==0?" %":" mm/s / °/s");
    const double heldSegment=(frame->currentIndex()==0?ManualProto::jointJogLimit(int(std::lround(speed->value()*100))):ManualProto::worldJogLimit(int(std::lround(speed->value()*10))))/1000.;
    speedHint->setText(QString("按住 ± 每段 %1 %2，松开后完成当前段停。")
        .arg(heldSegment,0,'f',3).arg(frame->currentIndex()==0?"°":"mm / °"));
    units->setText(frame->currentIndex()==0?"单步走多远：角度 °；移动有多快：额定关节速度 %。初次调整可用 0.1°、速度 0.50%。":"单步走多远：XYZ 为 mm，ABC 为 °；速度：XYZ 为 mm/s，ABC 为 °/s。Default 工具、WORLD 坐标。");
    speedHint->setToolTip(units->text());
    WorkstationUi::badgeState(channelBadge,live?"微调通道 · 已验证":"微调通道 · 未验证",live?"success":"idle");
    WorkstationUi::badgeState(serviceBadge,live?QString("%1号服务 · %2").arg(s.slot).arg(entered?"已进入":"未进入"):"服务 · 未知",entered?"success":"idle");
    WorkstationUi::badgeState(originBadge,!live?"起点 · 未知":s.has(ManualProto::AtHome)?"起点 · 已到达":s.has(ManualProto::HomeSaved)?"起点 · 已保存":entered?"起点 · 未捕捉":"起点 · 待服务确认",live&&s.has(ManualProto::HomeSaved)?"success":"idle");
    QString instruction,state="idle";
    if(!mode_)instruction="请选择顶部的 AC802 PLC 连接方式。";
    else if(!mainLive)instruction="先在顶部连接 PLC，再点击本页「登记」。";
    else if(!bridge.ownsSession())instruction="下一步：点击本页上方的「登记」。";
    else if(!live){instruction="等待微调扩展状态验证；如未恢复，展开下方诊断，重连微调通道。";state="warn";}
    else if(manual.uncertain()){instruction=why;state="error";}
    else if(s.has(ManualProto::Stopping)){instruction="已请求停止，等待 PLC 确认停稳。";state="warn";}
    else if(bridge.pending()){instruction="正在等待 PLC 回执："+BridgeProto::commandText(bridge.currentCommand())+"。";state="warn";}
    else if(returning_){instruction="正在返回实验起点；可点击红色停止按钮中止。";state="busy";}
    else if(held_){instruction=QString("正在按住移动 · 每段 %1 %2；松开后完成当前段停下。").arg(heldSegment,0,'f',3).arg(frame->currentIndex()==0?"°":"mm / °");state="busy";}
    else if(manual.targetActive()){instruction=manual.targetProgress()<7?QString("正在传输指定位置参数 %1 / 7；尚未发出运动指令。").arg(manual.targetProgress()):"正在执行指定位置运动；可点击红色停止按钮中止。";state="busy";}
    else if(manual.pending()||s.has(ManualProto::Busy)){instruction="动作处理中，等待完成回执。";state="warn";}
    else if(!selected){instruction=confirm->isEnabled()?"加载请求已发送。请核对示教器中的 manual_service，再点击「人工确认加载」。":load->isEnabled()?"下一步：加载本页显示的固定微调程序号。":why;}
    else if(!powered){instruction=enable->isEnabled()?"下一步：点击「使能」，再进入微调。":why;}
    else if(!entered){instruction=enter->isEnabled()?"下一步：点击「进入微调」，随后可使用方向按钮。":why;}
    else if(ready){instruction="微调已就绪。按住方向按钮逐步移动，或选择轴与方向后单步移动。";state="success";}
    else{instruction=why;state="warn";}
    health->setText(instruction);WorkstationUi::tone(health,state);
    setupHint->setText(live?QString("固定服务程序号 %1 · 工具 Default · WORLD；微调通道不会自动进入或运动。").arg(s.slot):"等待 PLC 微调扩展签名与动态扫描心跳。");
    receipt->setText(QString("阶段%1 · 请求%2 · 结果%3 · %4 · 柜内接受%5 / 完成%6%7").arg(s.phase).arg(s.ack).arg(s.result).arg(ManualProto::errorText(s.error)).arg(s.raw[11]).arg(s.raw[12]).arg(s.has(ManualProto::Stopping)?" · 等待停稳":""));
    const bool associated=originRecord.value("id").toInt()==s.origin&&originRecord.value("plc").toString()==bridge.endpointHost();
    origin->setText(!live?"连接后显示柜内保存的起点状态。":s.has(ManualProto::HomeSaved)?QString("起点编号 %1\n%2%3").arg(s.origin).arg(associated?originRecord.value("name").toString():"未关联本机试次备注").arg(s.has(ManualProto::AtHome)?"\n已回到起点":""):entered?"尚未捕捉实验起点。":"进入10号服务后确认柜内实验起点。");
    QStringList xyz;for(int i=0;i<3;++i){const auto v=mainLive?b.coordinate(i):std::optional<float>{};xyz<<(v?QString::number(*v,'f',3):"—");}
    coordinates->setText("TCP 监视 · X / Y / Z\n"+xyz.join(" / ")+"\n六轴当前位置由柜内在执行前读取。");
    const QString blocked=!ready?why:!idle||moving?"等待当前动作完成":"可操作";
    single->setToolTip(blocked);captureButton->setToolTip(blocked);returnButton->setToolTip(!s.has(ManualProto::HomeSaved)?"请先捕捉实验起点":blocked);
    for(auto* button:directions)button->setToolTip(ready?QString("按住分段移动，每段 %1；松开完成当前段后停，另有通信延迟。").arg(heldSegment,0,'f',3):blocked);
}
