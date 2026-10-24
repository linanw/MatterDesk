#pragma once
#include <QString>
inline QString homeStyle() { return QStringLiteral(R"(
QWidget { color:#34343a; font-family:"Noto Sans CJK SC","Noto Sans",sans-serif; font-size:13px; }
QWidget#root { background:#ebe9ed; }
QWidget#main { background:qlineargradient(x1:0,y1:0,x2:0.7,y2:1, stop:0 #383d5b, stop:0.45 #636181, stop:1 #b28c91); }
QWidget#gridHost, QWidget#scenePage, QWidget#automationPage, QWidget#activityPage, QStackedWidget#homePages, QScrollArea, QScrollArea>QWidget>QWidget, QWidget#sceneStrip, QWidget#roomNav { background:transparent; border:0; }
QLabel { background:transparent; }
QFrame#sidebar { background:#eeedf1; border:0; }
QLabel#brand { color:#303039; font-size:24px; font-weight:700; }
QLabel#sideEyebrow { color:#8b8791; font-size:11px; padding:8px 10px 4px 10px; }
QLabel#sidebarCaption { color:#98919c; font-size:10px; padding:10px 4px 0 4px; }
QPushButton { background:#eeedf0; border:1px solid transparent; border-radius:9px; padding:8px 13px; font-weight:500; }
QPushButton:hover { background:#e0dfe5; }
QPushButton:disabled { color:#aaa6b0; background:#e8e6eb; border-color:transparent; }
QPushButton#navigation { text-align:left; color:#62616d; background:transparent; padding:8px 12px; }
QPushButton#navigation:hover, QPushButton#sideAction:hover { background:#e3e1e8; }
QPushButton#navigation:checked { background:#dad7e0; color:#34313f; font-weight:600; }
QPushButton#sideAction { text-align:left; color:#74707f; background:transparent; padding:7px 10px; font-size:12px; }
QLabel#hero { font-size:34px; font-weight:700; color:white; }
QLabel#homeSummary { color:#e0dbe8; font-size:12px; }
QLabel#sectionTitle { color:white; font-size:21px; font-weight:600; }
QLabel#roomHeading { color:#fff; font-size:16px; font-weight:600; padding:8px 0 2px 2px; }
QLabel#homeSmall { color:#e3dde9; font-size:11px; }
QLabel#muted { color:#807887; font-size:12px; }
QLabel#mutedSmall { color:#918997; font-size:10px; }
QPushButton#primary { color:white; background:#df8d40; padding:10px 17px; }
QPushButton#primary:hover { background:#c77a32; }
QPushButton#secondary { color:#625b6c; background:#f4f1f5; padding:10px 15px; }
QPushButton#secondary:hover { background:#e5dfe9; }
QPushButton#quiet { background:transparent; color:#aa727b; }
QPushButton#categoryChip { color:#faf4ff; background:rgba(237,225,248,28); text-align:left; border-radius:13px; padding:10px 16px; }
QPushButton#categoryChip:hover { background:rgba(237,225,248,58); }
QPushButton#quickScene { text-align:left; color:#594960; background:#f0e7ed; border-radius:14px; padding:13px 18px; font-size:14px; }
QPushButton#quickScene:hover { background:#fff2d4; }
QFrame#connectionPanel { background:#f0edf3; border-radius:13px; }
QPushButton#connectButton { color:#75617e; background:white; }
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { border:1px solid #dfdbe3; border-radius:8px; padding:8px 10px; background:white; selection-background-color:#d2c7df; color:#514859; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color:#a997bb; }
QLineEdit#serverUrl { font-family:"Noto Sans Mono",monospace; font-size:12px; }
QComboBox { min-width:75px; }
QComboBox::drop-down { border:0; width:22px; }
QFrame#deviceCard { background:#f3f1f4; border:1px solid #f2edf1; border-radius:16px; }
QFrame#deviceCard:hover { background:#faf7fa; border-color:#f5d29b; }
QLabel#deviceName { color:#3f3748; font-size:14px; font-weight:600; }
QLabel#deviceStatus { font-size:10px; }
QLabel#reading { color:#62546c; font-size:16px; font-weight:500; }
QPushButton#more { background:transparent; color:#aaa0b2; padding:0; font-size:20px; }
QPushButton#favorite { background:transparent; color:#c69654; padding:0; font-size:21px; }
QPushButton#favorite:checked { color:#cf8b30; background:#fff3dc; }
QPushButton#accessoryControls { background:#e7e1ec; color:#695474; border-radius:8px; padding:7px 12px; }
QPushButton#power { border-radius:8px; font-size:12px; }
QFrame#empty { background:#ede8ef; border:1px solid #d8cfdf; border-radius:16px; }
QLabel#emptyTitle { font-size:18px; font-weight:600; color:#63546d; padding:8px; }
QLabel#demoBanner { background:#f8e6c9; color:#927143; border-radius:8px; padding:7px 12px; font-size:11px; }
QFrame#collectionCard { background:#f6f2f6; border:0; border-radius:14px; }
QLabel#collectionTitle { color:#51435d; font-size:16px; font-weight:600; }
QFrame#accessorySection { background:white; border:1px solid #e5dfe9; border-radius:12px; }
QLabel#controlTitle { color:#6b557a; font-weight:600; font-size:15px; }
QLabel#controlFeedback { color:#927044; }
QDialog { background:#f3f0f5; }
QDialog QLabel#hero { color:#51435d; font-size:26px; }
QDialog QLabel#sectionTitle { color:#6b557a; font-size:18px; }
QTabWidget::pane { border:1px solid #e1dae7; border-radius:9px; background:#f7f4f9; }
QTabBar::tab { background:#e8e1ed; padding:10px 16px; }
QTabBar::tab:selected { background:#f8f5fa; }
QListWidget { background:white; border:1px solid #dfd6e7; border-radius:8px; padding:5px; }
QPlainTextEdit { background:#fff; border:1px solid #ddd5e4; border-radius:8px; padding:8px; }
QPlainTextEdit#logs { color:#dfd5e6; background:#393144; font-family:monospace; font-size:11px; }
QSlider::groove:horizontal { border:0; height:6px; border-radius:3px; background:#e5deea; }
QSlider::sub-page:horizontal { background:#b3a0c4; border-radius:3px; }
QSlider::handle:horizontal { background:white; border:2px solid #9a82b0; width:14px; height:14px; margin:-5px 0; border-radius:8px; }
QSlider::handle:horizontal:disabled { border-color:#d3ccd9; }
QScrollBar:vertical { background:transparent; width:7px; margin:0; }
QScrollBar::handle:vertical { background:#b6aabb; border-radius:3px; min-height:26px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }
QToolTip { color:#56415e; background:#faf6fc; border:1px solid #cfc0db; padding:6px; }
)"); }
