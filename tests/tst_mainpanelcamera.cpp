#include "mainwindow.h"
#include "camerawindow.h"
#include "camerametadata.h"
#include "homestore.h"
#include "devicecard.h"
#include "homestyle.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QFont>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QWindow>
#include <QtTest>
#include <memory>

namespace {
// Loopback-only protocol fixture. It never connects to a controller, reads a
// camera, negotiates real RTP, requests microphone access or saves media.
class PanelFixture : public QObject {
public:
    QWebSocketServer socket{QStringLiteral("Main panel camera fixture"), QWebSocketServer::NonSecureMode};
    QPointer<QWebSocket> peer;
    QList<QJsonObject> requests;
    QSet<QString> activeKeys;
    QJsonObject metadata; // Optional vendor attributes; absent in the original media fixtures.
    PanelFixture() {
        socket.listen(QHostAddress::LocalHost, 0);
        connect(&socket, &QWebSocketServer::newConnection, this, [this] {
            peer = socket.nextPendingConnection(); peer->setParent(this);
            connect(peer, &QWebSocket::textMessageReceived, this, [this](const QString &message) {
                const auto request = QJsonDocument::fromJson(message.toUtf8()).object();
                requests.append(request);
                const auto command = request.value("command").toString();
                if (command == "start_listening" || command == "get_nodes") {
                    QJsonArray nodes{QJsonObject{{"node_id", 9007}, {"available", true}, {"attributes", QJsonObject{
                        {"0/40/5", "主面板协议测试桥"},
                        {"1/29/0", QJsonArray{QJsonObject{{"deviceType", 0x0142}, {"revision", 1}}}},
                        {"1/29/1", QJsonArray{29, 57, 0x0551, 0x0553}},
                        {"1/57/5", "面板测试摄像头"}, {"1/57/17", true},
                        {"2/29/0", QJsonArray{QJsonObject{{"deviceType", 0x010a}, {"revision", 1}}}},
                        {"2/29/1", QJsonArray{29, 57, 6}},
                        {"2/57/5", "面板测试开关"}, {"2/57/17", true}, {"2/6/0", false},
                        {"3/29/0", QJsonArray{QJsonObject{{"deviceType", 0x0142}, {"revision", 1}}}},
                        {"3/29/1", QJsonArray{29, 57, 0x0551, 0x0553}},
                        {"3/57/5", "第二面板摄像头"}, {"3/57/17", true}
                    }}}};
                    if (!metadata.isEmpty()) {
                        auto node = nodes.first().toObject(); auto attrs = node.value("attributes").toObject();
                        auto servers = attrs.value("1/29/1").toArray(); servers.append(qint64(0xFFF1FC02u)); attrs.insert("1/29/1", servers);
                        for (auto it = metadata.begin(); it != metadata.end(); ++it) attrs.insert(it.key(), it.value());
                        node.insert("attributes", attrs); nodes.replace(0, node);
                    }
                    reply(request, nodes);
                } else if (command == "read_attribute" && !metadata.isEmpty()) {
                    QJsonObject values;
                    for (const auto &path : request.value("args").toObject().value("attribute_path").toArray())
                        if (metadata.contains(path.toString())) values.insert(path.toString(), metadata.value(path.toString()));
                    reply(request, values);
                } else if (command == "camera_get_capabilities") {
                    reply(request, QJsonObject{{"features", QJsonArray{"Video", "Audio"}}, {"privacy", QJsonObject{}},
                        {"video", QJsonObject{{"codecs", QJsonArray{"H264"}}, {"rate_distortion_points", QJsonArray{}}}},
                        {"audio", QJsonObject{{"codecs", QJsonArray{"OPUS"}}, {"channels", 1},
                            {"sample_rates", QJsonArray{48000}}, {"bit_depths", QJsonArray{16}}}},
                        {"snapshot", QJsonObject{{"capabilities", QJsonArray{}}}}, {"sessions", QJsonArray{}}});
                } else if (command == "camera_start_stream") {
                    activeKeys.insert(request.value("args").toObject().value("client_session_key").toString());
                    reply(request, QJsonObject{{"webrtc_session_id", 81}, {"talkback", false},
                        {"video", QJsonObject{{"provenance", "reused"}, {"stream_id", 701}}},
                        {"audio", QJsonObject{{"provenance", "reused"}, {"stream_id", 702}}}});
                } else if (command == "camera_stop_stream") {
                    activeKeys.remove(request.value("args").toObject().value("client_session_key").toString());
                    reply(request, QJsonObject{});
                } else if (command == "camera_release_stream") {
                    reply(request, QJsonValue(QJsonValue::Null));
                } else {
                    send({{"message_id", request.value("message_id")}, {"error_code", 8}, {"details", "No media is available in this fixture"}});
                }
            });
            send({{"schema_version", 14}, {"min_supported_schema_version", 11}, {"sdk_version", "Main panel fixture"}, {"fabric_id", "panel-fixture"}});
        });
    }
    QUrl url() const { return QUrl(QString("ws://127.0.0.1:%1/ws").arg(socket.serverPort())); }
    void send(const QJsonObject &message) {
        if (peer) peer->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }
    void reply(const QJsonObject &request, const QJsonValue &result) {
        send({{"message_id", request.value("message_id")}, {"result", result}});
    }
    void reportMetadata(const QJsonObject &snapshot) {
        const QString path = QStringLiteral("1/4294048770/0"); metadata.insert(path, snapshot);
        send({{"event", "attribute_updated"}, {"data", QJsonArray{9007, path, snapshot}}});
    }
    int count(const QString &command) const {
        int count = 0;
        for (const auto &request : requests) count += request.value("command") == command;
        return count;
    }
};

QVariant evaluate(QWebEnginePage *page, const QString &script) {
    auto result = std::make_shared<QVariant>();
    QEventLoop loop; QPointer<QEventLoop> guard(&loop);
    page->runJavaScript(script, [result, guard](const QVariant &value) { *result = value; if (guard) guard->quit(); });
    QTimer::singleShot(3000, &loop, &QEventLoop::quit); loop.exec(); return *result;
}
QJsonObject playerInfo(QWebEnginePage *page) {
    return QJsonDocument::fromJson(evaluate(page, QStringLiteral(
        "window.MatterDeskCamera ? JSON.stringify(Object.assign(window.MatterDeskCamera.inspect(), "
        "{objectFit:getComputedStyle(document.getElementById('video')).objectFit,"
        "elementWidth:document.getElementById('video').getBoundingClientRect().width,"
        "elementHeight:document.getElementById('video').getBoundingClientRect().height,"
        "viewportWidth:innerWidth,viewportHeight:innerHeight})) : ''")).toString().toUtf8()).object();
}
QPushButton *navigation(MainWindow &window, const QString &page) {
    for (auto *button : window.findChildren<QPushButton *>("navigation"))
        if (button->property("page").toString() == page) return button;
    return nullptr;
}
DeviceCard *cameraCard(MainWindow &window) {
    for (auto *card : window.findChildren<DeviceCard *>()) if (card->device().hasCamera) return card;
    return nullptr;
}
struct TopLevelGeometry {
    QString stage;
    QPoint position;
    QSize size;
    QRect frame;
    bool maximized;
    quintptr handle;
    WId nativeId;
};
TopLevelGeometry topLevelGeometry(MainWindow &window, const QString &stage) {
    TopLevelGeometry result{stage, window.pos(), window.size(), window.frameGeometry(), window.isMaximized(),
        reinterpret_cast<quintptr>(window.windowHandle()), window.internalWinId()};
    qInfo().noquote() << "Top-level geometry:" << stage << "position" << result.position << "size" << result.size
        << "frame" << result.frame << "maximized" << result.maximized
        << "QWindow" << QString::number(result.handle, 16) << "nativeId" << QString::number(result.nativeId, 16);
    return result;
}
class TopLevelGeometryTrace : public QObject {
public:
    explicit TopLevelGeometryTrace(MainWindow &window) : m_window(window) { window.installEventFilter(this); }
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == &m_window && (event->type() == QEvent::Move || event->type() == QEvent::Resize
            || event->type() == QEvent::WinIdChange || event->type() == QEvent::WindowStateChange
            || event->type() == QEvent::PlatformSurface))
            topLevelGeometry(m_window, QStringLiteral("event-%1").arg(int(event->type())));
        return false;
    }
private:
    MainWindow &m_window;
};
}

class MainPanelCameraTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("MatterDeskTests");
        QCoreApplication::setApplicationName("MainPanelCamera-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_settings.isValid());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, m_settings.path());
        m_appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    void init() {
        QSettings().clear();
        if (QDir(m_appData).exists()) QVERIFY(QDir(m_appData).removeRecursively());
        QTest::failOnWarning(QRegularExpression(".*(Release of profile requested|WebEnginePage still not deleted).*"));
    }
    void cleanupTestCase() {
        QSettings().clear();
        if (QDir(m_appData).exists()) QVERIFY(QDir(m_appData).removeRecursively());
    }

    // This must remain the first real test: the first QWebEngineView can
    // switch texture composition and recreate the top-level surface. An
    // already initialized WebEngine hides that
    // first-open path, so exercise it before any other camera test.
    void openingEmbeddedCameraPreservesTopLevelGeometry_data() {
        QTest::addColumn<bool>("maximized");
        QTest::newRow("normal-window") << false;
        QTest::newRow("maximized-window") << true;
    }
    void openingEmbeddedCameraPreservesTopLevelGeometry() {
        QFETCH(bool, maximized);
        if (maximized && QGuiApplication::platformName() != QStringLiteral("xcb"))
            QSKIP("Maximized native-window regression requires an X11 window manager; run under private Xvfb + Openbox");
        PanelFixture fixture;
        MainWindow window;
        TopLevelGeometryTrace trace(window);
        connectFixture(window, fixture);
        window.resize(1100, 760);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.move(240, 160);
        QTRY_COMPARE(window.pos(), QPoint(240, 160));
        if (maximized) {
            window.showMaximized();
            QTRY_VERIFY(window.isMaximized());
        }
        auto *cameraNav = navigation(window, "camera"); QVERIFY(cameraNav);
        QTest::mouseClick(cameraNav, Qt::LeftButton);
        auto *card = cameraCard(window); QVERIFY(card);
        QTRY_VERIFY(card->isVisible());
        auto *snapshot = card->findChild<QAbstractButton *>("cameraSnapshot"); QVERIFY(snapshot);
        QTest::qWait(300); // Let initial WM decoration/layout requests settle.
        const auto before = topLevelGeometry(window, QStringLiteral("before-first-camera"));
        QVERIFY(before.nativeId != 0);
        QVERIFY(window.windowHandle());
        QPointer<QWindow> nativeWindow(window.windowHandle());
        if (!maximized) QVERIFY(before.position.x() > 0 && before.position.y() > 0);
        QList<TopLevelGeometry> checkpoints;
        for (int opening = 1; opening <= 2; ++opening) {
            QTest::mouseClick(snapshot, Qt::LeftButton);
            checkpoints.append(topLevelGeometry(window, QStringLiteral("open-%1-synchronous").arg(opening)));
            QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_start_stream"), opening, 15000);
            QPointer<CameraWindow> player(window.findChild<CameraWindow *>("mainCameraPlayer")); QVERIFY(player);
            auto *view = player->findChild<QWebEngineView *>("cameraVideo"); QVERIFY(view);
            QTRY_VERIFY_WITH_TIMEOUT(!playerInfo(view->page()).isEmpty(), 10000);
            QTest::qWait(300);
            checkpoints.append(topLevelGeometry(window, QStringLiteral("open-%1-asynchronous-ready").arg(opening)));
            auto *back = player->findChild<QPushButton *>("cameraBackButton"); QVERIFY(back);
            QTest::mouseClick(back, Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(player.isNull(), 5000);
            QTRY_VERIFY(card->isVisible());
            QTest::qWait(100);
            checkpoints.append(topLevelGeometry(window, QStringLiteral("return-%1").arg(opening)));
        }
        QTRY_COMPARE(fixture.count("camera_stop_stream"), 2);
        QTRY_COMPARE(fixture.count("camera_release_stream"), 4);
        QVERIFY(fixture.activeKeys.isEmpty());
        window.client()->disconnectFromServer();
        // Check at the end so a failing first-open build still leaves a full
        // first-open/return/reopen trace for diagnosis.
        for (const auto &current : checkpoints) {
            QVERIFY2(current.position == before.position, qPrintable(current.stage + QStringLiteral(": main window moved")));
            QVERIFY2(current.size == before.size, qPrintable(current.stage + QStringLiteral(": main window resized")));
            QVERIFY2(current.frame == before.frame, qPrintable(current.stage + QStringLiteral(": window-manager frame changed")));
            QVERIFY2(current.maximized == before.maximized, qPrintable(current.stage + QStringLiteral(": maximized state changed")));
            QVERIFY2(current.handle == before.handle, qPrintable(current.stage + QStringLiteral(": QWindow was replaced")));
            QVERIFY2(current.nativeId == before.nativeId, qPrintable(current.stage + QStringLiteral(": native window ID was replaced")));
        }
        QVERIFY(nativeWindow);
    }

    void snapshotOpensInRightPanelAndReturnPreservesDashboard() {
        PanelFixture fixture; QVERIFY(fixture.socket.isListening()); QVERIFY(fixture.socket.serverPort() != 5580);
        MainWindow window; connectFixture(window, fixture); window.show();
        auto *cameraNav = navigation(window, "camera"); QVERIFY(cameraNav); QTest::mouseClick(cameraNav, Qt::LeftButton);
        auto *search = window.findChild<QLineEdit *>("deviceSearch"); QVERIFY(search); search->setText("面板测试");
        auto *filter = window.findChild<QComboBox *>("deviceFilter"); QVERIFY(filter); filter->setCurrentIndex(1);
        auto *card = cameraCard(window); QVERIFY(card); QTRY_VERIFY(card->isVisible());
        QSet<QWidget *> topLevels;
        for (auto *widget : QApplication::topLevelWidgets()) if (widget->isVisible()) topLevels.insert(widget);
        auto *snapshot = card->findChild<QAbstractButton *>("cameraSnapshot"); QVERIFY(snapshot);
        QTest::mouseClick(snapshot, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_start_stream"), 1, 15000);
        auto *player = window.findChild<CameraWindow *>("mainCameraPlayer"); QVERIFY(player);
        auto *panels = window.findChild<QStackedWidget *>("mainPanels"); QVERIFY(panels);
        QVERIFY(!player->isWindow()); QCOMPARE(player->window(), &window); QCOMPARE(panels->currentWidget(), player);
        QCOMPARE(player->parentWidget(), panels); QVERIFY(player->isVisible());
        QVERIFY(window.findChild<QWidget *>("sidebar")->isVisible());
        QVERIFY(!window.findChild<QWidget *>("main")->isVisible());
        for (auto *widget : QApplication::topLevelWidgets()) if (widget->isVisible()) QVERIFY(topLevels.contains(widget));
        auto *view = player->findChild<QWebEngineView *>("cameraVideo"); QVERIFY(view);
        QTRY_VERIFY_WITH_TIMEOUT(!playerInfo(view->page()).isEmpty(), 10000);
        QVERIFY(!playerInfo(view->page()).value("micActive").toBool());
        auto *back = player->findChild<QPushButton *>("cameraBackButton"); QVERIFY(back);
        QVERIFY(back->text().contains("返回")); QPointer<CameraWindow> guarded(player);
        back->click(); QCOMPARE(panels->currentWidget(), window.findChild<QWidget *>("main"));
        QTRY_VERIFY_WITH_TIMEOUT(guarded.isNull(), 5000);
        QCOMPARE(panels->currentWidget(), window.findChild<QWidget *>("main"));
        QVERIFY(cameraNav->isChecked()); QCOMPARE(search->text(), QStringLiteral("面板测试")); QCOMPARE(filter->currentIndex(), 1);
        QTRY_VERIFY(card->isVisible()); verifyRetirement(fixture);
        window.client()->disconnectFromServer();
    }

    void videoFitsEntirePanelForPortraitAndLandscapeSources() {
        PanelFixture fixture; MainWindow window; connectFixture(window, fixture); window.show();
        auto *card = cameraCard(window); QVERIFY(card); card->cameraRequested(card->device());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_start_stream"), 1, 15000);
        auto *player = window.findChild<CameraWindow *>("mainCameraPlayer"); QVERIFY(player);
        auto *view = player->findChild<QWebEngineView *>("cameraVideo"); QVERIFY(view);
        auto *panels = window.findChild<QStackedWidget *>("mainPanels"); QVERIFY(panels);
        QTRY_COMPARE(playerInfo(view->page()).value("objectFit").toString(), QStringLiteral("contain"));
        if (qEnvironmentVariable("MATTERDESK_REQUIRE_CAMERA_CODECS") == QStringLiteral("1")) {
            const auto codecs = QJsonDocument::fromJson(evaluate(view->page(), QStringLiteral(
                "(()=>{const codecs=k=>(globalThis.RTCRtpReceiver?.getCapabilities?.(k)?.codecs||[])"
                ".map(c=>String(c.mimeType||'').toLowerCase());"
                "return JSON.stringify({video:codecs('video'),audio:codecs('audio')});})()"
            )).toString().toUtf8()).object();
            QVERIFY2(codecs.value("video").toArray().contains(QStringLiteral("video/h264")),
                     "Packaged WebEngine must advertise the camera's H.264 WebRTC decoder");
            QVERIFY2(codecs.value("audio").toArray().contains(QStringLiteral("audio/opus")),
                     "Packaged WebEngine must advertise the camera's Opus WebRTC decoder");
            qInfo("Packaged WebRTC codec capabilities include H.264 and Opus");
        }
        // Feed only local generated frames through the real video element.
        // Alternate aspect ratios and viewport shapes to catch stale sizing or
        // inherited cover styling; no screenshots or camera stream are used.
        for (const auto &source : {QSize(640, 360), QSize(240, 480)}) {
            const auto script = QStringLiteral(
                "(()=>{if(window.panelTestStream)window.panelTestStream.getTracks().forEach(t=>t.stop());"
                "const c=document.createElement('canvas');c.width=%1;c.height=%2;"
                "const x=c.getContext('2d');x.fillStyle='#245caf';x.fillRect(0,0,c.width,c.height);"
                "x.fillStyle='#51dcb8';x.fillRect(0,0,12,c.height);x.fillRect(c.width-12,0,12,c.height);"
                "x.fillStyle='#f4ca72';x.fillRect(0,0,c.width,12);x.fillRect(0,c.height-12,c.width,12);"
                "x.fillStyle='white';x.font='20px sans-serif';x.fillText('Synthetic %1 x %2',24,45);"
                "window.panelTestStream=c.captureStream(10);const v=document.getElementById('video');"
                "v.srcObject=window.panelTestStream;v.muted=true;v.play().catch(()=>{});return true;})()")
                .arg(source.width()).arg(source.height());
            QVERIFY(evaluate(view->page(), script).toBool());
            QTRY_COMPARE_WITH_TIMEOUT(playerInfo(view->page()).value("videoWidth").toInt(), source.width(), 5000);
            QTRY_COMPARE(playerInfo(view->page()).value("videoHeight").toInt(), source.height());
            for (const auto &size : {QSize(1460, 800), QSize(960, 1100)}) {
                window.resize(size);
                QTRY_COMPARE(player->geometry(), panels->contentsRect());
                QTRY_COMPARE(view->geometry(), player->contentsRect());
                QTRY_COMPARE(playerInfo(view->page()).value("elementWidth").toInt(), view->width());
                const auto info = playerInfo(view->page());
                QCOMPARE(info.value("elementHeight").toInt(), view->height());
                QCOMPARE(info.value("objectFit").toString(), QStringLiteral("contain"));
                QCOMPARE(info.value("viewportWidth").toInt(), view->width());
                QCOMPARE(info.value("viewportHeight").toInt(), view->height());
                const double scale = qMin(double(view->width()) / source.width(), double(view->height()) / source.height());
                QVERIFY(source.width() * scale <= view->width() + .01);
                QVERIFY(source.height() * scale <= view->height() + .01);
                const auto previewDir = qEnvironmentVariable("MATTERDESK_PANEL_PREVIEW_DIR");
                if (!previewDir.isEmpty()) {
                    QVERIFY(QDir().mkpath(previewDir)); QTest::qWait(200);
                    QVERIFY(window.grab().save(QDir(previewDir).filePath(QString("main-panel-%1x%2-%3x%4.png")
                        .arg(source.width()).arg(source.height()).arg(size.width()).arg(size.height()))));
                }
            }
        }
        evaluate(view->page(), "window.panelTestStream.getTracks().forEach(t=>t.stop());true");
        QPointer<CameraWindow> guarded(player); player->close(); QTRY_VERIFY_WITH_TIMEOUT(guarded.isNull(), 5000);
        verifyRetirement(fixture); window.client()->disconnectFromServer();
    }

    void switchingCameraDestroysOldPlayerAndKeepsOnlyOnePeer() {
        PanelFixture fixture; MainWindow window; connectFixture(window, fixture); window.show();
        auto *firstCard = cameraCard(window); QVERIFY(firstCard);
        firstCard->cameraRequested(firstCard->device());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_start_stream"), 1, 15000);
        QPointer<CameraWindow> first(window.findChild<CameraWindow *>("mainCameraPlayer")); QVERIFY(first);
        QPointer<QWebEnginePage> oldPage(first->findChild<QWebEngineView *>("cameraVideo")->page());
        QPointer<QWebEngineProfile> oldProfile(oldPage->profile());
        // Return and immediately reopen while WebEngine's asynchronous close
        // callback has not run. The old player must retire before replacement.
        auto *back = first->findChild<QPushButton *>("cameraBackButton"); QVERIFY(back); back->click();
        QCOMPARE(window.findChild<QStackedWidget *>("mainPanels")->currentWidget(), window.findChild<QWidget *>("main"));
        QVERIFY(first); firstCard->cameraRequested(firstCard->device());
        QTRY_VERIFY_WITH_TIMEOUT(first.isNull(), 5000); QVERIFY(oldPage.isNull()); QVERIFY(oldProfile.isNull());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_start_stream"), 2, 15000);
        QTRY_COMPARE(fixture.count("camera_stop_stream"), 1); QTRY_COMPARE(fixture.count("camera_release_stream"), 2);
        QPointer<CameraWindow> reopened(window.findChild<CameraWindow *>("mainCameraPlayer")); QVERIFY(reopened);
        QCOMPARE(window.findChildren<CameraWindow *>().size(), 1);
        oldPage = reopened->findChild<QWebEngineView *>("cameraVideo")->page(); oldProfile = oldPage->profile();
        DeviceCard *secondCard = nullptr;
        for (auto *card : window.findChildren<DeviceCard *>()) if (card->device().endpointId == 3) secondCard = card;
        QVERIFY(secondCard); secondCard->cameraRequested(secondCard->device());
        QTRY_VERIFY_WITH_TIMEOUT(reopened.isNull(), 5000); QVERIFY(oldPage.isNull()); QVERIFY(oldProfile.isNull());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_start_stream"), 3, 15000);
        QTRY_COMPARE(fixture.count("camera_stop_stream"), 2);
        QTRY_COMPARE(fixture.count("camera_release_stream"), 4);
        QCOMPARE(fixture.activeKeys.size(), 1);
        const auto players = window.findChildren<CameraWindow *>(); QCOMPARE(players.size(), 1);
        auto *second = players.first(); QVERIFY(second->isVisible()); QVERIFY(!second->isWindow());
        QCOMPARE(window.findChild<QStackedWidget *>("mainPanels")->currentWidget(), second);
        QPointer<CameraWindow> secondGuard(second); second->close(); QTRY_VERIFY_WITH_TIMEOUT(secondGuard.isNull(), 5000);
        QTRY_COMPARE(fixture.count("camera_stop_stream"), 3);
        QTRY_COMPARE(fixture.count("camera_release_stream"), 6); QVERIFY(fixture.activeKeys.isEmpty());
        QSet<int> stoppedEndpoints; QSet<QString> startKeys, stopKeys;
        for (const auto &request : fixture.requests) {
            const auto args = request.value("args").toObject();
            if (request.value("command") == "camera_start_stream") startKeys.insert(args.value("client_session_key").toString());
            if (request.value("command") == "camera_stop_stream") {
                stoppedEndpoints.insert(args.value("endpoint_id").toInt()); stopKeys.insert(args.value("client_session_key").toString());
            }
        }
        QCOMPARE(startKeys.size(), 3); QCOMPARE(startKeys, stopKeys); QCOMPARE(stoppedEndpoints, (QSet<int>{1, 3}));
        window.client()->disconnectFromServer();
    }

    void cloudMetadataUpdatesCardsWithoutReplacingPlayerOrLocalOrganization() {
        PanelFixture fixture;
        const QString prefix = QStringLiteral("1/4294048770/");
        const auto snapshot = [](int revision, const QString &name, const QString &roomId, const QString &roomName) {
            return QJsonObject{{"0", QString(64, QLatin1Char('a'))}, {"1", "12345678-1234-4234-8234-123456789abc"},
                {"2", revision}, {"3", name}, {"4", QString(64, QLatin1Char('b'))}, {"5", "Cloud home"},
                {"6", roomId}, {"7", roomName}, {"8", QStringLiteral("2026-10-24T08:00:00.%1Z").arg(revision, 3, 10, QLatin1Char('0'))}};
        };
        fixture.metadata = {{prefix + "0", snapshot(1, "Cloud camera", "10", "Cloud room")},
            {prefix + "1", QJsonArray{QJsonObject{{"0", "10"}, {"1", "Cloud room"}}, QJsonObject{{"0", "20"}, {"1", "Study"}}}},
            {prefix + "2", QJsonObject{{"0", true}, {"1", false}, {"2", false}, {"3", true}}},
            {prefix + "3", true}, {prefix + "4", true}, {prefix + "5", false}, {prefix + "6", ""}};
        MainWindow window; connectFixture(window, fixture); window.show();
        DeviceCard *card = nullptr;
        for (auto *candidate : window.findChildren<DeviceCard *>()) if (candidate->device().endpointId == 1) card = candidate;
        QVERIFY(card); QPointer<DeviceCard> originalCard(card);
        auto *name = card->findChild<QLabel *>("deviceName"); QVERIFY(name);
        auto *room = card->findChild<QLabel *>("mutedSmall"); QVERIFY(room);
        auto *manager = window.client()->cameraMetadata(); const auto device = card->device();
        QTRY_VERIFY(manager->state(device).known);
        QTRY_COMPARE(name->text(), QStringLiteral("Cloud camera"));
        QCOMPARE(room->text(), QStringLiteral("米家 · Cloud home / Cloud room"));
        QCOMPARE(fixture.count("device_command"), 0);
        card->cameraRequested(card->device());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_start_stream"), 1, 15000);
        QPointer<CameraWindow> player(window.findChild<CameraWindow *>("mainCameraPlayer")); QVERIFY(player);
        auto *view = player->findChild<QWebEngineView *>("cameraVideo"); QVERIFY(view);
        QPointer<QWebEnginePage> page(view->page());
        QTRY_VERIFY_WITH_TIMEOUT(!playerInfo(page).isEmpty(), 10000);
        QVERIFY(evaluate(page, "window.metadataSameDocument = 'preserved'; true").toBool());
        auto *panels = window.findChild<QStackedWidget *>("mainPanels"); QVERIFY(panels);

        fixture.reportMetadata(snapshot(2, "Cloud renamed", "20", "Study"));
        QTRY_COMPARE(manager->state(device).snapshot.revision, quint64(2));
        QTRY_COMPARE(name->text(), QStringLiteral("Cloud renamed"));
        QCOMPARE(room->text(), QStringLiteral("米家 · Cloud home / Study"));
        QVERIFY(originalCard && player && page); QCOMPARE(window.findChild<CameraWindow *>("mainCameraPlayer"), player.data());
        QCOMPARE(view->page(), page.data()); QCOMPARE(panels->currentWidget(), player.data());
        QCOMPARE(evaluate(page, "window.metadataSameDocument").toString(), QStringLiteral("preserved"));

        const QString localRoom = window.home()->saveRoom({{"name", "Local room"}}); QVERIFY(!localRoom.isEmpty());
        QVERIFY(window.home()->setAccessory(device.key, {{"name", "Local alias"}, {"roomId", localRoom}}));
        QTRY_COMPARE(name->text(), QStringLiteral("Local alias")); QCOMPARE(room->text(), QStringLiteral("Local room"));
        fixture.reportMetadata(snapshot(3, "Another cloud name", "10", "Cloud room"));
        QTRY_COMPARE(manager->state(device).snapshot.revision, quint64(3));
        QCOMPARE(manager->state(device).snapshot.name, QStringLiteral("Another cloud name"));
        QCOMPARE(name->text(), QStringLiteral("Local alias")); QCOMPARE(room->text(), QStringLiteral("Local room"));
        QCOMPARE(window.home()->accessory(device.key).value("name").toString(), QStringLiteral("Local alias"));
        QCOMPARE(window.home()->accessory(device.key).value("roomId").toString(), localRoom);
        auto incomplete = snapshot(4, "", "", ""); incomplete.insert("5", ""); incomplete.insert("8", "");
        fixture.metadata.insert(prefix + "1", QJsonArray{}); fixture.metadata.insert(prefix + "4", false);
        fixture.metadata.insert(prefix + "6", "metadata_incomplete"); fixture.reportMetadata(incomplete);
        QTRY_COMPARE(manager->state(device).lastError, QStringLiteral("metadata_incomplete"));
        QVERIFY(manager->state(device).snapshot.name.isEmpty());
        QCOMPARE(name->text(), QStringLiteral("Local alias")); QCOMPARE(room->text(), QStringLiteral("Local room"));
        QVERIFY(window.home()->setAccessory(device.key, {{"name", ""}, {"roomId", ""}}));
        QTRY_COMPARE(name->text(), QStringLiteral("摄像头")); QCOMPARE(room->text(), QStringLiteral("米家信息暂不可用"));
        QVERIFY(originalCard && player && page); QCOMPARE(window.findChild<CameraWindow *>("mainCameraPlayer"), player.data());
        QCOMPARE(view->page(), page.data()); QCOMPARE(panels->currentWidget(), player.data());
        QCOMPARE(evaluate(page, "window.metadataSameDocument").toString(), QStringLiteral("preserved"));
        QCOMPARE(fixture.count("camera_start_stream"), 1); QCOMPARE(fixture.count("camera_stop_stream"), 0);
        QCOMPARE(fixture.count("camera_release_stream"), 0); QCOMPARE(fixture.activeKeys.size(), 1);
        QCOMPARE(fixture.count("device_command"), 0); QCOMPARE(fixture.count("write_attribute"), 0);
        QVERIFY(!playerInfo(page).value("micActive").toBool());
        player->close(); QTRY_VERIFY_WITH_TIMEOUT(player.isNull(), 5000); QVERIFY(page.isNull());
        verifyRetirement(fixture); window.client()->disconnectFromServer();
    }

    void leavingCameraRetiresSession_data() {
        QTest::addColumn<bool>("removeNode");
        QTest::newRow("navigate to scenes") << false;
        QTest::newRow("camera removed") << true;
    }
    void leavingCameraRetiresSession() {
        QFETCH(bool, removeNode);
        PanelFixture fixture; MainWindow window; connectFixture(window, fixture); window.show();
        auto *card = cameraCard(window); QVERIFY(card); card->cameraRequested(card->device());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_start_stream"), 1, 15000);
        QPointer<CameraWindow> player(window.findChild<CameraWindow *>("mainCameraPlayer")); QVERIFY(player);
        QPointer<QWebEnginePage> page(player->findChild<QWebEngineView *>("cameraVideo")->page());
        QPointer<QWebEngineProfile> profile(page->profile());
        if (removeNode) fixture.send({{"event", "node_removed"}, {"data", 9007}});
        else { auto *nav = navigation(window, "scenes"); QVERIFY(nav); QTest::mouseClick(nav, Qt::LeftButton); }
        QTRY_VERIFY_WITH_TIMEOUT(player.isNull(), 5000); QVERIFY(page.isNull()); QVERIFY(profile.isNull());
        auto *panels = window.findChild<QStackedWidget *>("mainPanels"); QVERIFY(panels);
        QCOMPARE(panels->currentWidget(), window.findChild<QWidget *>("main"));
        QVERIFY(window.findChild<QWidget *>("sidebar")->isVisible());
        if (!removeNode) {
            QVERIFY(navigation(window, "scenes")->isChecked());
            QCOMPARE(window.findChild<QStackedWidget *>("homePages")->currentIndex(), 1);
        }
        verifyRetirement(fixture); window.client()->disconnectFromServer();
    }

    void disconnectAndWindowDestructionDisposePages_data() {
        QTest::addColumn<bool>("destroyMain");
        QTest::newRow("controller disconnected") << false;
        QTest::newRow("main window destroyed") << true;
    }
    void disconnectAndWindowDestructionDisposePages() {
        QFETCH(bool, destroyMain);
        PanelFixture fixture; auto window = std::make_unique<MainWindow>(); connectFixture(*window, fixture); window->show();
        auto *card = cameraCard(*window); QVERIFY(card); card->cameraRequested(card->device());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_start_stream"), 1, 15000);
        QPointer<CameraWindow> player(window->findChild<CameraWindow *>("mainCameraPlayer")); QVERIFY(player);
        QPointer<QWebEnginePage> page(player->findChild<QWebEngineView *>("cameraVideo")->page());
        QPointer<QWebEngineProfile> profile(page->profile());
        if (destroyMain) window.reset(); else window->client()->disconnectFromServer();
        QTRY_VERIFY_WITH_TIMEOUT(player.isNull(), 5000); QVERIFY(page.isNull()); QVERIFY(profile.isNull());
        // A closed controller cannot acknowledge subsequent releases. Assert
        // local media/profile lifetime here; connected exits above assert the
        // complete stop-acknowledgement -> release protocol separately.
        if (!destroyMain) {
            auto *panels = window->findChild<QStackedWidget *>("mainPanels"); QVERIFY(panels);
            QCOMPARE(panels->currentWidget(), window->findChild<QWidget *>("main"));
            QVERIFY(window->findChild<QWidget *>("sidebar")->isVisible());
            QVERIFY(!window->client()->isConnected());
        } else QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_stop_stream"), 1, 5000);
    }

private:
    void connectFixture(MainWindow &window, PanelFixture &fixture) {
        QVERIFY(fixture.socket.isListening()); QVERIFY(fixture.socket.serverPort() != 5580);
        QCOMPARE(fixture.url().host(), QStringLiteral("127.0.0.1"));
        window.client()->connectToServer(fixture.url()); QTRY_VERIFY(window.client()->isConnected());
        QTRY_COMPARE(window.client()->devices().size(), 3);
    }
    void verifyRetirement(const PanelFixture &fixture) {
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_stop_stream"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_release_stream"), 2, 5000);
        QVERIFY(fixture.activeKeys.isEmpty());
        QString key; bool stopped = false; QSet<QString> released;
        for (const auto &request : fixture.requests) {
            const auto command = request.value("command").toString();
            const auto args = request.value("args").toObject();
            QVERIFY(!command.contains("evict"));
            if (command == "camera_start_stream") {
                key = args.value("client_session_key").toString(); QVERIFY(!key.isEmpty());
                QVERIFY(!args.value("allow_eviction").toBool());
            } else if (command == "camera_stop_stream") {
                QCOMPARE(args.value("client_session_key").toString(), key); stopped = true;
            } else if (command == "camera_release_stream") {
                QVERIFY(stopped); QCOMPARE(args.value("node_id").toInteger(), qint64(9007));
                QCOMPARE(args.value("endpoint_id").toInt(), 1);
                const auto kind = args.value("kind").toString(); released.insert(kind);
                QCOMPARE(args.value("stream_id").toInt(), kind == "video" ? 701 : 702);
            }
        }
        QCOMPARE(released, (QSet<QString>{"video", "audio"}));
    }
    QTemporaryDir m_settings;
    QString m_appData;
};

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", qgetenv("QTWEBENGINE_CHROMIUM_FLAGS") + " --disable-gpu");
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    app.setStyle("Fusion"); app.setFont(QFont("Noto Sans CJK SC", 10)); app.setStyleSheet(homeStyle());
    MainPanelCameraTest test; return QTest::qExec(&test, argc, argv);
}

#include "tst_mainpanelcamera.moc"
