#include "workstation_settings.h"
#include "workstation_session.h"
#include "workstation_experiment.h"
#include "workstation_converter.h"
#include "workstation_home.h"
#include "workstation_modbus.h"
#include "workstation_plcbridgepanel.h"
#include "workstation_plot.h"
#include "workstation_replay.h"
#include "workstation_fileuploaddialog.h"
#include "workstation_deliverydialog.h"
#include "workstation_manualbridge.h"
#include "workstation_ui.h"
#include "workstation_livemonitor.h"
#include "workstation_twin.h"
#include "workstation_mode.h"
#include "workstation_welder.h"
#include "workstation_print.h"

#include <QtWidgets>
#include <QDateTime>
#include <QDesktopServices>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QHostAddress>
#include <QSettings>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace {
const QStringList LinkTypes{"STEP 虚拟控制器", "实机 STEP SDK", "AC802 PLC · Modbus TCP"};
const QString DefaultCabinetEndpoint = "192.168.39.220:30312";
const QString DefaultModbusEndpoint; // PLC LAN A must be entered explicitly; never reuse a robot IP.

QString duration(double seconds) {
    const auto s = static_cast<int>(std::min(seconds, 9999999.));
    return QString("%1:%2").arg(s / 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}

// 控制端拒绝命令时输出英文 Refused 行，这里按关键字翻译成人话，让操作者不用猜。
QString refuseHint(const QString& raw) {
    if (raw.contains("does not contain the expected project"))
        return "柜内没有作业的工程文件：请先用 U 盘或 STEPStudio 把工程存入柜内（一次即可），之后启动时本软件会自动加载。";
    if (raw.contains("expected program is not loaded; press load program first"))
        return "柜内没有载入作业程序：先点「加载作业程序」切换柜内程序，再记录起点或启动。";
    if (raw.contains("must be stopped before switching"))
        return "柜内程序未停止：请先在示教器上停止当前程序，再点「加载作业程序」。";
    if (raw.contains("robot must be fully stopped before recording"))
        return "机器人未完全停止：等它完全停下后再点「记录上位机起点」。";
    if (raw.contains("writing program home failed"))
        return "起点写入柜内程序失败：确认柜内已载入作业程序且处于停止状态，再重新点「记录上位机起点」。";
    if (raw.contains("no saved home; move to the start pose"))
        return "柜内程序没有记录起点：手动把机器人挪到打印起点，点「记录上位机起点」写入柜内程序，再点开始。";
    if (raw.contains("expected project and program") || raw.contains("not loaded; use the local pendant"))
        return "柜内没有载入作业对应的工程与程序：点「开始」或「加载作业程序」可自动切换（工程须已存入柜内）。";
    if (raw.contains("AUT, continuous mode"))
        return "柜内需处于自动模式、连续运行方式且伺服已使能：请到示教器切换后重试。";
    if (raw.contains("stopped at its entry") || raw.contains("reload"))
        return "程序须停在首行才能启动：请在示教器重新载入程序，使其回到首行。";
    if (raw.contains("positive override"))
        return "倍率需大于 0 才能启动：请在示教器上把倍率调高后重试。";
    if (raw.contains("controller reports an error"))
        return "控制器正在报错：请先在柜内查看并清除报警，再回界面重试。";
    if (raw.contains("moving or returning"))
        return "机器人正在运动或回程中：请先点「停止发送」并等它停稳。";
    if (raw.contains("control disabled"))
        return "启停控制权不可用或连接已断开：请重新连接后再试。";
    if (raw.contains("private IPv4"))
        return "拒绝连接：只允许接入已确认的控制柜私有网地址。";
    if (raw.contains("verified local virtual controller"))
        return "未找到受信任的本地虚拟控制器：请检查 STEPStudio 安装目录。";
    if (raw.isEmpty()) return "操作被控制端拒绝，原因未说明。";
    return "控制端拒绝：" + raw;
}

void setVisualProperty(QWidget* widget, const char* name, const QVariant& value) {
    if (widget->property(name) == value) return;
    widget->setProperty(name, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}
}

// 工作区根：exe 旁的 workspace.txt（一行路径）优先；否则安装根（exe 上溯一级）内含
// STEPStudio 4.7.0 时取安装根；再否则回退开发布局（exe 上溯三级）。
QString resolveWorkspace(const QString& exeDir) {
    QFile override(QDir(exeDir).filePath("workspace.txt"));
    if (override.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString line = QString::fromUtf8(override.readLine()).trimmed();
        if (!line.isEmpty()) return QDir::fromNativeSeparators(line);
    }
    const QDir installRoot(exeDir + "/..");
    if (installRoot.exists("STEPStudio 4.7.0")) return installRoot.absolutePath();
    return QDir(exeDir).absoluteFilePath("../../..");
}

class Workstation : public QMainWindow {
public:
    explicit Workstation(const QString& dir)
        : directory(dir), workspace(resolveWorkspace(QCoreApplication::applicationDirPath())),
          recordRoot(QDir(workspace).filePath("outputs/workstation_runs")),
          nativeTemplate(readNativeJob(QDir(dir).absoluteFilePath("../SR175_3200_Helix_Ready_25mms_v4.zip"))),
          session(workspace, QCoreApplication::applicationDirPath() + "/helix_pc.exe") {
        setWindowTitle("STEP 打印作业台 · SR175-3200 · R6.4 连续上传与密码便捷版");
        setMinimumSize(1120, 740);
        resize(1520, 920);
        build();
        elapsed.start();
        session.note = [this](QString text) { log(text); };
        session.event = [this](QString kind) { experimentEvent(kind); };
        session.sampled = [this] { sample(); };
        session.changed = [this] { refresh(); };
        session.ended = [this](bool lost) { finishExperiment(lost ? "feedback_lost" : "session_closed"); };
        refresh();
        connect(&modbus, &PlcBridgeClient::linkChanged, this, [this](bool up, const QString& detail) {
            modbusLinked = up;
            log(up ? "PLC TCP已连接，等待签名与动态心跳：" + detail : "PLC连接关闭：" + detail);
            refresh();
        });
        connect(&modbus, &PlcBridgeClient::rejected, this, [this](const QString& text) { log("PLC · " + text); });
        connect(&modbus, &PlcBridgeClient::notice, this, [this](const QString& text) { log("PLC · " + text); });
        connect(&modbus, &PlcBridgeClient::changed, this, [this] { refresh(); });
        log("C++ 工作台就绪。打开工程 / G 代码，或从「工程 / 工具」载入螺旋示例。");
        const auto settings = workstationSettings();
        const int savedType = settings.value("link/type", 0).toInt();
        if (savedType >= 0 && savedType < linkType->count()) linkType->setCurrentIndex(savedType);
        ip->setText(savedType == 2
            ? settings.value("plcBridge/address", DefaultModbusEndpoint).toString()
            : settings.value("cabinet/address", DefaultCabinetEndpoint).toString());
        programNumber->setValue(settings.value("plcBridge/programSlot", 1).toInt());
    }

    ~Workstation() override { session.note = {}; session.event = {}; session.sampled = {}; session.changed = {}; session.ended = {}; }

    void runTwinSmoke(const QString& imagePath) {
        linkType->setCurrentIndex(2);
        workTabs->setCurrentIndex(0);
        load(nativeTemplate);
        twinPanel->setDemoPose({25,-25,35,30,40,50});
        QTimer::singleShot(1200,this,[this,imagePath]{
            bool ok=workTabs->count()==7&&workTabs->indexOf(twinPanel)==-1&&twinScene->isVisible()&&!monitorDock->isVisible()&&twinPanel->view()->vertexCount()>1000&&twinPanel->view()->error().isEmpty()&&twinScene->error().isEmpty()&&twinScene->trajectory().goal.has_value()&&twinScene->trajectory().path.size()==job.points.size();
            bool saved=true;QJsonArray layoutReport;
            const QFileInfo info(imagePath);
            auto settle=[this]{for(int pass=0;pass<6;++pass){QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);refreshWorkspaceLayout();}};
            for(int i=0;i<3;++i){
                workTabs->setCurrentIndex(i);settle();
                const QRect geometry(twinPanel->mapTo(this,QPoint()),twinPanel->size());
                if(i==0)ok=ok&&twinScene->isVisible()&&!monitorDock->isVisible()&&twinScene->width()>=360&&twinScene->height()>=300;
                else {
                    const auto* host=poseHosts[i==1?1:0];
                    ok=ok&&twinPanel->isVisible()&&!twinScene->isVisible()&&twinPanel->parentWidget()==host&&twinPanel->view()->height()>=250&&twinPanel->width()>=360;
                    const double aspect=double(twinPanel->view()->width())/twinPanel->view()->height();
                    auto* controls=i==2?plcPanel->findChild<QWidget*>("plcPrimaryControls"):plcManual->findChild<QTabWidget*>("manualMotionTabs")->parentWidget();
                    const QRect actionsRect(controls->mapTo(this,QPoint()),controls->size());
                    auto* scroll=i==2?plcPanel->findChild<QScrollArea*>("plcControlScroll"):plcManual->findChild<QScrollArea*>("manualControlScroll");
                    ok=ok&&aspect>1.5&&aspect<2.1&&actionsRect.right()<geometry.left()&&std::abs(actionsRect.top()-geometry.top())<50&&geometry.top()<height()*.5&&scroll->horizontalScrollBar()->maximum()==0;
                    const QRect viewport(scroll->viewport()->mapTo(this,QPoint()),scroll->viewport()->size());
                    const QStringList essential=i==2?QStringList{"plcBridgeCommand1","plcBridgeCommand4","plcBridgeCommand5","plcBridgeCommand7"}:QStringList{"manualEnter","manualStop","manualExit"};
                    for(const auto& name:essential){auto* button=(i==2?static_cast<QWidget*>(plcPanel):static_cast<QWidget*>(plcManual))->findChild<QPushButton*>(name);ok=ok&&button&&viewport.contains(QRect(button->mapTo(this,QPoint()),button->size()));}
                    int rightWidth=0;
                    if(width()>=1400){
                        auto* right=i==2?monitorHosts[0]:plcManual->findChild<QWidget*>("manualOriginControls");
                        const QRect rightRect(right->mapTo(this,QPoint()),right->size());rightWidth=rightRect.width();
                        const double fraction=double(geometry.width())/(rightRect.right()-actionsRect.left()+1);
                        ok=ok&&viewport.contains(geometry)&&std::abs(actionsRect.width()-rightRect.width())<=2&&fraction>.48&&fraction<.51&&std::abs(geometry.center().x()-(actionsRect.left()+rightRect.right())/2)<=2;
                    }
                    const auto* snapshotState=twinPanel->findChild<QLabel*>("twinState");ok=ok&&snapshotState->mapTo(twinPanel,QPoint()).y()>twinPanel->view()->geometry().bottom();
                    qInfo()<<"Compact page"<<i+1<<"window"<<size()<<"controls"<<actionsRect<<"model"<<geometry<<"aspect"<<aspect<<"horizontal overflow"<<scroll->horizontalScrollBar()->maximum();
                    layoutReport.append(QJsonObject{{"page",i+1},{"controls_x",actionsRect.x()},{"controls_y",actionsRect.y()},{"controls_width",actionsRect.width()},{"controls_height",actionsRect.height()},{"right_width",rightWidth},{"model_x",geometry.x()},{"model_y",geometry.y()},{"model_width",geometry.width()},{"model_height",geometry.height()},{"aspect",aspect},{"horizontal_overflow",scroll->horizontalScrollBar()->maximum()},{"layout_passed",ok}});
                    const auto frame=twinPanel->view()->grabFramebuffer();int top=frame.height(),bottom=-1;
                    for(int y=0;y<frame.height();y+=2)for(int x=0;x<frame.width();x+=2){auto p=frame.pixelColor(x,y);if(p.red()>80&&p.red()>p.green()*1.5&&p.red()>p.blue()*1.25){top=std::min(top,y);bottom=std::max(bottom,y);}}
                    ok=ok&&bottom-top>frame.height()*.60;
                }
                saved=grab().save(i==0?imagePath:info.path()+"/"+info.completeBaseName()+QString("-page%1.png").arg(i+1))&&saved;
            }
            auto* settings=twinPanel->findChild<QToolButton*>("twinViewSettings");
            workTabs->setCurrentIndex(0);
            for(int preset=0;preset<5;++preset){
                auto* action=twinPanel->findChild<QAction*>(QString("twinBackground%1").arg(preset));
                if(preset<4)action->trigger();else twinPanel->setBackgroundPreset(4,QColor("#e5ddce"));
                QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                ok=ok&&settings&&action->isChecked()&&twinPanel->view()->background()==twinScene->background();
                const auto frame=twinScene->grabFramebuffer();int gold=0,cyan=0,translucent=0;
                for(int y=0;y<frame.height();y+=2)for(int x=0;x<frame.width();x+=2){const auto p=frame.pixelColor(x,y);
                    gold+=p.red()>180&&p.green()>120&&p.blue()<120&&p.red()-p.green()>30;
                    cyan+=p.green()-p.red()>28&&p.blue()-p.red()>40;
                    translucent+=p.alpha()!=255;
                }
                ok=ok&&gold>12&&cyan>25&&translucent==0&&twinScene->findChild<QLabel*>("twinSceneLegend")->isVisible();
                saved=grab().save(info.path()+"/"+info.completeBaseName()+QString("-background%1.png").arg(preset))&&saved;
            }
            twinPanel->setBackgroundPreset(0);workTabs->setCurrentIndex(0);
            const auto plan=twinScene->trajectory();
            if(plan.goal){twinPanel->setDemoPose(*plan.goal);QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                ok=ok&&twinScene->trajectory().anchor==plan.anchor&&twinScene->trajectory().goal==plan.goal;
                saved=grab().save(info.path()+"/"+info.completeBaseName()+"-end.png")&&saved;
            }
            twinPanel->findChild<QPushButton*>("twinDemo")->setChecked(false);
            for(int i=1;i<=2;++i){workTabs->setCurrentIndex(i);settle();twinPanel->view()->grabFramebuffer();
                saved=grab().save(info.path()+"/"+info.completeBaseName()+QString("-reference-page%1.png").arg(i+1))&&saved;}
            qInfo()<<"Twin model vertices"<<twinPanel->view()->vertexCount()<<"error"<<twinPanel->view()->error();
            QFile report(info.path()+"/"+info.completeBaseName()+"-layout.json");if(report.open(QIODevice::WriteOnly))report.write(QJsonDocument(QJsonObject{{"layout",layoutReport},{"passed",ok&&saved}}).toJson());
            QCoreApplication::exit(ok&&saved?0:31);
        });
    }

    void runWorkflowSmoke(const QString& imagePath) {
        linkType->setCurrentIndex(2);workTabs->setCurrentIndex(0);load(nativeTemplate);
        twinPanel->setDemoPose({25,-25,35,30,40,50});
        QTimer::singleShot(300,this,[this,imagePath]{
            const QFileInfo info(imagePath);auto path=[&](const QString& suffix){return info.path()+"/"+info.completeBaseName()+suffix+".png";};
            auto settle=[this]{for(int n=0;n<6;++n){QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);refreshWorkspaceLayout();}};
            bool ok=workTabs->widget(1)==plcManual&&workTabs->widget(2)==plcPanel&&workTabs->tabText(5).contains("焊机");
            ok=ok&&sourceGroup->width()>=360&&sourceTable->styleSheet().contains("10pt")&&gcodeDeliveryButton->isEnabled()&&gcodeDeliveryButton->text().contains("自动上传");
            auto* scenePath=findChild<QCheckBox*>("twinSceneShowTrajectory");auto* sceneLegend=twinScene->findChild<QLabel*>("twinSceneLegend");
            const auto pose=twinScene->currentPose();const auto plan=twinScene->trajectory();
            scenePath->setChecked(false);settle();
            ok=ok&&!sceneLegend->isVisible()&&twinScene->currentPose()==pose&&twinScene->trajectory().path==plan.path;
            ok=grab().save(path("-preparation-pose"))&&ok;
            scenePath->setChecked(true);settle();ok=ok&&sceneLegend->isVisible()&&twinScene->currentPose()==pose;
            ok=grab().save(imagePath)&&ok;
            workTabs->setCurrentWidget(plcManual);settle();
            auto* reg=plcManual->findChild<QPushButton*>("manualRegister");auto* logout=plcManual->findChild<QPushButton*>("manualLogout");
            auto* stop=plcManual->findChild<QPushButton*>("manualStop");auto* exit=plcManual->findChild<QPushButton*>("manualExit");auto* ack=plcManual->findChild<QPushButton*>("manualAcknowledge");auto* rearm=plcManual->findChild<QPushButton*>("manualRearm");
            ok=ok&&reg&&logout&&!reg->isEnabled()&&!logout->isEnabled()&&stop&&exit&&ack&&rearm&&ack->geometry().left()>exit->geometry().right()&&rearm->geometry().left()>ack->geometry().right();
            auto* trajectory=twinPanel->findChild<QCheckBox*>("twinShowTrajectory");trajectory->setChecked(true);settle();
            ok=ok&&trajectory->isChecked()&&twinPanel->view()->trajectory().path.size()==job.points.size()&&twinPanel->view()->trajectory().anchored;
            ok=grab().save(path("-manual-trajectory"))&&ok;
            workTabs->setCurrentWidget(plcPanel);settle();ok=ok&&trajectory->isVisible()&&trajectory->isChecked();ok=grab().save(path("-automatic-trajectory"))&&ok;
            workTabs->setCurrentWidget(welderPanel);settle();
            ok=ok&&!sourceGroup->isVisible()&&!welderPanel->client().fresh()&&welderPanel->findChildren<QDoubleSpinBox*>(QRegularExpression("^welderParameter[0-9]+$")).size()==16;
            for(auto* button:welderPanel->findChildren<QPushButton*>())if(button->objectName().startsWith("welder"))ok=ok&&!button->isEnabled();
            auto* welderScroll=welderPanel->findChild<QScrollArea*>("welderScroll");auto* params=welderPanel->findChild<QGroupBox*>("welderParameters");
            const QRect modelRect(twinPanel->mapTo(welderScroll->viewport(),QPoint()),twinPanel->size());
            ok=ok&&twinPanel->isVisible()&&twinPanel->parentWidget()==welderPanel->poseHost()&&trajectory->isVisible()&&trajectory->isChecked();
            ok=ok&&twinPanel->view()->currentPose()==pose&&twinPanel->view()->trajectory().path==plan.path&&welderScroll->horizontalScrollBar()->maximum()==0&&welderScroll->viewport()->rect().contains(modelRect);
            ok=grab().save(path("-welder-trajectory"))&&ok;
            trajectory->setChecked(false);settle();
            ok=ok&&!twinPanel->view()->findChild<QLabel*>("twinSceneLegend")->isVisible()&&scenePath->isChecked();
            ok=grab().save(path("-welder"))&&ok;
            welderScroll->ensureWidgetVisible(params,0,0);settle();ok=grab().save(path("-welder-parameters"))&&ok;
            auto* parameterScroll=welderPanel->findChild<QScrollArea*>("welderParameterScroll");
            ok=ok&&parameterScroll->horizontalScrollBar()->maximum()==0;
            parameterScroll->verticalScrollBar()->setValue(parameterScroll->verticalScrollBar()->maximum());settle();ok=grab().save(path("-welder-parameters-end"))&&ok;
            welderScroll->verticalScrollBar()->setValue(0);workTabs->setCurrentWidget(plcManual);settle();
            ok=ok&&twinPanel->parentWidget()==plcManual->poseHost()&&twinPanel->view()->currentPose()==pose;
            workTabs->setCurrentWidget(welderPanel);settle();ok=ok&&twinPanel->parentWidget()==welderPanel->poseHost();
            auto* robotControls=plcPanel->findChild<QWidget*>("plcPrimaryControls");auto* weldControls=welderPanel->findChild<QGroupBox*>("welderControls");
            auto* weldParameters=welderPanel->findChild<QGroupBox*>("welderParameters");auto* slot=plcPanel->findChild<QSpinBox*>("plcBridgeSlot");
            auto* current=welderPanel->findChild<QDoubleSpinBox*>("welderParameter2");slot->setValue(11);current->setValue(123.4);
            auto* read=welderPanel->findChild<QPushButton*>("welderRead");const auto session=modbus.sessionId();
            workTabs->setCurrentWidget(printPanel);settle();
            auto* printScroll=printPanel->findChild<QScrollArea*>("printScroll");
            bool printOk=workTabs->isTabVisible(6)&&workTabs->tabText(6).contains("⑤")&&printPanel->findChild<QWidget*>("plcPrimaryControls")==robotControls;
            printOk=printOk&&printPanel->findChild<QGroupBox*>("welderControls")==weldControls&&printPanel->findChild<QGroupBox*>("welderParameters")==weldParameters;
            printOk=printOk&&twinPanel->parentWidget()==printPanel->poseHost()&&twinPanel->view()->currentPose()==pose&&printScroll->horizontalScrollBar()->maximum()==0&&monitorDock->parentWidget()==printPanel->monitorHost();
            printOk=printOk&&slot->value()==11&&qFuzzyCompare(current->value(),123.4)&&modbus.sessionId()==session&&!read->isEnabled();
            for(int cmd:{1,2,3,4,5,6,7,8,9,11,12}){auto* button=printPanel->findChild<QPushButton*>(QString("plcBridgeCommand%1").arg(cmd));printOk=printOk&&button&&!button->isEnabled();}
            for(auto* button:printPanel->findChildren<QPushButton*>())if(button->objectName().startsWith("welder"))printOk=printOk&&!button->isEnabled();
            trajectory->setChecked(true);settle();printOk=printOk&&twinPanel->view()->trajectory().path==plan.path;
            ok=grab().save(path("-print"))&&ok;
            printScroll->ensureWidgetVisible(twinPanel,0,0);settle();ok=grab().save(path("-print-pose"))&&ok;
            ok=grab().save(path("-print-parameters"))&&ok;
            const QRect printViewport=printScroll->viewport()->rect();bool parametersVisible=true;
            for(auto* editor:printPanel->findChildren<QDoubleSpinBox*>(QRegularExpression("^welderParameter[0-9]+$")))parametersVisible=parametersVisible&&editor->isVisible()&&printViewport.contains(QRect(editor->mapTo(printScroll->viewport(),QPoint()),editor->size()));
            const auto* parameterArea=printPanel->findChild<QScrollArea*>("welderParameterScroll");
            qInfo()<<"Print layout"<<size()<<"page scroll"<<printScroll->horizontalScrollBar()->maximum()<<printScroll->verticalScrollBar()->maximum()<<"parameters scroll"<<parameterArea->verticalScrollBar()->maximum()<<"all parameters visible"<<parametersVisible;
            const bool shortPrintWindow=height()<900;
            printOk=printOk&&weldParameters->isVisible()&&parameterArea->verticalScrollBar()->maximum()==0&&printScroll->horizontalScrollBar()->maximum()==0&&(shortPrintWindow||(parametersVisible&&printScroll->verticalScrollBar()->maximum()==0));
            QList<QWidget*> printTargets;QJsonArray printUnreachableTargets;
            for(auto* editor:printPanel->findChildren<QDoubleSpinBox*>(QRegularExpression("^welderParameter[0-9]+$")))printTargets<<editor;
            for(const auto& name:QStringList{"welderAllOff","welderRead","welderApply","welderControl0","welderControl1","welderControl2","welderControl3","plcBridgeCommand2","plcBridgeCommand3","plcBridgeCommand4","plcBridgeCommand5","plcBridgeCommand6","plcBridgeCommand7"})printTargets<<printPanel->findChild<QPushButton*>(name);
            for(auto* target:printTargets){
                if(!target){printOk=false;continue;}
                if(shortPrintWindow){const int top=target->mapTo(printScroll->widget(),QPoint()).y();printScroll->verticalScrollBar()->setValue(top-(printViewport.height()-target->height())/2);settle();}
                const QRect targetRect(target->mapTo(printScroll->viewport(),QPoint()),target->size());
                const bool reachable=target->isVisible()&&printViewport.contains(targetRect);
                if(!reachable)printUnreachableTargets.append(QJsonObject{{"widget",target->objectName()},{"x",targetRect.x()},{"y",targetRect.y()},{"width",targetRect.width()},{"height",targetRect.height()}});
                printOk=printOk&&reachable;
            }
            if(shortPrintWindow){printScroll->verticalScrollBar()->setValue(printScroll->verticalScrollBar()->maximum());settle();ok=grab().save(path("-print-end"))&&ok;}
            printScroll->verticalScrollBar()->setValue(0);
            workTabs->setCurrentWidget(plcPanel);settle();
            printOk=printOk&&plcPanel->findChild<QWidget*>("plcPrimaryControls")==robotControls&&welderPanel->findChild<QGroupBox*>("welderControls")==weldControls&&welderPanel->findChild<QGroupBox*>("welderParameters")==weldParameters;
            printOk=printOk&&slot->value()==11&&qFuzzyCompare(current->value(),123.4)&&twinPanel->parentWidget()==plcPanel->poseHost();
            workTabs->setCurrentWidget(printPanel);settle();printOk=printOk&&twinPanel->parentWidget()==printPanel->poseHost()&&printPanel->findChild<QPushButton*>("welderRead")==read;
            workTabs->setCurrentWidget(welderPanel);settle();printOk=printOk&&twinPanel->parentWidget()==welderPanel->poseHost()&&qFuzzyCompare(current->value(),123.4);
            ok=ok&&printOk;
            RobotModeDialog dialog(modbus,[this]{return !plcManual->engaged()&&welderPanel->client().safeIdle();},this);dialog.show();settle();
            ok=ok&&dialog.findChild<QComboBox*>("robotModeTarget")->count()==3&&!dialog.findChild<QPushButton*>("robotModeApply")->isEnabled();
            ok=dialog.grab().save(path("-mode"))&&ok;dialog.close();
            ok=ok&&!modbus.isActive()&&modbus.sentFrames()==0;
            QFile report(info.path()+"/"+info.completeBaseName()+".json");if(report.open(QIODevice::WriteOnly))report.write(QJsonDocument(QJsonObject{{"passed",ok},{"combined_print_page_passed",printOk},{"print_unreachable_targets",printUnreachableTargets},{"window_width",width()},{"window_height",height()},{"source_width",sourceGroup->width()},{"physical_device_connected",false}}).toJson());
            QCoreApplication::exit(ok?0:44);
        });
    }

    void runPlcManualUiSmoke(const QString& imagePath) {
        linkType->setCurrentIndex(2);
        if(auto* tabs=findChild<QTabWidget*>("workTabs"))tabs->setCurrentWidget(plcManual);
        QTimer::singleShot(250,this,[this,imagePath]{
            const bool safe=!modbus.isActive()&&!plcManual->client().status().valid&&!plcManual->engaged()&&
                !runSidebar->isVisible()&&!sourceGroup->isVisible()&&quickStop->isVisible()&&!quickStop->isEnabled()&&
                monitorScroll->isVisible()&&plcMonitor->findChild<QPlainTextEdit*>("liveEventLog")->isVisible()&&
                !plcManual->findChild<QPushButton*>("manualLoad")->isEnabled()&&
                !plcManual->findChild<QPushButton*>("manualEnable")->isEnabled();
            bool saved=grab().save(imagePath);
            if(auto* motion=plcManual->findChild<QTabWidget*>("manualMotionTabs")){
                motion->setCurrentIndex(1);QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                const QFileInfo info(imagePath);
                saved=grab().save(info.path()+"/"+info.completeBaseName()+"-target.png")&&saved;
            }
            QCoreApplication::exit(safe&&saved?0:1);
        });
    }

    void runPlcBridgeSmoke(const QString& imagePath) {
        linkType->setCurrentIndex(2);
        auto* tabs = findChild<QTabWidget*>("workTabs");
        if (tabs) tabs->setCurrentWidget(plcPanel);
        QTimer::singleShot(250, this, [this, imagePath] {
            bool controlsSafe = !modbus.isActive() && !startButton->isEnabled() && !stopButton->isEnabled() && !loadProgramButton->isEnabled()&&
                !runSidebar->isVisible()&&!sourceGroup->isVisible()&&quickStop->isVisible()&&!quickStop->isEnabled()&&
                monitorScroll->isVisible()&&plcMonitor->findChild<QPlainTextEdit*>("liveEventLog")->isVisible()&&
                !workTabs->isTabVisible(3)&&!workTabs->isTabVisible(4);
            for (int c = 1; c <= 12; ++c) {
                auto* button = plcPanel->findChild<QPushButton*>(QString("plcBridgeCommand%1").arg(c));
                controlsSafe = controlsSafe && button && !button->isEnabled();
            }
            auto* details = plcPanel->findChild<QTabWidget*>("plcBridgeDetailTabs");
            bool rendered = details && details->count() == 5 && grab().save(imagePath);
            if(auto* toggle=plcPanel->findChild<QToolButton*>("plcAdvancedToggle"))toggle->setChecked(true);
            const QStringList suffixes{"io", "analog", "diagnostics", "features", "events"};
            if (details) for (int i = 0; i < details->count(); ++i) {
                details->setCurrentIndex(i); QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                if(auto* scroll=plcPanel->findChild<QScrollArea*>())scroll->ensureWidgetVisible(details,0,0);
                QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                const QFileInfo info(imagePath);
                rendered = grab().save(info.absolutePath() + "/" + info.completeBaseName() + "-" + suffixes.value(i) + ".png") && rendered;
            }
            workTabs->setCurrentIndex(0);QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            controlsSafe=controlsSafe&&sourceGroup->isVisible()&&!runSidebar->isVisible();
            const QFileInfo prepared(imagePath);
            rendered=grab().save(prepared.absolutePath()+"/"+prepared.completeBaseName()+"-preparation.png")&&rendered;
            if(width()>=1800){
                controlsSafe=controlsSafe&&plcPanel->findChild<QPushButton*>("plcBridgeCommand4")->width()<470;
            }
            QCoreApplication::exit(controlsSafe && rendered ? 0 : 41);
        });
    }

    void runFileUploadSmoke(const QString& imagePath) {
        load(nativeTemplate);
        refresh();
        FileUploadDialog dialog(job.files,job.project,"192.168.1.188:502",QDir::tempPath(),this);
        QTimer::singleShot(250,&dialog,[&] {
            auto* files = dialog.findChild<QTableWidget*>("fileUploadFiles");
            auto* upload = dialog.findChild<QPushButton*>("fileUploadStart");
            bool emptyPreserved = false;
            if (files) for (int r = 0; r < files->rowCount(); ++r)
                if (files->item(r,0)->text() == "capture_home.srd" && files->item(r,1)->text() == "0") emptyPreserved = true;
            const QFileInfo image(imagePath);
            const bool ok = FileBridge::validate(job.files).isEmpty() && files && files->rowCount() == 7 &&
                emptyPreserved && upload && upload->isEnabled() && gcodeDeliveryButton->isEnabled() &&
                !modbus.isActive() && !session.active() && dialog.grab().save(imagePath) &&
                grab().save(image.absolutePath() + "/" + image.completeBaseName() + "-main.png");
            dialog.accept(); QCoreApplication::exit(ok ? 0 : 42);
        });
        dialog.exec();
    }

    void experimentEvent(const QString& kind) {
        if (kind == "home_plan_failed") { homePlanPending = false; return; }
        if (!experiment.active()) return;
        if (kind == "start_cancelled" || kind == "queue_timeout") finishExperiment("start_cancelled");
        else if (kind == "send_failed" && !experiment.observedRunning()) finishExperiment("start_unconfirmed");
    }

    void runGcodeDeliverySmoke(const QString& imagePath, bool automatic = false) {
        if (automatic) { load(nativeTemplate); linkType->setCurrentIndex(2); workTabs->setCurrentIndex(0); }
        QTemporaryDir local;
        const auto source = local.filePath("sample.nc");
        { QFile file(source); if (!file.open(QIODevice::WriteOnly)) { QCoreApplication::exit(43); return; }
          file.write("G21 G17 G90 G94 G91.1\nG1 X10 F300\nG3 X0 Y10 I-10 J0\nM30\n"); }
        GcodeDeliveryDialog dialog(nativeTemplate, "192.168.1.188:502", local.filePath("output"), local.filePath("receipts"), this, automatic);
        const auto second = local.filePath("second.nc");
        if (automatic) {
            QFile file(second); file.open(QIODevice::WriteOnly); file.write("G90\nG1 Y20 F300\nM30\n"); file.close();
            connect(&dialog, &GcodeDeliveryDialog::previewChanged, this, [this, &dialog] { applyAutoPreview(dialog); });
            dialog.importSources({source,second});
        } else dialog.importSource(source);
        QTimer::singleShot(250, &dialog, [&] {
            auto* upload = dialog.findChild<QPushButton*>("deliveryUpload");
            auto* save = dialog.findChild<QPushButton*>("deliverySave");
            auto* copy = dialog.findChild<QPushButton*>("deliveryCopyHandoff");
            auto* status = dialog.findChild<QLabel*>("deliveryStatus");
            bool ok = upload && upload->isEnabled() && save && save->isEnabled() && copy && !copy->isEnabled() &&
                status && status->text().contains(automatic?"2个轨迹":"7个") && dialog.result().isEmpty();
            if (save) save->click();
            const auto prepared = automatic?Job{}:readNativeJob(dialog.packagePath());
            ok = ok && (automatic?QFileInfo::exists(dialog.packagePath()):prepared.error.isEmpty() && prepared.files.size() == 7) && dialog.result().isEmpty() &&
                !modbus.isActive() && !session.active() && dialog.grab().save(imagePath);
            if (ok) {
                if (automatic) {
                    applyAutoPreview(dialog);
                    ok = job.automaticPatch && job.files.size() == 6 && !job.runnable() &&
                        !job.files.contains("helix_25mms.srp") && job.sourceIsGcode && job.sourceProgram == "auto_01" &&
                        job.source.join('\n').contains("G1 X10") && sourceGroup->title().contains("G 代码原文") &&
                        sourceTable->rowCount() == job.source.size() && sourcePosition->text().contains(QString::number(job.source.size())) &&
                        sourceTree->topLevelItemCount() == 1 && sourceTree->topLevelItem(0)->childCount() == 3 &&
                        sourceTree->topLevelItem(0)->text(1).contains("补丁") &&
                        !editFileButton->isEnabled() && editFileButton->text().contains("查看") &&
                        !saveProjectButton->isEnabled() && gcodeDeliveryButton->isEnabled() &&
                        sourceHint->text().contains("尚未上传");
                    for (auto* item : sourceTree->findItems("auto_02", Qt::MatchExactly | Qt::MatchRecursive, 0)) {
                        sourceTree->setCurrentItem(item);
                        ok = ok && job.sourceFile == "auto_02.srp" && job.points.size() >= 2 &&
                            job.source.join('\n').contains("msHomeCart");
                    }
                    for (auto* item : sourceTree->findItems("second.nc", Qt::MatchExactly | Qt::MatchRecursive, 0)) {
                        sourceTree->setCurrentItem(item);
                        ok = ok && job.sourceIsGcode && job.sourceProgram == "auto_02" &&
                            job.source.join('\n').contains("G1 Y20") && job.points.last().y == 20 &&
                            sourceTable->item(1, 1)->text() == "G1 Y20 F300";
                    }
                    const auto displayed = job;
                    { QFile file(second); file.open(QIODevice::WriteOnly); file.write("G90\nG1 Y999 F300\nM30\n"); }
                    QFile::remove(second);
                    bool snapshotReused = false;
                    linkType->setCurrentIndex(1);
                    QTimer::singleShot(0, this, [&] {
                        auto* current = qobject_cast<GcodeDeliveryDialog*>(QApplication::activeModalWidget());
                        if (!current || current == &dialog) return;
                        const auto* source = current->findChild<QLineEdit*>("deliverySource");
                        const auto* route = current->findChild<QComboBox*>("deliveryTransport");
                        snapshotReused = source && source->text() == second && route && route->currentIndex() == 1 &&
                            current->preview().source == displayed.source && current->preview().mappedBytes == displayed.mappedBytes &&
                            current->preview().points.last().y == 20 && !session.active() && !modbus.isActive();
                        current->accept();
                    });
                    gcodeDeliveryButton->click();
                    ok = ok && snapshotReused;
                    linkType->setCurrentIndex(2);
                } else openPath(dialog.packagePath());
                refresh();
                const QFileInfo image(imagePath); ok = grab().save(image.absolutePath() + "/" + image.completeBaseName() + "-main.png") && ok; }
            const auto bad = local.filePath("blocked.nc");
            { QFile file(bad); file.open(QIODevice::WriteOnly); file.write("G90\nM3 S500\nG1 X1 F300\nM30\n"); }
            dialog.importSource(bad);
            ok = ok && !upload->isEnabled() && !save->isEnabled() && dialog.packagePath().isEmpty() && dialog.result().isEmpty();
            if (automatic) {
                applyAutoPreview(dialog);
                ok = ok && !job.error.isEmpty() && job.files.isEmpty() && job.points.isEmpty() &&
                    job.source.join('\n').contains("M3 S500") && summaryLabel->text().contains("本次未上传") &&
                    sourceTable->item(1, 1)->text() == "M3 S500";
                openPath(bad);
                ok = ok && job.inputPath == bad && !job.error.isEmpty();
                bool reused = false;
                QTimer::singleShot(0, this, [&] {
                    auto* imported = qobject_cast<GcodeDeliveryDialog*>(QApplication::activeModalWidget());
                    if (!imported || imported == &dialog) return;
                    const auto* sourcePath = imported->findChild<QLineEdit*>("deliverySource");
                    auto* omitOutput = imported->findChild<QCheckBox*>("deliveryOmitProcess");
                    reused = sourcePath && sourcePath->text() == bad && omitOutput;
                    if (omitOutput) omitOutput->setChecked(true);
                    reused = reused && job.error.isEmpty() && job.automaticPatch && job.sourceIsGcode &&
                        job.source.join('\n').contains("M3 S500") && job.points.last().x == 1 &&
                        imported->result().isEmpty() && !modbus.isActive() && !session.active();
                    imported->accept();
                });
                gcodeDeliveryButton->click();
                ok = ok && reused;
            }
            dialog.accept(); QCoreApplication::exit(ok ? 0 : 43);
        });
        dialog.exec();
    }

    void runTrajectorySmoke(const QString& source, const QString& imagePath) {
        linkType->setCurrentIndex(2);workTabs->setCurrentIndex(0);
        const auto automatic=Delivery::compileAuto({source},nativeTemplate,{5,.05,true});
        load(Delivery::autoPreview(automatic));
        findChild<QCheckBox*>("twinSceneShowTrajectory")->setChecked(true);
        findChild<QComboBox*>("twinSceneFocus")->setCurrentIndex(0);twinScene->fitScene();
        QTimer::singleShot(350,this,[this,automatic,imagePath]{
            bool ok=job.error.isEmpty()&&!modbus.isActive()&&!session.active()&&job.points.size()==354&&job.sourceIsGcode;
            QVector3D lo,hi;for(const auto& p:job.points){lo.setX(std::min(lo.x(),float(p.x)));lo.setY(std::min(lo.y(),float(p.y)));lo.setZ(std::min(lo.z(),float(p.z)));hi.setX(std::max(hi.x(),float(p.x)));hi.setY(std::max(hi.y(),float(p.y)));hi.setZ(std::max(hi.z(),float(p.z)));}
            ok=ok&&(hi-lo-QVector3D(36,36,36)).length()<.001;
            const auto generated=QString::fromUtf8(automatic.files.value("auto_01.srd"));
            QRegularExpression offset("POSOFFSET p[0-9]+ := \\{([^,]+),([^,]+),([^,]+),0,0,0\\}");
            auto matches=offset.globalMatch(generated);int index=1;bool coordinatesMatch=true;
            while(matches.hasNext()){const auto match=matches.next();if(index>=job.points.size()){coordinatesMatch=false;break;}const auto expected=job.points[index++];coordinatesMatch=coordinatesMatch&&std::abs(match.captured(1).toDouble()-expected.x)<1e-7&&std::abs(match.captured(2).toDouble()-expected.y)<1e-7&&std::abs(match.captured(3).toDouble()-expected.z)<1e-7;}
            coordinatesMatch=coordinatesMatch&&index==job.points.size();ok=ok&&coordinatesMatch;
            const auto original=twinScene->trajectory().relative;QJsonArray frames;
            const QFileInfo output(imagePath);
            auto verify=[&](TwinView* view,const QString& label){
                for(int pass=0;pass<4;++pass)QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                view->fitScene();const auto projected=view->projectedTrajectory();QRectF bounds;
                if(!projected.isEmpty()){bounds=QRectF(projected.first(),QSizeF());for(const auto& p:projected){bounds.setLeft(std::min(bounds.left(),p.x()));bounds.setRight(std::max(bounds.right(),p.x()));bounds.setTop(std::min(bounds.top(),p.y()));bounds.setBottom(std::max(bounds.bottom(),p.y()));}}
                view->grabFramebuffer();const auto frame=view->grab().toImage();int bright=0;for(int y=0;y<frame.height();++y)for(int x=0;x<frame.width();++x){const auto c=frame.pixelColor(x,y);bright+=c.red()>220&&c.green()>125&&c.green()<225&&c.blue()<100;}
                const bool visible=view->trajectoryFocus()&&projected.size()==354&&bounds.height()>view->height()*.45&&bounds.width()>view->width()*.22&&QRectF(view->rect().adjusted(8,55,-8,-35)).contains(bounds)&&bright>500;
                ok=ok&&visible;frames.append(QJsonObject{{"case",label},{"vertices",projected.size()},{"width_px",bounds.width()},{"height_px",bounds.height()},{"viewport_width",view->width()},{"viewport_height",view->height()},{"bright_pixels",bright},{"visible",visible}});
                const auto file=output.path()+"/"+output.completeBaseName()+"-"+label+".png";ok=view->grab().save(file)&&ok;
            };
            verify(twinScene,"detail");ok=grab().save(imagePath)&&ok;
            const auto onFrame=twinScene->grabFramebuffer();findChild<QCheckBox*>("twinSceneShowTrajectory")->setChecked(false);QApplication::processEvents();const auto offFrame=twinScene->grabFramebuffer();
            ok=ok&&twinScene->trajectory().relative==original&&onFrame!=offFrame;
            findChild<QCheckBox*>("twinSceneShowTrajectory")->setChecked(true);
            twinPanel->setBackgroundPreset(3);verify(twinScene,"dark");twinPanel->setBackgroundPreset(0);
            findChild<QComboBox*>("twinSceneFocus")->setCurrentIndex(1);ok=ok&&!twinScene->trajectoryFocus()&&twinScene->trajectory().relative==original;
            ok=grab().save(output.path()+"/"+output.completeBaseName()+"-robot.png")&&ok;
            workTabs->setCurrentIndex(2);refreshWorkspaceLayout();twinPanel->findChild<QAction*>("twinFocusTrajectory")->trigger();verify(twinPanel->view(),"compact");
            QFile report(output.path()+"/"+output.completeBaseName()+".json");if(report.open(QIODevice::WriteOnly))report.write(QJsonDocument(QJsonObject{{"source",automatic.sources.value(0)},{"segments",353},{"bounds_mm",QJsonArray{36,36,36}},{"path_matches_generated_srd",coordinatesMatch},{"device_writes",0},{"frames",frames},{"passed",ok}}).toJson());else ok=false;
            QTimer::singleShot(100,this,[this,ok]{close();QCoreApplication::exit(ok?0:44);});
        });
    }

    void runSmoke(bool simulate, bool manualOnly = false, bool sourceOnly = false) {
        const QStringList source{"G21 G17 G91", "G01 Z1 F300", "G04 T1", "G02 I0.5 J0 Z1 F300", "M30"};
        auto valid = compileGcodeJob("selftest.nc", source, 1, nativeTemplate);
        auto invalid = compileGcodeJob("blocked.nc", {"G91", "G64 P0.1", "M3 S500", "M30"}, 1, nativeTemplate);
        if (!nativeTemplate.reviewed || !valid.runnable() || valid.points.size() != 74 || invalid.runnable() || invalid.diagnostics.size() != 2) {
            qWarning() << nativeTemplate.error << valid.error << valid.points.size() << invalid.diagnostics;
            QCoreApplication::exit(11);
            return;
        }
        const QStringList generalSource{"G21 G17 G90 G94 G91.1", "G0 X1", "G1 X2 F300", "G3 X1 Y1 I-1 J0", "X0 Y0 I0 J-1", "G91 G1 Z1", "G4 P0.1", "M2"};
        const auto general = compileGcodeJob("absolute_modal.nc", generalSource, 1, nativeTemplate);
        const auto arcError = compileGcodeJob("bad_arc.nc", {"G90 G1 X1 F300", "G2 X3 Y2 I0 J1", "M30"}, 1, nativeTemplate);
        const auto omitted = compileGcodeJob("dry.nc", {"G90", "M3 S800", "G1 Z1 F300", "M5", "M30"}, 1, nativeTemplate, {5, .05, true});
        if (!general.runnable() || general.points.last().x != 0 || general.points.last().y != 0 || general.points.last().z != 1 ||
            general.files.size() != 5 || general.files.contains("go_home.srp") || arcError.runnable() || !omitted.runnable() ||
            std::count_if(omitted.steps.cbegin(), omitted.steps.cend(), [](const Step& step) { return step.skipped; }) != 2) {
            qWarning() << "Expanded converter failed" << general.error << arcError.error << omitted.error;
            QCoreApplication::exit(30); return;
        }
        auto edited = nativeTemplate;
        showNativeFile(edited, "_project.srd");
        if (edited.sourceFile != "_project.srd" || sourceRowForNativeLine(edited, edited.project, "_project", 1)) { QCoreApplication::exit(31); return; }
        const auto replacement = edited.files.value("go_home.srp") + "# Local edit test\n";
        QTemporaryDir editOutput;
        if (!updateNativeFile(edited, "go_home.srp", replacement).isEmpty() || edited.reviewed || edited.runnable() || !edited.modified || !edited.points.isEmpty() ||
            sourceRowForNativeLine(edited, edited.project, "go_home", 1) ||
            !exportStepJob(edited, editOutput.filePath("edited.zip")).isEmpty()) { QCoreApplication::exit(32); return; }
        const auto restored = readNativeJob(editOutput.filePath("edited.zip"));
        if (restored.files.size() != nativeTemplate.files.size() || restored.files.value("go_home.srp") != replacement ||
            restored.files.value("capture_home.srp") != nativeTemplate.files.value("capture_home.srp")) { QCoreApplication::exit(33); return; }
        load(std::move(valid));
        QTemporaryDir exported;
        const auto package = exported.filePath("converted.zip");
        if (!exported.isValid() || !exportStepJob(job, package).isEmpty() || readNativeJob(package).files != job.files ||
            !QFileInfo::exists(package + ".mapping.json")) {
            QCoreApplication::exit(17);
            return;
        }
        auto linked = readNativeJob(package);
        const auto first = job.steps.first();
        if (!linked.nativeInput || !linked.sourceIsGcode || linked.source != source || linked.runnable() ||
            sourceRowForNativeLine(linked, linked.project, linked.sourceProgram, first.firstLine) != first.row ||
            sourceRowForNativeLine(linked, "wrong.sr", linked.sourceProgram, first.firstLine) ||
            sourceRowForNativeLine(linked, linked.project, "wrong", first.firstLine)) {
            qWarning() << "Native G-code sidecar round-trip failed" << linked.mappingWarning;
            QCoreApplication::exit(23); return;
        }
        if (!restoreMappedPreview(linked, nativeTemplate) || linked.points.size() != job.points.size()) { QCoreApplication::exit(35); return; }
        showNativeSource(linked, linked.program);
        const auto redisplay = exported.filePath("browsed_native.zip");
        if (!exportStepJob(linked, redisplay).isEmpty() || !readNativeJob(redisplay).sourceIsGcode) { QCoreApplication::exit(38); return; }
        if (linked.sourceIsGcode || sourceRowForNativeLine(linked, linked.project, linked.sourceProgram, 1) != 1 ||
            sourceRowForNativeLine(linked, linked.project, linked.sourceProgram, linked.source.size() + 1)) {
            QCoreApplication::exit(24); return;
        }
        QFile sidecar(package + ".mapping.json");
        sidecar.open(QIODevice::ReadOnly);
        const auto originalMap = QJsonDocument::fromJson(sidecar.readAll()).object();
        sidecar.close();
        auto reject = [&](QJsonObject altered) {
            QFile file(exported.filePath("bad.mapping.json"));
            if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(altered).toJson()) < 0) return false;
            file.close();
            return !associateNativeGcode(linked, file.fileName()).isEmpty() && !linked.sourceIsGcode;
        };
        auto changedHash = originalMap;
        changedHash.insert("zip_sha256", "different");
        auto changedSource = originalMap;
        changedSource.insert("source_sha256", "different");
        auto overlapping = originalMap;
        auto entries = overlapping.value("source_map").toArray();
        auto duplicate = entries.first().toObject();
        duplicate.insert("source_line", 1);
        entries.append(duplicate);
        overlapping.insert("source_map", entries);
        if (!reject(changedHash) || !reject(changedSource) || !reject(overlapping)) { QCoreApplication::exit(25); return; }
        const auto rawLine = nativeTemplate.source.indexOf("        Lin(cpOrigin,dynPath,NULL,NULL,Default,WORLD);") + 1;
        if (!nativeTemplate.nativeInput || nativeTemplate.source.isEmpty() || rawLine < 1 ||
            sourceRowForNativeLine(nativeTemplate, nativeTemplate.project, nativeTemplate.program, rawLine) != rawLine) {
            QCoreApplication::exit(26); return;
        }
        load(nativeTemplate);
        const auto nativeSnapshot = qEnvironmentVariable("STEP_NATIVE_SOURCE_SNAPSHOT");
        if (sourceTable->rowCount() != nativeTemplate.source.size() || sourceGroup->title() != "STEP / SRP 原文" ||
            sourceTree->topLevelItemCount() != 1 || sourceTree->topLevelItem(0)->childCount() != 4 ||
            plot->grab().toImage().pixelColor(5, 5) != QColor("#f7fafc") ||
            (!nativeSnapshot.isEmpty() && !grab().save(nativeSnapshot))) { QCoreApplication::exit(27); return; }
        load(compileGcodeJob("selftest.nc", source, 1, nativeTemplate));
        qInfo() << "Native SRP source, verified G-code round-trip, mismatched hashes and ambiguous mapping checks passed";
        if (!simulate) {
            const auto converterSnapshot = qEnvironmentVariable("STEP_CONVERTER_SNAPSHOT");
            if (!converterSnapshot.isEmpty()) {
                bool captured = false;
                QTimer::singleShot(100, this, [&] {
                    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    if (!dialog) return;
                    auto* check = dialog->findChild<QPushButton*>("convertCheck");
                    auto* save = dialog->findChild<QPushButton*>("convertExport");
                    if (check && save) { check->click(); captured = save->isEnabled() && dialog->grab().save(converterSnapshot); }
                    dialog->accept();
                });
                openConversionDialog(this, nativeTemplate, workspace, general.inputBytes);
                if (!captured) { QCoreApplication::exit(34); return; }
            }
            const auto snapshot = qEnvironmentVariable("STEP_WORKSTATION_SNAPSHOT");
            const bool layout = sourceTable->rowCount() == source.size() && !stopButton->isEnabled() &&
                ip->text() == DefaultCabinetEndpoint && ip->isEnabled() && !ip->isReadOnly();
            QCoreApplication::exit(layout && (snapshot.isEmpty() || grab().save(snapshot)) ? 0 : 12);
            return;
        }
        recordRoot = QDir(workspace).filePath("tmp/workstation_experiment_verification");
        experimentLabel->setText("原生回归试次");
        const auto generalZip = exported.filePath("general.zip");
        if (!exportStepJob(general, generalZip).isEmpty()) { QCoreApplication::exit(36); return; }
        auto testJob = readNativeJob(generalZip);
        if (!restoreMappedPreview(testJob, nativeTemplate) || !testJob.trustedGcode || !testJob.runnable()) { QCoreApplication::exit(37); return; }
        load(Job{});
        session.prepare(job);
        auto* test = new QTimer(this);
        connect(test, &QTimer::timeout, this, [this, testJob, sourceOnly, phase = manualOnly ? 2 : -1, issued = false, baseline = QString(), frameIndex = 0, jogPhase = 0]() mutable {
            if (phase == -1 && session.fresh()) {
                if (!canOpenFile() || !openButton->property("primary").toBool()) { QCoreApplication::exit(20); return; }
                selectJob(testJob);
                phase = 0;
            }
            if (phase < 2 && !issued && canStart()) {
                if (!startButton->property("primary").toBool()) { QCoreApplication::exit(21); return; }
                if (phase == 0) {
                    const auto writable = recordRoot;
                    recordRoot = QCoreApplication::applicationFilePath();
                    const bool rejected = !queueExperiment() && session.pending.isEmpty();
                    recordRoot = writable;
                    if (!rejected) { QCoreApplication::exit(15); return; }
                }
                issued = queueExperiment();
            }
            if (phase == 0 && session.value("outcome") == "completed") {
                if (!sawSource || donePoint != job.points.size() - 1 || sourceTable->rowCount() != testJob.source.size() ||
                    !verifyExperiment("completed", true)) {
                    QCoreApplication::exit(13);
                    return;
                }
                qInfo() << "G-code mapping, storage gate and completed experiment archive passed";
                session.disconnect();
                ++phase;
                issued = false;
                load(nativeTemplate);
                prepare();
            } else if (phase == 1 && issued && session.value("state") == "0" && stopButton->isEnabled() &&
                       current > 0 && job.source.value(current - 1).trimmed().startsWith("Lin(cpOrigin,")) {
                if (current != session.value("line").toInt() || sourceTable->item(current - 1, 0)->background().color() != QColor("#ffebad")) {
                    QCoreApplication::exit(28); return;
                }
                const auto snapshot = qEnvironmentVariable("STEP_NATIVE_RUNNING_SNAPSHOT");
                if (!snapshot.isEmpty()) grab().save(snapshot);
                qInfo() << "Native controller line matched yellow SRP source row" << current;
                stopButton->click();
            } else if (phase == 1 && session.value("outcome") == "stopped" && !session.waitingStop) {
                if (!verifyExperiment("stopped", false)) { QCoreApplication::exit(16); return; }
                if (!verifyPairedHighlight()) { QCoreApplication::exit(29); return; }
                qInfo() << "Reviewed helix ZIP and stopped experiment archive passed";
                session.disconnect();
                if (sourceOnly) { QCoreApplication::exit(0); return; }
                ++phase;
                issued = false;
                load(Job{});
                session.prepare(job);
            } else if (phase == 2 && session.fresh() && session.control && session.value("mode") == "1") {
                const QStringList frames{"joints", "world", "base", "tool", "user SimPart", "world", "world"};
                if (issued && session.pending.isEmpty()) {
                    if (jogPhase == 0) {
                        const int reported = session.value("jogref").toInt();
                        const int resolved = reported == 4 && session.value("refname") == "ROBOTBASE" ? 2 : reported;
                        if (resolved != (frameIndex >= 5 ? 1 : frameIndex)) {
                            qWarning() << "Frame selection failed" << frames[frameIndex] << logs->toPlainText().right(1200);
                            QCoreApplication::exit(18);
                            return;
                        }
                        ++jogPhase;
                    } else {
                        if ((frameIndex <= 5 && (session.value("axes") == baseline || session.value("jogset").toDouble() <= 0)) ||
                            (frameIndex == 6 && session.value("axes") != baseline) || std::abs(session.value("speed").toDouble()) >= .01) {
                            qWarning() << "Direct speed jog failed" << frames[frameIndex] << logs->toPlainText().right(1200);
                            QCoreApplication::exit(19);
                            return;
                        }
                        qInfo() << "Direct manual jog passed" << frames[frameIndex] << session.value("jogset") << session.value("jogunit");
                        ++frameIndex;
                        jogPhase = 0;
                    }
                    issued = false;
                }
                if (frameIndex == 7) {
                    if (streamedJogSamples < 5) { QCoreApplication::exit(22); return; }
                    if (auto* tabs = findChild<QTabWidget*>("workTabs")) tabs->setCurrentWidget(plcPanel);
                    const auto snapshot = qEnvironmentVariable("STEP_WORKSTATION_SNAPSHOT");
                    if (!snapshot.isEmpty()) grab().save(snapshot);
                    session.disconnect();
                    QCoreApplication::exit(0);
                } else if (!issued) {
                    if (jogPhase == 0) issued = session.request(frames[frameIndex]);
                    else {
                        baseline = session.value("axes");
                        const auto code = QString("JWBTUWW").mid(frameIndex, 1);
                        issued = session.request(QString("jog %1 1+ %2").arg(code, frameIndex == 0 ? "1.0" : frameIndex == 5 ? "250.0" : frameIndex == 6 ? "250.01" : "12.5"));
                    }
                }
            }
        });
        test->start(80);
        QTimer::singleShot(61000, this, [this] {
            if (session.connected()) session.stop();
            QCoreApplication::exit(14);
        });
    }

    void openPath(const QString& path) {
        if (!path.isEmpty()) selectJob(path.endsWith(".zip", Qt::CaseInsensitive) ? readNativeJob(path) : readGcodeJob(path, nativeTemplate));
    }

    void runHomeSmoke() {
        load(Job{});
        session.prepare(job);
        auto* test = new QTimer(this);
        connect(test, &QTimer::timeout, this, [this, phase = 0, baseline = QString(), excursion = QString(), movingFrom = QString(), stopped = false, peakTCP = 0.]() mutable {
            if (!session.fresh() || session.waitingStop) return;
            if (session.value("home_returning") == "1") {
                bool ok = false;
                const double speed = session.value("speed").toDouble(&ok);
                const double cap = session.value("home_plan_speed").toDouble();
                if (!ok || !std::isfinite(speed) || cap <= 0 || cap > 250 || std::abs(speed) > cap || session.value("return_unit") != "mm_s") {
                    session.stop();
                    qWarning() << "TCP return speed or unit check failed";
                    QCoreApplication::exit(56); return;
                }
                peakTCP = std::max(peakTCP, std::abs(speed));
            }
            if (!session.pending.isEmpty() && phase != 8) return;
            const auto axes = session.value("axes");
            if (phase == 0) {
                baseline = axes;
                const auto key = originDeviceKey(true, session.model, session.identity, session.address);
                QMap<QString, QString> feedback;
                for (const auto& field : {"motion_rest", "toolname", "axes", "xyz", "abc"}) feedback.insert(field, session.value(field));
                const auto saved = originFromFeedback(feedback, key, session.model);
                QTemporaryDir config;
                if (!saved || !saveWorkstationOrigin(config.filePath("origins.json"), *saved).isEmpty() ||
                    !readWorkstationOrigin(config.filePath("origins.json"), key) ||
                    readWorkstationOrigin(config.filePath("origins.json"), originDeviceKey(true, session.model, "other", session.address))) {
                    qWarning() << "Workstation-origin persistence or controller isolation failed";
                    QCoreApplication::exit(41); return;
                }
                auto target = baseline.split(',');
                bool ok = false;
                const double firstAxis = target.value(0).toDouble(&ok);
                if (!ok || target.size() != 6) { QCoreApplication::exit(42); return; }
                target[0] = QString::number(firstAxis + .1, 'f', 6);
                excursion = target.join(',');
                QString command = "planhome_tcp 100.0";
                for (const auto& value : target) command += " " + value;
                if (!session.request(command)) { QCoreApplication::exit(42); return; }
                phase = 1;
                return;
            }
            if (!session.value("home_plan_error").isEmpty() && session.value("home_plan_error") != "none") {
                qWarning() << "Home smoke planning failed" << session.value("home_plan_error");
                QCoreApplication::exit(43); return;
            }
            if (phase >= 2 && session.value("home_return_result") == "unconfirmed" && session.value("motion_rest") == "1") {
                qWarning() << "Home smoke return was not confirmed" << logs->toPlainText().right(1800);
                session.disconnect();
                QCoreApplication::exit(55); return;
            }
            if (phase == 1 && session.value("home_plan_ready") == "1" && !session.value("home_path").isEmpty()) {
                qInfo() << "Home smoke outbound requested, TCP cap 100 mm/s";
                if (!session.request("returnhome " + session.value("home_plan_id"))) { QCoreApplication::exit(44); return; }
                phase = 2;
                return;
            }
            if (phase == 2 && session.value("home_return_result") == "arrived" && session.value("motion_rest") == "1") {
                qInfo() << "Home smoke outbound arrived";
                const auto target = excursion.split(','), actual = axes.split(',');
                bool reached = target.size() == 6 && actual.size() == 6;
                for (int i = 0; reached && i < 6; ++i) reached = std::abs(actual[i].toDouble() - target[i].toDouble()) <= .02;
                if (!reached) { QCoreApplication::exit(45); return; }
                QString command = "planhome_tcp 100.0";
                for (const auto& value : baseline.split(',')) command += " " + value;
                if (!session.request(command)) { QCoreApplication::exit(46); return; }
                phase = 3;
                return;
            }
            if (phase == 3 && session.value("home_plan_ready") == "1" && !session.value("home_path").isEmpty()) {
                if (session.value("program") != "" || !session.request("returnhome " + session.value("home_plan_id"))) {
                    qWarning() << "Independent return plan depended on a loaded project";
                    QCoreApplication::exit(47); return;
                }
                phase = 4;
                return;
            }
            if (phase == 4 && session.value("home_return_result") == "arrived" && session.value("motion_rest") == "1") {
                qInfo() << "Home smoke returned to the saved target";
                const auto target = baseline.split(','), actual = axes.split(',');
                bool reached = target.size() == 6 && actual.size() == 6;
                for (int i = 0; reached && i < 6; ++i) reached = std::abs(actual[i].toDouble() - target[i].toDouble()) <= .02;
                if (!reached) { QCoreApplication::exit(48); return; }
                QString command = "planhome_tcp 25.0";
                for (const auto& value : excursion.split(',')) command += " " + value;
                if (!session.request(command)) { QCoreApplication::exit(49); return; }
                phase = 5;
                return;
            }
            if (phase == 5 && session.value("home_plan_ready") == "1" && !session.value("home_path").isEmpty()) {
                movingFrom = axes;
                if (!session.request("returnhome " + session.value("home_plan_id"))) { QCoreApplication::exit(50); return; }
                phase = 8;
                return;
            }
            if (phase == 8 && session.value("home_returning") == "1" &&
                std::abs(axes.section(',', 0, 0).toDouble() - movingFrom.section(',', 0, 0).toDouble()) >= .005 && !stopped) {
                if (!stopButton->isEnabled() || session.canClose()) { QCoreApplication::exit(53); return; }
                session.stop();
                stopped = true;
                return;
            }
            if (phase == 8 && stopped && session.value("home_returning") != "1" &&
                session.value("home_return_result") == "stopped" && session.value("motion_rest") == "1") {
                const auto start = movingFrom.split(','), actual = axes.split(',');
                if (start.size() != 6 || actual.size() != 6 || std::abs(actual[0].toDouble() - start[0].toDouble()) < .001) {
                    QCoreApplication::exit(54); return;
                }
                qInfo() << "Home smoke passed: persistent device-scoped origin, project-independent PTP return, TCP caps 100/25 mm/s, and mid-motion stop; peak TCP mm/s =" << peakTCP;
                session.disconnect();
                QCoreApplication::exit(0);
            }
        });
        test->start(80);
        QTimer::singleShot(120000, this, [this] {
            qWarning() << "Home smoke timed out" << logs->toPlainText().right(3500);
            if (session.connected()) session.stop();
            QTimer::singleShot(2500, this, [this] {
                if (session.canClose()) session.disconnect();
                QCoreApplication::exit(48);
            });
        });
    }

    bool verifyPairedHighlight() {
        const int nativeRow = session.value("line").toInt();
        if (!job.nativeInput || !job.source.value(nativeRow - 1).trimmed().startsWith("Lin(cpOrigin,")) return false;
        QTemporaryDir folder;
        auto paired = job;
        paired.sourceIsGcode = true;
        paired.mappedBytes = "G91\nG01 X100 F1500\n";
        paired.mappedSource = {"G91", "G01 X100 F1500"};
        paired.mappedProgram = job.program;
        paired.steps = {Step{2, nativeRow, nativeRow, 0, 0, 0, 0}};
        paired.mappedSteps = paired.steps;
        const auto package = folder.filePath("paired.zip");
        if (!folder.isValid() || !exportStepJob(paired, package).isEmpty()) return false;
        QFile mapping(package + ".mapping.json");
        if (!mapping.open(QIODevice::ReadOnly)) return false;
        auto manifest = QJsonDocument::fromJson(mapping.readAll()).object();
        mapping.close();
        manifest.insert("zip_sha256", QString::fromLatin1(QCryptographicHash::hash(job.inputBytes, QCryptographicHash::Sha256).toHex()));
        const auto sourcePath = folder.filePath("paired.gcode");
        QFile source(sourcePath), sidecar(sourcePath + ".mapping.json");
        if (!source.open(QIODevice::WriteOnly) || source.write(paired.mappedBytes) != paired.mappedBytes.size() ||
            !sidecar.open(QIODevice::WriteOnly) || sidecar.write(QJsonDocument(manifest).toJson()) < 0) return false;
        source.close();
        sidecar.close();
        auto associated = job;
        const auto error = associateNativeGcode(associated, sourcePath);
        if (!error.isEmpty()) { qWarning() << error; return false; }
        load(std::move(associated));
        sample();
        refresh();
        const auto snapshot = qEnvironmentVariable("STEP_NATIVE_MAPPED_SNAPSHOT");
        if (current != 2 || sourcePosition->text() != "L2 / 2" || sourceTable->item(1, 0)->background().color() != QColor("#ffebad") ||
            (!snapshot.isEmpty() && !grab().save(snapshot))) return false;
        qInfo() << "External paired G-code highlighted L2 from paused native controller line" << nativeRow;
        return true;
    }

    bool verifyExperiment(const QString& expected, bool expectEndpoint) const {
        const QDir folder(experiment.folder());
        QFile summary(folder.filePath("run.json")), input(folder.filePath(job.nativeInput ? "input.zip" : "input.gcode")),
              trace(folder.filePath("trace.csv"));
        if (experiment.active() || !summary.open(QIODevice::ReadOnly) || !input.open(QIODevice::ReadOnly) || !trace.open(QIODevice::ReadOnly)) return false;
        const auto manifest = QJsonDocument::fromJson(summary.readAll()).object();
        const auto original = input.readAll();
        if (manifest.value("result").toString() != expected || manifest.value("capture_incomplete").toBool() ||
            manifest.contains("endpoint_distance_mm") != expectEndpoint || original != job.inputBytes ||
            manifest.value("input_sha256").toString() != QString::fromLatin1(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex())) return false;
        if (expectEndpoint && (manifest.value("distance_samples").toInt() <= 0 || manifest.value("endpoint_distance_mm").toDouble() > .05)) return false;
        const auto columns = trace.readLine().count(',');
        int count = 0;
        while (!trace.atEnd()) {
            if (trace.readLine().count(',') != columns) return false;
            ++count;
        }
        return count >= 2 && count == manifest.value("samples").toInt() && folder.exists("plan.csv") &&
            folder.exists("program/" + job.program + ".srp") && folder.exists("events.log");
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QMainWindow::resizeEvent(event);
        if(workTabs&&twinPanel)QTimer::singleShot(0,this,[this]{refreshWorkspaceLayout();});
    }
    void closeEvent(QCloseEvent* event) override {
        if (plcManual && plcManual->engaged()) {
            plcManual->halt(true);
            if (!plcManual->safeToClose()) { log("正在撤销PLC微调，请等待停稳回执再关闭。"); event->ignore(); return; }
        }
        if (!session.canClose()) {
            log("会话仍在运行或状态待确认，请先停止并核对控制器状态。");
            event->ignore();
            return;
        }
        if (!confirmLocalChanges()) { event->ignore(); return; }
        {
            auto settings = workstationSettings();
            const int type = linkType->currentIndex();
            settings.setValue("link/type", type);
            if (type == 2) settings.setValue("plcBridge/address", ip->text());
            else settings.setValue("cabinet/address", ip->text());
            settings.setValue("plcBridge/programSlot", programNumber->value());
        }
        session.disconnect();
        if (modbus.isActive()) modbus.disconnectFrom();
        event->accept();
    }

private:
    void build() {
        auto* base = new QWidget;
        setCentralWidget(base);
        auto* root = new QVBoxLayout(base);
        root->setContentsMargins(12, 8, 12, 8);
        root->setSpacing(8);
        auto* toolbar = new QHBoxLayout;
        root->addLayout(toolbar);
        auto* appTitle=new QLabel("STEP 打印作业台");appTitle->setObjectName("appTitle");toolbar->addWidget(appTitle);toolbar->addSpacing(12);
        openButton = new QPushButton("打开文件…");
        openButton->setToolTip("打开 G 代码或 STEP 工程 ZIP");
        toolbar->addWidget(openButton);
        connect(openButton, &QPushButton::clicked, this, &Workstation::open);
        auto* more = new QToolButton;
        more->setText("工程 / 工具");
        more->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(more);
        auto* projectMenu = menu->addMenu("工程文件");
        projectMenu->addAction("G代码仅转换 / STEPStudio…", this, [this] {
            const auto output = openConversionDialog(this, nativeTemplate, workspace, job.nativeInput ? QByteArray() : job.inputBytes);
            if (!output.isEmpty()) log("转换工程已导出，下一步在 STEPStudio 验证：" + output);
        });
        exportAction = projectMenu->addAction("另存完整 STEP 工程 ZIP…", this, &Workstation::exportJob);
        exportAction->setToolTip("导出后台生成的 SRP/SRD，供 STEPStudio 导入验证。另存源行映射与哈希。");
        saveSourceAction = projectMenu->addAction("导出当前文件原文…", this, &Workstation::saveSource);
        auto* mappingMenu = menu->addMenu("G 代码联动");
        associateAction = mappingMenu->addAction("关联 G 代码及行映射…", this, &Workstation::associateGcode);
        sampleAction = menu->addAction("载入螺旋示例", this, [this] { selectJob(nativeTemplate); });
        menu->addAction("打开实验记录目录", this, [this] {
            if (QDir().mkpath(recordRoot)) QDesktopServices::openUrl(QUrl::fromLocalFile(recordRoot));
            else log("无法打开实验记录目录。");
        });
        menu->addAction("浏览历史试次 / 轨迹回放…",this,[this]{RunBrowserDialog(recordRoot,this).exec();});
        menu->addAction("原有功能现在在哪里？",this,[this]{QMessageBox::information(this,"功能位置与连接方式",
            "① 作业准备：工程文件、原文、变量文件、轨迹预览与上传。\n\n"
            "③ 自动运行：登记、加载确认、使能运行；下方保留 I/O、坐标、寄存器、条件和事件五个标签。\n\n"
            "右侧监视与日志：设备模式与许可、TCP 坐标、当前请求和最近回执；顶部按钮可显示或收起。\n\n"
            "② 手动与起点：登记、关节 / WORLD 微调、定量单步、捕捉与返回起点。\n\n"
            "SDK 点动、六轴遥测和试次进度用于 SDK / 虚拟控制器方式，切换到对应方式后仍保留。PLC 现有接口没有同样的全部数据。\n\n"
            "历史试次与本地图像在本菜单；原相机页仅是本地图像查看器，没有接入实时相机。"
        );});
        menu->addAction("查看本地图像…",this,[this]{
            const auto path=QFileDialog::getOpenFileName(this,"打开本地图像",{},"图像 (*.png *.jpg *.jpeg *.bmp)");
            if(path.isEmpty())return;
            QDialog dialog(this);dialog.setWindowTitle(QFileInfo(path).fileName());dialog.resize(960,680);
            auto* layout=new QVBoxLayout(&dialog);auto* image=new QLabel;image->setAlignment(Qt::AlignCenter);
            image->setPixmap(QPixmap(path).scaled(920,620,Qt::KeepAspectRatio,Qt::SmoothTransformation));layout->addWidget(image);dialog.exec();
        });
        menu->addAction("界面与工程流程说明…", this, [this] {
            QMessageBox::information(this, "界面与工程流程",
                "工程文件：完整显示主程序、前置程序及 SRD 变量。旧工程可能自带 go_home，本机回程由上位机独立规划，不调用该文件。选择文件查看，点「编辑文件」修改本地副本，再另存 ZIP。\n\n"
                "转换流程：G 代码转 STEP 工程 → STEPStudio 仿真验证 → 导入同版本 ZIP。映射文件用于关联 G 代码和原生行号。\n\n"
                "WORLD / 相对起点：以记录的 TCP 起点为零，轴向平行世界坐标系，单位 mm。等轴、XY、XZ 是观察角度。\n\n"
                "绿线是已走过路径，黄线是当前段，灰线待走；蓝色虚线是控制器回读轨迹，十字圆标记是当前 TCP。\n\n"
                "仿真示例和实验记录目录也在本菜单。软件「停止发送」使用普通 STOP，实体急停在示教器上操作。");
        });
        more->setMenu(menu);
        toolbar->addWidget(more);
        fileLabel = new QLabel("未打开文件");
        fileLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        fileLabel->setMinimumWidth(130);
        toolbar->addWidget(fileLabel, 1);
        auto* connectionBar=new QFrame;connectionBar->setObjectName("connectionBar");
        auto* connectionLayout=new QHBoxLayout(connectionBar);connectionLayout->setContentsMargins(10,6,10,6);root->addWidget(connectionBar);
        connectionLayout->addWidget(new QLabel("设备连接"));
        linkType = new QComboBox;
        linkType->setObjectName("connectionMode");
        linkType->addItems(LinkTypes);
        linkType->setToolTip("AC802 PLC模式连接PLC LAN A，按设备接口V1控制机器人；仿真与SDK模式保留。");
        connectionLayout->addWidget(linkType);
        connect(linkType, &QComboBox::currentIndexChanged, this, [this](int index) {
            auto settings = workstationSettings();
            settings.setValue("link/type", index);
            settings.setValue(endpointType == 2 ? "plcBridge/address" : "cabinet/address", ip->text());
            endpointType = index;
            ip->setText(settings.value(index == 2 ? "plcBridge/address" : "cabinet/address",
                index == 2 ? DefaultModbusEndpoint : DefaultCabinetEndpoint).toString());
            ip->setPlaceholderText(index == 2 ? "PLC LAN A IP:502" : "控制柜 IP");
            refresh();
        });
        ip = new QLineEdit(DefaultCabinetEndpoint);
        ip->setPlaceholderText("控制柜 IP");
        ip->setToolTip("AC802 PLC模式填写PLC的LAN A地址，端口默认502；不要填写机器人LAN B侧地址。SDK模式端口30312。");
        ip->setFixedWidth(ip->fontMetrics().horizontalAdvance("255.255.255.255:30312") + 20);
        connectionLayout->addWidget(ip);
        linkButton = new QPushButton("连接");
        linkButton->setProperty("primary",true);linkButton->setObjectName("connectButton");connectionLayout->addWidget(linkButton);
        connect(linkButton, &QPushButton::clicked, this, [this] {
            if (modbus.isActive()) modbus.disconnectFrom();
            else if (session.connected()) session.disconnect();
            else if (linkType->currentIndex() == 0) prepare();
            else if (linkType->currentIndex() == 1) connectReal();
            else connectModbus();
        });
        linkLabel = WorkstationUi::badge("connectionStatus");linkLabel->setMaximumWidth(300);
        machine = new QLabel;
        machine->setProperty("hint",true);connectionLayout->addWidget(linkLabel);connectionLayout->addStretch();connectionLayout->addWidget(machine);
        auto* robotMode=new QToolButton;robotMode->setObjectName("robotModeButton");robotMode->setText("机器人模式…");connectionLayout->addWidget(robotMode);
        connect(robotMode,&QToolButton::clicked,this,[this]{RobotModeDialog dialog(modbus,[this]{return !plcManual->engaged() && welderPanel->client().safeIdle();},this);dialog.exec();});
        auto* monitorToggle=new QToolButton;monitorToggle->setObjectName("monitorToggle");monitorToggle->setText("监视与日志");monitorToggle->setCheckable(true);monitorToggle->setChecked(true);connectionLayout->addWidget(monitorToggle);
        monitorToggle->setToolTip("显示 / 收起右侧设备状态、坐标和操作记录");
        quickStop=new QPushButton("停止机器人");quickStop->setObjectName("globalPlcStop");quickStop->setProperty("danger",true);
        quickStop->setMinimumWidth(125);connectionLayout->addWidget(quickStop);
        connect(quickStop,&QPushButton::clicked,this,[this]{if(plcManual->engaged())plcManual->halt();if(welderPanel->client().fresh())welderPanel->client().issue(WelderProto::AllOff);modbus.issue(BridgeProto::Stop);});

        auto* columns = new QSplitter;workspaceColumns=columns;
        columns->setChildrenCollapsible(false);
        root->addWidget(columns, 1);
        sourceGroup = new QGroupBox("作业原文");
        auto* sourceLayout = new QVBoxLayout(sourceGroup);
        sourceTree = new QTreeWidget;
        sourceTree->setHeaderLabels({"工程文件", "用途"});
        sourceTree->setIndentation(13);
        sourceTree->setMinimumHeight(120);
        sourceTree->setMaximumHeight(225);
        sourceTree->setColumnWidth(0, 270);
        sourceTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
        sourceGroup->setMinimumWidth(360);
        sourceTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        sourceLayout->addWidget(sourceTree);
        auto* fileActions = new QHBoxLayout;
        editFileButton = new QPushButton("编辑文件…");
        saveProjectButton = new QPushButton("另存工程 ZIP…");
        fileActions->addWidget(editFileButton);
        fileActions->addWidget(saveProjectButton);
        sourceLayout->addLayout(fileActions);
        connect(editFileButton, &QPushButton::clicked, this, &Workstation::editProjectFile);
        connect(saveProjectButton, &QPushButton::clicked, this, &Workstation::exportJob);
        gcodeDeliveryButton = new QPushButton("导入 G 代码并自动上传…");
        gcodeDeliveryButton->setObjectName("gcodeDeliveryButton");
        gcodeDeliveryButton->setProperty("primary", true);
        sourceLayout->addWidget(gcodeDeliveryButton);
        connect(gcodeDeliveryButton, &QPushButton::clicked, this, &Workstation::deliverGcode);
        connect(sourceTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
            if (!item) return;
            const auto file = item->data(0, Qt::UserRole).toString();
            if (file.isEmpty()) return;
            if (!job.nativeInput) { if (file != "@gcode") editProjectFile(); return; }
            if (file.startsWith("@gcode:")) showProgramGcode(job, file.mid(7));
            else if (file == "@gcode") showMappedSource(job);
            else showNativeFile(job, file);
            current = done = 0;
            executedRows.clear();
            renderSource();
            if (session.fresh()) sample();
            refresh();
        });
        sourcePosition = new QLabel("L— / 0");
        sourcePosition->setAlignment(Qt::AlignRight);
        sourceLayout->addWidget(sourcePosition);
        sourceTable = new QTableWidget(0, 3);
        sourceTable->setHorizontalHeaderLabels({"行", "原文", "状态"});
        sourceTable->verticalHeader()->hide();
        sourceTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        sourceTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
        sourceTable->setColumnWidth(1, 680);
        sourceTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        sourceTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        sourceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        sourceTable->setSelectionMode(QAbstractItemView::NoSelection);
        sourceTable->setShowGrid(false);
        sourceTable->setWordWrap(false);
        sourceTable->setStyleSheet("QTableWidget { font-family: Consolas; font-size: 10pt; }"
                                  "QHeaderView::section { font-family: 'Microsoft YaHei UI'; font-size: 9pt; }");
        sourceTable->verticalHeader()->setMinimumSectionSize(24);
        sourceTable->verticalHeader()->setDefaultSectionSize(27);
        sourceLayout->addWidget(sourceTable, 1);
        sourceLayout->addWidget(new QLabel("<span style='color:#00695f'>✓ 已完成</span>　"
                                           "<span style='color:#97520d'>▶ 当前</span>　"
                                           "<span style='color:#526777'>· 待执行</span>"));
        sourceHint = new QLabel;
        sourceHint->setWordWrap(true);
        sourceLayout->addWidget(sourceHint);
        connect(sourceTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { selectStart(row + 1); });
        columns->addWidget(sourceGroup);

        auto* center = new QWidget;
        auto* centerLayout = new QVBoxLayout(center);
        centerLayout->setContentsMargins(0, 0, 0, 0);
        auto* tabs = new QTabWidget;workTabs=tabs;
        tabs->setObjectName("workTabs");
        centerLayout->addWidget(tabs, 1);
        auto* trajectory = new QWidget;
        auto* trajectoryLayout = new QVBoxLayout(trajectory);
        auto* projections = new QHBoxLayout;
        summaryLabel = new QLabel("机器人姿态与作业轨迹");
        summaryLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        projections->addWidget(summaryLabel, 1);
        auto* sceneFocus=new QComboBox;sceneFocus->setObjectName("twinSceneFocus");sceneFocus->addItems({"轨迹细节 · mm","机器人全景"});sceneFocus->setCurrentIndex(1);sceneFocus->setToolTip("轨迹细节按本次G代码真实范围取景；不改变轨迹坐标或机器人姿态");projections->addWidget(sceneFocus);
        trajectoryLayout->addLayout(projections);
        auto* sceneTools=new QHBoxLayout;
        auto* showScenePath=new QCheckBox("显示轨迹");showScenePath->setObjectName("twinSceneShowTrajectory");showScenePath->setToolTip("显示已导入作业的规划轨迹；仅改变三维显示");showScenePath->setChecked(workstationSettings().value("twin/sceneShowTrajectory",true).toBool());sceneTools->addWidget(showScenePath);
        auto* ghost=new QCheckBox("终点虚影");ghost->setChecked(true);sceneTools->addWidget(ghost);
        auto* bindStart=new QToolButton;bindStart->setObjectName("twinBindStart");bindStart->setText("起点对齐");bindStart->setToolTip("将最新静止快照设为轨迹预览起点；仅改变显示，不写入设备或移动机器人");sceneTools->addWidget(bindStart);
        auto* fit=new QToolButton;fit->setText("适应视图");sceneTools->addWidget(fit);sceneTools->addStretch();
        auto* sceneSettings=new QToolButton;sceneSettings->setObjectName("twinSceneSettings");sceneSettings->setText("视图 / 背景");sceneSettings->setPopupMode(QToolButton::InstantPopup);sceneTools->addWidget(sceneSettings);trajectoryLayout->addLayout(sceneTools);
        twinScene=new TwinView;twinScene->setObjectName("twinTrajectoryScene");twinScene->setMinimumSize(360,300);twinScene->setFrameScene(true);twinScene->loadModel(QCoreApplication::applicationDirPath()+"/assets");trajectoryLayout->addWidget(twinScene,1);
        connect(sceneFocus,&QComboBox::currentIndexChanged,this,[this,showScenePath](int index){if(index==0)showScenePath->setChecked(true);twinScene->setTrajectoryFocus(index==0);});
        auto* snapshot=new QLabel;snapshot->setObjectName("twinSceneSnapshot");snapshot->setWordWrap(true);snapshot->setProperty("hint",true);trajectoryLayout->addWidget(snapshot);
        auto* previewState=new QLabel;previewState->setObjectName("twinScenePreviewState");previewState->setWordWrap(true);previewState->setProperty("hint",true);trajectoryLayout->addWidget(previewState);
        connect(ghost,&QCheckBox::toggled,twinScene,&TwinView::setGhostVisible);connect(fit,&QToolButton::clicked,twinScene,&TwinView::fitScene);
        connect(showScenePath,&QCheckBox::toggled,this,[this,ghost](bool on){twinScene->setFrameScene(on);twinScene->fitScene();ghost->setEnabled(on);workstationSettings().setValue("twin/sceneShowTrajectory",on);});
        connect(bindStart,&QToolButton::clicked,this,[this]{if(twinPanel)twinPanel->bindPreviewOrigin();});
        // Existing progress and replay bookkeeping remains independent of the snapshot scene.
        plot=new Plot(trajectory);plot->hide();
        tabs->addTab(trajectory, "① 作业准备");
        plcPanel = new PlcBridgePanel(modbus);
        poseHosts[0]=plcPanel->poseHost();poseLayouts[0]=qobject_cast<QVBoxLayout*>(poseHosts[0]->layout());
        monitorHosts[0]=plcPanel->monitorHost();
        plcManual = new ManualBridgePanel(modbus);
        poseHosts[1]=plcManual->poseHost();poseLayouts[1]=qobject_cast<QVBoxLayout*>(poseHosts[1]->layout());
        monitorHosts[1]=plcManual->monitorHost();
        tabs->addTab(plcManual, "② 手动与起点");
        tabs->addTab(plcPanel, "③ 自动运行");
        connect(plcManual, &ManualBridgePanel::notice, this, [this](const QString& text){ log("微调 · " + text); });
        auto* manual = new QWidget;
        auto* manualLayout = new QGridLayout(manual);
        manualLayout->addWidget(new QLabel("点动坐标系"), 0, 0);
        jogFrame = new QComboBox;
        jogFrame->addItems({"关节 JOINTS", "世界 WORLD", "基座 ROBOTBASE", "工具 TOOL", "用户/工件 USER"});
        auto* frames = new QHBoxLayout;
        frames->addWidget(jogFrame, 1);
        userRefName = new QLineEdit("SimPart");
        userRefName->setPlaceholderText("用户坐标名");
        userRefName->setMaximumWidth(125);
        userRefName->setToolTip("输入控制器中已定义的 CARTREF 名称。隔离仿真模板中为 SimPart。按回车应用。");
        frames->addWidget(userRefName);
        manualLayout->addLayout(frames, 0, 1, 1, 3);
        connect(jogFrame, &QComboBox::currentIndexChanged, this, [this](int index) {
            const QStringList commands{"joints", "world", "base", "tool", "user"};
            if (index >= 0 && index < commands.size()) session.request(index == 4 ? "user " + userRefName->text().trimmed() : commands[index]);
        });
        connect(userRefName, &QLineEdit::returnPressed, this, [this] {
            if (jogFrame->currentIndex() == 4) session.request("user " + userRefName->text().trimmed());
        });
        linearJogSpeed = new QDoubleSpinBox;
        linearJogSpeed->setDecimals(2);
        linearJogSpeed->setRange(.25, 250);
        linearJogSpeed->setValue(12.5);
        linearJogSpeed->setSuffix(" mm/s");
        linearJogSpeed->setKeyboardTracking(false);
        angularJogSpeed = new QDoubleSpinBox;
        angularJogSpeed->setDecimals(2);
        angularJogSpeed->setRange(.05, 30);
        angularJogSpeed->setValue(1);
        angularJogSpeed->setSuffix(" °/s");
        angularJogSpeed->setKeyboardTracking(false);
        manualLayout->addWidget(new QLabel("XYZ 平移速度"), 1, 0);
        manualLayout->addWidget(linearJogSpeed, 1, 1);
        manualLayout->addWidget(new QLabel("关节/姿态速度"), 1, 2);
        manualLayout->addWidget(angularJogSpeed, 1, 3);
        manualFeedback = new QLabel("输入目标速度。手动 TCP 速度上限 250 mm/s，单次点动 0.30 s。");
        manualFeedback->setWordWrap(true);
        manualLayout->addWidget(manualFeedback, 2, 0, 1, 4);
        for (int axis = 0; axis < 6; ++axis) {
            auto* label = new QLabel;
            axisLabels << label;
            manualLayout->addWidget(label, axis + 3, 0);
            auto* position = new QLabel("等待回读");
            manualValues << position;
            manualLayout->addWidget(position, axis + 3, 1);
            for (int dir = 0; dir < 2; ++dir) {
                auto* button = new QPushButton(dir ? "+" : "−");
                manualButtons << button;
                manualLayout->addWidget(button, axis + 3, dir + 2);
                connect(button, &QPushButton::clicked, this, [this, axis, dir] {
                    const int reported = session.value("jogref").toInt();
                    const int ref = reported == 4 && session.value("refname") == "ROBOTBASE" ? 2 : reported;
                    if (ref < 0 || ref > 4) return;
                    const auto speed = ref != 0 && axis < 3 ? linearJogSpeed->value() : angularJogSpeed->value();
                    if (!(speed > 0)) return;
                    if (session.request(QString("jog %1 %2%3 %4").arg(QString("JWBTU").mid(ref, 1)).arg(axis + 1).arg(dir ? '+' : '-')
                                        .arg(speed, 0, 'f', 3)))
                        homeDirty = true;
                });
            }
        }
        auto* jointGroup = new QGroupBox("各轴实时角度 · °");
        auto* jointLayout = new QGridLayout(jointGroup);
        for (int i = 0; i < 6; ++i) {
            auto* value = new QLabel(QString("A%1  等待回读").arg(i + 1));
            jointReadouts << value;
            jointLayout->addWidget(value, i / 3, i % 3);
        }
        manualLayout->addWidget(jointGroup, 9, 0, 1, 4);
        auto* moveGroup = new QGroupBox("运动语句 · SDK 直控（示教调试 / 挪动机械臂）");
        auto* moveLayout = new QGridLayout(moveGroup);
        moveType = new QComboBox;
        moveType->addItems({"PTP · 关节", "PTP · 笛卡尔", "直线 LIN", "圆弧 CIRC"});
        moveDyn = new QSpinBox;
        moveDyn->setRange(1, 100);
        moveDyn->setValue(30);
        moveDyn->setSuffix(" %");
        moveDyn->setToolTip("速度倍率：控制器程序速度的百分比。");
        moveOvl = new QDoubleSpinBox;
        moveOvl->setDecimals(1);
        moveOvl->setRange(0, 500);
        moveOvl->setValue(0);
        moveOvl->setSuffix(" mm");
        moveOvl->setToolTip("过渡半径（绝对 mm）：大于 0 时与后续语句平滑衔接不停顿，0 精确到点。");
        moveLayout->addWidget(new QLabel("语句类型"), 0, 0);
        moveLayout->addWidget(moveType, 0, 1);
        moveLayout->addWidget(new QLabel("速度倍率"), 0, 2);
        moveLayout->addWidget(moveDyn, 0, 3);
        moveLayout->addWidget(new QLabel("过渡半径"), 1, 0);
        moveLayout->addWidget(moveOvl, 1, 1);
        for (int i = 0; i < 6; ++i) {
            auto* label = new QLabel(QString("J%1°").arg(i + 1));
            moveFieldLabels << label;
            moveLayout->addWidget(label, 2 + i / 3, (i % 3) * 2);
            auto* field = new QDoubleSpinBox;
            field->setDecimals(3);
            field->setRange(-10000, 10000);
            field->setKeyboardTracking(false);
            moveFields << field;
            moveLayout->addWidget(field, 2 + i / 3, (i % 3) * 2 + 1);
        }
        moveHelpRow = new QWidget;
        auto* moveHelpLayout = new QHBoxLayout(moveHelpRow);
        moveHelpLayout->setContentsMargins(0, 0, 0, 0);
        const QStringList helpNames{"HX mm", "HY mm", "HZ mm"};
        for (int i = 0; i < 3; ++i) {
            moveHelpLayout->addWidget(new QLabel(helpNames[i]));
            auto* field = new QDoubleSpinBox;
            field->setDecimals(3);
            field->setRange(-10000, 10000);
            field->setKeyboardTracking(false);
            moveHelp << field;
            moveHelpLayout->addWidget(field, 1);
        }
        moveLayout->addWidget(moveHelpRow, 4, 0, 1, 6);
        moveHelpRow->setVisible(false);
        moveFetchButton = new QPushButton("取当前位姿");
        moveSendButton = new QPushButton("发送运动语句");
        moveSendButton->setProperty("primary", true);
        auto* moveActions = new QHBoxLayout;
        moveActions->addWidget(moveFetchButton);
        moveActions->addWidget(moveSendButton);
        moveActions->addStretch(1);
        moveLayout->addLayout(moveActions, 5, 0, 1, 6);
        moveFeedback = new QLabel("一次发送一条语句；圆弧需填辅助点（HX HY HZ 为弧上中间点）。笛卡尔语句按当前点动坐标系执行。");
        moveFeedback->setWordWrap(true);
        moveLayout->addWidget(moveFeedback, 6, 0, 1, 6);
        manualLayout->addWidget(moveGroup, 10, 0, 1, 4);
        connect(moveType, &QComboBox::currentIndexChanged, this, [this](int index) {
            const QStringList names = index == 0 ? QStringList{"J1°", "J2°", "J3°", "J4°", "J5°", "J6°"} :
                QStringList{"X mm", "Y mm", "Z mm", "A °", "B °", "C °"};
            for (int i = 0; i < 6; ++i) moveFieldLabels[i]->setText(names[i]);
            moveHelpRow->setVisible(index == 3);
        });
        connect(moveFetchButton, &QPushButton::clicked, this, [this] {
            const int reported = session.value("jogref").toInt();
            const int frame = reported == 4 && session.value("refname") == "ROBOTBASE" ? 2 : reported;
            if (moveType->currentIndex() == 0) {
                const auto joints = session.value("axes").split(',');
                if (joints.size() != 6) { moveFeedback->setText("轴角回读未就绪，稍后再取。"); return; }
                for (int i = 0; i < 6; ++i) moveFields[i]->setValue(joints[i].toDouble());
            } else {
                const auto pose = frame == 1 ? session.value("xyz") + ',' + session.value("abc") :
                    frame == 2 ? session.value("base") : frame == 4 ? session.value("user") : QString();
                const auto values = pose.split(',');
                if (values.size() != 6) {
                    moveFeedback->setText("笛卡尔语句按当前点动坐标系取位姿，请先在上方把坐标系切到世界/基座/用户。");
                    return;
                }
                for (int i = 0; i < 6; ++i) moveFields[i]->setValue(values[i].toDouble());
            }
            moveFeedback->setText("已填入当前位姿。确认目标后再发送。");
        });
        connect(moveSendButton, &QPushButton::clicked, this, [this] {
            if (moveBusy) return;
            if (session.value("motion_rest") != "1") {
                moveFeedback->setText("机械臂运动未停止，稍等回读确认静止后再发送。");
                return;
            }
            const int type = moveType->currentIndex();
            QString command = type == 0 ? "moveptpj" : type == 1 ? "moveptp" : type == 2 ? "movelin" : "movecirc";
            for (int i = 0; i < 6; ++i) command += QString(" %1").arg(moveFields[i]->value(), 0, 'f', 3);
            if (type == 3)
                for (int i = 0; i < 3; ++i) command += QString(" %1").arg(moveHelp[i]->value(), 0, 'f', 3);
            command += QString(" %1 %2").arg(moveDyn->value()).arg(moveOvl->value(), 0, 'f', 1);
            bool seqKnown = false;
            moveSeqSeen = session.value("move_seq").toLongLong(&seqKnown);
            if (!seqKnown) moveSeqSeen = -1;
            const unsigned attempt = ++moveAttempt;
            moveBusy = true;
            if (session.request(command)) {
                setVisualProperty(moveFeedback, "tone", "busy");
                moveFeedback->setText("语句已发送，等待执行……执行中可随时点顶部停止或断开。");
            } else {
                moveBusy = false;
                moveFeedback->setText("命令未发出：连接未就绪或有命令在排队，稍后再试。");
            }
            QTimer::singleShot(70000, this, [this, attempt] {
                if (moveBusy && moveAttempt == attempt) {
                    moveBusy = false;
                    setVisualProperty(moveFeedback, "tone", "warn");
                    moveFeedback->setText("! 70 s 未收到回执。控制器可能仍在运动，请确认现场状态后再操作。");
                }
            });
        });
        auto* homeGroup = new QGroupBox("上位机起点与回程");
        auto* homeLayout = new QGridLayout(homeGroup);
        recordHomeButton = new QPushButton("将当前位置设为起点…");
        returnHomeButton = new QPushButton("返回起点…");
        returnHomeSpeed = new QDoubleSpinBox;
        returnHomeSpeed->setRange(.1, 250);
        returnHomeSpeed->setDecimals(2);
        returnHomeSpeed->setValue(25);
        returnHomeSpeed->setSuffix(" mm/s");
        returnHomeSpeed->setToolTip("回程 TCP 平移速度上限，单位 mm/s，最高 250 mm/s；按关节速度与工具长度保守限速，实际速度可能更低。低于控制器速度分辨率时拒绝移动。");
        homeLayout->addWidget(recordHomeButton, 0, 0);
        homeLayout->addWidget(new QLabel("回程末端速度上限"), 0, 1);
        homeLayout->addWidget(returnHomeSpeed, 0, 2);
        homeLayout->addWidget(returnHomeButton, 0, 3);
        homeInfo = new QLabel("尚未设置起点。先手动移动，再记录当前位置。");
        homeInfo->setWordWrap(true);
        homeLayout->addWidget(homeInfo, 1, 0, 1, 4);
        auto* homeHint = new QLabel("起点由上位机按控制器身份保存，与当前工程无关。回程先生成六轴及 TCP 预览，再由你确认；路径为关节分段运动，未做环境碰撞检测。仿真与实机连接均可移动。");
        homeHint->setWordWrap(true);
        homeLayout->addWidget(homeHint, 2, 0, 1, 4);
        manualLayout->addWidget(homeGroup, 11, 0, 1, 4);
        connect(recordHomeButton, &QPushButton::clicked, this, &Workstation::recordHome);
        connect(returnHomeButton, &QPushButton::clicked, this, &Workstation::planHomeReturn);
        tabs->addTab(manual, "SDK 手动调试");
        auto* camera = new QWidget;
        auto* cameraLayout = new QVBoxLayout(camera);
        auto* image = new QLabel("等待本地图像");
        image->setAlignment(Qt::AlignCenter);
        image->setMinimumSize(0, 100);
        cameraLayout->addWidget(image, 1);
        auto* openImage = new QPushButton("打开本地图像");
        cameraLayout->addWidget(openImage);
        connect(openImage, &QPushButton::clicked, this, [this, image] {
            const auto file = QFileDialog::getOpenFileName(this, "打开图像", {}, "图像 (*.png *.jpg *.jpeg *.bmp)");
            if (file.isEmpty()) return;
            const QPixmap pixmap(file);
            if (!pixmap.isNull()) image->setPixmap(pixmap.scaled(image->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        });
        tabs->addTab(camera, "相机预览");
        welderPanel = new WelderPanel(modbus);
        poseHosts[2]=welderPanel->poseHost();poseLayouts[2]=qobject_cast<QVBoxLayout*>(poseHosts[2]->layout());
        tabs->addTab(welderPanel, "④ 焊机工艺");
        printPanel=new PrintPanel(*plcPanel,*welderPanel);
        poseHosts[3]=printPanel->poseHost();poseLayouts[3]=qobject_cast<QVBoxLayout*>(poseHosts[3]->layout());
        monitorHosts[2]=printPanel->monitorHost();
        tabs->addTab(printPanel,"⑤ 联动打印");
        telemetry = new QLabel("实时遥测 · 未连接");
        telemetry->setWordWrap(true);
        centerLayout->addWidget(telemetry);
        columns->addWidget(center);

        auto* right = new QGroupBox("运行控制与进度");runSidebar=right;
        right->setMinimumWidth(280);
        auto* run = new QVBoxLayout(right);
        startButton = new QPushButton("开始");
        run->addWidget(startButton);
        connect(startButton, &QPushButton::clicked, this, &Workstation::start);
        stopButton = new QPushButton("停止发送");
        stopButton->setObjectName("stop");
        run->addWidget(stopButton);
        connect(stopButton, &QPushButton::clicked, this, [this] {
            if (linkType->currentIndex() == 2) {
                modbus.issue(BridgeProto::Stop);
                return;
            }
            session.stop();
        });
        run->addWidget(new QLabel("真正的急停请用示教器实体按钮"));
        modeManual = new QPushButton("切到手动");
        modeAuto = new QPushButton("切到自动");
        auto* mode = new QHBoxLayout;
        for (auto* button : {modeManual, modeAuto}) mode->addWidget(button);
        run->addLayout(mode);
        connect(modeManual, &QPushButton::clicked, this, [this] { session.request("manual"); });
        connect(modeAuto, &QPushButton::clicked, this, [this] { session.request("auto"); });
        cabinetLoadButton = new QPushButton("加载作业程序");
        cabinetLoadButton->setToolTip("实机专用：把柜内当前载入的程序切换为本作业的程序。\n要求柜内已存入作业工程（U 盘或 STEPStudio 导入一次），且柜内程序处于停止状态。\n柜内没有该工程时会提示导入方法。");
        run->addWidget(cabinetLoadButton);
        connect(cabinetLoadButton, &QPushButton::clicked, this, [this] { session.request("loadprogram"); });
        stateLabel = new QLabel;
        stateLabel->setObjectName("statusBanner");
        stateLabel->setWordWrap(true);
        run->addWidget(stateLabel);
        runPosition = new QLabel;
        run->addWidget(runPosition);
        progress = new QProgressBar;
        progress->setRange(0, 1000);
        progress->setTextVisible(false);
        run->addWidget(progress);
        runTime = new QLabel;
        run->addWidget(runTime);
        versionCheck = new QLabel;
        versionCheck->setObjectName("versionCheck");
        versionCheck->setWordWrap(true);
        run->addWidget(versionCheck);
        cabinetInfo = new QLabel;
        cabinetInfo->setWordWrap(true);
        run->addWidget(cabinetInfo);
        auto* dry = new QCheckBox("空跑模式 · 出光禁用");
        dry->setChecked(true);
        dry->setEnabled(false);
        dry->setObjectName("dry");
        run->addWidget(dry);
        auto* processBoundary = new QLabel("激光接口未接入");
        processBoundary->setObjectName("processBoundary");
        processBoundary->setWordWrap(true);
        run->addWidget(processBoundary);
        modbusGroup = new QGroupBox("AC802 PLC 设备控制");
        auto* modbusLayout = new QGridLayout(modbusGroup);
        programNumber = new QSpinBox; programNumber->setRange(1, BridgeProto::MaxPrograms);
        loadProgramButton = new QPushButton("选择并加载");
        loadProgramButton->setToolTip("V2选择示教器GDI绑定的1～255号程序；连接旧V1时仅四槽。加载后须人工核对，再单独运行。");
        modbusPause = new QPushButton("暂停");
        modbusReset = new QPushButton("清错");
        modbusLayout->addWidget(new QLabel("程序号"), 0, 0);
        modbusLayout->addWidget(programNumber, 0, 1);
        modbusLayout->addWidget(loadProgramButton, 1, 0, 1, 2);
        modbusLayout->addWidget(modbusPause, 2, 0);
        modbusLayout->addWidget(modbusReset, 2, 1);
        modbusReadoutLabel = new QLabel("登记、使能、加载人工确认与通讯停止复位，请打开中间的「PLC 设备控制」页。");
        modbusReadoutLabel->setWordWrap(true);
        modbusLayout->addWidget(modbusReadoutLabel, 3, 0, 1, 2);
        modbusGroup->setVisible(false); run->addWidget(modbusGroup);
        connect(loadProgramButton, &QPushButton::clicked, this, [this] { loadModbusProgram(); });
        connect(modbusPause, &QPushButton::clicked, this, [this] {
            if (modbus.status().paused()) start();
            else modbus.issue(BridgeProto::Pause);
        });
        connect(modbusReset, &QPushButton::clicked, this, [this] { modbus.issue(BridgeProto::ClearError); });
        connect(programNumber, &QSpinBox::valueChanged, this, [this] { refresh(); });
        connect(programNumber, &QSpinBox::valueChanged, plcPanel, &PlcBridgePanel::setProgramSlot);
        connect(plcPanel, &PlcBridgePanel::programSlotChanged, programNumber, &QSpinBox::setValue);
        auto* records = new QGroupBox("记录");
        auto* recordLayout = new QVBoxLayout(records);
        experimentLabel = new QLineEdit;
        experimentLabel->setMaxLength(100);
        experimentLabel->setPlaceholderText("试次备注（可选）");
        recordLayout->addWidget(experimentLabel);
        experimentStatus = new QLabel("启动后自动记录输入、计划路径和回读");
        experimentStatus->setWordWrap(true);
        recordLayout->addWidget(experimentStatus);
        openRecord = new QPushButton("打开本次记录");
        auto* recordActions = new QHBoxLayout;
        csvButton = new QPushButton("另存遥测 CSV");
        csvButton->setToolTip("手动点动和联调可单独记录遥测。作业试次会自动保存 trace.csv。");
        recordActions->addWidget(csvButton);
        recordActions->addWidget(openRecord);
        auto* runBrowser = new QPushButton("历史试次");
        runBrowser->setToolTip("浏览历次实验记录：拖时间轴离线回放，或选两份做参数与偏差对比。");
        recordActions->addWidget(runBrowser);
        connect(runBrowser, &QPushButton::clicked, this, [this] { RunBrowserDialog(recordRoot, this).exec(); });
        recordLayout->addLayout(recordActions);
        connect(csvButton, &QPushButton::clicked, this, &Workstation::toggleCsv);
        connect(openRecord, &QPushButton::clicked, this, [this] {
            if (!experiment.folder().isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(experiment.folder()));
        });
        logs = new QPlainTextEdit;
        logs->setReadOnly(true);
        logs->setMinimumHeight(100);
        logs->document()->setMaximumBlockCount(300);
        recordLayout->addWidget(logs, 1);
        run->addWidget(records, 1);
        columns->addWidget(right);
        plcMonitor=new PlcLiveMonitor(modbus);
        plcMonitor->openDiagnostics=[this]{workTabs->setCurrentWidget(plcPanel);plcPanel->showDiagnostics();};
        monitorScroll=new QScrollArea;monitorScroll->setObjectName("plcMonitorScroll");monitorScroll->setWidgetResizable(true);
        monitorScroll->setMinimumWidth(0);monitorScroll->setWidget(plcMonitor);monitorScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);monitorScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        monitorDock=new QWidget;monitorDock->setObjectName("workspaceMonitorDock");
        auto* dockLayout=new QVBoxLayout(monitorDock);dockLayout->setContentsMargins(0,0,0,0);dockLayout->setSpacing(8);
        twinPanel=new TwinPanel(modbus,QCoreApplication::applicationDirPath()+"/assets");
        twinPanel->setCompanionView(twinScene);sceneSettings->setMenu(twinPanel->viewMenu());
        twinScene->setFrameScene(showScenePath->isChecked());ghost->setEnabled(showScenePath->isChecked());
        twinPanel->previewChanged=[this,snapshot,previewState,bindStart,sceneFocus,showScenePath]{snapshot->setText(twinPanel->snapshotSummary());previewState->setText(twinPanel->previewStatus());bindStart->setEnabled(twinPanel->canBindPreview());const QSignalBlocker focusBlock(sceneFocus),pathBlock(showScenePath);sceneFocus->setCurrentIndex(twinScene->trajectoryFocus()?0:1);showScenePath->setChecked(twinScene->trajectoryVisible());};
        twinPanel->previewChanged();
        twinPanel->setParent(plcPanel);twinPanel->hide();
        dockLayout->addWidget(monitorScroll,1);monitorDock->hide();
        columns->setStretchFactor(0,0);columns->setStretchFactor(1,1);columns->setStretchFactor(2,0);
        columns->setSizes({520,990,290});
        connect(monitorToggle,&QToolButton::toggled,this,[this](bool enabled){monitorRequested=enabled;refreshWorkspaceLayout();});

        footer = new QLabel;
        footer->setObjectName("footer");
        root->addWidget(footer);
        connect(tabs,&QTabWidget::currentChanged,this,[this]{refreshWorkspaceLayout();QTimer::singleShot(0,this,[this]{refreshWorkspaceLayout();});});
    }

    void refreshWorkspaceLayout(){
        if(!workTabs||changingLayout)return;
        changingLayout=true;
        const bool plc=linkType->currentIndex()==2||modbusLinked;
        workTabs->setTabVisible(1,plc);workTabs->setTabVisible(2,plc);workTabs->setTabVisible(3,!plc);
        workTabs->setTabVisible(4,false);workTabs->setTabVisible(5,plc);
        workTabs->setTabVisible(6,plc);
        const bool printing=plc&&workTabs->currentWidget()==printPanel;
        printPanel->setActive(printing);
        twinPanel->setPrintLayout(printing);
        const bool operating=plc&&(workTabs->currentWidget()==plcPanel||workTabs->currentWidget()==plcManual);
        const bool welding=plc&&workTabs->currentWidget()==welderPanel;
        if(operating||welding||printing){
            const int host=printing?3:welding?2:workTabs->currentWidget()==plcPanel?0:1;
            if(twinPanel->parentWidget()!=poseHosts[host]){poseLayouts[host]->addWidget(twinPanel);twinPanel->show();}
            auto* scroll=printing?printPanel->findChild<QScrollArea*>("printScroll"):welding?welderPanel->findChild<QScrollArea*>("welderScroll"):host==0?plcPanel->findChild<QScrollArea*>("plcControlScroll"):plcManual->findChild<QScrollArea*>("manualControlScroll");
            const int unscrolledTop=twinPanel->mapTo(workTabs,QPoint()).y()+scroll->verticalScrollBar()->value();
            twinPanel->setViewportHeightLimit(printing?printPanel->refreshBodyHeight()-154:workTabs->height()-unscrolledTop-170);
        }
        if(operating||printing){
            const int host=printing?2:workTabs->currentWidget()==plcPanel?0:1;
            if(monitorDock->parentWidget()!=monitorHosts[host])qobject_cast<QVBoxLayout*>(monitorHosts[host]->layout())->addWidget(monitorDock);
            const bool compactMonitor=host!=0||workTabs->width()<1380;
            plcMonitor->setCompact(compactMonitor);
            const int monitorWidth=compactMonitor?workTabs->width()-40:std::max(310,std::min(1780,workTabs->width()-40)/4);
            const int monitorHeight=std::max(plcMonitor->minimumSizeHint().height(),plcMonitor->heightForWidth(monitorWidth));
            monitorScroll->setFixedHeight(std::max(compactMonitor?285:610,monitorHeight+2));
            monitorDock->setMinimumWidth(compactMonitor?0:310);monitorDock->setMaximumWidth(QWIDGETSIZE_MAX);
        }
        sourceGroup->setVisible(!operating&&!welding&&!printing);runSidebar->setVisible(!plc);telemetry->setVisible(!operating&&!welding&&!printing);
        monitorDock->setVisible((operating||printing)&&monitorRequested);
        for(int i=0;i<3;++i)monitorHosts[i]->setVisible((operating||printing)&&monitorRequested&&i==(printing?2:workTabs->currentWidget()==plcPanel?0:1));
        plcPanel->setMonitorVisible(operating&&monitorRequested&&workTabs->currentWidget()==plcPanel);
        monitorScroll->setVisible(plc&&monitorRequested);
        findChild<QToolButton*>("monitorToggle")->setVisible(plc);
        findChild<QToolButton*>("robotModeButton")->setVisible(plc);
        quickStop->setVisible(plc);
        QString why;const bool stopAllowed=plc&&modbus.canIssue(BridgeProto::Stop,0,&why);
        WorkstationUi::button(quickStop,stopAllowed,stopAllowed?"请求普通停止；实体急停在示教器":why);
        changingLayout=false;
    }

    void open() {
        if (!canOpenFile()) return;
        const auto path = QFileDialog::getOpenFileName(this, "打开 G 代码 / STEP ZIP", workspace,
            "G 代码与 STEP 工程 (*.nc *.gcode *.ngc *.tap *.txt *.zip)");
        openPath(path);
    }

    bool canOpenFile() const {
        if (session.preparing || experiment.active()) return false;
        return !session.connected() || session.canClose();
    }

    void selectJob(Job selected) {
        if (!canOpenFile()) { log("请先停止作业并确认状态，再打开新的文件。"); return; }
        if (!confirmLocalChanges()) return;
        const bool replaceSimulation = session.connected() && session.simulated;
        if (replaceSimulation) {
            session.disconnect();
            if (session.active()) return;
        }
        lastOperationError.clear();
        load(std::move(selected));
        if (replaceSimulation && job.runnable()) prepare();
    }

    void exportJob() {
        if (!job.error.isEmpty() || job.files.isEmpty()) return;
        const auto path = QFileDialog::getSaveFileName(this, "另存完整 STEP 工程 · 修改后需重新仿真", job.project.chopped(3) + ".zip", "STEP 工程 (*.zip)");
        if (path.isEmpty()) return;
        const auto error = exportStepJob(job, path);
        log(error.isEmpty() ? "已导出 STEP 工程；G 代码视图同时保存源行映射。可在 STEPStudio 导入 ZIP 验证。" : error);
        if (error.isEmpty()) { job.modified = false; renderSource(); refresh(); }
    }

    void deliverGcode() {
        if (session.active() || !autoUploadIdle() || experiment.active() || !canOpenFile()) {
            log("请先停止、断使能并退出微调，再导入自动作业。"); return;
        }
        if (!confirmLocalChanges()) return;
        const auto settings = workstationSettings();
        const auto endpoint = linkType->currentIndex() == 2 && !ip->text().trimmed().isEmpty() ? ip->text().trimmed() :
            settings.value("fileBridge/address", "192.168.1.188:502").toString();
        const auto data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        GcodeDeliveryDialog dialog(nativeTemplate, endpoint, QDir(data).filePath("gcode_deliveries"),
            QDir(data).filePath("file_upload_receipts"), this, true);
        const auto cabinet = linkType->currentIndex() == 1 && !ip->text().trimmed().isEmpty() ? ip->text().trimmed() :
            settings.value("cabinet/address", DefaultCabinetEndpoint).toString();
        dialog.setDirectTransfer(linkType->currentIndex() == 1 || settings.value("fileBridge/directTransfer",false).toBool(), cabinet);
        connect(&dialog, &GcodeDeliveryDialog::previewChanged, this, [this, &dialog] { applyAutoPreview(dialog); });
        dialog.importCurrentSource(job);
        dialog.exec();
        if (!dialog.packagePath().isEmpty()) log("自动作业补丁及原始G代码已保留：" + dialog.packagePath());
        const auto result = dialog.result();
        applyAutoPreview(dialog);
        if (!result.isEmpty()) log("G代码文件桥 · " + result.value("status").toString() + " · " + result.value("remote_directory").toString() +
            (result.value("status").toString()=="COMPLETE"?"；作业号11，须重新加载、确认加载、使能、运行。":"；本次上传未完成，请保留回执。"));
    }

    void applyAutoPreview(const GcodeDeliveryDialog& dialog) {
        if (!dialog.hasSources()) return;
        auto preview = dialog.preview();
        // Replace every view together, including a failed attempt, so old code/path cannot linger.
        load(std::move(preview));
        if(job.error.isEmpty()&&job.points.size()>1){findChild<QCheckBox*>("twinSceneShowTrajectory")->setChecked(true);findChild<QComboBox*>("twinSceneFocus")->setCurrentIndex(0);twinScene->fitScene();}
        if (dialog.result().value("status").toString() == "COMPLETE") programNumber->setValue(11);
    }

    bool autoUploadIdle() const {
        if(plcManual && plcManual->engaged())return false;
        if(modbus.operationInhibited() || (welderPanel && !welderPanel->client().safeIdle()))return false;
        if(!modbus.isActive())return true;
        const auto status=modbus.status();
        return modbus.fresh() && !modbus.pending() && status.stopped() && !status.enabled() && !status.has(BridgeProto::ActionBusy);
    }


    void rebuildProjectTree() {
        const QSignalBlocker blocked(sourceTree);
        sourceTree->clear();
        if (job.files.isEmpty()) return;
        auto* root = new QTreeWidgetItem(sourceTree, {job.project, job.automaticPatch ? "自动作业补丁 · 本地" : "完整工程"});
        root->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        if (job.automaticPatch) root->setToolTip(0, job.previewNote);
        const auto selected = job.sourceIsGcode ? (job.automaticPatch ? "@gcode:" + job.sourceProgram : QString("@gcode")) : job.sourceFile;
        QTreeWidgetItem* currentItem = nullptr;
        auto addFile = [&](QTreeWidgetItem* parent, const QString& name, const QString& role) {
            auto* item = new QTreeWidgetItem(parent, {name, role});
            item->setData(0, Qt::UserRole, name);
            item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
            item->setToolTip(0, job.project + '/' + name);
            if (name == selected) { currentItem = item; parent->setExpanded(true); }
        };
        if (!job.automaticPatch && (!job.sourceIsGcode || !job.nativeInput || !job.mappedSource.isEmpty())) {
            if (!job.nativeInput || !job.mappedSource.isEmpty()) {
                auto* mapped = new QTreeWidgetItem(root, {"G 代码", job.nativeInput ? "已关联" : "原始输入"});
                mapped->setData(0, Qt::UserRole, "@gcode");
                if (selected == "@gcode") currentItem = mapped;
            }
        }
        for (const auto& name : job.files.keys()) {
            if (!name.endsWith(".srp")) continue;
            const auto program = name.chopped(4);
            const QString role = job.automaticPatch ? (program == "auto_main" ? "11号入口" : QFileInfo(job.programSources.value(program)).fileName()) :
                program == "capture_home" ? "前置 · 作业坐标基准" : program == "go_home" ? "关节PTP回程 · PLC需手动选择" :
                program == job.program ? (job.reviewed || job.trustedGcode || !job.nativeInput ? "主程序" : "程序 · 入口待核对") : "辅助程序";
            auto* group = new QTreeWidgetItem(root, {program, role});
            group->setData(0, Qt::UserRole, name);
            if (job.automaticPatch) group->setToolTip(0, job.project + '/' + name + '\n' + job.programSources.value(program));
            if (job.automaticPatch && job.programGcodes.contains(program)) {
                const auto key = "@gcode:" + program;
                auto* original = new QTreeWidgetItem(group, {QFileInfo(job.programSources.value(program)).fileName(), "本次G代码原文"});
                original->setData(0, Qt::UserRole, key);
                original->setToolTip(0, job.programSources.value(program));
                if (selected == key) { currentItem = original; group->setExpanded(true); }
            }
            addFile(group, name, "源程序");
            if (job.files.contains(program + ".srd")) addFile(group, program + ".srd", "程序变量");
        }
        for (const auto& name : job.files.keys())
            if (name.endsWith(".srd") && !job.files.contains(name.chopped(1) + 'p')) addFile(root, name, "工程变量");
        root->setExpanded(true);
        if (currentItem) sourceTree->setCurrentItem(currentItem);
    }

    void editProjectFile() {
        if (!canOpenFile() || !sourceTree->currentItem()) return;
        const auto file = sourceTree->currentItem()->data(0, Qt::UserRole).toString();
        if (!job.files.contains(file)) return;
        QDialog dialog(this);
        dialog.setWindowTitle(file + (job.automaticPatch ? " · 本地自动作业预览" : " · 本地工程副本"));
        dialog.resize(1000, 700);
        QVBoxLayout layout(&dialog);
        QLabel hint("修改主程序、前置程序和 SRD 变量后，请另存完整 ZIP 并在 STEPStudio 重新验证。此处不会写入控制柜。");
        if (job.automaticPatch) hint.setText(job.previewNote + " 如需修改轨迹，请重新导入 G 代码生成并上传。");
        hint.setWordWrap(true);
        layout.addWidget(&hint);
        QPlainTextEdit editor;
        editor.setFont(QFont("Consolas", 11));
        editor.setPlainText(QString::fromUtf8(job.files.value(file)));
        editor.document()->setModified(false);
        layout.addWidget(&editor, 1);
        QDialogButtonBox buttons;
        if (job.automaticPatch) {
            editor.setReadOnly(true);
            buttons.addButton(QDialogButtonBox::Close);
            layout.addWidget(&buttons);
            connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            dialog.exec(); return;
        }
        auto* save = buttons.addButton("保存到工作副本", QDialogButtonBox::AcceptRole);
        buttons.addButton("取消修改", QDialogButtonBox::RejectRole);
        layout.addWidget(&buttons);
        connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(save, &QPushButton::clicked, &dialog, [&] {
            if (!editor.document()->isModified()) { dialog.accept(); return; }
            auto working = job;
            if (!working.nativeInput) { working.nativeInput = true; working.inputBytes.clear(); }
            const auto error = updateNativeFile(working, file, editor.toPlainText().replace("\r\n", "\n").toUtf8());
            if (!error.isEmpty()) { QMessageBox::warning(&dialog, "无法保存副本", error); return; }
            if (session.connected() && session.simulated) session.disconnect();
            load(std::move(working));
            dialog.accept();
        });
        dialog.exec();
    }

    void associateGcode() {
        if (!job.nativeInput || !canOpenFile()) return;
        const auto path = QFileDialog::getOpenFileName(this, "关联 G 代码与 SRP 行映射", workspace,
            "G 代码与行映射 (*.gcode *.nc *.txt *.mapping.json)");
        if (path.isEmpty()) return;
        auto linked = job;
        const auto error = associateNativeGcode(linked, path);
        if (!error.isEmpty()) { log(error); return; }
        load(std::move(linked));
        log("已校验 ZIP、原生程序和 G 代码的 SHA-256。高亮按对应程序的 SRP 行号映射。");
    }

    void saveSource() {
        if (job.source.isEmpty()) return;
        const auto path = QFileDialog::getSaveFileName(this, "导出左侧原文",
            job.sourceIsGcode ? "display.gcode" : job.sourceFile,
            job.sourceIsGcode ? "G 代码 (*.gcode)" : "STEP 原文 (*.srp *.srd)");
        if (path.isEmpty()) return;
        QSaveFile file(path);
        const auto bytes = job.nativeInput ? (job.sourceIsGcode ? job.mappedBytes : job.files.value(job.sourceFile)) : job.inputBytes;
        log(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit() ? "已导出左侧原文。" : "原文导出失败。");
    }

    void load(Job value) {
        if (value.nativeInput && value.sourceIsGcode) restoreMappedPreview(value, nativeTemplate);
        job = std::move(value);
        skippedRows.clear();
        for (const auto& step : job.steps) if (step.skipped) skippedRows.insert(step.row);
        resetProgress();
        fileLabel->setText(job.name + (job.source.isEmpty() ? "" : QString(" · 共 %1 行").arg(job.source.size())));
        fileLabel->setToolTip(fileLabel->text());
        summaryLabel->setText(job.error.isEmpty() ? (job.nativeInput ? job.summary : "已转换为 STEP 原生程序 · " + job.summary) :
            (job.automaticPatch ? "转换失败 · 本次未上传 · " : "解析失败 · 禁止执行 · ") + job.error);
        summaryLabel->setToolTip(!job.error.isEmpty() ? job.error : job.nativeInput ? "控制器执行原生 SRP。左侧可查看 SRP 原文或有版本校验的 G 代码映射。" :
            "G 代码在后台生成 SRP/SRD，控制器执行原生程序，界面用源行映射显示 G 代码进度。可从「工程 / 工具」导出 STEP 工程。");
        rebuildProjectTree();
        renderSource();
        plot->points = &job.points;
        plot->update();
        for (auto it = job.diagnostics.cbegin(); it != job.diagnostics.cend(); ++it) log(QString("L%1 · 未识别，整单禁止执行。%2").arg(it.key()).arg(it.value()));
        if (!job.error.isEmpty()) log(job.error);
        if (!job.mappingWarning.isEmpty()) log(job.mappingWarning);
        if (job.automaticPatch) log(job.previewNote);
        else if (job.nativeInput && !job.source.isEmpty()) log(job.sourceIsGcode ? "已载入 G 代码与原生程序行映射。" :
            "ZIP 未附带有效 G 代码映射，已显示 SRP 原文。执行行按控制器回读高亮。");
        refresh();
    }

    bool confirmLocalChanges() {

        if (!job.modified) return true;

        const auto choice = QMessageBox::question(this, "工程副本尚未另存", "主程序或变量文件已有修改，是否先另存完整工程 ZIP？",

            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);

        if (choice == QMessageBox::Cancel) return false;

        if (choice == QMessageBox::Save) { exportJob(); return !job.modified; }

        return true;

    }




    void renderSource() {
        if(twinPanel)twinPanel->setJobPath(job.points);
        plot->update();
        fileLabel->setText(job.name + (job.modified ? " * 工作副本已修改" : "") + (job.source.isEmpty() ? "" : QString(" · 共 %1 行").arg(job.source.size())));
        fileLabel->setToolTip(fileLabel->text() + (job.automaticPatch ? '\n' + job.programSources.value(job.sourceProgram) : QString()));
        sourceGroup->setTitle(job.automaticPatch ? (job.sourceIsGcode ? "G 代码原文 · 本次自动作业" : job.sourceFile.endsWith(".srd") ? "自动作业 · SRD 本地预览" : "自动作业 · SRP 本地预览") :
            job.sourceIsGcode ? (job.nativeInput ? "G 代码 · 已关联" : "G 代码原文") :
            job.sourceFile.endsWith(".srd") ? "STEP / SRD 变量" : "STEP / SRP 原文");
        sourceTree->setVisible(!job.files.isEmpty());
        sourceHint->setText(job.automaticPatch ? (!job.error.isEmpty() ? job.previewNote :
            (job.sourceIsGcode ? QFileInfo(job.programSources.value(job.sourceProgram)).fileName() + " → " + job.sourceProgram + '\n' : QString()) + job.summary) : job.localEdits ? "工作副本已修改，执行行高亮已暂停。请另存并在 STEPStudio 重新验证，再导入核对版本。" :
            job.sourceFile.endsWith(".srd") ? "变量文件不按运动行高亮。编辑后需另存并重新仿真。" :
            job.nativeInput ? "高亮跟随当前程序的控制器行号。实机运行前需核对柜内程序版本。" : "断开后双击运动行设置仿真起点");
        skippedRows.clear();
        for (const auto& step : job.steps) if (step.skipped) skippedRows.insert(step.row);
        sourceTable->setRowCount(static_cast<int>(job.source.size()));
        sourcePosition->setText(QString("L— / %1").arg(job.source.size()));
        for (int i = 0; i < job.source.size(); ++i) {
            for (int c = 0; c < 3; ++c) sourceTable->setItem(i, c, new QTableWidgetItem);
            sourceTable->item(i, 1)->setText(job.source[i]);
            auto tip = job.source[i];
            if (job.nativeInput && job.sourceIsGcode) {
                for (const auto& step : job.steps)
                    if (step.row == i + 1) tip += QString("\n%1.srp L%2–L%3").arg(job.sourceProgram).arg(step.firstLine).arg(step.lastLine);
            }
            sourceTable->item(i, 1)->setToolTip(tip);
            paintRow(i + 1);
        }
    }

    void selectStart(int row) {
        if (job.nativeInput || session.active() || !job.runnable() || row == job.start || job.source.isEmpty()) return;
        auto full = compileGcodeJob(job.name, job.source, 1, nativeTemplate);
        const bool motion = std::any_of(full.steps.cbegin(), full.steps.cend(), [row](const Step& step) { return step.row == row && step.motionSeconds > 0; });
        if (row != 1 && !motion) return;
        if (row == 1 || QMessageBox::question(this, "设置仿真起点", QString("从 L%1 重新计算增量路径，此前的指令不会执行。确认？").arg(row)) == QMessageBox::Yes) {
            auto partial = compileGcodeJob(job.name, job.source, row, nativeTemplate);
            partial.inputBytes = job.inputBytes;
            load(std::move(partial));
        }
    }

    void connectReal() {
        if (session.active() || linkType->currentIndex() != 1) return;
        const auto parts = ip->text().trimmed().split(':');
        const auto address = QHostAddress(parts.value(0));
        const auto v4 = address.toIPv4Address();
        const bool local = (v4 >> 24) == 10 || (v4 >> 20) == 0xac1 || (v4 >> 16) == 0xc0a8;
        if (!local || parts[0] != address.toString() || parts.size() > 2 || (parts.size() == 2 && parts[1] != "30312")) {
            log("请输入控制柜的内网 IPv4，STEP SDK 端口固定为 30312。");
            return;
        }
        resetProgress();
        hadFeedback = false;
        lastOperationError.clear();
        log("正在连接实机。连接成功后即可切换手动/自动、点动、回起点与启停作业。");
        const auto parameterFile = QDir(workspace).filePath("tmp/pendant_ui_runs/RobotParameter.xml");
        log(QFileInfo::exists(parameterFile) ? "已检测到示教器速度档案，实机点动与回起点可用。" :
            QString("未找到示教器速度档案：启停作业可用，但点动与回起点不可用。可把示教器系统的 RobotParameter.xml 复制到 %1 后重连。").arg(parameterFile));
        session.connectReal(address.toString());
    }

    void prepare() {
        resetProgress();
        hadFeedback = false;
        lastOperationError.clear();
        session.prepare(job);
    }

    void connectModbus() {
        if (modbus.isActive() || linkType->currentIndex() != 2) return;
        const auto parts = ip->text().trimmed().split(':');
        bool ok = true;
        const int port = parts.size() == 2 ? parts[1].toInt(&ok) : 502;
        if (parts.size() > 2 || parts[0].isEmpty() || !ok || port < 1 || port > 65535) {
            log("请填写PLC LAN A地址，例如实际PLC_IP:502；不要填写机器人地址。"); return;
        }
        workstationSettings().setValue("plcBridge/address", ip->text().trimmed());
        resetProgress(); hadFeedback = true; lastOperationError.clear();
        log("连接AC802 PLC，先验证HR300签名和动态扫描心跳；不会自动登记或执行动作。");
        modbus.connectTo(parts[0], quint16(port), 1);
    }

    void modbusStart() { modbus.issue(BridgeProto::Run); }
    void loadModbusProgram() { modbus.issue(BridgeProto::Load, quint16(programNumber->value())); }

    void refreshModbusControls() {
        using namespace BridgeProto;
        const auto s = modbus.status();
        const bool up = modbus.isConnected(), active = modbus.isActive(), live = modbus.fresh();
        modbusGroup->setVisible(true);
        const int maximum = s.valid ? s.maxPrograms : MaxPrograms;
        if (programNumber->maximum() != maximum) programNumber->setMaximum(maximum);
        startButton->setText(s.paused() ? "继续机器人程序" : "运行机器人程序");
        startButton->setEnabled(modbus.canIssue(Run));
        stopButton->setText("停止机器人"); stopButton->setEnabled(modbus.canIssue(Stop));
        modbusPause->setText(s.paused() ? "继续" : "暂停");
        modbusPause->setEnabled(modbus.canIssue(s.paused() ? Run : Pause));
        programNumber->setEnabled(!modbus.pending());
        loadProgramButton->setEnabled(modbus.canIssue(Load, quint16(programNumber->value())));
        modbusReset->setEnabled(modbus.canIssue(ClearError));
        for (auto* button : {modeManual, modeAuto, recordHomeButton, returnHomeButton, cabinetLoadButton}) button->setEnabled(false);
        returnHomeSpeed->setEnabled(false); jogFrame->setEnabled(false); userRefName->setEnabled(false);
        linearJogSpeed->setEnabled(false); angularJogSpeed->setEnabled(false);
        for (auto* button : manualButtons) button->setEnabled(false);
        linkType->setEnabled(!active); ip->setReadOnly(active);
        linkButton->setText(active ? "断开" : "连接"); linkButton->setEnabled(true);
        openButton->setEnabled(canOpenFile());
        const QString connection = live ? "● PLC 状态有效" : (up ? "◐ TCP已连接 · 待验证" : "○ PLC 未连接");
        linkLabel->setText(connection); setVisualProperty(linkLabel, "tone", live ? "success" : up?"warn":"idle");
        machine->setText("AC802 → STEP");
        QString why;
        modbus.canIssue(Run, 0, &why);
        stateLabel->setText(live ? "机器人：" + robotStateText(s) + (why.isEmpty() ? "；已满足运行门槛" : "；" + why) :
            "填写PLC LAN A地址并连接，再打开「PLC 设备控制」页。需要有效签名和变化的PLC扫描心跳。");
        setVisualProperty(stateLabel, "tone", live && (s.alarm() || s.emergency()) ? "error" : "idle");
        telemetry->setText(live && s.has(RobotOnline) ? QString("PLC心跳 %1 · 机器人 %2 · 使能 %3 · 外部自动 %4 · 故障 %5 · 急停 %6")
            .arg(s.plcHeartbeat).arg(robotStateText(s)).arg(s.enabled()).arg(s.externalAuto()).arg(s.alarm()).arg(s.emergency()) :
            "机器人状态无效；不显示旧状态作为实时反馈。");
        runPosition->clear(); progress->setValue(0); runTime->clear();
        versionCheck->setText(live && s.has(LoadConfirmed) ? "加载：操作者已确认示教器上的程序" : "加载：尚未人工确认；本地G代码不会自动下发");
        setVisualProperty(versionCheck, "tone", "idle");
        cabinetInfo->setText(live ? QString("PLC回执 %1：%2 / %3").arg(s.ackSeq).arg(resultText(s.result)).arg(errorText(s.error)) : QString());
        footer->setText(connection + (s.valid ? QString(" | PLC V%1 · 程序号1～%2").arg(s.version).arg(s.maxPrograms) : " | PLC V1/V2待验证") +
            " | 普通停止；实体急停在示教器");
        csvButton->setEnabled(false); experimentLabel->setEnabled(false);
        experimentStatus->setText("PLC设备控制不执行本地G代码，不记录轨迹作业试次");
    }

    void recordHome() {
        if (!session.fresh() || session.value("motion_rest") != "1" || session.value("error") == "1") return;
        const auto key = originDeviceKey(session.simulated, session.model, session.identity, session.address);
        QMap<QString, QString> feedback;
        for (const auto& field : {"motion_rest", "toolname", "axes", "xyz", "abc"}) feedback.insert(field, session.value(field));
        const auto saved = originFromFeedback(feedback, key, session.model);
        if (!saved) { log("起点未保存：需要静止状态、有效六轴回读和工具信息。"); return; }
        const auto path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath("workstation_origins.json");
        const auto error = saveWorkstationOrigin(path, *saved);
        if (!error.isEmpty()) { log(error); return; }
        homeDeviceKey = key;
        workstationOrigin = saved;
        homePlanPending = false;
        homePlanPath.clear();
        plot->plannedHome.clear();
        plot->update();
        // 实机额外把起点写进柜内程序变量（homeSaved/pHome），启动自检的起点检查由它放行；
        // 虚拟模式的起点变量由 capture 上传流程写入，不需要这里发命令。
        if (!session.simulated) {
            QString command = "recordhome";
            for (double value : saved->axes) command += QString(" %1").arg(value, 0, 'f', 6);
            if (session.control && session.request(command))
                log("正在把起点写入柜内程序，结果见下一条日志。");
            else
                log("提示：控制通道忙或未授权，起点只保存在上位机，未写入柜内程序；稍后重新点一次「记录上位机起点」即可补写。");
        }
        log(QString("已将当前 %1 六轴与 TCP 姿态保存为上位机起点，不修改工程文件。").arg(session.model));
        refresh();
    }

    void planHomeReturn() {
        if (!session.fresh() || !session.control || session.value("mode") != "1" ||
            session.value("enabled") != "1" || session.value("motion_rest") != "1" || !workstationOrigin ||
            homeDeviceKey.isEmpty() || homeDeviceKey != originDeviceKey(session.simulated, session.model, session.identity, session.address)) return;
        if (workstationOrigin->tool != session.value("toolname")) {
            QMessageBox::warning(this, "工具不匹配", "当前工具与保存起点时的工具不同。请先恢复原工具，或重新设置起点。");
            return;
        }
        const auto axes = session.value("axes").split(',');
        if (axes.size() != 6) { log("无法规划回程：六轴回读无效。"); return; }
        bool alreadyHome = true;
        for (int i = 0; i < 6; ++i) alreadyHome = alreadyHome && std::abs(axes[i].toDouble() - workstationOrigin->axes[i]) <= .02;
        if (alreadyHome) { QMessageBox::information(this, "已在起点", "当前六轴已在上位机记录的起点范围内，无需移动。"); return; }
        QString command = QString("planhome_tcp %1").arg(returnHomeSpeed->value(), 0, 'f', 2);
        for (double value : workstationOrigin->axes) command += QString(" %1").arg(value, 0, 'f', 6);
        if (command.size() > 500 || !session.request(command)) return;
        homePlanPending = true;
        homePlanReviewedId = 0;
        log("正在校验关节限位并计算回程预览，尚未发出运动命令。");
    }

    void reviewHomePlan(const QString& id) {
        const auto parts = session.value("home_path").split('~');
        if (parts.isEmpty() || parts.first().isEmpty() || id != session.value("home_plan_id") || session.value("home_plan_unit") != "mm_s") {
            homePlanPending = false;
            log("回程路径预览无效，运动未开始。");
            return;
        }
        QStringList rows;
        QVector<Point> planned;
        Point plotReference{};
        if (origin) plotReference = *origin;
        else {
            const auto xyz = session.value("xyz").split(',');
            if (xyz.size() == 3) { plotReference.x = xyz[0].toDouble(); plotReference.y = xyz[1].toDouble(); plotReference.z = xyz[2].toDouble(); }
        }
        int index = 0;
        for (const auto& part : parts) {
            const auto values = part.split(',');
            if (values.size() != 12) { homePlanPending = false; log("回程路径预览数据不完整，运动未开始。"); return; }
            QStringList axes, tcp;
            for (int i = 0; i < 12; ++i) {
                bool ok = false;
                const double value = values[i].toDouble(&ok);
                if (!ok || !std::isfinite(value)) { homePlanPending = false; log("回程路径包含无效数据，运动未开始。"); return; }
                (i < 6 ? axes : tcp) << QString::number(value, 'f', 2);
            }
            Point point{tcp[0].toDouble(), tcp[1].toDouble(), tcp[2].toDouble()};
            point.x -= plotReference.x; point.y -= plotReference.y; point.z -= plotReference.z;
            planned << point;
            rows << QString("%1  A1–A6: %2°\n    WORLD TCP: X %3, Y %4, Z %5 mm  /  A %6, B %7, C %8°")
                .arg(++index).arg(axes.join(" / ")).arg(tcp[0], tcp[1], tcp[2], tcp[3], tcp[4], tcp[5]);
        }
        homePlanReviewedId = id.toULongLong();
        homePlanPending = false;
        homePlanPath = rows.join('\n');
        plot->plannedHome = planned;
        plot->update();
        const auto estimated = session.value("home_plan_seconds").toDouble();
        QMessageBox message(QMessageBox::Warning, "返回起点路径预览",
            QString("上位机按六轴关节分段规划，预计约 %1。末端速度上限 %2 mm/s，实际速度可能更低。\n\n%3\n\n仅做关节限位检查和 TCP 正运动学预览；没有工件、夹具和围栏模型，未执行环境碰撞检测。%4")
                .arg(duration(estimated)).arg(session.value("home_plan_speed")).arg(homePlanPath,
                session.simulated ? "当前为隔离仿真执行。" : "确认后机器人将真实移动：请先清场，确认整个路径净空，现场人员撤离再确认。"),
            QMessageBox::Cancel, this);
        auto* run = message.addButton("确认低速返回", QMessageBox::AcceptRole);
        message.setDefaultButton(QMessageBox::Cancel);
        message.exec();
        if (message.clickedButton() != run) {
            session.request(QString("cancelhome %1").arg(id));
            log("已取消回程，机器人未移动。");
            return;
        }
        if (!session.fresh() || session.value("home_plan_ready") != "1" || session.value("home_plan_id") != id ||
            session.value("mode") != "1" || session.value("motion_rest") != "1" || !session.request("returnhome " + id)) {
            log("回程确认过期或控制器状态已变化，未发送运动命令。请重新规划。");
            return;
        }
        log("回程已确认，按预览路径在手动模式低速执行；可随时点「停止发送」。");
    }

    bool loaded() const {
        const QString project = session.simulated ? job.project : "SR175_3200_Helix.sr";
        const QString program = session.simulated ? job.program : "helix_25mms";
        return !project.isEmpty() && (session.value("project") == project || session.value("project") == project.chopped(3)) &&
            session.value("program") == program;
    }

    bool ready() const { return session.fresh() && session.control && session.pending.isEmpty() && !session.waitingStop; }

    bool canStart() const {
        // 实机可执行作业：G 代码直开、已核对螺旋 v4、带 G 代码映射的原生工程；
        // 仅查阅的 STEP ZIP（无映射且非模板）仍拦截。
        if (!session.simulated && job.nativeInput && !job.reviewed && !job.trustedGcode) return false;
        const auto override = session.value("override").toDouble();
        // 实机程序不一致时仍放行启动：点开始由控制端自动卸载旧程序并加载作业程序，
        // 安全前置（自动/连续/使能/倍率/首行/起点匹配）全部在控制端启动自检完成，这里不重复拦截。
        const bool cabinetMismatch = !session.simulated && !job.program.isEmpty() && !loaded();
        return !experiment.active() && ready() && (loaded() || cabinetMismatch) && !homeDirty &&
            (!session.simulated || session.value("outcome") == "none") && session.value("error") != "1" &&
            session.value("continuous") == "1" && session.value("mode") == "2" && session.value("enabled") == "1" &&
            (cabinetMismatch || (session.value("state") == "2" && (session.value("line") == "1" || session.value("line") == "2"))) &&
            (session.simulated || override > 0);
    }

    // 启动按钮实机连接后常亮，条件不满足时点击给出第一条具体原因，不再静默不动。
    QString startBlockReason() const {
        const bool cabinetMismatch = !session.simulated && !job.program.isEmpty() && !loaded();
        if (!ready()) return "连接未就绪或上一条操作还在等待回读，请稍候再试。";
        if (experiment.active()) return "实验记录进行中：请先停止当前作业再重新开始。";
        if (!session.simulated && job.nativeInput && !job.reviewed && !job.trustedGcode)
            return "当前打开的是仅查阅的 STEP ZIP，不能上机：请直接打开 G 代码文件（.nc/.gcode 等），或载入已核对的螺旋 v4 模板。";
        if (!loaded() && !cabinetMismatch) return "柜内没有载入作业对应的工程与程序：请先在示教器上载入后再启动。";
        if (homeDirty) return "上位机起点与当前记录不一致：请重新规划回程或重设起点。";
        if (session.value("error") == "1") return "控制器正在报错：请先在柜内查看并清除报警。";
        if (session.value("mode") != "2") return "需处于自动模式：请点上方「自动」按钮切换。";
        if (session.value("continuous") != "1") return "柜内需处于连续运行方式：请到示教器切换。";
        if (session.value("enabled") != "1") return "伺服未使能：请到示教器使能后再启动。";
        if (!cabinetMismatch && (session.value("state") != "2" || (session.value("line") != "1" && session.value("line") != "2")))
            return "程序须停在首行才能启动：请在示教器重新载入程序，使其回到首行。";
        if (!session.simulated && !(session.value("override").toDouble() > 0)) return "倍率需大于 0：请在示教器上调整倍率后重试。";
        return QString();
    }

    bool canReprepare() const {
        const auto state = session.value("state");
        return !experiment.active() && session.simulated && job.runnable() && ready() && (state == "1" || state == "2") && !canStart() && session.canClose();
    }

    void start() {
        if (linkType->currentIndex() == 2) {
            if (QMessageBox::question(this, "运行机器人程序", "将运行/继续示教器上已加载并人工确认的程序。本地G代码不会自动下发。确认运行？",
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) modbusStart();
            return;
        }
        if (!session.simulated && job.nativeInput && !job.reviewed && !job.trustedGcode) {
            log("实机运行前需确认作业可执行：当前打开的是仅查阅的 STEP ZIP。请直接打开 G 代码文件（.nc/.gcode 等），或载入已核对的螺旋 v4 模板 ZIP。");
            return;
        }
        if (!session.active() && linkType->currentIndex() == 0) { prepare(); return; }
        if (canReprepare()) { session.disconnect(); if (!session.active()) prepare(); return; }
        if (!canStart()) {
            const auto reason = startBlockReason();
            if (!reason.isEmpty()) log("暂不能开始 · " + reason);
            return;
        }
        if (QMessageBox::question(this, "开始前确认", "确认护目镜、围栏和示教器实体急停已检查。当前为空跑，出光未接入。") == QMessageBox::Yes && canStart())
            queueExperiment();
    }

    bool queueExperiment() {
        if (!canStart()) return false;
        const auto reference = session.simulated ? origin : std::nullopt;
        if (!experiment.begin(recordRoot, job, experimentLabel->text().trimmed(), session.simulated,
                              session.model, session.value("override"), reference)) {
            log("无法创建实验记录，本次启动未发送。请检查磁盘空间和记录目录权限。");
            return false;
        }
        startFailure.clear();
        confirmedStop = captureGap = reportedWriteError = false;
        plot->measured.clear();
        captureFeedback(0, false);
        experiment.event("start_requested_by_workstation");
        if (!experiment.healthy() || !session.request("start")) {
            finishExperiment("start_not_sent");
            return false;
        }
        log("实验记录已开始。");
        return true;
    }

    void finishExperiment(const QString& result, std::optional<double> endpoint = {}) {
        if (!experiment.active()) return;
        if (!experiment.finish(result, endpoint)) log("实验摘要写入失败。已写入的样本保留，请检查记录目录。");
        else log("实验记录已封存 · " + experiment.folder());
    }

    void captureFeedback(int observedSource, bool evaluateOutcome) {
        if (!experiment.active()) return;
        if (captureGap) {
            experiment.event("feedback_gap_ended");
            captureGap = false;
        }
        QMap<QString, QString> feedback;
        for (const auto* key : {"state", "line", "mode", "enabled", "override", "alarm", "speed", "xyz", "abc", "axes"})
            feedback.insert(key, session.value(key));
        const auto state = session.value("state");
        const auto outcome = session.value("outcome");
        const auto measured = loaded() && origin && !homeDirty ? plot->tcp : std::nullopt;
        const auto distance = measured && state == "0" ? distanceToPlan(job, *measured, observedSource) : std::nullopt;
        experiment.sample(feedback, observedSource, measured, distance);
        if (measured && (state == "0" || experiment.observedRunning())) {
            if (plot->measured.size() >= 5000) plot->measured.removeFirst();
            plot->measured << *measured;
            plot->update();
        }
        if (!experiment.healthy() && !reportedWriteError) {
            reportedWriteError = true;
            log("实验记录写入失败，已保存样本保留。记录将标为不完整。");
        }
        if (!evaluateOutcome || session.pending == "start") return;
        if (!startFailure.isEmpty()) {
            if (state != "0") { finishExperiment(startFailure); return; }
            startFailure.clear();
        }
        if (session.simulated && outcome == "completed" && state == "2") {
            std::optional<double> endpoint;
            if (measured && !job.points.isEmpty()) {
                const auto target = job.points.last();
                endpoint = std::hypot(measured->x - target.x, measured->y - target.y, measured->z - target.z);
            }
            finishExperiment("completed", endpoint);
        } else if (confirmedStop && outcome == "stopped" && (state == "1" || state == "2")) {
            finishExperiment("stopped");
        } else if (outcome == "ended_unverified" && (state == "1" || state == "2")) {
            finishExperiment("ended_unverified");
        }
    }

    void log(const QString& text) {
        QString display = text;
        if (text.startsWith("Refused:")) {
            lastOperationError = refuseHint(text.mid(8));
        } else if (text.startsWith("auto_load=loading")) {
            display = "柜内程序与作业不一致，正在自动卸载旧程序并加载作业程序…";
        } else if (text.startsWith("auto_load=loaded")) {
            display = "作业程序已自动加载到柜内，继续启动自检。";
        } else if (text == "load_program=already") {
            display = "柜内已是作业程序，无需加载。";
        } else if (text == "load_program=ok") {
            display = "作业程序已加载到柜内。";
        } else if (text == "program_home_recorded=1") {
            display = "起点已写入柜内程序（homeSaved/pHome），启动自检的起点检查将通过。";
        } else if (text == "virtual_command_result=refused" || text.startsWith("result=unconfirmed") ||
            text.startsWith("jog_frame_unconfirmed") || text.startsWith("base_reference_unconfirmed")) {
            lastOperationError = "操作未确认，请查看记录中的原因";
        } else if (text == "virtual_command_result=ok" || text.startsWith("start_confirmed=") || text.startsWith("stop_confirmed=")) {
            lastOperationError.clear();
        }
        const auto row = QDateTime::currentDateTime().toString("HH:mm:ss  ") + display;
        if(plcMonitor)plcMonitor->append(display);
        logs->appendPlainText(row);
        if (!session.logFolder.isEmpty()) {
            QFile output(QDir(session.logFolder).filePath("panel.log"));
            if (output.open(QIODevice::Append)) output.write((row + '\n').toUtf8());
        }
        if (experiment.active()) {
            experiment.event(text);
            if (text.startsWith("start_cancelled=")) {
                finishExperiment("start_cancelled");
            } else if (session.pending == "start") {
                if (text.startsWith("Refused:")) startFailure = "start_rejected";
                else if (text.startsWith("result=unconfirmed")) startFailure = "start_unconfirmed";
            } else if (text.startsWith("stop_confirmed=")) confirmedStop = true;
        }
        if (text == "virtual_command_result=ok" && session.pending == "capture") resetProgress();
    }

    void resetProgress() {
        current = done = donePoint = activePoint = 0;
        executedRows.clear();
        seconds = 0;
        lastRun = -1;
        homeDirty = false;
        origin.reset();
        if (plot) { plot->tcp.reset(); plot->plannedHome.clear(); plot->measured.clear(); plot->completed = plot->active = 0; plot->update(); }
        if (sourceTable && sourceTable->rowCount() == job.source.size()) {
            for (int i = 1; i <= job.source.size(); ++i) paintRow(i);
        }
    }

    void paintRow(int row) {
        if (row < 1 || row > sourceTable->rowCount()) return;
        QString flag;
        QString mark;
        QColor bg("#f1f5f7");
        if (job.diagnostics.contains(row)) { flag = "未识别 · 跳过"; mark = "!"; bg = QColor("#fce4e7"); }
        else if (skippedRows.contains(row)) { flag = "空跑 · 跳过"; mark = "·"; }
        else if (row == current) { mark = "▶"; bg = QColor("#ffebad"); }
        else if (job.nativeInput ? executedRows.contains(row) : row >= job.start && row <= done) { mark = "✓"; bg = QColor("#e0f3e7"); }
        else if (row == job.start && row != 1) { flag = "仿真起点"; bg = QColor("#e4f1fd"); }
        else if (row < job.start) flag = "未执行";
        sourceTable->item(row - 1, 0)->setText(QString::number(row) + " " + mark);
        sourceTable->item(row - 1, 2)->setText(flag);
        for (int c = 0; c < 3; ++c) sourceTable->item(row - 1, c)->setBackground(bg);
    }

    void sample() {
        if(twinPanel && session.value("motion_rest")=="1")twinPanel->acceptSdk(session.value("axes"),session.simulated,session.value("xyz"));
        hadFeedback = true;
        if (session.value("command_busy") == "1" && session.pending.startsWith("jog ")) ++streamedJogSamples;
        const auto state = session.value("state");
        int observedSource = 0;
        if (job.nativeInput) {
            const int previous = current;
            observedSource = sourceRowForNativeLine(job, session.value("project"), session.value("program"), session.value("line").toInt());
            current = state == "0" || state == "1" ? observedSource : 0;
            if (previous && previous != current && (current || session.value("outcome") == "completed")) executedRows.insert(previous);
            if (current) {
                done = current;
                sawSource = true;
            }
            paintRow(previous);
            paintRow(current);
            if (current) sourceTable->scrollToItem(sourceTable->item(current - 1, 0), QAbstractItemView::EnsureVisible);
        }
        const auto now = elapsed.elapsed();
        if (lastRun >= 0) seconds += std::min<qint64>(3000, now - lastRun) / 1000.;
        lastRun = state == "0" ? now : -1;
        if (session.control && (loaded() || session.value("home_returning") == "1")) {
            const auto xyz = session.value("xyz").split(',');
            if (xyz.size() == 3) {
                bool xOk = false, yOk = false, zOk = false;
                const Point point{xyz[0].toDouble(&xOk), xyz[1].toDouble(&yOk), xyz[2].toDouble(&zOk)};
                const bool poseValid = xOk && yOk && zOk && std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
                const auto line = session.value("line").toInt();
                if (poseValid && !origin && !homeDirty && state == "2" && (line == 1 || line == 2)) origin = point;
                if (poseValid && origin) plot->tcp = Point{point.x - origin->x, point.y - origin->y, point.z - origin->z};
                else plot->tcp.reset();
                const int previous = current, previousDone = done;
                if (session.value("home_returning") == "1") {
                    if (!job.nativeInput) current = 0;
                    activePoint = 0;
                } else if (session.value("outcome") == "completed" && state == "2") {
                    if (!job.nativeInput || (job.trustedGcode && job.sourceIsGcode)) done = static_cast<int>(job.source.size());
                    current = activePoint = 0;
                    donePoint = static_cast<int>(job.points.size()) - 1;
                    if (job.trustedGcode && job.sourceIsGcode) {
                        for (int row = 1; row <= job.source.size(); ++row) { executedRows.insert(row); paintRow(row); }
                    }
                } else if ((!job.nativeInput || job.trustedGcode) && (state == "0" || state == "1")) {
                    if (!job.nativeInput || job.sourceIsGcode) current = 0;
                    activePoint = 0;
                    const auto& steps = job.trustedGcode && !job.sourceIsGcode ? job.mappedSteps : job.steps;
                    for (const auto& step : steps) {
                        if (step.skipped || line < step.firstLine || line > step.lastLine) continue;
                        if (!job.nativeInput || job.sourceIsGcode) { current = step.row; observedSource = step.row; }
                        if (current != previous) rowEntered = seconds;
                        done = std::max(done, current - 1);
                        const auto pointIndex = step.firstPoint + (line - step.firstLine) / 2;
                        activePoint = step.lastPoint > step.firstPoint ? std::min(step.lastPoint, pointIndex + 1) : 0;
                        donePoint = std::max(donePoint, activePoint ? activePoint - 1 : step.firstPoint);
                        sawSource = true;
                        break;
                    }
                } else if (job.nativeInput && job.reviewed && plot->tcp && (state == "0" || state == "1")) {
                    const auto nativeLines = QString::fromUtf8(job.files.value(job.program + ".srp")).split('\n');
                    const auto instruction = nativeLines.value(line - 1).trimmed();
                    int first = 0, last = 0;
                    if (instruction.startsWith("Lin(cpOrigin,")) first = last = 1;
                    else if (instruction.startsWith("LinRel(dZ2,")) first = last = 2;
                    else if (instruction.startsWith("LinRel(dZ10,")) first = last = 3;
                    else if (instruction.startsWith("Lin(cpTarget,")) { first = 4; last = static_cast<int>(job.points.size()) - 1; }
                    activePoint = 0;
                    double closest = std::numeric_limits<double>::infinity();
                    for (int index = first; index > 0 && index <= last; ++index) {
                        const auto a = job.points[index - 1], b = job.points[index], p = *plot->tcp;
                        const Point v{b.x - a.x, b.y - a.y, b.z - a.z};
                        const auto length2 = v.x * v.x + v.y * v.y + v.z * v.z;
                        const double t = length2 > 0 ? std::clamp(((p.x - a.x) * v.x + (p.y - a.y) * v.y + (p.z - a.z) * v.z) / length2, 0., 1.) : 0.;
                        const auto distance = std::hypot(p.x - a.x - t * v.x, p.y - a.y - t * v.y, p.z - a.z - t * v.z);
                        if (distance < closest) { closest = distance; activePoint = index; }
                    }
                    if (activePoint) donePoint = std::max(donePoint, activePoint - 1);
                } else { if (!job.nativeInput) current = 0; activePoint = 0; }
                if (!job.nativeInput) {
                    paintRow(previous);
                    for (int row = previousDone + 1; row <= done; ++row) paintRow(row);
                    paintRow(current);
                    if (current) sourceTable->scrollToItem(sourceTable->item(current - 1, 0), QAbstractItemView::EnsureVisible);
                }
                plot->completed = donePoint;
                plot->active = activePoint;
                plot->update();
            } else plot->tcp.reset();
        }
        captureFeedback(observedSource, true);
        if (csv.isOpen()) {
            // 遥测的 xyz/abc/axes 在协议里就是逗号串，直接拼接正好各占 3/3/6 列，不能套 joinCsv（会整体加引号挤成一列）。
            const auto line = QStringList{QDateTime::currentDateTime().toString(Qt::ISODateWithMs), session.value("state"),
                session.value("mode"), session.value("override"), session.value("line"), session.value("alarm"), session.value("speed"),
                session.value("xyz"), session.value("abc"), session.value("axes"), session.value("jogset"), session.value("jogunit")}.join(',');
            if (csv.write((line + '\n').toUtf8()) < 0) { csv.close(); log("CSV 写入失败。"); }
            else csv.flush();
        }
    }

    void toggleCsv() {
        if (csv.isOpen()) { csv.close(); refresh(); return; }
        if (!session.fresh()) return;
        const auto path = QFileDialog::getSaveFileName(this, "保存遥测", "step_telemetry.csv", "CSV (*.csv)");
        if (path.isEmpty()) return;
        csv.setFileName(path);
        if (csv.open(QIODevice::WriteOnly)) csv.write("host_time,state,mode,override,line,message_id,tcp_mm_s,X,Y,Z,A,B,C,A1,A2,A3,A4,A5,A6,manual_speed,manual_speed_unit\n");
        else log("无法写入 CSV。");
        refresh();
    }

    void refresh() {
        const bool manualEngaged = plcManual && plcManual->engaged();
        if(welderPanel)welderPanel->setModeEnabled(linkType->currentIndex()==2 || modbusLinked);
        gcodeDeliveryButton->setEnabled(!session.active() && autoUploadIdle() && !experiment.active() && canOpenFile());
        gcodeDeliveryButton->setText(Delivery::currentSource(job).path.isEmpty() ? "导入 G 代码并自动上传…" : "上传当前 G 代码 / 更换文件…");
        gcodeDeliveryButton->setToolTip(gcodeDeliveryButton->isEnabled()?"默认使用当前显示的G代码；可更换文件，并选择PLC转传或电脑直传，更新11号作业":"请先停止、断使能并退出微调");
        exportAction->setEnabled(!job.automaticPatch && job.error.isEmpty() && !job.files.isEmpty());
        saveProjectButton->setEnabled(exportAction->isEnabled());
        saveProjectButton->setToolTip(job.automaticPatch ? "补丁已由导入窗口保存为 AutoJobPatch.zip；不含工程起点变量，不能另存为完整工程" : "另存当前完整工程副本");
        associateAction->setEnabled(!job.automaticPatch && job.nativeInput && canOpenFile() && job.error.isEmpty());
        saveSourceAction->setEnabled(!job.source.isEmpty());
        sourceTree->setEnabled(!experiment.active());
        const auto selectedFile = sourceTree->currentItem() ? sourceTree->currentItem()->data(0, Qt::UserRole).toString() : QString();
        editFileButton->setText(job.automaticPatch ? "查看文件…" : "编辑文件…");
        editFileButton->setEnabled(canOpenFile() && job.files.contains(selectedFile));
        const bool plcMode = modbusLinked || linkType->currentIndex() == 2;
        if(twinPanel)twinPanel->setContext(plcMode,session.active()||modbus.isActive(),session.fresh()&&session.value("motion_rest")=="1");
        if (auto* dry = findChild<QCheckBox*>("dry")) dry->setVisible(!plcMode);
        if (auto* boundary = findChild<QLabel*>("processBoundary"))
            boundary->setText(plcMode ? "本接口不下发空跑、焊机或送丝参数；实际输出取决于机器人已加载程序及现场联锁。" : "激光接口未接入");
        if (plcPanel) plcPanel->setModeEnabled(linkType->currentIndex() == 2);
        if (plcManual) plcManual->setModeEnabled(linkType->currentIndex() == 2);
        refreshWorkspaceLayout();
        if (modbusLinked || linkType->currentIndex() == 2) {
            refreshModbusControls();
            return;
        }
        modbusGroup->setVisible(false);
        const bool fresh = session.fresh(), idle = !session.active();
        if (fresh) {
            const auto key = originDeviceKey(session.simulated, session.model, session.identity, session.address);
            if (key != homeDeviceKey) {
                homeDeviceKey = key;
                workstationOrigin = readWorkstationOrigin(
                    QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath("workstation_origins.json"), key);
            }
        }
        if (homePlanPending && fresh && session.pending.isEmpty() && session.value("home_plan_ready") == "1" &&
            session.value("home_plan_id").toULongLong() != homePlanReviewedId) {
            const auto id = session.value("home_plan_id");
            QTimer::singleShot(0, this, [this, id] { reviewHomePlan(id); });
        } else if (homePlanPending && !session.value("home_plan_error").isEmpty()) {
            homePlanPending = false;
        }
        if (experiment.active() && !fresh && !captureGap) {
            captureGap = true;
            experiment.event("feedback_gap_started");
            if (!plot->measured.isEmpty()) plot->measured << Point{std::numeric_limits<double>::quiet_NaN(), 0, 0};
        }
        const auto state = session.value("state");
        const bool settled = state == "1" || state == "2";
        const bool operable = !experiment.active() && ready() && settled;
        const auto mode = session.value("mode");
        const auto rate = session.value("override").toDouble();
        const auto jogRef = session.value("jogref");
        // 伺服使能不再作为按钮前置：未使能时点动由控制端按柜子回执报错提示，柜侧状态由操作者处理。
        const bool manual = operable && (mode == "0" || mode == "1");
        const auto jointLimits = session.value("manual_joints").split(',');
        double maximumReturnSpeed = 0;
        if (session.value("manual_profile") == "1") {
            const auto limit = session.value("manual_return_linear").toDouble();
            if (std::isfinite(limit)) maximumReturnSpeed = std::clamp(limit, 0., 250.);
        }
        returnHomeSpeed->setMaximum(maximumReturnSpeed >= .1 ? maximumReturnSpeed : 250.);
        const bool complete = fresh && session.value("outcome") == "completed";
        QString runText = !fresh ? (session.preparing || (session.connected() && !hadFeedback) ? "… 连接中" :
                                   session.active() || session.lost ? "! 状态未知" : "○ 待命") :
            complete ? "✓ 已完成" : state == "0" ? "▶ 运行中" : state == "1" ? "Ⅱ 已暂停" : state == "2" ? "■ 已停止" : "↺ 回归中";
        if (!job.error.isEmpty()) runText = "! 解析失败";
        else if (fresh && session.value("error") == "1") runText = "! 控制器错误";
        const QString connection = fresh ? (session.simulated ? "● 仿真已连接" : "● 实机已连接") :
            session.preparing ? "○ 正在准备" : session.connected() ? "○ 等待回读" : session.lost ? "! 已断开" : "○ 未连接";
        linkLabel->setText(connection);
        const bool linkError = session.lost || (session.connected() && hadFeedback && !fresh);
        const bool returning = fresh && session.value("home_returning") == "1";
        if (returning) runText = "↩ 返回起点中";
        const bool running = returning || session.preparing || (session.connected() && !fresh && !hadFeedback) ||
            (fresh && (state == "0" || state == "3")) || !session.pending.isEmpty() || session.waitingStop;
        const bool error = !job.error.isEmpty() || (fresh && (session.value("error") == "1" || session.value("home_return_result") == "unconfirmed")) || linkError || !lastOperationError.isEmpty();
        const QString tone = error ? "error" : running ? "busy" : "idle";
        setVisualProperty(linkLabel, "tone", linkError ? "error" : session.active() ? "busy" : "idle");
        machine->setText(fresh ? session.model : "SR175-3200");
        linkType->setEnabled(idle);
        ip->setReadOnly(!idle);
        sampleAction->setEnabled(canOpenFile());
        openButton->setEnabled(canOpenFile());
        linkButton->setText(session.connected() ? "断开" : "连接");
        const bool prepareable = idle && linkType->currentIndex() == 0 && (job.name.isEmpty() || job.runnable());
        linkButton->setEnabled((idle && linkType->currentIndex() == 1) || prepareable || (session.connected() && session.canClose()));
        // 实机连接后开始按钮常亮：条件不满足时点击会提示具体原因，前置状态交给操作者在柜侧处理。
        const bool realLive = fresh && !session.simulated && ready() && session.control;
        startButton->setText(
            canStart() ? (session.simulated ? "开始仿真作业" : "请求实机启动") : canReprepare() ? "重新准备仿真" : prepareable ? "准备仿真" : realLive ? "请求实机启动" : "开始");
        startButton->setEnabled(canStart() || canReprepare() || prepareable || realLive);
        stopButton->setEnabled(session.control && session.connected() && !session.waitingStop &&
            (session.pending == "start" || session.pending.startsWith("returnhome ") || returning ||
             (fresh && session.value("home_return_result") == "unconfirmed" && session.value("motion_rest") != "1") ||
             (fresh && loaded() && (state == "0" || state == "1" || state == "3"))));
        modeManual->setEnabled(operable && (mode != "0" && mode != "1"));
        modeAuto->setEnabled(operable && (mode != "2" || session.value("enabled") != "1"));
        cabinetLoadButton->setVisible(session.connected() && !session.simulated);
        cabinetLoadButton->setEnabled(fresh && session.control && !returning &&
            session.pending.isEmpty() && !session.waitingStop);
        const bool canSaveOrigin = fresh && session.value("motion_rest") == "1" && session.value("error") != "1" &&
            session.pending.isEmpty() && !session.waitingStop && !returning;
        const bool matchingHome = workstationOrigin && !homeDeviceKey.isEmpty() &&
            homeDeviceKey == originDeviceKey(session.simulated, session.model, session.identity, session.address) &&
            workstationOrigin->tool == session.value("toolname");
        recordHomeButton->setEnabled(canSaveOrigin);
        returnHomeButton->setEnabled(manual && maximumReturnSpeed >= .1 && matchingHome && !returning &&
                                     session.pending.isEmpty() && !session.waitingStop);
        returnHomeSpeed->setEnabled(manual && maximumReturnSpeed >= .1 && !returning);
        const auto homeResult = session.value("home_return_result");
        const QString originAxes = workstationOrigin ? QStringList{QString::number(workstationOrigin->axes[0], 'f', 2),
            QString::number(workstationOrigin->axes[1], 'f', 2), QString::number(workstationOrigin->axes[2], 'f', 2),
            QString::number(workstationOrigin->axes[3], 'f', 2), QString::number(workstationOrigin->axes[4], 'f', 2),
            QString::number(workstationOrigin->axes[5], 'f', 2)}.join(" / ") : QString();
        const QString resultText = returning ? "↩ 正在返回" : homeResult == "arrived" ? "✓ 已回到上位机起点" :
            homeResult == "unconfirmed" ? "! 回程结果待确认" : homeResult == "stopped" ? "■ 回程已停止" : "已保存上位机起点";
        homeInfo->setText(workstationOrigin ? QString("%1 · %2 · A1–A6：%3° · 工具 %4 · %5")
            .arg(fresh && !matchingHome ? "当前控制器或工具与起点不匹配" : resultText, workstationOrigin->model,
                 originAxes, workstationOrigin->tool, workstationOrigin->savedUtc) :
            fresh ? "此控制器尚未设置上位机起点。手动移动到目标姿态后，点“将当前位置设为起点”。" : "连接控制器后显示该设备已保存的上位机起点。");
        setVisualProperty(homeInfo, "tone", homeResult == "unconfirmed" ? "error" : returning ? "busy" : "idle");
        const bool profileReady = session.value("manual_profile") == "1";
        const int reportedFrame = jogRef.toInt();
        const int frame = reportedFrame == 4 && session.value("refname") == "ROBOTBASE" ? 2 : reportedFrame;
        jogFrame->setEnabled(manual);
        if (session.pending != "joints" && session.pending != "world" && session.pending != "base" && session.pending != "tool" && !session.pending.startsWith("user")) {
            const QSignalBlocker blocked(jogFrame);
            jogFrame->setCurrentIndex(fresh && frame >= 0 && frame <= 4 ? frame : -1);
        }
        userRefName->setVisible(jogFrame->currentIndex() == 4);
        userRefName->setEnabled(manual);
        linearJogSpeed->setEnabled(manual && frame > 0 && frame <= 4 && profileReady);
        angularJogSpeed->setEnabled(manual && profileReady);
        if (profileReady && frame == 0 && jointLimits.size() == 6) {
            double limit = 360;
            for (const auto& item : jointLimits) limit = std::min(limit, item.toDouble());
            angularJogSpeed->setMaximum(limit);
        } else if (profileReady) angularJogSpeed->setMaximum(session.value("manual_angular").toDouble());
        const double linearSpeed = linearJogSpeed->value(), angularSpeed = angularJogSpeed->value();
        for (int i = 0; i < manualButtons.size(); ++i) {
            const bool needsLinear = frame != 0 && i / 2 < 3;
            manualButtons[i]->setEnabled(manual && profileReady && frame >= 0 && frame <= 4 &&
                                         (needsLinear ? linearSpeed > 0 : angularSpeed > 0));
        }
        const auto joints = session.value("axes").split(',');
        QStringList pose;
        if (frame == 1) pose = (session.value("xyz") + ',' + session.value("abc")).split(',');
        else if (frame == 2) pose = session.value("base").split(',');
        else if (frame == 4) pose = session.value("user").split(',');
        for (int i = 0; i < 6; ++i) {
            const auto axis = frame == 0 ? QString("A%1").arg(i + 1) : QString("XYZABC").mid(i, 1);
            const auto position = frame == 0 ? joints.value(i) : pose.value(i);
            const auto unit = frame == 0 || i >= 3 ? " °" : " mm";
            axisLabels[i]->setText(axis);
            manualValues[i]->setText(fresh && !position.isEmpty() ? QString::number(position.toDouble(), 'f', 2) + unit :
                                     frame == 3 && fresh ? "沿工具坐标轴" : "等待回读");
            jointReadouts[i]->setText(QString("A%1  %2").arg(i + 1).arg(fresh && joints.size() == 6 ? QString::number(joints[i].toDouble(), 'f', 2) + " °" : "等待回读"));
        }
        if (!fresh) manualFeedback->setText("连接虚拟控制器或实机后显示六轴角度，连接后即可点动。");
        else if (!session.simulated && !profileReady)
            manualFeedback->setText("手动速度档案未加载：点动暂不可用。重连后控制端会自动改用内置默认限速；也可把示教器的 RobotParameter.xml 复制到工作区 tmp\\pendant_ui_runs\\ 后重连。");
        else if (!session.simulated) manualFeedback->setText("六轴角度为控制柜回读。");
        else if (!profileReady) manualFeedback->setText("尚未核对虚拟控制器手动速度参数。");
        else if (angularSpeed <= 0 || (frame > 0 && frame <= 4 && linearSpeed <= 0)) manualFeedback->setText("手动速度回读未就绪，暂不能点动。");
        else if (session.value("jogset").toDouble() > 0) manualFeedback->setText(QString("控制器速度设定 %1 %2 · 单次 0.30 s · TCP 上限 250 mm/s")
            .arg(session.value("jogset"), session.value("jogunit") == "mm_s" ? "mm/s" : "°/s"));
        else manualFeedback->setText("输入目标速度，控制器按可用精度向下取值并回读应用速度。单次 0.30 s。");
        if (moveBusy) {
            bool seqKnown = false;
            const auto seq = session.value("move_seq").toLongLong(&seqKnown);
            if (seqKnown && seq != moveSeqSeen) {
                moveBusy = false;
                moveSeqSeen = seq;
                const auto result = session.value("move_state");
                setVisualProperty(moveFeedback, "tone", result == "done" ? "idle" : result == "aborted" ? "warn" : "error");
                if (result == "done") moveFeedback->setText("✓ 运动语句执行完成，可继续发送下一条或点动微调。");
                else if (result == "aborted") moveFeedback->setText("■ 运动语句已中止（停止请求或断开触发）。");
                else moveFeedback->setText("! 运动语句被拒绝：" + session.value("move_reason") + "。调整参数或状态后重试。");
            }
        }
        const bool moveFree = !moveBusy && session.pending.isEmpty() && !session.waitingStop;
        const bool moveEnable = manual && profileReady && fresh && moveFree && session.value("motion_rest") == "1";
        const bool moveCartesianOk = moveType->currentIndex() == 0 || (frame == 1 || frame == 2 || frame == 4);
        moveType->setEnabled(moveEnable);
        moveDyn->setEnabled(moveEnable);
        moveOvl->setEnabled(moveEnable);
        for (auto* field : moveFields) field->setEnabled(moveEnable);
        for (auto* field : moveHelp) field->setEnabled(moveEnable);
        moveFetchButton->setEnabled(moveEnable && (moveType->currentIndex() == 0 ? joints.size() == 6 : frame >= 1 && frame <= 4 && frame != 3));
        moveSendButton->setEnabled(moveEnable && moveCartesianOk);
        if (!fresh) {
            if (current) {
                const auto old = current;
                current = 0;
                paintRow(old);
            }
            plot->tcp.reset();
            plot->active = 0;
            plot->update();
            lastRun = -1;
        }
        QString instruction = runText;
        QPushButton* next = nullptr;
        const bool linkDown = session.lost || (session.connected() && hadFeedback && !fresh);
        const bool stopUnconfirmed = fresh && session.value("home_return_result") == "unconfirmed" && session.value("motion_rest") != "1";
        if (!job.error.isEmpty()) {
            instruction = QString("需处理 · 程序解析未通过%1。下一步：修正左侧标红的指令后重新打开文件。")
                .arg(job.diagnostics.isEmpty() ? QString() : QString("（共 %1 处问题）").arg(job.diagnostics.size()));
            next = openButton;
        } else if (linkDown) {
            instruction = "需处理 · 通信中断。停止命令可能已发出但无法确认：先到柜子现场确认机器人已停稳，再检查网线与连接，重连后核对状态。实机状态以现场为准。";
        } else if (session.waitingStop) {
            instruction = "进行中 · 已请求停止，等待控制器确认。若长时间无回读，请到柜子现场确认；物理急停始终最快。";
        } else if (stopUnconfirmed) {
            instruction = "需处理 · 停止结果未确认：柜内未回读到“已停稳”。请到柜子确认机器人状态，必要时在示教器上手动停止。";
        } else if (linkType->currentIndex() == 2) instruction = "Modbus TCP 未接入";
        else if (session.preparing) instruction = "进行中 · 准备 STEP 虚拟控制器";
        else if (returning) instruction = "进行中 · 返回记录的起点";
        else if (!session.pending.isEmpty()) instruction = "进行中 · 等待操作回读（约 3 秒未回读会自动撤销本次操作）";
        else if (idle) { instruction = "下一步 · 连接运行目标"; next = linkButton; }
        else if (!fresh) instruction = hadFeedback ? "需处理 · 回读中断，软件在自动重试。连续中断请按通信中断处理：到柜子现场确认状态。" : "进行中 · 等待首次回读";
        else if (job.name.isEmpty() && canOpenFile()) { instruction = "下一步 · 打开 G 代码或 STEP ZIP"; next = openButton; }
        else if (canStart()) { instruction = "下一步 · 开始作业"; next = startButton; }
        else if (canReprepare()) { instruction = "下一步 · 重新准备仿真作业"; next = startButton; }
        else if (modeAuto->isEnabled()) { instruction = "下一步 · 切到自动运行"; next = modeAuto; }
        else if (fresh && state == "2" && session.value("outcome") != "none" && !complete)
            instruction = "已停止 · STEP 程序停止后停在当前行，直接再启动会被拒绝：请在柜内重新载入程序回到首行，再回本界面操作。";
        if (!lastOperationError.isEmpty()) instruction = "需处理 · " + lastOperationError;
        stateLabel->setText(instruction);
        setVisualProperty(stateLabel, "tone", tone);
        setVisualProperty(progress, "tone", tone);
        for (auto* button : {linkButton, openButton, startButton, modeAuto})
            setVisualProperty(button, "primary", button == next && button->isEnabled());
        const auto row = job.nativeInput ? current : fresh ? current ? current : done : done;
        sourcePosition->setText(QString("L%1 / %2").arg(row ? QString::number(row) : "—").arg(job.source.size()));
        if (job.nativeInput && fresh && !job.sourceFile.endsWith(".srd") && !job.localEdits) {
            const bool match = (session.value("project") == job.project || session.value("project") + ".sr" == job.project) &&
                session.value("program") == job.sourceProgram;
            sourceHint->setText(!match ? "柜内当前程序与左侧不一致，未高亮。请选择对应程序。" :
                !current && (state == "0" || state == "1") && job.sourceIsGcode ? "当前 SRP 行没有 G 代码映射，未高亮。可切换 SRP 原文查看。" :
                "高亮来自控制器程序行号回读。实机运行前需核对柜内程序版本。");
        }
        runPosition->setText(job.nativeInput && fresh ? QString("STEP L%1 · %2").arg(session.value("line"),
            current ? QString("左侧 L%1 / %2").arg(current).arg(job.source.size()) : "源行待回读") :
            job.source.isEmpty() ? (fresh ? "STEP 行 " + session.value("line") : "等待作业行号") :
                             QString("第 %1 / %2 行").arg(row ? QString::number(row) : "—").arg(job.source.size()));
        const bool unknownProgress = job.nativeInput && job.points.isEmpty() && fresh && state == "0";
        progress->setRange(0, unknownProgress ? 0 : 1000);
        progress->setValue(job.nativeInput ? (complete ? 1000 : donePoint * 1000 / std::max(1, static_cast<int>(job.points.size()) - 1)) :
            std::max(0, done - job.start + 1) * 1000 / std::max(1, static_cast<int>(job.source.size()) - job.start + 1));
        QString remaining = complete ? "00:00" : "待估算";
        if (!job.nativeInput && fresh && current && rate > 0 && !complete) {
            double estimate = 0;
            for (const auto& step : job.steps) {
                if (step.row < current) continue;
                const double stepTime = step.motionSeconds * 100 / rate + step.dwellSeconds;
                estimate += step.row == current ? std::max(0., stepTime - (seconds - rowEntered)) : stepTime;
            }
            remaining = "≈" + duration(estimate);
        }
        runTime->setText("已用 " + duration(seconds) + "  |  剩余 " + remaining);
        // 版本一致性核对：界面作业与柜内当前载入的工程/程序按名称比对。读不到柜内名称时如实标“待核对”，不猜。
        QString checkText = "程序核对 · 待核对";
        QString checkTone = "idle";
        if (!fresh) checkText = "程序核对 · 待核对（等待柜子回读）";
        else if (job.project.isEmpty()) checkText = "程序核对 · 未打开作业";
        else if (session.value("project").isEmpty() && session.value("program").isEmpty())
            checkText = "程序核对 · 待核对：柜内未报告程序名，启动前请在柜内载入作业程序。";
        else {
            const bool projectMatch = session.value("project") == job.project || session.value("project") + ".sr" == job.project;
            const bool programMatch = session.value("program") == job.sourceProgram;
            if (projectMatch && programMatch) checkText = "程序核对 · 一致（" + session.value("project") + " / " + session.value("program") + "）";
            else {
                checkText = "程序核对 · 不一致。柜内是 " + session.value("project") + " / " + session.value("program") +
                    "，作业要求 " + job.project + " / " + job.sourceProgram + "。点「开始」时将自动卸载柜内旧程序并加载作业程序（工程须已存入柜内）。";
                checkTone = "warn";
            }
        }
        versionCheck->setText(checkText);
        setVisualProperty(versionCheck, "tone", checkTone);
        cabinetInfo->setText(fresh ? QString("模式 %1 · 伺服 %2 · TCP %3 mm/s\n消息 %4 · %5")
            .arg(mode == "2" ? "自动" : mode == "3" ? "外部自动" : "手动", session.value("enabled") == "1" ? "已使能" : "未使能",
                 session.value("speed"), session.value("alarm"), session.simulated ? "STEP 隔离虚拟控制器" : session.address + " / " + session.identity) : "控制器回读待确认");
        // 速度三对照：设定（作业上限 25 mm/s / 回程规划 / 点动）、按倍率折算的应用值、控制器回读的实测 TCP 速度。
        double settingValue = 0;
        QString settingLabel, settingUnit = "mm/s";
        if (returning) {
            settingValue = session.value("home_plan_speed").toDouble();
            settingLabel = "回程规划";
        } else if (manual && session.value("jogset").toDouble() > 0) {
            settingValue = session.value("jogset").toDouble();
            settingUnit = session.value("jogunit") == "mm_s" ? "mm/s" : "°/s";
            settingLabel = "点动";
        } else if (fresh && loaded() && !job.project.isEmpty()) {
            settingValue = 25;
            settingLabel = "作业限";
        }
        QString speedLine;
        if (!fresh) speedLine = "速度 · 实测 —";
        else if (settingValue > 0) {
            const double applied = settingValue * rate / 100.;
            speedLine = QString("速度 · 设定 %1 %2（%3） · 应用 %4 %5 · 实测 %6 mm/s · 倍率 %7%")
                .arg(settingValue, 0, 'f', 1).arg(settingUnit, settingLabel)
                .arg(applied, 0, 'f', 1).arg(settingUnit)
                .arg(session.value("speed")).arg(rate, 0, 'f', 0);
        } else
            speedLine = QString("速度 · 实测 %1 mm/s · 倍率 %2%").arg(session.value("speed")).arg(rate, 0, 'f', 0);
        telemetry->setText(fresh ? speedLine + "\nWORLD " + session.value("xyz") + " / " + session.value("abc") +
            "\n关节 " + session.value("axes") : "实时遥测 · 等待连接");
        footer->setText(connection + "   |   " + runText + "   |   行 " + (row ? QString::number(row) : fresh ? session.value("line") : "待回读") +
                        "   |   激光未接入   |   空跑模式");
        setVisualProperty(footer, "tone", tone);
        if (idle && csv.isOpen()) csv.close();
        csvButton->setEnabled(csv.isOpen() || fresh);
        csvButton->setText(csv.isOpen() ? "结束另存" : "另存遥测 CSV");
        experimentLabel->setEnabled(!experiment.active());
        openRecord->setEnabled(!experiment.folder().isEmpty());
        setVisualProperty(experimentStatus, "tone", experiment.active() ? fresh ? "busy" : "error" :
                          experiment.result() == "feedback_lost" || !experiment.healthy() ? "error" : "idle");
        if (!experiment.folder().isEmpty()) {
            static const QMap<QString, QString> results{{"completed", "已完成"}, {"stopped", "已停止"}, {"ended_unverified", "结束结果待核对"},
                {"feedback_lost", "连接中断"}, {"session_closed", "会话结束"}, {"start_cancelled", "启动已取消"},
                {"start_rejected", "启动被拒绝"}, {"start_unconfirmed", "启动未确认"}, {"start_not_sent", "启动未发送"}};
            const auto status = experiment.active() ? (fresh ? "记录中" : "记录中 · 回读待确认") : results.value(experiment.result(), "已封存");
            QString text = QString("%1 · %2 帧").arg(status).arg(experiment.samples());
            if (experiment.compared()) text += QString("\n路径距离 RMS %1 / 峰值 %2 mm")
                .arg(experiment.rmsDistance(), 0, 'f', 3).arg(experiment.maxDistance(), 0, 'f', 3);
            if (!experiment.healthy()) text += "\n记录不完整";
            experimentStatus->setText(text);
        }
    }

    QString directory, workspace, recordRoot;
    QString lastOperationError;
    bool hadFeedback = false;
    Job nativeTemplate, job;
    Session session;
    QElapsedTimer elapsed;
    QFile csv;
    Experiment experiment;
    QString startFailure;
    bool confirmedStop = false, captureGap = false, reportedWriteError = false;
    bool homeDirty = false, sawSource = false;
    QString homeDeviceKey, homePlanPath;
    std::optional<WorkstationOrigin> workstationOrigin;
    bool homePlanPending = false;
    qulonglong homePlanReviewedId = 0;
    int streamedJogSamples = 0;
    int current = 0, done = 0, donePoint = 0, activePoint = 0;
    qint64 lastRun = -1;
    double seconds = 0, rowEntered = 0;
    std::optional<Point> origin;
    QSet<int> skippedRows, executedRows;
    QAction *sampleAction = nullptr, *exportAction = nullptr, *associateAction = nullptr, *saveSourceAction = nullptr;
    QPushButton *openButton = nullptr, *linkButton = nullptr, *startButton = nullptr, *stopButton = nullptr,
                *modeManual = nullptr, *modeAuto = nullptr, *csvButton = nullptr, *openRecord = nullptr;
    QLabel *fileLabel = nullptr, *linkLabel = nullptr, *machine = nullptr, *sourcePosition = nullptr, *summaryLabel = nullptr,
           *telemetry = nullptr, *stateLabel = nullptr, *runPosition = nullptr, *runTime = nullptr, *cabinetInfo = nullptr, *footer = nullptr,
           *versionCheck = nullptr;
    QComboBox* linkType = nullptr;
    QComboBox* jogFrame = nullptr;
    PlcBridgeClient modbus;
    PlcBridgePanel* plcPanel = nullptr;
    ManualBridgePanel* plcManual = nullptr;
    int endpointType = 0;
    bool modbusLinked = false;
    QGroupBox* modbusGroup = nullptr;
    QSpinBox* programNumber = nullptr;
    QPushButton *loadProgramButton = nullptr, *modbusPause = nullptr, *modbusReset = nullptr;
    QLabel* modbusReadoutLabel = nullptr;
    QGroupBox* sourceGroup = nullptr;
    QGroupBox* runSidebar = nullptr;
    PlcLiveMonitor* plcMonitor = nullptr;
    TwinPanel* twinPanel = nullptr;
    TwinView* twinScene = nullptr;
    std::array<QWidget*,4> poseHosts{};
    std::array<QVBoxLayout*,4> poseLayouts{};
    std::array<QWidget*,3> monitorHosts{};
    QWidget* monitorDock = nullptr;
    QScrollArea* monitorScroll = nullptr;
    WelderPanel* welderPanel=nullptr;
    PrintPanel* printPanel=nullptr;
    QSplitter* workspaceColumns = nullptr;
    bool monitorRequested = true;
    QTabWidget* workTabs = nullptr;
    QPushButton* quickStop = nullptr;
    bool changingLayout = false;
    QTreeWidget* sourceTree = nullptr;
    QPushButton *editFileButton = nullptr, *saveProjectButton = nullptr, *gcodeDeliveryButton = nullptr;
    QLabel* sourceHint = nullptr;
    QLineEdit* userRefName = nullptr;
    QDoubleSpinBox *linearJogSpeed = nullptr, *angularJogSpeed = nullptr;
    QLabel* manualFeedback = nullptr;
    QPushButton *recordHomeButton = nullptr, *returnHomeButton = nullptr, *cabinetLoadButton = nullptr;
    QDoubleSpinBox* returnHomeSpeed = nullptr;
    QLabel* homeInfo = nullptr;
    QLineEdit* ip = nullptr;
    QLineEdit* experimentLabel = nullptr;
    QLabel* experimentStatus = nullptr;
    QTableWidget* sourceTable = nullptr;
    QProgressBar* progress = nullptr;
    QPlainTextEdit* logs = nullptr;
    Plot* plot = nullptr;
    QList<QPushButton*> manualButtons;
    QList<QLabel*> axisLabels;
    QList<QLabel*> manualValues, jointReadouts;
    QComboBox* moveType = nullptr;
    QSpinBox* moveDyn = nullptr;
    QDoubleSpinBox* moveOvl = nullptr;
    QList<QDoubleSpinBox*> moveFields, moveHelp;
    QList<QLabel*> moveFieldLabels;
    QWidget* moveHelpRow = nullptr;
    QPushButton *moveFetchButton = nullptr, *moveSendButton = nullptr;
    QLabel* moveFeedback = nullptr;
    bool moveBusy = false;
    qlonglong moveSeqSeen = -1;
    unsigned moveAttempt = 0;
};

int main(int argc, char** argv) {
    // The shared pose panel is reparented across pages; retain its GL resources.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    QTemporaryDir isolatedSettings;
    if (app.arguments().contains("--trajectory-ui-smoke") || app.arguments().contains("--workflow-ui-smoke") || app.arguments().contains("--twin-smoke") || app.arguments().contains("--plc-manual-ui-smoke") || app.arguments().contains("--plc-bridge-smoke") || app.arguments().contains("--file-upload-smoke") || app.arguments().contains("--gcode-delivery-smoke") || app.arguments().contains("--auto-job-ui-smoke")) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolatedSettings.path());
    }
    app.setFont(QFont("Microsoft YaHei UI", 11));
    app.setStyleSheet(WorkstationUi::styleSheet());
    const auto args = app.arguments();
    const auto explicitDir = args.indexOf("--control-dir");
    const QString dir = explicitDir >= 0 ? args.value(explicitDir + 1) : QDir(app.applicationDirPath()).absoluteFilePath("..");
    Workstation workstation(QDir(dir).absolutePath());
    const auto startupFile = args.indexOf("--open");
    if (startupFile >= 0) workstation.openPath(args.value(startupFile + 1));
    if(args.contains("--ui-small"))workstation.resize(1120,740);
    if(args.contains("--ui-wide"))workstation.resize(1920,1040);
    workstation.show();
    const auto twinSmoke=args.indexOf("--twin-smoke");
    const auto workflowSmoke=args.indexOf("--workflow-ui-smoke");
    if(workflowSmoke>=0)QTimer::singleShot(0,&workstation,[&]{workstation.runWorkflowSmoke(args.value(workflowSmoke+1));});
    if(twinSmoke>=0)QTimer::singleShot(0,&workstation,[&]{workstation.runTwinSmoke(args.value(twinSmoke+1));});
    const auto manualUiSmoke=args.indexOf("--plc-manual-ui-smoke");
    if(manualUiSmoke>=0)QTimer::singleShot(0,&workstation,[&]{workstation.runPlcManualUiSmoke(args.value(manualUiSmoke+1));});
    const auto bridgeSmoke = args.indexOf("--plc-bridge-smoke");
    if (bridgeSmoke >= 0) QTimer::singleShot(0, &workstation, [&] {
        workstation.runPlcBridgeSmoke(args.value(bridgeSmoke + 1));
    });
    const auto uploadSmoke = args.indexOf("--file-upload-smoke");
    if (uploadSmoke >= 0) QTimer::singleShot(0,&workstation,[&] {
        workstation.runFileUploadSmoke(args.value(uploadSmoke + 1));
    });
    const auto deliverySmoke = args.indexOf("--gcode-delivery-smoke");
    if (deliverySmoke >= 0) QTimer::singleShot(0, &workstation, [&] {
        workstation.runGcodeDeliverySmoke(args.value(deliverySmoke + 1));
    });
    const auto autoSmoke = args.indexOf("--auto-job-ui-smoke");
    if(autoSmoke>=0)QTimer::singleShot(0,&workstation,[&]{workstation.runGcodeDeliverySmoke(args.value(autoSmoke+1),true);});
    const auto trajectorySmoke=args.indexOf("--trajectory-ui-smoke");
    if(trajectorySmoke>=0)QTimer::singleShot(0,&workstation,[&]{workstation.runTrajectorySmoke(args.value(trajectorySmoke+1),args.value(trajectorySmoke+2));});
    if (args.contains("--home-smoke")) QTimer::singleShot(0, &workstation, [&] { workstation.runHomeSmoke(); });
    if (args.contains("--smoke") || args.contains("--sim-smoke") || args.contains("--manual-smoke") || args.contains("--source-smoke"))
        QTimer::singleShot(0, &workstation, [&] { workstation.runSmoke(!args.contains("--smoke"), args.contains("--manual-smoke"), args.contains("--source-smoke")); });
    return app.exec();
}
