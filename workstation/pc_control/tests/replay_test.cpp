#include "../workstation_replay.h"
#include "../workstation_plot.h"

#include <QtWidgets>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <iostream>

namespace {
int failures = 0;
void check(bool ok, const char* reason) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << reason << '\n'; }
}
void save(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "fixture saved");
}
QPushButton* button(QWidget& owner, const QString& name) {
    for (auto* candidate : owner.findChildren<QPushButton*>())
        if (candidate->text() == name) return candidate;
    return nullptr;
}
Plot* plotIn(QWidget& owner) {
    for (auto* widget : owner.findChildren<QWidget*>())
        if (auto* plot = dynamic_cast<Plot*>(widget)) return plot;
    return nullptr;
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTimer::singleShot(5000, &app, [&] { std::cerr << "FAIL: replay test timed out\n"; app.exit(1); });
    QTemporaryDir temp;
    check(temp.isValid(), "temporary archive created");
    for (int index = 0; index < 2; ++index) {
        const auto folder = temp.filePath(QString::number(index));
        QDir().mkpath(folder);
        QJsonObject metadata{{"label", "comma, quote\" label"}, {"mode", "simulation"},
            {"samples", 2}, {"result", "stopped"}, {"controller_project", "example.sr"}};
        if (!index) {
            metadata.insert("path_distance_rms_mm", 1.25);
            metadata.insert("path_distance_max_mm", 2.5);
            metadata.insert("endpoint_distance_mm", 0.5);
        }
        save(folder + "/run.json", QJsonDocument(metadata).toJson());
        save(folder + "/plan.csv", "point_index,source_line,x_offset_mm,y_offset_mm,z_offset_mm\n0,,0,0,0\n1,2,1,2,3\n2,3,4,5,6\n");
        QByteArray trace("host_utc,elapsed_s,sample,program_state,native_line,source_line,mode,servo,override_pct,message_id,tcp_mm_s,X_mm,Y_mm,Z_mm,A_deg,B_deg,C_deg,A1_deg,A2_deg,A3_deg,A4_deg,A5_deg,A6_deg,x_offset_mm,y_offset_mm,z_offset_mm,path_distance_mm\n");
        trace += "2026-11-11T00:00:00Z,0,1,0,42,2,2,1,20,0,5,1,2,3,0,0,0,1,2,3,4,5,6,1,2,3,0.1\n";
        trace += "2026-11-11T00:00:00Z,0.2,2,0,43,3,2,1,20,0,5,4,5,6,0,0,0,1,2,3,4,5,6,4,5,6,0.2\n";
        save(folder + "/trace.csv", trace);
    }
    const auto runs = scanRuns(temp.path());
    check(runs.size() == 2 && runs.first().samples == 2, "archives listed");
    check(std::abs(runningSeconds(runs.first().folder) - 0.2) < 1e-9, "trace duration read");
    const auto pauseFolder = temp.filePath("pause");
    QDir().mkpath(pauseFolder);
    save(pauseFolder + "/trace.csv", "utc,elapsed_s,sample,program_state\nu,0,1,0\nu,1,2,1\nu,11,3,0\nu,12,4,2\n");
    check(std::abs(runningSeconds(pauseFolder) - 2) < 1e-9, "pause duration excluded from running estimate");
    RunBrowserDialog browser(temp.path());
    auto* table = browser.findChild<QTableWidget*>();
    auto* replay = button(browser, "回放");
    auto* compare = button(browser, "对比");
    check(table && replay && compare, "history actions available");
    if (table && replay && compare) {
        table->selectRow(0);
        std::cerr << "Opening replay\n";
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            check(dialog != nullptr, "replay opens");
            if (!dialog) return;
            auto* plot = plotIn(*dialog);
            check(plot && plot->tcp && plot->measured.size() == 1, "first frame appears before playing");
            if (plot && !plot->measured.isEmpty()) {
                const auto p = plot->measured.first();
                check(p.x == 1 && p.y == 2 && p.z == 3, "measured XYZ retained");
                check(plot->completed == 0 && plot->active == 1, "source line drives yellow segment");
            }
            auto* play = button(*dialog, "播放");
            check(play != nullptr, "play action available");
            QTimer::singleShot(400, dialog, [&, dialog, plot] {
                check(plot && plot->measured.size() == 2 && plot->tcp && plot->tcp->z == 6, "playback advances to second frame");
                check(plot && plot->completed == 1 && plot->active == 2, "progress follows source rather than native line");
                dialog->accept();
            });
            if (play) play->click();
        });
        replay->click();
        std::cerr << "Replay closed; opening comparison\n";
        table->selectAll();
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            auto* comparison = dialog ? dialog->findChild<QTableWidget*>() : nullptr;
            check(comparison != nullptr, "comparison opens");
            int missing = 0;
            if (comparison)
                for (int row = 0; row < comparison->rowCount(); ++row)
                    for (int column = 1; column <= 2; ++column)
                        if (comparison->item(row, column)->text() == "未记录") ++missing;
            check(missing == 3, "missing metrics stay unavailable rather than becoming zero");
            if (dialog) dialog->accept();
        });
        compare->click();
    }
    std::cout << "replay_test: failures=" << failures << '\n';
    return failures ? 1 : 0;
}
