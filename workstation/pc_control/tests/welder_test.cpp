#include "../workstation_welder.h"
#include "../workstation_mode.h"
#include "../workstation_print.h"
#include <QTcpServer>
#include <QtEndian>
#include <QTest>
#include <memory>
#include <iostream>

namespace {
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::cerr<<"FAIL: "<<label<<'\n';}}
bool waitFor(const std::function<bool()>& ready,int ms=2500){QElapsedTimer t;t.start();while(!ready()&&t.elapsed()<ms){QEventLoop loop;QTimer::singleShot(10,&loop,&QEventLoop::quit);loop.exec();}return ready();}
void settle(int ms){waitFor([]{return false;},ms);}
quint16 word(const QByteArray& b,int n){return qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(b.constData()+n));}
void put(QByteArray& b,quint16 v){const auto n=qToBigEndian(v);b.append(reinterpret_cast<const char*>(&n),2);}
quint32 pair(const QVector<quint16>& w,int n){return (quint32(w[n])<<16)|w[n+1];}
void set(QVector<quint16>& w,int n,quint32 v){w[n]=quint16(v>>16);w[n+1]=quint16(v);}
class Sim : public QObject {
public:
    QTcpServer server;QTimer scan;
    QVector<quint16> main=QVector<quint16>(64,0),welder=QVector<quint16>(64,0);
    QList<QVector<quint16>> commands;QList<int> writes;
    int heartbeats=0,invalid=0;bool dynamic=true,ack=true;
    Sim(){
        main[0]=0x4d42;main[1]=2;main[2]=1|2|4|64|1024;main[17]=0x90;main[57]=1;main[58]=255;main[60]=49;main[61]=56;main[63]=2;set(main,10,123);
        welder[0]=WelderProto::Magic;welder[1]=1;welder[3]=127;set(welder,4,123);welder[11]=2;
        const auto& p=WelderProto::parameters();for(int i=0;i<18;++i)welder[20+i]=p[i].initial;
        scan.setInterval(20);connect(&scan,&QTimer::timeout,this,[this]{++main[12];if(dynamic)++welder[2];});scan.start();
        connect(&server,&QTcpServer::newConnection,this,[this]{
            auto* s=server.nextPendingConnection();auto rx=std::make_shared<QByteArray>();
            connect(s,&QTcpSocket::readyRead,this,[this,s,rx]{
                *rx+=s->readAll();while(rx->size()>=8){const int total=6+word(*rx,4);if(rx->size()<total)return;const auto q=rx->left(total);rx->remove(0,total);const int fc=quint8(q[7]),address=word(q,8);QByteArray pdu;
                    if(fc==3&&word(q,10)==64&&(address==300||address==7200)){
                        const auto& w=address==300?main:welder;pdu.append(char(3)).append(char(128));for(auto v:w)put(pdu,v);
                    }else if(fc==16&&(address==200||address==7000)){
                        const int count=word(q,10);QVector<quint16> w;for(int i=0;i<count;++i)w<<word(q,13+2*i);writes<<address;
                        if(address==200&&count==16){set(main,4,pair(w,2));main[6]=w[1];main[7]=2;if(w[1]==1){main[2]|=8|16;set(main,15,pair(w,6));set(welder,14,pair(w,6));}}
                        else if(address==7000&&count==32){commands<<w;if(ack){set(welder,6,pair(w,2));welder[8]=1;welder[9]=0;welder[13]=w[8];if(w[8]==1)welder[10]=w[9];if(w[8]==2)for(int i=0;i<20;++i)welder[20+i]=w[10+i];if(w[8]==3)welder[10]=0;}}
                        else ++invalid;
                        pdu.append(char(16));put(pdu,address);put(pdu,count);
                    }else if(fc==6&&(address==215||address==7064)){
                        writes<<address;if(address==7064){++heartbeats;welder[19]=word(q,10);}pdu=q.mid(7);
                    }else{++invalid;pdu.append(char(fc|0x80)).append(char(2));}
                    QByteArray reply=q.left(4);put(reply,quint16(1+pdu.size()));reply.append(q[6]);reply+=pdu;s->write(reply);
                }
            });
        });check(server.listen(QHostAddress::LocalHost),"loopback-only simulator listens");
    }
};
}
int main(int argc,char** argv){
    QApplication app(argc,argv);
    const auto encoded=WelderProto::frame(WelderProto::Parameters,0x12345678,0x9abcdef0,77,0,QVector<quint16>(20,123));
    quint16 checksum=0xa55a;for(int i=0;i<31;++i)checksum^=encoded[i];
    check(encoded.size()==32&&encoded[0]==0x5742&&encoded[1]==1&&pair(encoded,2)==0x12345678&&pair(encoded,4)==0x9abcdef0&&checksum==encoded[31],"command identity, endian and checksum");
    Sim sim;PlcBridgeClient bridge;WelderPanel panel(bridge);auto& welder=panel.client();panel.setModeEnabled(true);
    check(welder.safeIdle()&&!welder.fresh(),"uninstalled module does not prevent existing workflow");
    check(!welder.issue(WelderProto::Controls,3)&&sim.commands.isEmpty(),"offline process writes rejected");
    bridge.connectTo("127.0.0.1",sim.server.serverPort());check(waitFor([&]{return bridge.fresh()&&welder.fresh();}),"separate signed welder status advances");
    check(!welder.issue(WelderProto::Controls,2),"unregistered host cannot set outputs");
    check(bridge.issue(BridgeProto::Register),"main session registration requested");check(waitFor([&]{return bridge.ownsSession()&&welder.ready();}),"welder borrows main-session identity");
    {
        PlcBridgePanel robot(bridge);robot.setModeEnabled(true);PrintPanel combined(robot,panel);
        auto* slot=robot.findChild<QSpinBox*>("plcBridgeSlot");auto* parameter=panel.findChild<QDoubleSpinBox*>("welderParameter2");
        auto* prepare=panel.findChild<QPushButton*>("welderControl1");auto* read=panel.findChild<QPushButton*>("welderRead");
        const auto session=bridge.sessionId();const int before=sim.commands.size();slot->setValue(11);parameter->setValue(123.4);
        for(int size:{1920,1120,1520}){
            combined.resize(size,900);combined.setActive(true);settle(30);
            check(combined.findChild<QSpinBox*>("plcBridgeSlot")==slot&&combined.findChild<QPushButton*>("welderRead")==read,"combined page uses the original robot and welder widgets");
            check(slot->value()==11&&qFuzzyCompare(parameter->value(),123.4)&&bridge.sessionId()==session&&bridge.ownsSession(),"combined page preserves selected program, parameter draft and session");
            combined.setActive(false);settle(30);
            check(robot.findChild<QSpinBox*>("plcBridgeSlot")==slot&&panel.findChild<QDoubleSpinBox*>("welderParameter2")==parameter,"standalone pages recover the same controls");
        }
        check(sim.commands.size()==before,"page changes do not issue or replay welder commands");
        combined.setActive(true);prepare->click();
        check(waitFor([&]{return !welder.pending()&&welder.status()[10]==2;}),"existing prepare button works while mounted on combined page");
        check(sim.commands.size()==before+1&&sim.commands.last()[8]==WelderProto::Controls&&sim.commands.last()[9]==2,"combined prepare sends exactly one explicit controls frame");
        combined.setActive(false);
        check(welder.issue(WelderProto::AllOff)&&waitFor([&]{return welder.safeIdle();}),"existing all-off recovers after combined operation");
    }
    check(waitFor([&]{return sim.heartbeats>0;}),"lease heartbeat uses separate welder address");
    bridge.setOperationInhibited(true);
    check(!bridge.canIssue(BridgeProto::Enable)&&!bridge.canIssue(BridgeProto::Load,11)&&bridge.canIssue(BridgeProto::Stop),"mode operation inhibits commands and retains ordinary stop");
    check(!welder.issue(WelderProto::Controls,2),"mode switch inhibits welder output requests");bridge.setOperationInhibited(false);
    check(welder.issue(WelderProto::Controls,2),"explicit prepare request");check(waitFor([&]{return !welder.pending()&&welder.status()[10]==2;}),"application ack confirms prepare");
    sim.welder[11]=0;settle(230);check(!welder.issue(WelderProto::Controls,3),"no arc when welder is unready");
    sim.welder[11]=2;settle(230);check(!welder.issue(WelderProto::Controls,7),"gas and arc cannot coexist");
    check(welder.issue(WelderProto::Controls,3),"explicit arc request with ready feedback");check(waitFor([&]{return !welder.pending()&&welder.status()[10]==3;}),"arc request status is distinguished from arc-success input");
    check(!welder.safeIdle(),"active process blocks mode and upload operations");
    check(welder.issue(WelderProto::AllOff),"explicit all-off request");check(waitFor([&]{return !welder.pending()&&welder.safeIdle();}),"all-off confirmed from PLC");
    QVector<quint16> p(20,0);const auto& specs=WelderProto::parameters();for(int i=0;i<18;++i)p[i]=specs[i].initial;p[2]=1254;p[5]=123;p[18]=5;
    check(welder.issue(WelderProto::Parameters,0,p),"parameter frame accepted");check(waitFor([&]{return !welder.pending()&&welder.status()[22]==1254&&welder.status()[25]==123&&welder.status()[38]==5;}),"scaled parameters and auxiliary options round-trip");
    p[2]=4001;check(!welder.issue(WelderProto::Parameters,0,p),"out-of-range parameter rejected before write");
    sim.welder[0]=0;settle(230);const int n=sim.commands.size();check(!welder.issue(WelderProto::Controls,2),"foreign protocol signature blocks writes");settle(260);check(sim.commands.size()==n,"foreign module receives no control frames");
    sim.welder[0]=WelderProto::Magic;check(waitFor([&]{return welder.ready();}),"read-only status recovers without output replay");check(sim.commands.size()==n,"reconnection never replays process outputs");
    // Allow the 200 ms poll to observe the final counter before the 1500 ms freshness deadline.
    sim.dynamic=false;check(waitFor([&]{return !welder.fresh();},2400)&&!welder.issue(WelderProto::Controls,2),"frozen scan blocks stale commands");sim.dynamic=true;check(waitFor([&]{return welder.ready();}),"new scan restores readiness");
    sim.ack=false;check(welder.issue(WelderProto::Controls,2),"start request with withheld acknowledgement");check(waitFor([&]{return !welder.pending();},3600),"missing application ack times out");const int h=sim.heartbeats;settle(600);check(sim.heartbeats==h,"uncertain request stops lease renewal");
    sim.ack=true;check(welder.issue(WelderProto::AllOff),"all-off allowed after uncertain request");check(waitFor([&]{return welder.safeIdle();}),"explicit stop recovers uncertain process state");
    for(int address:sim.writes)check(address==200||address==215||address==7000||address==7064,"all writes remain in non-overlapping protocol ranges");
    check(sim.invalid==0,"no unexpected register operations");
    BridgeProto::Status mode;for(int n=1;n<=3;++n){mode.rawStatus=quint16(0x10<<n);check(RobotModeDialog::feedbackMode(mode)==n,"three PLC mode bits decode independently");}mode.rawStatus=0x60;check(RobotModeDialog::feedbackMode(mode)==0,"conflicting mode bits stay unknown");
    bridge.disconnectFrom();check(waitFor([&]{return !welder.fresh();}),"disconnect invalidates process controls");
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
