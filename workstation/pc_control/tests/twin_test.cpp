#include "../workstation_settings.h"
#include "../workstation_twin.h"
#include <QTcpServer>
#include <QtEndian>
#include <QTest>
#include <iostream>
#include <memory>
#include <limits>
#include <cstring>

namespace {
int checks=0,failures=0;
void check(bool ok,const char* text){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<text<<'\n';}}
bool waitFor(std::function<bool()> f,int ms=2500){QElapsedTimer t;t.start();while(!f()&&t.elapsed()<ms)QTest::qWait(10);return f();}
void put(QByteArray& b,quint16 value){auto n=qToBigEndian(value);b.append(reinterpret_cast<const char*>(&n),2);}
quint16 get(const QByteArray& b,int i){return qFromBigEndian<quint16>(b.constData()+i);}
void set32(QVector<quint16>& w,int i,quint32 n){w[i]=quint16(n>>16);w[i+1]=quint16(n);}
QVector<quint16> packet(){QVector<quint16>w(40,0);w[0]=0x5053;w[1]=1;w[2]=13;w[3]=1;w[5]=1;w[7]=1;w[36]=0x0603;w[37]=12;w[38]=1000;return w;}
QVector<quint16> nativePacket(const TwinJoints& joints, quint32 counter=1, bool stopped=true){
    QVector<quint16> w(40,0);w[0]=0x4A52;w[1]=1;w[2]=stopped?11:9;w[5]=1;set32(w,6,counter);set32(w,32,counter);
    w[36]=0x0641;w[37]=6;w[38]=1;w[39]=40;
    for(int i=0;i<6;++i){float value=float(joints[i]);quint32 bits=0;std::memcpy(&bits,&value,sizeof(bits));w[10+2*i]=quint16(bits);w[11+2*i]=quint16(bits>>16);}
    return w;
}
class Plc:public QObject {
public:
    QTcpServer server;QTimer scan;
    QVector<quint16> main=QVector<quint16>(64,0),pose=packet();
    QVector<quint16> joints;
    int writes=0,poseReads=0,jointReads=0;bool scanning=true;
    Plc(){
        main[0]=BridgeProto::Magic;main[1]=2;main[2]=1|2|4|64|1024;main[11]=1;main[17]=0x94;
        main[57]=1;main[58]=255;main[60]=49;main[61]=56;main[63]=2;
        scan.setInterval(20);connect(&scan,&QTimer::timeout,this,[this]{if(scanning)++main[12];});scan.start();
        connect(&server,&QTcpServer::newConnection,this,[this]{
            auto* socket=server.nextPendingConnection();auto bytes=std::make_shared<QByteArray>();
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,this,[this,socket,bytes]{
                *bytes+=socket->readAll();
                while(bytes->size()>=12){int total=6+get(*bytes,4);if(bytes->size()<total)return;auto r=bytes->left(total);bytes->remove(0,total);
                    int fc=quint8(r[7]),start=get(r,8),count=get(r,10);QByteArray pdu;
                    if(fc!=3){++writes;pdu.append(char(fc|0x80)).append(char(1));}
                    else if((start==300&&count==64)||(start==450&&count==40)||(start==500&&count==40&&!joints.isEmpty())){
                        auto& w=start==300?main:start==500?joints:pose;if(start==450)++poseReads;if(start==500)++jointReads;
                        pdu.append(char(3)).append(char(w.size()*2));for(auto v:w)put(pdu,v);
                    }else pdu.append(char(0x83)).append(char(2));
                    QByteArray reply=r.left(4);put(reply,quint16(pdu.size()+1));reply.append(r[6]).append(pdu);socket->write(reply);
                }
            });
        });check(server.listen(QHostAddress::LocalHost,0),"loopback PLC listens");
    }
};
bool near(QVector3D a,QVector3D b){return (a-b).length()<.02f;}
}
int main(int argc,char** argv){
    QApplication app(argc,argv);QTemporaryDir settings;QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settings.path());
    const auto isolated = workstationSettings();
    check(isolated.fileName().startsWith(settings.path()),"test preferences are isolated from the running workstation");
    if(!isolated.fileName().startsWith(settings.path()))return 1;
    const QString assets=argc>1?QString::fromLocal8Bit(argv[1]):QString();
    auto w=packet();set32(w,10,quint32(qint32(-123456)));set32(w,12,quint32(qint32(-1)));set32(w,20,360000);
    auto p=TwinPacket::decode(w);check(p.valid&&std::abs(p.joints[0]+123.456)<1e-9,"negative scaled joint");
    check(std::abs(p.joints[1]+.001)<1e-9&&p.joints[5]==360,"signed subunit and sixth axis");
    for(int index:{0,1,3,5,7,36,37,38}){auto bad=w;bad[index]=0;check(!TwinPacket::decode(bad).valid,"reject invalid signature, protocol, boot, frame and sequence");}
    check(!TwinPacket::decode(w.mid(0,39)).valid,"reject truncated frame");
    {
        const TwinJoints expected{-123.456,-.125,45,90,-60,355};auto native=nativePacket(expected);
        auto decoded=TwinPacket::decode(native);
        check(decoded.valid&&decoded.nativeJoints&&!decoded.hasTcp&&std::abs(decoded.joints[0]-expected[0])<.0001&&decoded.joints[5]==355,
            "native independent feedback decodes actual six REAL axes without fabricating TCP");
        native[33]=2;check(!TwinPacket::decode(native).valid,"torn native publication counter is rejected");
        native=nativePacket(expected);native[11]=0x7fc0;native[10]=0;
        check(!TwinPacket::decode(native).valid,"native NaN cannot become a model angle");
    }
    auto bad=w;set32(bad,10,10000001);check(!TwinPacket::decode(bad).valid,"reject out of encoding range");
    TwinRig rig;QString error;check(rig.load(assets+"/sr175_rig.json",&error),"load supplied CAD joint rig");
    const QVector3D flange(2066,0,2297);
    auto m=rig.transforms({});check(near(m[6].map(flange),flange),"zero pose preserves original CAD placement");
    m=rig.transforms({90,0,0,0,0,0});check(near(m[0].map(flange),flange),"base remains fixed");check(near(m[6].map(flange),{0,2066,2297}),"J1 carries all descendants around base");
    m=rig.transforms({0,90,0,0,0,0});check(near(m[6].map(flange),{1965,0,-1074}),"J2 rotates around actual shoulder pivot");
    m=rig.transforms({0,0,90,0,0,0});check(near(m[6].map(flange),{630,0,261}),"J3 rotates around actual elbow pivot");
    m=rig.transforms({0,0,0,0,90,0});check(near(m[6].map(flange),{1841,0,2072}),"J5 bends 225mm flange offset");
    m=rig.transforms({0,0,0,0,0,90});check(near(m[6].map(flange),flange),"J6 rolls without moving flange center");
    {
        const TwinJoints start{25,-25,35,30,40,50};auto target=rig.flange(start);
        const auto expected=target.column(3).toVector3D()+QVector3D(100,0,332);target.setColumn(3,QVector4D(expected,1));
        const auto solved=rig.inverse(target,start);check(solved.has_value(),"fixed-orientation job endpoint has a joint-limit compliant ghost pose");
        if(solved){const auto actual=rig.flange(*solved);check((actual.column(3).toVector3D()-expected).length()<.2f,"ghost flange reaches the requested WORLD endpoint");
            bool orientation=true,limits=true;for(int i=0;i<3;++i)orientation=orientation&&(actual.column(i)-target.column(i)).length()<.002f;
            for(int i=0;i<6;++i)limits=limits&&(*solved)[i]>=rig.lower[i]&&(*solved)[i]<=rig.upper[i];
            check(orientation&&limits,"endpoint preserves start orientation within joint limits");}
        target.setColumn(3,QVector4D(18000,0,0,1));check(!rig.inverse(target,start),"unreachable endpoint never produces a fabricated pose");
        TwinTrajectory plan;plan.setPath({{0,0,0},{80,0,80},{100,0,332}},rig);
        check(!plan.anchored&&!plan.endpointDistance(),"reference preview cannot report actual endpoint arrival");
        const QVector3D tcp(1200,500,1800);plan.bind(start,rig,tcp);
        check(near(plan.path.first(),rig.flange(start).column(3).toVector3D())&&near(plan.path.last()-plan.path.first(),{100,0,332}),"path uses one common model frame and WORLD relative offsets");
        const auto anchor=plan.anchor;const auto ghost=plan.goal;plan.currentTcp=tcp+QVector3D(100,0,330);
        check(plan.endpointDistance()&&std::abs(*plan.endpointDistance()-2)<.001,"end comparison uses measured TCP rather than planned animation");
        check(plan.anchor==anchor&&plan.goal==ghost,"new end snapshot cannot move the fixed origin or target ghost");
        plan.clearAnchor(rig);check(!plan.anchored&&!plan.anchorTcp&&!plan.currentTcp&&!plan.endpointDistance(),"source reset clears measurement comparison and bound origin");
        plan.setPath({{0,0,0},{std::numeric_limits<double>::quiet_NaN(),0,0}},rig);check(plan.path.isEmpty()&&!plan.goal,"invalid path removes old route and ghost");
    }
    {
        Plc sim;PlcBridgeClient bridge;TwinPlcReader reader(bridge);reader.setEnabled(true);
        bridge.connectTo("127.0.0.1",sim.server.serverPort(),1);
        check(waitFor([&]{return reader.fresh();}),"read-only reader accepts fresh complete PLC snapshot");
        check(!bridge.ownsSession()&&sim.poseReads>0&&sim.writes==0,"monitor never registers or writes commands or heartbeats");
        sim.pose[2]=12;check(waitFor([&]{return !reader.fresh();}),"moving or invalid snapshot is withheld");
        sim.pose[2]=13;sim.pose[7]=2;check(waitFor([&]{return reader.fresh();}),"post-motion snapshot becomes usable");
        sim.pose[5]=2;check(waitFor([&]{return !reader.fresh();}),"reject PLC boot mismatch");
        sim.pose[5]=1;sim.pose[7]=3;check(waitFor([&]{return reader.fresh();}),"recover on coherent boot and new counter");
        sim.pose[8]=8000;check(waitFor([&]{return !reader.fresh();}),"reject expired PLC age");
        sim.pose[8]=0;sim.pose[7]=4;check(waitFor([&]{return reader.fresh();}),"accept new snapshot after expiry");
        QTest::qWait(8150);check(!reader.fresh(),"repeated frozen counter expires despite successful reads");
        sim.pose[7]=5;check(waitFor([&]{return reader.fresh();}),"new counter clears frozen state");
        sim.scanning=false;check(waitFor([&]{return !reader.fresh();}),"stalled PLC scan invalidates pose");
        check(waitFor([&]{return !bridge.isActive();}),"stalled scan closes main connection before explicit reconnect");
        sim.scanning=true;sim.pose[7]=6;bridge.connectTo("127.0.0.1",sim.server.serverPort(),1);
        check(waitFor([&]{return reader.fresh();}),"explicit reconnect and dynamic PLC scan restore pose");
        bridge.disconnectFrom();check(waitFor([&]{return !reader.fresh()&&!reader.packet().valid;}),"disconnect clears acquisition state");
        check(sim.writes==0,"entire monitor lifecycle sends zero write requests");
    }
    {
        Plc sim;const TwinJoints first{25,-25,35,30,40,50};sim.joints=nativePacket(first);
        PlcBridgeClient bridge;TwinPanel panel(bridge,assets);TwinView scene;scene.loadModel(assets);
        panel.setCompanionView(&scene);panel.setJobPath({{0,0,0},{100,0,332}});panel.setContext(true,true,false);
        bridge.connectTo("127.0.0.1",sim.server.serverPort(),1);
        check(waitFor([&]{return panel.view()->currentPose()==first;}),"connecting PLC alone loads all six real axes without entering manual service");
        const auto anchor=scene.trajectory().anchor;const auto ghost=scene.trajectory().goal;
        check(!bridge.ownsSession()&&sim.writes==0&&sim.jointReads>0&&sim.poseReads==0&&scene.currentPose()==first,
            "independent pose and large scene require only reads and never enter or load a service");
        const TwinJoints moving{30,-20,40,35,45,55};sim.joints=nativePacket(moving,2,false);
        check(waitFor([&]{return scene.currentPose()==moving;}),"new six-axis feedback updates the model during motion");
        check(scene.trajectory().anchor==anchor&&scene.trajectory().goal==ghost&&!panel.canBindPreview()&&!scene.trajectory().endpointDistance(),
            "running feedback preserves fixed preview origin and ghost and does not invent measured TCP arrival");
        sim.joints[8]=1500;check(waitFor([&]{return panel.findChild<QLabel*>("twinState")->text().contains(QString::fromUtf8("保留"));}),
            "expired native feedback holds actual pose and visibly marks it stale");
        check(scene.currentPose()==moving,"stale native data cannot reset model to zero");
        sim.joints=nativePacket(first,3);check(waitFor([&]{return scene.currentPose()==first&&panel.canBindPreview();}),
            "fresh stopped feedback restores the shared model and optional origin alignment");
        bridge.disconnectFrom();check(waitFor([&]{return panel.findChild<QLabel*>("twinState")->text().contains(QString::fromUtf8("保留"));}),
            "disconnect removes live feedback status");
        check(sim.writes==0,"entire continuous feedback lifecycle sends zero writes");
    }
    {
        PlcBridgeClient bridge;TwinPanel panel(bridge,assets);panel.resize(900,330);panel.show();QTest::qWait(700);
        check(panel.view()->vertexCount()>100000&&panel.view()->error().isEmpty(),"original STEP mesh renders");
        check(panel.findChild<QLabel*>("twinJoint1")->text()==QString::fromUtf8("—"),"offline reference is not fabricated feedback");
        check(panel.view()->background()==QColor("#e9f0f6"),"embedded model defaults to light background");
        panel.findChild<QAction*>("twinBackground3")->trigger();QTest::qWait(100);
        check(panel.view()->background()==QColor("#18222f"),"background menu switches actual canvas color");
        panel.setBackgroundPreset(4,QColor("#b3c9a8"));
        check(workstationSettings().value("twin/backgroundCustom").toString()=="#b3c9a8","custom background selection persists");
        {TwinPanel restored(bridge,assets);check(restored.view()->background()==QColor("#b3c9a8"),"new model card restores custom background");}
        panel.setBackgroundPreset(0);
        TwinView scene;scene.loadModel(assets);panel.setCompanionView(&scene);panel.setJobPath({{0,0,0},{100,0,332}});
        panel.setDemoPose({20,-15,30,40,25,60});QTest::qWait(100);
        check(panel.findChild<QLabel*>("twinJoint6")->text()=="60.000","offline demo drives all six joints");
        if(argc>2)panel.grab().save(QString::fromLocal8Bit(argv[2]));
        panel.setContext(false,true,true);panel.acceptSdk("1,2,3,4,5,6",false);QTest::qWait(100);
        check(!panel.findChild<QPushButton*>("twinDemo")->isEnabled(),"live connection disables demo");
        check(!panel.findChild<QAction*>("twinDemoAction")->isEnabled(),"live connection disables demo menu entry");
        check(panel.findChild<QLabel*>("twinJoint6")->text()=="6.000","stationary session snapshot updates pose");
        panel.acceptSdk("25,-25,35,30,40,50",false,"1200,500,1800");panel.bindPreviewOrigin();
        const auto start=scene.trajectory();check(start.anchored&&start.goal&&scene.currentPose()==panel.view()->currentPose(),"large scene and compact card share actual start snapshot");
        panel.setBackgroundPreset(2);check(scene.background()==panel.view()->background(),"one background menu updates both embedded scenes");
        if(start.goal){QStringList axes;for(double q:*start.goal)axes<<QString::number(q,'f',8);panel.acceptSdk(axes.join(','),false,"1300,500,2132");
            check(scene.trajectory().anchor==start.anchor&&scene.trajectory().goal==start.goal,"end snapshot updates actual robot while keeping planned ghost fixed");
            check(scene.trajectory().endpointDistance()&&*scene.trajectory().endpointDistance()<.001,"only measured end TCP reports arrival");
            check(scene.currentPose()!=start.anchor,"actual mesh advances on a new stationary snapshot");}
        const auto held=scene.currentPose();panel.setContext(false,true,false);
        check(scene.currentPose()==held&&scene.trajectory().goal==start.goal,"moving or missing fresh feedback holds both actual snapshot and fixed target");
        check(!panel.canBindPreview(),"moving or stale state cannot redefine the preview origin");
        panel.acceptSdk("1,2,nan,4,5,6",false);check(panel.findChild<QLabel*>("twinState")->text().contains(QString::fromUtf8("保留")),"invalid SDK angle retains last pose with stale label");
        panel.setContext(true,false,false);check(panel.findChild<QLabel*>("twinJoint6")->text()==QString::fromUtf8("—"),"changing acquisition source clears old angles");
        check(!scene.trajectory().anchored&&!scene.trajectory().endpointDistance(),"changing source also clears large scene start alignment");
    }
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
