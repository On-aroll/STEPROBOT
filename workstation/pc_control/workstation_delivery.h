#pragma once

#include "workstation_job.h"
#include <QJsonObject>
#include <array>

// Offline preparation only. No connection, motion command or GDI allocation here.
namespace Delivery {
struct Package {
    QString directory, zip, manifest, error;
};
struct Source {
    QString path;
    QByteArray bytes;
    int start = 1;
    QString error;
};
Source readSource(const QString& path);
Source currentSource(const Job& job);
Job compile(const QString& source, const Job& nativeTemplate, ConversionSettings settings = {}, bool includeReturn = true);
Package save(const Job& job, const QString& source, const QString& root);
QString record(const Package& package, const Job& job, const QJsonObject& transfer);
QString handoff(const Job& job, const QJsonObject& transfer);
QJsonObject manifest(const Job& job, const QString& source);
struct AutoJob {
    QString project = "pcjob_manual_r63.sr", error, revision;
    int number = 11;
    int patchLineOffset = 0;
    std::array<quint16,3> tag{};
    QStringList sources;
    QList<Job> parts;
    QMap<QString,QByteArray> files;
};
AutoJob compileAuto(const QStringList& sources, const Job& nativeTemplate, ConversionSettings settings = {});
AutoJob compileAutoSnapshots(const QList<Source>& sources, const Job& nativeTemplate, ConversionSettings settings = {});
Job autoPreview(const AutoJob& job, const QJsonObject& transfer = {});
Package saveAuto(const AutoJob& job, const QString& root);
QString autoHandoff(const AutoJob& job, const QJsonObject& transfer);
QString recordAuto(const Package& package, const AutoJob& job, const QJsonObject& transfer);
}
