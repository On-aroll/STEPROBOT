#include "workstation_modbus.h"
#include <QNetworkProxy>
#include <QtEndian>
#include <algorithm>

namespace {
quint16 word(const QByteArray& b, int at) {
    return qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(b.constData() + at));
}
void appendWord(QByteArray& b, quint16 v) {
    const auto big = qToBigEndian<quint16>(v);
    b.append(reinterpret_cast<const char*>(&big), 2);
}
QString exceptionText(quint8 code) {
    switch (code) {
    case 1: return "非法功能码";
    case 2: return "非法寄存器地址";
    case 3: return "寄存器数量或数值超出范围";
    case 4: return "从站执行出错";
    case 6: return "从站忙，请求被拒绝";
    case 10: return "网关不可用";
    case 11: return "网关目标无响应";
    default: return QString("异常码 %1").arg(code);
    }
}
}
ModbusClient::ModbusClient(QObject* parent) : QObject(parent) {
    pollTimer.setInterval(250);
    connect(&pollTimer, &QTimer::timeout, this, [this] { pollDue = true; pump(); });
    replyTimer.setSingleShot(true);
    connect(&replyTimer, &QTimer::timeout, this, [this] {
        if (!inFlightFunction) return;
        inFlightFunction = 0; writes.clear();
        fail("从站响应超时；写命令结果可能未知，未自动重发。");
    });
    connect(&socket, &QTcpSocket::connected, this, [this] {
        pollDue = true;
        emit linkChanged(true, QString("%1:%2").arg(socket.peerName()).arg(socket.peerPort()));
        pollTimer.start(); pump();
    });
    connect(&socket, &QTcpSocket::disconnected, this, [this] {
        pollTimer.stop(); replyTimer.stop(); inFlightFunction = 0;
        writes.clear(); rx.clear(); emit linkChanged(false, "连接已断开");
    });
    connect(&socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError code) {
        if (code == QAbstractSocket::RemoteHostClosedError || !isConnected()) {
            pollTimer.stop(); replyTimer.stop(); inFlightFunction = 0;
            writes.clear(); rx.clear(); emit linkChanged(false, socket.errorString());
        } else fail(socket.errorString());
    });
    connect(&socket, &QTcpSocket::readyRead, this, &ModbusClient::ingest);
}
ModbusClient::~ModbusClient() {
    // Members are destroyed before QObject disconnects its receiver connections.
    // QTcpSocket::abort/destruction may emit disconnected: never access dead queues/timers.
    pollTimer.stop(); replyTimer.stop();
    QObject::disconnect(&socket, nullptr, this, nullptr);
    socket.abort();
}
void ModbusClient::connectTo(const QString& host, quint16 port, quint8 unitId) {
    if (isActive()) return;
    unit = unitId; rx.clear(); writes.clear(); inFlightFunction = 0; pollDue = true;
    sentFramesCounter = receivedFramesCounter = errorCounter = lastRoundTrip = 0;
    // Raw industrial TCP uses a direct route, regardless of HTTP/system proxy settings.
    socket.setProxy(QNetworkProxy::NoProxy);
    emit trafficChanged(); socket.connectToHost(host, port);
}
void ModbusClient::disconnectFrom(const QString& reason) {
    pollTimer.stop(); replyTimer.stop(); inFlightFunction = 0;
    writes.clear(); rx.clear();
    if (isActive()) socket.abort();
    emit linkChanged(false, reason.isEmpty() ? "已手动断开" : reason);
}
void ModbusClient::setPollRange(int functionCode, int startAddress, int count) {
    pollFunction = functionCode == 4 ? 4 : 3;
    pollStart = std::clamp(startAddress, 0, 65535);
    pollCount = std::clamp(count, 1, std::min(125, 65536 - pollStart));
}
void ModbusClient::setPollInterval(int milliseconds) { pollTimer.setInterval(std::clamp(milliseconds, 25, 1000)); }
void ModbusClient::writePriorityRegisters(int address, const QVector<quint16>& values, int discardStart, int discardCount) {
    if (!isConnected()) { emit failure("未连接从站，优先停止未发送。"); return; }
    if (values.isEmpty() || values.size() > 123 || address < 0 || address + values.size() > 65536) {
        fail("优先写入地址或数量无效。"); return;
    }
    QQueue<WriteRequest> kept;
    while (!writes.isEmpty()) {
        auto w = writes.dequeue();
        if (w.address + w.values.size() <= discardStart || w.address >= discardStart + discardCount) kept.enqueue(w);
    }
    writes = kept;
    writes.prepend({16, address, values});
    pump(); // A transmitted transaction cannot be recalled; STOP is next, with no old queued movement after it.
}
void ModbusClient::writeRegister(int address, quint16 value) {
    if (!isConnected()) { emit failure("未连接从站，写入未发送。"); return; }
    if (address < 0 || address > 65535 || writes.size() >= 64) { fail("写入地址无效或发送队列已满。"); return; }
    writes.enqueue({6, address, {value}}); pump();
}
void ModbusClient::writeRegisters(int address, const QVector<quint16>& values) {
    if (!isConnected()) { emit failure("未连接从站，写入未发送。"); return; }
    if (values.isEmpty() || values.size() > 123 || address < 0 || address + values.size() > 65536 || writes.size() >= 64) {
        fail("批量写入地址/数量无效或发送队列已满。"); return;
    }
    writes.enqueue({16, address, values}); pump();
}
void ModbusClient::transmit(int functionCode, const QByteArray& pdu) {
    QByteArray frame;
    appendWord(frame, ++transaction); appendWord(frame, 0);
    appendWord(frame, quint16(1 + pdu.size())); frame.append(char(unit)); frame += pdu;
    inFlightFunction = functionCode;
    if (socket.write(frame) != frame.size()) { inFlightFunction = 0; writes.clear(); fail("请求写入套接字失败。"); return; }
    ++sentFramesCounter; roundTrip.restart(); replyTimer.start(1500); emit trafficChanged();
}
void ModbusClient::pump() {
    if (!isConnected() || inFlightFunction) return;
    QByteArray pdu;
    if (!writes.isEmpty()) {
        const auto request = writes.dequeue();
        activeAddress = request.address; activeCount = int(request.values.size()); activeValue = request.values.first();
        pdu.append(char(request.function)); appendWord(pdu, quint16(activeAddress));
        if (request.function == 6) appendWord(pdu, activeValue);
        else {
            appendWord(pdu, quint16(activeCount)); pdu.append(char(activeCount * 2));
            for (auto value : request.values) appendWord(pdu, value);
        }
        transmit(request.function, pdu);
    } else if (pollDue) {
        pollDue = false; activeAddress = pollStart; activeCount = pollCount;
        pdu.append(char(pollFunction)); appendWord(pdu, quint16(activeAddress)); appendWord(pdu, quint16(activeCount));
        transmit(pollFunction, pdu);
    }
}
void ModbusClient::ingest() {
    rx += socket.readAll();
    while (rx.size() >= 7) {
        const int length = word(rx, 4);
        if (word(rx, 2) != 0 || length < 2 || length > 254) {
            fail("Modbus MBAP 协议标识或长度无效。"); disconnectFrom("响应报文无效"); return;
        }
        const int total = 6 + length;
        if (rx.size() < total) break;
        const auto id = word(rx, 0); const auto replyUnit = quint8(rx[6]);
        const auto pdu = rx.mid(7, length - 1); rx.remove(0, total);
        if (!inFlightFunction || id != transaction) continue;
        const int expectedFc = inFlightFunction, address = activeAddress, count = activeCount;
        replyTimer.stop(); inFlightFunction = 0; ++receivedFramesCounter; lastRoundTrip = roundTrip.elapsed();
        const auto fc = quint8(pdu[0]);
        if (replyUnit != unit || (fc != expectedFc && fc != (expectedFc | 0x80))) {
            fail("响应单元号或功能码与请求不一致。");
        } else if (fc & 0x80) {
            if (pdu.size() != 2) fail("异常响应长度无效。");
            else fail(QString("从站返回异常：功能码 %1，%2。").arg(expectedFc).arg(exceptionText(quint8(pdu[1]))));
        } else if (fc == 3 || fc == 4) {
            if (pdu.size() != 2 + count * 2 || quint8(pdu[1]) != count * 2) fail("读响应字节数与请求不一致。");
            else {
                QVector<quint16> values; values.reserve(count);
                for (int i = 0; i < count; ++i) values << word(pdu, 2 + 2 * i);
                emit registersRead(fc, address, values, lastRoundTrip);
            }
        } else if (pdu.size() != 5 || word(pdu, 1) != address || word(pdu, 3) != (fc == 6 ? activeValue : count)) {
            fail("写响应的地址、数量或回显值与请求不一致。");
        } else emit writeAcknowledged(fc, address, count);
        emit trafficChanged();
        if (!isConnected()) return;
    }
    pump();
}
void ModbusClient::fail(const QString& message) {
    ++errorCounter; lastRoundTrip = 0; emit trafficChanged(); emit failure(message);
}
