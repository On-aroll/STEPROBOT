#pragma once

#include <QByteArray>
#include <QMap>
#include <QStringList>
#include <QVector>

struct Point {
    double x = 0;
    double y = 0;
    double z = 0;
};

struct Step {
    int row;
    int firstLine;
    int lastLine;
    int firstPoint;
    int lastPoint;
    double motionSeconds;
    double dwellSeconds;
    bool skipped = false;
};

struct ConversionSettings {
    double rapidMmS = 5;
    double arcToleranceMm = 0.05;
    bool omitProcessOutputs = false;
};

struct Job {
    QString name;
    QByteArray inputBytes;
    QString project;
    QString program;
    QString summary;
    QString error;
    QString mappingWarning;
    QString sourceProgram;
    QString sourceFile;
    QStringList source;
    QStringList mappedSource;
    QByteArray mappedBytes;
    QString mappedProgram;
    int mappedStart = 1;
    QVector<Step> mappedSteps;
    QMap<int, QString> diagnostics;
    QMap<QString, QByteArray> files;
    QVector<Point> points;
    QVector<Step> steps;
    int start = 1;
    bool reviewed = false;
    bool nativeInput = false;
    bool sourceIsGcode = true;
    bool modified = false;
    bool localEdits = false;
    bool trustedGcode = false;
    ConversionSettings conversion;

    bool runnable() const { return error.isEmpty() && (nativeInput ? reviewed || trustedGcode : !steps.isEmpty()); }
};

Job readNativeJob(const QString& path);
void showNativeSource(Job& job, const QString& program);
void showNativeFile(Job& job, const QString& name);
QString updateNativeFile(Job& job, const QString& name, const QByteArray& bytes);
void showMappedSource(Job& job);
QString associateNativeGcode(Job& job, const QString& mappingPath);
bool restoreMappedPreview(Job& job, const Job& nativeTemplate);
int sourceRowForNativeLine(const Job& job, const QString& project, const QString& program, int nativeLine);
Job readGcodeJob(const QString& path, const Job& nativeTemplate, ConversionSettings settings = {});
Job compileGcodeJob(QString name, const QStringList& source, int start, const Job& nativeTemplate, ConversionSettings settings = {});
QString exportStepJob(const Job& job, const QString& zipPath);
QString conversionReport(const Job& job);
