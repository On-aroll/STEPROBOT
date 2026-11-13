#pragma once

#include "workstation_job.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>

#include <optional>

std::optional<double> distanceToPlan(const Job& job, Point measured, int sourceRow);

class Experiment {
public:
    bool begin(const QString& root, const Job& job, const QString& label, bool simulation,
               const QString& model, const QString& overridePct, std::optional<Point> origin);
    void sample(const QMap<QString, QString>& feedback, int sourceRow, std::optional<Point> measured,
                std::optional<double> distance);
    void event(const QString& text);
    void setStreamOrigin(Point origin);
    bool finish(const QString& result, std::optional<double> endpointError = {});

    bool active() const { return recording; }
    bool healthy() const { return !writeFailed; }
    bool observedRunning() const { return observedRun; }
    qsizetype samples() const { return count; }
    qsizetype compared() const { return distanceCount; }
    double rmsDistance() const;
    double maxDistance() const { return maxDeviation; }
    QString folder() const { return directory; }
    QString result() const { return metadata.value("result").toString(); }

private:
    QString directory;
    QFile trace;
    QFile events;
    QElapsedTimer clock;
    QJsonObject metadata;
    bool recording = false;
    bool writeFailed = false;
    bool observedRun = false;
    qsizetype count = 0;
    qsizetype distanceCount = 0;
    double sumSquared = 0;
    double maxDeviation = 0;
    qint64 previousSample = -1;
    qint64 maxGap = 0;
    QString finalState;
};
