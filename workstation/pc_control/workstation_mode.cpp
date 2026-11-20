#include "workstation_settings.h"
#include "workstation_mode.h"
#include "workstation_ui.h"
#include <QHostAddress>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
QString modeName(int mode){return mode==1?"手动":mode==2?"自动":mode==3?"外部自动":"未知 / 等待反馈";}
}
int RobotModeDialog::feedbackMode(const BridgeProto::Status& s){
    const int bits=s.rawStatus&0xe0;
    return bits==0x20?1:bits==0x40?2:bits==0x80?3:0;
}
RobotModeDialog::RobotModeDialog(PlcBridgeClient& b,std::function<bool()> safe,QWidget* parent)
    :QDialog(parent),bridge(b),idle(std::move(safe)){
    setObjectName("robotModeDialog");setWindowTitle("机器人操作模式");setMinimumWidth(500);
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args){args->flags|=CREATE_NO_WINDOW;});
#endif
    auto* layout=new QVBoxLayout(this);
    current=new QLabel;current->setObjectName("robotModeCurrent");layout->addWidget(current);
    layout->addWidget(WorkstationUi::hint("先停止、断使能并退出微调。模式切换使用控制柜 SDK；加载、使能与运行继续由 PLC 控制。"));
    auto* form=new QFormLayout;
    host=new QLineEdit(workstationSettings().value("robotMode/ip","192.168.3.188").toString());host->setObjectName("robotModeIp");form->addRow("控制柜 IP",host);
    target=new QComboBox;target->setObjectName("robotModeTarget");target->addItems({"手动","自动","外部自动"});target->setCurrentIndex(2);form->addRow("切换到",target);layout->addLayout(form);
    layout->addWidget(WorkstationUi::hint("电脑需能访问控制柜 IP 的 30312 端口。PLC 自动作业和微调服务均使用「外部自动」。切回后重新核对加载状态。"));
    result=WorkstationUi::hint();result->setObjectName("robotModeResult");layout->addWidget(result);
    apply=new QPushButton("切换模式");apply->setObjectName("robotModeApply");layout->addWidget(apply);
    connect(apply,&QPushButton::clicked,this,&RobotModeDialog::begin);
    connect(&bridge,&PlcBridgeClient::changed,this,&RobotModeDialog::refresh);
    connect(&process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int code,QProcess::ExitStatus state){
        if(!busy)return;
        const auto data=QJsonDocument::fromJson(process.readAllStandardOutput().trimmed().split('\n').last()).object();
        if(state!=QProcess::NormalExit||code!=0||!data.value("ok").toBool()||data.value("nonce").toString()!=nonce||data.value("target").toInt()!=requested||data.value("observed").toInt()!=requested){
            finish("切换未确认："+data.value("error").toString("SDK 未返回有效确认；请查看实际模式"));return;
        }
        sdkConfirmed=true;result->setText("控制柜已回读目标模式，等待 PLC 反馈确认…");refresh();
    });
    connect(&process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError e){if(busy&&e==QProcess::FailedToStart)finish("无法启动模式组件："+process.errorString());});
    timer.setInterval(100);connect(&timer,&QTimer::timeout,this,&RobotModeDialog::refresh);timer.start();refresh();
}
RobotModeDialog::~RobotModeDialog(){if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(1000);}if(busy)bridge.setOperationInhibited(false);}
bool RobotModeDialog::allowed(QString* why) const{
    const auto no=[why](const QString& s){if(why)*why=s;return false;};
    const auto s=bridge.status();
    if(!bridge.fresh()||!bridge.ownsSession())return no("先连接 PLC 并登记本机控制权");
    if(!s.has(BridgeProto::RobotOnline)||!s.stopped()||s.enabled()||s.alarm()||s.emergency()||bridge.pending()||s.has(BridgeProto::ActionBusy))return no("等待机器人在线、停稳、断使能且无在途命令或故障");
    if(!idle())return no("先退出微调并停止焊机工艺输出");
    if(why)why->clear();return true;
}
void RobotModeDialog::begin(){
    QString why;if(busy||!allowed(&why)){result->setText(why);return;}
    const QHostAddress address(host->text().trimmed());const auto ip=address.toIPv4Address();
    if(address.protocol()!=QAbstractSocket::IPv4Protocol||!((ip>>24)==10||(ip>>20)==0xac1||(ip>>16)==0xc0a8)){result->setText("请输入控制柜的私有 IPv4 地址");return;}
    const auto helper=QCoreApplication::applicationDirPath()+"/mode_helper/StepModeSwitch.exe";
    if(!QFileInfo::exists(helper)){result->setText("未安装模式切换组件");return;}
    requested=target->currentIndex()+1;nonce=QUuid::createUuid().toString(QUuid::Id128);boot=bridge.status().boot;owner=bridge.sessionId();
    busy=true;sdkConfirmed=false;elapsed.restart();bridge.setOperationInhibited(true);
    workstationSettings().setValue("robotMode/ip",address.toString());
    result->setText("正在切换，等待控制柜与 PLC 回读…");process.setWorkingDirectory(QFileInfo(helper).absolutePath());process.start(helper,{address.toString(),QString::number(requested),nonce});refresh();
}
void RobotModeDialog::finish(const QString& text){
    busy=false;if(process.state()!=QProcess::NotRunning)process.kill();bridge.setOperationInhibited(false);result->setText(text);refresh();
}
void RobotModeDialog::refresh(){
    current->setText("当前模式："+(bridge.fresh()?modeName(feedbackMode(bridge.status())):"PLC 反馈未确认"));
    if(busy){
        QString why;
        if(!allowed(&why)||bridge.status().boot!=boot||bridge.sessionId()!=owner){finish("状态已变化，切换结果待确认："+why);return;}
        if(sdkConfirmed&&feedbackMode(bridge.status())==requested){finish("已确认："+modeName(requested)+"（控制柜及 PLC 回读一致）");return;}
        if(elapsed.elapsed()>10000){finish("模式切换超时，结果待确认；请检查控制柜及网络");return;}
    }
    QString why;const bool can=allowed(&why)&&!bridge.operationInhibited();apply->setEnabled(!busy&&can);apply->setToolTip(why);host->setEnabled(!busy);target->setEnabled(!busy);
    if(!busy&&result->text().isEmpty())result->setText(why.isEmpty()?"选择目标模式后点击切换。":why);
}
