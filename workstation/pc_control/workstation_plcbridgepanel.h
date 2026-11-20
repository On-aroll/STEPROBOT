#pragma once
#include "workstation_plcbridge.h"
#include <QWidget>
#include <QMap>
class QLabel;
class QSpinBox;
class QComboBox;
class QPushButton;
class QPlainTextEdit;
class QTableWidget;
class QTabWidget;
class QGridLayout;
class QVBoxLayout;
class QBoxLayout;
class PlcBridgePanel : public QWidget {
    Q_OBJECT
public:
    explicit PlcBridgePanel(PlcBridgeClient& client, QWidget* parent = nullptr);
    void setModeEnabled(bool enabled);
    void setProgramSlot(int value);
    void showDiagnostics();
    QWidget* poseHost() const { return poseHost_; }
    QWidget* monitorHost() const { return monitorHost_; }
    void setMonitorVisible(bool visible);
    void setPrintHosts(QVBoxLayout* summary,QVBoxLayout* controls,QVBoxLayout* preparation);
    void setPrintRowHeight(int height);
protected:
    void resizeEvent(QResizeEvent* event) override;
signals:
    void programSlotChanged(int value);
private:
    void refresh();
    void send(BridgeProto::Command command);
    void exportDiagnostics();
    void layoutFlow(bool compact);
    void mountPrintControls();
    void restorePrintControls();
    PlcBridgeClient& client;
    bool modeEnabled = false;
    QSpinBox* slot;
    QComboBox* coordinates;
    QLabel *selectionInfo, *connection, *robot, *permissions, *receipt, *position, *traffic, *flags, *pendingLabel;
    QLabel *nextAction, *connectionBadge, *sessionBadge, *robotBadge, *enableBadge, *loadBadge;
    QLabel *registerHint, *loadHint, *runHint;
    QPlainTextEdit* events;
    QTabWidget* pages;
    QTableWidget *ioTable, *analog, *diagnostics, *coverage;
    QMap<int, QPushButton*> actions;
    QWidget *poseHost_=nullptr,*monitorHost_=nullptr;
    QGridLayout* workspace=nullptr;
    QGridLayout* flow_=nullptr;
    QList<QWidget*> flowCards_;
    QVBoxLayout* pageLayout_=nullptr;
    QWidget *summary_=nullptr,*controls_=nullptr;
    QLabel* heading_=nullptr;
    bool printWorkspace_=false;
    QWidget *printPreparation_=nullptr,*printRun_=nullptr;
    struct PrintReturn { QWidget* widget; QBoxLayout* layout; int index; };
    QList<PrintReturn> printReturns_;
    QList<QWidget*> printHidden_;
};
