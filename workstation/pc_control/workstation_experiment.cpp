#include "workstation_experiment.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>

namespace {
QString utcNow() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }

QString sha256(const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }

bool save(const QString& path, const QByteArray& bytes) {
    QSaveFile output(path);
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
}

QString number(double value) { return QString::number(value, 'f', 6); }

QString joinCsv(const QStringList& row) {
    QStringList safe;
    safe.reserve(row.size());
    for (const auto& field : row) {
        if (field.contains(u'"') || field.contains(u',') || field.contains(u'\n') || field.contains(u'\r'))
            safe << u'"' + QString(field).replace(u'"', QLatin1String("\"\"")) + u'"';
        else
            safe << field;
    }
    return safe.join(u',');
}

QStringList xyz(Point point) { return {number(point.x), number(point.y), number(point.z)}; }

QStringList fields(const QString& text, int count) {
    const auto values = text.split(',');
    if (values.size() == count) return values;
    QStringList empty;
    for (int i = 0; i < count; ++i) empty << QString();
    return empty;
}

bool append(QFile& file, const QByteArray& bytes) { return file.write(bytes) == bytes.size() && file.flush(); }

double segmentDistance(Point p, Point a, Point b) {
    const Point d{b.x - a.x, b.y - a.y, b.z - a.z};
    const Point q{p.x - a.x, p.y - a.y, p.z - a.z};
    const double length2 = d.x * d.x + d.y * d.y + d.z * d.z;
    const double t = length2 > 0 ? std::clamp((q.x * d.x + q.y * d.y + q.z * d.z) / length2, 0., 1.) : 0.;
    return std::hypot(q.x - d.x * t, q.y - d.y * t, q.z - d.z * t);
}
}

std::optional<double> distanceToPlan(const Job& job, Point measured, int sourceRow) {
    int first = 0, last = static_cast<int>(job.points.size()) - 1;
    if (!job.nativeInput && !job.steps.isEmpty()) {
        const auto step = std::find_if(job.steps.cbegin(), job.steps.cend(), [sourceRow](const Step& s) {
            return s.row == sourceRow && s.lastPoint > s.firstPoint && !s.skipped;
        });
        if (step == job.steps.cend()) return {};
        first = step->firstPoint;
        last = step->lastPoint;
    }
    if (first >= last) return {};
    double closest = segmentDistance(measured, job.points[first], job.points[first + 1]);
    for (int i = first + 1; i < last; ++i) closest = std::min(closest, segmentDistance(measured, job.points[i], job.points[i + 1]));
    return closest;
}

bool Experiment::begin(const QString& root, const Job& job, const QString& label, bool simulation,
                       const QString& model, const QString& overridePct, std::optional<Point> origin) {
    if (recording || job.inputBytes.isEmpty() || !QDir().mkpath(root)) return false;
    const auto prefix = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    QTemporaryDir attempt(QDir(root).filePath(prefix + "-XXXXXX"));
    if (!attempt.isValid()) return false;
    const QDir run(attempt.path());
    const auto input = job.nativeInput ? "input.zip" : "input.gcode";
    if (!save(run.filePath(input), job.inputBytes) || !run.mkpath("program")) return false;
    if (job.nativeInput && !save(run.filePath(job.sourceIsGcode ? "display.gcode" : job.sourceFile.endsWith(".srd") ? "display.srd" : "display.srp"), (job.source.join('\n') + '\n').toUtf8())) return false;

    QJsonObject programs;
    for (auto file = job.files.cbegin(); file != job.files.cend(); ++file) {
        if (!save(run.filePath("program/" + file.key()), file.value())) return false;
        programs.insert(file.key(), sha256(file.value()));
    }
    QByteArray plan("point_index,source_line,x_offset_mm,y_offset_mm,z_offset_mm\n");
    QVector<int> pointRows(job.points.size(), 0);
    for (const auto& step : job.steps) {
        for (int i = step.firstPoint + 1; i <= step.lastPoint; ++i) pointRows[i] = step.row;
    }
    for (int i = 0; i < job.points.size(); ++i) {
        QStringList row{QString::number(i), pointRows[i] ? QString::number(pointRows[i]) : QString()};
        row << xyz(job.points[i]);
        plan += (row.join(',') + '\n').toUtf8();
    }
    if (!save(run.filePath("plan.csv"), plan)) return false;

    QJsonObject initial{
        {"schema", 1}, {"result", "in_progress"}, {"requested_utc", utcNow()}, {"label", label}, {"job", job.name},
        {"input", input}, {"input_sha256", sha256(job.inputBytes)}, {"local_program_sha256", programs},
        {"controller_project", job.project}, {"controller_program", job.program}, {"start_source_line", job.start},
        {"display_format", job.sourceIsGcode ? "gcode" : job.sourceFile.endsWith(".srd") ? "srd" : "srp"}, {"display_program", job.sourceProgram},
        {"mode", simulation ? "simulation" : "real"}, {"dry_run", true}, {"controller_model", model},
        {"initial_override_pct", overridePct.toDouble()}, {"time_base", "host_receive_time"},
        {"path_comparison", origin ? (job.nativeInput || job.steps.isEmpty() ? "nearest_planned_polyline" : "nearest_polyline_on_active_source_line") : "unavailable"}
    };
    if (job.nativeInput && job.sourceIsGcode) {
        QJsonArray mapping;
        for (const auto& step : job.steps) mapping.append(QJsonObject{{"source_line", step.row},
            {"srp_first_line", step.firstLine}, {"srp_last_line", step.lastLine}});
        initial.insert("display_source_map", mapping);
    }
    if (origin) initial.insert("simulation_origin_world_mm", QJsonArray{origin->x, origin->y, origin->z});
    if (!save(run.filePath("run.json"), QJsonDocument(initial).toJson())) return false;

    trace.setFileName(run.filePath("trace.csv"));
    events.setFileName(run.filePath("events.log"));
    if (!trace.open(QIODevice::WriteOnly) || !events.open(QIODevice::WriteOnly) ||
        !append(trace, "host_utc,elapsed_s,sample,program_state,native_line,source_line,mode,servo,override_pct,message_id,tcp_mm_s,"
                       "X_mm,Y_mm,Z_mm,A_deg,B_deg,C_deg,A1_deg,A2_deg,A3_deg,A4_deg,A5_deg,A6_deg,"
                       "x_offset_mm,y_offset_mm,z_offset_mm,path_distance_mm\n")) {
        trace.close();
        events.close();
        return false;
    }
    attempt.setAutoRemove(false);
    directory = run.path();
    metadata = initial;
    recording = true;
    writeFailed = observedRun = false;
    count = distanceCount = 0;
    sumSquared = maxDeviation = 0;
    previousSample = -1;
    maxGap = 0;
    finalState.clear();
    clock.start();
    return true;
}

void Experiment::setStreamOrigin(Point origin) {
    if (!recording) return;
    metadata.insert("origin_world_mm", QJsonArray{origin.x, origin.y, origin.z});
    if (metadata.value("mode") == "simulation")
        metadata.insert("simulation_origin_world_mm", QJsonArray{origin.x, origin.y, origin.z});
    metadata.insert("controller_project", "SDK.sr");
    metadata.insert("controller_program", "SDK");
    metadata.insert("execution", "sdk_stream");
    metadata.insert("path_comparison", "nearest_polyline_on_active_source_line");
    if (!save(QDir(directory).filePath("run.json"), QJsonDocument(metadata).toJson())) writeFailed = true;
}

void Experiment::sample(const QMap<QString, QString>& feedback, int sourceRow, std::optional<Point> measured,
                        std::optional<double> distance) {
    if (!recording) return;
    const qint64 now = clock.elapsed();
    finalState = feedback.value("state");
    if (!observedRun && finalState == "0") event("first_running_feedback");
    observedRun = observedRun || finalState == "0";
    if (previousSample >= 0) maxGap = std::max(maxGap, now - previousSample);
    previousSample = now;
    if (!trace.isOpen()) return;

    QStringList row{utcNow(), number(static_cast<double>(now) / 1000), QString::number(count + 1), finalState,
                    feedback.value("line"), sourceRow ? QString::number(sourceRow) : QString(),
                    feedback.value("mode"), feedback.value("enabled"), feedback.value("override"),
                    feedback.value("alarm"), feedback.value("speed")};
    row << fields(feedback.value("xyz"), 3) << fields(feedback.value("abc"), 3) << fields(feedback.value("axes"), 6);
    row << (measured ? xyz(*measured) : fields({}, 3));
    row << (distance ? number(*distance) : QString());
    if (!append(trace, (joinCsv(row) + '\n').toUtf8())) {
        writeFailed = true;
        trace.close();
        return;
    }
    ++count;
    if (distance) {
        ++distanceCount;
        sumSquared += *distance * *distance;
        maxDeviation = std::max(maxDeviation, *distance);
    }
}

void Experiment::event(const QString& text) {
    if (recording && events.isOpen() && !append(events, (utcNow() + "  " + text + '\n').toUtf8())) {
        writeFailed = true;
        events.close();
    }
}

bool Experiment::finish(const QString& result, std::optional<double> endpointError) {
    if (!recording) return false;
    event("run_result=" + result);
    recording = false;
    trace.close();
    events.close();
    metadata.insert("result", result);
    metadata.insert("ended_utc", utcNow());
    metadata.insert("observed_running", observedRun);
    metadata.insert("last_controller_state", finalState);
    metadata.insert("samples", count);
    metadata.insert("max_host_sample_gap_s", static_cast<double>(maxGap) / 1000);
    metadata.insert("distance_samples", distanceCount);
    metadata.insert("capture_incomplete", writeFailed);
    if (distanceCount) {
        metadata.insert("path_distance_rms_mm", rmsDistance());
        metadata.insert("path_distance_max_mm", maxDeviation);
    }
    if (endpointError) metadata.insert("endpoint_distance_mm", *endpointError);
    return save(QDir(directory).filePath("run.json"), QJsonDocument(metadata).toJson());
}

double Experiment::rmsDistance() const {
    return distanceCount ? std::sqrt(sumSquared / static_cast<double>(distanceCount)) : 0;
}
