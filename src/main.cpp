#include "mainwindow.h"
#include "homestyle.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QTimer>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    app.setApplicationName("MatterDesk"); app.setOrganizationName("MatterDesk"); app.setApplicationVersion("0.6.0");
    app.setStyle("Fusion"); app.setFont(QFont("Noto Sans CJK SC", 10));
    const QString icon = QDir(QCoreApplication::applicationDirPath()).filePath("../packaging/matterdesk.svg");
    if (QFileInfo::exists(icon)) app.setWindowIcon(QIcon(icon));
    app.setStyleSheet(homeStyle());
    QCommandLineParser parser; parser.setApplicationDescription("Linux Qt Matter 客户端，通过本地 Matter Server 控制 Homebridge 桥接设备。"); parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"demo", "使用模拟设备，无网络连接"});
    parser.addOption({"server", "连接指定 Matter Server", "url"});
    parser.addOption({"screenshot", "将界面截图保存为 PNG", "file"});
    parser.addOption({"quit-after", "指定毫秒后退出（用于自动验证）", "milliseconds"});
    parser.process(app);
    MainWindow window; window.show();
    if (parser.isSet("demo")) window.startDemo();
    else if (parser.isSet("server")) {
        window.setServerUrl(parser.value("server")); window.client()->connectToServer(QUrl(parser.value("server")));
    }
    if (parser.isSet("screenshot")) {
        const QString path = parser.value("screenshot");
        QTimer::singleShot(1200, &window, [&window, path] { if (!window.grab().save(path)) std::fprintf(stderr, "Cannot save screenshot\n"); });
    }
    if (parser.isSet("quit-after")) {
        bool ok = false; const int delay = parser.value("quit-after").toInt(&ok);
        if (!ok || delay < 0) { std::fprintf(stderr, "--quit-after requires a nonnegative integer\n"); return 2; }
        QTimer::singleShot(delay, &app, &QApplication::quit);
    }
    return app.exec();
}
