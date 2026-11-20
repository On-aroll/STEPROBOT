#pragma once
#include "workstation_plcbridgepanel.h"
#include "workstation_welder.h"
#include "workstation_ui.h"

// One set of widgets and clients serves the standalone and print pages.
// Mounting or resizing a view never issues a device command.
class PrintPanel : public QWidget {
public:
    PrintPanel(PlcBridgePanel& robot,WelderPanel& welder,QWidget* parent=nullptr)
        :QWidget(parent),robot_(robot),welder_(welder){
        setObjectName("printPanel");auto* page=WorkstationUi::page(this,"printScroll");
        page->parentWidget()->setMaximumWidth(1880);page->setSpacing(6);
        page->parentWidget()->parentWidget()->layout()->setContentsMargins(14,6,14,6);
        setStyleSheet("QLabel#plcNextAction {padding:6px 9px;font-size:10pt;} QLabel#welderState {padding:6px 9px;font-size:9pt;}");
        summary_=new QVBoxLayout;summary_->setSpacing(5);page->addLayout(summary_);
        preparation_=new QVBoxLayout;preparation_->setSpacing(0);page->addLayout(preparation_);
        processHint_=WorkstationUi::hint("起弧 / 收弧：手动");processHint_->setObjectName("printProcessHint");processHint_->setToolTip("按轨迹自动联动尚未接入。");processHint_->hide();
        auto* body=new QHBoxLayout;body->setSpacing(10);page->addLayout(body);
        auto* actions=new QWidget;actionColumn_=actions;actions->setObjectName("printActionColumn");auto* actionLayout=new QVBoxLayout(actions);actionLayout_=actionLayout;actionLayout->setContentsMargins(0,0,0,0);actionLayout->setSpacing(4);body->addWidget(actions,1,Qt::AlignTop);
        robotHost_=new QWidget;robotHost_->setObjectName("printRobotHost");robotLayout_=new QVBoxLayout(robotHost_);robotLayout_->setContentsMargins(0,0,0,0);actionLayout->addWidget(robotHost_);
        welderHost_=new QWidget;welderHost_->setObjectName("printWelderHost");welderLayout_=new QVBoxLayout(welderHost_);welderLayout_->setContentsMargins(0,0,0,0);actionLayout->addWidget(welderHost_);
        poseHost_=new QWidget;poseHost_->setObjectName("printPoseHost");auto* pose=new QVBoxLayout(poseHost_);pose->setContentsMargins(0,12,0,0);body->addWidget(poseHost_,2,Qt::AlignTop);
        parameterColumn_=new QWidget;parameterColumn_->setObjectName("printParameterColumn");parameters_=new QVBoxLayout(parameterColumn_);parameters_->setContentsMargins(0,12,0,0);parameters_->setSpacing(6);body->addWidget(parameterColumn_,1,Qt::AlignTop);
        auto* monitor=WorkstationUi::disclosure(page,"设备监视与日志","printMonitor");monitorHost_=new QWidget;monitorHost_->setObjectName("printMonitorHost");logLayout_=new QVBoxLayout(monitor);logLayout_->setContentsMargins(0,0,0,0);logLayout_->addWidget(monitorHost_);auto* hostLayout=new QVBoxLayout(monitorHost_);hostLayout->setContentsMargins(0,0,0,0);
        page->addStretch();
    }
    QWidget* poseHost() const {return poseHost_;}
    QWidget* monitorHost() const {return monitorHost_;}
    int refreshBodyHeight(){
        auto* scroll=findChild<QScrollArea*>("printScroll");
        auto* toggle=findChild<QToolButton*>("printMonitorToggle");
        const int top=poseHost_->mapTo(scroll->widget(),QPoint()).y();
        const int height=qMax(342,scroll->viewport()->height()-top-toggle->sizeHint().height()-18);
        if(active_&&(height!=bodyHeight_||actionColumn_->width()!=actionWidth_)){
            bodyHeight_=height;actionWidth_=actionColumn_->width();
            robot_.setPrintRowHeight(28);welder_.setPrintRowHeight(28);
            actionLayout_->invalidate();
            const int natural=actionLayout_->minimumSize().height();
            const int rowHeight=28+qBound(0,(height-natural)/10,28);
            robot_.setPrintRowHeight(rowHeight);welder_.setPrintRowHeight(rowHeight);
            welder_.setPrintParameterHeight(height-12);
        }
        return height;
    }
    void setActive(bool on){
        if(active_==on)return;active_=on;bodyHeight_=actionWidth_=0;
        robot_.setPrintHosts(on?summary_:nullptr,on?robotLayout_:nullptr,on?preparation_:nullptr);
        if(on){auto* bar=findChild<QWidget*>("printPreparationBar");qobject_cast<QHBoxLayout*>(bar->layout())->insertWidget(6,processHint_);processHint_->show();}
        welder_.setPrintHosts(on?welderLayout_:nullptr,on?parameters_:nullptr,on?logLayout_:nullptr);
    }
private:
    PlcBridgePanel& robot_;
    WelderPanel& welder_;
    QWidget *robotHost_,*poseHost_,*welderHost_,*monitorHost_,*parameterColumn_,*actionColumn_;
    QVBoxLayout *summary_,*preparation_,*robotLayout_,*welderLayout_,*parameters_,*logLayout_,*actionLayout_;
    int bodyHeight_=0,actionWidth_=0;
    bool active_=false;
    QLabel* processHint_=nullptr;
};
