#include "devicecard.h"
#include "mainwindow.h"

#include <QAbstractButton>
#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>

namespace {
const QColor Magenta(220, 30, 110);
const QColor Cobalt(30, 100, 225);

QJsonArray fixtureNodes() {
    const QJsonObject attributes{
        {"0/40/5", "快照界面测试桥"},
        {"1/29/0", QJsonArray{QJsonObject{{"deviceType", 0x0142}, {"revision", 1}}}},
        {"1/29/1", QJsonArray{29, 57, 0x0551, 0x0553}},
        {"1/57/5", "测试摄像头（无真实视频源）"}, {"1/57/17", true},
        {"2/29/0", QJsonArray{QJsonObject{{"deviceType", 0x010a}, {"revision", 1}}}},
        {"2/29/1", QJsonArray{29, 57, 6}},
        {"2/57/5", "测试插座"}, {"2/57/17", true}, {"2/6/0", false}
    };
    return {QJsonObject{{"node_id", 9001}, {"available", true}, {"attributes", attributes}}};
}

class SnapshotFixtureServer : public QObject {
public:
    QWebSocketServer server{QStringLiteral("Snapshot UI fixture"), QWebSocketServer::NonSecureMode};
    QWebSocket *peer = nullptr;
    QList<QJsonObject> requests;
    QList<QJsonObject> pendingSnapshots;
    int snapshots = 0;
    bool holdSnapshots = false;
    bool privacyError = false;
    QColor color = Magenta;
    QByteArray lastEncodedImage;

    SnapshotFixtureServer() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QWebSocketServer::newConnection, this, [this] {
            peer = server.nextPendingConnection(); peer->setParent(this);
            connect(peer, &QWebSocket::textMessageReceived, this, [this](const QString &message) {
                const auto request = QJsonDocument::fromJson(message.toUtf8()).object();
                requests.append(request);
                const QString command = request.value("command").toString();
                if (command == "start_listening" || command == "get_nodes") reply(request, fixtureNodes());
                else if (command == "camera_get_capabilities") reply(request, capabilities());
                else if (command == "camera_snapshot") {
                    ++snapshots;
                    if (holdSnapshots) pendingSnapshots.append(request);
                    else answerSnapshot(request);
                } else if (command == "camera_release_stream") reply(request, QJsonValue(QJsonValue::Null));
                else send({{"message_id", request.value("message_id")}, {"error_code", 8}, {"details", "No real media is available in this UI fixture"}});
            });
            send({{"schema_version", 14}, {"min_supported_schema_version", 11}, {"sdk_version", "Snapshot UI fixture"}});
        });
    }
    QUrl url() const { return QUrl(QString("ws://127.0.0.1:%1/ws").arg(server.serverPort())); }
    QJsonObject capabilities() const {
        return {
            {"features", QJsonArray{"Video", "Audio", "Speaker", "Snapshot"}},
            {"privacy", QJsonObject{{"hard_mode_on", false}, {"soft_livestream_mode_enabled", false}}},
            // Only a 1080p native capability: the client must ask for a thumbnail
            // using resolution, rather than exclude this capability with a ceiling.
            {"snapshot", QJsonObject{{"capabilities", QJsonArray{QJsonObject{
                {"resolution", QJsonObject{{"width", 1920}, {"height", 1080}}},
                {"max_frame_rate", 1}, {"image_codec", "JPEG"},
                {"requires_encoded_pixels", false}, {"requires_hardware_encoder", false}
            }}}}}
        };
    }
    void completeSnapshots() {
        const auto pending = pendingSnapshots; pendingSnapshots.clear();
        for (const auto &request : pending) answerSnapshot(request);
    }
    void answerSnapshot(const QJsonObject &request) {
        if (privacyError) {
            const QString details = QString::fromUtf8(QJsonDocument(QJsonObject{
                {"message", "Snapshot blocked by privacy"}, {"modes", QJsonArray{"hard_mode_on"}}, {"device_status", 126}
            }).toJson(QJsonDocument::Compact));
            send({{"message_id", request.value("message_id")}, {"error_code", 106}, {"details", details}});
            return;
        }
        QImage image(96, 54, QImage::Format_RGB32); image.fill(color);
        QByteArray jpeg; QBuffer buffer(&jpeg); buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "JPEG", 90); lastEncodedImage = jpeg.toBase64();
        reply(request, QJsonObject{
            {"data", QString::fromLatin1(lastEncodedImage)}, {"codec", "JPEG"},
            {"resolution", QJsonObject{{"width", image.width()}, {"height", image.height()}}},
            {"degraded", false}, {"stream_id", 77}, {"provenance", "adopted"}
        });
    }
    void reply(const QJsonObject &request, const QJsonValue &result) {
        send({{"message_id", request.value("message_id")}, {"result", result}});
    }
    void send(const QJsonObject &message) {
        if (peer) peer->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }
    int count(const QString &command) const {
        int result = 0;
        for (const auto &request : requests) result += request.value("command").toString() == command;
        return result;
    }
};

DeviceCard *cameraCard(MainWindow &window) {
    for (auto *card : window.findChildren<DeviceCard *>()) if (card->device().hasCamera && !card->isHidden()) return card;
    return nullptr;
}
QPushButton *navigation(MainWindow &window, const QString &name) {
    for (auto *button : window.findChildren<QPushButton *>("navigation"))
        if (button->text().contains(name)) return button;
    return nullptr;
}
QPushButton *globalRefresh(MainWindow &window) {
    for (auto *button : window.findChildren<QPushButton *>("secondary"))
        if (button->text().contains("刷新")) return button;
    return nullptr;
}
bool showsColor(QWidget *preview, const QColor &color) {
    if (!preview || preview->width() <= 0 || preview->height() <= 0) return false;
    const QImage image = preview->grab().toImage().convertToFormat(QImage::Format_RGB32);
    int matches = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            if (qAbs(qRed(row[x]) - color.red()) < 42
                && qAbs(qGreen(row[x]) - color.green()) < 42
                && qAbs(qBlue(row[x]) - color.blue()) < 42) ++matches;
    }
    return matches > 128;
}
}

class SnapshotUiTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("MatterDeskTests");
        QCoreApplication::setApplicationName("SnapshotUi");
        QVERIFY(m_settings.isValid());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, m_settings.path());
    }

    void visibleCameraDisplaysJpegAndManualRefreshIsThrottled() {
        SnapshotFixtureServer fixture; QVERIFY(fixture.server.isListening());
        MainWindow window; window.show(); window.client()->connectToServer(fixture.url());
        QTRY_VERIFY(window.client()->isConnected());
        QTRY_VERIFY(cameraCard(window));
        auto *card = cameraCard(window);
        auto *preview = card->findChild<QAbstractButton *>("cameraSnapshot"); QVERIFY(preview);
        auto *refresh = card->findChild<QPushButton *>("snapshotRefresh"); QVERIFY(refresh);
        QTRY_VERIFY(showsColor(preview, Magenta));
        QCOMPARE(fixture.snapshots, 1);
        QCOMPARE(fixture.count("camera_start_stream"), 0);
        QVERIFY(card->findChild<QLabel *>("snapshotTime")->text().contains("更新"));
        QVERIFY(!card->findChild<QLabel *>("snapshotStatus")->text().contains("旧快照"));
        const auto shot = fixture.requests.last().value("args").toObject();
        QCOMPARE(shot.value("codec").toString(), QStringLiteral("JPEG"));
        QVERIFY(!shot.contains("max_resolution"));
        QVERIFY(shot.value("resolution").toObject().value("width").toInt() <= 640);

        fixture.color = Cobalt; fixture.holdSnapshots = true;
        QTest::mouseClick(refresh, Qt::LeftButton);
        QTRY_COMPARE(fixture.snapshots, 2);
        QVERIFY(!refresh->isEnabled());
        for (int i = 0; i < 8; ++i) QTest::mouseClick(refresh, Qt::LeftButton);
        QTest::qWait(80); QCOMPARE(fixture.snapshots, 2);
        fixture.holdSnapshots = false; fixture.completeSnapshots();
        QTRY_VERIFY(showsColor(preview, Cobalt));
        QTRY_VERIFY(refresh->isEnabled());
        QTest::mouseClick(refresh, Qt::LeftButton);
        QTest::qWait(150); QCOMPARE(fixture.snapshots, 2);
        QVERIFY(!card->findChild<QLabel *>("snapshotStatus")->text().contains("旧快照"));
    }

    void hiddenCameraDoesNotRequestAndReconnectClearsThenReloadsImage() {
        SnapshotFixtureServer fixture; QVERIFY(fixture.server.isListening());
        MainWindow window; window.client()->connectToServer(fixture.url());
        QTRY_VERIFY(window.client()->isConnected());
        QTest::qWait(180);
        QCOMPARE(fixture.snapshots, 0);
        QCOMPARE(fixture.count("camera_get_capabilities"), 0);
        window.show();
        QTRY_VERIFY(cameraCard(window));
        auto *card = cameraCard(window);
        auto *preview = card->findChild<QAbstractButton *>("cameraSnapshot"); QVERIFY(preview);
        QTRY_VERIFY(showsColor(preview, Magenta));
        QCOMPARE(fixture.snapshots, 1);
        auto *power = navigation(window, "插座"); QVERIFY(power);
        QTest::mouseClick(power, Qt::LeftButton);
        QTRY_VERIFY(card->isHidden()); QTest::qWait(80);
        auto *refresh = globalRefresh(window); QVERIFY(refresh);
        QTest::mouseClick(refresh, Qt::LeftButton);
        QTest::qWait(200); QCOMPARE(fixture.snapshots, 1);

        auto *all = navigation(window, "设备总览"); QVERIFY(all);
        QTest::mouseClick(all, Qt::LeftButton);
        QTRY_VERIFY(card->isVisible());
        window.hide(); QTest::qWait(80);
        // Exercise the visibility guard even if an already queued action arrives.
        refresh->click(); QTest::qWait(150); QCOMPARE(fixture.snapshots, 1);
        window.client()->disconnectFromServer();
        QTRY_VERIFY(!showsColor(preview, Magenta));
        QCOMPARE(card->findChild<QLabel *>("snapshotTime")->text(), QStringLiteral("尚无快照"));

        fixture.color = Cobalt;
        window.show(); window.client()->connectToServer(fixture.url());
        QTRY_VERIFY(window.client()->isConnected());
        QTRY_VERIFY(cameraCard(window));
        card = cameraCard(window); preview = card->findChild<QAbstractButton *>("cameraSnapshot");
        QTRY_VERIFY(showsColor(preview, Cobalt));
        QCOMPARE(fixture.snapshots, 2);
        QCOMPARE(fixture.count("camera_start_stream"), 0);
    }

    void privacyErrorClearsImageWithoutGlobalBannerAndManualRecoveryWorks() {
        SnapshotFixtureServer fixture; QVERIFY(fixture.server.isListening());
        MainWindow window; window.show(); window.client()->connectToServer(fixture.url());
        QTRY_VERIFY(window.client()->isConnected());
        QTRY_VERIFY(cameraCard(window));
        auto *card = cameraCard(window);
        auto *preview = card->findChild<QAbstractButton *>("cameraSnapshot"); QVERIFY(preview);
        auto *refresh = card->findChild<QPushButton *>("snapshotRefresh"); QVERIFY(refresh);
        QTRY_VERIFY(showsColor(preview, Magenta));
        fixture.privacyError = true;
        QTest::mouseClick(refresh, Qt::LeftButton);
        QTRY_COMPARE(fixture.snapshots, 2);
        QTRY_VERIFY(!showsColor(preview, Magenta));
        QVERIFY(card->findChild<QLabel *>("snapshotStatus")->text().contains("隐私"));
        QCOMPARE(card->findChild<QLabel *>("snapshotTime")->text(), QStringLiteral("尚无快照"));
        QVERIFY(window.findChild<QLabel *>("notice")->isHidden());
        const QString log = window.findChild<QPlainTextEdit *>("logs")->toPlainText();
        QVERIFY(!fixture.lastEncodedImage.isEmpty());
        QVERIFY(!log.contains(QString::fromLatin1(fixture.lastEncodedImage.left(100))));

        fixture.privacyError = false; fixture.color = Cobalt;
        QTest::qWait(3050);
        QTest::mouseClick(refresh, Qt::LeftButton);
        QTRY_VERIFY(showsColor(preview, Cobalt));
        QCOMPARE(fixture.snapshots, 3);
        QVERIFY(!card->findChild<QLabel *>("snapshotStatus")->text().contains("旧快照"));
        QCOMPARE(fixture.count("camera_start_stream"), 0);
    }

private:
    QTemporaryDir m_settings;
};

QTEST_MAIN(SnapshotUiTest)
#include "tst_snapshotui.moc"
