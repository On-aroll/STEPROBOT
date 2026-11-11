#pragma once

#include "workstation_job.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QTimer>

#include <functional>

class Session : public QObject {
public:
    Session(QString workspace, QString client, QString studioRoot, QString simulationSeed);
    ~Session() override;

    void prepare(const Job& job);
    void connectReal(QString ip, QString serial = {});
    void disconnect();
    bool request(QString command);
    void stop();

    bool connected() const;
    bool fresh() const;
    bool canClose() const;
    bool active() const { return preparing || connected(); }
    QString value(const QString& key) const { return feedback.value(key); }

    std::function<void(QString)> note;
    std::function<void()> changed;
    std::function<void()> sampled;
    std::function<void(bool)> ended;
    bool preparing = false;
    bool simulated = false;
    bool control = false;
    bool waitingStop = false;
    bool lost = false;
    QString pending;
    QString model;
    QString address;
    QString identity;
    QString logFolder;

private:
    void waitForVirtualPorts(const Job& job, qint64 deadline);
    void attach(QString mode, QStringList parameters);
    void receive();
    void handle(const QString& line);
    void poll();
    void fail(QString why);
    void cleanVirtual();
    bool send(const QString& command);

    QString workspace;
    QString client;
    QString studioRoot, simulationSeed;
    QProcess kernel;
    QProcess motion;
    QProcess bootstrap;
    QProcess worker;
    QTimer timer;
    QElapsedTimer clock;
    QByteArray incoming;
    QMap<QString, QString> feedback;
    qint64 lastStatus = -1;
    qint64 queuedAt = 0;
    bool outstanding = false;
    bool commandSent = false;
    bool ownedRun = false;
    bool timeoutStop = false;
    bool closing = false;
};
