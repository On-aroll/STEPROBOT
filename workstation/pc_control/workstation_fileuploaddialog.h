#pragma once

#include "workstation_filebridge.h"
#include <QDialog>

class QLabel;
class QLineEdit;
class QTableWidget;
class QPushButton;
class QProgressBar;
class QAction;

class FileUploadDialog : public QDialog {
    Q_OBJECT
public:
    FileUploadDialog(const QMap<QString,QByteArray>& files, const QString& source,
        const QString& endpoint, const QString& receiptDirectory, QWidget* parent = nullptr);
    QJsonObject result() const { return lastResult; }
    void beginUpload() { start(false); }
    void useAutoTarget(const QString& project, quint16 number, std::array<quint16,3> tag);
    void setDirectMode(bool direct, const QString& cabinetEndpoint = {});
protected:
    void reject() override;
    void closeEvent(QCloseEvent* event) override;
private:
    void start(bool probe);
    void updateProgress(const QJsonObject& event);
    void updateControls();
    FileBridge::Options options;
    QString receiptDirectory, validation;
    QJsonObject lastResult;
    FileBridge::Transfer* worker = nullptr;
    QLineEdit* address = nullptr;
    QLineEdit *cabinetAddress = nullptr, *ftpUser = nullptr, *ftpPassword = nullptr;
    QAction* passwordVisibility = nullptr;
    QWidget* directFields = nullptr;
    QLabel *hint = nullptr, *status = nullptr, *destination = nullptr, *receiptLabel = nullptr, *addressLabel = nullptr;
    QTableWidget* table = nullptr;
    QProgressBar* bar = nullptr;
    QPushButton *probeButton = nullptr, *uploadButton = nullptr, *closeButton = nullptr, *receiptButton = nullptr;
    bool attempted = false, cancelling = false;
};
