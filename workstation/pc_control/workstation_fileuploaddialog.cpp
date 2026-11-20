#include "workstation_settings.h"
#include "workstation_fileuploaddialog.h"
#include <QtWidgets>
#include <QDesktopServices>
#include <QSettings>

namespace {
QString defaultFtpPassword() {
    const auto environment = qEnvironmentVariable("STEP_FTP_PASSWORD");
    if (!environment.isEmpty()) return environment;
    QSettings local(QDir(QCoreApplication::applicationDirPath()).filePath("workstation.local.ini"), QSettings::IniFormat);
    return local.value("ftp/password").toString();
}
QIcon passwordEye(bool visible) {
    QPixmap pixmap(22,22); pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor("#466482"),1.7,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
    QPainterPath outline; outline.moveTo(2,11); outline.cubicTo(7,4,15,4,20,11);
    outline.cubicTo(15,18,7,18,2,11); painter.drawPath(outline);
    painter.drawEllipse(QPointF(11,11),2.5,2.5);
    if (visible) painter.drawLine(QPointF(3,3),QPointF(19,19));
    return QIcon(pixmap);
}
}

FileUploadDialog::FileUploadDialog(const QMap<QString,QByteArray>& files, const QString& source,
    const QString& endpoint, const QString& receipts, QWidget* parent)
    : QDialog(parent), receiptDirectory(receipts) {
    setObjectName("fileUploadDialog");
    setWindowTitle("完整 STEP 工程上传 · PLC → 控制柜"); resize(790,620);
    options.files = files; options.project = FileBridge::newProject();
    options.receipt = QDir(receiptDirectory).filePath("bridge_receipt_" + options.project.chopped(3) + ".json");
    validation = FileBridge::validate(files);
    auto* root = new QVBoxLayout(this);
    auto* title = new QLabel("电脑 → Modbus TCP → PLC → FTP → 控制柜");
    title->setObjectName("fileUploadRoute");
    auto font = title->font(); font.setBold(true); title->setFont(font); root->addWidget(title);
    auto* note = new QLabel("上传当前工程的完整副本，保留空 SRD。完成后到示教器核对并加载；GDI 编号需另行绑定。");
    note->setObjectName("fileUploadNote");
    note->setWordWrap(true); root->addWidget(note);
    auto* form = new QFormLayout;
    address = new QLineEdit(endpoint); address->setObjectName("fileUploadEndpoint");
    address->setPlaceholderText("PLC 电脑侧 IP:端口，例如 192.168.1.188:502");
    addressLabel = new QLabel("PLC 电脑侧地址"); addressLabel->setObjectName("fileUploadEndpointLabel");
    form->addRow(addressLabel,address);
    auto* from = new QLabel(source); from->setTextFormat(Qt::PlainText); from->setWordWrap(true);
    form->addRow("当前工程",from);
    destination = new QLabel("/UserPrograms/" + options.project);
    destination->setTextInteractionFlags(Qt::TextSelectableByMouse); destination->setObjectName("fileUploadDestination");
    form->addRow("目标目录",destination); root->addLayout(form);
    directFields=new QWidget;auto* directForm=new QFormLayout(directFields);directForm->setContentsMargins(0,0,0,0);
    const auto settings = workstationSettings();
    const auto cabinet = settings.value("cabinet/address", "192.168.39.220:30312").toString().section(':',0,0);
    cabinetAddress=new QLineEdit(settings.value("fileBridge/cabinetAddress",cabinet+":21").toString());cabinetAddress->setObjectName("fileUploadCabinet");
    ftpUser=new QLineEdit(settings.value("fileBridge/ftpUser","root").toString());ftpUser->setObjectName("fileUploadFtpUser");
    ftpPassword=new QLineEdit(defaultFtpPassword());ftpPassword->setEchoMode(QLineEdit::Password);ftpPassword->setObjectName("fileUploadFtpPassword");
    passwordVisibility=ftpPassword->addAction(passwordEye(false),QLineEdit::TrailingPosition);
    passwordVisibility->setObjectName("fileUploadTogglePassword");passwordVisibility->setCheckable(true);
    passwordVisibility->setText("显示密码");passwordVisibility->setToolTip("显示密码");
    connect(passwordVisibility,&QAction::toggled,this,[this](bool visible){
        ftpPassword->setEchoMode(visible?QLineEdit::Normal:QLineEdit::Password);
        passwordVisibility->setIcon(passwordEye(visible));
        passwordVisibility->setText(visible?"隐藏密码":"显示密码");
        passwordVisibility->setToolTip(passwordVisibility->text());
    });
    directForm->addRow("控制柜 FTP 地址",cabinetAddress);directForm->addRow("FTP 用户名",ftpUser);directForm->addRow("FTP 密码",ftpPassword);
    root->insertWidget(2,directFields);directFields->hide();
    table = new QTableWidget(files.size(),3); table->setObjectName("fileUploadFiles");
    table->setHorizontalHeaderLabels({"文件（当前副本）","字节数","传输状态"});
    table->verticalHeader()->hide(); table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2,QHeaderView::ResizeToContents);
    qint64 bytes = 0; int row = 0;
    for (auto it = files.cbegin(); it != files.cend(); ++it,++row) {
        bytes += it.value().size();
        table->setItem(row,0,new QTableWidgetItem(it.key()));
        table->setItem(row,1,new QTableWidgetItem(QString::number(it.value().size())));
        table->item(row,1)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table->setItem(row,2,new QTableWidgetItem(it.value().isEmpty() ? "待传（空文件）" : "待传"));
    }
    root->addWidget(table,1);
    hint = new QLabel(validation.isEmpty() ? QString("本地预检通过：%1 个文件，共 %2 字节。每次上传创建新目录。").arg(files.size()).arg(bytes) : "预检未通过：" + validation);
    hint->setWordWrap(true); root->addWidget(hint);
    bar = new QProgressBar; bar->setRange(0,100); bar->setValue(0); bar->setTextVisible(false); root->addWidget(bar);
    status = new QLabel("尚未连接。可先做只读检查，或直接开始上传并由工具重新检查。");
    status->setObjectName("fileUploadStatus"); status->setWordWrap(true); root->addWidget(status);
    receiptLabel = new QLabel(options.receipt); receiptLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    receiptLabel->setWordWrap(true); root->addWidget(receiptLabel);
    auto* buttons = new QHBoxLayout;
    probeButton = new QPushButton("只读检查 PLC"); probeButton->setObjectName("fileUploadProbe");
    uploadButton = new QPushButton("开始上传"); uploadButton->setObjectName("fileUploadStart"); uploadButton->setProperty("primary",true);
    receiptButton = new QPushButton("打开回执文件夹"); receiptButton->setEnabled(false);
    closeButton = new QPushButton("关闭"); closeButton->setObjectName("fileUploadClose");
    buttons->addWidget(probeButton); buttons->addWidget(uploadButton); buttons->addStretch();
    buttons->addWidget(receiptButton); buttons->addWidget(closeButton); root->addLayout(buttons);
    connect(probeButton,&QPushButton::clicked,this,[this] { start(true); });
    connect(uploadButton,&QPushButton::clicked,this,[this] { start(false); });
    connect(closeButton,&QPushButton::clicked,this,&FileUploadDialog::reject);
    connect(receiptButton,&QPushButton::clicked,this,[this] { QDesktopServices::openUrl(QUrl::fromLocalFile(receiptDirectory)); });
    updateControls();
}

void FileUploadDialog::useAutoTarget(const QString& project,quint16 number,std::array<quint16,3> tag) {
    options.project=project;options.programNumber=number;options.replaceAuto=true;options.tag=tag;
    options.receipt=QDir(receiptDirectory).filePath("auto_receipt_"+QUuid::createUuid().toString(QUuid::Id128)+".json");
    validation=FileBridge::validate(options.files,true);
    setWindowTitle("固定作业更新 · PLC → 控制柜");
    if(auto* note=findChild<QLabel*>("fileUploadNote"))note->setText("更新已绑定的固定作业入口及本次轨迹，复用程序号；完成后重新加载本次版本。");
    destination->setText("/UserPrograms/"+project+QString(" · 作业号%1").arg(number));
    hint->setText(validation.isEmpty()?"仅更新auto_main及本次轨迹；不写_project.srd或manual_service。":validation);
    receiptLabel->setText(options.receipt);updateControls();
}

void FileUploadDialog::setDirectMode(bool direct, const QString& cabinetEndpoint) {
    if(worker || attempted || !options.replaceAuto)return;
    options.directFtp=direct;directFields->setVisible(direct);
    addressLabel->setText(direct ? "PLC 许可 / 回读地址" : "PLC 电脑侧地址");
    probeButton->setText(direct ? "检查 PLC 许可（只读）" : "只读检查 PLC");
    if (direct && !cabinetEndpoint.isEmpty()) {
        const auto host = cabinetEndpoint.section(':',0,0).trimmed();
        if (cabinetAddress->text().section(':',0,0) != host) cabinetAddress->setText(host + ":21");
    }
    if(direct) {
        setWindowTitle("电脑直传 · 上位机 → 控制柜");
        if(auto* title=findChild<QLabel*>("fileUploadRoute"))title->setText("文件传输：上位机 → FTP → 控制柜");
        if(auto* note=findChild<QLabel*>("fileUploadNote"))note->setText("点「开始上传」自动连接控制柜，无需先连接顶部 SDK。PLC另行负责上传许可、回读校验及11号版本确认；完成后仍需重新加载11号。");
        status->setText("尚未连接。填写控制柜 FTP 地址和账号后开始上传；PLC 连接用于许可和回读确认。");
    } else {
        setWindowTitle("固定作业更新 · PLC → 控制柜");
        if(auto* title=findChild<QLabel*>("fileUploadRoute"))title->setText("文件传输：上位机 → Modbus TCP → PLC → FTP → 控制柜");
        if(auto* note=findChild<QLabel*>("fileUploadNote"))note->setText("更新已绑定的固定作业入口及本次轨迹，复用11号；PLC上传并回读通过后重新加载本次版本。");
    }
}

void FileUploadDialog::updateControls() {
    const bool busy = worker != nullptr;
    address->setReadOnly(busy || attempted);
    for(auto* field:{cabinetAddress,ftpUser,ftpPassword})field->setReadOnly(busy || attempted);
    probeButton->setEnabled(!busy && !attempted);
    uploadButton->setEnabled(!busy && !attempted && validation.isEmpty());
    closeButton->setText(busy ? cancelling ? "正在取消…" : "取消并清理" : "关闭");
    closeButton->setEnabled(!cancelling);
}

void FileUploadDialog::start(bool probe) {
    if (worker || attempted || (!probe && !validation.isEmpty())) return;
    const auto parts = address->text().trimmed().split(':');
    bool portOk = parts.size() == 1;
    const int port = parts.size() == 1 ? 502 : parts.value(1).toInt(&portOk);
    if (parts.isEmpty() || parts.size() > 2 || !FileBridge::privateEndpoint(parts[0]) || !portOk || port < 1 || port > 65535) {
        status->setText("请填写 PLC 电脑侧私有 IPv4 地址和端口，例如 192.168.1.188:502。"); return;
    }
    if (!probe && !QDir().mkpath(receiptDirectory)) { status->setText("无法创建本地回执目录，未连接PLC。"); return; }
    options.host = parts[0]; options.port = quint16(port); options.probe = probe;
    if(options.directFtp && !probe) {
        const auto cabinet=cabinetAddress->text().trimmed().split(':');bool cabinetPortOk=cabinet.size()==1;
        const auto cabinetPort=cabinet.size()==1?21:cabinet.value(1).toInt(&cabinetPortOk);
        if(cabinet.size()>2 || !FileBridge::privateEndpoint(cabinet.value(0)) || !cabinetPortOk || cabinetPort<1 || cabinetPort>65535 || ftpUser->text().isEmpty() || ftpPassword->text().isEmpty()) {
            status->setText("请填写控制柜私有IPv4地址、FTP端口、用户名和密码。");return;
        }
        options.cabinetHost=cabinet[0];options.cabinetPort=quint16(cabinetPort);options.ftpUser=ftpUser->text();options.ftpPassword=ftpPassword->text();
    }
    if (!probe) attempted = true;
    auto settings = workstationSettings();
    settings.setValue("fileBridge/address",address->text().trimmed());
    if (options.directFtp && !probe) {
        settings.setValue("fileBridge/cabinetAddress",cabinetAddress->text().trimmed());
        settings.setValue("fileBridge/ftpUser",ftpUser->text());
    }
    worker = new FileBridge::Transfer(options,this);
    connect(worker,&FileBridge::Transfer::progress,this,&FileUploadDialog::updateProgress);
    connect(worker,&FileBridge::Transfer::outcome,this,[this](const QJsonObject& value) {
        lastResult = value;
        const auto state = value.value("status").toString();
        if (state == "PROBED") {
            const auto s = value.value("plc_status").toObject();
            if(options.replaceAuto)status->setText(QString("只读检查：固定入口配置%1，上传空闲条件%2；手动号%3 / 自动号%4，PLC启动计数%5。\n%6")
                .arg(s.value("auto_configured").toInt()==1?"有效":"未生效（HR6249=0）")
                .arg(s.value("auto_idle").toInt()==1?"满足":"未满足（HR6259=0）")
                .arg(s.value("manual_number").toInt()).arg(s.value("auto_number").toInt()).arg(s.value("boot").toInt())
                .arg(s.value("auto_configured").toInt()==1?"上传时仍会重新校验工程、编号和状态。":
                    "请在InoProShop核对 xFixedMappingConfirmed=TRUE、uiFixedJobGuardVersion=140，以及固定工程和编号配置。"));
            else status->setText(QString("只读检查：state=%1，flags=%2，空文件能力=%3，实现标记=%4。上传时仍会重新校验许可和状态。")
                .arg(s.value("state").toInt()).arg(s.value("flags").toInt()).arg(s.value("capabilities").toInt() & 1).arg(s.value("revision").toInt()));
        } else if (state == "COMPLETE") {
            bar->setValue(100); status->setText(options.replaceAuto ?
                QString("上传、回读通过；固定作业号%1可用。接下来重新加载并确认本次版本。").arg(options.programNumber):
                "整组上传成功，回读一致、连接已关闭。请到示教器核对并加载新工程。未加载、启动或更改GDI绑定。");
            status->setText(status->text()+QString("\n本次总耗时 %1 秒；分阶段耗时已保存到回执。").arg(value.value("total_ms").toDouble()/1000.,0,'f',3));
        } else if (state == "CANCELLED") {
            status->setText("已取消：" + value.value("cleanup").toString("没有取得文件会话。") + "。部分目录不会自动删除。");
        } else {
            status->setText("未完成：" + value.value("error").toString() + "\n" + value.value("cleanup").toString() + "。请核对后再决定是否新建另一次上传。");
        }
        receiptButton->setEnabled(QFileInfo::exists(options.receipt));
        if (!options.probe) {
            options.ftpPassword.clear();ftpPassword->setText(defaultFtpPassword());passwordVisibility->setChecked(false);
        }
    });
    connect(worker,&QThread::finished,this,[this] {
        auto* done = worker; worker = nullptr; cancelling = false;
        done->deleteLater(); updateControls();
    });
    status->setText(probe ? "正在只读检查 PLC 文件桥…" : options.directFtp ?
        "正在检查 PLC 上传许可；通过后由上位机直接连接控制柜 FTP…" : "正在连接 PLC 并检查文件桥…");
    updateControls(); worker->start();
}

void FileUploadDialog::updateProgress(const QJsonObject& event) {
    const auto phase = event.value("phase").toString();
    if (phase == "recovering" || phase == "recovered") {
        status->setText(phase == "recovering" ? "上次文件会话已失败且连接已关闭，正在释放文件通道…" :
            "上次文件会话已释放，继续检查本次上传；无需在PLC切换 xReset。");
        return;
    }
    const auto s = event.value("status").toObject();
    const int ordinal = s.value("ordinal").toInt(), verified = s.value("verified").toInt();
    const auto name = event.value("name").toString();
    const int size = s.value("size").toInt(), received = s.value("received").toInt();
    const int sent = s.value("sent").toInt(), readback = s.value("readback").toInt();
    QString text = phase == "verified" ? "回读一致，已关闭" : phase == "direct_upload" ? "电脑FTP直传中" : phase == "receiving" ? "PLC接收中" : phase == "cancelling" ? "清理中" : "FTP上传 / 回读中";
    for (int row = 0; row < table->rowCount(); ++row) if (table->item(row,0)->text() == name) {
        table->item(row,2)->setText(text); table->scrollToItem(table->item(row,0)); break;
    }
    if(options.directFtp)status->setText(QString("[%1/%2] %3 · %4\nPLC回读 %5/%6 字节；step=%7，reply=%8")
        .arg(ordinal).arg(options.files.size()).arg(name,text).arg(readback).arg(size)
        .arg(s.value("ftp_step").toInt()).arg(s.value("ftp_reply").toInt()));
    else status->setText(QString("[%1/%2] %3 · %4\nPLC接收 %5/%6，FTP发送 %7/%6，回读 %8/%6；step=%9，reply=%10")
        .arg(ordinal).arg(options.files.size()).arg(name,text).arg(received).arg(size).arg(sent).arg(readback)
        .arg(s.value("ftp_step").toInt()).arg(s.value("ftp_reply").toInt()));
    const double fraction = phase == "verified" ? 1. : options.directFtp ?
        phase=="ftp" ? .5+.49*(size?double(readback)/size:0.) : .1 : size ?
        std::min(0.99,(received + double(sent) + readback) / (3. * size)) : 0.;
    const double completed = phase == "verified" ? verified : std::max(0,ordinal - 1) + fraction;
    bar->setValue(int(100. * completed / options.files.size()));
}

void FileUploadDialog::reject() {
    if (worker) {
        cancelling = true; worker->requestInterruption();
        status->setText("正在取消，等待当前通讯结束及PLC连接清理结果。请保留窗口。"); updateControls(); return;
    }
    QDialog::reject();
}

void FileUploadDialog::closeEvent(QCloseEvent* event) {
    if (worker) { reject(); event->ignore(); } else event->accept();
}
