#pragma once
#include "workstation_plcbridge.h"
#include <QtWidgets>
#include <array>

namespace WelderProto {
constexpr int CommandBase=7000, Heartbeat=7064, StatusBase=7200, CommandWords=32, StatusWords=64, ParameterCount=20;
constexpr quint16 Magic=0x5742, Version=1;
enum Operation : quint16 { Controls=1, Parameters=2, AllOff=3 };
struct Parameter { const char* name; const char* unit; int minimum,maximum,scale,initial; };
const std::array<Parameter,18>& parameters();
QVector<quint16> frame(Operation op,quint32 sequence,quint32 owner,quint32 boot,quint16 controls,const QVector<quint16>& parameters={});
QString errorText(int code);
}

class WelderClient : public QObject {
    Q_OBJECT
public:
    explicit WelderClient(PlcBridgeClient& bridge,QObject* parent=nullptr);
    void setEnabled(bool enabled);
    bool fresh() const;
    bool ready(QString* why=nullptr) const;
    bool safeIdle() const;
    bool pending() const { return pending_!=0; }
    const QVector<quint16>& status() const { return status_; }
    bool issue(WelderProto::Operation op,quint16 controls=0,const QVector<quint16>& parameters={});
signals:
    void changed();
    void notice(const QString& message);
private:
    void tick();
    void receive(int fc,int start,const QVector<quint16>& words,qint64);
    PlcBridgeClient& bridge;
    ModbusClient wire;
    QTimer timer;
    QElapsedTimer scanAge,packetAge,commandAge,heartbeatAge,retryAge;
    QVector<quint16> status_=QVector<quint16>(WelderProto::StatusWords,0);
    quint32 sequence_=0,pending_=0;
    quint16 heartbeat_=0;
    WelderProto::Operation operation_=WelderProto::AllOff;
    bool enabled_=false,valid_=false,advanced_=false,seenInstalled_=false,uncertain_=false;
};

class WelderPanel : public QWidget {
public:
    explicit WelderPanel(PlcBridgeClient& bridge,QWidget* parent=nullptr);
    WelderClient& client(){return welder;}
    void setModeEnabled(bool enabled){welder.setEnabled(enabled);}
    QWidget* poseHost() const { return poseHost_; }
    void setPrintHosts(QVBoxLayout* controls,QVBoxLayout* parameters,QVBoxLayout* logHost=nullptr);
    void setPrintRowHeight(int height);
    void setPrintParameterHeight(int height);
protected:
    void resizeEvent(QResizeEvent* event) override;
private:
    void refresh();
    void readParameters();
    void toggle(int bit);
    void layoutParameters(bool twoColumns);
    void layoutWorkspace();
    void setPrintLayout(bool enabled);
    PlcBridgeClient& bridge;
    WelderClient welder;
    QLabel *state,*actual,*fault,*output;
    QPlainTextEdit* log;
    QList<QDoubleSpinBox*> values;
    QComboBox *operation,*work;
    QCheckBox *hotAc,*liftArc,*airCooling;
    QPushButton *read,*apply,*off;
    QList<QPushButton*> controls;
    QWidget* poseHost_=nullptr;
    QGroupBox* parameterBox=nullptr;
    QGroupBox* controlsBox=nullptr;
    QVBoxLayout* pageLayout_=nullptr;
    QGridLayout *body=nullptr,*parameterGrid=nullptr;
    QScrollArea* parameterScroll=nullptr;
    QVBoxLayout *auxiliary_=nullptr,*actions_=nullptr;
    QLabel *parameterHint_=nullptr,*outputHint_=nullptr;
    QGridLayout* printOutputs_=nullptr;
    QGridLayout* printAuxiliary_=nullptr;
    int printParameterBaseHeight_=0,printEditorBaseHeight_=0;
    QList<QLabel*> parameterLabels;
    QList<QWidget*> parameterEditors;
    bool compactLayout=true;
    bool printWorkspace_=false;
    bool loaded=false;
};
