#pragma once
#include "workstation_plcbridge.h"
#include "workstation_ui.h"
#include <functional>

// Read-only device overview. No requests or separate robot connections are issued here.
class PlcLiveMonitor : public QWidget {
public:
    explicit PlcLiveMonitor(PlcBridgeClient& value,QWidget* parent=nullptr):QWidget(parent),client(value){
        setObjectName("plcLiveMonitor");
        layout=new QGridLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(10);
        auto* stateBox=new QGroupBox("设备监视");auto* stateLayout=new QVBoxLayout(stateBox);stateLayout->setSpacing(8);
        state=WorkstationUi::badge("liveRobotState");state->setMinimumHeight(36);stateLayout->addWidget(state);
        auto* statusGrid=new QGridLayout;statusGrid->setVerticalSpacing(8);
        const QStringList names{"PLC 心跳","外部自动","远程许可","停止锁存","程序确认"};
        for(int i=0;i<names.size();++i){auto* key=new QLabel(names[i]);key->setProperty("field",true);statusGrid->addWidget(key,i/2,(i%2)*2);auto* valueLabel=new QLabel("—");valueLabel->setAlignment(Qt::AlignRight);statusGrid->addWidget(valueLabel,i/2,(i%2)*2+1);fields<<valueLabel;}
        stateLayout->addLayout(statusGrid);cards<<stateBox;
        auto* positionBox=new QGroupBox("TCP 坐标监视");auto* positionLayout=new QVBoxLayout(positionBox);
        auto* coordinates=new QGridLayout;coordinates->setVerticalSpacing(8);coordinates->setHorizontalSpacing(10);
        const QStringList axes{"X","Y","Z","A","B","C"};
        for(int i=0;i<6;++i){auto* name=new QLabel(axes[i]);name->setProperty("field",true);auto* valueLabel=new QLabel("—");valueLabel->setStyleSheet("font:600 11pt Consolas;");valueLabel->setAlignment(Qt::AlignRight);valueLabel->setMinimumWidth(0);valueLabel->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);coordinates->addWidget(name,i%3,(i/3)*2);coordinates->addWidget(valueLabel,i%3,(i/3)*2+1);values<<valueLabel;}
        coordinates->setColumnStretch(1,1);coordinates->setColumnStretch(3,1);
        positionLayout->addLayout(coordinates);
        positionLayout->addWidget(WorkstationUi::hint("XYZ：mm　ABC：°\n需请求 XYZABC 输出，坐标仅供监视。"));cards<<positionBox;
        auto* eventBox=new QGroupBox("回执与操作记录");auto* eventLayout=new QVBoxLayout(eventBox);
        request=new QLabel;request->setWordWrap(true);request->setObjectName("liveCurrentRequest");eventLayout->addWidget(request);
        receipt=WorkstationUi::hint();receipt->setObjectName("liveReceipt");eventLayout->addWidget(receipt);
        events=new QPlainTextEdit;events->setObjectName("liveEventLog");events->setReadOnly(true);events->setMaximumBlockCount(500);events->setMinimumHeight(110);
        events->setPlaceholderText("连接、登记、加载和微调的过程会显示在这里。\n尚未连接设备。");eventLayout->addWidget(events,1);
        auto* buttons=new QHBoxLayout;auto* diagnostics=new QPushButton("I/O 与详细诊断");auto* save=new QPushButton("保存日志…");buttons->addWidget(diagnostics,1);buttons->addWidget(save);eventLayout->addLayout(buttons);cards<<eventBox;
        setCompact(false);
        connect(diagnostics,&QPushButton::clicked,this,[this]{if(openDiagnostics)openDiagnostics();});
        connect(save,&QPushButton::clicked,this,[this]{
            const auto path=QFileDialog::getSaveFileName(this,"保存上位机操作记录","workstation_events.txt","文本 (*.txt)");if(path.isEmpty())return;
            QSaveFile file(path);const auto data=events->toPlainText().toUtf8();
            if(!file.open(QIODevice::WriteOnly)||file.write(data)!=data.size()||!file.commit())QMessageBox::warning(this,"保存日志","日志保存失败，请检查目标目录。");
        });
        connect(&client,&PlcBridgeClient::changed,this,[this]{refresh();});refresh();
    }
    std::function<void()> openDiagnostics;
    void setCompact(bool compact){
        if(compactMode==int(compact))return;compactMode=int(compact);
        for(auto* card:cards)layout->removeWidget(card);
        for(int i=0;i<4;++i){layout->setColumnStretch(i,0);layout->setRowStretch(i,0);}
        for(int i=0;i<cards.size();++i){layout->addWidget(cards[i],compact?0:i,compact?i:0);layout->setColumnStretch(i,compact||i==0?1:0);}
        events->setMaximumHeight(110);
        setMinimumWidth(compact?0:310);setMaximumWidth(QWIDGETSIZE_MAX);
    }
    void append(const QString& message){events->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss  ")+message);}
private:
    void refresh(){
        using namespace BridgeProto;const auto s=client.status();const bool live=client.fresh(),robot=live&&s.has(RobotOnline);
        WorkstationUi::badgeState(state,!robot?"机器人状态 · 未知":robotStateText(s)+(s.enabled()?" · 已使能":" · 未使能"),!robot?"idle":s.alarm()||s.emergency()?"error":s.running()?"busy":"success");
        const QStringList text{live?QString("%1 · 有效").arg(s.plcHeartbeat):"未验证",!robot?"—":s.externalAuto()?"已就绪":"未就绪",!live?"—":s.has(RemotePermit)?"已放行":"未放行",!live?"—":s.has(StopLatch)?"待复位":"未锁存",!robot?"—":s.has(LoadConfirmed)?QString("%1 号").arg(s.selectedSlot):"未确认"};
        for(int i=0;i<fields.size();++i)fields[i]->setText(text[i]);
        for(int i=0;i<values.size();++i){const auto value=live?s.coordinate(i):std::optional<float>{};values[i]->setText(value?QString::number(*value,'f',3):"—");values[i]->setToolTip(values[i]->text());}
        request->setText(!live?"等待有效 PLC 状态":client.pending()?QString("等待 %1 回执\n请求 %2 · %3 ms").arg(commandText(client.currentCommand())).arg(client.pendingSequence()).arg(client.commandAgeMs()):"当前无在途请求");
        WorkstationUi::tone(request,client.pending()?"warn":"idle");
        receipt->setText(!live?"PLC 应用回执将在连接后显示。":QString("回执 %1 · %2\n%3 · %4").arg(s.ackSeq).arg(commandText(Command(s.ackCommand))).arg(resultText(s.result)).arg(errorText(s.error)));
    }
    PlcBridgeClient& client;
    QLabel *state,*request,*receipt;
    QList<QLabel*> fields,values;
    QPlainTextEdit* events;
    QGridLayout* layout;
    QList<QWidget*> cards;
    int compactMode=-1;
};
