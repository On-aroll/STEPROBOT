#include "workstation_home.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

QString originDeviceKey(bool simulated, const QString& model, const QString& identity, const QString& address) {
    return QString::fromLatin1(QCryptographicHash::hash((QString(simulated ? "virtual|" : "real|") + model + '|' + identity + '|' +
        (simulated ? QString("local") : address)).toUtf8(), QCryptographicHash::Sha256).toHex());
}

std::optional<WorkstationOrigin> originFromFeedback(const QMap<QString, QString>& feedback, const QString& device, const QString& model) {
    if (device.isEmpty() || model.isEmpty() || feedback.value("motion_rest") != "1" || feedback.value("toolname").isEmpty()) return {};
    const auto axes = feedback.value("axes").split(',');
    const auto pose = (feedback.value("xyz") + ',' + feedback.value("abc")).split(',');
    if (axes.size() != 6 || pose.size() != 6) return {};
    WorkstationOrigin origin{device, model, feedback.value("toolname"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)};
    for (int i = 0; i < 6; ++i) {
        bool axisOk = false, poseOk = false;
        origin.axes[i] = axes[i].toDouble(&axisOk);
        origin.xyzabc[i] = pose[i].toDouble(&poseOk);
        if (!axisOk || !poseOk || !std::isfinite(origin.axes[i]) || !std::isfinite(origin.xyzabc[i]) || std::abs(origin.axes[i]) > 10000 || std::abs(origin.xyzabc[i]) > 100000) return {};
    }
    return origin;
}

QString saveWorkstationOrigin(const QString& path, const WorkstationOrigin& origin) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return "无法创建上位机起点配置目录。";
    QJsonObject records;
    QFile existing(path);
    if (existing.exists()) {
        if (!existing.open(QIODevice::ReadOnly) || existing.size() > 1000000) return "上位机起点配置无法读取。";
        const auto document = QJsonDocument::fromJson(existing.readAll());
        if (!document.isObject() || document.object().value("schema").toInt() != 1) return "上位机起点配置格式无效，未覆盖原文件。";
        records = document.object().value("origins").toObject();
    }
    QJsonArray axes, pose;
    for (int i = 0; i < 6; ++i) { axes.append(origin.axes[i]); pose.append(origin.xyzabc[i]); }
    records.insert(origin.device, QJsonObject{{"model", origin.model}, {"tool", origin.tool}, {"saved_utc", origin.savedUtc},
        {"axes_deg", axes}, {"world_xyzabc", pose}});
    const auto bytes = QJsonDocument(QJsonObject{{"schema", 1}, {"origins", records}}).toJson();
    QSaveFile output(path);
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit() ? QString() : "上位机起点保存失败。";
}

std::optional<WorkstationOrigin> readWorkstationOrigin(const QString& path, const QString& device) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1000000) return {};
    const auto document = QJsonDocument::fromJson(file.readAll()).object();
    if (document.value("schema").toInt() != 1) return {};
    const auto record = document.value("origins").toObject().value(device).toObject();
    const auto axes = record.value("axes_deg").toArray(), pose = record.value("world_xyzabc").toArray();
    if (axes.size() != 6 || pose.size() != 6 || record.value("model").toString().isEmpty() || record.value("tool").toString().isEmpty()) return {};
    WorkstationOrigin origin{device, record.value("model").toString(), record.value("tool").toString(), record.value("saved_utc").toString()};
    for (int i = 0; i < 6; ++i) {
        if (!axes[i].isDouble() || !pose[i].isDouble()) return {};
        origin.axes[i] = axes[i].toDouble(); origin.xyzabc[i] = pose[i].toDouble();
        if (!std::isfinite(origin.axes[i]) || !std::isfinite(origin.xyzabc[i]) || std::abs(origin.axes[i]) > 10000 || std::abs(origin.xyzabc[i]) > 100000) return {};
    }
    return origin;
}
