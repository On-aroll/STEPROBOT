#include "../workstation_modbus.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>

#include <iostream>

namespace {

int failures = 0;
void check(bool ok, const char* reason) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << reason << '\n';
    }
}

static_assert(ModbusMap::diNumber(0, 0) == 33 && ModbusMap::diNumber(3, 15) == 96, "DI 打包覆盖 DI33-DI96");
static_assert(ModbusMap::aiNumber(4) == 5 && ModbusMap::aiNumber(31) == 32, "Word5-32 映射 AI5-AI32");

template <typename Pred>
bool waitFor(Pred pred, int ms) {
    if (pred()) return true;
    QEventLoop loop;
    QTimer deadline, tick;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&tick, &QTimer::timeout, &loop, &QEventLoop::quit);
    tick.start(15);
    deadline.start(ms);
    while (!pred() && deadline.isActive()) loop.exec();
    return pred();
}

// 最小 Modbus TCP 从站模拟：按手册映射的 32 个寄存器应答 FC03/04/06/16。
class SlaveSimulator : public QObject {
public:
    bool start() {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            client = server.nextPendingConnection();
            connect(client, &QTcpSocket::readyRead, this, [this] {
                rx += client->readAll();
                process();
            });
        });
        return server.listen(QHostAddress::LocalHost);
    }
    quint16 port() const { return server.serverPort(); }
    quint16 regs[ModbusMap::WordCount] = {};
    bool holdStaleReply = false;
    int readRequests = 0, writeRequests = 0;

private:
    void process() {
        while (rx.size() >= 8) {
            const int length = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(rx.constData() + 4));
            const int total = 6 + length;
            if (rx.size() < total) return;
            const quint16 id = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(rx.constData()));
            const quint8 unitId = static_cast<quint8>(rx[6]);
            const quint8 fc = static_cast<quint8>(rx[7]);
            const QByteArray pdu = rx.mid(8, total - 8);
            rx.remove(0, total);
            if (unitId != 1) continue;
            if (fc == 3 || fc == 4) {
                ++readRequests;
                const auto start = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(pdu.constData()));
                const auto count = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(pdu.constData() + 2));
                QByteArray out;
                if (start + count > ModbusMap::WordCount)
                    out.append(char(fc | 0x80)).append(char(2));
                else {
                    out.append(char(fc));
                    out.append(char(2 * count));
                    for (int i = 0; i < count; ++i) {
                        const quint16 big = qToBigEndian<quint16>(regs[start + i]);
                        out.append(reinterpret_cast<const char*>(&big), 2);
                    }
                }
                send(id, unitId, out);
            } else if (fc == 6) {
                ++writeRequests;
                const auto address = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(pdu.constData()));
                const auto value = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(pdu.constData() + 2));
                if (address < ModbusMap::WordCount) regs[address] = value;
                send(id, unitId, QByteArray(1, char(fc)) + pdu.left(4));
            } else if (fc == 16) {
                ++writeRequests;
                const auto address = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(pdu.constData()));
                const auto count = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(pdu.constData() + 2));
                for (int i = 0; i < count; ++i) {
                    const auto value = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(pdu.constData() + 5 + 2 * i));
                    if (address + i < ModbusMap::WordCount) regs[address + i] = value;
                }
                send(id, unitId, QByteArray(1, char(fc)) + pdu.left(4));
            } else {
                QByteArray out;
                out.append(char(fc | 0x80)).append(char(1));
                send(id, unitId, out);
            }
        }
    }

    void send(quint16 id, quint8 unitId, const QByteArray& pdu) {
        QByteArray frame;
        const quint16 bigId = qToBigEndian<quint16>(id);
        frame.append(reinterpret_cast<const char*>(&bigId), 2);
        frame.append('\0').append('\0');
        const quint16 length = qToBigEndian<quint16>(static_cast<quint16>(1 + pdu.size()));
        frame.append(reinterpret_cast<const char*>(&length), 2);
        frame.append(char(unitId));
        frame += pdu;
        // holdStaleReply：暂存一次应答；下一次请求时先补发旧帧，验证客户端按事务 ID 丢弃迟到响应。
        if (holdStaleReply) {
            stale = frame;
            holdStaleReply = false;
            return;
        }
        if (!stale.isEmpty()) {
            client->write(stale);
            stale.clear();
        }
        client->write(frame);
    }

    QTcpServer server;
    QTcpSocket* client = nullptr;
    QByteArray rx, stale;
};

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    SlaveSimulator sim;
    if (!sim.start()) {
        std::cerr << "FAIL: simulator listen\n";
        return 1;
    }
    sim.regs[0] = 0x0001;  // DI33 常亮
    sim.regs[3] = 0x8000;  // DI96 亮
    sim.regs[4] = 1234;    // AI5
    sim.regs[31] = 777;    // AI32

    ModbusClient client;
    bool linkUp = false;
    int lastFc = 0, lastStart = 0;
    qint64 lastRoundTrip = -1;
    QVector<quint16> lastValues;
    bool ackSeen = false;
    int ackFc = 0, ackAddr = -1, ackCount = -1;
    QString failureText;
    int failureCount = 0;
    QObject::connect(&client, &ModbusClient::linkChanged, [&](bool up, const QString&) {
        linkUp = up;
    });
    QObject::connect(&client, &ModbusClient::registersRead, [&](int fc, int start, const QVector<quint16>& values, qint64 rt) {
        lastFc = fc;
        lastStart = start;
        lastValues = values;
        lastRoundTrip = rt;
    });
    QObject::connect(&client, &ModbusClient::writeAcknowledged, [&](int fc, int address, int count) {
        ackSeen = true;
        ackFc = fc;
        ackAddr = address;
        ackCount = count;
    });
    QObject::connect(&client, &ModbusClient::failure, [&](const QString& message) {
        failureText = message;
        ++failureCount;
    });

    client.connectTo("127.0.0.1", sim.port(), 1);
    check(waitFor([&] { return linkUp; }, 3000), "link comes up");
    check(client.isConnected(), "client reports connected");

    // 默认轮询：FC03 从地址 0 读 32 个字。
    check(waitFor([&] { return lastFc == 3 && lastValues.size() == ModbusMap::WordCount; }, 3000), "first poll returns 32 words");
    check(lastStart == 0, "poll starts at address 0");
    check(lastValues.value(0) == 0x0001 && lastValues.value(3) == 0x8000, "DI packing words read back");
    check(lastValues.value(4) == 1234 && lastValues.value(31) == 777, "AI words read back");
    check(lastRoundTrip >= 0, "round trip time recorded");
    check(failureCount == 0, "no failures during polling");

    // FC04 输入寄存器轮询。
    client.setPollRange(4, 2, 5);
    check(waitFor([&] { return lastFc == 4 && lastStart == 2 && lastValues.size() == 5; }, 3000), "FC04 poll range applies");

    // FC06 写单个。
    client.setPollRange(3, 0, ModbusMap::WordCount);
    ackSeen = false;
    client.writeRegister(0, 0xBEEF);
    check(waitFor([&] { return ackSeen && ackFc == 6 && ackAddr == 0 && ackCount == 1; }, 3000), "FC06 write acknowledged");
    check(sim.regs[0] == 0xBEEF, "simulator applied FC06 write");

    // FC16 写多个。
    ackSeen = false;
    client.writeRegisters(4, {11, 22, 33});
    check(waitFor([&] { return ackSeen && ackFc == 16 && ackAddr == 4 && ackCount == 3; }, 3000), "FC16 write acknowledged");
    check(sim.regs[4] == 11 && sim.regs[5] == 22 && sim.regs[6] == 33, "simulator applied FC16 writes");

    // 写入后读回，验证写读一致。
    lastFc = 0;
    waitFor([&] { return lastFc == 3 && lastValues.size() == 32; }, 3000);
    check(lastFc == 3 && lastValues.value(0) == 0xBEEF && lastValues.value(4) == 11 && lastValues.value(5) == 22,
          "written values read back");

    // 从站异常响应（非法地址 → 异常码 2）。
    const int errorsBefore = client.errorCount();
    client.setPollRange(3, 30, 8);
    check(waitFor([&] { return client.errorCount() > errorsBefore; }, 3000), "exception response counted as error");
    check(failureText.contains("非法寄存器地址"), "exception code 2 translated");
    client.setPollRange(3, 0, ModbusMap::WordCount);
    check(waitFor([&] { return lastFc == 3 && lastValues.size() == ModbusMap::WordCount; }, 3000), "polling resumes after exception");

    // 迟到响应：暂存的旧 FC03 应答在新 FC04 请求后补发，应按事务 ID 丢弃且不报错、不覆盖新数据。
    const int failuresBeforeStale = failureCount;
    sim.holdStaleReply = true;
    check(waitFor([&] { return failureCount == failuresBeforeStale + 1; }, 4000), "held reply triggers timeout failure");
    check(failureText.contains("超时"), "timeout is the reported failure");
    client.setPollRange(4, 1, 3);
    check(waitFor([&] { return lastFc == 4 && lastStart == 1 && lastValues.size() == 3; }, 4000),
          "stale frame dropped by transaction id, new poll proceeds");
    check(failureCount == failuresBeforeStale + 1, "stale frame produces no extra failure");
    client.setPollRange(3, 0, ModbusMap::WordCount);
    check(waitFor([&] { return lastFc == 3 && lastValues.size() == ModbusMap::WordCount; }, 4000), "polling recovers after stale reply");

    // 未连接时写入被拒绝。
    const int failuresBeforeOffline = failureCount;
    client.disconnectFrom("测试结束");
    check(waitFor([&] { return !linkUp; }, 1000), "link goes down on manual disconnect");
    client.writeRegister(0, 1);
    check(failureCount == failuresBeforeOffline + 1 && failureText.contains("未连接"), "offline write refused");
    check(client.sentFrames() > 0 && client.receivedFrames() > 0, "traffic counters collected");
    check(client.sentFrames() > client.receivedFrames(), "timed-out requests leave sent above received");

    std::cout << "modbus_test: " << (failures ? "FAILURES" : "all checks passed") << " (" << failures << ")\n";
    return failures ? 1 : 0;
}
