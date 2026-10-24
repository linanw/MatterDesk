// Explicit integration probe for an isolated synthetic camera, never part of ctest.
// It neither loads saved user settings nor records audio. --fake-mic uses Chromium's
// synthetic device and automatically accepts only this test dialog's permission.
#include "camerawindow.h"
#include "matterclient.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QWebEngineView>
#include <QWebEnginePage>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QWebEnginePermission>
#endif
#include <cstdio>
#include <memory>

int main(int argc, char **argv) {
    // Chromium may initialize during QApplication construction.
    for (int i = 1; i < argc; ++i) if (QByteArray(argv[i]) == "--fake-mic") {
        QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
        flags += " --use-fake-device-for-media-stream";
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
    }
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setApplicationName("MatterDeskCameraProbe");
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOption({"server", "Isolated loopback test server (5580 is refused)", "url"});
    parser.addOption({"screenshot", "Save synthetic video preview", "path"});
    parser.addOption({"fake-mic", "Test talkback using Chromium synthetic microphone only"});
    parser.process(app);
    const QUrl url(parser.value("server"));
    if (!url.isValid() || url.scheme() != "ws" || url.host() != "127.0.0.1" || url.port() <= 0 || url.port() == 5580 || !url.userInfo().isEmpty()) {
        std::fprintf(stderr, "Specify an isolated synthetic loopback server with --server; production 5580 is refused.\n"); return 2;
    }
    MatterClient client; QPointer<CameraWindow> window;
    QJsonObject result; QElapsedTimer elapsed; elapsed.start();
    QObject::connect(&client, &MatterClient::cameraResponse, &app, [&](const QString &, const QString &command, bool ok, const QJsonValue &, const QJsonObject &error) {
        auto operations = result.value("operations").toObject();
        QJsonObject status{{"success", ok}};
        if (!ok) { status.insert("error_code", error.value("error_code")); status.insert("reason", error.value("reason")); status.insert("details", error.value("details")); }
        operations.insert(command, status); result.insert("operations", operations);
    });
    QObject::connect(&client, &MatterClient::cameraEvent, &app, [&](const QString &event, const QJsonObject &data) {
        auto events = result.value("events").toArray();
        events.append(QJsonObject{{"event", event}, {"type", data.value("event_type")},
            {"node_id", data.value("node_id")}, {"nodeIdIsNumber", data.value("node_id").isDouble()},
            {"endpoint_id", data.value("endpoint_id")}, {"hasSessionKey", data.contains("client_session_key")}});
        result.insert("events", events);
    });
    int phase = 0; bool probing = false, requestedMic = false, endedMic = false, requestedSound = false;
    qint64 streamingAt = 0, micAt = 0;
    auto findButton = [&](const QString &text) -> QPushButton * {
        if (window) for (auto *button : window->findChildren<QPushButton *>()) if (button->text() == text) return button;
        return nullptr;
    };
    QTimer ticker; ticker.setInterval(200);
    QObject::connect(&ticker, &QTimer::timeout, &app, [&] {
        if (elapsed.elapsed() > 35000) {
            result.insert("success", false); result.insert("reason", "timeout");
            result.insert("phase", phase); result.insert("controller", client.statusText()); result.insert("endpoints", client.devices().size());
            if (window) for (auto *text : window->findChildren<QLabel *>()) if (text->objectName() == "muted") result.insert("status", text->text().left(150));
            std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData()); std::fflush(stdout);
            ticker.stop(); if (window) window->close(); QTimer::singleShot(1000, &app, [&] { app.exit(1); }); return;
        }
        if (phase == 0 && client.isConnected()) {
            for (auto device : client.devices()) if (device.hasCamera && device.available) {
                device.name = "合成测试摄像头（非真实画面）";
                window = new CameraWindow(&client, device); window->setAttribute(Qt::WA_DeleteOnClose); window->show();
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
                QObject::connect(window->findChild<QWebEngineView *>()->page(), &QWebEnginePage::permissionRequested, &app,
                    [&](QWebEnginePermission permission) {
                        result.insert("micPermission", QJsonObject{{"scheme", permission.origin().scheme()},
                            {"type", int(permission.permissionType())}, {"valid", permission.isValid()}});
                    });
#endif
                phase = 1; break;
            }
        }
        if (phase == 1) if (auto *start = findButton("开始直播"); start && start->isEnabled()) { start->click(); phase = 2; }
        if (phase != 2 || probing || !window) return;
        if (parser.isSet("fake-mic")) {
            for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QMessageBox *>(widget); dialog && dialog->windowTitle() == "麦克风权限") {
                result.insert("nativeMicPromptsAccepted", result.value("nativeMicPromptsAccepted").toInt() + 1);
                dialog->button(QMessageBox::Yes)->click();
            }
        }
        probing = true;
        window->findChild<QWebEngineView *>()->page()->runJavaScript(
            "JSON.stringify((()=>{window.MatterDeskCamera?.sampleStats?.().then(s=>window.__qaStats=s);const v=document.querySelector('video');return {width:v?.videoWidth||0,height:v?.videoHeight||0,time:v?.currentTime||0,frames:v?.getVideoPlaybackQuality?.().totalVideoFrames||0,audioTracks:v?.srcObject?.getAudioTracks().length||0,muted:v?.muted,rtp:window.__qaStats||null,player:window.MatterDeskCamera?.inspect?.()||null}})())",
            [&](const QVariant &value) {
                probing = false;
                const auto state = QJsonDocument::fromJson(value.toString().toUtf8()).object();
                result.insert("player", state.value("player"));
                if (state.value("player").toObject().value("state").toString() == "error") {
                    result.insert("success", false); result.insert("reason", "player-error");
                    std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData()); std::fflush(stdout);
                    phase = 3; if (window) window->close();
                    QTimer::singleShot(1500, &app, [&] { app.exit(1); }); return;
                }
                if (state.value("width").toInt() <= 0 || state.value("time").toDouble() < 0.7) return;
                if (state.value("rtp").toObject().value("audioPackets").toInt() <= 0) return;
                result.insert("video", state);
                if (!requestedSound) {
                    auto *sound = findButton("收听声音"); if (!sound || !sound->isEnabled()) return;
                    requestedSound = true;
                    window->findChild<QWebEngineView *>()->page()->runJavaScript("document.querySelector('video').volume=0", [&, sound = QPointer<QPushButton>(sound)](const QVariant &) { if (sound) sound->click(); });
                    return;
                }
                if (state.value("muted").toBool()) return;
                result.insert("listenUnmutedAtZeroTestVolume", true);
                if (!streamingAt) streamingAt = elapsed.elapsed();
                if (!parser.isSet("fake-mic")) { if (elapsed.elapsed() - streamingAt < 800) return; }
                else {
                    if (!requestedMic) {
                        auto *talk = findButton("开启对讲"); if (!talk || !talk->isEnabled()) return;
                        requestedMic = true; talk->click(); return;
                    }
                    bool active = false;
                    if (window) for (auto *label : window->findChildren<QLabel *>()) active |= label->text().contains("麦克风正在对讲");
                    if (active && !micAt) { micAt = elapsed.elapsed(); result.insert("syntheticMicActive", true); }
                    if (!endedMic && micAt && elapsed.elapsed() - micAt > 1200) {
                        if (auto *talk = findButton("结束对讲")) { talk->click(); endedMic = true; }
                        return;
                    }
                    if (!endedMic || active) return;
                    result.insert("syntheticMicStopped", true);
                }
                if (window && parser.isSet("screenshot")) window->grab().save(parser.value("screenshot"));
                if (window) window->close(); phase = 3;
                QTimer::singleShot(1600, &app, [&] {
                    const auto id = client.cameraRequest("camera_get_capabilities", {{"node_id", 1}, {"endpoint_id", 1}});
                    auto request = std::make_shared<QString>(id);
                    QObject::connect(&client, &MatterClient::cameraResponse, &app, [&, request](const QString &reply, const QString &, bool ok, const QJsonValue &caps, const QJsonObject &) {
                        if (reply != *request) return;
                        const bool stopped = ok && caps.toObject().value("sessions").toArray().isEmpty();
                        result.insert("sessionStopped", stopped); result.insert("success", stopped);
                        std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData()); std::fflush(stdout); app.exit(stopped ? 0 : 1);
                    });
                });
            });
    });
    client.connectToServer(url); ticker.start();
    return app.exec();
}
