#include "../workstation_filebridge.h"
#include "../workstation_job.h"
#include "../workstation_delivery.h"
#if defined(STEP_DELIVERY_GUI_TEST) || defined(STEP_AUTO_GUI_TEST)
#include "../workstation_deliverydialog.h"
#include "../workstation_fileuploaddialog.h"
#include <QtWidgets>
#endif
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkInterface>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>
#include <functional>
#include <algorithm>
#include <iostream>
#include <memory>

namespace {
int checks = 0, failures = 0;
void check(bool ok,const char* message) { ++checks; if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
quint16 get(const QByteArray& b,int i) { return qFromBigEndian<quint16>(b.constData() + i); }
void put(QByteArray& b,quint16 w) { b.append(char(w >> 8)); b.append(char(w)); }
quint32 u32(const QVector<quint16>& w,int i) { return quint32(w[i]) << 16 | w[i+1]; }
void set32(QVector<quint16>& w,int i,quint32 n) { w[i] = quint16(n >> 16); w[i+1] = quint16(n); }
bool waitFor(const std::function<bool()>& predicate,int ms = 6000) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < ms) {
        QEventLoop loop; QTimer::singleShot(5,&loop,&QEventLoop::quit); loop.exec();
    }
    return predicate();
}

// A contract peer bound to this PC (loopback by default). It models register ACKs and the
// FTP completion reports; it does NOT emulate an actual FTP server or robot.
class Peer : public QObject {
public:
    QTcpServer server;
    QVector<quint16> s = QVector<quint16>(64);
    QMap<QString,QByteArray> files;
    QVector<int> ops, addresses;
    QMap<QString,QByteArray>* cabinetFiles = nullptr;
    int invalid = 0, writes = 0, heartbeats = 0, reads = 0;
    int ackDelayReads = 0, pendingAckReads = 0;
    quint32 pendingAck = 0;
    quint16 pendingAckOp = 0;
    std::function<void(int)> onRead;
    bool holdFtp = false, rejectStart = false, ignoreAck = false, reboot = false,
         changeOwner = false, badReadback = false, badCompletion = false, dirtyClose = false,
         torn = false, brokenHeader = false, dropReply = false, fragment = false, cleanupUnknown = false;
    void autoConfig(const QString& project="pcjob_manual_r63.sr") {
        s[47]=5;s[48]=0x0103;s[49]=1;s[52]=10;s[53]=11;s[54]=15;s[59]=1;
        set32(s,55,FileBridge::crc32(project.toLatin1()));
    }
    explicit Peer(const QHostAddress& bindAddress = QHostAddress::LocalHost) {
        s[0] = 0xF7B2; s[1] = 1; s[7] = 3; set32(s,10,7);
        s[37] = 32; set32(s,39,1048576); s[41] = 192; s[47] = 1; s[48] = 0x0101;
        connect(&server,&QTcpServer::newConnection,this,[this] {
            auto* socket = server.nextPendingConnection();
            auto buffer = std::make_shared<QByteArray>();
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,this,[this,socket,buffer] {
                *buffer += socket->readAll();
                while (buffer->size() >= 7) {
                    const int length = get(*buffer,4), total = 6 + length;
                    if (buffer->size() < total) return;
                    const auto request = buffer->left(total); buffer->remove(0,total);
                    const auto pdu = request.mid(7); QByteArray reply;
                    if (pdu.size() == 5 && quint8(pdu[0]) == 3 && get(pdu,1) == 6200 && get(pdu,3) == 64) {
                        if(pendingAckReads && --pendingAckReads==0) {set32(s,4,pendingAck);s[6]=pendingAckOp;}
                        ++reads; if (onRead) onRead(reads); reply.append(char(3)); reply.append(char(128));
                        auto snapshot = s;
                        if (torn) snapshot[63] = 2;
                        for (auto n : snapshot) put(reply,n);
                    } else if (pdu.size() >= 6 && quint8(pdu[0]) == 16) {
                        const int address = get(pdu,1), count = get(pdu,3);
                        ++writes; addresses.append(address);
                        if (pdu.size() != 6 + count * 2 || quint8(pdu[5]) != count * 2) ++invalid;
                        QVector<quint16> w; for (int i = 0; i < count; ++i) w.append(get(pdu,6 + i * 2));
                        if (address == 6000 && count == 120) command(w);
                        else if (address == 6120 && count == 5) ++heartbeats;
                        else ++invalid;
                        reply = pdu.left(5);
                        if (dropReply && address == 6000) { socket->disconnectFromHost(); return; }
                    } else { ++invalid; reply.append(char(0x83)); reply.append(char(2)); }
                    QByteArray packet = request.left(2); put(packet,brokenHeader ? 1 : 0); put(packet,quint16(reply.size()+1));
                    packet.append(request[6]); packet += reply;
                    if (fragment) {
                        socket->write(packet.left(4));
                        QTimer::singleShot(2,socket,[socket,packet] { socket->write(packet.mid(4)); });
                    } else socket->write(packet);
                }
            });
        });
        check(server.listen(bindAddress),"peer listens on a local machine address");
    }
private:
    QString name;
    QByteArray data;
    void command(const QVector<quint16>& w) {
        const int op = w[2]; ops.append(op);
        QByteArray content; for (int i = 0; i < 118; ++i) put(content,w[i]);
        const bool autoStart=op==1 && (w[18]==2 || w[18]==3);
        if (w[0] != 0xF7B1 || w[1] != 1 || (!autoStart && (w[19] || w[116] || w[117])) ||
            FileBridge::crc32(content) != u32(w,118) || w[13] > 192) ++invalid;
        if (ignoreAck) return;
        if (rejectStart && op == 1) {
            s[31] = 0xE007; set32(s,32,u32(w,3)); set32(s,34,u32(w,5)); return;
        }
        if (reboot && op == 1) { set32(s,10,u32(w,7)+1); return; }
        if (u32(w,7) != u32(s,10)) ++invalid;
        if (op != 1 && (u32(w,5) != u32(s,8) || u32(w,3) != u32(s,4)+1)) ++invalid;
        pendingAck=u32(w,3);pendingAckOp=quint16(op);pendingAckReads=ackDelayReads;
        if(!pendingAckReads){set32(s,4,pendingAck);s[6]=pendingAckOp;}
        set32(s,8,u32(w,5));
        if (op == 1) {
            if (w[17] == 1) s[20] = 0;
            s[2] = 100; s[3] = 0; s[7] = 3; set32(s,12,0); set32(s,14,u32(w,9)); set32(s,16,0); set32(s,18,u32(w,14));
            s[21] = w[16]; s[22] = w[17]; s[44] = w[18];
            if(autoStart){s[50]=1;s[51]=0;s[57]=w[19];s[58]=w[116];s[60]=w[117];}
            set32(s,27,0); set32(s,29,0); s[45] = s[46] = 0;
            QByteArray names; for (int i = 20; i < 116; ++i) put(names,w[i]); names.truncate(w[13]);
            const auto parts = names.split('\0'); if (parts.size() != 3 || !parts.last().isEmpty()) ++invalid;
            name = QString::fromLatin1(parts.value(1)); data.clear();
            if (u32(w,9) == 0 && u32(w,14) != 0) ++invalid;
        } else if (op == 2) {
            if (u32(w,11) != data.size() || !w[13]) ++invalid;
            QByteArray chunk; for (int i = 20; i < 116; ++i) put(chunk,w[i]); chunk.truncate(w[13]); data += chunk;
            set32(s,12,quint32(data.size())); set32(s,16,FileBridge::crc32(data));
        } else if (op == 3) {
            if(s[44]==3) {
                if(!data.isEmpty() || !cabinetFiles)++invalid;
                data=cabinetFiles?cabinetFiles->value(name):QByteArray();
            } else if (data.size() != u32(s,14) || FileBridge::crc32(data) != u32(s,18)) ++invalid;
            s[2] = 300; s[7] = 17; s[23] = 20;
            if (changeOwner) { set32(s,8,u32(s,8)^0x1234u); return; }
            if (holdFtp) return;
            QTimer::singleShot(25,this,[this] {
                if(s[44]==3 && (data.size()!=u32(s,14) || FileBridge::crc32(data)!=u32(s,18))) {
                    s[2]=900;s[3]=0xE014;s[7]=3;s[23]=0;s[25]=0xF205;s[26]=70;s[50]=0;return;
                }
                files.insert(name,data); s[2] = 200; s[7] = dirtyClose ? 5 : 7;
                ++s[20]; s[23] = 0; s[24] = 221; s[45] = s[46] = badCompletion ? 150 : 226;
                set32(s,16,FileBridge::crc32(data));set32(s,27,s[44]==3?0:quint32(data.size())); set32(s,29,quint32(data.size() + (badReadback ? 1 : 0)));
            });
        } else if (op == 4) { s[2] = cleanupUnknown ? 910 : 900; s[3] = 0xE010; s[7] = cleanupUnknown ? 1 : 3; }
        else if (op == 5) { if (s[20] != s[21]) ++invalid; s[2] = 400; s[7] = 15;if(s[44]>=2)s[51]=1;s[50]=0; }
        else if (op == 6) { s[2] = 0; s[7] = 3; s[50] = 0; }
        else ++invalid;
    }
};

// Real local TCP FTP endpoint. Used with the Modbus peer to exercise the PC
// direct route, binary/empty STOR, split/coalesced replies and PLC CRC rejection.
class DirectCabinet : public QObject {
public:
    QTcpServer server, dataServer;
    QMap<QString,QByteArray> files;
    QStringList commands;
    QTcpSocket* control=nullptr;
    QTcpSocket* dataSocket=nullptr;
    QByteArray pending, payload;
    QString target;
    bool corrupt=false, reject=false, rejectLogin=false, badCompletion=false, badQuit=false, fragmented=false, stall=false;
    std::function<void()> onStor;
    int connections=0;
    DirectCabinet() {
        check(dataServer.listen(QHostAddress::LocalHost),"FTP data listens only on loopback");
        connect(&dataServer,&QTcpServer::newConnection,this,[this] {
            dataSocket=dataServer.nextPendingConnection();auto* socket=dataSocket;payload.clear();
            connect(socket,&QTcpSocket::readyRead,this,[this,socket]{payload+=socket->readAll();});
            connect(socket,&QTcpSocket::disconnected,this,[this,socket] {
                payload+=socket->readAll();auto bytes=payload;if(corrupt && !bytes.isEmpty())bytes[0]=char(bytes[0]^1);
                files[target]=bytes;reply(badCompletion?"426 Failed":"226 Complete");socket->deleteLater();dataSocket=nullptr;
            });
        });
        connect(&server,&QTcpServer::newConnection,this,[this] {
            ++connections;control=server.nextPendingConnection();auto* socket=control;pending.clear();
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            reply("220-Test\r\nwelcome\r\n220 Ready");
            connect(socket,&QTcpSocket::readyRead,this,[this,socket] {
                pending+=socket->readAll();
                while(pending.contains("\r\n")) {
                    const auto i=pending.indexOf("\r\n");const auto cmd=pending.left(i);pending.remove(0,i+2);
                    commands<<QString::fromLatin1(cmd.startsWith("PASS ")?QByteArray("PASS ***"):cmd);
                    if(cmd.startsWith("USER "))reply("331 Password");
                    else if(cmd.startsWith("PASS "))reply(rejectLogin?"530 Login incorrect":"230 Logged in");
                    else if(cmd.startsWith("CWD "))reply("250 Folder");
                    else if(cmd=="TYPE I")reply("200 Binary");
                    else if(cmd=="PASV")reply("227 Passive (203,0,113,99,"+QByteArray::number(dataServer.serverPort()/256)+","+QByteArray::number(dataServer.serverPort()%256)+")");
                    else if(cmd.startsWith("STOR ")){target=QString::fromLatin1(cmd.mid(5));if(onStor)onStor();if(!stall)reply(reject?"550 Refused":"150 Upload");}
                    else if(cmd=="QUIT")reply(badQuit?"500 Failed":"221 Bye");
                    else {check(false,"PC direct uses only file transfer FTP commands");reply("500 Bad");}
                }
            });
        });
        check(server.listen(QHostAddress::LocalHost),"FTP control listens only on loopback");
    }
    void reply(const QByteArray& line) {
        if(!control || control->state()!=QAbstractSocket::ConnectedState)return;
        const auto packet=line+"\r\n";
        if(!fragmented){control->write(packet);return;}
        auto* socket=control;socket->write(packet.left(2));
        QTimer::singleShot(1,socket,[socket,packet]{socket->write(packet.mid(2));});
    }
};

QMap<QString,QByteArray> sample() {
    QByteArray binary; for (int i = 0; i < 601; ++i) binary.append(char(i % 256));
    return {{"_project.srd",QByteArray("native\r\n")},{"empty.srd",{}},{"empty.srp",binary}};
}
FileBridge::Options options(Peer& peer,const QString& receipt) {
    FileBridge::Options o; o.host = peer.server.serverAddress().toString(); o.port = peer.server.serverPort(); o.files = sample();
    o.project = FileBridge::newProject(); o.receipt = receipt; o.ioTimeoutMs = 500;
    o.ackTimeoutMs = 800; o.ftpTimeoutMs = 1500; o.cleanupTimeoutMs = 1200; return o;
}
QJsonObject run(FileBridge::Options o,FileBridge::Transfer** active = nullptr,
    const std::function<void(FileBridge::Transfer&,const QJsonObject&)>& callback = {}) {
    FileBridge::Transfer worker(std::move(o)); QJsonObject result;
    if (active) *active = &worker;
    QObject::connect(&worker,&FileBridge::Transfer::outcome,QCoreApplication::instance(),[&](const QJsonObject& r) { result = r; });
    QObject::connect(&worker,&FileBridge::Transfer::progress,QCoreApplication::instance(),[&](const QJsonObject& e) { if (callback) callback(worker,e); });
    worker.start();
    if (!waitFor([&] { return worker.isFinished() && !result.isEmpty(); },10000)) {
        check(false,"transfer terminates within deadline"); worker.requestInterruption();
        waitFor([&] { return worker.isFinished(); },5000);
    }
    worker.wait(); if (active) *active = nullptr; return result;
}
void vectors() {
    check(FileBridge::crc32("123456789") == 0xCBF43926u && FileBridge::crc32({}) == 0,"standard CRC32 known answers");
    QFile file(QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()).filePath("filebridge_vectors.json"));
    check(file.open(QIODevice::ReadOnly),"Python-generated reference vectors readable");
    const auto cases = QJsonDocument::fromJson(file.readAll()).object().value("cases").toArray();
    check(cases.size() == 3,"three Python reference vectors");
    for (const auto& item : cases) {
        const auto c = item.toObject(); auto n = [&](const char* key) { return quint32(c.value(key).toDouble()); };
        const auto got = FileBridge::frame(quint16(n("op")),n("seq"),n("session"),n("boot"),n("size"),n("offset"),
            QByteArray::fromHex(c.value("payload").toString().toLatin1()),n("crc"),quint16(n("count")),quint16(n("ordinal")));
        const auto expected = c.value("words").toArray(); bool same = expected.size() == got.size();
        for (int i = 0; same && i < got.size(); ++i) same = got[i] == expected[i].toInt();
        check(same,"all 120 native frame words match hardware-verified Python encoder");
    }
}
}

int main(int argc,char** argv) {
#if defined(STEP_DELIVERY_GUI_TEST) || defined(STEP_AUTO_GUI_TEST)
    QApplication app(argc, argv); QTemporaryDir temp;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
    qputenv("STEP_FTP_PASSWORD", "fixture_site_password");
    QHostAddress address;
    for (const auto& candidate : QNetworkInterface::allAddresses())
        if (FileBridge::privateEndpoint(candidate.toString())) { address = candidate; break; }
    check(!address.isNull(), "GUI flow peer uses an address assigned to this PC");
    if (address.isNull()) return 1;
    Peer peer(address);
#ifdef STEP_AUTO_GUI_TEST
    peer.autoConfig();
#endif
    const auto base = readNativeJob(app.arguments().value(1));
    const auto source = temp.filePath("job.nc");
    { QFile file(source); file.open(QIODevice::WriteOnly); file.write("G21 G17 G90 G94 G91.1\nG1 X1 F300\nM30\n"); }
    GcodeDeliveryDialog dialog(base, address.toString() + ":" + QString::number(peer.server.serverPort()),
        temp.filePath("jobs"), temp.filePath("receipts"),nullptr,
#ifdef STEP_AUTO_GUI_TEST
        true
#else
        false
#endif
    );
#ifdef STEP_AUTO_GUI_TEST
    QList<Job> previewEvents;
    QObject::connect(&dialog, &GcodeDeliveryDialog::previewChanged, &dialog, [&] { previewEvents.append(dialog.preview()); });
#endif
    dialog.importSource(source);
    check(peer.reads == 0 && peer.writes == 0 && dialog.result().isEmpty(), "GUI import performs no network operations");
    auto* send = dialog.findChild<QPushButton*>("deliveryUpload");
    check(send && send->isEnabled(), "valid import exposes explicit upload action");
#ifdef STEP_AUTO_GUI_TEST
    const auto importedPreview = dialog.preview();
    check(importedPreview.automaticPatch && importedPreview.files.size() == 4 && !importedPreview.runnable() &&
        importedPreview.previewNote.contains("尚未上传") && importedPreview.sourceIsGcode &&
        importedPreview.sourceProgram == "auto_01" && importedPreview.source.join('\n').contains("G1 X1") &&
        previewEvents.size() == 1,
        "GUI exposes the actual generated patch for local preview without claiming upload or SDK run permission");
    auto* transport=dialog.findChild<QComboBox*>("deliveryTransport");
    check(transport && transport->count()==2 && transport->currentIndex()==0,"GUI exposes PLC and SDK-manual direct transfer routes with PLC default");
    if(transport) {
        dialog.setDirectTransfer(true,"192.168.39.220:30312");
        const auto* route=dialog.findChild<QLabel*>("deliveryRoute");
        const auto* connection=dialog.findChild<QLabel*>("deliveryConnectionHint");
        check(transport->currentIndex()==1 && send->text().contains("电脑直传") && route &&
            route->text().contains("上位机直传控制柜") && !route->text().contains("PLC转传") && connection &&
            connection->text().contains("无需先点顶部") && dialog.preview().files==importedPreview.files && peer.writes==0,
            "SDK context selects direct route without changing the generated snapshot or connecting a device");
        dialog.setDirectTransfer(false);
    }
    {
        const auto generated=Delivery::compileAuto({source},base);
        FileUploadDialog direct(generated.files,"local offline UI",address.toString()+":"+QString::number(peer.server.serverPort()),temp.filePath("receipts"));
        direct.useAutoTarget(generated.project,11,generated.tag);direct.setDirectMode(true,"192.168.39.220:30312");direct.show();app.processEvents();
        auto* secret=direct.findChild<QLineEdit*>("fileUploadFtpPassword");
        check(secret && secret->echoMode()==QLineEdit::Password && secret->isVisible() && peer.writes==0,"direct configuration shows password field and performs no network action");
        auto* eye=direct.findChild<QAction*>("fileUploadTogglePassword");
        check(secret && secret->text()=="fixture_site_password" && eye && !eye->icon().isNull() && !eye->isChecked(),
            "fixed field password is prefilled and hidden behind an eye button");
        if(eye)eye->trigger();
        check(secret && secret->echoMode()==QLineEdit::Normal && eye && eye->text()==QString("隐藏密码") &&
            secret->text()=="fixture_site_password" && peer.writes==0,"eye reveals the exact password without changing or sending it");
        if(eye)eye->trigger();
        check(secret && secret->echoMode()==QLineEdit::Password && eye && eye->text()==QString("显示密码"),
            "eye hides the password again");
        const auto* route=direct.findChild<QLabel*>("fileUploadRoute");
        const auto* cabinet=direct.findChild<QLineEdit*>("fileUploadCabinet");
        const auto* plcLabel=direct.findChild<QLabel*>("fileUploadEndpointLabel");
        check(route && route->text()==QString("文件传输：上位机 → FTP → 控制柜") && cabinet &&
            cabinet->text()=="192.168.39.220:21" && plcLabel && plcLabel->text().contains("许可 / 回读") &&
            direct.windowTitle().contains("上位机 → 控制柜"),
            "direct upload UI separates the cabinet FTP endpoint from the PLC permission/readback endpoint");
        if(app.arguments().size()>2)check(direct.grab().save(QFileInfo(app.arguments()[2]).dir().filePath("auto_direct_config.png")),"direct configuration UI rendered offline");
        direct.close();
    }
    {
        GcodeDeliveryDialog reused(base,"127.0.0.1:1",temp.filePath("unused"),temp.filePath("receipts"),nullptr,true);
        { QFile file(source); file.open(QIODevice::WriteOnly); file.write("G90\nG1 X999 F300\nM30\n"); }
        reused.importCurrentSource(importedPreview);
        const auto* currentPath=reused.findChild<QLineEdit*>("deliverySource");
        check(currentPath && currentPath->text()==source && reused.preview().mappedBytes==importedPreview.mappedBytes &&
            reused.preview().source==importedPreview.source && reused.preview().points.last().x==1 && peer.writes==0,
            "reopening upload uses the displayed automatic source snapshot even when its disk file changed");
        auto* rapid=reused.findChild<QDoubleSpinBox*>();
        if(rapid)rapid->setValue(4);
        check(reused.preview().points.last().x==1 && !reused.preview().source.join('\n').contains("X999"),
            "changing conversion settings keeps the imported source snapshot instead of rereading disk");
        const auto replacement=temp.filePath("replacement.nc");
        { QFile file(replacement); file.open(QIODevice::WriteOnly); file.write("G90\nG1 Y8 F300\nM30\n"); }
        reused.importSource(replacement);
        check(reused.preview().points.last().y==8 && reused.preview().source.join('\n').contains("G1 Y8") &&
            !reused.preview().source.join('\n').contains("G1 X1"),
            "choosing another G-code replaces the source text and upload trajectory together");
        QFile::remove(replacement);
        GcodeDeliveryDialog reopened(base,"127.0.0.1:1",temp.filePath("unused"),temp.filePath("receipts"),nullptr,true);
        reopened.importCurrentSource(reused.preview());
        check(reopened.preview().error.isEmpty() && reopened.preview().points.last().y==8 &&
            reopened.preview().source==reused.preview().source,
            "displayed source can be uploaded again after the original disk file was removed");
    }
    // Editing the source on disk after import must not change what is previewed or uploaded.
    { QFile file(source); file.open(QIODevice::WriteOnly); file.write("G90\nG1 X999 F300\nM30\n"); }
#endif
    QTimer poll;
    poll.setInterval(20);
    bool completed = false;
    QObject::connect(&poll, &QTimer::timeout, &dialog, [&] {
        auto* upload = dialog.findChild<FileUploadDialog*>();
        if (upload) {
            auto* close = upload->findChild<QPushButton*>("fileUploadClose");
            if (!upload->result().isEmpty() && close && close->text() == QString::fromUtf8("关闭")) close->click();
        }
        if (dialog.result().isEmpty()) return;
        completed = true; poll.stop();
        auto* copy = dialog.findChild<QPushButton*>("deliveryCopyHandoff");
        auto* report = dialog.findChild<QPlainTextEdit*>("deliveryReport");
        const auto result = dialog.result();
        check(result.value("status") == "COMPLETE" && copy && copy->isEnabled() && send && !send->isEnabled(),
            "one explicit click saves and uploads; completion exposes handoff without automatic retry");
#ifdef STEP_AUTO_GUI_TEST
        check(report && report->toPlainText().contains("auto_main") && report->toPlainText().contains("11"),"GUI shows reusable program number after verified upload");
        const auto preview = dialog.preview();
        check(preview.files == peer.files && preview.files == importedPreview.files &&
            preview.previewNote.contains("上传及回读通过") && preview.previewNote.contains("待重新加载") && !preview.runnable(),
            "verified GUI handoff preserves the uploaded snapshot and still requires cabinet reload");
        check(previewEvents.size() == 3 && previewEvents[1].summary.contains("正在上传") &&
            previewEvents.last().summary.contains("上传及回读通过") && preview.source == importedPreview.source &&
            preview.mappedBytes == importedPreview.mappedBytes && !preview.source.join('\n').contains("X999"),
            "parent receives import, uploading and verified snapshots without reading the source a second time");
        check(peer.files.size()==4 && peer.files.contains("auto_main.srp") && !peer.files.contains("_project.srd") && peer.invalid==0,"GUI updates only variable auto job files");
        check(QFileInfo(dialog.packagePath()).dir().exists("delivery.json"),"auto GUI persists source and delivery manifest");
#else
        check(report && report->toPlainText().contains(result.value("remote_directory").toString()) &&
            report->toPlainText().contains(QString::fromUtf8("编号目前未分配")), "GUI completion shows new path and pending cabinet GDI binding");
        const auto generated = Delivery::compile(source, base);
        check(peer.files == generated.files && peer.invalid == 0 &&
            std::all_of(peer.addresses.begin(), peer.addresses.end(), [](int a) { return a == 6000 || a == 6120; }),
            "GUI flow sends exact generated files using only file-channel registers");
        const auto folder = QFileInfo(dialog.packagePath()).dir();
        check(folder.exists("delivery.json") && folder.exists("handoff.txt"), "GUI completion persists human-readable binding handoff");
#endif
        if (app.arguments().size() > 2) check(dialog.grab().save(app.arguments()[2]), "completion UI rendered");
        dialog.accept();
    });
    poll.start();
    QTimer::singleShot(0, &dialog, [send] { if (send) send->click(); });
    dialog.exec();
    check(completed, "GUI flow completed using local PC peer only");
    std::cout << checks << " GUI flow checks; " << failures << " failures (local PC simulator only)\n";
    return failures ? 1 : 0;
#else
    QCoreApplication app(argc,argv); QTemporaryDir temp;
    int fileNumber = 0; auto path = [&] { return temp.filePath(QString("receipt_%1.json").arg(++fileNumber)); };
    {
        Peer peer;peer.ackDelayReads=3;
        auto o=options(peer,path());o.files["empty.srp"]=QByteArray(40001,'x');
        const auto result=run(o);
        check(result.value("status")=="COMPLETE" && peer.files==o.files && peer.invalid==0,"fast polling waits for delayed application ACK before advancing 40KB chunks");
        check(peer.ops.count(2)==(40001+191)/192+1,"fast polling never duplicates a chunk or pipelines the single mailbox");
    }
    for(int scenario=0;scenario<10;++scenario) {
        Peer peer;peer.autoConfig();peer.s[47]=13;peer.s[48]=0x0104;
        DirectCabinet cabinet;peer.cabinetFiles=&cabinet.files;
        auto o=options(peer,path());o.replaceAuto=true;o.directFtp=true;o.project="pcjob_manual_r63.sr";o.tag={123,456,789};
        o.cabinetHost="127.0.0.1";o.cabinetPort=cabinet.server.serverPort();o.ftpUser="test";o.ftpPassword="local_test_password";
        QByteArray binary;for(int i=0;i<40001;++i)binary.append(char(i%256));
        o.files={{"auto_main.srp",binary},{"auto_main.srd",{}},{"auto_01.srp","code"},{"auto_01.srd",{}}};
        cabinet.fragmented=scenario==1;cabinet.corrupt=scenario==2;cabinet.reject=scenario==3;
        cabinet.badCompletion=scenario==4;cabinet.badQuit=scenario==5;
        if(scenario==6){peer.s[47]=5;peer.s[48]=0x0103;}
        if(scenario>=7)cabinet.stall=true;
        if(scenario==8)cabinet.onStor=[&]{peer.s[59]=0;};
        if(scenario==9)cabinet.onStor=[&]{set32(peer.s,10,u32(peer.s,10)+1);};
        const auto r=run(o,nullptr,[&](FileBridge::Transfer& worker,const QJsonObject& event){
            if(scenario==7 && event.value("phase")=="direct_upload")worker.requestInterruption();
        });
        if(scenario<2) {
            if(r.value("status")!="COMPLETE")std::cerr<<QJsonDocument(r).toJson().constData()<<'\n';
            check(r.value("status")=="COMPLETE" && peer.files==o.files && cabinet.files==o.files,"direct FTP binary and empty files pass full PLC verification");
            check(!peer.ops.contains(2) && peer.s[51]==1,"direct route skips Modbus payload and only releases complete verified job");
            check(r.value("transport")=="pc_direct_ftp_plc_verify" && r.value("total_ms").toDouble()>0 &&
                  r.value("files").toArray().first().toObject().contains("direct_ftp_upload_ms"),"direct receipt contains route and stage timing");
        } else if(scenario==7)check(r.value("status")=="CANCELLED" && peer.s[51]==0 && peer.ops.contains(4),"cancelled direct FTP closes and asks owning PLC to clean session");
        else check(r.value("status")=="INCOMPLETE" && peer.s[51]==0,"direct error cannot release job number");
        if(scenario==6)check(cabinet.connections==0 && peer.writes==0,"old PLC rejects direct before opening FTP or writing commands");
        if(scenario>=2 && scenario<=5) {
            check(peer.s[2]==0 && peer.ops.contains(6),"known direct FTP failures release the file channel without a local reset");
        }
        check(peer.invalid==0 && !r.contains("ftpPassword") && !QJsonDocument(r).toJson().contains("local_test_password"),"direct uses valid file registers and excludes credentials from receipt");
        check(!r.value("robot_loaded").toBool() && !r.value("robot_started").toBool(),"direct transfer never loads or starts robot");
    }
    const auto autoBase=readNativeJob(app.arguments().value(1));
    const auto autoSource=temp.filePath("auto.nc");
    {QFile file(autoSource);file.open(QIODevice::WriteOnly);file.write("G90\nG1 X1 F300\nM30\n");}
    for (bool direct : {false,true}) {
        Peer peer;peer.autoConfig();peer.s[47]=13;peer.s[48]=0x0104;
        DirectCabinet cabinet;peer.cabinetFiles=&cabinet.files;
        for (int attempt=0;attempt<3;++attempt) {
            const auto generated=Delivery::compileAuto({autoSource},autoBase);
            auto o=options(peer,path());o.replaceAuto=true;o.directFtp=direct;o.project=generated.project;o.files=generated.files;o.tag=generated.tag;
            o.cabinetHost="127.0.0.1";o.cabinetPort=cabinet.server.serverPort();o.ftpUser="test";o.ftpPassword="local_test_password";
            const auto result=run(o);
            check(result.value("status")=="COMPLETE" && peer.files==generated.files && peer.s[51]==1 && peer.s[50]==0 && peer.invalid==0,
                "three consecutive different revisions upload through either route with no PLC reset");
            check(!peer.ops.contains(4) && !peer.ops.contains(6),"successful consecutive uploads reuse completed state without aborting or resetting");
        }
    }
    {
        Peer peer;peer.autoConfig();peer.s[47]=13;peer.s[48]=0x0104;
        DirectCabinet cabinet;peer.cabinetFiles=&cabinet.files;cabinet.rejectLogin=true;
        const auto generated=Delivery::compileAuto({autoSource},autoBase);
        auto o=options(peer,path());o.replaceAuto=true;o.directFtp=true;o.project=generated.project;o.files=generated.files;o.tag=generated.tag;
        o.cabinetHost="127.0.0.1";o.cabinetPort=cabinet.server.serverPort();o.ftpUser="test";o.ftpPassword="local_test_password";
        const auto failure=run(o);
        check(failure.value("status")=="INCOMPLETE" && failure.value("error").toString().contains("530") &&
            failure.value("failure_status").toObject().value("state")==100 && peer.s[2]==0 && peer.s[51]==0 &&
            peer.ops.contains(4) && peer.ops.contains(6) && cabinet.files.isEmpty(),
            "login 530 reproduces field failure and now aborts and releases its own reservation");
        cabinet.rejectLogin=false;o.receipt=path();
        check(run(o).value("status")=="COMPLETE" && peer.files==generated.files && peer.invalid==0,
            "correcting a failed login permits the next upload immediately without xReset");
    }
    for (bool direct : {false,true}) {
        Peer peer;peer.autoConfig();peer.s[47]=13;peer.s[48]=0x0104;
        peer.s[2]=900;peer.s[3]=0xE011;peer.s[7]=3;set32(peer.s,8,0x12345678);set32(peer.s,4,1);peer.s[6]=1;
        DirectCabinet cabinet;peer.cabinetFiles=&cabinet.files;
        const auto generated=Delivery::compileAuto({autoSource},autoBase);
        auto o=options(peer,path());o.replaceAuto=true;o.directFtp=direct;o.project=generated.project;o.files=generated.files;o.tag=generated.tag;
        o.cabinetHost="127.0.0.1";o.cabinetPort=cabinet.server.serverPort();o.ftpUser="test";o.ftpPassword="local_test_password";
        const auto result=run(o);
        check(result.value("status")=="COMPLETE" && result.value("previous_session_cleanup").toObject().value("status")=="RELEASED" &&
            !peer.ops.isEmpty() && peer.ops.first()==6 && !peer.ops.contains(4) && peer.invalid==0,
            "next upload releases a stable expired closed session before creating its own new session");
    }
    for (int blocked=0;blocked<7;++blocked) {
        Peer peer;peer.autoConfig();peer.s[2]=900;peer.s[3]=0xE011;set32(peer.s,8,1234);set32(peer.s,4,1);peer.s[6]=1;
        if(blocked==0)peer.s[7]=1;
        if(blocked==1)peer.s[2]=910;
        if(blocked==2)peer.s[2]=100;
        if(blocked==3)peer.s[59]=0;
        if(blocked==4)peer.s[49]=0;
        if(blocked==5)peer.s[50]=1;
        if(blocked==6)peer.onRead=[&](int reads){if(reads==2)set32(peer.s,8,5678);};
        const auto generated=Delivery::compileAuto({autoSource},autoBase);
        auto o=options(peer,path());o.replaceAuto=true;o.project=generated.project;o.files=generated.files;o.tag=generated.tag;
        const auto result=run(o);
        check(result.value("status")=="INCOMPLETE" && peer.writes==0,
            "failed-session recovery refuses dirty handles, active transfers, non-idle robot, invalid mapping and changed ownership");
    }
    {
        const auto processSource = temp.filePath("process.nc");
        const auto nextSource = temp.filePath("next.nc");
        { QFile file(processSource); file.open(QIODevice::WriteOnly); file.write("G90\nG1 X7 F300\nM5\nM30\n"); }
        { QFile file(nextSource); file.open(QIODevice::WriteOnly); file.write("G90\nG1 Y9 F300\nM30\n"); }
        check(!readGcodeJob(processSource, autoBase).error.isEmpty(), "plain reparse rejects unconfirmed M5 and reproduces the prior preview mismatch");
        const auto automatic = Delivery::compileAuto({processSource, nextSource}, autoBase, {4, .02, true});
        auto preview = Delivery::autoPreview(automatic, {{"status", "COMPLETE"}});
        check(automatic.error.isEmpty() && preview.error.isEmpty() && preview.conversion.omitProcessOutputs &&
            preview.conversion.rapidMmS == 4 && preview.conversion.arcToleranceMm == .02 &&
            preview.source.join('\n').contains("M5") && preview.points.last().x == 7,
            "verified preview reuses the confirmed conversion settings, original M5 and exact path");
        const auto motion = preview.steps.first();
        const auto nativeLines = QString::fromUtf8(preview.files.value("auto_01.srp")).split('\n');
        check(nativeLines[motion.firstLine - 1].startsWith("cpTarget := RelRef(msHomeCart,") &&
            preview.steps.last().skipped && sourceRowForNativeLine(preview, preview.project, "auto_01", motion.firstLine) == 2,
            "automatic source mapping points to the actual uploaded movement line after the guard");
        QFile::remove(processSource); QFile::remove(nextSource);
        check(showProgramGcode(preview, "auto_02") && preview.source.join('\n').contains("G1 Y9") &&
            preview.points.last().y == 9 && !preview.source.join('\n').contains("X7"),
            "second source and path switch together without reopening either source file");
        check(showProgramGcode(preview, "auto_01") && preview.points.last().x == 7 && preview.source.join('\n').contains("M5") &&
            !restoreMappedPreview(preview, autoBase) && preview.points.last().x == 7 && !preview.runnable(),
            "return to original source retains the converted snapshot without recompilation or SDK permissions");
    }
    for(int count:{1,3,15}) {
        QStringList sources;for(int i=0;i<count;++i)sources<<autoSource;
        const auto job=Delivery::compileAuto(sources,autoBase);
        check(job.error.isEmpty() && job.files.size()==2*(count+1) && FileBridge::validate(job.files,true).isEmpty(),"one/many/max-size job compiles to complete auto-only file set");
        check(!job.files.contains("_project.srd") && !job.files.contains("manual_service.srp") && !job.files.contains("capture_home.srp"),"daily job cannot overwrite origin or manual service");
        check(job.files.value("auto_01.srp").startsWith("ajRead1 := AIRead") && job.files.value("auto_01.srp").contains("msHomeCart") &&
            job.files.value("auto_01.srd").contains("ROBOTCARTPOS cpTarget"),"all trajectories guard revision before using shared manual origin with matching position type");
        for (auto it=job.files.cbegin();it!=job.files.cend();++it) {
            if (!it.key().endsWith(".srp")) continue;
            const auto data=job.files.value(it.key().chopped(4)+".srd");
            check(!it.value().contains("IF(AIRead") && it.value().contains("IF(ajRead1!=") &&
                  data.contains("INT ajRead1 := 0\n") && data.contains("INT ajRead2 := 0\n") && data.contains("INT ajRead3 := 0\n"),
                  "each entry and child uses STEP assignment-form AIRead with declared snapshot variables");
        }
        auto preview = Delivery::autoPreview(job);
        check(preview.automaticPatch && preview.nativeInput && !preview.runnable() && preview.files == job.files &&
            preview.program == "auto_main" && preview.sourceIsGcode && preview.sourceProgram == "auto_01" &&
            preview.source == job.parts.first().source && preview.mappedBytes == job.parts.first().inputBytes &&
            preview.previewNote.contains("尚未上传"),
            "one/many patch previews default to the exact converted G-code snapshot and remain local");
        check(preview.steps.first().firstLine == job.parts.first().steps.first().firstLine + job.patchLineOffset &&
            sourceRowForNativeLine(preview, preview.project, "auto_01", preview.steps.first().firstLine) == preview.steps.first().row &&
            sourceRowForNativeLine(preview, preview.project, "auto_01", 1) == 0,
            "automatic G-code highlighting offsets the generated revision guard and never maps guard lines to motion");
        showNativeSource(preview, "auto_main");
        check(preview.source.join('\n').contains("ProgCall(auto_01);"), "entry SRP remains available in the same job snapshot");
        const auto lastProgram = QString("auto_%1").arg(count, 2, 10, QLatin1Char('0'));
        showNativeSource(preview, lastProgram);
        check(preview.sourceFile == lastProgram + ".srp" && preview.points.size() == job.parts.last().points.size() &&
            preview.source.join('\n').contains("msHomeCart") && preview.programSources.value(lastProgram) == autoSource,
            "selecting each generated program updates the trajectory and original source identity");
        const auto beforeEdit = preview.files;
        check(!updateNativeFile(preview, lastProgram + ".srp", "EOF\n").isEmpty() && preview.files == beforeEdit &&
            !exportStepJob(preview, temp.filePath("not_complete.zip")).isEmpty(),
            "patch preview cannot edit a verified revision or masquerade as a complete origin-bearing project");
        check(Delivery::autoPreview(job, {{"status", "INCOMPLETE"}}).previewNote.contains("上传未完成"),
            "failed upload preview does not report the program as released");
        const auto saved=Delivery::saveAuto(job,temp.filePath("auto_jobs"));
        check(saved.error.isEmpty() && QFileInfo::exists(saved.zip),"auto patch, sources and manifest saved");
        check(!readNativeJob(saved.zip).error.isEmpty(), "complete-project import continues to reject automatic patch archives");
        Peer peer;peer.autoConfig();auto o=options(peer,path());o.files=job.files;o.project=job.project;o.replaceAuto=true;o.tag=job.tag;
        const auto result=run(o);
        check(result.value("status")=="COMPLETE" && peer.files==job.files && peer.invalid==0,"tagged fixed auto upload completes through local register peer");
        check(Delivery::recordAuto(saved,job,result).isEmpty() && Delivery::autoHandoff(job,result).contains("11"),"verified upload yields fixed-number handoff");
    }
    {
        auto job=Delivery::compileAuto({autoSource},autoBase);auto bad=job.files;bad.insert("_project.srd","protected");
        check(!FileBridge::validate(bad,true).isEmpty(),"protected origin file rejected locally");
        bad=job.files;bad.insert("auto_03.srp",bad.take("auto_01.srp"));bad.insert("auto_03.srd",bad.take("auto_01.srd"));
        check(!FileBridge::validate(bad,true).isEmpty(),"gaps in program list rejected");
        Peer old;auto o=options(old,path());o.files=job.files;o.project=job.project;o.replaceAuto=true;o.tag=job.tag;
        check(run(o).value("status")!="COMPLETE" && old.writes==0,"old PLC rejected before command or heartbeat writes");
        Peer wrong;wrong.autoConfig("pcjob_other.sr");o=options(wrong,path());o.files=job.files;o.project=job.project;o.replaceAuto=true;o.tag=job.tag;
        const auto wrongTarget=run(o);
        check(wrongTarget.value("status")!="COMPLETE" && wrongTarget.value("error").toString().contains("工程或编号") && wrong.writes==0,"wrong fixed target reports binding mismatch without writes");
        for(bool idleFailure:{false,true}) {
            Peer blocked;blocked.autoConfig();blocked.s[idleFailure?59:49]=0;
            o=options(blocked,path());o.files=job.files;o.project=job.project;o.replaceAuto=true;o.tag=job.tag;
            const auto r=run(o);const auto message=r.value("error").toString();
            check(r.value("status")=="INCOMPLETE" && r.value("preflight_failed").toBool() && blocked.writes==0 && blocked.heartbeats==0 && blocked.files.isEmpty(),
                "failed fixed-entry preflight never writes commands, heartbeats or files");
            check(idleFailure?message.contains("HR6259=0")&&!message.contains("xFixedMappingConfirmed"):
                message.contains("HR6249=0")&&message.contains("xFixedMappingConfirmed")&&message.contains("uiFixedJobGuardVersion"),
                "configuration reset and non-idle robot report distinct corrective steps");
        }
        QStringList many;for(int i=0;i<16;++i)many<<autoSource;
        check(!Delivery::compileAuto(many,autoBase).error.isEmpty(),"capacity overflow rejected");
        check(job.tag!=Delivery::compileAuto({autoSource},autoBase).tag,"each conversion changes the SRP revision literals");
    }
    {
        // Qt bypasses application proxies for loopback automatically. Reproduce
        // this field failure on an address assigned to THIS PC, never the PLC.
        QHostAddress localAddress = QHostAddress::LocalHost;
        for (const auto& candidate : QNetworkInterface::allAddresses())
            if (FileBridge::privateEndpoint(candidate.toString())) { localAddress = candidate; break; }
        check(!localAddress.isLoopback(),"proxy regression has a private IPv4 assigned to this PC");
        Peer p(localAddress); const auto originalProxy = QNetworkProxy::applicationProxy();
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpCachingProxy,"127.0.0.1",9));
        const auto r = run(options(p,path()));
        const bool proxyUnchanged = QNetworkProxy::applicationProxy().type() == QNetworkProxy::HttpCachingProxy;
        QNetworkProxy::setApplicationProxy(originalProxy);
        check(proxyUnchanged,"direct PLC transfer preserves application-wide proxy settings");
        check(r.value("status") == "COMPLETE" && p.files == sample() && p.invalid == 0,
            "local PLC file transfer bypasses unsupported application HTTP caching proxy");
        if (r.value("status") != "COMPLETE") std::cerr << "Proxy regression result: " << QJsonDocument(r).toJson(QJsonDocument::Compact).constData() << '\n';
    }
    if (app.arguments().contains("--proxy-regression")) {
        std::cout << checks << " proxy regression checks; " << failures << " failures (local PC simulator only)\n";
        return failures ? 1 : 0;
    }
    vectors();
    check(FileBridge::validate(sample()).isEmpty(),"native binary and zero-length members accepted");
    auto bad = sample(); bad.remove("empty.srd"); check(!FileBridge::validate(bad).isEmpty(),"missing pair refused");
    bad = sample(); bad.insert("../escape.srp",{}); check(!FileBridge::validate(bad).isEmpty(),"path traversal refused");
    bad = sample(); bad.insert("extra.txt","text"); check(!FileBridge::validate(bad).isEmpty(),"non-native file refused");
    bad = sample(); bad["empty.srp"] = QByteArray(FileBridge::MaxFile + 1,'x'); check(!FileBridge::validate(bad).isEmpty(),"oversized member refused");
    bad = sample();
    for (int i = 0; i < 31; ++i) bad.insert(QString("extra_%1.srd").arg(i),{});
    check(!FileBridge::validate(bad).isEmpty(),"too many files refused");
    bad = sample();
    for (int i = 0; i < 6; ++i) bad.insert(QString("extra_%1.srd").arg(i),QByteArray(FileBridge::MaxFile,'x'));
    check(!FileBridge::validate(bad).isEmpty(),"total uncompressed size limit enforced");
    check(FileBridge::privateEndpoint("192.168.1.188") && FileBridge::privateEndpoint("172.16.1.1") &&
        !FileBridge::privateEndpoint("172.32.1.1") && !FileBridge::privateEndpoint("8.8.8.8"),"GUI restricts endpoints to private IPv4");
    {
        Peer p; p.fragment = true; const auto receipt = path(); auto o = options(p,receipt);
        const auto frozen = o.files; auto result = run(o);
        check(result.value("status") == "COMPLETE","multi-file native transfer completed");
        check(p.files == frozen && p.files.contains("empty.srd") && p.files.value("empty.srd").isEmpty(),"all original binary bytes and empty member preserved");
        check(p.invalid == 0 && p.ops.count(1) == 3 && p.ops.count(3) == 3 && p.ops.count(5) == 1 && p.ops.count(2) == 5,"exact START CHUNK DEPLOY FINISH sequence");
        check(p.heartbeats > 0 && std::all_of(p.addresses.begin(),p.addresses.end(),[](int a) { return a == 6000 || a == 6120; }),"only dedicated file-channel registers written");
        check(!result.value("robot_loaded").toBool() && !result.value("robot_started").toBool(),"receipt never claims load or motion");
        QFile r(receipt); check(r.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(r.readAll()).object().value("status") == "COMPLETE","complete atomic local receipt");
        const auto again = run(options(p,receipt));
        check(again.value("status") == "INCOMPLETE" && p.ops.count(5) == 1,"existing receipt never overwritten or retried");
    }
    {
        Peer p; p.s[47] = 0; const auto r = run(options(p,path()));
        check(r.value("status") == "INCOMPLETE" && p.writes == 0,"old PLC refuses empty files before command or heartbeat writes");
    }
    {
        Peer p; p.s[7] = 2; const auto r = run(options(p,path()));
        check(r.value("status") == "INCOMPLETE" && p.writes == 0,"local permit closed prevents all writes");
    }
    {
        Peer p; p.s[2] = 300; const auto r = run(options(p,path()));
        check(r.value("status") == "INCOMPLETE" && p.writes == 0,"busy session cannot be stolen");
    }
    {
        Peer p; auto o = options(p,path()); o.probe = true; o.files.clear(); const auto r = run(o);
        check(r.value("status") == "PROBED" && p.writes == 0 && !QFileInfo::exists(o.receipt),"probe reads only and creates no receipt");
    }
    {
        Peer p; p.rejectStart = true; const auto r = run(options(p,path()));
        check(r.value("status") == "INCOMPLETE" && p.ops == QVector<int>{1},"application rejection stops without retry");
    }
    {
        Peer p; p.ignoreAck = true; const auto r = run(options(p,path()));
        check(r.value("status") == "INCOMPLETE" && p.ops == QVector<int>{1},"FC16 echo without application ACK cannot succeed");
    }
    {
        Peer p; p.reboot = true; const auto r = run(options(p,path()));
        check(r.value("status") == "INCOMPLETE" && p.ops == QVector<int>{1},"PLC restart invalidates session without replay");
    }
    {
        Peer p; p.changeOwner = true; const auto r = run(options(p,path()));
        check(r.value("status") == "INCOMPLETE" && !p.ops.contains(4),"foreign ownership stops without aborting another session");
    }
    for (int fault = 0; fault < 3; ++fault) {
        Peer p; p.badReadback = fault == 0; p.badCompletion = fault == 1; p.dirtyClose = fault == 2;
        const auto r = run(options(p,path()));
        check(r.value("status") == "INCOMPLETE" && !p.ops.contains(5),"bad readback, FTP completion or unclosed sockets cannot FINISH");
    }
    {
        Peer p; p.holdFtp = true; const auto r = run(options(p,path()));
        check(r.value("status") == "INCOMPLETE" && !p.ops.contains(5),"FTP wait bounded and never silently succeeds");
    }
    {
        Peer p; p.holdFtp = true; bool cancel = false;
        const auto r = run(options(p,path()),nullptr,[&](FileBridge::Transfer& worker,const QJsonObject& e) {
            if (!cancel && e.value("phase") == "ftp") { cancel = true; worker.requestInterruption(); }
        });
        check(r.value("status") == "CANCELLED" && p.ops.contains(4) && p.ops.contains(6) && p.s[2] == 0,"cooperative cancel aborts, verifies cleanup and releases only own session");
    }
    {
        Peer p; p.dropReply = true; const auto r = run(options(p,path()));
        check(r.value("status") == "INCOMPLETE" && p.ops == QVector<int>{1},"lost write response never reconnects or resends");
    }
    {
        Peer p; p.holdFtp = true; p.cleanupUnknown = true; bool cancel = false;
        const auto r = run(options(p,path()),nullptr,[&](FileBridge::Transfer& worker,const QJsonObject& e) {
            if (!cancel && e.value("phase") == "ftp") { cancel = true; worker.requestInterruption(); }
        });
        check(r.value("status") == "CANCELLED" && p.ops.contains(4) && !p.ops.contains(6),"unknown cleanup cannot be released or claimed successful");
    }
    {
        Peer p; auto o = options(p,temp.filePath("missing/receipt.json")); const auto r = run(o);
        check(r.value("status") == "INCOMPLETE" && p.reads == 0 && p.writes == 0,"unwritable receipt prevents connecting to PLC");
    }
    {
        Peer p; p.brokenHeader = true; const auto r = run(options(p,path()));
        check(r.value("status") == "INCOMPLETE" && p.writes == 0,"bad MBAP rejected before writes");
    }
    {
        Peer p; p.torn = true; const auto r = run(options(p,path()));
        check(r.value("status") == "INCOMPLETE" && p.writes == 0 && p.reads == 10,"unstable snapshot bounded and prevents writes");
    }
    {
        Peer p; auto o = options(p,path()); o.files.clear(); const auto r = run(o);
        check(r.value("status") == "INCOMPLETE" && p.reads == 0 && p.writes == 0,"invalid bundle rejected entirely offline");
    }
    if (app.arguments().size() > 1) {
        const auto job = readNativeJob(app.arguments()[1]); check(job.error.isEmpty(),"actual robot ZIP opens");
        Peer p; auto o = options(p,path()); o.files = job.files;
        const auto r = run(o); qint64 bytes = 0; for (auto it = job.files.cbegin(); it != job.files.cend(); ++it) bytes += it.value().size();
        check(r.value("status") == "COMPLETE" && p.files == job.files && job.files.size() == 7 && bytes == 3970 && p.invalid == 0,"actual 7-file 3970-byte ZIP passes native client through loopback peer");
        const auto source = temp.filePath("delivery.nc");
        const QByteArray raw = "\xEF\xBB\xBF" "G21 G17 G90 G94 G91.1\r\nG1 X1 F300\r\nG3 X0 Y1 I-1 J0\r\nM30\r\n";
        { QFile file(source); check(file.open(QIODevice::WriteOnly) && file.write(raw) == raw.size(), "delivery source written"); }
        const auto generated = Delivery::compile(source, job);
        check(generated.runnable() && generated.files.size() == 7 && generated.program == "gcode_path", "G-code delivery prepares complete three-program project");
        check(generated.inputBytes == raw && generated.files.value("capture_home.srd").isEmpty() &&
            generated.files.value("go_home.srp") == job.files.value("go_home.srp") &&
            generated.files.value("go_home.srd") == job.files.value("go_home.srd"), "source bytes, empty variables and reviewed return pair preserved");
        const auto package = Delivery::save(generated, source, temp.filePath("deliveries"));
        check(package.error.isEmpty() && QFileInfo::exists(package.zip + ".mapping.json") &&
            QFileInfo::exists(package.zip + ".source.gcode") && QFileInfo::exists(package.zip + ".conversion.txt"), "one action saves archive, original source, mapping and conversion report");
        auto reopened = readNativeJob(package.zip);
        check(reopened.error.isEmpty() && reopened.mappingWarning.isEmpty() && reopened.files == generated.files &&
            restoreMappedPreview(reopened, job), "generated delivery ZIP roundtrips with hash-checked G-code preview");
        auto changed = reopened; changed.files["go_home.srp"].append("# edit\n");
        check(!restoreMappedPreview(changed, job) && !changed.runnable() && changed.points.isEmpty(),
            "edited return program loses previous conversion trust and preview");
        const auto second = Delivery::save(generated, source, temp.filePath("deliveries"));
        check(second.error.isEmpty() && second.directory != package.directory, "separate local deliveries never overwrite an earlier job");
        auto object = Delivery::manifest(generated, source);
        const auto pending = [](const QJsonObject& value) {
            const auto programs = value.value("programs").toArray();
            return programs.size() == 3 && std::all_of(programs.begin(), programs.end(), [](const QJsonValue& program) {
                return program.toObject().value("gdi_number").isNull() && !program.toObject().value("binding_confirmed").toBool();
            });
        };
        check(pending(object) && object.value("remote_directory").isNull() && !object.value("robot_started").toBool(), "offline preparation never invents GDI binding or remote success");
        Peer generatedPeer; auto generatedOptions = options(generatedPeer, path()); generatedOptions.files = generated.files;
        const auto uploaded = run(generatedOptions);
        check(uploaded.value("status") == "COMPLETE" && generatedPeer.files == generated.files && generatedPeer.invalid == 0 &&
            std::all_of(generatedPeer.addresses.begin(), generatedPeer.addresses.end(), [](int address) { return address == 6000 || address == 6120; }),
            "G-code to native project to Modbus file-channel completes through local peer without motion writes");
        check(Delivery::record(package, generated, uploaded).isEmpty(), "upload result linked to saved delivery record");
        QFile manifest(package.manifest); manifest.open(QIODevice::ReadOnly); object = QJsonDocument::fromJson(manifest.readAll()).object(); manifest.close();
        check(object.value("status") == "UPLOADED_GDI_BINDING_PENDING" && pending(object) &&
            !object.value("robot_loaded").toBool() && !object.value("robot_enabled").toBool() && !object.value("robot_started").toBool(),
            "successful transfer leaves all GDI and motion states pending");
        check(Delivery::handoff(generated, uploaded).contains(uploaded.value("remote_directory").toString()) &&
            Delivery::handoff(generated, {}).contains("尚未"), "handoff shows exact new directory only after successful transfer");
        const auto processSource = temp.filePath("process.nc");
        { QFile file(processSource); file.open(QIODevice::WriteOnly); file.write("G90\nM3 S500\nG1 X1 F300\nM5\nM30\n"); }
        const auto rejected = Delivery::compile(processSource, job);
        check(!rejected.runnable() && !Delivery::save(rejected, processSource, temp.filePath("invalid")).error.isEmpty(), "unsupported process output blocks delivery before saving or connecting");
        check(Delivery::compile(processSource, job, {5,.05,true}).runnable(), "only explicit process omission permits dry-run conversion");
        check(Delivery::compile(source, job, {}, false).files.size() == 5, "return program optional without changing original converter output");
    }
    std::cout << checks << " checks; " << failures << " failures (local PC simulators only)\n";
    return failures ? 1 : 0;
#endif
}
