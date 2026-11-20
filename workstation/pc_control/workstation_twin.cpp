#include "workstation_settings.h"
#include "workstation_twin.h"
#include "workstation_ui.h"
#include <QtEndian>
#include <cmath>
#include <cstring>

TwinPacket TwinPacket::decode(const QVector<quint16>& w) {
    TwinPacket p;
    if(w.size()==40&&w[0]==0x4A52) {
        if(w[1]!=1||w[36]!=0x0641||w[37]!=6||w[38]!=1||w[39]!=40||w[6]!=w[32]||w[7]!=w[33])return p;
        p.nativeJoints=true;p.boot=(quint32(w[4])<<16)|w[5];p.counter=(quint32(w[6])<<16)|w[7];p.flags=w[2];p.age=w[8];
        if(!p.boot||!p.counter)return {};
        for(int i=0;i<6;++i){
            const quint32 bits=(quint32(w[11+2*i])<<16)|w[10+2*i];
            float value=0;std::memcpy(&value,&bits,sizeof(value));
            if(!std::isfinite(value)||std::abs(value)>10000)return {};
            p.joints[i]=value;
        }
        p.valid=true;return p;
    }
    if(w.size()!=40||w[0]!=0x5053||w[1]!=1||w[36]!=0x0603||w[37]!=12||w[38]!=1000)return p;
    const auto u32=[&w](int i){return (quint32(w[i])<<16)|w[i+1];};
    p.boot=u32(4);p.counter=u32(6);p.flags=w[2];p.age=w[8];
    if(!p.boot||!p.counter||w[3]<1||w[3]>99)return {};
    // Validate the complete published frame; TCP XYZ is used only for snapshot comparison.
    for(int i=0;i<12;++i){
        qint64 n=u32(10+2*i);if(n>=0x80000000LL)n-=0x100000000LL;
        if(std::abs(n)>10000000LL)return {};
        if(i<6)p.joints[i]=double(n)/1000.;
        else if(i<9)p.tcp[i-6]=float(double(n)/1000.);
    }
    p.hasTcp=true;p.valid=true;return p;
}
TwinPlcReader::TwinPlcReader(PlcBridgeClient& b,QObject* parent):QObject(parent),bridge(b) {
    wire.setPollRange(3,500,40);wire.setPollInterval(100);
    connect(&bridge,&PlcBridgeClient::linkChanged,this,[this](bool up,const QString&){
        reset();legacy_=false;wire.setPollRange(3,500,40);if(up)reconnect();else wire.disconnectFrom();if(changed)changed();
    });
    connect(&wire,&ModbusClient::registersRead,this,[this](int fc,int start,const QVector<quint16>& words,qint64){
        if(fc!=3||start!=(legacy_?450:500))return;
        if(!legacy_&&(words.size()!=40||words[0]!=0x4A52||words[1]!=1)){
            wire.disconnectFrom();reset();legacy_=true;wire.setPollRange(3,450,40);
            QTimer::singleShot(0,this,[this]{reconnect();});if(changed)changed();return;
        }
        const auto next=TwinPacket::decode(words);
        if(next.valid&&(!packet_.valid||packet_.boot!=next.boot||packet_.counter!=next.counter))advance.restart();
        receipt.restart();packet_=next;if(changed)changed();
    });
    connect(&wire,&ModbusClient::failure,this,[this](const QString&){
        const bool fallback=enabled_&&!legacy_&&bridge.isConnected();
        wire.disconnectFrom();reset();
        if(fallback){legacy_=true;wire.setPollRange(3,450,40);QTimer::singleShot(0,this,[this]{reconnect();});}
        if(changed)changed();
    });
    connect(&wire,&ModbusClient::linkChanged,this,[this](bool up,const QString&){if(!up)reset();if(changed)changed();});
}
void TwinPlcReader::reset(){packet_={};receipt.invalidate();advance.invalidate();}
void TwinPlcReader::setEnabled(bool e){if(e==enabled_)return;enabled_=e;reset();legacy_=false;wire.setPollRange(3,500,40);if(e)reconnect();else wire.disconnectFrom();}
void TwinPlcReader::reconnect(){if(enabled_&&bridge.isConnected()&&!wire.isActive())wire.connectTo(bridge.endpointHost(),bridge.endpointPort(),bridge.endpointUnit());}
qint64 TwinPlcReader::ageMs() const {return packet_.valid&&receipt.isValid()?qint64(packet_.age)+receipt.elapsed():-1;}
bool TwinPlcReader::fresh() const {
    return enabled_&&wire.isConnected()&&bridge.fresh()&&bridge.status().has(BridgeProto::RobotOnline)&&
        packet_.valid&&packet_.boot==bridge.status().boot&&
        (packet_.flags&(packet_.nativeJoints?9:13))==(packet_.nativeJoints?9:13)&&
        receipt.isValid()&&receipt.elapsed()<1000&&advance.isValid()&&
        advance.elapsed()<(packet_.nativeJoints?1500:8000)&&ageMs()<(packet_.nativeJoints?1500:8000);
}

bool TwinRig::load(const QString& path,QString* error) {
    QFile file(path);if(!file.open(QIODevice::ReadOnly)){if(error)*error="找不到关节配置";return false;}
    const auto root=QJsonDocument::fromJson(file.readAll()).object();const auto rows=root["joints"].toArray();
    if(rows.size()!=6){if(error)*error="关节配置必须包含六个轴";return false;}
    std::array<TwinJoint,6> parsed;
    const auto vector=[](const QJsonValue& v,QVector3D& out){
        const auto a=v.toArray();if(a.size()!=3)return false;
        for(const auto& n:a)if(!n.isDouble()||!std::isfinite(n.toDouble()))return false;
        out=QVector3D(a[0].toDouble(),a[1].toDouble(),a[2].toDouble());return true;
    };
    for(int i=0;i<6;++i){auto o=rows[i].toObject();auto& j=parsed[i];
        if(!vector(o["pivot"],j.pivot)||!vector(o["axis"],j.axis)||j.axis.length()<.99f){if(error)*error="关节轴心或方向无效";return false;}
        j.axis.normalize();j.offset=o["offset_deg"].toDouble(0);j.sign=o["sign"].toDouble(1);
        if(!std::isfinite(j.offset)||(j.sign!=1&&j.sign!=-1)){if(error)*error="关节零位或符号无效";return false;}
    }
    joints=parsed;return true;
}
std::array<QMatrix4x4,7> TwinRig::transforms(const TwinJoints& angles) const {
    std::array<QMatrix4x4,7> result;
    for(int i=0;i<6;++i){
        QMatrix4x4 turn;turn.translate(joints[i].pivot);
        turn.rotate(float(joints[i].sign*(angles[i]-joints[i].offset)),joints[i].axis);
        turn.translate(-joints[i].pivot);result[i+1]=result[i]*turn;
    }return result;
}
QMatrix4x4 TwinRig::flange(const TwinJoints& angles) const {
    auto matrix=transforms(angles)[6];matrix.translate(2066,0,2297);return matrix;
}
namespace {
QVector3D rotationError(const QMatrix4x4& target,const QMatrix4x4& actual){
    QQuaternion q=QQuaternion::fromRotationMatrix(target.normalMatrix())*QQuaternion::fromRotationMatrix(actual.normalMatrix()).conjugated();
    q.normalize();if(q.scalar()<0)q=-q;const auto v=q.vector();const auto length=v.length();
    return length<1e-8f?2*v:v*(2*std::atan2(length,q.scalar())/length);
}
bool solve6(double a[6][7],double* result){
    for(int column=0;column<6;++column){int pivot=column;for(int row=column+1;row<6;++row)if(std::abs(a[row][column])>std::abs(a[pivot][column]))pivot=row;
        if(std::abs(a[pivot][column])<1e-12)return false;
        for(int k=column;k<7;++k)std::swap(a[column][k],a[pivot][k]);const double divisor=a[column][column];
        for(int k=column;k<7;++k)a[column][k]/=divisor;
        for(int row=0;row<6;++row)if(row!=column){const auto factor=a[row][column];for(int k=column;k<7;++k)a[row][k]-=factor*a[column][k];}
    }for(int i=0;i<6;++i)result[i]=a[i][6];return true;
}
}
std::optional<TwinJoints> TwinRig::inverse(const QMatrix4x4& target,const TwinJoints& seed) const {
    for(int row=0;row<4;++row)for(int col=0;col<4;++col)if(!std::isfinite(target(row,col)))return {};
    TwinJoints pose=seed;for(int i=0;i<6;++i){if(!std::isfinite(pose[i])||pose[i]<lower[i]||pose[i]>upper[i])return {};}
    constexpr double orientationScale=300,radToDeg=57.29577951308232;
    const auto loss=[&](const QMatrix4x4& matrix){return double((target.column(3).toVector3D()-matrix.column(3).toVector3D()).lengthSquared())+orientationScale*orientationScale*rotationError(target,matrix).lengthSquared();};
    for(int iteration=0;iteration<180;++iteration){
        const auto frames=transforms(pose);const auto matrix=flange(pose);const auto point=matrix.column(3).toVector3D();
        const auto dp=target.column(3).toVector3D()-point,dr=rotationError(target,matrix);
        if(dp.length()<.15f&&dr.length()<.0009f)return pose;
        double j[6][6]{},error[6]{dp.x(),dp.y(),dp.z(),dr.x()*orientationScale,dr.y()*orientationScale,dr.z()*orientationScale};
        for(int axis=0;axis<6;++axis){auto direction=frames[axis].mapVector(joints[axis].axis)*float(joints[axis].sign);auto center=frames[axis].map(joints[axis].pivot);
            const auto tangent=QVector3D::crossProduct(direction,point-center);
            for(int k=0;k<3;++k){j[k][axis]=tangent[k];j[k+3][axis]=direction[k]*orientationScale;}
        }
        bool improved=false;const auto oldLoss=loss(matrix);
        for(double damping:{2.,10.,50.,150.}){
            double equations[6][7]{},step[6]{};
            for(int r=0;r<6;++r){for(int c=0;c<6;++c){for(int k=0;k<6;++k)equations[r][c]+=j[k][r]*j[k][c];if(r==c)equations[r][c]+=damping*damping;}
                for(int k=0;k<6;++k)equations[r][6]+=j[k][r]*error[k];}
            if(!solve6(equations,step))continue;
            double peak=0;for(double x:step)peak=std::max(peak,std::abs(x*radToDeg));const double limit=peak>8?8/peak:1;
            for(double scale:{1.,.5,.25,.125}){auto candidate=pose;for(int k=0;k<6;++k)candidate[k]=std::clamp(candidate[k]+step[k]*radToDeg*limit*scale,lower[k],upper[k]);
                if(loss(flange(candidate))<oldLoss-1e-6){pose=candidate;improved=true;break;}}
            if(improved)break;
        }
        if(!improved)return {};
    }return {};
}
void TwinTrajectory::setPath(const QVector<Point>& points,const TwinRig& rig){
    relative.clear();for(const auto& point:points){
        if(!std::isfinite(point.x)||!std::isfinite(point.y)||!std::isfinite(point.z)||std::abs(point.x)>10000||std::abs(point.y)>10000||std::abs(point.z)>10000){relative.clear();break;}
        relative<<QVector3D(point.x,point.y,point.z);
    }
    clearAnchor(rig);
}
void TwinTrajectory::bind(const TwinJoints& pose,const TwinRig& rig,std::optional<QVector3D> tcp){anchor=pose;anchorTcp=tcp;currentTcp=tcp;anchored=true;rebuild(rig);}
void TwinTrajectory::clearAnchor(const TwinRig& rig){anchor={};anchored=false;anchorTcp.reset();currentTcp.reset();rebuild(rig);}
void TwinTrajectory::rebuild(const TwinRig& rig){
    path.clear();goal.reset();if(relative.size()<2)return;
    target=rig.flange(anchor);const auto start=target.column(3).toVector3D();for(const auto& offset:relative)path<<start+offset;
    target.setColumn(3,QVector4D(start+relative.last(),1));goal=rig.inverse(target,anchor);
}
std::optional<double> TwinTrajectory::endpointDistance() const {if(!anchored||!anchorTcp||!currentTcp||relative.isEmpty())return {};return double((*currentTcp-*anchorTcp-relative.last()).length());}

TwinView::TwinView(QWidget* parent):QOpenGLWidget(parent),buffer(QOpenGLBuffer::VertexBuffer) {
    setObjectName("twinCanvas");setMinimumSize(200,190);setMouseTracking(false);
    QSurfaceFormat format;format.setDepthBufferSize(24);format.setSamples(4);format.setVersion(2,1);setFormat(format);
    auto* overlay=new QVBoxLayout(this);overlay->setContentsMargins(10,8,10,8);
    auto* top=new QHBoxLayout;caption=new QLabel("SR175 / 3200");caption->setAttribute(Qt::WA_TransparentForMouseEvents);top->addWidget(caption,0,Qt::AlignTop);top->addStretch();
    legend=new QLabel("<span style='color:#cf343e'>■</span> 实体　当前姿态快照<br><span style='color:#e99b14'>━</span> 亮线　作业规划轨迹<br><span style='color:#19b6d0'>□</span> 虚影　理想结束姿态");legend->setObjectName("twinSceneLegend");legend->setAttribute(Qt::WA_TransparentForMouseEvents);legend->hide();top->addWidget(legend,0,Qt::AlignTop);overlay->addLayout(top);
    message=new QLabel;message->setWordWrap(true);message->setAttribute(Qt::WA_TransparentForMouseEvents);overlay->addWidget(message,0,Qt::AlignLeft);overlay->addStretch();
    endpoint=new QLabel("理想终点",this);endpoint->setAttribute(Qt::WA_TransparentForMouseEvents);endpoint->resize(110,24);endpoint->hide();
    help=new QLabel("拖动旋转 · 滚轮缩放");help->setAttribute(Qt::WA_TransparentForMouseEvents);overlay->addWidget(help,0,Qt::AlignLeft);
    setBackground(background_);
}
TwinView::~TwinView(){if(context()){disconnect(context(),nullptr,this,nullptr);makeCurrent();release();doneCurrent();}}
void TwinView::release(){buffer.destroy();shader.removeAllShaders();lineShader.removeAllShaders();ready=false;}
bool TwinView::loadModel(const QString& folder) {
    if(!rig.load(folder+"/sr175_rig.json",&error_))return false;
    QFile json(folder+"/sr175_model.json"),mesh(folder+"/sr175.mesh");
    if(!json.open(QIODevice::ReadOnly)||!mesh.open(QIODevice::ReadOnly)){error_="三维模型文件缺失";return false;}
    const auto data=mesh.readAll();if(data.isEmpty()||data.size()%36||data.size()>150000000){error_="三维网格格式无效";return false;}
    vertices.resize(data.size()/4);
    for(qsizetype i=0;i<vertices.size();++i){quint32 bits=qFromLittleEndian<quint32>(data.constData()+4*i);std::memcpy(&vertices[i],&bits,4);if(!std::isfinite(vertices[i])){error_="三维网格包含无效坐标";return false;}}
    for(const auto& value:QJsonDocument::fromJson(json.readAll()).object()["parts"].toArray()){
        auto o=value.toObject();Part p{o["link"].toInt(-1),o["first"].toInt(-1),o["count"].toInt(-1)};
        if(p.link<0||p.link>6||p.first<0||p.count<=0||qint64(p.first)+p.count>vertexCount()){error_="模型零件索引无效";return false;}
        parts<<p;
    }
    if(parts.isEmpty()){error_="模型没有零件";return false;}error_.clear();fitScene();return true;
}
void TwinView::setPose(const TwinJoints& joints){for(double v:joints)if(!std::isfinite(v))return;if(pose==joints)return;pose=joints;if(autoFit&&!sceneMode)fitScene();update();}
void TwinView::setView(int v){yaw=v==1?0:v==2?-90:-55;pitch=v==3?89:v==1||v==2?0:20;fitScene();update();}
void TwinView::setTrajectory(const TwinTrajectory& value){const bool changed=plan.path!=value.path;plan=value;if(changed){setFrameScene(sceneMode);if(autoFit&&focusedPath())fitScene();}update();}
void TwinView::setFrameScene(bool value){const bool changed=sceneMode!=value;sceneMode=value;legend->setVisible(value&&!focusedPath());caption->setText(focusedPath()?"G 代码轨迹细节 · mm":value?"SR175 / 3200 · "+feedbackLabel:"SR175 / 3200");help->setText(focusedPath()?"拖动旋转 · 滚轮缩放  |  等比例显示本次规划轨迹":value?"拖动旋转 · 滚轮缩放  |  实际姿态以有效轴角反馈为准":"拖动旋转 · 滚轮缩放");if(changed&&pathFocus)fitScene();update();}
void TwinView::setTrajectoryFocus(bool value){if(pathFocus==value)return;pathFocus=value;if(value){yaw=-55;pitch=30;}setFrameScene(sceneMode);fitScene();}
void TwinView::setFeedbackState(const QString& text){if(feedbackLabel==text)return;feedbackLabel=text;setFrameScene(sceneMode);setBackground(background_);}
void TwinView::fitScene(){
    autoFit=true;QVector<QVector3D> samples;
    const auto extend=[&](QVector3D p){samples<<p;};
    const auto includeRobot=[&](const TwinJoints& q){const auto frames=rig.transforms(q);
        for(const auto& part:parts)for(int i=part.first;i<part.first+part.count;i+=30)extend(frames[part.link].map(QVector3D(vertices[i*9],vertices[i*9+1],vertices[i*9+2])));
    };
    if(!focusedPath()){includeRobot(pose);if(sceneMode&&plan.goal)includeRobot(*plan.goal);}
    if(sceneMode)for(const auto& p:plan.path)extend(p);if(samples.isEmpty())return;
    const auto rad=[](float v){return v*3.14159265f/180;};
    const QVector3D toward(std::cos(rad(pitch))*std::cos(rad(yaw)),std::cos(rad(pitch))*std::sin(rad(yaw)),std::sin(rad(pitch)));
    const auto right=QVector3D::crossProduct(QVector3D(0,0,1),toward).normalized(),up=QVector3D::crossProduct(toward,right).normalized();
    QVector3D lo(1e9,1e9,1e9),hi(-1e9,-1e9,-1e9);
    for(const auto& p:samples){const QVector3D v(QVector3D::dotProduct(p,right),QVector3D::dotProduct(p,up),QVector3D::dotProduct(p,toward));for(int k=0;k<3;++k){lo[k]=std::min(lo[k],v[k]);hi[k]=std::max(hi[k],v[k]);}}
    const auto center=(lo+hi)/2;cameraCenter=right*center.x()+up*center.y()+toward*center.z();
    const float tanY=std::tan(rad(18.5f)),tanX=tanY*float(std::max(1,width()))/std::max(1,height());distance=focusedPath()?1:800;
    const float fill=focusedPath()?(height()<330?.59f:.69f):sceneMode?.80f:.88f;
    for(const auto& p:samples){const auto delta=p-cameraCenter;const float need=std::max(std::abs(QVector3D::dotProduct(delta,right))/tanX,std::abs(QVector3D::dotProduct(delta,up))/tanY)/fill;
        distance=std::max(distance,QVector3D::dotProduct(delta,toward)+need);}
    distance=std::min(distance,30000.f);update();
}
void TwinView::resizeGL(int,int){if(autoFit)fitScene();}
void TwinView::setBackground(const QColor& color){
    if(!color.isValid())return;background_=color;
    const bool light=color.lightnessF()>.48;
    for(auto* label:{caption,help})if(label)label->setStyleSheet(QString("color:%1;background:transparent;font-size:9pt;").arg(light?"#536b80":"#c6d4e4"));
    if(legend){legend->setStyleSheet(QString("QLabel { background:%1;padding:7px 10px;border-radius:5px;font-size:9pt; }").arg(light?"rgba(255,255,255,210)":"rgba(24,34,47,210)"));
        legend->setText(QString("<span style='color:%1'><span style='color:#cf343e'>■</span> 模型　%2<br><span style='color:#e99b14'>━</span> 亮线　作业规划轨迹<br><span style='color:#19b6d0'>□</span> 虚影　理想结束姿态</span>").arg(light?"#385367":"#d3e3ec",feedbackLabel));}
    for(auto* label:{endpoint,message})if(label)label->setStyleSheet(QString("color:%1;background:transparent;font-size:9pt;").arg(light?"#385367":"#d3e3ec"));
    update();
}
void TwinView::initializeGL(){
    initializeOpenGLFunctions();
    connect(context(),&QOpenGLContext::aboutToBeDestroyed,this,[this]{makeCurrent();release();doneCurrent();},Qt::DirectConnection);
    const char* vs="attribute vec3 position;attribute vec3 normal;attribute vec3 color;uniform mat4 mvp;uniform mat4 model;varying vec3 n;varying vec3 rgb;void main(){gl_Position=mvp*vec4(position,1.0);n=mat3(model)*normal;rgb=color;}";
    const char* fs="varying vec3 n;varying vec3 rgb;uniform float stale;uniform float ghost;void main(){vec3 N=normalize(n);float light=0.40+0.50*max(dot(N,normalize(vec3(0.3,-0.5,1.0))),0.0)+0.10*abs(dot(N,normalize(vec3(-1.0,0.2,0.3))));vec3 tint=mix(rgb,rgb*0.55+vec3(0.17,0.19,0.21),stale);gl_FragColor=vec4(mix(tint*light,vec3(0.02,0.72,0.87)*(0.65+0.35*light),ghost),mix(1.0,0.28,ghost));}";
    if(!shader.addShaderFromSourceCode(QOpenGLShader::Vertex,vs)||!shader.addShaderFromSourceCode(QOpenGLShader::Fragment,fs)||!shader.link()){error_="显卡着色器初始化失败："+shader.log();return;}
    if(!lineShader.addShaderFromSourceCode(QOpenGLShader::Vertex,"attribute vec3 position;uniform mat4 mvp;void main(){gl_Position=mvp*vec4(position,1.0);}")||
       !lineShader.addShaderFromSourceCode(QOpenGLShader::Fragment,"uniform vec4 tint;void main(){gl_FragColor=tint;}")||!lineShader.link()){error_="轨迹着色器初始化失败";return;}
    buffer.create();buffer.bind();buffer.allocate(vertices.constData(),int(vertices.size()*4));buffer.release();ready=true;
}
QMatrix4x4 TwinView::viewProjection() const {
    QMatrix4x4 projection,view;
    const float nearPlane=focusedPath()?std::max(.001f,distance*.001f):10;
    projection.perspective(37,float(width())/std::max(1,height()),nearPlane,50000);
    const auto rad=[](float x){return x*3.14159265f/180;};
    const QVector3D toward(std::cos(rad(pitch))*std::cos(rad(yaw)),std::cos(rad(pitch))*std::sin(rad(yaw)),std::sin(rad(pitch)));
    view.lookAt(cameraCenter+distance*toward,cameraCenter,QVector3D(0,0,1));
    return projection*view;
}
QVector<QPointF> TwinView::projectedTrajectory() const {
    QVector<QPointF> points;const auto vp=viewProjection();
    for(const auto& point:plan.path){const auto clip=vp*QVector4D(point,1);
        if(clip.w()<=0){points<<QPointF(qQNaN(),qQNaN());continue;}
        const auto n=clip.toVector3DAffine();points<<QPointF((n.x()+1)*width()/2,(1-n.y())*height()/2);
    }return points;
}
void TwinView::paintTrajectory(const QMatrix4x4& vp){
    if(!sceneMode||plan.path.size()<2)return;
    // Screen-space triangle ribbons retain their width on OpenGL drivers with 1px-only lines.
    lineShader.bind();const auto attribute=lineShader.attributeLocation("position");lineShader.enableAttributeArray(attribute);
    const auto screen=[&](QVector3D point){const auto clip=vp*QVector4D(point,1);const auto n=clip.toVector3DAffine();return QPointF((n.x()+1)*width()/2,(1-n.y())*height()/2);};
    QVector3D lo=plan.path.first(),hi=lo;
    for(const auto& point:plan.path)for(int axis=0;axis<3;++axis){lo[axis]=std::min(lo[axis],point[axis]);hi[axis]=std::max(hi[axis],point[axis]);}
    const auto span=hi-lo;
    if(focusedPath()){
        const double extent=std::max({double(span.x()),double(span.y()),double(span.z()),.001});
        double grid=std::pow(10.,std::floor(std::log10(extent/5)));if(extent/grid>10)grid*=5;
        QVector<QVector3D> lines;
        for(double x=std::ceil(lo.x()/grid)*grid;x<=hi.x()+grid*.001;x+=grid)lines<<QVector3D(float(x),lo.y(),lo.z())<<QVector3D(float(x),hi.y(),lo.z());
        for(double y=std::ceil(lo.y()/grid)*grid;y<=hi.y()+grid*.001;y+=grid)lines<<QVector3D(lo.x(),float(y),lo.z())<<QVector3D(hi.x(),float(y),lo.z());
        lineShader.setUniformValue("mvp",vp);lineShader.setAttributeArray(attribute,lines.constData());lineShader.setUniformValue("tint",background_.lightnessF()>.48?QVector4D(.78,.83,.88,1):QVector4D(.25,.33,.42,1));glLineWidth(1);glDrawArrays(GL_LINES,0,lines.size());
    }
    QMatrix4x4 screenMatrix;lineShader.setUniformValue("mvp",screenMatrix);
    const auto vertex=[&](QPointF point){return QVector3D(float(point.x()*2/width()-1),float(1-point.y()*2/height()),0);};
    const auto draw=[&](const QVector<QPointF>& points,float thickness,QVector4D color,bool dots){
        QVector<QVector3D> triangles;
        for(int i=1;i<points.size();++i){const auto a=points[i-1],b=points[i];if(!std::isfinite(a.x())||!std::isfinite(a.y())||!std::isfinite(b.x())||!std::isfinite(b.y()))continue;
            const auto delta=b-a;const double length=std::hypot(delta.x(),delta.y());if(length<.001)continue;
            const QPointF n(-delta.y()/length*thickness*.5,delta.x()/length*thickness*.5);
            triangles<<vertex(a+n)<<vertex(a-n)<<vertex(b+n)<<vertex(b+n)<<vertex(a-n)<<vertex(b-n);
        }
        if(dots)for(const auto& point:points){if(!std::isfinite(point.x())||!std::isfinite(point.y()))continue;for(int side=0;side<10;++side){const double a=side*.6283185307,b=(side+1)*.6283185307;triangles<<vertex(point)<<vertex(point+QPointF(std::cos(a),std::sin(a))*thickness*.5)<<vertex(point+QPointF(std::cos(b),std::sin(b))*thickness*.5);}}
        if(triangles.isEmpty())return;lineShader.setAttributeArray(attribute,triangles.constData());lineShader.setUniformValue("tint",color);glDrawArrays(GL_TRIANGLES,0,triangles.size());
    };
    glDisable(GL_DEPTH_TEST);glEnable(GL_BLEND);glBlendEquation(GL_FUNC_ADD);glBlendFuncSeparate(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    const auto points=projectedTrajectory();
    draw(points,focusedPath()?5.8f:7.f,{.09f,.20f,.29f,.62f},true);
    draw(points,focusedPath()?3.f:4.f,{1,.706f,0,1},true);
    draw(points,.8f,{1,.941f,.639f,1},true);
    draw({points.first()},12,{1,1,1,1},true);draw({points.first()},8,{0,.53f,.42f,1},true);
    draw({points.last()},12,{1,1,1,1},true);draw({points.last()},8,{0,.66f,.83f,1},true);
    if(focusedPath()){
        const QPointF origin(width()-54,height()-50),projectedCenter=screen(cameraCenter);
        for(int axis=0;axis<3;++axis){QVector3D direction;direction[axis]=std::max({span.x(),span.y(),span.z(),1.f})*.3f;const auto delta=screen(cameraCenter+direction)-projectedCenter;const double length=std::hypot(delta.x(),delta.y());
            const auto tip=origin+delta*(length>0?28/length:0);draw({origin,tip},2,axis==0?QVector4D(.85f,.26f,.26f,1):axis==1?QVector4D(0,.53f,.42f,1):QVector4D(.14f,.54f,.86f,1),true);}
    }
    lineShader.disableAttributeArray(attribute);lineShader.release();glDisable(GL_BLEND);
}
void TwinView::paintGL(){
    glBindBuffer(GL_ARRAY_BUFFER,0);
    glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);glDepthMask(GL_TRUE);
    glClearColor(background_.redF(),background_.greenF(),background_.blueF(),1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    const auto vp=viewProjection();
    if(ready&&error_.isEmpty()&&!focusedPath()){
        glEnable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);
        if(sceneMode){
            QVector<QVector3D> grid;for(int x=-3000;x<=5000;x+=500)grid<<QVector3D(x,-3500,-35)<<QVector3D(x,3500,-35);
            for(int y=-3500;y<=3500;y+=500)grid<<QVector3D(-3000,y,-35)<<QVector3D(5000,y,-35);
            lineShader.bind();lineShader.setUniformValue("mvp",vp);const auto a=lineShader.attributeLocation("position");
            lineShader.enableAttributeArray(a);lineShader.setAttributeArray(a,grid.constData());
            lineShader.setUniformValue("tint",background_.lightnessF()>.48?QVector4D(.63,.7,.76,1):QVector4D(.2,.28,.36,1));glLineWidth(1);glDrawArrays(GL_LINES,0,grid.size());
            lineShader.disableAttributeArray(a);lineShader.release();
        }
        shader.bind();buffer.bind();
        const auto position=shader.attributeLocation("position"),normal=shader.attributeLocation("normal"),color=shader.attributeLocation("color");
        shader.enableAttributeArray(position);shader.enableAttributeArray(normal);shader.enableAttributeArray(color);
        shader.setAttributeBuffer(position,GL_FLOAT,0,3,36);shader.setAttributeBuffer(normal,GL_FLOAT,12,3,36);shader.setAttributeBuffer(color,GL_FLOAT,24,3,36);
        shader.setUniformValue("stale",stale?1.f:0.f);
        shader.setUniformValue("ghost",0.f);
        const auto transforms=rig.transforms(pose);
        for(const auto& part:parts){
            const auto m=transforms[part.link];shader.setUniformValue("mvp",vp*m);shader.setUniformValue("model",m);
            glDrawArrays(GL_TRIANGLES,part.first,part.count);
        }
        bool coincident=plan.goal.has_value();if(plan.goal)for(int i=0;i<6;++i)coincident=coincident&&std::abs(pose[i]-(*plan.goal)[i])<.12;
        if(sceneMode&&ghostVisible&&plan.goal&&!coincident){
            glEnable(GL_BLEND);glBlendEquation(GL_FUNC_ADD);glBlendFuncSeparate(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);glDepthMask(GL_FALSE);glDepthFunc(GL_LEQUAL);
            glEnable(GL_CULL_FACE);glCullFace(GL_BACK);shader.setUniformValue("ghost",1.f);shader.setUniformValue("stale",0.f);
            const auto goalFrames=rig.transforms(*plan.goal);
            const auto drawGhost=[&]{for(const auto& part:parts)if(part.link>0){const auto m=goalFrames[part.link];shader.setUniformValue("mvp",vp*m);shader.setUniformValue("model",m);glDrawArrays(GL_TRIANGLES,part.first,part.count);}};
            glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);glDepthMask(GL_TRUE);drawGhost();
            glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glDepthMask(GL_FALSE);glDepthFunc(GL_EQUAL);drawGhost();
            glDisable(GL_CULL_FACE);glDepthMask(GL_TRUE);glDepthFunc(GL_LESS);glDisable(GL_BLEND);
        }
        shader.disableAttributeArray(position);shader.disableAttributeArray(normal);shader.disableAttributeArray(color);buffer.release();shader.release();glDisable(GL_DEPTH_TEST);
    }
    const auto project=[&vp,this](QVector3D p){auto clip=vp*QVector4D(p,1);if(clip.w()<=0)return QPointF(-999,-999);auto n=clip.toVector3DAffine();return QPointF((n.x()+1)*width()/2,(1-n.y())*height()/2);};
    if(ready&&error_.isEmpty()){
        glDisable(GL_DEPTH_TEST);glEnable(GL_BLEND);glBlendEquation(GL_FUNC_ADD);glBlendFuncSeparate(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
        lineShader.bind();lineShader.setUniformValue("mvp",vp);const auto attribute=lineShader.attributeLocation("position");lineShader.enableAttributeArray(attribute);
        const auto line=[&](const QVector<QVector3D>& points,int mode,QVector4D color,float width){
            lineShader.setAttributeArray(attribute,points.constData());lineShader.setUniformValue("tint",color);glLineWidth(width);glDrawArrays(mode,0,points.size());
        };
        if(!focusedPath())for(int i=0;i<3;++i){QVector3D tip;tip[i]=500;line({QVector3D(),tip},GL_LINES,i==0?QVector4D(.9,.23,.23,1):i==1?QVector4D(.08,.65,.4,1):QVector4D(.15,.49,.94,1),2);}
        lineShader.disableAttributeArray(attribute);lineShader.release();glLineWidth(1);glDisable(GL_BLEND);
    }
    if(ready&&error_.isEmpty())paintTrajectory(vp);
    QString pathInfo;
    if(focusedPath()){QVector3D lo=plan.relative.first(),hi=lo;for(const auto& p:plan.relative)for(int axis=0;axis<3;++axis){lo[axis]=std::min(lo[axis],p[axis]);hi[axis]=std::max(hi[axis],p[axis]);}const auto span=hi-lo;pathInfo=QString("%1 段 · 范围 X %2 × Y %3 × Z %4 mm").arg(plan.relative.size()-1).arg(span.x(),0,'f',1).arg(span.y(),0,'f',1).arg(span.z(),0,'f',1);}
    message->setText(!error_.isEmpty()?error_:sceneMode&&plan.path.size()<2?"打开作业后，在模型旁显示轨迹和终点姿态":pathInfo);
    message->setVisible(!message->text().isEmpty());
    endpoint->setText(focusedPath()?"终点":"理想终点");endpoint->setVisible(sceneMode&&plan.path.size()>1&&error_.isEmpty());
    if(endpoint->isVisible()){auto point=project(plan.path.last());endpoint->move(std::clamp(int(point.x()+12),8,std::max(8,width()-115)),std::clamp(int(point.y()-26),90,std::max(90,height()-55)));}
}

void TwinView::mousePressEvent(QMouseEvent* e){last=e->pos();}
void TwinView::mouseMoveEvent(QMouseEvent* e){if(e->buttons()&Qt::LeftButton){autoFit=false;auto delta=e->pos()-last;yaw+=delta.x()*.4f;pitch=std::clamp(pitch+delta.y()*.35f,-85.f,89.f);last=e->pos();update();}}
void TwinView::wheelEvent(QWheelEvent* e){autoFit=false;distance=std::clamp(distance*std::pow(.999f,float(e->angleDelta().y())),focusedPath()?.5f:800.f,30000.f);update();e->accept();}

TwinPanel::TwinPanel(PlcBridgeClient& bridge,const QString& folder,QWidget* parent):QWidget(parent),reader(bridge,this){
    setObjectName("twinPanel");
    setStyleSheet("QWidget#twinPanel { background:white; border:1px solid #d7e0eb; border-radius:6px; } QLabel[twinValue=\"true\"] { color:#243f5e; font:600 11pt Consolas; }");
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(12,10,12,12);layout->setSpacing(10);
    auto* bar=new QHBoxLayout;auto* title=new QLabel("机器人姿态");title->setProperty("cardTitle",true);title->setToolTip("原始 SR175/3200 STEP 装配。关节轴向与零位仍需现场对照校准。");bar->addWidget(title);bar->addStretch();
    auto* showPath=new QCheckBox("显示轨迹");showPath->setObjectName("twinShowTrajectory");showPath->setToolTip("显示已导入作业的规划轨迹；仅改变三维显示");bar->addWidget(showPath);
    auto* retry=new QToolButton;retry->setText("读取姿态");retry->setToolTip("读取当前完整六轴快照；采集断开时重连");retry->setObjectName("twinReadPose");retry->setStyleSheet("QToolButton { padding:3px 7px;font-size:9pt; }");bar->addWidget(retry);connect(retry,&QToolButton::clicked,this,[this]{reader.reconnect();refresh();});
    auto* settings=new QToolButton;settings->setObjectName("twinViewSettings");settings->setText("视图 / 背景");settings->setPopupMode(QToolButton::InstantPopup);settings->setStyleSheet("QToolButton { padding:3px 7px;font-size:9pt; }");bar->addWidget(settings);layout->addLayout(bar);
    setMinimumWidth(360);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Maximum);
    canvas=new TwinView;canvas->loadModel(folder);canvas->setMinimumWidth(0);layout->addWidget(canvas);
    connect(showPath,&QCheckBox::toggled,this,[this](bool on){canvas->setFrameScene(on);canvas->setTrajectory(preview);canvas->fitScene();workstationSettings().setValue("twin/showTrajectory",on);});
    showPath->setChecked(workstationSettings().value("twin/showTrajectory",true).toBool());
    auto* meta=new QVBoxLayout;meta->setContentsMargins(0,0,0,0);meta->setSpacing(6);layout->addLayout(meta);
    state=new QLabel;state->setObjectName("twinState");state->setWordWrap(true);state->setStyleSheet("font-size:10pt;color:#5d6d81;");meta->addWidget(state);
    auto* snapshotRow=new QHBoxLayout;meta->addLayout(snapshotRow);
    captureTime=new QLabel("快照时间：—");captureTime->setObjectName("twinCaptureTime");captureTime->setProperty("hint",true);snapshotRow->addWidget(captureTime);snapshotRow->addStretch();
    source=new QLabel(this);source->setObjectName("twinSource");source->setStyleSheet("font-size:8pt;color:#66778d;");
    age=new QLabel;age->setObjectName("twinAge");age->setStyleSheet("font-size:8pt;color:#66778d;");
    snapshotRow->addWidget(age);source->hide();
    auto* divider=new QFrame;divider->setFrameShape(QFrame::HLine);divider->setStyleSheet("color:#dfe7ef;");meta->addWidget(divider);
    auto* grid=new QGridLayout;grid->setVerticalSpacing(5);grid->setHorizontalSpacing(10);
    for(int i=0;i<6;++i){const int column=(i%3)*2;auto* name=new QLabel(QString("J%1 / °").arg(i+1));name->setProperty("field",true);name->setStyleSheet("font-size:9pt;");grid->addWidget(name,i/3,column);auto* value=new QLabel("—");value->setObjectName(QString("twinJoint%1").arg(i+1));value->setProperty("twinValue",true);value->setStyleSheet("font:600 10pt Consolas;");value->setAlignment(Qt::AlignRight);grid->addWidget(value,i/3,column+1);values<<value;}for(int c:{1,3,5})grid->setColumnStretch(c,1);meta->addLayout(grid);
    detail=new QLabel(this);detail->setObjectName("twinDetail");detail->hide();
    auto* menu=new QMenu(settings);settingsMenu=menu;settings->setMenu(menu);auto* views=menu->addMenu("观察方向");auto* directions=new QActionGroup(this);
    auto* focusPath=menu->addAction("轨迹细节 · 按G代码范围缩放");focusPath->setObjectName("twinFocusTrajectory");
    connect(focusPath,&QAction::triggered,this,[this,showPath]{showPath->setChecked(true);canvas->setTrajectoryFocus(true);if(companion){companion->setFrameScene(true);companion->setTrajectoryFocus(true);}if(previewChanged)previewChanged();});
    auto* wholeRobot=menu->addAction("机器人全景");wholeRobot->setObjectName("twinFocusRobot");
    connect(wholeRobot,&QAction::triggered,this,[this]{canvas->setTrajectoryFocus(false);if(companion)companion->setTrajectoryFocus(false);if(previewChanged)previewChanged();});
    menu->addAction("读取姿态",this,[this]{reader.reconnect();refresh();});
    const QStringList viewNames{"等轴视角","正视","侧视","俯视"};
    for(int i=0;i<viewNames.size();++i){auto* action=views->addAction(viewNames[i]);action->setCheckable(true);directions->addAction(action);if(i==0)action->setChecked(true);connect(action,&QAction::triggered,this,[this,i]{canvas->setView(i);if(companion)companion->setView(i);});}
    menu->addAction("复位视角",this,[this,directions]{canvas->setView(0);if(companion)companion->setView(0);directions->actions().first()->setChecked(true);});
    auto* bgMenu=menu->addMenu("背景颜色");bgMenu->setObjectName("twinBackgroundMenu");backgrounds=new QActionGroup(this);
    const QStringList bgNames{"浅色","中性灰","淡蓝","深色","自选颜色…"};
    for(int i=0;i<bgNames.size();++i){auto* action=bgMenu->addAction(bgNames[i]);action->setObjectName(QString("twinBackground%1").arg(i));action->setCheckable(true);backgrounds->addAction(action);connect(action,&QAction::triggered,this,[this,i]{
        if(i<4)setBackgroundPreset(i);else{const auto color=QColorDialog::getColor(canvas->background(),this,"选择三维背景色");if(color.isValid())setBackgroundPreset(4,color);else backgrounds->actions()[workstationSettings().value("twin/backgroundPreset",0).toInt()]->setChecked(true);}
    });}
    menu->addSeparator();demoAction=menu->addAction("离线演示…");demoAction->setObjectName("twinDemoAction");
    demoDialog=new QDialog(this);demoDialog->setWindowTitle("离线姿态演示");demoDialog->setMinimumWidth(320);auto* demoLayout=new QVBoxLayout(demoDialog);demoLayout->addWidget(WorkstationUi::hint("调整仅用于本地模型显示。"));
    demo=new QPushButton("启用离线演示");demo->setCheckable(true);demo->setObjectName("twinDemo");demoLayout->addWidget(demo);
    demoBox=new QGroupBox("六轴角度 / °");auto* demoGrid=new QGridLayout(demoBox);
    for(int i=0;i<6;++i){auto* spin=new QDoubleSpinBox;spin->setRange(canvas->rig.lower[i],canvas->rig.upper[i]);spin->setDecimals(1);spin->setSingleStep(5);spin->setSuffix(" °");spin->setObjectName(QString("twinDemoJoint%1").arg(i+1));demoValues<<spin;demoGrid->addWidget(new QLabel(QString("J%1").arg(i+1)),i,0);demoGrid->addWidget(spin,i,1);connect(spin,&QDoubleSpinBox::valueChanged,this,[this]{if(demo_){TwinJoints pose;for(int n=0;n<6;++n)pose[n]=demoValues[n]->value();lastTcp.reset();showPose(pose);}});}demoLayout->addWidget(demoBox);demoBox->hide();
    connect(demoAction,&QAction::triggered,this,[this]{demoDialog->show();demoDialog->raise();});
    connect(demoDialog,&QDialog::finished,this,[this]{demo->setChecked(false);});
    reader.changed=[this]{refresh();};timer.setInterval(100);connect(&timer,&QTimer::timeout,this,[this]{refresh();});timer.start();
    connect(demo,&QPushButton::toggled,this,[this](bool on){demo_=on&&!connected_;demoBox->setVisible(demo_);resetPreview();if(demo_){TwinJoints pose;for(int n=0;n<6;++n)pose[n]=demoValues[n]->value();showPose(pose);}else{hasPose=false;seenBoot=seenCounter=0;seenSdkSerial=0;captureTime->setText("快照时间：—");for(auto* label:values)label->setText("—");canvas->setPose({});if(companion)companion->setPose({});}refresh();});refresh();
    const auto prefs = workstationSettings();setBackgroundPreset(prefs.value("twin/backgroundPreset",0).toInt(),QColor(prefs.value("twin/backgroundCustom","#e9f0f6").toString()));
    canvas->setFixedHeight(300);
}
void TwinPanel::resizeEvent(QResizeEvent* event){
    QWidget::resizeEvent(event);
    updateViewportSize();
}
void TwinPanel::setViewportHeightLimit(int height){
    height=std::clamp(height,printLayout_?200:250,printLayout_?1600:470);if(viewportHeightLimit==height)return;viewportHeightLimit=height;updateViewportSize();
}
void TwinPanel::setPrintLayout(bool on){
    if(printLayout_==on)return;printLayout_=on;layout()->setContentsMargins(on?8:12,on?8:10,on?8:12,on?8:12);layout()->setSpacing(on?6:10);
    viewportHeightLimit=std::clamp(viewportHeightLimit,on?200:250,on?1600:470);updateViewportSize();
}
void TwinPanel::updateViewportSize(){
    const int height=printLayout_?viewportHeightLimit:std::clamp(qRound((width()-24)*.60),250,viewportHeightLimit);
    if(canvas->height()!=height)canvas->setFixedHeight(height);
    setFixedHeight(height+(printLayout_?142:158));
}
void TwinPanel::setBackgroundPreset(int preset,const QColor& custom){
    const QList<QColor> colors{QColor("#e9f0f6"),QColor("#babfc5"),QColor("#cce4f4"),QColor("#18222f")};
    if(preset<0||preset>4)preset=0;if(preset==4&&!custom.isValid())preset=0;
    canvas->setBackground(preset==4?custom:colors[preset]);backgrounds->actions()[preset]->setChecked(true);
    if(companion)companion->setBackground(canvas->background());
    auto prefs = workstationSettings();prefs.setValue("twin/backgroundPreset",preset);if(preset==4)prefs.setValue("twin/backgroundCustom",custom.name());
}
void TwinPanel::setContext(bool plc,bool connected,bool fresh){
    if(plc_!=plc){hasPose=false;sdkValid_=false;seenBoot=seenCounter=0;seenSdkSerial=sdkSerial;resetPreview();captureTime->setText("快照时间：—");for(auto* label:values)label->setText("—");canvas->setPose({});if(companion)companion->setPose({});}
    plc_=plc;connected_=connected;sdkFresh_=fresh;reader.setEnabled(plc);
    if(connected_&&demo_){demo->setChecked(false);hasPose=false;for(auto* label:values)label->setText("—");canvas->setPose({});if(companion)companion->setPose({});}
    demo->setEnabled(!connected_);demoAction->setEnabled(!connected_);refresh();
}
void TwinPanel::acceptSdk(const QString& axes,bool simulated,const QString& xyz){
    if(plc_)return;const auto parts=axes.split(',');sdkValid_=false;if(parts.size()!=6){refresh();return;}
    TwinJoints pose;for(int i=0;i<6;++i){bool ok;pose[i]=parts[i].toDouble(&ok);if(!ok||!std::isfinite(pose[i])||std::abs(pose[i])>10000){refresh();return;}}
    sdkTcp.reset();const auto coords=xyz.split(',');if(coords.size()==3){QVector3D tcp;bool valid=true;for(int i=0;i<3;++i){bool ok;const auto value=coords[i].toDouble(&ok);valid=valid&&ok&&std::isfinite(value)&&std::abs(value)<=10000;tcp[i]=float(value);}if(valid)sdkTcp=tcp;}
    sdkPose=pose;sdkValid_=true;simulated_=simulated;++sdkSerial;sdkAge.restart();refresh();
}
void TwinPanel::showPose(const TwinJoints& joints){
    lastPose=joints;hasPose=true;poseAvailable=true;poseDemo=demo_;canvas->setPose(joints);if(companion)companion->setPose(joints);
    const bool first=!preview.anchored;
    if(first&&canBindPreview())preview.bind(joints,canvas->rig,lastTcp);else preview.currentTcp=lastTcp;
    captureTime->setText(demo_?"快照时间：离线设置":"快照时间："+QDateTime::currentDateTime().toString("HH:mm:ss"));for(int i=0;i<6;++i)values[i]->setText(QString::number(joints[i],'f',3));syncPreview();if(first&&companion)companion->fitScene();
}
void TwinPanel::syncPreview(){canvas->setTrajectory(preview);if(companion)companion->setTrajectory(preview);if(previewChanged)previewChanged();}
void TwinPanel::resetPreview(){poseAvailable=false;poseDemo=false;lastTcp.reset();sdkTcp.reset();preview.clearAnchor(canvas->rig);syncPreview();}
void TwinPanel::setCompanionView(TwinView* view){companion=view;if(!view)return;view->setFrameScene(true);view->setPose(canvas->currentPose());view->setBackground(canvas->background());view->setTrajectory(preview);view->fitScene();refresh();}
void TwinPanel::setJobPath(const QVector<Point>& points){
    bool same=points.size()==preview.relative.size();for(int i=0;same&&i<points.size();++i)same=QVector3D(points[i].x,points[i].y,points[i].z)==preview.relative[i];if(same)return;
    preview.setPath(points,canvas->rig);if(canBindPreview())preview.bind(lastPose,canvas->rig,lastTcp);syncPreview();if(companion)companion->fitScene();
}
bool TwinPanel::canBindPreview() const {return poseAvailable&&(demo_||(plc_?reader.fresh()&&(!reader.packet().nativeJoints||(reader.packet().flags&2)!=0):sdkFresh_&&sdkValid_&&sdkAge.isValid()&&sdkAge.elapsed()<3000));}
void TwinPanel::bindPreviewOrigin(){if(!canBindPreview())return;preview.bind(lastPose,canvas->rig,lastTcp);syncPreview();if(companion)companion->fitScene();}
QString TwinPanel::previewStatus() const {
    if(preview.relative.size()<2)return "打开可预览轨迹的作业后显示亮线与终点虚影";
    QVector3D lo=preview.relative.first(),hi=lo;for(const auto& p:preview.relative)for(int axis=0;axis<3;++axis){lo[axis]=std::min(lo[axis],p[axis]);hi[axis]=std::max(hi[axis],p[axis]);}
    const auto extent=hi-lo;
    const auto end=preview.relative.last();QString text=QString("%1 段 · 范围 X %2 × Y %3 × Z %4 mm  |  ").arg(preview.relative.size()-1).arg(extent.x(),0,'f',1).arg(extent.y(),0,'f',1).arg(extent.z(),0,'f',1);
    text+=preview.anchored?(poseDemo?"离线预览起点已固定":"预览起点已固定"):"参考起点 · 等待静止轴角";
    text+=QString("  |  终点偏移 X %1  Y %2  Z %3 mm").arg(end.x(),0,'f',1).arg(end.y(),0,'f',1).arg(end.z(),0,'f',1);
    if(!preview.goal)text+="  |  未解出限位内终点姿态，仅显示目标点";
    else if(!preview.anchored)text+="  |  虚影为参考预览";
    if(const auto delta=preview.endpointDistance())text+=QString("  |  快照距终点 %1 mm").arg(*delta,0,'f',1);
    return text;
}
QString TwinPanel::snapshotSummary() const {
    QString text=state->text();if(!hasPose)return text;
    text+="  ·  "+captureTime->text().mid(5)+"  |  ";QStringList axes;for(int i=0;i<6;++i)axes<<QString("J%1 %2°").arg(i+1).arg(lastPose[i],0,'f',2);return text+axes.join("   ");
}
void TwinPanel::setDemoPose(const TwinJoints& pose){if(connected_)return;for(int i=0;i<6;++i)demoValues[i]->setValue(pose[i]);demo->setChecked(true);}
void TwinPanel::refresh(){
    const bool live=plc_?reader.fresh():sdkFresh_&&sdkValid_&&sdkAge.isValid()&&sdkAge.elapsed()<3000;
    const qint64 ms=plc_?reader.ageMs():sdkAge.isValid()?sdkAge.elapsed():-1;
    source->setText(demo_?"离线演示 · °":plc_?"PLC 快照 · °":"STEP 静止快照 · °");
    if(demo_){state->setText("离线演示 · 未连接设备");canvas->setStale(false);age->setText("模型角度：人工调整");}
    else {if(live){const auto packet=reader.packet();
            if(!hasPose||(plc_&&(seenBoot!=packet.boot||seenCounter!=packet.counter))||(!plc_&&seenSdkSerial!=sdkSerial)){
                if(plc_&&seenBoot&&seenBoot!=packet.boot)resetPreview();
                lastTcp=plc_?(packet.hasTcp?std::optional<QVector3D>(packet.tcp):std::nullopt):sdkTcp;
                showPose(plc_?packet.joints:sdkPose);seenBoot=packet.boot;seenCounter=packet.counter;seenSdkSerial=sdkSerial;
            }
        }canvas->setStale(!live);
        state->setText(live?(plc_?(reader.packet().nativeJoints?"● 有效 · PLC 六轴反馈":"● 有效 · PLC 空闲快照"):simulated_?"● 有效 · 虚拟控制器快照":"● 有效 · 实机快照"):
            hasPose?"保留上次姿态 · 等待新快照":plc_&&connected_?(reader.legacySource()?"参考姿态 · PLC 持续轴角反馈尚未接通":"已连接 · 等待实际六轴反馈"):"参考姿态 · 等待有效轴角");
        age->setText(ms<0?"数据年龄：—":QString("数据年龄：%1 s").arg(ms/1000.,0,'f',1));
    }
    detail->setText(demo_?"调整六轴仅驱动本页模型。":plc_?(reader.legacySource()?"当前 PLC 尚未提供持续六轴反馈；安装配套 PLC 轴角读取程序后，连接即可更新。原有手动服务快照仍可读取。":
        "连接 PLC 后自动读取控制柜的 J1～J6，无需进入微调。新反馈更新五个页面；超时保留上一姿态并标记过期。起点对齐只在停止时可用。"):
        live?"静止时读取六轴并更新模型；运动期间保留上次姿态。":"连接后在静止时读取六轴。尚未取得有效快照时显示参考姿态。");
    state->setToolTip(detail->text());
    const auto feedback=demo_?QString("离线演示"):live?QString(plc_&&reader.packet().nativeJoints?"实际轴角 · 持续更新":"实际姿态 · 静止快照"):
        hasPose?QString("上次姿态 · 已过期"):QString("参考姿态 · 未读到轴角");
    canvas->setFeedbackState(feedback);if(companion)companion->setFeedbackState(feedback);
    if(companion)companion->setStale(!demo_&&!live);
    if(previewChanged)previewChanged();
}
