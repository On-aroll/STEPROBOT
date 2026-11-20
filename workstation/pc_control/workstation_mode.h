#pragma once
#include "workstation_plcbridge.h"
#include <QtWidgets>
#include <QProcess>
#include <functional>

class RobotModeDialog : public QDialog {
public:
    RobotModeDialog(PlcBridgeClient& bridge, std::function<bool()> idle, QWidget* parent=nullptr);
    ~RobotModeDialog() override;
    static int feedbackMode(const BridgeProto::Status& status);
private:
    void refresh();
    bool allowed(QString* why=nullptr) const;
    void begin();
    void finish(const QString& text);
    PlcBridgeClient& bridge;
    std::function<bool()> idle;
    QProcess process;
    QTimer timer;
    QElapsedTimer elapsed;
    QLineEdit* host;
    QComboBox* target;
    QLabel *current, *result;
    QPushButton* apply;
    QString nonce;
    quint32 boot=0,owner=0;
    int requested=0;
    bool busy=false,sdkConfirmed=false;
};
