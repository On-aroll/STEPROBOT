#pragma once
#include <QtWidgets>

namespace WorkstationUi {
inline QVBoxLayout* page(QWidget* parent,const QString& scrollName){
    auto* outer=new QVBoxLayout(parent);outer->setContentsMargins(0,0,0,0);
    auto* scroll=new QScrollArea;scroll->setObjectName(scrollName);scroll->setWidgetResizable(true);outer->addWidget(scroll);
    auto* canvas=new QWidget;canvas->setObjectName("workflowContent");scroll->setWidget(canvas);
    auto* center=new QHBoxLayout(canvas);center->setContentsMargins(14,12,14,12);
    auto* content=new QWidget;content->setObjectName("pageContent");content->setMaximumWidth(1260);
    center->addWidget(content,1);
    auto* layout=new QVBoxLayout(content);layout->setContentsMargins(0,0,0,0);layout->setSpacing(12);
    return layout;
}
inline void property(QWidget* widget,const char* key,const QVariant& value){
    if(widget->property(key)==value)return;
    widget->setProperty(key,value);widget->style()->unpolish(widget);widget->style()->polish(widget);widget->update();
}
inline void tone(QWidget* widget,const QString& value){property(widget,"tone",value);}
inline QLabel* badge(const QString& name){
    auto* label=new QLabel;label->setObjectName(name);label->setProperty("chip",true);
    label->setAlignment(Qt::AlignCenter);label->setMinimumHeight(28);return label;
}
inline void badgeState(QLabel* label,const QString& text,const QString& state){label->setText(text);tone(label,state);}
inline QLabel* hint(const QString& text={}){
    auto* label=new QLabel(text);label->setWordWrap(true);label->setProperty("hint",true);return label;
}
inline void button(QPushButton* widget,bool allowed,const QString& reason,const QString& state={}){
    widget->setEnabled(allowed);property(widget,"actionState",state);widget->setToolTip(reason);
    widget->setAccessibleDescription(reason);
}
inline QWidget* disclosure(QVBoxLayout* layout,const QString& title,const QString& name){
    auto* toggle=new QToolButton;toggle->setObjectName(name+"Toggle");toggle->setText(title);
    toggle->setCheckable(true);toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);toggle->setArrowType(Qt::RightArrow);
    toggle->setProperty("disclosure",true);layout->addWidget(toggle);
    auto* content=new QWidget;content->setObjectName(name);content->hide();layout->addWidget(content);
    QObject::connect(toggle,&QToolButton::toggled,content,[toggle,content](bool open){content->setVisible(open);toggle->setArrowType(open?Qt::DownArrow:Qt::RightArrow);});
    return content;
}
inline QString styleSheet(){return QStringLiteral(R"(
    QWidget { color:#24374e; }
    QMainWindow { background:#e9eff6; }
    QWidget#workflowContent, QWidget#pageContent { background:#f5f7fb; }
    QWidget#plcLiveMonitor { background:#f5f7fb; }
    QGroupBox { background:white; border:1px solid #d7e0eb; border-radius:8px; margin-top:12px; padding:13px 10px 9px; font-weight:600; }
    QGroupBox::title { subcontrol-origin:margin; left:12px; padding:0 6px; color:#334c68; }
    QPushButton, QToolButton { background:white; color:#234866; border:1px solid #9baec5; border-radius:5px; padding:7px 11px; font-weight:500; }
    QPushButton:enabled:hover, QToolButton:enabled:hover { background:#eaf2ff; border-color:#2867c7; color:#114fae; }
    QPushButton:enabled:pressed, QToolButton:enabled:pressed { background:#164b9c; border-color:#164b9c; color:white; }
    QPushButton:focus, QToolButton:focus { border:2px solid #2867c7; padding:6px 10px; }
    QPushButton:disabled, QToolButton:disabled { color:#7c8999; background:#edf0f4; border:1px solid #d4dde7; font-weight:400; }
    QPushButton[primary="true"]:enabled, QPushButton[actionState="next"]:enabled { background:#245fbd; color:white; border-color:#245fbd; font-weight:600; }
    QPushButton[primary="true"]:enabled:hover, QPushButton[actionState="next"]:enabled:hover { background:#194c9e; border-color:#194c9e; }
    QPushButton[actionState="done"] { background:#eaf6f0; color:#287553; border-color:#b8dac8; font-weight:600; }
    QPushButton[actionState="done"]:enabled:hover { background:#d4eddf; border-color:#6aaf8b; color:#185937; }
    QPushButton[actionState="busy"] { background:#fff2d4; color:#895707; border-color:#e0bb65; }
    QPushButton[danger="true"]:enabled, QPushButton#stop:enabled { background:#c53a43; color:white; border-color:#c53a43; font-weight:600; }
    QPushButton[danger="true"]:enabled:hover, QPushButton#stop:enabled:hover { background:#a92b34; border-color:#a92b34; }
    QPushButton[motion="true"] { padding:8px 12px; font-size:12pt; }
    QPushButton[motion="true"]:enabled { color:#174d84; border-color:#7599bd; background:#f2f7ff; font-weight:600; }
    QPushButton[motion="true"]:focus { padding:7px 11px; }
    QPushButton[motion="true"]:enabled:hover { background:#dfecff; border-color:#245fbd; }
    QPushButton[motion="true"]:enabled:pressed { background:#245fbd; color:white; }
    QToolButton[disclosure="true"] { border:none; background:transparent; text-align:left; color:#536982; padding:5px 2px; }
    QToolButton:checked { background:#e4edfa; border-color:#7b9dcc; color:#174d84; }
    QLabel[chip="true"] { background:#edf1f6; color:#5d6d81; border:1px solid #d9e1ec; border-radius:5px; padding:4px 8px; font-weight:600; }
    QLabel[chip="true"][tone="success"] { background:#e7f5ed; color:#16633e; border-color:#b5d9c4; }
    QLabel[chip="true"][tone="busy"] { background:#e6efff; color:#1b4fa2; border-color:#b8cef0; }
    QLabel[chip="true"][tone="warn"] { background:#fff3d9; color:#855506; border-color:#e5ce93; }
    QLabel[chip="true"][tone="error"] { background:#fbeaec; color:#a52736; border-color:#e8b5bd; }
    QLabel[hint="true"] { color:#66778d; font-size:10pt; }
    QLabel[heading="true"] { color:#182d49; font-size:16pt; font-weight:600; }
    QLabel[readout="true"] { color:#243f5e; font-family:Consolas; font-size:15pt; font-weight:600; }
    QLabel[field="true"] { color:#63748a; }
    QLabel[cardTitle="true"] { color:#244260; font-size:12pt; font-weight:600; }
    QPlainTextEdit#liveEventLog { font-size:10pt; background:#f9fbfe; border-color:#d9e2ee; padding:5px; }
    QLabel[banner="true"], QLabel#statusBanner { background:#eaf1fc; border:1px solid #c8d8ef; border-radius:6px; padding:10px; color:#214b82; }
    QLabel[banner="true"][tone="success"] { background:#eaf6ef; border-color:#bfdcca; color:#1b6945; }
    QLabel[banner="true"][tone="warn"], QLabel#statusBanner[tone="warn"] { background:#fff5df; border-color:#e7d39d; color:#825308; }
    QLabel[banner="true"][tone="error"], QLabel#statusBanner[tone="error"] { background:#fbeaec; border-color:#e6b3bc; color:#a12838; }
    QLabel[tone="error"] { color:#a62938; }
    QLabel[tone="warn"] { color:#8a5a00; }
    QLabel[tone="success"] { color:#176441; }
    QLabel[tone="busy"] { color:#1758a8; }
    QLabel[tone="idle"] { color:#61718a; }
    QLabel#appTitle { font-size:16pt; font-weight:700; color:#153759; }
    QFrame#connectionBar { background:white; border:1px solid #d7e0eb; border-radius:8px; }
    QTabWidget::pane { background:white; border:1px solid #d7e0eb; border-radius:6px; top:-1px; }
    QTabBar::tab { background:#e5eaf2; color:#54687f; border:1px solid #d2dce8; padding:10px 17px; margin-right:4px; border-top-left-radius:6px; border-top-right-radius:6px; }
    QTabBar::tab:selected { background:white; color:#164e9e; border-top:3px solid #245fbd; padding-top:8px; font-weight:600; }
    QTabBar::tab:hover:!selected { background:#dce7f6; }
    QTabBar::tab:disabled { color:#9ca7b5; }
    QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { background:white; border:1px solid #b6c4d5; border-radius:4px; padding:6px; min-height:19px; selection-background-color:#245fbd; }
    QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color:#245fbd; }
    QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled { background:#edf0f4; color:#8993a3; border-color:#d5dce5; }
    QSpinBox, QDoubleSpinBox { padding-right:26px; }
    QSpinBox::up-button, QDoubleSpinBox::up-button { subcontrol-origin:border; subcontrol-position:top right; width:22px; background:#eff4fa; border-left:1px solid #c9d5e4; border-bottom:1px solid #c9d5e4; border-top-right-radius:4px; }
    QSpinBox::down-button, QDoubleSpinBox::down-button { subcontrol-origin:border; subcontrol-position:bottom right; width:22px; background:#eff4fa; border-left:1px solid #c9d5e4; border-bottom-right-radius:4px; }
    QSpinBox::up-button:hover, QSpinBox::down-button:hover, QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background:#dce9fc; }
    QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image:url(:/ui/chevron-up.svg); width:10px; height:6px; }
    QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image:url(:/ui/chevron-down.svg); width:10px; height:6px; }
    QTableWidget, QTreeWidget, QPlainTextEdit { background:white; border:1px solid #d7e0eb; border-radius:4px; selection-background-color:#e0ecff; selection-color:#163f72; }
    QHeaderView::section { background:#f1f5fa; color:#536a82; border:none; border-bottom:1px solid #d7e0eb; padding:6px; }
    QScrollArea { background:transparent; border:none; }
    QScrollBar:vertical { background:#f0f3f7; width:10px; margin:0; }
    QScrollBar::handle:vertical { background:#b7c4d5; border-radius:4px; min-height:24px; }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
    QSplitter::handle { background:#e1e7f0; width:5px; }
    QProgressBar { background:#e4eaf2; border:none; border-radius:4px; min-height:8px; max-height:8px; }
    QProgressBar::chunk { background:#245fbd; border-radius:4px; }
    QProgressBar[tone="error"]::chunk { background:#bd3542; }
    QCheckBox#dry { color:#175991; background:#e4f1fd; padding:6px; }
    QLabel#footer { background:#17334f; color:#d8e6f5; padding:7px 12px; border-radius:5px; }
    QLabel#footer[tone="busy"] { color:#a7d7ff; }
    QLabel#footer[tone="error"] { color:#ffb6bd; }
    QToolTip { color:#203e5e; background:#f8fbff; border:1px solid #a5bad5; padding:5px; }
)" );}
}
