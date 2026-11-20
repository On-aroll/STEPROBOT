#pragma once
#include <QTcpSocket>
#include <functional>
#include <memory>

namespace FileBridge {
// The FTP transfer described in the STEP SDK manual. File operations only.
class DirectFtp {
public:
    DirectFtp(QString host, quint16 port, int timeoutMs, std::function<void()> service);
    void login(const QString& user, const QString& password, const QString& project);
    void upload(const QString& name, const QByteArray& bytes);
    void quit();
private:
    void connected(QTcpSocket& socket, quint16 port);
    void write(QTcpSocket& socket, const QByteArray& data);
    int reply();
    void command(const QByteArray& text, std::initializer_list<int> expected);
    std::unique_ptr<QTcpSocket> passive();
    void check(int code, std::initializer_list<int> expected);
    QString host;
    quint16 port;
    int timeout;
    std::function<void()> service;
    QTcpSocket control;
    QByteArray pending, lastReply;
};
}
