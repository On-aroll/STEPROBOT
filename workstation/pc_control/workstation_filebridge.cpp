#include "workstation_filebridge.h"
#include "workstation_directftp.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTcpSocket>
#include <QtEndian>
#include <algorithm>
#include <functional>
#include <stdexcept>

namespace FileBridge {
namespace {
[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toUtf8().constData()); }
struct Cancelled {};
void appendWord(QByteArray& b, quint16 w) { b.append(char(w >> 8)); b.append(char(w & 255)); }
quint16 word(const QByteArray& b, int i) { return qFromBigEndian<quint16>(b.constData() + i); }
quint32 number(const QJsonObject& s, const char* key) { return quint32(s.value(key).toDouble()); }
quint32 u32(const QVector<quint16>& w, int i) { return quint32(w[i]) << 16 | w[i + 1]; }
void put32(QVector<quint16>& w, int i, quint32 n) { w[i] = quint16(n >> 16); w[i + 1] = quint16(n); }
bool completion(quint32 n) { return n == 226 || n == 250; }

// Constructed and used only in Transfer::run(), never on the GUI thread.
class Wire {
public:
    explicit Wire(const Options& o) : timeout(o.ioTimeoutMs), unit(o.unit) {
        // Industrial Modbus TCP must connect directly to the configured PLC.
        // A default/system HTTP proxy cannot transport this raw TCP channel.
        socket.setProxy(QNetworkProxy::NoProxy);
        socket.connectToHost(o.host, o.port);
        if (!socket.waitForConnected(timeout)) fail("PLC连接失败：" + socket.errorString());
        socket.setSocketOption(QAbstractSocket::LowDelayOption, 1);
    }
    QVector<quint16> read() {
        QByteArray pdu(1, char(3)); appendWord(pdu, StatusAddress); appendWord(pdu, 64);
        const auto r = request(pdu);
        if (r.size() != 130 || quint8(r[1]) != 128) { healthy = false; fail("PLC状态响应长度不正确"); }
        QVector<quint16> w; w.reserve(64);
        for (int i = 0; i < 64; ++i) w.append(word(r, 2 + i * 2));
        return w;
    }
    void write(int address, const QVector<quint16>& w) {
        QByteArray pdu(1, char(16)); appendWord(pdu, quint16(address)); appendWord(pdu, quint16(w.size()));
        pdu.append(char(w.size() * 2));
        for (auto v : w) appendWord(pdu, v);
        const auto r = request(pdu);
        if (r.size() != 5 || word(r, 1) != address || word(r, 3) != w.size()) {
            healthy = false; fail("PLC写响应回显不正确；结果待核对，未重发");
        }
    }
    bool healthy = true;
private:
    QByteArray request(const QByteArray& pdu) {
        try {
            QElapsedTimer deadline; deadline.start();
            auto remaining = [&] { return std::max(0, timeout - int(deadline.elapsed())); };
            QByteArray packet; appendWord(packet, ++tid); appendWord(packet, 0);
            appendWord(packet, quint16(pdu.size() + 1)); packet.append(char(unit)); packet += pdu;
            if (socket.write(packet) != packet.size()) fail("Modbus发送失败，未自动重发");
            while (socket.bytesToWrite())
                if (!remaining() || !socket.waitForBytesWritten(remaining())) fail("Modbus发送超时，结果待核对");
            auto exact = [&](int length) {
                QByteArray bytes;
                while (bytes.size() < length) {
                    if (socket.bytesAvailable() == 0 &&
                        (!remaining() || !socket.waitForReadyRead(remaining())))
                        fail("Modbus响应中断或超时；结果待核对，未自动重发");
                    bytes += socket.read(length - bytes.size());
                }
                return bytes;
            };
            const auto h = exact(7);
            const int n = word(h, 4);
            if (word(h, 0) != tid || word(h, 2) != 0 || quint8(h[6]) != unit || n < 2 || n > 254)
                fail("Modbus响应事务、单元或MBAP头不匹配");
            const auto r = exact(n - 1);
            if (quint8(r[0]) == (quint8(pdu[0]) | 0x80))
                fail("PLC返回Modbus异常：" + QString::fromLatin1(r.toHex()));
            if (r[0] != pdu[0]) fail("Modbus响应功能码不匹配");
            return r;
        } catch (...) { healthy = false; throw; }
    }
    QTcpSocket socket;
    int timeout;
    quint8 unit;
    quint16 tid = 0;
};

QJsonObject readStatus(Wire& wire) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        const auto w = wire.read();
        if (w[0] != 0xF7B2 || w[1] != 1) fail("PLC文件桥签名不匹配（HR6200/6201）");
        if (w[38] != w[63] || (w[38] & 1)) continue;
        QJsonObject s;
        const std::pair<const char*, int> singles[] = {
            {"state",2},{"error",3},{"ack_op",6},{"flags",7},{"verified",20},{"expected",21},
            {"ordinal",22},{"ftp_step",23},{"ftp_reply",24},{"ftp_error",25},{"ftp_error_stage",26},
            {"reject",31},{"max_files",37},{"chunk",41},{"data_close_error",42},{"control_close_error",43},
            {"mode",44},{"upload_completion",45},{"readback_completion",46},{"capabilities",47},{"revision",48},
            {"auto_configured",49},{"auto_busy",50},{"auto_ready",51},{"manual_number",52},
            {"auto_number",53},{"auto_capacity",54},{"auto_idle",59},{"tag1",57},{"tag2",58},{"tag3",60},{"ftp_batch_bytes",61},{"ftp_backend",62}};
        const std::pair<const char*, int> doubles[] = {
            {"ack",4},{"session",8},{"boot",10},{"received",12},{"size",14},{"crc32",16},
            {"expected_crc32",18},{"sent",27},{"readback",29},{"reject_seq",32},{"reject_session",34},{"max_file",39},
            {"auto_project_crc32",55}};
        for (auto [key,i] : singles) s.insert(key, int(w[i]));
        for (auto [key,i] : doubles) s.insert(key, double(u32(w,i)));
        return s;
    }
    fail("PLC状态快照连续变化，停止传输并检查扫描周期/映射");
}

class Upload {
public:
    Upload(const Options& o, Wire& w, std::function<bool()> cancelled,
        std::function<void(const QJsonObject&)> progress)
        : o(o), wire(w), cancelled(std::move(cancelled)), progress(std::move(progress)) {
        clock.start();
    }
    QJsonObject first, last, recovery;
    QJsonArray receipts;
    quint32 session = 0, boot = 0;
    void upload() {
        first = last = readStatus(wire);
        if ((number(first,"flags") & 3) != 3)
            fail("PLC文件许可未开或FTP连接关闭尚未确认；未复位、未开始新上传");
        if (number(first,"state") != 0 && number(first,"state") != 400 && number(first,"state") != 900)
            fail("PLC文件会话仍在工作或连接清理未确认；未复位、未接管其他上传");
        if (number(first,"max_file") != MaxFile || number(first,"chunk") != Chunk || number(first,"max_files") != MaxFiles)
            fail("PLC文件容量/协议与上位机不一致");
        if (o.replaceAuto) {
            if ((number(first,"revision") != 0x0103 && number(first,"revision") != 0x0104) || !(number(first,"capabilities") & 4))
                fail("PLC文件桥尚不支持R6.4固定作业更新；请核对配套文件桥版本。未写入文件命令或心跳");
            if (number(first,"auto_number") != o.programNumber || number(first,"manual_number") != 10 ||
                number(first,"auto_capacity") != 15 || number(first,"auto_project_crc32") != crc32(o.project.toLatin1()))
                fail(QString("PLC固定工程或编号与本次作业不一致：PLC手动号%1、自动号%2、容量%3；本次目标%4 / %5号。请核对现场配置。未写入文件命令或心跳")
                    .arg(number(first,"manual_number")).arg(number(first,"auto_number")).arg(number(first,"auto_capacity")).arg(o.project).arg(o.programNumber));
            if (number(first,"auto_configured") != 1)
                fail("PLC固定入口配置未生效（HR6249=0）。请在InoProShop检查 GVL_FileBridge.xFixedMappingConfirmed 是否为TRUE，以及 GVL_RobotBridge.uiFixedJobGuardVersion 是否由配套程序发布为140。在线改过的确认标志可能在重启后恢复FALSE；请核对并保存已确认的配置。未写入文件命令或心跳");
            if (number(first,"auto_idle") != 1)
                fail("机器人尚不满足上传条件（HR6259=0）。请退出微调，停止、断使能，等待机器人停稳且无进行中的操作，再重新发起上传。未写入文件命令或心跳");
        }
        if(o.directFtp && (number(first,"revision")!=0x0104 || !(number(first,"capabilities")&8)))
            fail("电脑直传需要PLC文件桥0104及直传回读能力；未连接控制柜");
        for (auto it = o.files.cbegin(); it != o.files.cend(); ++it)
            if (it.value().isEmpty() && !(number(first,"capabilities") & 1))
                fail("工程有空文件，PLC未声明V1.1空文件能力；未写入命令或心跳");
        if (number(first,"state") == 900) releaseClosedFailure();
        boot = number(last,"boot");
        do { session = QRandomGenerator::global()->generate(); } while (!session || session == number(last,"session"));
        std::unique_ptr<DirectFtp> direct;
        qint64 lastGuardCheck=-1000000;
        auto service=[&] {
            checkCancel();heartbeat();
            if(clock.elapsed()-lastGuardCheck>=100) {
                const auto s=ownedStatus();
                if(number(s,"state")!=100 || number(s,"auto_busy")!=1 || number(s,"auto_idle")!=1 || !(number(s,"flags")&1))
                    fail("PLC上传许可或空闲联锁已撤销，电脑直传停止；本次作业不可用");
                lastGuardCheck=clock.elapsed();
            }
        };
        int ordinal = 0;
        for (auto it = o.files.cbegin(); it != o.files.cend(); ++it) {
            ++ordinal; file = it.key();
            QElapsedTimer fileClock;fileClock.start();
            const auto& data = it.value();
            const quint32 crc = crc32(data);
            auto names = o.project.toLatin1(); names.append('\0'); names += file.toLatin1(); names.append('\0');
            last = command(1, quint32(data.size()), 0, names, crc, quint16(o.files.size()), quint16(ordinal));
            notify("receiving");
            const auto startMs=fileClock.elapsed();
            if(o.directFtp) {
                lastGuardCheck=-1000000;
                if(!direct) {
                    direct=std::make_unique<DirectFtp>(o.cabinetHost,o.cabinetPort,o.ioTimeoutMs,service);
                    direct->login(o.ftpUser,o.ftpPassword,o.project);
                }
                notify("direct_upload");direct->upload(file,data);
                if(ordinal==o.files.size())direct->quit();
            }
            for (int offset = 0; !o.directFtp && offset < data.size(); offset += Chunk) {
                const auto part = data.mid(offset, Chunk);
                last = command(2, 0, quint32(offset), part);
                if (number(last,"received") != offset + part.size()) fail("PLC已确认的文件偏移不正确");
                notify("receiving");
            }
            if (!o.directFtp && (number(last,"received") != data.size() || number(last,"size") != data.size() ||
                number(last,"crc32") != crc || number(last,"expected_crc32") != crc))
                fail("PLC内存文件长度或CRC不一致，未启动FTP");
            const auto transferMs=fileClock.elapsed()-startMs;
            const auto deployMs=fileClock.elapsed();
            last = command(3); notify("ftp");
            QElapsedTimer wait; wait.start();
            while (true) {
                checkCancel(); heartbeat(); last = ownedStatus(); notify("ftp");
                const auto state = number(last,"state");
                if (state == 200 && number(last,"verified") == ordinal) {
                    if ((number(last,"flags") & 6) != 6 || number(last,"ordinal") != ordinal ||
                        number(last,"expected") != o.files.size() || number(last,"sent") != (o.directFtp?0:data.size()) ||
                        number(last,"readback") != data.size() || number(last,"size") != data.size() ||
                        number(last,"received") != (o.directFtp?0:data.size()) || number(last,"crc32") != crc ||
                        number(last,"expected_crc32") != crc || number(last,"mode") != (o.directFtp?3:o.replaceAuto ? 2 : 1) ||
                        (!o.directFtp && !completion(number(last,"upload_completion"))) || !completion(number(last,"readback_completion")) ||
                        number(last,"error") || number(last,"ftp_error") || number(last,"ftp_error_stage") ||
                        number(last,"data_close_error") || number(last,"control_close_error"))
                        fail("PLC报告的回读验证或连接清理不完整");
                    break;
                }
                if (state == 900 || state == 910) fail("PLC FTP失败：" + QString::fromUtf8(QJsonDocument(last).toJson(QJsonDocument::Compact)));
                if (wait.elapsed() >= o.ftpTimeoutMs) fail("FTP等待超时；未自动重试，停止心跳后由PLC租约清理");
                QThread::msleep(10);
            }
            receipts.append(QJsonObject{{"name",file},{"bytes",double(data.size())},
                {"crc32",QString::number(crc,16).rightJustified(8,'0')},
                {"local_sha256",QString::fromLatin1(QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex())},
                {"start_ack_ms",double(startMs)},{"transfer_to_plc_ms",o.directFtp?QJsonValue():QJsonValue(double(transferMs))},
                {"direct_ftp_upload_ms",o.directFtp?QJsonValue(double(transferMs)):QJsonValue()},
                {"plc_ftp_verify_ms",double(fileClock.elapsed()-deployMs)},{"total_ms",double(fileClock.elapsed())},
                {"plc_status",last}});
            notify("verified");
        }
        last = command(5);
        if (number(last,"state") != 400 || (number(last,"flags") & 10) != 10 ||
            number(last,"verified") != o.files.size() || number(last,"expected") != o.files.size())
            fail("PLC未确认整组工程完成");
        if (o.replaceAuto && (number(last,"auto_ready") != 1 || number(last,"auto_number") != o.programNumber ||
            number(last,"auto_project_crc32") != crc32(o.project.toLatin1()) || number(last,"tag1") != o.tag[0] ||
            number(last,"tag2") != o.tag[1] || number(last,"tag3") != o.tag[2])) fail("PLC未放行本次固定作业入口或版本不一致");
    }
    QString cancelCleanly() {
        // Cancel or close a failed attempt only over its intact original transport.
        // Never reconnect, retransmit an uncertain request or take another owner's lease.
        if (!wire.healthy || !session || commandUncertain) return "未取得会话或通讯结果未知；未发送清理命令";
        cleaning = true;
        try {
            last = readStatus(wire);
            if (number(last,"boot") != boot || number(last,"session") != session)
                return "PLC启动号或所有者变化；未发送清理命令";
            if (number(last,"state") == 910) return "PLC连接清理未确认；保留故障，未释放会话";
            if (number(last,"state") != 900 && number(last,"state") != 400) last = command(4);
            QElapsedTimer wait; wait.start();
            while (number(last,"state") == 310 || !(number(last,"flags") & 2)) {
                if (wait.elapsed() >= o.cleanupTimeoutMs || number(last,"state") == 910)
                    return "清理未确认；需在PLC核对连接和部分目录";
                heartbeat(); QThread::msleep(100); last = ownedStatus(); notify("cancelling");
            }
            if (number(last,"state") != 900 && number(last,"state") != 400)
                return "清理状态异常；需人工核对";
            last = command(6);
            return number(last,"state") == 0 ? "已确认连接关闭并释放会话；部分工程目录仍保留" : "释放结果待核对";
        } catch (const std::exception& e) { return "清理结果未知：" + QString::fromUtf8(e.what()); }
    }
private:
    void releaseClosedFailure() {
        // State 900 is terminal. RELEASE is already supported by the PLC and
        // cannot clear an active transfer or a handle with unknown close status.
        const auto closed = last;
        const auto owner = number(closed,"session"), previousAck = number(closed,"ack");
        if (!owner || !previousAck || previousAck == 0xFFFFFFFFu || number(closed,"auto_busy"))
            fail("旧文件会话身份或清理状态不完整；未释放，请核对PLC文件桥");
        QThread::msleep(5);
        last = readStatus(wire);
        for (const auto* key : {"state","flags","session","boot","ack","ack_op","auto_idle","auto_busy","auto_configured","auto_project_crc32","auto_number","manual_number","revision"})
            if (number(last,key) != number(closed,key)) fail("旧文件会话状态发生变化；未释放或开始上传");
        recovery = {{"status","RELEASE_REQUESTED"},{"previous_session",double(owner)},
            {"previous_error",closed.value("error")},{"boot",closed.value("boot")},{"release_sequence",double(previousAck+1)}};
        notify("recovering"); checkCancel();
        wire.write(Command,frame(6,previousAck+1,owner,number(closed,"boot")));
        QElapsedTimer wait; wait.start();
        while (wait.elapsed() < o.ackTimeoutMs) {
            checkCancel(); last = readStatus(wire);
            if (number(last,"boot") != number(closed,"boot") || number(last,"session") != owner)
                fail("释放旧会话时PLC启动号或所有者变化；未继续上传");
            if (number(last,"reject_session") == owner && number(last,"reject_seq") == previousAck+1 && number(last,"reject"))
                fail("PLC拒绝释放旧文件会话；未继续上传");
            if (number(last,"ack") == previousAck+1 && number(last,"ack_op") == 6) {
                if (number(last,"state") != 0 || (number(last,"flags") & 3) != 3 || number(last,"auto_busy"))
                    fail("PLC未确认旧文件会话已释放；未继续上传");
                recovery.insert("status","RELEASED"); notify("recovered"); return;
            }
            QThread::msleep(2);
        }
        fail("释放旧文件会话结果未知；未重发、未开始上传");
    }
    void checkCancel() { if (!cleaning && cancelled()) throw Cancelled{}; }
    void heartbeat() {
        checkCancel();
        if (clock.elapsed() - lastHeartbeat < 500) return;
        QVector<quint16> w(5); put32(w,0,session); put32(w,2,boot); w[4] = ++hb;
        wire.write(Heartbeat,w); lastHeartbeat = clock.elapsed();
    }
    QJsonObject ownedStatus() {
        const auto s = readStatus(wire);
        if (number(s,"boot") != boot) fail("PLC已重启；上传失效，未自动重试");
        if (number(s,"session") != session) fail("PLC文件会话所有者发生变化");
        return s;
    }
    QJsonObject command(quint16 op, quint32 size = 0, quint32 offset = 0, const QByteArray& payload = {},
        quint32 crc = 0, quint16 count = 0, quint16 ordinal = 0) {
        checkCancel(); ++seq;
        heartbeat();
        commandUncertain = true;
        wire.write(Command,frame(op,seq,session,boot,size,offset,payload,crc,count,ordinal,o.directFtp?3:o.replaceAuto ? 2 : 1,o.tag));
        QElapsedTimer wait; wait.start();
        while (wait.elapsed() < o.ackTimeoutMs) {
            checkCancel(); heartbeat(); last = readStatus(wire);
            if (number(last,"boot") != boot) fail("PLC在应用确认前重启；未自动重发");
            if (number(last,"reject_session") == session && number(last,"reject_seq") == seq && number(last,"reject")) {
                commandUncertain = false;
                fail(QString("PLC拒绝命令，错误码0x%1").arg(number(last,"reject"),4,16,QLatin1Char('0')));
            }
            if (number(last,"session") == session && number(last,"ack") == seq && number(last,"ack_op") == op) {
                commandUncertain = false;
                if (!cleaning && (number(last,"state") == 900 || number(last,"state") == 910))
                    fail("PLC文件会话失败：" + QString::fromUtf8(QJsonDocument(last).toJson(QJsonDocument::Compact)));
                return last;
            }
            QThread::msleep(2);
        }
        fail("PLC应用层ACK超时；Modbus写应答不能代替文件确认，未自动重发");
    }
    void notify(const QString& phase) {
        progress(QJsonObject{{"phase",phase},{"name",file},{"status",last}});
    }
    const Options& o;
    Wire& wire;
    std::function<bool()> cancelled;
    std::function<void(const QJsonObject&)> progress;
    QElapsedTimer clock;
    qint64 lastHeartbeat = -1000000;
    quint32 seq = 0;
    quint16 hb = 0;
    bool cleaning = false;
    bool commandUncertain = false;
    QString file;
};
}

quint32 crc32(const QByteArray& bytes) {
    quint32 crc = 0xFFFFFFFFu;
    for (const auto c : bytes) {
        crc ^= quint8(c);
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

QString validate(const QMap<QString, QByteArray>& files, bool replaceAuto) {
    if (files.isEmpty() || files.size() > MaxFiles) return "工程须包含1～32个文件";
    static const QRegularExpression name("\\A[a-z0-9_]{1,44}\\.(?:srp|srd)\\z");
    qint64 total = 0; int programs = 0;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        if (it.key().size() > 48 || !name.match(it.key()).hasMatch()) return "只接受平铺的小写ASCII SRP/SRD文件：" + it.key();
        if (it.value().size() > MaxFile) return "单文件超过1 MiB：" + it.key();
        total += it.value().size();
        if (it.key().endsWith(".srp")) {
            ++programs;
            if (!files.contains(it.key().chopped(1) + 'd')) return "缺少程序配对的SRD：" + it.key();
        }
    }
    if (total > MaxTotal) return "工程文件合计超过5 MiB";
    if (replaceAuto) {
        if (programs < 2 || files.size() != programs * 2) return "固定作业须含入口及1～15个轨迹的完整SRP/SRD配对";
        QSet<QString> expected{"auto_main.srp", "auto_main.srd"};
        for (int n = 1; n < programs; ++n) {
            const auto base = QString("auto_%1").arg(n, 2, 10, QLatin1Char('0'));
            expected.insert(base + ".srp"); expected.insert(base + ".srd");
        }
        for (auto it = files.cbegin(); it != files.cend(); ++it)
            if (!expected.contains(it.key())) return "作业文件集不连续或含受保护文件：" + it.key();
    } else if (!files.contains("_project.srd") || !programs) return "需要_project.srd及至少一对SRP/SRD";
    return {};
}

bool privateEndpoint(const QString& host) {
    QHostAddress a;
    if (!a.setAddress(host) || a.protocol() != QAbstractSocket::IPv4Protocol) return false;
    const auto n = a.toIPv4Address();
    return (n >> 24) == 10 || (n >> 20) == 0xAC1 || (n >> 16) == 0xC0A8;
}

QString newProject() {
    return "pcjob_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_") +
        QString::number(QRandomGenerator::global()->generate(),16).rightJustified(8,'0') + ".sr";
}

QVector<quint16> frame(quint16 op, quint32 sequence, quint32 session, quint32 boot,
    quint32 size, quint32 offset, const QByteArray& payload, quint32 crc, quint16 count, quint16 ordinal, quint16 mode,
    std::array<quint16,3> tag) {
    if (!sequence || !session || payload.size() > Chunk) fail("文件帧序号、所有者或载荷无效");
    QVector<quint16> w(120);
    w[0] = 0xF7B1; w[1] = 1; w[2] = op;
    put32(w,3,sequence); put32(w,5,session); put32(w,7,boot); put32(w,9,size); put32(w,11,offset);
    w[13] = quint16(payload.size()); put32(w,14,crc); w[16] = count; w[17] = ordinal;
    w[18] = op == 1 ? mode : 0;
    if(op==1 && (mode==2 || mode==3)) { w[19]=tag[0];w[116]=tag[1];w[117]=tag[2]; }
    for (int i = 0; i < payload.size(); ++i) w[20 + i / 2] |= quint16(quint8(payload[i])) << (i % 2 ? 0 : 8);
    QByteArray bytes; for (int i = 0; i < 118; ++i) appendWord(bytes,w[i]);
    put32(w,118,crc32(bytes)); return w;
}

Transfer::Transfer(Options o, QObject* parent) : QThread(parent), options(std::move(o)) {}

void Transfer::run() {
    QElapsedTimer elapsed;elapsed.start();
    QJsonObject result{{"protocol",1},{"implementation","qt-native-1.2-batch"},{"host",options.host},
        {"transport",options.directFtp?"pc_direct_ftp_plc_verify":"plc_ftp"},
        {"cabinet_host",options.directFtp?QJsonValue(options.cabinetHost):QJsonValue()},
        {"port",options.port},{"unit",options.unit},{"project",options.project},
        {"remote_directory","/UserPrograms/" + options.project},{"native_bundle",true},
        {"robot_loaded",false},{"robot_started",false},{"receipt",options.receipt},
        {"replace_auto",options.replaceAuto},{"program_number",options.replaceAuto ? QJsonValue(options.programNumber) : QJsonValue()}};
    bool reserved = false;
    try {
        QHostAddress host(options.host);
        if ((!privateEndpoint(options.host) && !host.isLoopback()) || !options.port || !options.unit || options.unit > 247)
            fail("须指定PLC IPv4地址、有效端口及1～247单元号");
        if (!options.probe) {
            if(options.directFtp && (!options.replaceAuto ||
                (!privateEndpoint(options.cabinetHost) && !QHostAddress(options.cabinetHost).isLoopback()) ||
                !options.cabinetPort || options.ftpUser.isEmpty() || options.ftpPassword.isEmpty()))
                fail("电脑直传仅支持固定自动作业，须填写控制柜IPv4地址、FTP端口及账号密码");
            const auto error = validate(options.files, options.replaceAuto);
            if (!error.isEmpty()) fail(error);
            if (options.replaceAuto && (!options.programNumber || options.programNumber > 255 || options.programNumber == 10))
                fail("固定作业号须为1～255且不能占用手动服务10号");
            if(options.replaceAuto && std::any_of(options.tag.begin(),options.tag.end(),[](quint16 n){return n==0 || n>32767;}))
                fail("自动作业版本标记无效");
            const QRegularExpression pattern("\\Apcjob_[a-z0-9_]{3,39}\\.sr\\z");
            if (!pattern.match(options.project).hasMatch() || options.project.size() < 12 || options.project.size() > 48)
                fail("目标必须是新的pcjob_*.sr工程名");
            QFile placeholder(options.receipt);
            if (!placeholder.open(QIODevice::WriteOnly | QIODevice::NewOnly)) fail("无法新建回执（现有文件不会覆盖）：" + placeholder.errorString());
            result.insert("status","PENDING");
            const auto bytes = QJsonDocument(result).toJson();
            if (placeholder.write(bytes) != bytes.size() || !placeholder.flush()) fail("回执无法写入，未连接PLC");
            reserved = true;
        }
        if (isInterruptionRequested()) throw Cancelled{};
        Wire wire(options);
        if (options.probe) {
            result.insert("status","PROBED"); result.insert("plc_status",readStatus(wire));
        } else {
            Upload upload(options,wire,[this] { return isInterruptionRequested(); },
                [this](const QJsonObject& e) { emit progress(e); });
            try { upload.upload(); result.insert("status","COMPLETE"); }
            catch (const Cancelled&) {
                result.insert("status","CANCELLED"); result.insert("cleanup",upload.cancelCleanly());
            }
            catch (const std::exception& e) {
                result.insert("status","INCOMPLETE"); result.insert("error",QString::fromUtf8(e.what()));
                result.insert("preflight_failed",upload.session==0);
                result.insert("failure_status",upload.last);
                result.insert("cleanup",upload.session==0?"上传预检未通过；未取得文件会话，未向控制柜写入本次文件":upload.cancelCleanly());
            }
            result.insert("initial_status",upload.first);
            if (!upload.recovery.isEmpty()) result.insert("previous_session_cleanup",upload.recovery);
            result.insert("session",double(upload.session)); result.insert("plc_boot",double(upload.boot));
            result.insert("files",upload.receipts); result.insert("final_status",upload.last);
        }
    } catch (const Cancelled&) { result.insert("status","CANCELLED"); }
    catch (const std::exception& e) { result.insert("status","INCOMPLETE"); result.insert("error",QString::fromUtf8(e.what())); }
    result.insert("completed_at",QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    result.insert("total_ms",double(elapsed.elapsed()));
    if (reserved) {
        QSaveFile output(options.receipt);
        const auto bytes = QJsonDocument(result).toJson();
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
            result.insert("transfer_status",result.value("status"));
            result.insert("status","RECEIPT_ERROR"); result.insert("error","回执封存失败；PLC结果见界面，勿自动重复上传");
        }
    }
    emit outcome(result);
}
}
