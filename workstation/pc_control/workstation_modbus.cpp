#include "workstation_modbus.h"

#include <QtEndian>

#include <algorithm>

namespace {
constexpr int HeaderBytes = 7;   // MBAP：事务 2 + 协议 2 + 长度 2 + 单元 1
constexpr int ReplyTimeoutMs = 1500;

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
    connect(&pollTimer, &QTimer::timeout, this, &ModbusClient::pump);
    replyTimer.setSingleShot(true);
    connect(&replyTimer, &QTimer::timeout, this, [this] {
        if (!inFlightFunction) return;
        inFlightFunction = 0;
        fail("从站响应超时。");
    });
    connect(&socket, &QTcpSocket::connected, this, [this] {
        emit linkChanged(true, QString("%1:%2").arg(socket.peerName(), QString::number(socket.peerPort())));
        pollTimer.start();
    });
    connect(&socket, &QTcpSocket::disconnected, this, [this] {
        pollTimer.stop();
        replyTimer.stop();
        inFlightFunction = 0;
        rx.clear();
        emit linkChanged(false, "连接已断开");
    });
    connect(&socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError code) {
        const bool wasConnecting = socket.state() != QAbstractSocket::ConnectedState;
        if (code == QAbstractSocket::RemoteHostClosedError || wasConnecting) {
            pollTimer.stop();
            replyTimer.stop();
            inFlightFunction = 0;
            rx.clear();
            emit linkChanged(false, socket.errorString());
        } else
            fail(socket.errorString());
    });
    connect(&socket, &QTcpSocket::readyRead, this, &ModbusClient::ingest);
}

void ModbusClient::connectTo(const QString& host, quint16 port, quint8 unitId) {
    if (isConnected()) return;
    unit = unitId;
    sentFramesCounter = receivedFramesCounter = errorCounter = lastRoundTrip = 0;
    emit trafficChanged();
    socket.connectToHost(host, port);
}

void ModbusClient::disconnectFrom(const QString& reason) {
    pollTimer.stop();
    replyTimer.stop();
    inFlightFunction = 0;
    writeQueued = false;
    writeValues.clear();
    rx.clear();
    if (socket.state() != QAbstractSocket::UnconnectedState) socket.abort();
    emit linkChanged(false, reason.isEmpty() ? "已手动断开" : reason);
}

void ModbusClient::setPollRange(int functionCode, int startAddress, int count) {
    pollFunction = functionCode == 4 ? 4 : 3;
    pollStart = std::clamp(startAddress, 0, 65535);
    pollCount = std::clamp(count, 1, 125);
}

void ModbusClient::writeRegister(int address, quint16 value) {
    if (!isConnected()) { emit failure("未连接从站，写入未发送。"); return; }
    writeFunction = 6;
    writeAddress = std::clamp(address, 0, 65535);
    writeValues = {value};
    writeQueued = true;
    pump();
}

void ModbusClient::writeRegisters(int address, const QVector<quint16>& values) {
    if (!isConnected()) { emit failure("未连接从站，写入未发送。"); return; }
    if (values.isEmpty() || values.size() > 123) { emit failure("批量写入需要 1–123 个寄存器。"); return; }
    writeFunction = 16;
    writeAddress = std::clamp(address, 0, 65535);
    writeValues = values;
    writeQueued = true;
    pump();
}

void ModbusClient::transmit(int functionCode, const QByteArray& pdu) {
    const quint16 id = ++transaction;
    QByteArray frame;
    const quint16 bigId = qToBigEndian<quint16>(id);
    frame.append(reinterpret_cast<const char*>(&bigId), 2);
    frame.append('\0').append('\0');
    const quint16 length = qToBigEndian<quint16>(static_cast<quint16>(1 + pdu.size()));
    frame.append(reinterpret_cast<const char*>(&length), 2);
    frame.append(char(unit));
    frame.append(pdu);
    if (socket.write(frame) != frame.size()) { fail("请求写入套接字失败。"); return; }
    ++sentFramesCounter;
    inFlightFunction = functionCode;
    roundTrip.restart();
    replyTimer.start(ReplyTimeoutMs);
    emit trafficChanged();
}

void ModbusClient::pump() {
    if (!isConnected() || inFlightFunction) return;
    if (writeQueued) {
        writeQueued = false;
        if (writeFunction == 6) {
            QByteArray pdu;
            pdu.append(char(6));
            const quint16 address = qToBigEndian<quint16>(static_cast<quint16>(writeAddress));
            pdu.append(reinterpret_cast<const char*>(&address), 2);
            const quint16 value = qToBigEndian<quint16>(writeValues.first());
            pdu.append(reinterpret_cast<const char*>(&value), 2);
            transmit(6, pdu);
        } else {
            QByteArray pdu;
            pdu.append(char(16));
            const quint16 address = qToBigEndian<quint16>(static_cast<quint16>(writeAddress));
            pdu.append(reinterpret_cast<const char*>(&address), 2);
            const quint16 count = qToBigEndian<quint16>(static_cast<quint16>(writeValues.size()));
            pdu.append(reinterpret_cast<const char*>(&count), 2);
            pdu.append(char(static_cast<quint8>(writeValues.size() * 2)));
            for (const auto value : writeValues) {
                const quint16 big = qToBigEndian<quint16>(value);
                pdu.append(reinterpret_cast<const char*>(&big), 2);
            }
            transmit(16, pdu);
        }
        writeValues.clear();
        return;
    }
    QByteArray pdu;
    pdu.append(char(pollFunction));
    const quint16 address = qToBigEndian<quint16>(static_cast<quint16>(pollStart));
    pdu.append(reinterpret_cast<const char*>(&address), 2);
    const quint16 count = qToBigEndian<quint16>(static_cast<quint16>(pollCount));
    pdu.append(reinterpret_cast<const char*>(&count), 2);
    transmit(pollFunction, pdu);
}

void ModbusClient::ingest() {
    rx.append(socket.readAll());
    while (true) {
        if (rx.size() < HeaderBytes) return;
        const auto length = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(rx.constData() + 4));
        const int total = HeaderBytes + length - 1;
        if (rx.size() < total) return;
        const quint16 id = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(rx.constData()));
        QByteArray pdu = rx.mid(HeaderBytes, length - 1);
        rx.remove(0, total);
        if (!inFlightFunction || id != transaction) continue;   // 迟到或陈旧的响应直接丢弃
        replyTimer.stop();
        inFlightFunction = 0;
        ++receivedFramesCounter;
        lastRoundTrip = roundTrip.elapsed();
        if (pdu.isEmpty()) { fail("响应缺少功能码。"); continue; }
        const quint8 function = static_cast<quint8>(pdu.front());
        pdu.remove(0, 1);
        if (function & 0x80) {
            const quint8 code = pdu.isEmpty() ? 0 : static_cast<quint8>(pdu.front());
            fail(QString("从站返回异常：功能码 %1，%2。").arg(function & 0x7f).arg(exceptionText(code)));
            continue;
        }
        if ((function == 3 || function == 4) && !pdu.isEmpty()) {
            const int bytes = static_cast<quint8>(pdu.front());
            pdu.remove(0, 1);
            if (bytes % 2 || bytes / 2 > pollCount || bytes != pdu.size()) { fail("读响应字节数与声明不一致。"); continue; }
            QVector<quint16> values;
            values.reserve(bytes / 2);
            for (int i = 0; i + 1 < pdu.size(); i += 2)
                values << qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(pdu.constData() + i));
            emit registersRead(function, pollStart, values, lastRoundTrip);
        } else if (function == 6 && pdu.size() == 4)
            emit writeAcknowledged(6, qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(pdu.constData())), 1);
        else if (function == 16 && pdu.size() == 4)
            emit writeAcknowledged(16, qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(pdu.constData())),
                                   qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(pdu.constData() + 2)));
        else
            fail(QString("无法解析的功能码或响应长度：%1").arg(function));
        emit trafficChanged();
    }
}

void ModbusClient::fail(const QString& message) {
    ++errorCounter;
    lastRoundTrip = 0;
    emit trafficChanged();
    emit failure(message);
}
