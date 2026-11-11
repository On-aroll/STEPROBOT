#pragma once

#include "workstation_motion.h"
#include <QMap>
#include <QString>
#include <optional>

struct WorkstationOrigin {
    QString device, model, tool, savedUtc;
    JointPose axes{}, xyzabc{};
};

QString originDeviceKey(bool simulated, const QString& model, const QString& identity, const QString& address);
std::optional<WorkstationOrigin> originFromFeedback(const QMap<QString, QString>& feedback, const QString& device, const QString& model);
QString saveWorkstationOrigin(const QString& path, const WorkstationOrigin& origin);
std::optional<WorkstationOrigin> readWorkstationOrigin(const QString& path, const QString& device);
