#pragma once

#include <QJsonObject>
#include <QMap>
#include <QThread>
#include <QVector>
#include <array>

// File channel V1.1 only. This module never writes the robot motion/GDI registers.
namespace FileBridge {
constexpr int Command = 6000, Heartbeat = 6120, StatusAddress = 6200;
constexpr int MaxFile = 1048576, MaxTotal = 5242880, MaxFiles = 32, Chunk = 192;
quint32 crc32(const QByteArray& bytes);
QString validate(const QMap<QString, QByteArray>& files, bool replaceAuto = false);
bool privateEndpoint(const QString& host);
QString newProject();
QVector<quint16> frame(quint16 op, quint32 sequence, quint32 session, quint32 boot,
    quint32 size = 0, quint32 offset = 0, const QByteArray& payload = {},
    quint32 crc = 0, quint16 count = 0, quint16 ordinal = 0, quint16 mode = 1,
    std::array<quint16,3> tag = {});

struct Options {
    QString host;
    quint16 port = 502;
    quint8 unit = 1;
    QMap<QString, QByteArray> files; // Immutable snapshot, including empty SRD members.
    QString project;
    QString receipt;
    bool replaceAuto = false;
    bool directFtp = false; // STEP SDK manual FTP route; PLC still verifies and releases the job.
    QString cabinetHost = "192.168.39.220";
    quint16 cabinetPort = 21;
    QString ftpUser = "root", ftpPassword;
    quint16 programNumber = 11;
    std::array<quint16,3> tag{};
    bool probe = false;
    int ioTimeoutMs = 3000;
    int ackTimeoutMs = 8000;
    int ftpTimeoutMs = 43200000;
    int cleanupTimeoutMs = 15000;
};

class Transfer : public QThread {
    Q_OBJECT
public:
    explicit Transfer(Options options, QObject* parent = nullptr);
signals:
    void progress(const QJsonObject& event);
    void outcome(const QJsonObject& result);
protected:
    void run() override;
private:
    Options options;
};
}
