#include "camerawindow.h"
#include "matterclient.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSet>
#include <QHash>
#include <QTimer>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QWebEnginePermission>
#endif
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>
#include <memory>

namespace {
MatterDevice windowFixture() {
    // This is a window lifecycle fixture, not a simulated camera in demoNodes.
    // The demo controller is deliberately refused by CameraBridge; no peer,
    // camera stream, microphone request or server connection can be started.
    MatterDevice device;
    device.key = "9001/1"; device.nodeId = 9001; device.endpointId = 1;
    device.name = QStringLiteral("窗口生命周期测试（无视频源）");
    device.nodeName = QStringLiteral("仅用于回归测试");
    device.kind = "camera"; device.hasCamera = true; device.available = true;
    return device;
}

QJsonObject inspectPlayer(QWebEnginePage *page) {
    auto result = std::make_shared<QJsonObject>();
    QEventLoop loop;
    QPointer<QEventLoop> guardedLoop(&loop);
    page->runJavaScript(
        "window.MatterDeskCamera ? JSON.stringify(Object.assign(window.MatterDeskCamera.inspect(), "
        "{ objectFit: getComputedStyle(document.getElementById('video')).objectFit, "
        "placeholderDisplay: getComputedStyle(document.getElementById('placeholder')).display })) : ''",
        [result, guardedLoop](const QVariant &value) {
            *result = QJsonDocument::fromJson(value.toString().toUtf8()).object();
            if (guardedLoop) guardedLoop->quit();
        });
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    loop.exec();
    return *result;
}

struct Lifetime {
    QPointer<QWebEnginePage> page;
    QPointer<QWebEngineProfile> profile;
    QStringList destroyed;
};

class CapabilitiesFixtureServer : public QObject {
public:
    QWebSocketServer socket{QStringLiteral("Camera capability serialization fixture"), QWebSocketServer::NonSecureMode};
    QWebSocket *peer = nullptr;
    QList<QJsonObject> requests;
    bool firstStartBusy = false;
    int starts = 0;
    bool sharedStreams = false, deferSharedStart = false, holdFirstSharedRelease = false;
    QString forcedSharedProvenance;
    QSet<QString> activeViewers, sharedKinds{QStringLiteral("video"), QStringLiteral("audio")};
    QHash<QString, QString> provenanceByKey;
    QList<QJsonObject> pendingSharedStarts, pendingSharedReleases;
    int inUseReleases = 0, successfulReleases = 0, idempotentStops = 0;
    const QString otherSession = QStringLiteral("mini-already-playing");

    CapabilitiesFixtureServer() {
        socket.listen(QHostAddress::LocalHost, 0);
        connect(&socket, &QWebSocketServer::newConnection, this, [this] {
            peer = socket.nextPendingConnection(); peer->setParent(this);
            connect(peer, &QWebSocket::textMessageReceived, this, [this](const QString &message) {
                const auto request = QJsonDocument::fromJson(message.toUtf8()).object();
                requests.append(request);
                const QString command = request.value("command").toString();
                if (command == "start_listening") {
                    const QJsonObject attributes{
                        {"0/40/5", "仅用于能力传输测试"},
                        {"1/29/0", QJsonArray{QJsonObject{{"deviceType", 0x0142}, {"revision", 1}}}},
                        {"1/29/1", QJsonArray{29, 57, 0x0551, 0x0553}},
                        {"1/57/5", "原生通道测试（无视频源）"}, {"1/57/17", true}
                    };
                    reply(request, QJsonArray{QJsonObject{{"node_id", 9001}, {"available", true}, {"attributes", attributes}}});
                } else if (command == "camera_get_capabilities") {
                    reply(request, QJsonObject{
                        {"features", QJsonArray{"Video", "Audio", "Speaker"}},
                        {"privacy", QJsonObject{}},
                        {"video", QJsonObject{{"codecs", QJsonArray{"H264"}}, {"rate_distortion_points", QJsonArray{}}}},
                        {"audio", QJsonObject{{"codecs", QJsonArray{"OPUS"}}, {"channels", 1},
                            {"sample_rates", QJsonArray{48000}}, {"bit_depths", QJsonArray{16}}, {"two_way_talk_support", "FullDuplex"}}},
                        {"snapshot", QJsonObject{{"capabilities", QJsonArray{}}}},
                        {"limits", firstStartBusy ? QJsonObject{{"max_concurrent_sessions", 1}} : QJsonObject{}},
                        {"sessions", firstStartBusy ? QJsonArray{QJsonObject{{"client_session_key", otherSession},
                            {"webrtc_session_id", 77}, {"video_stream_id", 901}, {"audio_stream_id", 902}}} : QJsonArray{}}
                    });
                } else if (sharedStreams && command == "camera_start_stream") {
                    const auto key = request.value("args").toObject().value("client_session_key").toString();
                    activeViewers.insert(key);
                    ++starts;
                    provenanceByKey.insert(key, forcedSharedProvenance.isEmpty()
                        ? (starts == 1 ? QStringLiteral("allocated") : QStringLiteral("reused")) : forcedSharedProvenance);
                    if (deferSharedStart) pendingSharedStarts.append(request);
                    else replySharedStart(request);
                } else if (sharedStreams && command == "camera_stop_stream") {
                    const bool ended = activeViewers.remove(request.value("args").toObject().value("client_session_key").toString()) > 0;
                    if (!ended) ++idempotentStops;
                    reply(request, QJsonObject{{"ended", ended}});
                } else if (sharedStreams && command == "camera_release_stream") {
                    if (holdFirstSharedRelease) {
                        holdFirstSharedRelease = false; pendingSharedReleases.append(request); return;
                    }
                    if (!activeViewers.isEmpty()) {
                        ++inUseReleases;
                        send({{"message_id", request.value("message_id")}, {"error_code", 104}, {"details", "Fixture stream still in use"}});
                    } else {
                        sharedKinds.remove(request.value("args").toObject().value("kind").toString());
                        ++successfulReleases; reply(request, QJsonValue(QJsonValue::Null));
                    }
                } else if (firstStartBusy && command == "camera_start_stream") {
                    if (++starts == 1) {
                        // Matter error code 0 is still a failure when a status is
                        // supplied. Keep it numeric through WebSocket/WebChannel.
                        send({{"message_id", request.value("message_id")}, {"error_code", 0},
                            {"status", 137}, {"details", "ResourceExhausted"}});
                    } else {
                        // Retry can be accepted without opening real media or
                        // allocating resources owned by the existing Mini viewer.
                        reply(request, QJsonObject{{"webrtc_session_id", 78}, {"talkback", false},
                            {"video", QJsonObject{{"provenance", "adopted"}, {"stream_id", 903}}},
                            {"audio", QJsonValue(QJsonValue::Null)}});
                    }
                } else if (firstStartBusy && (command == "camera_stop_stream" || command == "camera_release_stream")) {
                    reply(request, QJsonValue(QJsonValue::Null));
                } else {
                    // Do not satisfy any start/media request if the player regresses.
                    send({{"message_id", request.value("message_id")}, {"error_code", 8}, {"details", "This fixture never starts media"}});
                }
            });
            send({{"schema_version", 14}, {"min_supported_schema_version", 11}, {"sdk_version", "Native capability test fixture"}});
        });
    }
    void replySharedStart(const QJsonObject &request) {
        const auto key = request.value("args").toObject().value("client_session_key").toString();
        const auto provenance = provenanceByKey.value(key);
        reply(request, QJsonObject{{"webrtc_session_id", provenance == "allocated" ? 81 : 82}, {"talkback", false},
            {"video", QJsonObject{{"provenance", provenance}, {"stream_id", 701}}},
            {"audio", QJsonObject{{"provenance", provenance}, {"stream_id", 702}}}});
    }
    QUrl url() const { return QUrl(QString("ws://127.0.0.1:%1/ws").arg(socket.serverPort())); }
    void reply(const QJsonObject &request, const QJsonValue &result) {
        send({{"message_id", request.value("message_id")}, {"result", result}});
    }
    void send(const QJsonObject &message) {
        if (peer) peer->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }
    int count(const QString &command) const {
        int result = 0;
        for (const auto &request : requests) if (request.value("command").toString() == command) ++result;
        return result;
    }
};
}

class CameraWindowTest : public QObject {
    Q_OBJECT
private slots:
    void init() {
        // Treat the warning that accompanied the native profile lifetime crash
        // as a test failure even if a platform happens not to crash this time.
        QTest::failOnWarning(QRegularExpression(QStringLiteral(
            ".*(Release of profile requested|WebEnginePage still not deleted).*")));
    }

    void loadedPlayerHasNoMediaThenDirectDeletion() {
        MatterClient client; client.setDemoMode(true);
        auto *window = new CameraWindow(&client, windowFixture());
        window->show();
        assertPlayerIdle(window);
        QCOMPARE(inspectPlayer(window->findChild<QWebEngineView *>("cameraVideo")->page())
                     .value("objectFit").toString(), QStringLiteral("contain"));
        auto life = watchLifetime(window);
        delete window;
        verifyReleased(life);
        QTest::qWait(40);
    }

    void titleBarCloseDisposesPlayerAndProfile() {
        MatterClient client; client.setDemoMode(true);
        auto *window = new CameraWindow(&client, windowFixture());
        window->setAttribute(Qt::WA_DeleteOnClose); window->show();
        assertPlayerIdle(window);
        auto life = watchLifetime(window);
        QPointer<CameraWindow> guardedWindow(window);
        window->close();
        QTRY_VERIFY_WITH_TIMEOUT(guardedWindow.isNull(), 4000);
        verifyReleased(life);
    }

    void escapeUsesSameDisposalPath() {
        MatterClient client; client.setDemoMode(true);
        auto *window = new CameraWindow(&client, windowFixture());
        window->setAttribute(Qt::WA_DeleteOnClose); window->show();
        assertPlayerIdle(window);
        auto life = watchLifetime(window);
        QPointer<CameraWindow> guardedWindow(window);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(guardedWindow.isNull(), 4000);
        verifyReleased(life);
    }

    void embeddedPlayerFillsClientAreaAndOverlaysControls() {
        MatterClient client; client.setDemoMode(true);
        QWidget host;
        auto *layout = new QVBoxLayout(&host); layout->setContentsMargins(0, 0, 0, 0);
        auto *window = new CameraWindow(&client, windowFixture(), &host);
        window->setCompactMode(true); window->setEmbeddedMode();
        // Both call orders must preserve the child geometry and zero minimum.
        window->setCompactMode(true);
        window->setAttribute(Qt::WA_DeleteOnClose);
        layout->addWidget(window); host.resize(400, 280); host.show();
        QVERIFY(!window->isWindow());
        QCOMPARE(window->minimumSize(), QSize(0, 0));
        assertPlayerIdle(window);
        auto *view = window->findChild<QWebEngineView *>("cameraVideo"); QVERIFY(view);
        auto *overlay = window->findChild<QWidget *>("cameraOverlay"); QVERIFY(overlay);
        // The retry button is normally hidden; include the widest control row.
        window->setPlayerState("error", QStringLiteral("连接失败，点击重试。"), false, false, false);
        for (const QSize size : {QSize(400, 280), QSize(640, 360), QSize(400, 280)}) {
            host.resize(size);
            QTRY_COMPARE(window->size(), size);
            QTRY_COMPARE(view->geometry(), window->rect());
            QVERIFY(window->rect().contains(overlay->geometry()));
            QVERIFY(overlay->isVisible());
            for (auto *button : overlay->findChildren<QPushButton *>()) {
                if (!button->isVisible()) continue;
                const QRect buttonRect(button->mapTo(window, QPoint()), button->size());
                QVERIFY2(overlay->geometry().contains(buttonRect), qPrintable(button->text()));
                // Native controls must be the hit target above the WebEngine.
                QCOMPARE(window->childAt(buttonRect.center()), button);
            }
        }
        QTRY_COMPARE(inspectPlayer(view->page()).value("objectFit").toString(), QStringLiteral("cover"));
        QTRY_COMPARE(inspectPlayer(view->page()).value("placeholderDisplay").toString(), QStringLiteral("none"));
        // Viewer controls may hide while the same embedded player remains alive.
        auto *originalPage = view->page();
        window->setOverlayVisible(false);
        QVERIFY(!overlay->isVisible());
        window->setPlayerState("error", QStringLiteral("后台状态更新"), false, false, false);
        host.resize(480, 320);
        QTRY_COMPARE(window->size(), QSize(480, 320));
        QVERIFY(!overlay->isVisible());
        QCOMPARE(view->page(), originalPage);
        // MatterDesk fits the entire frame and retains its status placeholder.
        window->setVideoFit(true);
        QTRY_COMPARE(inspectPlayer(view->page()).value("objectFit").toString(), QStringLiteral("contain"));
        QTRY_COMPARE(inspectPlayer(view->page()).value("placeholderDisplay").toString(), QStringLiteral("block"));
        window->setVideoFit(false);
        QTRY_COMPARE(inspectPlayer(view->page()).value("objectFit").toString(), QStringLiteral("cover"));
        QTRY_COMPARE(inspectPlayer(view->page()).value("placeholderDisplay").toString(), QStringLiteral("none"));
        window->setOverlayVisible(true);
        QVERIFY(overlay->isVisible());
        auto life = watchLifetime(window);
        QPointer<CameraWindow> guardedWindow(window);
        QPushButton *stop = nullptr;
        for (auto *button : overlay->findChildren<QPushButton *>())
            if (button->property("cameraAction").toString() == "stop") stop = button;
        QVERIFY(stop); QVERIFY(stop->isEnabled());
        stop->click();
        QTRY_VERIFY_WITH_TIMEOUT(guardedWindow.isNull(), 4000);
        QVERIFY(host.isVisible());
        verifyReleased(life);
    }

    void embeddedEscapeUsesSameDisposalPath() {
        MatterClient client; client.setDemoMode(true);
        QWidget host;
        auto *window = new CameraWindow(&client, windowFixture(), &host);
        window->setEmbeddedMode(); window->setCompactMode(true);
        window->setAttribute(Qt::WA_DeleteOnClose); host.show(); window->show();
        assertPlayerIdle(window);
        auto life = watchLifetime(window);
        QPointer<CameraWindow> guardedWindow(window);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(guardedWindow.isNull(), 4000);
        QVERIFY(host.isVisible());
        verifyReleased(life);
    }

    void directDeletionWhileJavaScriptCloseCallbackIsQueued() {
        MatterClient client; client.setDemoMode(true);
        auto *window = new CameraWindow(&client, windowFixture());
        window->show();
        assertPlayerIdle(window);
        auto life = watchLifetime(window);
        // close() queues JS disposal plus a fallback timer. Destruction can also
        // occur immediately afterward when the application's parent closes.
        window->close();
        delete window;
        verifyReleased(life);
        QTest::qWait(750);
    }

    void directDeletionBeforePlayerFinishesLoading() {
        MatterClient client; client.setDemoMode(true);
        auto *window = new CameraWindow(&client, windowFixture());
        auto life = watchLifetime(window);
        delete window;
        verifyReleased(life);
        QTest::qWait(40);
    }

    void capabilityObjectSurvivesWebSocketAndNativeWebChannel() {
        CapabilitiesFixtureServer fixture;
        QVERIFY(fixture.socket.isListening());
        MatterClient client;
        client.connectToServer(fixture.url());
        QTRY_VERIFY(client.isConnected());
        QCOMPARE(client.devices().size(), 1);
        QVERIFY(client.devices().first().hasCamera);
        QSignalSpy responses(&client, &MatterClient::cameraResponse);
        auto window = std::make_unique<CameraWindow>(&client, client.devices().first());
        window->show();
        auto *view = window->findChild<QWebEngineView *>("cameraVideo"); QVERIFY(view);
        QTRY_COMPARE_WITH_TIMEOUT(responses.size(), 1, 15000);
        QCOMPARE(responses.first().at(1).toString(), QStringLiteral("camera_get_capabilities"));
        QVERIFY(responses.first().at(2).toBool());
        const auto coreResult = responses.first().at(3).value<QJsonValue>();
        QVERIFY(coreResult.isObject());
        QCOMPARE(coreResult.toObject().value("audio").toObject().value("codecs").toArray(), QJsonArray{"OPUS"});
        QJsonObject state;
        QElapsedTimer timer; timer.start();
        do {
            state = inspectPlayer(view->page());
            if (!state.value("audioCodecs").toArray().isEmpty()) break;
            QTest::qWait(30);
        } while (timer.elapsed() < 5000);
        const QByteArray metadata = QJsonDocument(state).toJson(QJsonDocument::Compact);
        QVERIFY2(!state.value("audioCodecs").toArray().isEmpty(), metadata.constData());
        QCOMPARE(state.value("capabilitiesType").toString(), QStringLiteral("object"));
        QCOMPARE(state.value("capabilityFeatures").toArray(), QJsonArray({"Video", "Audio", "Speaker"}));
        QCOMPARE(state.value("videoCodecs").toArray(), QJsonArray{"H264"});
        QCOMPARE(state.value("audioCodecs").toArray(), QJsonArray{"OPUS"});
        QCOMPARE(state.value("twoWayTalkSupport").toString(), QStringLiteral("FullDuplex"));
        QVERIFY(state.value("connected").toBool());
        QCOMPARE(state.value("peerState").toString(), QStringLiteral("closed"));
        QVERIFY(!state.value("micActive").toBool());
        QCOMPARE(state.value("videoWidth").toInt(), 0);
        QCOMPARE(state.value("remoteAudioTracks").toInt(), 0);
        QCOMPARE(fixture.requests.size(), 2);
        QCOMPARE(fixture.requests.last().value("command").toString(), QStringLiteral("camera_get_capabilities"));
        const auto args = fixture.requests.last().value("args").toObject();
        QCOMPARE(args.value("node_id").toInteger(), qint64(9001));
        QCOMPARE(args.value("endpoint_id").toInt(), 1);
        auto life = watchLifetime(window.get());
        window.reset(); verifyReleased(life);
        client.disconnectFromServer();
    }

    void compactAutoPlayExplainsBusyStatusZeroAndRetryPreservesOtherViewer() {
        CapabilitiesFixtureServer fixture; fixture.firstStartBusy = true;
        QVERIFY(fixture.socket.isListening());
        MatterClient client; client.connectToServer(fixture.url()); QTRY_VERIFY(client.isConnected());
        QSignalSpy responses(&client, &MatterClient::cameraResponse);
        QSignalSpy globalErrors(&client, &MatterClient::errorOccurred);
        auto window = std::make_unique<CameraWindow>(&client, client.devices().first());
        window->setCompactMode(true); window->show();
        auto *view = window->findChild<QWebEngineView *>("cameraVideo"); QVERIFY(view);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QSignalSpy permissions(view->page(), &QWebEnginePage::permissionRequested);
#else
        QSignalSpy permissions(view->page(), &QWebEnginePage::featurePermissionRequested);
#endif
        QVERIFY(permissions.isValid());
        // Exercise the compact window's queued native play path before its
        // bundled WebEngine player has necessarily completed loading.
        window->play();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.starts, 1, 15000);
        QLabel *status = nullptr;
        for (auto *label : window->findChildren<QLabel *>())
            if (label->accessibleName() == "cameraStatus") status = label;
        QVERIFY(status);
        QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("Mini"))
            && status->text().contains(QStringLiteral("停止直播"))
            && status->text().contains(QStringLiteral("重试")), 10000);
        QVERIFY(status->isVisible());
        QVERIFY(!status->text().contains(QStringLiteral("错误 0")));
        QPushButton *retry = nullptr, *talk = nullptr;
        for (auto *button : window->findChildren<QPushButton *>()) {
            if (button->property("cameraAction").toString() == "start") retry = button;
            if (button->property("cameraAction").toString() == "talk") talk = button;
        }
        QVERIFY(retry); QCOMPARE(retry->text(), QStringLiteral("重试"));
        QVERIFY(retry->isVisible()); QVERIFY(retry->isEnabled());
        QVERIFY(talk); QVERIFY(!talk->isEnabled()); QVERIFY(!talk->isChecked());
        QJsonObject failure;
        for (const auto &response : responses) {
            if (response.at(1).toString() == "camera_start_stream" && !response.at(2).toBool())
                failure = response.at(4).value<QJsonObject>();
        }
        QVERIFY(failure.value("error_code").isDouble()); QCOMPARE(failure.value("error_code").toInt(-1), 0);
        QVERIFY(failure.value("status").isDouble()); QCOMPARE(failure.value("status").toInt(-1), 137);
        const auto player = inspectPlayer(view->page());
        QCOMPARE(player.value("state").toString(), QStringLiteral("error"));
        const auto diagnostic = player.value("lastFailure").toObject();
        QVERIFY(diagnostic.value("code").isDouble()); QCOMPARE(diagnostic.value("code").toInt(-1), 0);
        QCOMPARE(diagnostic.value("status").toInt(-1), 137);
        QVERIFY(!player.value("micActive").toBool()); QCOMPARE(player.value("peerState").toString(), QStringLiteral("closed"));
        QCOMPARE(permissions.count(), 0);
        QCOMPARE(globalErrors.count(), 0); // The native camera label owns this failure.
        QTest::mouseClick(retry, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.starts, 2, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(([&responses] {
            for (const auto &response : responses)
                if (response.at(1).toString() == "camera_start_stream" && response.at(2).toBool()) return true;
            return false;
        })(), 10000);
        QVERIFY(!inspectPlayer(view->page()).value("micActive").toBool()); QCOMPARE(permissions.count(), 0);
        QCOMPARE(globalErrors.count(), 0);
        auto life = watchLifetime(window.get()); window.reset(); verifyReleased(life);
        QTRY_COMPARE(fixture.count("camera_stop_stream"), 2);
        QCOMPARE(fixture.count("camera_release_stream"), 0);
        QStringList attemptedSessions;
        for (const auto &request : fixture.requests) {
            const auto command = request.value("command").toString();
            const auto args = request.value("args").toObject();
            QVERIFY2(!command.contains("evict"), qPrintable(command));
            if (command == "camera_start_stream") {
                QVERIFY(args.value("allow_eviction").isBool()); QVERIFY(!args.value("allow_eviction").toBool());
                const auto key = args.value("client_session_key").toString(); QVERIFY(!key.isEmpty());
                QVERIFY(key != fixture.otherSession); attemptedSessions.append(key);
            }
        }
        QCOMPARE(attemptedSessions.size(), 2); QVERIFY(attemptedSessions.first() != attemptedSessions.last());
        for (const auto &request : fixture.requests) if (request.value("command").toString() == "camera_stop_stream") {
            const auto key = request.value("args").toObject().value("client_session_key").toString();
            QVERIFY(key != fixture.otherSession); QVERIFY(attemptedSessions.contains(key));
        }
        client.disconnectFromServer();
    }

    void sharedNativeCleanupPreservesTheOtherViewerAndReleasesTheLastReusedAllocation_data() {
        QTest::addColumn<bool>("lateReusedStart");
        QTest::newRow("acknowledged reused start") << false;
        QTest::newRow("reused start acknowledged after window destruction") << true;
    }

    void sharedNativeCleanupPreservesTheOtherViewerAndReleasesTheLastReusedAllocation() {
        QFETCH(bool, lateReusedStart);
        CapabilitiesFixtureServer fixture; fixture.sharedStreams = true;
        QVERIFY(fixture.socket.isListening());
        MatterClient client; client.connectToServer(fixture.url()); QTRY_VERIFY(client.isConnected());
        QSignalSpy globalErrors(&client, &MatterClient::errorOccurred);
        QSignalSpy responses(&client, &MatterClient::cameraResponse);
        auto first = std::make_unique<CameraWindow>(&client, client.devices().first());
        first->setCompactMode(true); first->show(); first->play();
        auto *firstView = first->findChild<QWebEngineView *>("cameraVideo"); QVERIFY(firstView);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.starts, 1, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(([&responses] {
            for (const auto &response : responses)
                if (response.at(1).toString() == "camera_start_stream" && response.at(2).toBool()) return true;
            return false;
        })(), 10000);
        const auto firstKey = *fixture.activeViewers.cbegin();
        QVERIFY(!firstKey.isEmpty());
        QVERIFY(!inspectPlayer(firstView->page()).value("micActive").toBool());
        fixture.deferSharedStart = lateReusedStart;
        auto second = std::make_unique<CameraWindow>(&client, client.devices().first());
        second->setCompactMode(true); second->show(); second->play();
        auto *secondView = second->findChild<QWebEngineView *>("cameraVideo"); QVERIFY(secondView);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QSignalSpy permissions(secondView->page(), &QWebEnginePage::permissionRequested);
#else
        QSignalSpy permissions(secondView->page(), &QWebEnginePage::featurePermissionRequested);
#endif
        QTRY_COMPARE_WITH_TIMEOUT(fixture.starts, 2, 15000);
        QTRY_COMPARE(fixture.activeViewers.size(), 2);
        auto survivingKeys = fixture.activeViewers; survivingKeys.remove(firstKey);
        const auto secondKey = *survivingKeys.cbegin(); QVERIFY(secondKey != firstKey);
        if (lateReusedStart) QTRY_COMPARE(fixture.pendingSharedStarts.size(), 1);
        else QTRY_VERIFY_WITH_TIMEOUT(([&responses] {
            int count = 0;
            for (const auto &response : responses)
                if (response.at(1).toString() == "camera_start_stream" && response.at(2).toBool()) ++count;
            return count == 2;
        })(), 10000);
        auto firstLife = watchLifetime(first.get()); first.reset(); verifyReleased(firstLife);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_stop_stream"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.inUseReleases, 2, 5000);
        QCOMPARE(fixture.successfulReleases, 0); QCOMPARE(fixture.sharedKinds.size(), 2);
        QCOMPARE(fixture.activeViewers, QSet<QString>{secondKey});
        QCOMPARE(globalErrors.count(), 0);
        const auto survivor = inspectPlayer(secondView->page());
        QCOMPARE(survivor.value("state").toString(), QStringLiteral("connecting"));
        QVERIFY(survivor.value("peerState").toString() != "closed");
        QVERIFY(!survivor.value("micActive").toBool()); QCOMPARE(permissions.count(), 0);
        auto secondLife = watchLifetime(second.get()); second.reset(); verifyReleased(secondLife);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_stop_stream"), 2, 5000);
        QTRY_VERIFY(fixture.activeViewers.isEmpty());
        if (lateReusedStart) {
            QCOMPARE(fixture.successfulReleases, 0);
            fixture.replySharedStart(fixture.pendingSharedStarts.takeFirst());
        }
        QTRY_COMPARE_WITH_TIMEOUT(fixture.successfulReleases, 2, 5000);
        QVERIFY(fixture.sharedKinds.isEmpty()); QCOMPARE(fixture.count("camera_release_stream"), 4);
        QCOMPARE(globalErrors.count(), 0); QCOMPARE(permissions.count(), 0);
        int seenStops = 0, seenReleases = 0;
        for (const auto &request : fixture.requests) {
            const auto command = request.value("command").toString();
            const auto args = request.value("args").toObject();
            QVERIFY(!command.contains("evict"));
            if (command == "camera_stop_stream") {
                const auto key = args.value("client_session_key").toString();
                QCOMPARE(key, seenStops++ == 0 ? firstKey : secondKey);
            } else if (command == "camera_release_stream") {
                QCOMPARE(seenStops, seenReleases < 2 ? 1 : 2); ++seenReleases;
                QCOMPARE(args.value("node_id").toInteger(), qint64(9001)); QCOMPARE(args.value("endpoint_id").toInt(), 1);
                const auto kind = args.value("kind").toString();
                QVERIFY(kind == "video" || kind == "audio");
                QCOMPARE(args.value("stream_id").toInt(), kind == "video" ? 701 : 702);
            }
        }
        QCOMPARE(seenStops, 2); QCOMPARE(seenReleases, 4); client.disconnectFromServer();
    }

    void nativeCleanupNeverReleasesAdoptedStreams() {
        CapabilitiesFixtureServer fixture; fixture.sharedStreams = true; fixture.forcedSharedProvenance = "adopted";
        MatterClient client; client.connectToServer(fixture.url()); QTRY_VERIFY(client.isConnected());
        QSignalSpy responses(&client, &MatterClient::cameraResponse);
        auto window = std::make_unique<CameraWindow>(&client, client.devices().first());
        window->setCompactMode(true); window->show(); window->play();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.starts, 1, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(([&responses] {
            for (const auto &response : responses)
                if (response.at(1).toString() == "camera_start_stream" && response.at(2).toBool()) return true;
            return false;
        })(), 10000);
        auto *view = window->findChild<QWebEngineView *>("cameraVideo"); QVERIFY(view);
        QVERIFY(!inspectPlayer(view->page()).value("micActive").toBool());
        auto life = watchLifetime(window.get()); window.reset(); verifyReleased(life);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_stop_stream"), 1, 5000);
        QTest::qWait(100);
        QCOMPARE(fixture.count("camera_release_stream"), 0); QCOMPARE(fixture.sharedKinds.size(), 2);
        client.disconnectFromServer();
    }

    void windowDestructionAfterStopAcknowledgementCompletesPendingStreamRelease_data() {
        QTest::addColumn<QString>("provenance");
        QTest::newRow("allocated") << QStringLiteral("allocated");
        QTest::newRow("reused") << QStringLiteral("reused");
    }

    void windowDestructionAfterStopAcknowledgementCompletesPendingStreamRelease() {
        QFETCH(QString, provenance);
        CapabilitiesFixtureServer fixture; fixture.sharedStreams = true;
        fixture.forcedSharedProvenance = provenance; fixture.holdFirstSharedRelease = true;
        MatterClient client; client.connectToServer(fixture.url()); QTRY_VERIFY(client.isConnected());
        QSignalSpy errors(&client, &MatterClient::errorOccurred);
        auto window = std::make_unique<CameraWindow>(&client, client.devices().first());
        window->setCompactMode(true); window->show(); window->play();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.starts, 1, 15000);
        auto *bridge = window->findChild<CameraBridge *>(); QVERIFY(bridge);
        bridge->action("stop", false);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.pendingSharedReleases.size(), 1, 5000);
        QCOMPARE(fixture.count("camera_stop_stream"), 1);
        QVERIFY(fixture.activeViewers.isEmpty()); QCOMPARE(fixture.successfulReleases, 0);
        QString key;
        for (const auto &request : fixture.requests) if (request.value("command").toString() == "camera_stop_stream")
            key = request.value("args").toObject().value("client_session_key").toString();
        QVERIFY(!key.isEmpty());
        auto life = watchLifetime(window.get()); window.reset(); verifyReleased(life);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_stop_stream"), 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.successfulReleases, 2, 5000);
        QCOMPARE(fixture.idempotentStops, 1); QVERIFY(fixture.sharedKinds.isEmpty());
        QCOMPARE(fixture.count("camera_release_stream"), 3); QCOMPARE(errors.count(), 0);
        for (const auto &request : fixture.requests) if (request.value("command").toString() == "camera_stop_stream")
            QCOMPARE(request.value("args").toObject().value("client_session_key").toString(), key);
        // The discarded page's original release can complete afterward safely.
        fixture.reply(fixture.pendingSharedReleases.takeFirst(), QJsonValue(QJsonValue::Null));
        QTest::qWait(50); QCOMPARE(errors.count(), 0);
        client.disconnectFromServer();
    }

    void retirementCompletionIgnoresAnOlderAttemptAndPreventsRedundantCleanup() {
        CapabilitiesFixtureServer fixture; fixture.sharedStreams = true;
        MatterClient client; client.connectToServer(fixture.url()); QTRY_VERIFY(client.isConnected());
        QSignalSpy errors(&client, &MatterClient::errorOccurred);
        CameraBridge bridge(&client, client.devices().first(), nullptr);
        QSignalSpy responses(&bridge, &CameraBridge::response);
        bridge.request("old-start", "camera_start_stream", QJsonObject{{"client_session_key", "old-key"}});
        QTRY_COMPARE(responses.count(), 1);
        bridge.request("old-stop", "camera_stop_stream", QJsonObject{{"client_session_key", "old-key"}});
        QTRY_COMPARE(responses.count(), 2);
        bridge.request("new-start", "camera_start_stream", QJsonObject{{"client_session_key", "new-key"}});
        QTRY_COMPARE(responses.count(), 3);
        bridge.retired("old-key"); bridge.cancelSession();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_stop_stream"), 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.successfulReleases, 2, 5000);
        QVERIFY(fixture.activeViewers.isEmpty());
        QString lastStopped;
        for (const auto &request : fixture.requests) if (request.value("command").toString() == "camera_stop_stream")
            lastStopped = request.value("args").toObject().value("client_session_key").toString();
        QCOMPARE(lastStopped, QStringLiteral("new-key")); QCOMPARE(errors.count(), 0);

        // A completed attempt explicitly retires its key, so close is a no-op.
        fixture.sharedKinds = {QStringLiteral("video"), QStringLiteral("audio")};
        bridge.request("done-start", "camera_start_stream", QJsonObject{{"client_session_key", "done-key"}});
        QTRY_COMPARE(responses.count(), 4);
        bridge.request("done-stop", "camera_stop_stream", QJsonObject{{"client_session_key", "done-key"}});
        QTRY_COMPARE(responses.count(), 5);
        bridge.request("done-video", "camera_release_stream", QJsonObject{{"kind", "video"}, {"stream_id", 701}});
        bridge.request("done-audio", "camera_release_stream", QJsonObject{{"kind", "audio"}, {"stream_id", 702}});
        QTRY_COMPARE(responses.count(), 7);
        bridge.retired("done-key"); bridge.cancelSession();
        QTest::qWait(50);
        QCOMPARE(fixture.count("camera_stop_stream"), 3); QCOMPARE(fixture.count("camera_release_stream"), 4);
        QVERIFY(fixture.sharedKinds.isEmpty()); QCOMPARE(errors.count(), 0);
        client.disconnectFromServer();
    }

private:
    void assertPlayerIdle(CameraWindow *window) {
        auto *view = window->findChild<QWebEngineView *>("cameraVideo");
        QVERIFY(view);
        QVERIFY(view->page()->profile()->isOffTheRecord());
        QVERIFY(!view->page()->settings()->testAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls));
        QVERIFY(!view->page()->settings()->testAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls));
        QJsonObject state;
        QTRY_VERIFY_WITH_TIMEOUT(!(state = inspectPlayer(view->page())).isEmpty(), 15000);
        QCOMPARE(view->page()->url(), QUrl("qrc:///matterdesk/camera/player.html"));
        QVERIFY(state.value("state").toString() == "idle" || state.value("state").toString() == "disconnected");
        QVERIFY(!state.value("connected").toBool());
        QVERIFY(!state.value("micActive").toBool());
        QVERIFY(!state.value("talkAvailable").toBool());
        QCOMPARE(state.value("peerState").toString(), QStringLiteral("closed"));
        QCOMPARE(state.value("videoWidth").toInt(), 0);
        QCOMPARE(state.value("receivedVideoFrames").toInt(), 0);
        QCOMPARE(state.value("remoteAudioTracks").toInt(), 0);
    }

    std::shared_ptr<Lifetime> watchLifetime(CameraWindow *window) {
        auto life = std::make_shared<Lifetime>();
        auto *view = window->findChild<QWebEngineView *>("cameraVideo");
        life->page = view->page(); life->profile = view->page()->profile();
        connect(life->page, &QObject::destroyed, this, [life] { life->destroyed.append("page"); });
        connect(life->profile, &QObject::destroyed, this, [life] { life->destroyed.append("profile"); });
        return life;
    }

    void verifyReleased(const std::shared_ptr<Lifetime> &life) {
        QVERIFY(life->page.isNull());
        QVERIFY(life->profile.isNull());
        QCOMPARE(life->destroyed, QStringList({"page", "profile"}));
    }
};

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    // Headless rendering only; Chromium's process sandbox remains enabled.
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", qgetenv("QTWEBENGINE_CHROMIUM_FLAGS") + " --disable-gpu");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setApplicationName("MatterDeskCameraWindowTests");
    CameraWindowTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_camerawindow.moc"
