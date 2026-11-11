#include "workstation_converter.h"

#include <QtWidgets>
#include <QProcess>
#include <QDesktopServices>
#include <QStringDecoder>

QString openConversionDialog(QWidget* parent, const Job& nativeTemplate, const QString& workspace, const QByteArray& initialSource) {
    QDialog dialog(parent);
    dialog.setWindowTitle("G 代码 → STEP 工程 · STEPStudio 验证前准备");
    dialog.resize(1120, 780);
    auto* root = new QVBoxLayout(&dialog);
    auto* heading = new QLabel("① 导入或填写 G 代码　→　② 检查并生成完整工程　→　③ STEPStudio 仿真　→　④ 上位机导入同版本 ZIP");
    heading->setWordWrap(true);
    root->addWidget(heading);
    auto* fileRow = new QHBoxLayout;
    auto* path = new QLineEdit;
    path->setReadOnly(true);
    path->setPlaceholderText("可直接在下方填写 G 代码");
    auto* browse = new QPushButton("选择 G 代码…");
    fileRow->addWidget(path, 1);
    fileRow->addWidget(browse);
    root->addLayout(fileRow);
    auto* split = new QSplitter;
    root->addWidget(split, 1);
    auto* source = new QPlainTextEdit;
    source->setFont(QFont("Consolas", 11));
    source->setPlainText(initialSource.isEmpty() ? "G21 G17 G90 G94 G91.1\nG0 Z2\nG1 X20 F300\nY20\nX0\nY0\nG4 P0.5\nM30\n" : QString::fromUtf8(initialSource));
    split->addWidget(source);
    auto* right = new QWidget;
    auto* rightLayout = new QVBoxLayout(right);
    auto* settingsForm = new QFormLayout;
    auto* rapid = new QDoubleSpinBox;
    rapid->setRange(.001, 25);
    rapid->setDecimals(3);
    rapid->setValue(5);
    rapid->setSuffix(" mm/s");
    auto* tolerance = new QDoubleSpinBox;
    tolerance->setRange(.001, .1);
    tolerance->setDecimals(3);
    tolerance->setValue(.05);
    tolerance->setSuffix(" mm");
    settingsForm->addRow("G0 受限直线速度", rapid);
    settingsForm->addRow("圆弧弦高误差", tolerance);
    rightLayout->addLayout(settingsForm);
    auto* omit = new QCheckBox("明确忽略 M3/M4/M5/M11 工艺输出，生成空跑工程");
    omit->setToolTip("勾选后仅保留运动和等待；不控制激光、送粉或挤出。每条被忽略指令会记录在报告和映射中。");
    rightLayout->addWidget(omit);
    auto* coordinate = new QLabel("工件零点 = capture_home 记录的 TCP；轴向平行 WORLD。\n固定 Default 工具和起点姿态，F 单位 mm/min。\n支持 G0/G1、XY 的 I/J 圆弧和螺旋、G90/G91、模态 F、G4 P/T。\n输出始终为空跑，转换通过后仍需 STEPStudio 场景验证。");
    coordinate->setWordWrap(true);
    rightLayout->addWidget(coordinate);
    auto* tree = new QTreeWidget;
    tree->setHeaderLabel("生成的完整工程");
    tree->setMaximumHeight(200);
    rightLayout->addWidget(tree);
    auto* report = new QPlainTextEdit;
    report->setReadOnly(true);
    report->setPlainText("点击「检查 / 转换」查看逐行诊断和工程文件。导出主程序、起点基准程序及其变量；设备级返回由上位机独立规划，不生成工程内返回程序。");
    rightLayout->addWidget(report, 1);
    split->addWidget(right);
    split->setSizes({520, 560});
    auto* status = new QLabel("待转换 · STEPStudio 未验证");
    status->setWordWrap(true);
    root->addWidget(status);
    auto* buttons = new QHBoxLayout;
    auto* check = new QPushButton("检查 / 转换");
    check->setObjectName("convertCheck");
    auto* save = new QPushButton("生成 STEP 工程 ZIP…");
    save->setObjectName("convertExport");
    auto* studio = new QPushButton("打开 STEPStudio");
    auto* folder = new QPushButton("打开导出目录");
    auto* close = new QPushButton("关闭");
    check->setProperty("primary", true);
    save->setEnabled(false);
    folder->setEnabled(false);
    for (auto* button : {check, save, studio, folder}) buttons->addWidget(button);
    buttons->addStretch();
    buttons->addWidget(close);
    root->addLayout(buttons);
    Job compiled;
    QString exported;
    const auto invalidate = [&] { save->setEnabled(false); status->setText("内容或参数已改变，请重新检查 / 转换。STEPStudio 未验证。"); };
    QObject::connect(source, &QPlainTextEdit::textChanged, &dialog, invalidate);
    QObject::connect(rapid, &QDoubleSpinBox::valueChanged, &dialog, invalidate);
    QObject::connect(tolerance, &QDoubleSpinBox::valueChanged, &dialog, invalidate);
    QObject::connect(omit, &QCheckBox::toggled, &dialog, invalidate);
    QObject::connect(browse, &QPushButton::clicked, &dialog, [&] {
        const auto selected = QFileDialog::getOpenFileName(&dialog, "原始 G 代码", workspace, "G 代码 (*.gcode *.nc *.txt *.ngc *.tap)");
        if (selected.isEmpty()) return;
        QFile file(selected);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 32768) { status->setText("文件无法读取或超过 32 KiB。"); return; }
        QStringDecoder decoder(QStringDecoder::Utf8);
        const auto text = decoder(file.readAll());
        if (decoder.hasError()) { status->setText("源文件需要有效 UTF-8 文本。"); return; }
        path->setText(selected);
        source->setPlainText(text);
    });
    QObject::connect(check, &QPushButton::clicked, &dialog, [&] {
        QString text = source->toPlainText();
        if (text.endsWith('\n')) text.chop(1);
        const auto bytes = (text + '\n').toUtf8();
        if (bytes.size() > 32768) { status->setText("G 代码超过 32 KiB。"); save->setEnabled(false); return; }
        compiled = compileGcodeJob(path->text().isEmpty() ? "input.gcode" : QFileInfo(path->text()).fileName(), text.split('\n'), 1,
                                   nativeTemplate, {rapid->value(), tolerance->value(), omit->isChecked()});
        compiled.inputBytes = bytes;
        tree->clear();
        if (compiled.error.isEmpty()) {
            auto* project = new QTreeWidgetItem(tree, {compiled.project});
            for (auto it = compiled.files.cbegin(); it != compiled.files.cend(); ++it)
                new QTreeWidgetItem(project, {it.key()});
            project->setExpanded(true);
        }
        report->setPlainText(conversionReport(compiled));
        save->setEnabled(compiled.runnable());
        status->setText(compiled.error.isEmpty() ? "转换通过 · 7 个工程文件 · 待 STEPStudio 仿真验证" : "转换失败 · 请按源文件行号修正，未生成可执行工程");
    });
    QObject::connect(tree, &QTreeWidget::itemDoubleClicked, &dialog, [&](QTreeWidgetItem* item, int) {
        if (!compiled.files.contains(item->text(0))) return;
        QDialog preview(&dialog);
        preview.setWindowTitle(item->text(0) + " · 生成预览");
        preview.resize(850, 600);
        QVBoxLayout layout(&preview);
        QPlainTextEdit content;
        content.setReadOnly(true);
        content.setFont(QFont("Consolas", 11));
        content.setPlainText(QString::fromUtf8(compiled.files.value(item->text(0))));
        layout.addWidget(&content);
        preview.exec();
    });
    QObject::connect(save, &QPushButton::clicked, &dialog, [&] {
        const auto output = QFileDialog::getSaveFileName(&dialog, "生成待验证 STEP 工程", QDir(workspace).filePath("GcodePreview.zip"), "STEP 工程 (*.zip)");
        if (output.isEmpty()) return;
        const auto error = exportStepJob(compiled, output);
        if (!error.isEmpty()) { status->setText(error); return; }
        exported = output;
        folder->setEnabled(true);
        status->setText("已保存 ZIP、原始 G 代码、行映射和转换报告。下一步：在 STEPStudio 中导入并验证；没有自动发送至控制柜。");
    });
    const auto studioPath = QDir(workspace).filePath("STEPStudio 4.7.0/STEPStudio 4.7.0/bin/STEPStudio 4.7.0.exe");
    studio->setEnabled(QFileInfo::exists(studioPath));
    QObject::connect(studio, &QPushButton::clicked, &dialog, [&] {
        if (!QProcess::startDetached(studioPath, {})) status->setText("STEPStudio 未能启动，请从现有入口打开并导入 ZIP。");
    });
    QObject::connect(folder, &QPushButton::clicked, &dialog, [&] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(exported).absolutePath())); });
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
    return exported;
}
