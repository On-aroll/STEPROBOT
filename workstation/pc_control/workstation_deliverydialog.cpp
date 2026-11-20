#include "workstation_deliverydialog.h"
#include "workstation_fileuploaddialog.h"
#include "workstation_settings.h"

#include <QtWidgets>
#include <QDesktopServices>

GcodeDeliveryDialog::GcodeDeliveryDialog(const Job& base, const QString& address, const QString& outputs,
    const QString& receipts, QWidget* parent, bool autoJobMode)
    : QDialog(parent), nativeTemplate(base), autoMode(autoJobMode), endpoint(address), outputRoot(outputs), receiptRoot(receipts) {
    setObjectName("gcodeDeliveryDialog");
    setWindowTitle("G 代码转换与上传"); resize(980, 740);
    auto* root = new QVBoxLayout(this);
    heading = new QLabel("① 导入并自动转换　→　② 保存并经PLC上传　→　③ 示教器绑定新工程编号　→　④ 手动加载 / 使能 / 运行");
    heading->setObjectName("deliveryRoute");
    if(autoMode) { setWindowTitle("自动作业 · 复用固定程序号");heading->setText("导入G代码 → 自动转换 / PLC上传 / 回读 → 显示11号 → 登记 / 加载 / 确认 / 使能 / 运行"); }
    heading->setWordWrap(true); root->addWidget(heading);
    auto* row = new QHBoxLayout;
    path = new QLineEdit; path->setReadOnly(true); path->setObjectName("deliverySource");
    browse = new QPushButton("导入G代码…"); browse->setObjectName("deliveryBrowse");
    row->addWidget(path, 1); row->addWidget(browse); root->addLayout(row);
    auto* form = new QFormLayout;
    rapid = new QDoubleSpinBox; rapid->setRange(.001, 25); rapid->setDecimals(3); rapid->setValue(5); rapid->setSuffix(" mm/s");
    tolerance = new QDoubleSpinBox; tolerance->setRange(.001, .1); tolerance->setDecimals(3); tolerance->setValue(.05); tolerance->setSuffix(" mm");
    form->addRow("G0 受限直线速度", rapid); form->addRow("圆弧弦高误差", tolerance); root->addLayout(form);
    omit = new QCheckBox("明确忽略 M3/M4/M5/M11 工艺输出，生成空跑工程"); omit->setObjectName("deliveryOmitProcess");
    includeReturn = new QCheckBox("附带今天已验证模板的 go_home（关节PTP回程，须单独核对路径）");
    includeReturn->setChecked(true); root->addWidget(omit); root->addWidget(includeReturn);
    autoUpload=new QCheckBox("换选 G 代码并转换通过后自动上传");autoUpload->setObjectName("deliveryAutoUpload");autoUpload->setChecked(true);root->addWidget(autoUpload);
    autoUpload->setVisible(autoMode);includeReturn->setVisible(!autoMode);
    transport=new QComboBox;transport->setObjectName("deliveryTransport");
    transport->addItems({"PLC 转传：上位机 → PLC → 控制柜","电脑直传：上位机 → 控制柜（SDK手册FTP）"});
    root->addWidget(transport);transport->setVisible(autoMode);
    connect(transport,&QComboBox::currentIndexChanged,this,[this]{refresh();});
    connect(transport,&QComboBox::activated,this,[this](int index){
        auto settings = workstationSettings(); settings.setValue("fileBridge/directTransfer", index == 1);
    });
    connectionHint = new QLabel; connectionHint->setObjectName("deliveryConnectionHint");
    connectionHint->setWordWrap(true); root->addWidget(connectionHint); connectionHint->setVisible(autoMode);
    auto* note = new QLabel("仅生成空跑运动。毫米、WORLD轴向、Default工具，零点为本工程 capture_home 记录的TCP；F为mm/min。"
        "当前转换器上限：32 KiB源文件、1000路径点、25 mm/s。转换通过仍须验证场景、姿态和路径。"
        "PLC桥接程序本次无需修改；每次上传创建独立目录，柜内GDI绑定由你完成。");
    note->setWordWrap(true); root->addWidget(note);
    if(autoMode)note->setText("可一次选择1～15个G代码，按下方清单顺序执行，共用10号手动服务已捕捉的实验起点。仅生成空跑运动；不含焊接和送丝输出。"
        "打印方向保持捕捉起点时的末端方向，J5/J6随轨迹协调。"
        "固定入口为 pcjob_manual_r63.sr / auto_main，作业号11；须先完成一次性入口绑定与PLC更新。日常上传不写起点或手动服务文件。");
    status = new QLabel("请选择G代码；导入只在本地转换，不连接设备。"); status->setObjectName("deliveryStatus");
    status->setWordWrap(true); root->addWidget(status);
    report = new QPlainTextEdit; report->setReadOnly(true); report->setObjectName("deliveryReport"); root->addWidget(report, 1);
    auto* buttons = new QHBoxLayout;
    save = new QPushButton("仅保存工程"); save->setObjectName("deliverySave");
    send = new QPushButton("保存并经PLC上传…"); send->setObjectName("deliveryUpload"); send->setProperty("primary", true);
    folder = new QPushButton("打开作业目录");
    copy = new QPushButton("复制绑定清单"); copy->setObjectName("deliveryCopyHandoff");
    if(autoMode)copy->setText("复制作业清单");
    auto* close = new QPushButton("关闭");
    for (auto* button : {save, send, folder, copy}) buttons->addWidget(button);
    buttons->addStretch(); buttons->addWidget(close); root->addLayout(buttons);
    connect(browse, &QPushButton::clicked, this, [this] {
        if(autoMode) {
            const auto sources=QFileDialog::getOpenFileNames(this,"导入一个或多个G代码",{},"G代码 (*.nc *.gcode *.ngc *.tap *.txt)");
            if(!sources.isEmpty()) { importSources(sources);QTimer::singleShot(0,this,&GcodeDeliveryDialog::startAutoUpload); }
            return;
        }
        const auto file = QFileDialog::getOpenFileName(this, "导入G代码并自动转换", QFileInfo(path->text()).absolutePath(),
            "G代码 (*.nc *.gcode *.ngc *.tap *.txt)");
        if (!file.isEmpty()) importSource(file);
    });
    connect(rapid, &QDoubleSpinBox::valueChanged, this, [this] { rebuild(); });
    connect(tolerance, &QDoubleSpinBox::valueChanged, this, [this] { rebuild(); });
    connect(omit, &QCheckBox::toggled, this, [this] { rebuild(); });
    connect(includeReturn, &QCheckBox::toggled, this, [this] { rebuild(); });
    connect(save, &QPushButton::clicked, this, [this] { savePackage(); });
    connect(send, &QPushButton::clicked, this, &GcodeDeliveryDialog::upload);
    connect(folder, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(package.directory)); });
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(autoMode?Delivery::autoHandoff(autoJob,transfer):Delivery::handoff(job, transfer)); });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    refresh();
}

void GcodeDeliveryDialog::importSource(const QString& source) {
    if(autoMode){importSources({source});return;}
    if (attempted) return;
    path->setText(source); rebuild();
}

void GcodeDeliveryDialog::importSources(const QStringList& sources) {
    if(attempted || sources.isEmpty())return;
    sourceSnapshots.clear();
    for (const auto& source : sources) sourceSnapshots.append(Delivery::readSource(source));
    sourcePaths=sources;path->setText(sources.join("；"));browse->setText("更换G代码…");rebuild();
}

void GcodeDeliveryDialog::importCurrentSource(const Job& current) {
    if (!autoMode || attempted) return;
    const auto source = Delivery::currentSource(current);
    if (source.path.isEmpty()) return;
    sourceSnapshots = {source}; sourcePaths = {source.path};
    path->setText(source.path); browse->setText("更换G代码…");
    const QSignalBlocker rapidBlock(rapid), toleranceBlock(tolerance), omitBlock(omit);
    rapid->setValue(current.conversion.rapidMmS); tolerance->setValue(current.conversion.arcToleranceMm);
    omit->setChecked(current.conversion.omitProcessOutputs);
    rebuild();
}

void GcodeDeliveryDialog::setDirectTransfer(bool direct, const QString& cabinet) {
    if (!autoMode || attempted) return;
    cabinetEndpoint = cabinet;
    transport->setCurrentIndex(direct ? 1 : 0);
    refresh();
}

void GcodeDeliveryDialog::startAutoUpload() {
    if (autoMode && autoUpload->isChecked() && !autoJob.parts.isEmpty() && autoJob.error.isEmpty()) upload();
}

void GcodeDeliveryDialog::rebuild() {
    if (attempted) return;
    package = {}; transfer = {}; job = {};
    if(autoMode) {
        autoJob=Delivery::compileAutoSnapshots(sourceSnapshots,nativeTemplate,{rapid->value(),tolerance->value(),omit->isChecked()});
        QStringList lines;
        for(int i=0;i<autoJob.parts.size();++i)lines<<QString("%1. auto_%2 ← %3\n%4").arg(i+1).arg(i+1,2,10,QLatin1Char('0')).arg(sourcePaths[i],autoJob.parts[i].summary);
        status->setText(autoJob.error.isEmpty()?QString("转换通过：%1个轨迹，%2个文件，固定作业号11；版本%3。").arg(autoJob.parts.size()).arg(autoJob.files.size()).arg(autoJob.revision):autoJob.error);
        report->setPlainText(lines.join("\n\n"));refresh();emit previewChanged();return;
    }
    if (!path->text().isEmpty()) {
        job = Delivery::compile(path->text(), nativeTemplate, {rapid->value(), tolerance->value(), omit->isChecked()}, includeReturn->isChecked());
        if (job.error.isEmpty()) {
            status->setText(QString("本地转换通过：%1个原生文件；尚未上传，尚无GDI程序号。").arg(job.files.size()));
            report->setPlainText(conversionReport(job));
        } else { status->setText("转换失败：" + job.error); report->setPlainText(conversionReport(job)); }
    }
    refresh();
}

bool GcodeDeliveryDialog::savePackage() {
    if(autoMode) {
        if(!autoJob.error.isEmpty()||autoJob.parts.isEmpty())return false;
        if(!package.zip.isEmpty()&&package.error.isEmpty())return true;
        package=Delivery::saveAuto(autoJob,outputRoot);
        status->setText(package.error.isEmpty()?"已保存自动作业："+package.directory:package.error);refresh();return package.error.isEmpty();
    }
    if (!job.runnable() || path->text().isEmpty()) return false;
    if (!package.zip.isEmpty() && package.error.isEmpty()) return true;
    package = Delivery::save(job, path->text(), outputRoot);
    if (!package.error.isEmpty()) status->setText("本地保存未完成：" + package.error);
    else status->setText("已保存工程、源文件、行映射、转换报告及交接记录：" + package.directory + "。尚未上传。");
    refresh(); return package.error.isEmpty();
}

void GcodeDeliveryDialog::upload() {
    if (attempted || !savePackage()) return;
    attempted = true; refresh();
    if (autoMode) { uploading = true; emit previewChanged(); }
    // Only this explicit user action starts a file transfer. No control-panel calls.
    FileUploadDialog dialog(autoMode?autoJob.files:job.files, package.zip, endpoint, receiptRoot, this);
    if(autoMode) {dialog.useAutoTarget(autoJob.project,quint16(autoJob.number),autoJob.tag);dialog.setDirectMode(transport->currentIndex()==1,cabinetEndpoint);}
    if(!autoMode || transport->currentIndex()==0)QTimer::singleShot(0, &dialog, &FileUploadDialog::beginUpload);
    dialog.exec(); uploading = false; transfer = dialog.result();
    if(autoMode) {
        const auto error=Delivery::recordAuto(package,autoJob,transfer);
        status->setText(transfer.value("status").toString()=="COMPLETE"?"上传与回读通过 · 可用作业号11 · 请重新加载本次版本":"上传未完成 · 本次作业号未放行");
        report->setPlainText(Delivery::autoHandoff(autoJob,transfer)+"\n\n作业目录："+package.directory+"\n"+error);refresh();emit previewChanged();return;
    }
    const auto recordError = Delivery::record(package, job, transfer);
    status->setText(transfer.value("status").toString() == "COMPLETE" ?
        "上传及回读成功 · 下一步：在示教器绑定本次新工程的GDI编号。" :
        "上传未完成 · 保留回执核对；本窗口不自动重试。");
    report->setPlainText(Delivery::handoff(job, transfer) + "\n\n本地作业：" + package.directory +
        "\n文件回执：" + transfer.value("receipt").toString() + (recordError.isEmpty() ? QString() : "\n记录提示：" + recordError));
    refresh();
}

void GcodeDeliveryDialog::refresh() {
    const bool valid = autoMode?autoJob.error.isEmpty()&&!autoJob.parts.isEmpty():!path->text().isEmpty() && job.runnable();
    for (QWidget* widget : QList<QWidget*>{browse, rapid, tolerance, omit, includeReturn}) widget->setEnabled(!attempted);
    save->setEnabled(valid && !attempted); send->setEnabled(valid && !attempted);
    transport->setEnabled(!attempted);
    if(autoMode) {
        const bool direct = transport->currentIndex() == 1;
        heading->setText(direct ? "当前 G 代码 → 自动转换 → 上位机直传控制柜 → PLC回读确认11号" :
            "当前 G 代码 → 自动转换 → PLC转传控制柜 → 回读确认11号");
        connectionHint->setText(direct ?
            "文件传输：上位机通过 FTP 直接连接控制柜。PLC另行提供上传许可、回读校验和11号版本确认。无需先点顶部「连接」；开始上传时建立独立连接。" :
            "文件传输：上位机发给 PLC，由 PLC 上传控制柜并回读校验。无需先点顶部「连接」；开始上传时自动连接 PLC。可在此独立切换传输方式。");
        send->setText(direct ? "上传当前 G 代码 · 电脑直传…" : "上传当前 G 代码 · PLC转传…");
        send->setToolTip("默认上传当前窗口中列出的 G 代码快照；更换文件会同步更新主界面原文和轨迹。");
    }
    folder->setEnabled(!package.zip.isEmpty() && package.error.isEmpty());
    copy->setEnabled(transfer.value("status").toString() == "COMPLETE");
}
