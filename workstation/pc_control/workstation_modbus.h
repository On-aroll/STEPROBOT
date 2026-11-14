#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QVector>

// 新时达 SR175-3200 Modbus TCP 从站寄存器映射（手册 V1.8.0 第四章）：
// Word1-4 打包 DI33-96（每字 16 位，bit0 为 DI33），Word5-32 依次对应 AI5-32。
// 机器人在示教器「参数配置 → 系统配置 → 总线 → ModBus-TCP」启用后监听 502 端口，
// 支持 4X 保持寄存器与 3X 输入寄存器；本类作为主站轮询读取并写入保持寄存器。
namespace ModbusMap {
constexpr int WordCount = 32;
constexpr int BaseAddress = 0;
constexpr int DiBase = 33;
constexpr int DiWords = 4;
constexpr int diNumber(int wordIndex, int bit) { return DiBase + wordIndex * 16 + bit; }
// Word5-32 依次对应 AI5-32：AI 号与 Word 号相同（Word5→AI5，Word32→AI32）。
constexpr int aiNumber(int wordIndex) { return wordIndex + 1; }
inline QString diName(int wordIndex, int bit) { return QStringLiteral("DI%1").arg(diNumber(wordIndex, bit)); }
inline QString aiName(int wordIndex) { return QStringLiteral("AI%1").arg(aiNumber(wordIndex)); }
}

// Modbus TCP 主站：串行事务（同一时刻至多一个在途请求），写请求优先于轮询读。
class ModbusClient : public QObject {
    Q_OBJECT
public:
    explicit ModbusClient(QObject* parent = nullptr);

    void connectTo(const QString& host, quint16 port, quint8 unitId = 1);
    void disconnectFrom(const QString& reason = {});
    bool isConnected() const { return socket.state() == QAbstractSocket::ConnectedState; }
    bool isActive() const { return socket.state() != QAbstractSocket::UnconnectedState; }

    void setPollRange(int functionCode, int startAddress, int count);
    void writeRegister(int address, quint16 value);
    void writeRegisters(int address, const QVector<quint16>& values);

    qint64 sentFrames() const { return sentFramesCounter; }
    qint64 receivedFrames() const { return receivedFramesCounter; }
    qint64 errorCount() const { return errorCounter; }
    qint64 lastRoundTripMs() const { return lastRoundTrip; }

signals:
    void linkChanged(bool connected, const QString& detail);
    void registersRead(int functionCode, int startAddress, const QVector<quint16>& values, qint64 roundTripMs);
    void writeAcknowledged(int functionCode, int address, int count);
    void failure(const QString& message);
    void trafficChanged();

private:
    void transmit(int functionCode, const QByteArray& pdu);
    void pump();
    void ingest();
    void fail(const QString& message);

    QTcpSocket socket;
    QTimer pollTimer, replyTimer;
    QByteArray rx;
    quint16 transaction = 0;
    quint8 unit = 1;
    int inFlightFunction = 0;
    bool writeQueued = false;
    int writeFunction = 0, writeAddress = 0;
    QVector<quint16> writeValues;
    int pollFunction = 3, pollStart = ModbusMap::BaseAddress, pollCount = ModbusMap::WordCount;
    QElapsedTimer roundTrip;
    qint64 sentFramesCounter = 0, receivedFramesCounter = 0, errorCounter = 0, lastRoundTrip = 0;
};
