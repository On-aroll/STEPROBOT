#pragma once
#include "workstation_plcbridge.h"
#include "workstation_job.h"
#include <QtWidgets>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <array>
#include <optional>

using TwinJoints = std::array<double,6>;
struct TwinPacket {
    bool valid=false;
    bool nativeJoints=false,hasTcp=false;
    quint32 boot=0,counter=0;
    quint16 flags=0,age=0;
    TwinJoints joints{};
    QVector3D tcp;
    static TwinPacket decode(const QVector<quint16>& words);
};

// Separate FC03 reader: never registers a session, sends heartbeats or writes.
class TwinPlcReader : public QObject {
public:
    explicit TwinPlcReader(PlcBridgeClient& bridge,QObject* parent=nullptr);
    void setEnabled(bool enabled);
    void reconnect();
    bool fresh() const;
    qint64 ageMs() const;
    TwinPacket packet() const { return packet_; }
    bool legacySource() const { return legacy_; }
    std::function<void()> changed;
private:
    void reset();
    PlcBridgeClient& bridge;
    ModbusClient wire;
    QElapsedTimer receipt,advance;
    TwinPacket packet_;
    bool enabled_=false;
    bool legacy_=false;
};

struct TwinJoint {
    QVector3D pivot,axis;
    double offset=0,sign=1;
};
struct TwinRig {
    std::array<TwinJoint,6> joints;
    TwinJoints lower{-183,-60,-130,-220,-125,-360},upper{183,85,80,220,125,360};
    bool load(const QString& path,QString* error=nullptr);
    std::array<QMatrix4x4,7> transforms(const TwinJoints& angles) const;
    QMatrix4x4 flange(const TwinJoints& angles) const;
    std::optional<TwinJoints> inverse(const QMatrix4x4& target,const TwinJoints& seed) const;
};

// Relative WORLD path with a fixed starting orientation. No motion commands.
struct TwinTrajectory {
    QVector<QVector3D> relative,path;
    TwinJoints anchor{};
    std::optional<TwinJoints> goal;
    std::optional<QVector3D> anchorTcp,currentTcp;
    QMatrix4x4 target;
    bool anchored=false;
    void setPath(const QVector<Point>& points,const TwinRig& rig);
    void bind(const TwinJoints& pose,const TwinRig& rig,std::optional<QVector3D> tcp={});
    void clearAnchor(const TwinRig& rig);
    std::optional<double> endpointDistance() const;
private:
    void rebuild(const TwinRig& rig);
};

class TwinView : public QOpenGLWidget,protected QOpenGLFunctions {
public:
    explicit TwinView(QWidget* parent=nullptr);
    ~TwinView() override;
    bool loadModel(const QString& folder);
    void setPose(const TwinJoints& joints);
    void setView(int view);
    void setBackground(const QColor& color);
    void setTrajectory(const TwinTrajectory& value);
    void setGhostVisible(bool value) { ghostVisible=value;update(); }
    void setFrameScene(bool value);
    void setTrajectoryFocus(bool value);
    bool trajectoryFocus() const { return pathFocus; }
    bool trajectoryVisible() const { return sceneMode; }
    QVector<QPointF> projectedTrajectory() const;
    void setFeedbackState(const QString& text);
    void fitScene();
    TwinJoints currentPose() const { return pose; }
    const TwinTrajectory& trajectory() const { return plan; }
    QColor background() const { return background_; }
    void setStale(bool value) { if(stale==value)return;stale=value;update(); }
    QString error() const { return error_; }
    int vertexCount() const { return vertices.size()/9; }
    TwinRig rig;
protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width,int height) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
private:
    void release();
    bool focusedPath() const { return pathFocus && sceneMode && plan.path.size() > 1; }
    QMatrix4x4 viewProjection() const;
    void paintTrajectory(const QMatrix4x4& vp);
    struct Part {int link=0,first=0,count=0;};
    QList<Part> parts;
    QVector<float> vertices;
    QOpenGLBuffer buffer;
    QOpenGLShaderProgram shader;
    QOpenGLShaderProgram lineShader;
    TwinJoints pose{};
    TwinTrajectory plan;
    QString error_;
    QString feedbackLabel = "参考姿态 · 未读到轴角";
    QColor background_{"#e9f0f6"};
    QLabel *caption=nullptr,*help=nullptr,*legend=nullptr,*endpoint=nullptr,*message=nullptr;
    QPoint last;
    float yaw=-55,pitch=20,distance=6200;
    QVector3D cameraCenter{800,0,1200};
    bool sceneMode=false,ghostVisible=true;
    bool pathFocus=false;
    bool autoFit=true;
    bool stale=true,ready=false;
};

class TwinPanel : public QWidget {
public:
    explicit TwinPanel(PlcBridgeClient& bridge,const QString& modelFolder,QWidget* parent=nullptr);
    void setContext(bool plc,bool connected,bool sdkFresh);
    void acceptSdk(const QString& axes,bool simulated,const QString& xyz={});
    void setDemoPose(const TwinJoints& joints);
    TwinView* view() const { return canvas; }
    void setBackgroundPreset(int preset,const QColor& custom={});
    void setCompanionView(TwinView* view);
    void setJobPath(const QVector<Point>& points);
    void bindPreviewOrigin();
    QMenu* viewMenu() const { return settingsMenu; }
    QString previewStatus() const;
    QString snapshotSummary() const;
    bool canBindPreview() const;
    void setViewportHeightLimit(int height);
    void setPrintLayout(bool enabled);
    std::function<void()> previewChanged;
protected:
    void resizeEvent(QResizeEvent* event) override;
private:
    void refresh();
    void showPose(const TwinJoints& joints);
    void syncPreview();
    void resetPreview();
    void updateViewportSize();
    TwinPlcReader reader;
    TwinView* canvas;
    QPointer<TwinView> companion;
    QMenu* settingsMenu=nullptr;
    TwinTrajectory preview;
    std::optional<QVector3D> sdkTcp,lastTcp;
    bool poseAvailable=false,poseDemo=false;
    int viewportHeightLimit=470;
    bool printLayout_=false;
    QLabel *state,*source,*age,*captureTime,*detail;
    QList<QLabel*> values;
    QList<QDoubleSpinBox*> demoValues;
    QGroupBox* demoBox;
    QDialog* demoDialog;
    QAction* demoAction;
    QActionGroup* backgrounds;
    QPushButton* demo;
    QTimer timer;
    QElapsedTimer sdkAge;
    TwinJoints sdkPose{},lastPose{};
    quint32 seenBoot=0,seenCounter=0;
    quint64 sdkSerial=0,seenSdkSerial=0;
    bool plc_=false,connected_=false,sdkFresh_=false,sdkValid_=false,simulated_=false,demo_=false,hasPose=false;
};
