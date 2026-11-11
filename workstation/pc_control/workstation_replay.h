#pragma once

#include "workstation_job.h"

#include <QDialog>
#include <QJsonObject>
#include <QVector>

// 一次实验试次的 run.json 摘要：列表展示与两两对比都从这里取数。
struct RunSummary {
    QString folder, label, jobName, mode, result, requestedUtc, endedUtc;
    QString controllerProject, controllerProgram;
    int startLine = 0;
    int samples = 0;
    int distanceSamples = 0;
    double overridePct = 0;
    double rmsMm = 0;
    double maxMm = 0;
    double endpointMm = 0;
    bool hasDistance = false;
    bool hasEndpoint = false;
    bool complete = true;
    bool observedRunning = false;

    static RunSummary parse(const QString& folder, const QJsonObject& metadata);
};

// 扫描记录根目录下每个含 run.json 的试次文件夹，新的在前。
QVector<RunSummary> scanRuns(const QString& root);

// 按相邻回读间隔估计运行秒数，剔除暂停和停止期间。
double runningSeconds(const QString& folder);

// 历史试次浏览：列表、离线回放、两两对比的统一入口。
class RunBrowserDialog : public QDialog {
public:
    explicit RunBrowserDialog(const QString& root, QWidget* parent = nullptr);
};
