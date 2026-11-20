#include "workstation_welder.h"
#include "workstation_ui.h"
#include <limits>
#include <cmath>

namespace {
quint32 pair(const QVector<quint16>& w,int n){return (quint32(w[n])<<16)|w[n+1];}
void put(QVector<quint16>& w,int n,quint32 v){w[n]=quint16(v>>16);w[n+1]=quint16(v);}
}
namespace WelderProto {
const std::array<Parameter,18>& parameters(){
    static const std::array<Parameter,18> specs{{
        {"操作方式","",0,2,1,0},{"焊接模式","",0,4,1,1},
        {"给定电流","A",40,4000,10,1000},{"热丝频率","Hz",20,100,1,50},
        {"热丝给定电流","A",0,200,1,150},{"前气时间","s",1,999,100,1},
        {"延气时间","s",1,600,10,2},{"电流爬升时间","s",1,100,10,5},
        {"电流衰减时间","s",1,150,10,2},{"脉冲频率","Hz",2,200,10,5},
        {"脉冲占空比","%",1,999,10,500},{"峰值电流","A",50,4000,10,1000},
        {"基值电流","A",50,4000,10,300},{"起弧电流","A",50,4000,10,50},
        {"收弧电流","A",50,4000,10,50},{"交流频率","Hz",40,250,1,40},
        {"清理比例设定","",90,180,1,90},{"交流偏置设定","",90,180,1,90}
    }};return specs;
}
QVector<quint16> frame(Operation op,quint32 sequence,quint32 owner,quint32 boot,quint16 controls,const QVector<quint16>& params){
    QVector<quint16> w(CommandWords,0);w[0]=Magic;w[1]=Version;put(w,2,sequence);put(w,4,owner);put(w,6,boot);w[8]=op;w[9]=controls;
    for(int i=0;i<std::min<int>(ParameterCount,params.size());++i)w[10+i]=params[i];
    quint16 checksum=0xa55a;for(int i=0;i<31;++i)checksum^=w[i];w[31]=checksum;return w;
}
QString errorText(int code){
    switch(code){case 0:return "无错误";case 1:return "PLC 许可、心跳、PDO 或机器人联锁未通过";case 2:return "无效控制位";
    case 3:return "起弧/热丝需要外部自动且未暂停";case 4:return "先打开准备信号，并等待焊机就绪、无报警";
    case 5:return "检气期间不能起弧";case 6:return "先关闭全部工艺输出，再应用参数";case 7:return "参数超出协议范围";default:return "无效操作";}
}
}
WelderClient::WelderClient(PlcBridgeClient& b,QObject* parent):QObject(parent),bridge(b),wire(this){
    wire.setPollRange(3,WelderProto::StatusBase,WelderProto::StatusWords);wire.setPollInterval(200);
    connect(&wire,&ModbusClient::registersRead,this,&WelderClient::receive);
    connect(&wire,&ModbusClient::linkChanged,this,[this](bool up,const QString&){if(!up){valid_=advanced_=false;if(pending_){pending_=0;uncertain_=true;emit notice("焊机连接中断；停止续租，输出结果待 PLC 确认");}}emit changed();});
    timer.setInterval(100);connect(&timer,&QTimer::timeout,this,&WelderClient::tick);timer.start();
}
void WelderClient::setEnabled(bool on){
    if(enabled_==on)return;enabled_=on;
    if(!on){wire.disconnectFrom();valid_=advanced_=false;heartbeatAge.invalidate();}else retryAge.invalidate();
    emit changed();
}
bool WelderClient::fresh() const{return enabled_&&wire.isConnected()&&valid_&&advanced_&&packetAge.isValid()&&packetAge.elapsed()<1500&&scanAge.isValid()&&scanAge.elapsed()<1500&&bridge.fresh()&&pair(status_,4)==bridge.status().boot;}
bool WelderClient::ready(QString* why) const{
    const auto no=[why](const QString& text){if(why)*why=text;return false;};
    if(!fresh())return no("等待焊机 PLC 扩展签名、版本与扫描心跳；尚未安装时仅显示离线");
    if(!bridge.ownsSession()||pair(status_,14)!=bridge.sessionId())return no("先在手动页或自动页登记本机控制权");
    if(bridge.operationInhibited())return no("机器人模式正在切换");
    if(uncertain_)return no("上次工艺请求结果待确认；请关闭全部工艺输出并等待回读");
    if(pending_)return no("等待焊机 PLC 回执");
    if((status_[3]&2)==0)return no("PLC 尚未确认焊机 PDO 映射");
    if((status_[3]&4)==0)return no("焊机 EtherCAT 从站未进入有效 OP 状态");
    if((status_[3]&64)==0)return no("PLC 本地焊接许可尚未开放");
    if((status_[3]&8)==0)return no("等待焊机心跳与机器人许可；检查停止锁存、报警和急停");
    if(why)why->clear();return true;
}
bool WelderClient::safeIdle() const{return !seenInstalled_||(!pending_&&!uncertain_&&fresh()&&status_[10]==0);}
void WelderClient::tick(){
    if(!enabled_||!bridge.isConnected()){if(wire.isActive())wire.disconnectFrom();return;}
    if(!wire.isActive()&&(!retryAge.isValid()||retryAge.elapsed()>5000)){retryAge.restart();wire.connectTo(bridge.endpointHost(),bridge.endpointPort(),bridge.endpointUnit());}
    // Expire an unacknowledged request before deciding whether to renew its lease.
    if(pending_&&commandAge.elapsed()>3000){pending_=0;uncertain_=true;emit notice("焊机操作回执超时；停止续租，不自动重发；请关闭全部工艺输出");emit changed();}
    if(fresh()&&bridge.ownsSession()&&pair(status_,14)==bridge.sessionId()&&!uncertain_&&(!heartbeatAge.isValid()||heartbeatAge.elapsed()>=250)){
        wire.writeRegister(WelderProto::Heartbeat,++heartbeat_);heartbeatAge.restart();
    }
    if(valid_&&!fresh())emit changed();
}
void WelderClient::receive(int fc,int start,const QVector<quint16>& w,qint64){
    if(fc!=3||start!=WelderProto::StatusBase||w.size()!=WelderProto::StatusWords)return;
    const bool valid=w[0]==WelderProto::Magic&&w[1]==WelderProto::Version&&(w[3]&1)&&pair(w,4)!=0;
    if(!valid){valid_=advanced_=false;emit changed();return;}
    if(!valid_||pair(w,4)!=pair(status_,4)){advanced_=false;scanAge.restart();}
    else if(w[2]!=status_[2]){advanced_=true;scanAge.restart();}
    status_=w;valid_=true;seenInstalled_=true;packetAge.restart();
    if(pending_&&fresh()&&pair(w,14)==bridge.sessionId()&&pair(w,6)==pending_&&w[13]==operation_){
        pending_=0;uncertain_=false;emit notice(w[8]==1?"PLC 已应用焊机请求；实际引弧与电流以反馈为准":"PLC 拒绝焊机请求："+WelderProto::errorText(w[9]));
    }
    emit changed();
}
bool WelderClient::issue(WelderProto::Operation op,quint16 controls,const QVector<quint16>& params){
    QString why;
    if(op==WelderProto::AllOff){if(!fresh()||!bridge.ownsSession()||pair(status_,14)!=bridge.sessionId()){emit notice("关闭输出需要有效焊机桥与本机会话；失联时 PLC 看门狗会撤销输出");return false;}}
    else if(!ready(&why)){emit notice(why);return false;}
    if(op==WelderProto::Controls){
        if(controls&~15u)return false;
        if((controls&9)&&((status_[3]&32)==0||(controls&2)==0||(status_[11]&0x82)!=2)){emit notice("起弧/热丝需要外部自动、准备信号、焊机就绪且无报警");return false;}
        if((controls&5)==5){emit notice("先关闭检气再起弧");return false;}
    }
    if(op==WelderProto::Parameters){
        if(status_[10]!=0||params.size()!=20)return false;
        const auto& specs=WelderProto::parameters();for(int i=0;i<18;++i)if(params[i]<specs[i].minimum||params[i]>specs[i].maximum)return false;
        if(params[18]>7||params[19]!=0)return false;
    }
    if(sequence_==std::numeric_limits<quint32>::max()){emit notice("焊机请求编号已用尽，请重新启动并登记");return false;}
    pending_=++sequence_;operation_=op;commandAge.restart();
    wire.writePriorityRegisters(WelderProto::CommandBase,WelderProto::frame(op,pending_,bridge.sessionId(),bridge.status().boot,controls,params),WelderProto::CommandBase,WelderProto::CommandWords);
    emit changed();return true;
}
WelderPanel::WelderPanel(PlcBridgeClient& b,QWidget* parent):QWidget(parent),bridge(b),welder(b,this){
    setObjectName("welderPanel");auto* layout=WorkstationUi::page(this,"welderScroll");pageLayout_=layout;layout->parentWidget()->setMaximumWidth(1780);layout->setSpacing(8);
    auto* title=new QLabel("焊机工艺");title->setProperty("heading",true);layout->addWidget(title);
    layout->addWidget(WorkstationUi::hint("复用原工程的奥太 EtherCAT 协议。此页手动控制工艺，当前 G 代码作业保持空跑；不会自动随轨迹起弧。"));
    state=WorkstationUi::hint();state->setObjectName("welderState");state->setProperty("banner",true);layout->addWidget(state);
    body=new QGridLayout;layout->addLayout(body);body->setColumnStretch(0,1);body->setColumnStretch(1,1);
    auto* statusBox=new QGroupBox("工艺操作与反馈");controlsBox=statusBox;statusBox->setObjectName("welderControls");statusBox->setMinimumWidth(320);auto* actions=new QVBoxLayout(statusBox);actions_=actions;body->addWidget(statusBox,0,0,Qt::AlignTop);
    poseHost_=new QWidget;poseHost_->setObjectName("welderPoseHost");auto* poseLayout=new QVBoxLayout(poseHost_);poseLayout->setContentsMargins(0,8,0,0);body->addWidget(poseHost_,0,1,Qt::AlignTop);
    actual=new QLabel;actual->setObjectName("welderActual");actual->setWordWrap(true);actions->addWidget(actual);
    fault=WorkstationUi::hint();fault->setObjectName("welderFault");actions->addWidget(fault);
    output=WorkstationUi::hint();output->setObjectName("welderOutputs");actions->addWidget(output);
    const QStringList names{"起弧","焊机准备","检气","热丝电源"};
    for(int i:{1,2,3,0}){auto* button=new QPushButton(names[i]);button->setObjectName(QString("welderControl%1").arg(i));button->setMinimumHeight(38);controls.append(button);button->setProperty("controlBit",i);actions->addWidget(button);connect(button,&QPushButton::clicked,this,[this,i]{toggle(i);});}
    off=new QPushButton("关闭全部工艺输出");off->setObjectName("welderAllOff");off->setProperty("danger",true);off->setMinimumHeight(42);actions->addWidget(off);connect(off,&QPushButton::clicked,this,[this]{welder.issue(WelderProto::AllOff);});
    outputHint_=WorkstationUi::hint("准备信号对应原程序的「焊机开机」。停止机器人、通信失效、PDO 故障或撤销许可会停弧/关热丝；恢复后需重新点击。");actions->addWidget(outputHint_);actions->addStretch();
    parameterBox=new QGroupBox("焊接参数");parameterBox->setObjectName("welderParameters");auto* parameterLayout=new QVBoxLayout(parameterBox);body->addWidget(parameterBox,1,0,1,2,Qt::AlignTop);
    parameterScroll=new QScrollArea;parameterScroll->setObjectName("welderParameterScroll");parameterScroll->setWidgetResizable(true);parameterScroll->setFrameShape(QFrame::NoFrame);parameterScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);parameterScroll->setFixedHeight(350);parameterLayout->addWidget(parameterScroll);
    auto* parameterContent=new QWidget;parameterScroll->setWidget(parameterContent);parameterGrid=new QGridLayout(parameterContent);parameterGrid->setContentsMargins(0,0,6,0);parameterGrid->setColumnStretch(1,1);parameterGrid->setColumnStretch(3,1);
    operation=new QComboBox;operation->setObjectName("welderOperation");operation->addItems({"两步","四步","反复"});
    work=new QComboBox;work->setObjectName("welderWorkMode");work->addItems({"直流恒流","直流脉冲","交流恒流","交流脉冲","直流手弧"});
    const auto& specs=WelderProto::parameters();
    parameterEditors<<operation<<work;
    for(int i=2;i<18;++i){const auto& spec=specs[i];auto* spin=new QDoubleSpinBox;spin->setObjectName(QString("welderParameter%1").arg(i));spin->setRange(double(spec.minimum)/spec.scale,double(spec.maximum)/spec.scale);spin->setDecimals(spec.scale==100?2:spec.scale==10?1:0);spin->setSingleStep(1.0/spec.scale);spin->setValue(double(spec.initial)/spec.scale);spin->setSuffix(QString(" ")+spec.unit);values<<spin;parameterEditors<<spin;}
    for(int i=0;i<18;++i){auto* label=new QLabel(specs[i].name);parameterLabels<<label;parameterGrid->addWidget(label,i/2,(i%2)*2);parameterGrid->addWidget(parameterEditors[i],i/2,(i%2)*2+1);}
    hotAc=new QCheckBox("热丝交流");liftArc=new QCheckBox("提升引弧（关闭高频）");airCooling=new QCheckBox("气冷焊枪");
    auto* auxiliary=new QVBoxLayout;auxiliary_=auxiliary;auxiliary->setSpacing(4);for(auto* check:{hotAc,liftArc,airCooling})auxiliary->addWidget(check);parameterLayout->addLayout(auxiliary);
    auto* paramActions=new QHBoxLayout;read=new QPushButton("读取 PLC 参数");read->setObjectName("welderRead");apply=new QPushButton("应用全部参数");apply->setObjectName("welderApply");paramActions->addWidget(read);paramActions->addWidget(apply);parameterLayout->addLayout(paramActions);
    parameterHint_=WorkstationUi::hint("参数仅在全部工艺输出关闭时写入。应用后以 PLC 回执和重新读取的参数为准。清理比例、交流偏置沿用旧工程设定值，协议值为 130−设定值。");parameterLayout->addWidget(parameterHint_);parameterLayout->addStretch();
    connect(read,&QPushButton::clicked,this,&WelderPanel::readParameters);
    connect(apply,&QPushButton::clicked,this,[this]{QVector<quint16> p(20,0);p[0]=operation->currentIndex();p[1]=work->currentIndex();const auto& specs=WelderProto::parameters();for(int i=2;i<18;++i)p[i]=quint16(std::lround(values[i-2]->value()*specs[i].scale));p[18]=(hotAc->isChecked()?1:0)|(liftArc->isChecked()?2:0)|(airCooling->isChecked()?4:0);welder.issue(WelderProto::Parameters,0,p);});
    log=new QPlainTextEdit;log->setObjectName("welderLog");log->setReadOnly(true);log->setMaximumBlockCount(100);log->setMinimumHeight(80);log->setMaximumHeight(130);layout->addWidget(log);
    connect(&welder,&WelderClient::changed,this,&WelderPanel::refresh);connect(&bridge,&PlcBridgeClient::changed,this,&WelderPanel::refresh);
    connect(&welder,&WelderClient::notice,this,[this](const QString& text){log->appendPlainText(QTime::currentTime().toString("HH:mm:ss ")+text);});layout->addStretch();refresh();
}
void WelderPanel::resizeEvent(QResizeEvent* event){
    QWidget::resizeEvent(event);if(!printWorkspace_)layoutWorkspace();
}
void WelderPanel::layoutWorkspace(){
    const bool compact=width()<1380;if(!body)return;compactLayout=compact;
    body->removeWidget(parameterBox);body->addWidget(parameterBox,compact?1:0,compact?0:2,1,compact?2:1,Qt::AlignTop);
    body->setColumnStretch(0,1);body->setColumnStretch(1,compact?1:2);body->setColumnStretch(2,compact?0:1);
    layoutParameters(compact);
}
void WelderPanel::layoutParameters(bool compact){
    for(int i=0;i<18;++i){parameterGrid->removeWidget(parameterLabels[i]);parameterGrid->removeWidget(parameterEditors[i]);}
    parameterGrid->setColumnStretch(3,compact?1:0);
    for(int i=0;i<18;++i){const int row=compact?i/2:i,col=compact?(i%2)*2:0;parameterGrid->addWidget(parameterLabels[i],row,col);parameterGrid->addWidget(parameterEditors[i],row,col+1);}
    parameterScroll->setFixedHeight(compact?350:440);
}
void WelderPanel::setPrintHosts(QVBoxLayout* controls,QVBoxLayout* parameters,QVBoxLayout* logHost){
    const bool active=controls&&parameters;
    if(active){controls->addWidget(state);controls->addWidget(controlsBox);parameters->addWidget(parameterBox);(logHost?logHost:parameters)->addWidget(log);setPrintLayout(true);}
    else if(printWorkspace_){setPrintLayout(false);pageLayout_->insertWidget(2,state);body->addWidget(controlsBox,0,0,Qt::AlignTop);pageLayout_->insertWidget(4,log);layoutWorkspace();}
    printWorkspace_=active;
    for(auto* widget:{static_cast<QWidget*>(state),static_cast<QWidget*>(controlsBox),static_cast<QWidget*>(log),static_cast<QWidget*>(parameterBox)})widget->show();
}
void WelderPanel::setPrintLayout(bool on){
    const QString style=on?"QGroupBox {padding:7px 6px 6px;margin-top:10px;} QLabel,QCheckBox,QComboBox,QDoubleSpinBox {font-size:10pt;} QPushButton {font-size:10pt;padding:4px 7px;} QPushButton:focus {padding:3px 6px;} QComboBox {padding:1px 5px;min-height:17px;} QDoubleSpinBox {padding:1px 26px 1px 5px;min-height:17px;}":"";
    controlsBox->setStyleSheet(style);parameterBox->setStyleSheet(style);controlsBox->setMinimumWidth(on?224:320);
    controlsBox->setToolTip(on?outputHint_->text():QString());parameterBox->setToolTip(on?parameterHint_->text():QString());
    outputHint_->setVisible(!on);parameterHint_->setVisible(!on);actions_->setSpacing(on?3:6);actions_->setContentsMargins(on?8:11,on?8:11,on?8:11,on?8:11);
    if(on){
        if(!printOutputs_){printOutputs_=new QGridLayout;printOutputs_->setSpacing(4);actions_->insertLayout(3,printOutputs_);}
        for(int i=0;i<controls.size();++i){controls[i]->setMinimumHeight(28);printOutputs_->addWidget(controls[i],i,0);}
        printOutputs_->addWidget(off,controls.size(),0);off->setMinimumHeight(30);
        layoutParameters(false);parameterGrid->setVerticalSpacing(2);parameterGrid->setHorizontalSpacing(10);
        printEditorBaseHeight_=21;for(auto* editor:parameterEditors)editor->setFixedHeight(printEditorBaseHeight_);
        parameterScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);parameterScroll->verticalScrollBar()->setValue(0);
        parameterScroll->widget()->ensurePolished();parameterScroll->widget()->layout()->invalidate();
        const int contentHeight=parameterScroll->widget()->minimumSizeHint().height();parameterScroll->setFixedHeight(contentHeight+2);
        auxiliary_->setDirection(QBoxLayout::TopToBottom);auxiliary_->setSpacing(2);
        if(!printAuxiliary_){printAuxiliary_=new QGridLayout;printAuxiliary_->setContentsMargins(0,0,0,0);printAuxiliary_->setSpacing(2);auxiliary_->addLayout(printAuxiliary_);}
        printAuxiliary_->addWidget(hotAc,0,0);printAuxiliary_->addWidget(airCooling,0,1);printAuxiliary_->addWidget(liftArc,1,0,1,2);
        read->setMinimumHeight(28);apply->setMinimumHeight(28);
        parameterBox->layout()->invalidate();parameterBox->layout()->activate();
        printParameterBaseHeight_=parameterBox->minimumSizeHint().height();
        log->setMinimumHeight(48);log->setMaximumHeight(56);log->setPlaceholderText("焊机操作回执");
    }else{
        parameterBox->setMinimumHeight(0);read->setMinimumHeight(0);apply->setMinimumHeight(0);
        for(auto* editor:parameterEditors){editor->setMinimumHeight(0);editor->setMaximumHeight(QWIDGETSIZE_MAX);}
        for(int i=0;i<controls.size();++i){actions_->insertWidget(3+i,controls[i]);controls[i]->setMinimumHeight(38);}
        actions_->insertWidget(7,off);off->setMinimumHeight(42);parameterGrid->setVerticalSpacing(6);parameterGrid->setHorizontalSpacing(6);
        parameterScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);auxiliary_->setDirection(QBoxLayout::TopToBottom);auxiliary_->setSpacing(4);
        for(auto* check:{hotAc,liftArc,airCooling})auxiliary_->addWidget(check);
        log->setMinimumHeight(80);log->setMaximumHeight(130);log->setPlaceholderText("");
    }
}
void WelderPanel::setPrintRowHeight(int height){
    if(!printWorkspace_)return;
    for(auto* button:controls)button->setMinimumHeight(height);
    off->setMinimumHeight(height+2);
}
void WelderPanel::setPrintParameterHeight(int height){
    if(!printWorkspace_)return;
    const int extra=qMax(0,height-printParameterBaseHeight_);
    const int gapGrowth=qMin(4,extra/55),buttonGrowth=qMin(12,extra/14);
    const int editorGrowth=qMax(0,(extra-17*gapGrowth-buttonGrowth)/18);
    parameterGrid->setVerticalSpacing(2+gapGrowth);
    for(auto* editor:parameterEditors)editor->setFixedHeight(printEditorBaseHeight_+editorGrowth);
    read->setMinimumHeight(28+buttonGrowth);apply->setMinimumHeight(28+buttonGrowth);
    parameterGrid->invalidate();parameterGrid->activate();
    parameterScroll->setFixedHeight(parameterScroll->widget()->minimumSizeHint().height()+2);
    parameterBox->setMinimumHeight(qMax(printParameterBaseHeight_,height));
}
void WelderPanel::readParameters(){
    if(!welder.fresh())return;const auto& w=welder.status();operation->setCurrentIndex(w[20]);work->setCurrentIndex(w[21]);const auto& specs=WelderProto::parameters();for(int i=2;i<18;++i)values[i-2]->setValue(double(w[20+i])/specs[i].scale);hotAc->setChecked(w[38]&1);liftArc->setChecked(w[38]&2);airCooling->setChecked(w[38]&4);loaded=true;
}
void WelderPanel::toggle(int bit){
    if(!welder.ready())return;const auto before=welder.status()[10];quint16 value=before^(1u<<bit);
    if(bit==1&&(value&2)==0)value=0;
    if(bit==0&&(value&1)&&QMessageBox::question(controlsBox->window(),"起弧", "确认焊枪、工件及现场条件已就绪，并手动起弧？",QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes)return;
    if(bit==3&&(value&8)&&QMessageBox::question(controlsBox->window(),"开启热丝电源","确认开启热丝电源？",QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes)return;
    if(welder.status()[10]!=before){log->appendPlainText("工艺输出状态已变化，请核对后重新操作。");return;}
    welder.issue(WelderProto::Controls,value);
}
void WelderPanel::refresh(){
    QString why;const bool can=welder.ready(&why),live=welder.fresh();const auto& w=welder.status();
    if(!live)loaded=false;else if(!loaded)readParameters();
    state->setText(can?"焊机接口已就绪 · 工艺输出以本页反馈为准":why);WorkstationUi::tone(state,can?"success":"idle");
    actual->setText(live?QString("焊接电压  %1 V\n焊接电流  %2 A\n热丝实际电流  %3 A").arg(w[16]/10.,0,'f',1).arg(w[17]/10.,0,'f',1).arg(w[18]/10.,0,'f',1):"焊接电压  —\n焊接电流  —\n热丝实际电流  —");
    QStringList faults;const QStringList labels{"焊枪开关异常","焊机热保护","缺水保护","热丝电源热保护","热丝电源通信保护"};for(int i=0;i<5;++i)if(w[11]&(1u<<(i+2)))faults<<labels[i];
    fault->setText(!live?"报警反馈：未确认":QString("焊机：%1 · 引弧：%2\n报警：%3（原始码 %4）").arg(w[11]&2?"就绪":"未就绪").arg(w[11]&1?"成功":"无").arg(faults.isEmpty()?"无":faults.join("、")).arg(w[12]));
    output->setText(live?QString("准备 %1 · 检气 %2 · 热丝 %3 · 起弧请求 %4").arg(w[10]&2?"开":"关").arg(w[10]&4?"开":"关").arg(w[10]&8?"开":"关").arg(w[10]&1?"开":"关"):"工艺输出：未确认");
    const QStringList names{"起弧","焊机准备","检气","热丝电源"};
    for(auto* button:controls){const int bit=button->property("controlBit").toInt();button->setText(QString("%1%2").arg(live&&(w[10]&(1u<<bit))?"关闭":"开启").arg(names[bit]));button->setEnabled(can);button->setToolTip(why);}
    off->setEnabled(live&&bridge.ownsSession());read->setEnabled(live&&!welder.pending());apply->setEnabled(can&&w[10]==0);apply->setToolTip(w[10]?"先关闭全部工艺输出":why);
}
