#include "../workstation_plcbridge.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTcpServer>
#include <QtEndian>
#include <cstring>
#include <iostream>
#include <functional>
#include <memory>
#include <limits>
#ifdef STEP_PLC_PANEL_TEST
#include "../workstation_ui.h"
#include "../workstation_livemonitor.h"
#include "../workstation_plcbridgepanel.h"
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#endif

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* text) {
    ++checks;
    if (!ok) { ++failures; std::cerr << "FAIL: " << text << '\n'; }
}
bool waitFor(const std::function<bool()>& predicate, int ms = 2500) {
    QElapsedTimer t; t.start();
    while (!predicate() && t.elapsed() < ms) {
        QEventLoop loop; QTimer::singleShot(10, &loop, &QEventLoop::quit); loop.exec();
    }
    return predicate();
}
void settle(int ms) { waitFor([] { return false; }, ms); }
quint16 get(const QByteArray& b, int at) { return qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(b.constData() + at)); }
void put(QByteArray& b, quint16 w) { const auto n=qToBigEndian(w); b.append(reinterpret_cast<const char*>(&n),2); }
void set32(QVector<quint16>& w, int at, quint32 n) { w[at]=quint16(n>>16); w[at+1]=quint16(n); }
quint32 get32(const QVector<quint16>& w, int at) { return (quint32(w[at])<<16)|w[at+1]; }

// Loopback-only contract simulator. No real device IP or automatic motion.
class Simulator : public QObject {
public:
    QTcpServer server;
    QTimer scan;
    QVector<quint16> status = QVector<quint16>(64,0);
    QVector<QVector<quint16>> commands;
    QVector<int> heartbeatAddresses;
    bool dynamic = true, finishActions = true, rejectNext = false;
    int reads = 0, invalidRequests = 0;
    Simulator(quint16 version = 1) {
        status[0]=BridgeProto::Magic; status[1]=version; status[2]=1|2|4|64;
        if(version==2) { status[2]|=1024;status[57]=1;status[58]=255;status[60]=49;status[61]=56;status[63]=2; }
        status[17]=0x90; set32(status,10,0x12345678u);
        for(int i=0;i<32;++i) status[20+i]=quint16(1000+i);
        scan.setInterval(25);
        connect(&scan,&QTimer::timeout,this,[this] { if(dynamic) ++status[12]; });
        scan.start();
        connect(&server,&QTcpServer::newConnection,this,[this] {
            auto* socket=server.nextPendingConnection();
            auto buffer=std::make_shared<QByteArray>();
            connect(socket,&QTcpSocket::readyRead,this,[this,socket,buffer] {
                *buffer+=socket->readAll();
                while(buffer->size()>=8) {
                    int total=6+get(*buffer,4);
                    if(buffer->size()<total)return;
                    const auto request=buffer->left(total);buffer->remove(0,total);
                    const int fc=quint8(request[7]);
                    QByteArray pdu;
                    if(fc==3 && request.size()==12 && get(request,8)==300 && get(request,10)==64) {
                        status[20]=status[17];
                        ++reads;pdu.append(char(3)).append(char(128));
                        for(auto value:status)put(pdu,value);
                    } else if(fc==16 && request.size()==45 && get(request,8)==200 && get(request,10)==16 && quint8(request[12])==32) {
                        QVector<quint16> words;for(int i=0;i<16;++i)words<<get(request,13+2*i);
                        commands<<words; apply(words);
                        pdu.append(char(16));put(pdu,200);put(pdu,16);
                    } else if(fc==6 && request.size()==12 && get(request,8)==215) {
                        heartbeatAddresses<<get(request,8); status[9]=get(request,10);
                        pdu=request.mid(7);
                    } else { ++invalidRequests; pdu.append(char(fc|0x80)).append(char(2)); }
                    QByteArray response=request.left(4);put(response,quint16(1+pdu.size()));
                    response.append(request[6]);response+=pdu;socket->write(response);
                }
            });
        });
        check(server.listen(QHostAddress::LocalHost),"simulator listens on loopback");
    }
    void apply(const QVector<quint16>& w) {
        const auto seq=get32(w,2), owner=get32(w,6);
        if(rejectNext) { rejectNext=false;set32(status,52,seq);status[54]=9;return; }
        if(w[1]==BridgeProto::Stop && (status[2]&32)) set32(status,55,get32(status,4));
        set32(status,4,seq);status[6]=w[1];status[7]=2;status[8]=0;
        status[9]=w[15];
        if(w[0]!=status[1] || get32(w,8)!=get32(status,10)) { status[7]=4;status[8]=3;return; }
        if(w[1]==BridgeProto::Register) { status[2]|=8|16;set32(status,15,owner);return; }
        if(!finishActions && w[1]!=BridgeProto::Stop) { status[7]=1;status[2]|=32;return; }
        status[2]&=quint16(~32);
        switch(w[1]) {
        case BridgeProto::Enable:status[17]|=4;break;
        case BridgeProto::Disable:status[17]&=quint16(~4);break;
        case BridgeProto::Load:
            if(w[4]<1 || w[4]>(status[1]==2?255:4)) {status[7]=4;status[8]=8;break;}
            status[13]=w[4];
            if(status[1]==2) {status[14]&=quint16(~15);status[59]=w[4];}
            else status[14]=(status[14]&quint16(~15))|(1<<(w[4]-1));
            status[2]&=quint16(~512);status[7]=3;break;
        case BridgeProto::ConfirmLoad:status[2]|=512;break;
        case BridgeProto::Run:status[17]=(status[17]&quint16(~0x18))|1;break;
        case BridgeProto::Pause:status[17]=(status[17]&quint16(~1))|8;break;
        case BridgeProto::Stop:status[17]=(status[17]&quint16(~9))|16;status[2]&=quint16(~512);break;
        case BridgeProto::ClearError:status[17]&=quint16(~2);break;
        case BridgeProto::Rearm:status[2]&=quint16(~256);break;
        case BridgeProto::Coordinates:status[2]=(status[2]&quint16(~128))|(w[4]?128:0);status[14]=(status[14]&quint16(~4096))|(w[4]?4096:0);status[7]=3;break;
        case BridgeProto::Logout:status[2]&=quint16(~(8|16));set32(status,15,0);break;
        }
    }
};
struct Fixture {
    Simulator sim;
    PlcBridgeClient client;
    Fixture(quint16 version = 1) : sim(version) { client.connectTo("127.0.0.1",sim.server.serverPort());check(waitFor([this]{return client.fresh();}),"signature and advancing PLC scan verified"); }
    void reg() { check(client.issue(BridgeProto::Register),"explicit register issued");check(waitFor([this]{return client.ownsSession()&&!client.pending();}),"matching application ack registers owner"); }
    void command(BridgeProto::Command c, quint16 p=0) {
        check(client.issue(c,p),"allowed command issued");
        check(waitFor([this]{return !client.pending();}),"matching PLC application ack completes request");
    }
};
}
int main(int argc,char** argv) {
#ifdef STEP_PLC_PANEL_TEST
    QApplication app(argc,argv);app.setStyle("Fusion");app.setFont(QFont("Microsoft YaHei UI",11));app.setStyleSheet(WorkstationUi::styleSheet());
#else
    QCoreApplication app(argc,argv);
#endif
    using namespace BridgeProto;
#ifdef STEP_PLC_PANEL_TEST
    {
        Fixture f; PlcBridgePanel panel(f.client); panel.setModeEnabled(true); panel.resize(1240,900); panel.show();
        PlcLiveMonitor monitor(f.client);monitor.append("local monitor check");
        check(monitor.findChild<QPlainTextEdit*>("liveEventLog")->toPlainText().contains("local monitor check"),"persistent monitor retains events");
        auto snapshot=[&](const QString& name){
            const auto folder=qEnvironmentVariable("STEP_UI_PANEL_SNAPSHOT_DIR");if(folder.isEmpty())return;
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);check(panel.grab().save(folder+"/panel_"+name+".png"),"panel state screenshot saved");
        };
        auto* tabs=panel.findChild<QTabWidget*>("plcBridgeDetailTabs");
        auto* io=panel.findChild<QTableWidget*>("plcBridgeIoTable");
        auto* ao=panel.findChild<QTableWidget*>("plcBridgeAnalogTable");
        auto* diag=panel.findChild<QTableWidget*>("plcBridgeDiagnosticTable");
        auto* coverage=panel.findChild<QTableWidget*>("plcBridgeCoverageTable");
        auto* selection=panel.findChild<QSpinBox*>("plcBridgeSlot");
        auto* coord=panel.findChild<QComboBox*>("plcBridgeCoordinates");
        auto* xyz=panel.findChild<QLabel*>("plcBridgeCoordinatesReadout");
        check(tabs&&tabs->count()==5,"all five monitoring/diagnostic pages exist");
        check(io&&io->rowCount()==96,"all 32 desired DI and 64 DO rows exist");
        check(ao&&ao->rowCount()==28,"all AO5 through AO32 rows exist");
        check(diag&&diag->rowCount()==64,"complete 64-word PLC status table exists");
        check(coverage&&coverage->rowCount()==17,"all commands and local-only boundaries visible");
        if(!tabs||!io||!ao||!diag||!coverage||!selection||!coord||!xyz) return 1;
        QMap<int,QPushButton*> buttons;
        for(int c=1;c<=12;++c) {
            auto* b=panel.findChild<QPushButton*>(QString("plcBridgeCommand%1").arg(c));
            check(b,"PLC command has an actual UI button"); if(!b)return 1;buttons[c]=b;
        }
        check(buttons[Register]->isEnabled()&&!buttons[Enable]->isEnabled(),"only registration is allowed before ownership");
        snapshot("registration");
        check(coverage->item(Enable-1,3)->text().contains("登记"),"disabled enable reason is visible in table");
        buttons[Register]->click();check(waitFor([&]{return f.client.ownsSession()&&!f.client.pending();}),"UI register submits and confirms ownership");
        check(buttons[Register]->property("actionState")=="done","registration badge comes from confirmed ownership");
        check(panel.findChild<QLabel*>("plcSessionBadge")->text().contains("已登记"),"session completion has an explicit label");
        selection->setValue(4);buttons[Enable]->click();
        check(waitFor([&]{return f.client.status().enabled()&&!f.client.pending();}),"UI enable confirmed");
        buttons[Load]->click();snapshot("pending");check(waitFor([&]{return f.client.status().selectedSlot==4&&!f.client.pending();}),"UI loads chosen slot4");
        snapshot("confirm_load");
        check(f.sim.commands.last()[1]==Load&&f.sim.commands.last()[4]==4,"UI sends correct slot parameter");
        auto answer=[](int value){QTimer::singleShot(0,qApp,[value]{
            if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                if(auto* button=box->button(QMessageBox::StandardButton(value)))button->click();
        });};
        auto count=f.sim.commands.size(); answer(QMessageBox::No);buttons[ConfirmLoad]->click();settle(50);
        check(f.sim.commands.size()==count&&!f.client.status().has(LoadConfirmed),"declining manual confirmation sends no command");
        answer(QMessageBox::Yes);buttons[ConfirmLoad]->click();
        check(waitFor([&]{return f.client.status().has(LoadConfirmed)&&!f.client.pending();}),"explicit manual confirmation button maps to command12");
        check(panel.findChild<QLabel*>("plcLoadBadge")->text().contains("已人工确认"),"load badge distinguishes human confirmation from sending");
        check(monitor.findChild<QLabel*>("liveReceipt")->text().contains("确认"),"persistent monitor shows actual application receipt");
        snapshot("ready");
        count=f.sim.commands.size();answer(QMessageBox::No);buttons[Run]->click();settle(50);
        check(f.sim.commands.size()==count,"declining run confirmation sends no motion");
        answer(QMessageBox::Yes);buttons[Run]->click();
        check(waitFor([&]{return f.client.status().running()&&!f.client.pending();}),"UI run confirmed");
        snapshot("running");
        buttons[Pause]->click();check(waitFor([&]{return f.client.status().paused()&&!f.client.pending();}),"UI pause confirmed");
        buttons[Stop]->click();check(waitFor([&]{return f.client.status().stopped()&&!f.client.pending();}),"UI stop confirmed");
        coord->setCurrentIndex(1);buttons[Coordinates]->click();
        check(waitFor([&]{return f.client.status().has(CoordinatesRequested)&&!f.client.pending();}),"UI coordinate selector sends parameter1");
        coord->setCurrentIndex(0);buttons[Coordinates]->click();
        check(waitFor([&]{return !f.client.status().has(CoordinatesRequested)&&!f.client.pending();}),"UI coordinate selector sends parameter0");
        f.sim.status[17]|=2;f.sim.status[2]|=StopLatch;
        check(waitFor([&]{return f.client.status().alarm()&&f.client.status().has(StopLatch);}),"alarm and stop latch feedback visible");
        check(buttons[ClearError]->isEnabled()&&!buttons[Rearm]->isEnabled(),"clear available while latched; rearm blocked until alarm clear");
        buttons[ClearError]->click();check(waitFor([&]{return !f.client.status().alarm()&&!f.client.pending();}),"UI clear-error confirmed");
        check(f.client.status().has(StopLatch),"acknowledgement preserves stop latch");
        check(buttons[Rearm]->isEnabled()&&!buttons[Enable]->isEnabled(),"latch reset enabled while enable remains blocked");
        buttons[Rearm]->click();check(waitFor([&]{return !f.client.status().has(StopLatch)&&!f.client.pending();}),"UI explicit latch reset confirmed");
        buttons[Disable]->click();check(waitFor([&]{return !f.client.status().enabled()&&!f.client.pending();}),"UI disable confirmed");
        buttons[Logout]->click();check(waitFor([&]{return !f.client.ownsSession()&&!f.client.pending();}),"UI logout releases ownership");
        check(!buttons[Enable]->isEnabled()&&buttons[Register]->isEnabled(),"logout requires new explicit registration");
        buttons[Register]->click();check(waitFor([&]{return f.client.ownsSession()&&!f.client.pending();}),"UI can explicitly register again");
        f.sim.status[14]=0x1008;f.sim.status[21]=0x8001;f.sim.status[23]=0x8000;f.sim.status[24]=65535;
        f.sim.status[2]|=CoordinatesRequested;
        const float coordinate=-123.5f;quint32 bits;std::memcpy(&bits,&coordinate,4);f.sim.status[40]=quint16(bits);f.sim.status[41]=quint16(bits>>16);
        check(waitFor([&]{return f.client.status().desiredWord==0x1008;}),"mapped diagnostic values received");
        check(io->item(3,3)->text()=="1 / ON"&&io->item(12,3)->text()=="1 / ON","DI slot4 and coordinate request decoded from desired word");
        check(io->item(48,3)->text()=="1 / ON"&&io->item(95,3)->text()=="1 / ON","DO49 and DO96 include correct word boundaries");
        check(ao->item(0,3)->text()=="65535","AO word remains unsigned and unscaled");
        check(xyz->text().contains("X=-123.500"),"coordinate REAL correctly displayed from low-word-first pair");
        check(diag->item(14,3)->text()=="0X1008","complete status table contains desired system word");
        check(coverage->item(12,3)->text().contains("仅显示"),"local commissioning is not falsely writable");
        auto* filter=panel.findChild<QComboBox*>("plcBridgeIoFilter");
        check(filter&&io->isRowHidden(0)&&!io->isRowHidden(32),"I/O page initially shows robot feedback");
        if(filter){filter->setCurrentIndex(1);check(!io->isRowHidden(0)&&io->isRowHidden(32),"I/O filter distinguishes desired outputs from feedback");}
        panel.setModeEnabled(false);bool blocked=true;for(auto* b:buttons)blocked=blocked&&!b->isEnabled();
        check(blocked,"all PLC buttons disabled outside PLC mode");panel.setModeEnabled(true);
        f.sim.status[2]&=quint16(~RobotOnline);check(waitFor([&]{return !f.client.status().has(RobotOnline);}),"robot offline transition received");
        check(io->item(32,3)->text().contains("无有效")&&ao->item(0,3)->text()=="—"&&diag->item(20,3)->text()=="—","robot offline hides stale DO/AO/raw mirror values");
        check(io->item(3,3)->text()=="1 / ON","PLC desired word stays labeled as desired while robot offline");
        f.sim.dynamic=false;check(waitFor([&]{return !f.client.isConnected();},2500),"UI loses freshness when PLC scan freezes");
        blocked=true;for(auto* b:buttons)blocked=blocked&&!b->isEnabled();
        check(blocked&&diag->item(14,3)->text()=="—","frozen PLC disables all commands and clears current diagnostics");
        check(monitor.findChild<QLabel*>("liveRobotState")->text().contains("未知"),"persistent monitor drops stale robot state");
    }

    {
        Fixture f(2); PlcBridgePanel panel(f.client); panel.setModeEnabled(true);panel.resize(840,840);panel.show();
        auto* selection=panel.findChild<QSpinBox*>("plcBridgeSlot");
        auto* info=panel.findChild<QLabel*>("plcBridgeProgramCapability");
        auto* io=panel.findChild<QTableWidget*>("plcBridgeIoTable");
        auto* diag=panel.findChild<QTableWidget*>("plcBridgeDiagnosticTable");
        auto* load=panel.findChild<QPushButton*>("plcBridgeCommand4");
        auto* confirm=panel.findChild<QPushButton*>("plcBridgeCommand12");
        check(selection&&info&&io&&diag&&load&&confirm,"V2 controls and capability readout exist");
        if(!selection||!info||!io||!diag||!load||!confirm)return 1;
        check(selection->maximum()==255&&info->text().contains("1～255"),"V2 picker exposes verified 1..255 capability");
        f.reg();
        for(int number:{16,255}) {
            selection->setValue(number);load->click();
            check(waitFor([&]{return !f.client.pending()&&f.client.status().selectedSlot==number;}),"V2 load button sends selected number including16 and255");
            check(f.sim.commands.last()[0]==2&&f.sim.commands.last()[4]==number,"V2 UI uses version2 and exact program number");
        }
        check(io->item(16,3)->text()=="1 / ON"&&io->item(23,3)->text()=="1 / ON"&&io->item(24,3)->text()=="0 / OFF",
              "V2 GDI program255 displays eight bits and zero reserved bits");
        check(io->item(16,1)->text()=="HR359.0"&&diag->item(58,4)->text()=="255","V2 diagnostics expose capacity and second output word");
        check(!f.client.status().running()&&!f.client.status().has(LoadConfirmed),"V2 high-number load never confirms or runs automatically");
        QTimer::singleShot(0,qApp,[]{if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            if(auto* b=box->button(QMessageBox::Yes))b->click();});
        confirm->click();check(waitFor([&]{return !f.client.pending()&&f.client.status().has(LoadConfirmed);}),"program255 manual confirmation is supported");
        check(f.sim.commands.last()[4]==255&&!f.client.status().running(),"program255 confirmation does not run");
        f.sim.status[2]&=quint16(~GdiMappingConfirmed);
        check(waitFor([&]{return !load->isEnabled();}),"missing local GDI confirmation disables loading");
        if(qEnvironmentVariableIsSet("STEP_GDI255_PANEL_SNAPSHOT"))
            check(panel.grab().save(qEnvironmentVariable("STEP_GDI255_PANEL_SNAPSHOT")),"V2 panel screenshot saved");
    }
    std::cout<<"plcbridge_panel_test: "<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
#else
    {
        QVector<quint16> w(64,0);w[0]=Magic;w[1]=LegacyVersion;w[2]=RobotOnline|CoordinatesRequested;
        set32(w,10,0x12345678);set32(w,4,0xABCDEF12);
        const float expected=-123.5f;quint32 bits;std::memcpy(&bits,&expected,4);
        w[40]=quint16(bits);w[41]=quint16(bits>>16);
        auto s=decode(w);check(s.boot==0x12345678 && s.ackSeq==0xABCDEF12,"metadata high word first");
        check(s.coordinate(0).has_value() && *s.coordinate(0)==expected,"robot REAL low word first");
        check(!s.coordinate(6).has_value(),"out of range coordinate rejected");
        w[0]=0;check(!decode(w).valid,"wrong signature rejected");
        w[0]=Magic;w[1]=3;check(!decode(w).valid,"unknown bridge version rejected");
    }
    {
        QVector<quint16> w(64,0);w[0]=Magic;w[1]=LegacyVersion;w[2]=RobotOnline|CoordinatesRequested;w[63]=0xBEEF;
        auto s=decode(w);check(s.rawWords[0]==Magic&&s.rawWords[63]==0xBEEF,"all 64 raw status words retained");
        for(int n=33;n<=96;++n) {
            for(int i=20;i<24;++i)w[i]=0;
            w[20+(n-33)/16]=quint16(1)<<(n-33)%16;s=decode(w);
            check(s.digitalOutput(n)==std::optional<bool>(true),"DO33 through DO96 map all word/bit boundaries");
            if(n<96)check(s.digitalOutput(n+1)==std::optional<bool>(false),"adjacent DO remains off");
        }
        check(!s.digitalOutput(32)&&!s.digitalOutput(97),"DO outside mapped range rejected");
        for(int n=33;n<=48;++n) { w[14]=quint16(1)<<(n-33);s=decode(w);check(s.desiredInput(n)==std::optional<bool>(true),"desired DI33 through DI48 bit mapping"); }
        check(!s.desiredInput(32)&&!s.desiredInput(49),"unmapped DI cannot be exposed as control");
        for(int n=5;n<=32;++n) {w[20+n-1]=quint16(60000+n);s=decode(w);check(s.analogRaw(n)==std::optional<quint16>(60000+n),"AO5 through AO32 retain exact unsigned WORD");}
        w[40]=0;w[41]=0x7FC0;s=decode(w);check(!s.coordinate(0),"non-finite coordinate suppressed");
        w[2]=0;s=decode(w);check(!s.digitalOutput(33)&&!s.analogRaw(5)&&!s.coordinate(0),"offline robot feedback is invalid");
        check(s.desiredInput(48).has_value(),"PLC desired output is independent of feedback validity");
    }
    {
        Fixture f;f.reg();
        for(quint16 slot=1;slot<=4;++slot) {f.command(Load,slot);check(f.client.status().selectedSlot==slot,"all four program slots selectable");}
        f.command(Coordinates,1);check(f.client.status().has(CoordinatesRequested)&&f.client.status().result==SentUnconfirmed,"coordinate mode1 stays unconfirmed");
        f.command(Coordinates,0);check(!f.client.status().has(CoordinatesRequested),"AO mode0 supported");
        check(!f.client.issue(Coordinates,2)&&!f.client.issue(Load,5),"unsupported coordinate/slot parameters blocked");
        f.sim.status[17]|=2;f.sim.status[2]|=StopLatch;
        check(waitFor([&]{return f.client.status().alarm()&&f.client.status().has(StopLatch);}),"latched alarm observed before clear");
        check(!f.client.issue(Rearm),"alarm blocks latch reset before acknowledgement");
        f.command(ClearError);check(!f.client.status().alarm(),"clear-error command confirmed");
        check(f.client.status().has(StopLatch),"communications stop latch survives acknowledgement");
        const auto beforeAck=f.sim.commands.size();f.command(ClearError);
        check(f.sim.commands.size()==beforeAck+1&&f.sim.commands.last()[1]==ClearError,"explicit acknowledgement sent even with low alarm summary");
        f.sim.status[17]|=0x100;check(waitFor([&]{return f.client.status().emergency();}),"emergency observed");
        check(!f.client.issue(ClearError)&&!f.client.issue(Rearm),"acknowledgement and latch reset blocked in emergency");
        f.sim.status[17]&=quint16(~0x100);check(waitFor([&]{return !f.client.status().emergency();}),"emergency released");
        check(!f.client.issue(Enable),"stop latch blocks enable");
        f.command(Rearm);check(!f.client.status().has(StopLatch),"explicit rearm clears latch");
        check(!f.client.status().has(LoadConfirmed)&&!f.client.status().running(),"rearm never implies loaded program or automatic run");
        f.sim.status[2]&=quint16(~Commissioned);check(waitFor([&]{return !f.client.status().has(Commissioned);}),"local commissioning revocation observed");
        check(!f.client.issue(Enable)&&!f.client.issue(Coordinates,1),"PC cannot bypass local commissioning");
    }
    {
        Fixture f;settle(550);
        check(f.sim.commands.isEmpty() && f.sim.heartbeatAddresses.isEmpty(),"connect is read only, no automatic registration/heartbeat");
        check(!f.client.issue(Enable),"motion rejected before explicit registration");
        f.reg();settle(600);
        check(f.sim.heartbeatAddresses.size()>=2,"registered owner heartbeat is maintained at HR215");
        const auto registration=f.sim.commands.first();
        check(registration.size()==16 && registration[0]==1 && registration[1]==1,"full 16 word FC16 command frame");
        check(get32(registration,6)==f.client.sessionId() && get32(registration,8)==0x12345678,"session and boot sent high word first");
        check(registration[5]==0 && registration[10]==0 && registration[11]==0 && registration[12]==0 &&
              registration[13]==0 && registration[14]==0,"all reserved command words zero");
        f.command(Enable);
        check(f.client.status().enabled(),"enable confirmed by robot feedback");
        f.command(Load,3);
        check(f.client.status().result==SentUnconfirmed && !f.client.status().has(LoadConfirmed),"load is sent/unconfirmed");
        const auto before=f.sim.commands.size();settle(350);
        check(f.sim.commands.size()==before,"load does not auto-confirm or auto-run");
        check(!f.client.issue(Run),"run blocked before manual loaded-program confirmation");
        check(!f.client.issue(ConfirmLoad,2),"wrong program slot cannot be confirmed");
        f.command(ConfirmLoad,3);f.command(Run);
        check(f.client.status().running(),"run feedback parsed");
        f.command(Pause);check(f.client.status().paused(),"pause feedback parsed");
        f.command(Run);f.command(Stop);check(f.client.status().stopped(),"stop feedback parsed");
        f.command(Disable);f.command(Logout);
        check(!f.client.ownsSession(),"explicit logout releases session");
        check(f.sim.invalidRequests==0,"only FC03 HR300, FC16 HR200 and FC06 HR215 used");
    }
    {
        Fixture f;f.reg();f.sim.finishActions=false;check(f.client.issue(Enable),"deferred enable issued");
        settle(700);check(f.client.pending(),"FC16 transport receipt does not complete action");
        check(!f.client.issue(Load,1),"busy command cannot replace pending command");
        check(f.client.issue(Stop),"STOP can preempt pending command");
        check(waitFor([&]{return !f.client.pending();}),"STOP application completion observed");
        check(f.sim.commands.size()==3 && f.sim.commands[1][1]==Enable && f.sim.commands[2][1]==Stop,"queued enable/STOP never overwritten by heartbeat");
        check(get32(f.sim.commands[2],2)>get32(f.sim.commands[1],2),"preempting STOP has newer sequence");
    }
    {
        Fixture f;f.reg();f.sim.status[2]&=quint16(~RemotePermit);
        check(waitFor([&]{return !f.client.status().has(RemotePermit);}),"revoked remote permit observed");
        check(!f.client.issue(Enable),"enable blocked after permit loss");
        f.command(Stop);
    }
    {
        Fixture f;f.reg();f.sim.rejectNext=true;f.command(Enable);
        check(f.client.status().rejectReason==9,"separate rejected-sequence report observed");
        check(!f.client.status().enabled(),"rejected command not reported as enabled");
    }
    {
        Fixture f;f.reg();const auto count=f.sim.commands.size();
        f.sim.dynamic=false;
        check(waitFor([&]{return !f.client.isConnected();},2500),"frozen PLC scan invalidates TCP-connected status");
        settle(350);check(f.sim.commands.size()==count,"no re-register/replay after frozen scan");
    }
    {
        Fixture f;f.reg();const auto count=f.sim.commands.size();set32(f.sim.status,10,0x12345679);
        check(waitFor([&]{return !f.client.isConnected();}),"PLC boot change disconnects and clears session");
        check(f.sim.commands.size()==count,"PLC restart does not trigger command replay");
    }
    {
        Simulator sim;sim.status[0]=0;PlcBridgeClient c;c.connectTo("127.0.0.1",sim.server.serverPort());
        settle(450);check(!c.fresh() && !c.isConnected() && sim.commands.isEmpty(),"wrong server/signature remains read only");
    }
    {
        Fixture f;f.sim.status[2]|=Registered|HostAlive;set32(f.sim.status,15,0xABCDEF01);
        check(waitFor([&]{return f.client.status().owner==0xABCDEF01;}),"other owner observed");
        check(!f.client.issue(Register) && f.sim.commands.isEmpty(),"other owner's session cannot be taken over");
    }

    {
        Simulator sim(2);auto w=sim.status;auto s=decode(w);
        check(s.valid&&s.version==2&&s.maxPrograms==255&&s.selectionMode==1,"V2 capability decoded");
        for(int index:{57,58,60,61,63}) {auto bad=w;bad[index]=0;check(!decode(bad).valid,"incomplete V2 capability rejected");}
        w[58]=256;check(!decode(w).valid,"V2 capacity beyond implemented GDI8 rejected");w[58]=255;
        for(int number=1;number<=255;++number) {
            w[59]=quint16(number);s=decode(w);
            for(int bit=0;bit<8;++bit) check(s.desiredInput(49+bit)==std::optional<bool>(bool(number&(1<<bit))),"all255 GDI numbers decode exact bit order");
            check(s.desiredInput(57)==std::optional<bool>(false),"GDI upper byte stays zero");
        }
        check(!s.desiredInput(65),"V2 rejects DI beyond two output words");
    }
    {
        Fixture f(2);f.reg();
        check(f.sim.commands.first()[0]==2,"V2 registration uses negotiated version");
        for(quint16 number:{quint16(1),quint16(4),quint16(5),quint16(16),quint16(128),quint16(255)}) {
            f.command(Load,number);
            check(f.client.status().selectedSlot==number&&f.client.status().desiredProgramWord==number,"GDI load preserves program number across legacy boundary");
            check(!(f.client.status().desiredWord&15)&&!f.client.status().running(),"GDI load never uses one-hot bits or auto-starts");
            check(f.sim.commands.last()[0]==2&&f.sim.commands.last()[4]==number,"exact V2 number serialized in full command frame");
        }
        check(!f.client.issue(Load,0)&&!f.client.issue(Load,256)&&!f.client.issue(Load,65535),"V2 invalid number bounds rejected locally");
        f.command(ConfirmLoad,255);
        check(f.client.status().has(LoadConfirmed)&&!f.client.status().running(),"V2 manual confirmation preserves separate start");
        f.sim.status[2]&=quint16(~GdiMappingConfirmed);
        check(waitFor([&]{return !f.client.status().has(GdiMappingConfirmed);}),"GDI local permission revocation observed");
        check(!f.client.issue(Enable)&&!f.client.issue(Load,16)&&!f.client.issue(ConfirmLoad,255)&&!f.client.issue(Rearm),"PC cannot bypass local GDI configuration");
        f.command(Stop);
        check(f.client.status().stopped(),"STOP remains available without GDI confirmation");
    }
    {
        Fixture f(2);f.reg();const auto count=f.sim.commands.size();f.sim.status[1]=1;
        check(waitFor([&]{return !f.client.isConnected();}),"changing V2 to V1 invalidates live session");
        check(f.sim.commands.size()==count,"capability changes never replay or automatically downgrade commands");
    }
    std::cout<<"plcbridge_test: "<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
#endif
}
