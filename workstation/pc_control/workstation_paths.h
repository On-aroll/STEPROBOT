#pragma once

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

struct WorkstationPaths {
    QString dataRoot, studioRoot, simulationSeed;
};

inline WorkstationPaths workstationPaths(const QString& controlDir) {
    QFile file(QDir(controlDir).filePath("workstation.paths.json"));
    QJsonObject settings;
    if (file.open(QIODevice::ReadOnly) && file.size() < 65536) settings = QJsonDocument::fromJson(file.readAll()).object();
    const auto resolve = [&](const char* environment, const char* key, const QString& fallback = {}) {
        auto path = qEnvironmentVariable(environment);
        if (path.isEmpty()) path = settings.value(key).toString();
        if (path.isEmpty()) path = fallback;
        return path.isEmpty() ? QString() : QDir::cleanPath(QDir(controlDir).absoluteFilePath(path));
    };
    return {resolve("STEP_DATA_ROOT", "data_root", QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)),
            resolve("STEP_STUDIO_ROOT", "studio_root"), resolve("STEP_SIMULATION_SEED", "simulation_seed")};
}
