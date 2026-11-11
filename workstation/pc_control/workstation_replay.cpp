#include "workstation_replay.h"
#include "workstation_plot.h"

#include <QtWidgets>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <limits>

namespace {
// 读取归档中的单行字段，处理逗号与双引号转义。
QStringList parseCsvLine(const QString& line) {
    QStringList fields;
    QString field;
    bool quoted = false;
    for (int i = 0; i < line.size(); ++i) {
        const auto character = line.at(i);
        if (quoted) {
            if (character == u'"') {
                if (i + 1 < line.size() && line.at(i + 1) == u'"') { field += character; ++i; }
                else quoted = false;
            } else field += character;
        } else if (character == u'"') quoted = true;
        else if (character == u',') { fields << field; field.clear(); }
        else field += character;
    }
    fields << field;
    return fields;
}

QString stateText(const QString& state) {
    if (state == "0") return "运行中";
    if (state == "1") return "已暂停";
    if (state == "2") return "已停止";
    return state.isEmpty() ? "—" : "状态 " + state;
}

QString modeText(const QString& mode) {
    if (mode == "0" || mode == "1") return "手动";
    if (mode == "2") return "自动";
    if (mode == "3") return "外部自动";
    return mode.isEmpty() ? "—" : "模式 " + mode;
}

QString fmtSeconds(double seconds) {
    const auto s = static_cast<int>(std::max(0., seconds));
    return QString("%1:%2").arg(s / 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}

QString resultText(const QString& result) {
    static const QMap<QString, QString> names{{"completed", "已完成"}, {"stopped", "已停止"}, {"ended_unverified", "结束未核对"},
        {"feedback_lost", "连接中断"}, {"session_closed", "会话结束"}, {"start_cancelled", "启动已取消"},
        {"start_rejected", "启动被拒绝"}, {"start_unconfirmed", "启动未确认"}, {"start_not_sent", "启动未发送"},
        {"in_progress", "记录中"}};
    return names.value(result, result.isEmpty() ? "—" : result);
}

QString joined(const QStringList& values, const QString& separator = " , ") {
    QStringList shown;
    for (const auto& value : values) shown << (value.isEmpty() ? "—" : value);
    return shown.join(separator);
}
}

RunSummary RunSummary::parse(const QString& folder, const QJsonObject& metadata) {
    RunSummary run;
    run.folder = folder;
    run.label = metadata.value("label").toString();
    run.jobName = metadata.value("job").toString();
    run.mode = metadata.value("mode").toString();
    run.result = metadata.value("result").toString();
    run.requestedUtc = metadata.value("requested_utc").toString();
    run.endedUtc = metadata.value("ended_utc").toString();
    run.controllerProject = metadata.value("controller_project").toString();
    run.controllerProgram = metadata.value("controller_program").toString();
    run.startLine = metadata.value("start_source_line").toInt();
    run.samples = metadata.value("samples").toInt();
    run.distanceSamples = metadata.value("distance_samples").toInt();
    run.overridePct = metadata.value("initial_override_pct").toDouble();
    run.hasDistance = metadata.contains("path_distance_rms_mm");
    run.rmsMm = metadata.value("path_distance_rms_mm").toDouble();
    run.maxMm = metadata.value("path_distance_max_mm").toDouble();
    run.hasEndpoint = metadata.contains("endpoint_distance_mm");
    run.endpointMm = metadata.value("endpoint_distance_mm").toDouble();
    run.complete = !metadata.value("capture_incomplete").toBool();
    run.observedRunning = metadata.value("observed_running").toBool();
    return run;
}

QVector<RunSummary> scanRuns(const QString& root) {
    QVector<RunSummary> runs;
    for (const auto& entry : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed)) {
        QFile file(QDir(entry.absoluteFilePath()).filePath("run.json"));
        if (!file.open(QIODevice::ReadOnly)) continue;
        const auto document = QJsonDocument::fromJson(file.readAll());
        if (!document.isObject()) continue;
        runs << RunSummary::parse(entry.absoluteFilePath(), document.object());
    }
    return runs;
}

double runningSeconds(const QString& folder) {
    QFile file(QDir(folder).filePath("trace.csv"));
    if (!file.open(QIODevice::ReadOnly)) return 0;
    double total = 0, previous = 0;
    bool wasRunning = false;
    const auto lines = QString::fromUtf8(file.readAll()).split('\n');
    for (int i = 1; i < lines.size(); ++i) {
        const auto line = lines.at(i).trimmed();
        if (line.isEmpty()) continue;
        const auto f = parseCsvLine(line);
        if (f.size() < 4) continue;
        bool ok = false;
        const double elapsed = f.at(1).toDouble(&ok);
        if (!ok || !std::isfinite(elapsed)) continue;
        if (wasRunning && elapsed >= previous) total += elapsed - previous;
        previous = elapsed;
        wasRunning = f.at(3) == "0";
    }
    return total;
}

namespace {
struct Frame {
    double elapsed = 0;
    QString utc, state, nativeLine, sourceLine, mode, enabled, overridePct, alarm, speed;
    double xyz[3]{}, abc[3]{}, axes[6]{};
    bool hasMeasured = false;
    Point measured{};
    double distance = 0;
    bool hasDistance = false;
};

// 拖时间轴回放一次实验：计划轨迹按源行进度着色，叠加当时实测点与 TCP 标记。
class ReplayDialog : public QDialog {
public:
    ReplayDialog(const RunSummary& run, QWidget* parent = nullptr) : QDialog(parent) {
        setWindowTitle("离线回放 · " + QDir(run.folder).dirName() + (run.label.isEmpty() ? "" : " · " + run.label));
        resize(840, 660);
        auto* layout = new QVBoxLayout(this);

        const auto headline = QString("%1\n%2 · %3 · 倍率 %4% · 柜内 %5 / %6 · 结果 %7 · 样本 %8 帧")
            .arg(run.requestedUtc.isEmpty() ? "（无开始时间）" : run.requestedUtc,
                 run.label.isEmpty() ? "（无备注）" : run.label, run.mode == "real" ? "实机" : "仿真")
            .arg(run.overridePct, 0, 'f', 0)
            .arg(run.controllerProject.isEmpty() ? "—" : run.controllerProject,
                 run.controllerProgram.isEmpty() ? "—" : run.controllerProgram, resultText(run.result))
            .arg(run.samples);
        auto* summary = new QLabel(headline);
        summary->setWordWrap(true);
        layout->addWidget(summary);

        plot = new Plot;
        layout->addWidget(plot, 1);

        loadPlan(run.folder);
        frames = loadTrace(run.folder);
        measuredPoints.reserve(frames.size());
        for (const auto& frame : frames)
            measuredPoints << (frame.hasMeasured ? frame.measured : Point{std::numeric_limits<double>::quiet_NaN(), 0, 0});
        if (planPoints.size() >= 2) plot->points = &planPoints;

        auto* eventList = new QListWidget;
        eventList->setMaximumHeight(96);
        QFile eventsFile(QDir(run.folder).filePath("events.log"));
        if (eventsFile.open(QIODevice::ReadOnly)) {
            for (const auto& raw : QString::fromUtf8(eventsFile.readAll()).split('\n')) {
                const auto line = raw.trimmed();
                if (!line.isEmpty()) eventList->addItem(line);
            }
        }
        if (eventList->count()) layout->addWidget(eventList);

        auto* controls = new QHBoxLayout;
        playButton = new QPushButton("播放");
        controls->addWidget(playButton);
        speedBox = new QComboBox;
        for (const double speed : {0.25, 0.5, 1.0, 2.0, 4.0, 8.0}) speedBox->addItem(QString::number(speed, 'f', 2) + "x", speed);
        speedBox->setCurrentIndex(2);
        controls->addWidget(speedBox);
        slider = new QSlider(Qt::Horizontal);
        slider->setRange(0, std::max(0, static_cast<int>(frames.size()) - 1));
        controls->addWidget(slider, 1);
        layout->addLayout(controls);

        frameInfo = new QLabel;
        frameInfo->setWordWrap(true);
        layout->addWidget(frameInfo);

        if (frames.isEmpty()) {
            frameInfo->setText("这次试次没有可回放的样本（trace.csv 为空或缺失）。");
            playButton->setEnabled(false);
            slider->setEnabled(false);
            speedBox->setEnabled(false);
            return;
        }
        connect(slider, &QSlider::valueChanged, this, [this](int value) { showFrame(value); });
        connect(playButton, &QPushButton::clicked, this, [this] {
            playing = !playing;
            playButton->setText(playing ? "暂停" : "播放");
            wall.restart();
            if (playing && current >= frames.size() - 1) slider->setValue(0);
        });
        ticker.setInterval(40);
        connect(&ticker, &QTimer::timeout, this, [this] {
            if (!playing) return;
            cursor += wall.restart() / 1000. * speedBox->currentData().toDouble();
            int next = current;
            while (next + 1 < frames.size() && frames.at(next + 1).elapsed <= cursor) ++next;
            if (next >= frames.size() - 1) { playing = false; playButton->setText("播放"); }
            if (next != current) {
                const double playbackTime = cursor;
                slider->setValue(next);
                cursor = playbackTime;
            }
        });
        ticker.start();
        showFrame(0);
    }

private:
    void loadPlan(const QString& folder) {
        QFile file(QDir(folder).filePath("plan.csv"));
        if (!file.open(QIODevice::ReadOnly)) return;
        bool header = true;
        for (const auto& raw : QString::fromUtf8(file.readAll()).split('\n')) {
            const auto line = raw.trimmed();
            if (line.isEmpty()) continue;
            const auto f = parseCsvLine(line);
            if (header) { header = false; continue; }
            if (f.size() < 5) continue;
            planRows << f.at(1).toInt();
            planPoints << Point{f.at(2).toDouble(), f.at(3).toDouble(), f.at(4).toDouble()};
        }
    }

    static QVector<Frame> loadTrace(const QString& folder) {
        QVector<Frame> frames;
        QFile file(QDir(folder).filePath("trace.csv"));
        if (!file.open(QIODevice::ReadOnly)) return frames;
        bool header = true;
        for (const auto& raw : QString::fromUtf8(file.readAll()).split('\n')) {
            const auto line = raw.trimmed();
            if (line.isEmpty()) continue;
            const auto f = parseCsvLine(line);
            if (header) { header = false; continue; }
            // trace 列序见 Experiment::begin 的表头：27 列，实测与路径距离在末尾 4 列。
            if (f.size() < 27) continue;
            Frame frame;
            frame.utc = f.at(0);
            frame.elapsed = f.at(1).toDouble();
            frame.state = f.at(3);
            frame.nativeLine = f.at(4);
            frame.sourceLine = f.at(5);
            frame.mode = f.at(6);
            frame.enabled = f.at(7);
            frame.overridePct = f.at(8);
            frame.alarm = f.at(9);
            frame.speed = f.at(10);
            for (int i = 0; i < 3; ++i) {
                frame.xyz[i] = f.at(11 + i).toDouble();
                frame.abc[i] = f.at(14 + i).toDouble();
            }
            for (int i = 0; i < 6; ++i) frame.axes[i] = f.at(17 + i).toDouble();
            frame.hasMeasured = !f.at(23).isEmpty();
            if (frame.hasMeasured) frame.measured = Point{f.at(23).toDouble(), f.at(24).toDouble(), f.at(25).toDouble()};
            frame.hasDistance = !f.at(26).isEmpty();
            frame.distance = frame.hasDistance ? f.at(26).toDouble() : 0;
            frames << frame;
        }
        return frames;
    }

    void showFrame(int index) {
        if (index < 0 || index >= frames.size()) return;
        current = index;
        cursor = frames.at(index).elapsed;
        const auto& frame = frames.at(index);
        int completed = 0, active = 0;
        const int line = frame.sourceLine.toInt();
        if (line > 0)
            for (int i = 1; i < planRows.size(); ++i)
                if (planRows.at(i) > 0 && planRows.at(i) < line) completed = i;
                else if (planRows.at(i) == line && !active) active = i;
        plot->completed = completed;
        plot->active = active;
        plot->measured = measuredPoints.mid(0, index + 1);
        if (frame.hasMeasured) plot->tcp = frame.measured;
        else if (!planPoints.isEmpty()) plot->tcp = planPoints.at(std::min(completed, static_cast<int>(planPoints.size()) - 1));
        else plot->tcp = std::nullopt;
        plot->update();
        const auto position = joined({QString::number(frame.xyz[0], 'f', 2), QString::number(frame.xyz[1], 'f', 2),
                                      QString::number(frame.xyz[2], 'f', 2)});
        const auto orientation = joined({QString::number(frame.abc[0], 'f', 1), QString::number(frame.abc[1], 'f', 1),
                                         QString::number(frame.abc[2], 'f', 1)});
        const auto joints = joined({QString::number(frame.axes[0], 'f', 2), QString::number(frame.axes[1], 'f', 2),
                                    QString::number(frame.axes[2], 'f', 2), QString::number(frame.axes[3], 'f', 2),
                                    QString::number(frame.axes[4], 'f', 2), QString::number(frame.axes[5], 'f', 2)});
        QString text = QString("t=%1 s · %2 · %3 · 伺服 %4 · STEP 行 %5 · 源行 %6 · TCP %7 mm/s · 倍率 %8% · 报警 %9")
            .arg(frame.elapsed, 0, 'f', 1).arg(stateText(frame.state), modeText(frame.mode),
                 frame.enabled == "1" ? "已使能" : frame.enabled == "0" ? "未使能" : "—")
            .arg(frame.nativeLine.isEmpty() ? "—" : frame.nativeLine, frame.sourceLine.isEmpty() ? "—" : frame.sourceLine)
            .arg(frame.speed.isEmpty() ? "—" : frame.speed, frame.overridePct.isEmpty() ? "—" : frame.overridePct,
                 frame.alarm.isEmpty() || frame.alarm == "0" ? "无" : frame.alarm);
        if (frame.hasDistance) text += QString("\n路径距离 %1 mm · 位置 %2 · 姿态 %3 · 关节 %4")
            .arg(frame.distance, 0, 'f', 3).arg(position, orientation, joints);
        else text += QString("\n位置 %1 · 姿态 %2 · 关节 %3").arg(position, orientation, joints);
        frameInfo->setText(text);
    }

    Plot* plot = nullptr;
    QLabel* frameInfo = nullptr;
    QPushButton* playButton = nullptr;
    QComboBox* speedBox = nullptr;
    QSlider* slider = nullptr;
    QTimer ticker;
    QElapsedTimer wall;
    QVector<Frame> frames;
    QVector<Point> planPoints, measuredPoints;
    QVector<int> planRows;
    double cursor = 0;
    int current = 0;
    bool playing = false;
};

// 两份试次并排对比：参数、耗时与轨迹偏差逐项列出，可导出纯文本报告。
class CompareDialog : public QDialog {
public:
    CompareDialog(const RunSummary& first, const RunSummary& second, QWidget* parent = nullptr)
        : QDialog(parent), a(first), b(second) {
        setWindowTitle("试次对比 · 甲 " + QDir(first.folder).dirName() + " · 乙 " + QDir(second.folder).dirName());
        resize(760, 600);
        auto* layout = new QVBoxLayout(this);
        table = new QTableWidget;
        table->setColumnCount(4);
        table->setHorizontalHeaderLabels({"项目", "甲", "乙", "差异"});
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->verticalHeader()->setVisible(false);
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table, 1);

        addRow("开始时间", shown(a.requestedUtc), shown(b.requestedUtc));
        addRow("备注", shown(a.label), shown(b.label));
        addRow("作业", shown(a.jobName), shown(b.jobName));
        addRow("目标", a.mode == "real" ? "实机" : "仿真", b.mode == "real" ? "实机" : "仿真");
        addRow("柜内工程 / 程序", shown(a.controllerProject) + " / " + shown(a.controllerProgram),
               shown(b.controllerProject) + " / " + shown(b.controllerProgram));
        addValue("起始行", a.startLine, b.startLine, "", 0);
        addValue("倍率 %", a.overridePct, b.overridePct, "%", 0);
        addValue("样本帧数", a.samples, b.samples, "帧", 0);
        addValue("运行时长（采样估计）s", runningSeconds(a.folder), runningSeconds(b.folder), "s", 1);
        if (a.hasDistance && b.hasDistance) {
            addValue("路径 RMS mm", a.rmsMm, b.rmsMm, "mm", 3);
            addValue("路径峰值 mm", a.maxMm, b.maxMm, "mm", 3);
        } else if (a.hasDistance || b.hasDistance) {
            addRow("路径 RMS mm", a.hasDistance ? QString::number(a.rmsMm, 'f', 3) : "未记录", b.hasDistance ? QString::number(b.rmsMm, 'f', 3) : "未记录");
            addRow("路径峰值 mm", a.hasDistance ? QString::number(a.maxMm, 'f', 3) : "未记录", b.hasDistance ? QString::number(b.maxMm, 'f', 3) : "未记录");
        }
        if (a.hasEndpoint && b.hasEndpoint) addValue("端点误差 mm", a.endpointMm, b.endpointMm, "mm", 3);
        else if (a.hasEndpoint || b.hasEndpoint) addRow("端点误差 mm", a.hasEndpoint ? QString::number(a.endpointMm, 'f', 3) : "未记录", b.hasEndpoint ? QString::number(b.endpointMm, 'f', 3) : "未记录");
        addRow("结果", resultText(a.result), resultText(b.result));
        addRow("记录完整", a.complete ? "完整" : "不完整", b.complete ? "完整" : "不完整");
        table->setRowCount(rows.size());
        for (int i = 0; i < rows.size(); ++i)
            for (int column = 0; column < 4; ++column) {
                auto* item = new QTableWidgetItem(rows.at(i).at(column));
                if (column == 3 && rows.at(i).at(column) != "相同") item->setForeground(QColor("#b77900"));
                table->setItem(i, column, item);
            }

        auto* actions = new QHBoxLayout;
        auto* exportButton = new QPushButton("导出对比报告");
        auto* closeButton = new QPushButton("关闭");
        actions->addStretch(1);
        actions->addWidget(exportButton);
        actions->addWidget(closeButton);
        layout->addLayout(actions);
        connect(exportButton, &QPushButton::clicked, this, &CompareDialog::saveReport);
        connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    }

private:
    static QString shown(const QString& value) { return value.isEmpty() ? "—" : value; }

    void addRow(const QString& name, const QString& left, const QString& right) {
        rows << (QStringList{name, left, right} << (left == right ? "相同" : "不同"));
    }

    void addValue(const QString& name, double left, double right, const QString& unit, int decimals) {
        const auto format = [&unit, decimals](double value) {
            return QString::number(value, 'f', decimals) + (unit == "%" ? "%" : unit.isEmpty() ? "" : " " + unit);
        };
        addRow(name, format(left), format(right));
        rows.last()[3] = std::fabs(right - left) < 1e-9 ? "相同"
            : QString("%1%2%3").arg(right > left ? "+" : "", QString::number(right - left, 'f', decimals), unit == "%" ? "%" : " " + unit);
    }

    void saveReport() {
        const auto path = QFileDialog::getSaveFileName(this, "导出对比报告",
            QDir(QDir(a.folder).filePath("..")).filePath("对比报告.txt"), "文本 (*.txt)");
        if (path.isEmpty()) return;
        QString report = QString("实验对比报告\n甲 %1\n乙 %2\n\n").arg(QDir::toNativeSeparators(a.folder), QDir::toNativeSeparators(b.folder));
        for (const auto& row : rows) report += row.join("  |  ") + '\n';
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly) || output.write(report.toUtf8()) < 0)
            QMessageBox::warning(this, "导出失败", "无法写入所选文件，请换一个位置再试。");
        else QMessageBox::information(this, "导出完成", "对比报告已保存到：\n" + QDir::toNativeSeparators(path));
    }

    RunSummary a, b;
    QTableWidget* table = nullptr;
    QVector<QStringList> rows;
};
}

RunBrowserDialog::RunBrowserDialog(const QString& root, QWidget* parent) : QDialog(parent) {
    setWindowTitle("历史试次 · " + QDir::toNativeSeparators(root));
    resize(960, 480);
    auto* layout = new QVBoxLayout(this);
    const auto runs = scanRuns(root);
    auto* table = new QTableWidget(runs.size(), 10, this);
    table->setHorizontalHeaderLabels({"开始时间", "备注", "作业", "目标", "结果", "样本", "RMS mm", "峰值 mm", "端点 mm", "记录"});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(table, 1);
    if (runs.isEmpty()) layout->addWidget(new QLabel(QString("还没有历史试次。开始一次作业会自动在 %1 下建一份记录。").arg(QDir::toNativeSeparators(root))));
    else layout->addWidget(new QLabel("选中一行点「回放」拖时间轴重看；按住 Ctrl 选两行点「对比」。双击一行也直接回放。"));
    auto* actions = new QHBoxLayout;
    auto* openFolder = new QPushButton("打开记录文件夹");
    auto* replayButton = new QPushButton("回放");
    auto* compareButton = new QPushButton("对比");
    auto* closeButton = new QPushButton("关闭");
    actions->addWidget(openFolder);
    actions->addStretch(1);
    actions->addWidget(replayButton);
    actions->addWidget(compareButton);
    actions->addWidget(closeButton);
    layout->addLayout(actions);

    const auto text = [](const QString& value) { return value.isEmpty() ? "—" : value; };
    for (int i = 0; i < runs.size(); ++i) {
        const auto& run = runs.at(i);
        const QStringList cells{text(run.requestedUtc), text(run.label), text(run.jobName),
            run.mode == "real" ? "实机" : "仿真", resultText(run.result), QString::number(run.samples),
            run.hasDistance ? QString::number(run.rmsMm, 'f', 3) : "—",
            run.hasDistance ? QString::number(run.maxMm, 'f', 3) : "—",
            run.hasEndpoint ? QString::number(run.endpointMm, 'f', 3) : "—",
            run.complete ? "完整" : "不完整"};
        for (int column = 0; column < cells.size(); ++column) table->setItem(i, column, new QTableWidgetItem(cells.at(column)));
    }

    const auto pickedRows = [table] {
        QSet<int> picked;
        for (const auto* item : table->selectedItems()) picked.insert(item->row());
        auto rows = picked.values();
        std::sort(rows.begin(), rows.end());
        return rows;
    };
    const auto updateButtons = [table, replayButton, compareButton] {
        const auto picked = [table] {
            QSet<int> rows;
            for (const auto* item : table->selectedItems()) rows.insert(item->row());
            return rows.size();
        }();
        replayButton->setEnabled(picked == 1);
        compareButton->setEnabled(picked == 2);
    };
    connect(table->selectionModel(), &QItemSelectionModel::selectionChanged, this, updateButtons);
    updateButtons();
    connect(table, &QTableWidget::cellDoubleClicked, this, [this, table, runs](int row, int) {
        if (row < 0 || row >= runs.size()) return;
        ReplayDialog(runs.at(row), this).exec();
    });
    connect(replayButton, &QPushButton::clicked, this, [this, table, runs, pickedRows] {
        const auto picked = pickedRows();
        if (picked.size() == 1) ReplayDialog(runs.at(picked.first()), this).exec();
    });
    connect(compareButton, &QPushButton::clicked, this, [this, table, runs, pickedRows] {
        const auto picked = pickedRows();
        if (picked.size() != 2) return;
        CompareDialog(runs.at(picked.first()), runs.at(picked.last()), this).exec();
    });
    connect(openFolder, &QPushButton::clicked, this, [root] { QDesktopServices::openUrl(QUrl::fromLocalFile(root)); });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
}
