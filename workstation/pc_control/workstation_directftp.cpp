#include "workstation_directftp.h"
#include <QElapsedTimer>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <algorithm>
#include <stdexcept>

namespace FileBridge {
namespace {
[[noreturn]] void error(const QString& text) { throw std::runtime_error(text.toUtf8().constData()); }
bool credential(const QString& text) {
    if (text.isEmpty() || text.size()>64) return false;
    return std::all_of(text.begin(),text.end(),[](QChar c){return c.unicode()>=32 && c.unicode()<=126;});
}
}
DirectFtp::DirectFtp(QString h,quint16 p,int t,std::function<void()> s)
    :host(std::move(h)),port(p),timeout(t),service(std::move(s)) {}
void DirectFtp::connected(QTcpSocket& socket,quint16 p) {
    service();socket.setProxy(QNetworkProxy::NoProxy);socket.connectToHost(host,p);
    // waitForConnected aborts its connection attempt when its deadline expires;
    // use the I/O deadline once, rather than restarting/aborting it every 20 ms.
    if(!socket.waitForConnected(timeout))error("控制柜FTP连接失败："+socket.errorString());
    service();
    socket.setSocketOption(QAbstractSocket::LowDelayOption,1);
}
void DirectFtp::write(QTcpSocket& socket,const QByteArray& data) {
    QElapsedTimer timer;timer.start();qint64 offset=0;
    while(offset<data.size() || socket.bytesToWrite()) {
        service();
        if(socket.state()!=QAbstractSocket::ConnectedState)error("控制柜FTP数据连接中断");
        if(offset<data.size() && socket.bytesToWrite()<65536) {
            const auto n=socket.write(data.constData()+offset,std::min<qint64>(65536,data.size()-offset));
            if(n<=0)error("控制柜FTP发送未取得进展");
            offset+=n;
        }
        if(socket.bytesToWrite()) {
            if(socket.waitForBytesWritten(20))timer.restart();
            else if(timer.elapsed()>=timeout)error("控制柜FTP发送无进展，已停止");
        }
    }
}
int DirectFtp::reply() {
    QElapsedTimer timer;timer.start();QByteArray multiline;int lines=0;
    while(true) {
        service();
        const auto newline=pending.indexOf('\n');
        if(newline>=0) {
            auto line=pending.left(newline);pending.remove(0,newline+1);
            if(line.endsWith('\r'))line.chop(1);
            if(line.size()>255 || ++lines>256)error("控制柜FTP回复过长");
            if(!multiline.isEmpty() && !line.startsWith(multiline+' '))continue;
            if(line.size()<4 || line[0]<'0' || line[0]>'9' || line[1]<'0' || line[1]>'9' || line[2]<'0' || line[2]>'9')
                error("控制柜FTP回复格式错误");
            if(line[3]=='-'){multiline=line.left(3);continue;}
            if(line[3]!=' ')error("控制柜FTP回复格式错误");
            lastReply=line;return line.left(3).toInt();
        }
        if(pending.size()>255)error("控制柜FTP回复过长");
        if(control.bytesAvailable() || control.waitForReadyRead(20))pending+=control.readAll();
        else if(control.state()!=QAbstractSocket::ConnectedState || timer.elapsed()>=timeout)
            error("控制柜FTP回复中断或超时");
    }
}
void DirectFtp::check(int code,std::initializer_list<int> expected) {
    if(std::find(expected.begin(),expected.end(),code)==expected.end())
        error(QString("控制柜FTP拒绝操作，回复码%1").arg(code));
}
void DirectFtp::command(const QByteArray& text,std::initializer_list<int> expected) {
    write(control,text+"\r\n");check(reply(),expected);
}
void DirectFtp::login(const QString& user,const QString& password,const QString& project) {
    if(!credential(user)||!credential(password))error("FTP用户名和密码须为1～64个可打印ASCII字符");
    if(!QRegularExpression("\\Apcjob_[a-z0-9_]{3,39}\\.sr\\z").match(project).hasMatch())error("FTP目标工程名无效");
    connected(control,port);check(reply(),{220});
    write(control,"USER "+user.toLatin1()+"\r\n");const auto code=reply();check(code,{230,331});
    if(code==331)command("PASS "+password.toLatin1(),{230});
    command("CWD /UserPrograms",{250});
    command("CWD "+project.toLatin1(),{250}); // Existing fixed mapping only; never MKD.
    command("TYPE I",{200});
}
std::unique_ptr<QTcpSocket> DirectFtp::passive() {
    command("PASV",{227});
    const auto match=QRegularExpression("\\((\\d{1,3}),(\\d{1,3}),(\\d{1,3}),(\\d{1,3}),(\\d{1,3}),(\\d{1,3})\\)")
        .match(QString::fromLatin1(lastReply));
    if(!match.hasMatch())error("控制柜FTP PASV回复无效");
    for(int i=1;i<=6;++i)if(match.captured(i).toInt()>255)error("控制柜FTP PASV端口无效");
    const auto p=match.captured(5).toInt()*256+match.captured(6).toInt();
    if(!p || p==21)error("控制柜FTP PASV端口无效");
    auto socket=std::make_unique<QTcpSocket>();connected(*socket,quint16(p));return socket;
}
void DirectFtp::upload(const QString& name,const QByteArray& bytes) {
    if(!QRegularExpression("\\Aauto_(?:main|0[1-9]|1[0-5])\\.sr[pd]\\z").match(name).hasMatch())error("FTP文件不在自动作业白名单");
    auto socket=passive();
    command("STOR "+name.toLatin1(),{125,150});
    write(*socket,bytes);socket->disconnectFromHost();
    if(socket->state()!=QAbstractSocket::UnconnectedState) {
        QElapsedTimer timer;timer.start();
        while(socket->state()!=QAbstractSocket::UnconnectedState) {
            service();socket->waitForDisconnected(20);
            if(timer.elapsed()>=timeout)error("控制柜FTP数据连接关闭未确认");
        }
    }
    check(reply(),{226,250});
}
void DirectFtp::quit() {
    command("QUIT",{221});control.disconnectFromHost();
    if(control.state()!=QAbstractSocket::UnconnectedState) {
        QElapsedTimer timer;timer.start();
        while(control.state()!=QAbstractSocket::UnconnectedState) {
            service();control.waitForDisconnected(20);
            if(timer.elapsed()>=timeout)error("控制柜FTP控制连接关闭未确认");
        }
    }
}
}
