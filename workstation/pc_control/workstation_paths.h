#pragma once

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

struct WorkstationPaths {
    QString dataRoot;
};

inline WorkstationPaths workstationPaths(const QString& controlDir) {
    QFile file(QDir(controlDir).filePath("workstation.paths.json"));
    QJsonObject settings;
    if (file.open(QIODevice::ReadOnly) && file.size() < 65536) settings = QJsonDocument::fromJson(file.readAll()).object();
    auto path = qEnvironmentVariable("STEP_DATA_ROOT");
    if (path.isEmpty()) path = settings.value("data_root").toString();
    if (path.isEmpty()) path = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return {QDir::cleanPath(QDir(controlDir).absoluteFilePath(path))};
}
