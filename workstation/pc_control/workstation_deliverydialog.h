#pragma once

#include "workstation_delivery.h"
#include <QDialog>

class QLineEdit;
class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

class GcodeDeliveryDialog : public QDialog {
    Q_OBJECT
public:
    GcodeDeliveryDialog(const Job& nativeTemplate, const QString& endpoint, const QString& outputRoot,
        const QString& receiptRoot, QWidget* parent = nullptr, bool autoMode = false);
    void importSource(const QString& source);
    void importSources(const QStringList& sources);
    void importCurrentSource(const Job& current);
    void setDirectTransfer(bool direct, const QString& cabinetEndpoint = {});
    void startAutoUpload();
    bool hasSources() const { return autoMode && !sourcePaths.isEmpty(); }
    QString packagePath() const { return package.error.isEmpty() ? package.zip : QString(); }
    QJsonObject result() const { return transfer; }
    Job preview() const { return autoMode ? Delivery::autoPreview(autoJob, uploading ? QJsonObject{{"status", "UPLOADING"}} : transfer) : Job{}; }
signals:
    void previewChanged();
private:
    void rebuild();
    bool savePackage();
    void upload();
    void refresh();
    Job nativeTemplate, job;
    Delivery::AutoJob autoJob;
    QStringList sourcePaths;
    QList<Delivery::Source> sourceSnapshots;
    bool autoMode = false;
    Delivery::Package package;
    QJsonObject transfer;
    QString endpoint, outputRoot, receiptRoot, cabinetEndpoint;
    QLineEdit* path = nullptr;
    QDoubleSpinBox *rapid = nullptr, *tolerance = nullptr;
    QCheckBox *omit = nullptr, *includeReturn = nullptr, *autoUpload = nullptr;
    QComboBox* transport = nullptr;
    QLabel *status = nullptr, *heading = nullptr, *connectionHint = nullptr;
    QPlainTextEdit* report = nullptr;
    QPushButton *browse = nullptr, *save = nullptr, *send = nullptr, *folder = nullptr, *copy = nullptr;
    bool attempted = false;
    bool uploading = false;
};
