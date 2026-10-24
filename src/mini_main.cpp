#include "miniwindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QTimer>
#include <QSettings>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    app.setApplicationName("MatterCamViewer"); app.setOrganizationName("MatterDesk"); app.setApplicationVersion(MATTERCAMVIEWER_VERSION);
    app.setApplicationDisplayName("MatterCamViewer");
    app.setDesktopFileName("mattercamviewer");
    app.setStyle("Fusion"); app.setFont(QFont("Noto Sans CJK SC", 10));
    const QString icon = QDir(QCoreApplication::applicationDirPath()).filePath("../packaging/mattercamviewer.svg");
    if (QFileInfo::exists(icon)) app.setWindowIcon(QIcon(icon));
    else app.setWindowIcon(QIcon::fromTheme(QStringLiteral("mattercamviewer")));
    app.setStyleSheet(R"(
        QWidget { color:#26332b; font-family:"Noto Sans CJK SC","Noto Sans",sans-serif; font-size:13px; }
        QWidget#miniRoot, QDialog { background:#f7f8f5; }
        QLabel { background:transparent; }
        QLabel#miniTitle { font-size:19px; font-weight:600; }
        QLabel#miniStatus, QLabel#miniTime { color:#858b82; font-size:11px; }
        QLabel#miniHint { color:#848b80; font-size:12px; }
        QPushButton { background:#e9eee6; color:#365441; border:0; border-radius:8px; padding:8px 13px; }
        QPushButton:hover { background:#dde7d9; }
        QPushButton:disabled { color:#a4aaa0; background:#eef0ea; }
        QPushButton#primary { background:#327452; color:white; }
        QPushButton#primary:disabled { background:#e5e9df; color:#a1a99b; }
        QLineEdit, QComboBox { background:white; border:1px solid #dfe5d9; border-radius:7px; padding:8px 10px; selection-background-color:#dce8d5; }
        QLineEdit:focus, QComboBox:focus { border-color:#7ba585; }
        QComboBox::drop-down { border:0; width:20px; }
        QLabel#muted, QLabel#mutedSmall { color:#858c80; font-size:11px; }
        QLabel#emptyTitle { font-size:18px; font-weight:600; }
        QToolTip { color:#294231; background:#f8faf5; border:1px solid #cfdcc6; padding:5px; }
    )");
    QCommandLineParser parser;
    parser.setApplicationDescription("极简 Matter 摄像头：自动连接本地控制器，无边框快照与直播，单击显隐面板、双击恢复默认尺寸或全屏、拖动移动。");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"server", "指定控制器（默认连接本机 127.0.0.1:5580）", "url", "ws://127.0.0.1:5580/ws"});
    parser.addOption({"screenshot", "保存界面 PNG（用于验证）", "file"});
    parser.addOption({"screenshot-delay", "截图前等待毫秒数", "milliseconds", "2000"});
    parser.addOption({"quit-after", "指定毫秒后退出（用于验证）", "milliseconds"}); parser.process(app);
    const QUrl url(parser.value("server"));
    if (!url.isValid() || (url.scheme() != "ws" && url.scheme() != "wss") || url.host().isEmpty() || !url.userInfo().isEmpty()) {
        std::fprintf(stderr, "Invalid Matter controller WebSocket URL.\n"); return 2;
    }
    bool screenshotDelayValid = false;
    const int screenshotDelay = parser.value("screenshot-delay").toInt(&screenshotDelayValid);
    if (!screenshotDelayValid || screenshotDelay < 0 || screenshotDelay > 300000) {
        std::fprintf(stderr, "--screenshot-delay must be between 0 and 300000 milliseconds.\n"); return 2;
    }
    // Import only viewer preferences from the former Mini name. Controller and
    // Fabric storage remain owned by the existing local Matter Server.
    QSettings settings;
    const QSettings legacy(QStringLiteral("MatterDesk"), QStringLiteral("MatterDeskMini"));
    for (const auto &key : {QStringLiteral("mini/selectedCameraKey"), QStringLiteral("mini/alwaysOnTop")})
        if (!settings.contains(key) && legacy.contains(key)) settings.setValue(key, legacy.value(key));
    MiniWindow window(url); window.show();
    if (parser.isSet("screenshot")) {
        const QString path = parser.value("screenshot");
        QTimer::singleShot(screenshotDelay, &window, [&window, path] {
            if (!window.grab().save(path)) std::fprintf(stderr, "Cannot save screenshot.\n");
        });
    }
    if (parser.isSet("quit-after")) {
        bool ok = false; const int delay = parser.value("quit-after").toInt(&ok);
        if (!ok || delay < 0) { std::fprintf(stderr, "--quit-after requires a nonnegative integer.\n"); return 2; }
        QTimer::singleShot(delay, &app, &QApplication::quit);
    }
    return app.exec();
}
