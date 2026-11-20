#include "../workstation_manualbridge.h"
#include "../workstation_ui.h"
#include <QtWidgets>
#include <QTcpServer>
#include <QtEndian>
#include <QTest>
#include <functional>
#include <iostream>
#include <memory>
#include <cmath>

namespace {
int failures=0,checks=0;
void check(bool ok,const char* message){++checks;if(!ok){++failures;std::cerr<<"FAIL: "<<message<<'\n';}}
bool waitFor(const std::function<bool()>& predicate,int ms=2400){
    QElapsedTimer age;age.start();while(!predicate()&&age.elapsed()<ms){QEventLoop loop;QTimer::singleShot(10,&loop,&QEventLoop::quit);loop.exec();}return predicate();
}
void settle(int ms){waitFor([]{return false;},ms);}
quint16 get(const QByteArray& b,int i){return qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(b.constData()+i));}
void put(QByteArray& b,quint16 value){const auto n=qToBigEndian(value);b.append(reinterpret_cast<const char*>(&n),2);}
void set32(QVector<quint16>& w,int i,quint32 n){w[i]=quint16(n>>16);w[i+1]=quint16(n);}
quint32 get32(const QVector<quint16>& w,int i){return quint32(w[i])<<16|w[i+1];}
class Simulator:public QObject{
public:
    QTcpServer server;QTimer scan;
    QVector<quint16> main=QVector<quint16>(64,0),status=QVector<quint16>(32,0),position=QVector<quint16>(40,0);
    QList<QVector<quint16>> commands,mainCommands;
    QList<QTcpSocket*> extended;
    bool dynamic=true,jogging=false,autoStage=true,positionDynamic=true;int invalid=0,segments=0,rejectStageAction=0;
    Simulator(){
        main[0]=BridgeProto::Magic;main[1]=2;main[2]=1|2|4|64|512|1024;main[17]=0x94;main[13]=10;
        main[57]=1;main[58]=255;main[60]=49;main[61]=56;main[63]=2;set32(main,10,1);
        status[0]=ManualProto::Magic;status[1]=2;status[2]=ManualProto::Installed|ManualProto::Binding;status[14]=10;set32(status,20,1);status[28]=31;status[29]=0x0603;
        position[0]=0x5053;position[1]=1;position[2]=12;position[3]=1;set32(position,4,1);set32(position,6,1);position[36]=0x0603;position[37]=12;position[38]=1000;
        scan.setInterval(20);connect(&scan,&QTimer::timeout,this,[this]{++main[12];if(dynamic)++status[9];if(jogging)++segments;if(positionDynamic&&main[12]%10==0)set32(position,6,get32(position,6)+1);});scan.start();
        connect(&server,&QTcpServer::newConnection,this,[this]{
            auto* socket=server.nextPendingConnection();auto buffer=std::make_shared<QByteArray>();
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,this,[this,socket,buffer]{
                *buffer+=socket->readAll();
                while(buffer->size()>=12){
                    const int total=6+get(*buffer,4);if(buffer->size()<total)return;
                    const auto r=buffer->left(total);buffer->remove(0,total);const int fc=quint8(r[7]),base=get(r,8);QByteArray pdu;
                    if(fc==3&&((base==300&&get(r,10)==64)||(base==400&&get(r,10)==32)||(base==450&&get(r,10)==40))){
                        const auto& data=base==300?main:base==450?position:status;pdu.append(char(3)).append(char(data.size()*2));for(auto value:data)put(pdu,value);
                        if(base==400&&!extended.contains(socket))extended<<socket;
                    }else if(fc==16&&get(r,10)==16&&(base==200||base==220)){
                        QVector<quint16> w;for(int i=0;i<16;++i)w<<get(r,13+2*i);
                        if(base==200){
                            mainCommands<<w;
                            set32(main,4,get32(w,2));main[6]=w[1];main[7]=2;
                            if(w[1]==BridgeProto::Register){main[2]|=8|16;set32(main,15,get32(w,6));set32(status,22,get32(w,6));}
                            if(w[1]==BridgeProto::Stop){main[17]=0x94;status[2]=3;status[7]=2;}
                            if(w[1]==BridgeProto::ClearError){main[17]&=quint16(~2);}
                            if(w[1]==BridgeProto::Rearm){main[2]&=quint16(~BridgeProto::StopLatch);}
                        }else{
                            quint16 sum=0xa55a;for(int i=0;i<13;++i)sum^=w[i];
                            if(w[0]!=ManualProto::Magic||w[13]!=sum||get32(w,8)!=get32(main,15)||get32(w,10)!=1)++invalid;
                            commands<<w;set32(status,4,get32(w,2));status[6]=w[1];status[7]=1;status[8]=0;
                            if(w[1]==ManualProto::Enter){status[2]|=ManualProto::Entered|ManualProto::ServiceAlive|ManualProto::HostValid;status[7]=2;main[17]=0x85;}
                            else if(w[1]==ManualProto::Step||w[1]==ManualProto::Move||w[1]==ManualProto::Return){status[2]|=ManualProto::Busy;status[3]=40;}
                            else if(w[1]==ManualProto::Jog){status[2]|=ManualProto::Busy;status[3]=60;jogging=true;}
                            else if(w[1]==ManualProto::Stage){
                                status[2]|=ManualProto::Busy;status[3]=40;
                                if(autoStage)QTimer::singleShot(30,this,[this,w]{
                                    if(status[6]!=ManualProto::Stage||get32(status,4)!=get32(w,2))return;
                                    complete();if(w[5]==rejectStageAction){status[7]=4;status[8]=14;}
                                });
                            }
                            else if(w[1]==ManualProto::Exit||w[1]==ManualProto::Cancel){
                                if(w[1]==ManualProto::Exit||(!jogging&&(status[2]&ManualProto::Busy))){status[2]=3;main[17]=0x94;}jogging=false;status[2]&=quint16(~ManualProto::Busy);status[3]=0;status[7]=2;
                            }
                        }
                        pdu.append(char(16));put(pdu,base);put(pdu,16);
                    }else if(fc==6&&(base==215||base==235)){pdu=r.mid(7);}
                    else{++invalid;pdu.append(char(fc|0x80)).append(char(2));}
                    QByteArray response=r.left(4);put(response,quint16(pdu.size()+1));response.append(r[6]);response+=pdu;socket->write(response);
                }
            });
        });
        check(server.listen(QHostAddress::LocalHost),"loopback server only");
    }
    void complete(){status[7]=2;status[3]=0;status[2]&=quint16(~ManualProto::Busy);}
};
struct Fixture{
    Simulator sim;PlcBridgeClient bridge;ManualBridgePanel panel;
    Fixture():panel(bridge){
        panel.setModeEnabled(true);panel.resize(1200,620);panel.show();
        bridge.connectTo("127.0.0.1",sim.server.serverPort(),1);
        check(waitFor([this]{return bridge.fresh()&&panel.client().fresh();}),"both PLC channels have dynamic scan");
        check(bridge.issue(BridgeProto::Register),"explicit main registration");
        check(waitFor([this]{return bridge.ownsSession()&&!bridge.pending();}),"main owner acknowledged");
    }
    void enter(){check(panel.client().issue(ManualProto::Enter),"explicit enter accepted");check(waitFor([this]{return !panel.client().pending()&&panel.client().status().has(ManualProto::Entered);}),"resident entry acknowledged");}
    QPushButton* direction(){return panel.findChild<QPushButton*>("manualDir1");}
    void press(){auto* b=direction();check(b&&b->isEnabled(),"direction enabled");QTest::mousePress(b,Qt::LeftButton);}
    void release(){QTest::mouseRelease(direction(),Qt::LeftButton);}
};
}
int main(int argc,char** argv){
    QApplication app(argc,argv);app.setStyle("Fusion");app.setFont(QFont("Microsoft YaHei UI",11));app.setStyleSheet(WorkstationUi::styleSheet());QTemporaryDir settings;
    QStandardPaths::setTestModeEnabled(true);QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    {
        Fixture f;
        auto* ack=f.panel.findChild<QPushButton*>("manualAcknowledge");
        auto* reset=f.panel.findChild<QPushButton*>("manualRearm");
        auto* health=f.panel.findChild<QLabel*>("manualRecoveryStatus");
        check(ack&&reset&&health,"manual page exposes explicit recovery controls");
        f.sim.main[17]=0x92;f.sim.main[2]|=BridgeProto::StopLatch;f.sim.main[2]&=quint16(~BridgeProto::LoadConfirmed);
        check(waitFor([&]{return ack->isEnabled()&&!reset->isEnabled()&&f.bridge.status().alarm();}),"latched alarm permits only acknowledgement first");
        check(health->text().contains("报警：有"),"alarm displayed distinctly");
        check(!f.panel.client().ready(),"alarm blocks manual service entry");
        ack->click();check(waitFor([&]{return !f.bridge.pending()&&!f.bridge.status().alarm();}),"manual recovery sends cabinet clear request");
        check(f.bridge.status().has(BridgeProto::StopLatch),"acknowledgement does not release stop latch");
        check(waitFor([&]{return reset->isEnabled();}),"reset available after alarm summary clears");
        reset->click();check(waitFor([&]{return !f.bridge.pending()&&!f.bridge.status().has(BridgeProto::StopLatch);}),"explicit stop-latch reset acknowledged");
        settle(180);
        check(f.sim.mainCommands.size()==3&&f.sim.mainCommands[1][1]==BridgeProto::ClearError&&f.sim.mainCommands[2][1]==BridgeProto::Rearm,"recovery sends only register clear reset");
        check(f.sim.commands.isEmpty()&&!f.bridge.status().enabled()&&!f.bridge.status().running(),"recovery never enables starts or enters the service");
    }
    {
        Fixture f;check(f.sim.commands.isEmpty(),"connect and register never enter or move");f.enter();
        check(f.panel.findChild<QPushButton*>("manualEnter")->property("actionState")=="done","entry completion shown only from entered feedback");
        check(!f.panel.findChild<QPushButton*>("manualLoad")->isEnabled(),"service reload unavailable while entered");
        check(f.panel.findChild<QLabel*>("manualServiceBadge")->text().contains("已进入"),"current service state visible");
        if(argc>1){
            const QString path=QString::fromLocal8Bit(argv[1]);check(f.panel.grab().save(path),"manual panel render saved");
            auto* tabs=f.panel.findChild<QTabWidget*>("manualMotionTabs");tabs->setCurrentIndex(1);
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);const QFileInfo info(path);
            check(f.panel.grab().save(info.path()+"/"+info.completeBaseName()+"-target.png"),"absolute target panel render saved");tabs->setCurrentIndex(0);
        }
        check(f.panel.client().issue(ManualProto::Step,2,-100,100),"negative single step accepted");
        check(waitFor([&]{return f.sim.commands.size()==2;}),"step reaches PLC");
        check(qint16(f.sim.commands.last()[6])==-100&&f.sim.commands.last()[5]==2,"signed angle and axis preserved");
        check(!f.panel.client().issue(ManualProto::Step,1,100,100),"second in-flight step rejected");settle(350);
        check(f.sim.commands.size()==2,"pending step never retried or queued");f.sim.complete();
        check(waitFor([&]{return !f.panel.client().pending();}),"application completion clears pending");
        check(!f.panel.client().issue(ManualProto::Step,1,10001,100),"oversize input rejected before transmit");
        f.press();check(waitFor([&]{return f.sim.commands.size()==3;}),"real mouse press starts resident jog");settle(400);
        check(f.sim.commands.size()==3&&f.sim.segments>3&&f.panel.client().pending(),"resident jog continues across several segments without PC per-step retransmit");
        check(f.direction()->isEnabled()&&f.direction()->isDown(),"feedback refresh preserves real mouse hold and release delivery");
        f.release();check(waitFor([&]{return f.sim.commands.size()==4&&f.sim.commands.last()[1]==ManualProto::Cancel;}),"real mouse release withdraws jog");
        check(waitFor([&]{return !f.panel.client().pending();}),"release stop receipt confirmed");settle(350);check(f.sim.commands.size()==4,"release prevents all later repeats");
        check(f.panel.client().status().has(ManualProto::Entered)&&f.bridge.status().enabled(),"normal release retains resident service and enable");
        f.press();check(waitFor([&]{return f.sim.commands.size()==5;}),"next hold works without reload or re-enable");
        f.release();check(waitFor([&]{return !f.panel.client().pending();}),"second release completes");
        check(f.sim.invalid==0,"all wire frames obey protocol and same PLC endpoint");
    }
    {
        Fixture f;f.enter();f.press();check(waitFor([&]{return f.sim.commands.size()==2;}),"focus test started step");
        QEvent lost(QEvent::UngrabMouse);QApplication::sendEvent(f.direction(),&lost);
        check(waitFor([&]{return f.sim.commands.size()==3&&f.sim.commands.last()[1]==ManualProto::Exit;}),"mouse capture loss requests ordinary stop and exit");
        settle(300);check(f.sim.commands.size()==3,"focus loss does not resume");
    }
    {
        Fixture f;f.enter();f.press();check(waitFor([&]{return f.sim.commands.size()==2;}),"navigation test started step");
        f.panel.hide();
        check(waitFor([&]{return f.sim.commands.size()==3&&f.sim.commands.last()[1]==ManualProto::Exit;}),"leaving manual page stops held motion");
        settle(180);check(f.sim.commands.size()==3,"hidden page never repeats a step");
    }
    {
        Fixture f;f.enter();f.press();check(waitFor([&]{return f.sim.commands.size()==2;}),"disconnect test in flight");
        for(auto* socket:f.sim.extended)socket->abort();
        check(waitFor([&]{return f.panel.client().uncertain();}),"disconnect preserves unknown execution outcome");
        check(!f.panel.findChild<QPushButton*>("manualLoad")->isEnabled()&&!f.direction()->isEnabled(),"lost manual status disables setup and motion");
        check(!f.panel.safeToClose(),"unknown result never treated as safe stop");settle(300);check(f.sim.commands.size()==2,"disconnect never retransmits step");
        f.sim.status[2]=3;f.sim.status[7]=2;f.sim.main[17]=0x94;f.panel.client().reconnect();
        check(waitFor([&]{return f.panel.client().fresh()&&!f.panel.client().uncertain();}),"explicit reconnect plus fresh stopped feedback resolves uncertainty");
    }
    {
        Fixture f;f.enter();
        check(f.panel.findChild<QComboBox*>("manualMultiplier")->count()==9,"nine displacement presets available");
        check(f.panel.findChild<QDoubleSpinBox*>("manualDistance")->maximum()==10,"single increment supports ten units");
        std::array<std::optional<double>,6> target;target[0]=-.001;target[3]=-179.125;
        check(f.panel.client().moveTarget(true,target,32767),"WORLD target accepts the full representable speed");
        check(waitFor([&]{return f.sim.commands.size()==9;}),"begin plus six fields plus one final move");
        check(f.sim.commands[1][1]==ManualProto::Stage&&f.sim.commands[1][5]==23&&f.sim.commands[1][6]==2,"target kind staged without motion");
        check(qint16(f.sim.commands[2][6])==-1&&f.sim.commands[2][7]==999,"negative subunit target represented exactly");
        check(f.sim.commands[3][6]==0&&f.sim.commands[3][7]==2000,"unselected coordinate preserves cabinet snapshot");
        check(qint16(f.sim.commands[5][6])==-180&&f.sim.commands[5][7]==875,"negative orientation fraction preserved");
        check(f.sim.commands.last()[1]==ManualProto::Move&&f.sim.commands.last()[5]==30&&f.sim.commands.last()[7]==32767,"only final committed frame starts motion with exact maximum WORLD speed");
        check(!f.panel.client().issue(ManualProto::Step,1,100,100),"no interleaved motion during target transaction");
        f.sim.complete();check(waitFor([&]{return !f.panel.client().pending()&&!f.panel.client().targetActive();}),"absolute move completion closes transaction");
        target[0]=10000.001;check(!f.panel.client().moveTarget(true,target,100),"out of range absolute target rejected locally");
    }
    {
        Fixture f;f.enter();f.sim.rejectStageAction=26;
        std::array<std::optional<double>,6> target;target[0]=10;
        check(f.panel.client().moveTarget(false,target,100),"failed field case starts explicitly");
        check(waitFor([&]{return !f.panel.client().targetActive()&&!f.panel.client().pending();}),"field rejection ends target transaction");
        settle(250);bool move=false;for(const auto& w:f.sim.commands)move|=w[1]==ManualProto::Move;
        check(!move,"partial or rejected target never commits motion");
    }
    {
        Fixture f;f.enter();f.sim.autoStage=false;
        std::array<std::optional<double>,6> target;target[2]=100;
        check(f.panel.client().moveTarget(true,target,100),"cancel case starts staged target");
        check(waitFor([&]{return f.sim.commands.size()==2;}),"only begin transmitted before acknowledgment");
        f.panel.halt(true);check(waitFor([&]{return !f.panel.client().pending();}),"stop preempts target staging");
        settle(250);check(f.sim.commands.size()==3&&f.sim.commands.last()[1]==ManualProto::Exit,"canceled target does not advance after late receipt");
    }
    {
        Fixture f;f.enter();
        const std::array<int,12> actual{-1,20000,-30000,0,45000,-179125,2000125,-999,2105400,-180000,45000,0};
        for(int i=0;i<12;++i)set32(f.sim.position,10+2*i,quint32(actual[i]));
        f.sim.position[2]=13;set32(f.sim.position,6,get32(f.sim.position,6)+1);
        check(waitFor([&]{return f.panel.client().positionFresh();}),"complete R6.3 position snapshot read via same PLC channel");
        check(std::abs(f.panel.client().position().values[0]+.001)<.0001,"negative subunit joint feedback decoded exactly");
        check(f.panel.findChild<QLabel*>("manualJointCurrent0")->text().contains("-0.001"),"joint position shown in manual panel");
        auto* tabs=f.panel.findChild<QTabWidget*>("manualMotionTabs");tabs->setCurrentIndex(1);
        auto* fill=f.panel.findChild<QPushButton*>("manualFillCurrent");
        check(fill&&fill->isEnabled(),"fill current available only with valid snapshot");
        const auto count=f.sim.commands.size();fill->click();
        check(f.panel.findChild<QDoubleSpinBox*>("manualTargetValue1")->value()==20,"fill current joint target without motion");
        check(!f.panel.findChild<QCheckBox*>("manualTargetUse1")->isChecked()&&f.sim.commands.size()==count,"fill does not select or send motion");
        f.panel.findChild<QComboBox*>("manualTargetFrame")->setCurrentIndex(1);fill->click();
        check(f.panel.findChild<QDoubleSpinBox*>("manualTargetValue0")->value()==2000.125,"WORLD target prefills from same frame snapshot");
        check(f.panel.findChild<QDoubleSpinBox*>("manualTargetValue1")->value()==-.999,"negative WORLD coordinate preserved");
        f.panel.findChild<QCheckBox*>("manualTargetUse0")->setChecked(true);
        f.panel.findChild<QDoubleSpinBox*>("manualTargetValue0")->setValue(2001.125);settle(300);
        check(f.panel.findChild<QDoubleSpinBox*>("manualTargetValue0")->value()==2001.125,"incoming feedback never overwrites edited target");
        if(argc>1){const QFileInfo info(QString::fromLocal8Bit(argv[1]));check(f.panel.grab().save(info.path()+"/plc_manual_position.png"),"position panel screenshot saved");}
        f.sim.position[2]=12;check(waitFor([&]{return !fill->isEnabled();}),"invalid reference disables fill");
        check(f.panel.findChild<QLabel*>("manualJointCurrent0")->text()=="—","invalid position never displayed as zero");
        check(f.sim.invalid==0,"position reads use only documented PLC block");
    }
    {
        Fixture f;f.sim.status[1]=1;settle(180);check(!f.panel.client().ready(),"new actions unavailable on old PLC protocol");
    }
    {
        Fixture f;f.enter();
        auto* speed=f.panel.findChild<QDoubleSpinBox*>("manualSpeed");
        auto* preset=f.panel.findChild<QComboBox*>("manualSpeedPreset");
        check(speed->maximum()==100&&preset->count()==11,"all joints have full speed plus retained fine presets");
        check(speed->value()==1,"startup preserves low manual speed");
        for(int axis=1;axis<=6;++axis){
            const auto count=f.sim.commands.size();
            check(f.panel.client().issue(ManualProto::Step,quint16(axis),100,10000),"every joint accepts 100 percent speed");
            check(waitFor([&]{return f.sim.commands.size()==count+1;}),"full joint speed reaches PLC");
            check(f.sim.commands.last()[7]==10000,"full speed encoded as 10000 without truncation");
            f.sim.complete();check(waitFor([&]{return !f.panel.client().pending();}),"full speed step completed");
        }
        check(!f.panel.client().issue(ManualProto::Step,1,100,10001),"above full speed rejected");
        check(!f.panel.client().issue(ManualProto::Jog,6,1001,10000),"held segment above one degree rejected");
        check(!f.panel.client().issue(ManualProto::Jog,6,1000,500),"large segment rejected at slow speed");
        speed->setValue(100);const auto before=f.sim.commands.size();f.press();
        check(waitFor([&]{return f.sim.commands.size()==before+1;}),"full-speed real button hold transmitted");
        check(f.sim.commands.last()[6]==1000&&f.sim.commands.last()[7]==10000,"full-speed hold uses one degree independently of single-step distance");
        f.release();check(waitFor([&]{return !f.panel.client().pending();}),"full-speed release completes without reload");
        check(f.panel.client().status().has(ManualProto::Entered)&&f.bridge.status().enabled(),"high-speed release retains enable and service");
        f.panel.findChild<QComboBox*>("manualFrame")->setCurrentIndex(1);
        check(speed->maximum()==3276.7&&speed->value()==100&&preset->count()==13,"WORLD opens its own physical-unit speed range and presets");
        check(waitFor([&]{return !f.panel.client().pending()&&f.direction()->isEnabled();}),"coordinate change withdrawal acknowledged before next hold");
        speed->setValue(3276.7);const auto worldBefore=f.sim.commands.size();f.press();
        check(waitFor([&]{return f.sim.commands.size()==worldBefore+1;}),"maximum WORLD held speed reaches PLC");
        check(f.sim.commands.last()[5]==11&&f.sim.commands.last()[6]==10000&&f.sim.commands.last()[7]==32767,"WORLD held button encodes ten-unit segment and full speed without overflow");
        f.release();check(waitFor([&]{return !f.panel.client().pending();}),"WORLD release completes and permits another action");
        check(!f.panel.client().issue(ManualProto::Step,11,100,32768),"WORLD speed cannot overflow the marker bit");
        check(!f.panel.client().issue(ManualProto::Jog,11,501,250),"WORLD held segment follows current speed profile");
    }
    {
        Fixture f;f.enter();
        std::array<std::optional<double>,6> target;target[0]=10;
        check(!f.panel.client().moveTarget(false,target,10001)&&!f.panel.client().moveTarget(true,target,32768),"target speed validates against the selected coordinate type");
        check(f.panel.client().moveTarget(false,target,10000),"joint absolute target accepts 100 percent");
        check(waitFor([&]{return f.sim.commands.size()==9;}),"joint target transaction reaches final motion frame");
        check(f.sim.commands.last()[7]==10000,"joint absolute target retains full speed across staging");
        f.sim.complete();check(waitFor([&]{return !f.panel.client().pending();}),"joint absolute completion received");
        f.panel.findChild<QTabWidget*>("manualMotionTabs")->setCurrentIndex(1);
        f.panel.findChild<QComboBox*>("manualTargetFrame")->setCurrentIndex(1);
        auto* targetSpeed=f.panel.findChild<QDoubleSpinBox*>("manualTargetSpeed");
        check(targetSpeed->maximum()==3276.7&&targetSpeed->decimals()==1&&targetSpeed->suffix().contains("mm/s"),"target WORLD speed control shows the full numeric range and correct units");
        targetSpeed->setValue(1000);
        f.panel.findChild<QDoubleSpinBox*>("manualTargetValue0")->setValue(100);
        f.panel.findChild<QCheckBox*>("manualTargetUse0")->setChecked(true);
        QTimer::singleShot(20,[]{for(auto* w:QApplication::topLevelWidgets())if(auto* box=qobject_cast<QMessageBox*>(w))if(auto* yes=box->button(QMessageBox::Yes))yes->click();});
        f.panel.findChild<QPushButton*>("manualTargetMove")->click();
        check(waitFor([&]{return f.sim.commands.size()==17;}),"actual target button commits all six staged fields");
        check(f.sim.commands.last()[7]==10000,"1000 mm/s UI selection becomes speed word 10000, not 100000");
        f.sim.complete();check(waitFor([&]{return !f.panel.client().pending();}),"WORLD UI target completes");
        if(f.sim.commands.size()!=17)std::cerr<<"TARGET UI: count="<<f.sim.commands.size()<<' '<<f.panel.findChild<QPlainTextEdit*>("manualEventLog")->toPlainText().toStdString()<<'\n';
        if(argc>1){const QFileInfo info(QString::fromLocal8Bit(argv[1]));check(f.panel.grab().save(info.path()+"/plc_manual_target_fullspeed.png"),"open WORLD target speed screenshot saved");}
    }
    {
        Fixture f;f.enter();f.sim.status[2]|=ManualProto::HomeSaved;f.sim.status[27]=17;
        auto* returnSpeed=f.panel.findChild<QDoubleSpinBox*>("manualReturnSpeed");returnSpeed->setValue(100);
        auto* button=f.panel.findChild<QPushButton*>("manualReturn");
        check(returnSpeed->maximum()==100&&waitFor([&]{return button->isEnabled();}),"return speed is open to 100 percent for a saved origin");
        QTimer::singleShot(20,[]{for(auto* w:QApplication::topLevelWidgets())if(auto* box=qobject_cast<QMessageBox*>(w))if(auto* yes=box->button(QMessageBox::Yes))yes->click();});
        button->click();check(waitFor([&]{return f.sim.commands.size()==2;}),"return button sends one command");
        check(f.sim.commands.last()[1]==ManualProto::Return&&f.sim.commands.last()[6]==17&&f.sim.commands.last()[7]==10000,"return sends saved origin and full joint speed");
        f.sim.complete();check(waitFor([&]{return !f.panel.client().pending();}),"one return receipt completes the action");
        settle(300);check(f.sim.commands.size()==2,"uncorroborated AtHome never causes automatic repeated return");
        if(f.sim.commands.size()!=2)std::cerr<<"RETURN UI: count="<<f.sim.commands.size()<<' '<<f.panel.findChild<QPlainTextEdit*>("manualEventLog")->toPlainText().toStdString()<<'\n';
    }
    {
        Fixture f;f.sim.status[28]=15;f.sim.status[29]=0x0602;settle(200);
        check(!f.panel.client().ready(),"R6.2 PLC cannot execute new speed protocol");
        check(!f.panel.client().issue(ManualProto::Enter),"wrong revision cannot enter service");
        check(f.sim.commands.isEmpty(),"wrong revision transmits no motion or entry request");
    }
    {
        Fixture f;f.enter();f.sim.dynamic=false;
        check(waitFor([&]{return f.panel.client().uncertain()&&!f.panel.client().fresh();},2200),"frozen manual PLC scan revokes client lease");
        check(!f.panel.safeToClose(),"frozen feedback cannot confirm stop");
    }
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
