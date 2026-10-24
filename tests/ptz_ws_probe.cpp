#include "matterclient.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTextStream>
#include <QTimer>

// Explicitly opt-in, synthetic fixture only. Never run against a home controller.
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() != 3 || arguments.at(1) != "--synthetic-fixture") return 2;
    const QUrl url(arguments.at(2));
    if (url.scheme() != "ws" || url.host() != "127.0.0.1" || url.port() < 1024 || url.port() == 5580
        || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) return 2;
    MatterClient client;
    MatterDevice device;
    QString request;
    bool started = false;
    int direction = 0;
    QJsonArray accepted;
    QTimer next;
    next.setInterval(20);
    QTimer timeout;
    timeout.setSingleShot(true); timeout.start(25000);
    QObject::connect(&timeout, &QTimer::timeout, &app, [&] { app.exit(3); });
    QObject::connect(&client, &MatterClient::connectionChanged, &app, [&] {
        if (started || !client.isConnected()) return;
        for (const auto &candidate : client.devices()) {
            if (!candidate.hasCameraPtz || !candidate.available) continue;
            device = candidate; started = true;
            request = client.readCameraPtz(device);
            return;
        }
    });
    QObject::connect(&next, &QTimer::timeout, &app, [&] {
        if (!request.isEmpty() || !client.canMoveCameraStep(device, direction)) return;
        next.stop(); request = client.moveCameraStep(device, direction);
    });
    QObject::connect(&client, &MatterClient::cameraPtzResponse, &app,
        [&](const QString &id, const QString &operation, bool success, const QString &detail) {
            if (id != request) return;
            request.clear();
            if (!success) { QTextStream(stderr) << detail << '\n'; app.exit(4); return; }
            if (operation == "read") { next.start(); return; }
            accepted.append(direction++);
            if (direction == 4) {
                QTextStream(stdout) << QJsonDocument(QJsonObject{{"synthetic", true}, {"readPathMapping", true},
                    {"acceptedDirections", accepted}, {"physicalMovementVerified", false}}).toJson(QJsonDocument::Compact) << '\n';
                client.disconnectFromServer(); app.exit(0);
            } else next.start();
        });
    client.connectToServer(url);
    return app.exec();
}
