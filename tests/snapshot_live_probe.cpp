// Explicit integration probe for a separately commissioned synthetic camera.
// Production controller port 5580 is refused; no image bytes enter its output.
#include "mainwindow.h"
#include "devicecard.h"
#include "camerasnapshots.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QFont>
#include <QJsonDocument>
#include <QPushButton>
#include <QTimer>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    app.setApplicationName("MatterDeskSnapshotProbe"); app.setOrganizationName("MatterDeskTests");
    app.setStyle("Fusion"); app.setFont(QFont("Noto Sans CJK SC", 10));
    // Use the application's own styles when running from a source checkout.
    QFile source(QFileInfo(QString::fromUtf8(__FILE__)).absoluteDir().filePath("../src/main.cpp"));
    if (source.open(QIODevice::ReadOnly)) {
        const auto code = source.readAll(); const QByteArray marker("app.setStyleSheet(R\"(");
        const auto start = code.indexOf(marker), end = code.indexOf(")\");", start);
        if (start >= 0 && end > start) app.setStyleSheet(QString::fromUtf8(code.mid(start + marker.size(), end - start - marker.size())));
    }
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOption({"server", "Isolated loopback synthetic camera controller", "url"});
    parser.addOption({"screenshot", "Save synthetic snapshot preview", "path"}); parser.process(app);
    const QUrl url(parser.value("server"));
    if (!url.isValid() || url.scheme() != "ws" || url.host() != "127.0.0.1" || url.port() <= 0
        || url.port() == 5580 || !url.userInfo().isEmpty()) {
        std::fprintf(stderr, "An isolated loopback test controller is required; 5580 is refused.\n"); return 2;
    }
    MainWindow window; window.show();
    window.setServerUrl(url.toString());
    auto *client = window.client(); auto *snapshots = window.findChild<CameraSnapshots *>();
    QJsonObject result{{"syntheticOnly", true}}; QJsonObject counts;
    QObject::connect(client, &MatterClient::cameraResponse, &app,
        [&](const QString &, const QString &command, bool ok, const QJsonValue &, const QJsonObject &error) {
            if (ok) counts.insert(command, counts.value(command).toInt() + 1);
            else result.insert("lastError", QJsonObject{{"command", command}, {"code", error.value("error_code")}});
        });
    QElapsedTimer elapsed; elapsed.start(); QDateTime firstCaptured;
    int phase = 0; QString key; QTimer timer; timer.setInterval(100);
    auto print = [&](bool success) {
        result.insert("success", success); result.insert("commands", counts);
        std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData()); std::fflush(stdout);
    };
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (elapsed.elapsed() > 25000) {
            result.insert("reason", "timeout"); result.insert("phase", phase);
            if (!key.isEmpty()) result.insert("status", snapshots->state(key).message);
            print(false); timer.stop(); window.hide(); QTimer::singleShot(1000, &app, [&] { app.exit(1); }); return;
        }
        DeviceCard *card = nullptr;
        for (auto *candidate : window.findChildren<DeviceCard *>()) if (candidate->device().hasCamera) { card = candidate; key = card->device().key; break; }
        if (!card || key.isEmpty()) return;
        const auto state = snapshots->state(key);
        if (phase == 0 && !state.image.isNull() && !state.loading && counts.value("camera_release_stream").toInt() >= 1) {
            result.insert("decodedWidth", state.image.width()); result.insert("decodedHeight", state.image.height());
            result.insert("firstPreviewFresh", !state.stale && state.message.isEmpty()); firstCaptured = state.capturedAt;
            for (auto *nav : window.findChildren<QPushButton *>("navigation")) if (nav->text().contains("摄像头")) nav->click();
            phase = 1; return;
        }
        if (phase == 1) {
            if (auto *refresh = card->findChild<QPushButton *>("snapshotRefresh"); refresh && refresh->isEnabled()) { refresh->click(); phase = 2; }
            return;
        }
        if (phase == 2 && state.capturedAt > firstCaptured && !state.loading && counts.value("camera_release_stream").toInt() >= 2) {
            result.insert("manualRefreshChangedTimestamp", true);
            if (parser.isSet("screenshot")) window.grab().save(parser.value("screenshot"));
            snapshots->setActive(false); window.hide(); phase = 3;
            const auto device = card->device();
            const auto request = client->cameraRequest("camera_get_capabilities", {{"node_id", device.nodeId}, {"endpoint_id", device.endpointId}}, 10000, true);
            QObject::connect(client, &MatterClient::cameraResponse, &app,
                [&, request](const QString &id, const QString &, bool ok, const QJsonValue &value, const QJsonObject &) {
                    if (id != request) return;
                    const auto caps = value.toObject();
                    const bool clean = ok && caps.contains("sessions") && caps.value("sessions").toArray().isEmpty()
                        && caps.value("allocated").toObject().contains("snapshot") && caps.value("allocated").toObject().value("snapshot").toArray().isEmpty();
                    result.insert("resourcesReleased", clean);
                    const bool success = clean && result.value("firstPreviewFresh").toBool();
                    print(success);
                    timer.stop(); app.exit(success ? 0 : 1);
                });
        }
    });
    client->connectToServer(url); timer.start(); return app.exec();
}
