#include "camerasnapshots.h"
#include "matterclient.h"

#include <QBuffer>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTest>
#include <QWebSocket>
#include <QWebSocketServer>
#include <functional>

namespace {
QJsonObject capabilities(QSize size = {640, 480}) {
    return {{"features", QJsonArray{"Snapshot"}}, {"privacy", QJsonObject{}},
        {"snapshot", QJsonObject{{"capabilities", QJsonArray{QJsonObject{
            {"image_codec", "JPEG"}, {"resolution", QJsonObject{{"width", size.width()}, {"height", size.height()}}},
            {"max_frame_rate", 1}, {"requires_hardware_encoder", false}, {"requires_encoded_pixels", false}}}}}}};
}
QJsonObject frame(QString provenance = "allocated", int streamId = 4, QSize size = {64, 36}, const char *format = "JPEG") {
    QImage image(size, QImage::Format_RGB32);
    image.fill(QColor("#607d8b"));
    QByteArray bytes;
    QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer, format);
    return {{"data", QString::fromLatin1(bytes.toBase64())}, {"codec", "JPEG"},
        {"resolution", QJsonObject{{"width", size.width()}, {"height", size.height()}}},
        {"stream_id", streamId}, {"provenance", provenance}, {"degraded", false}};
}
QJsonArray nodes(int count = 1) {
    QJsonObject attrs{{"0/40/5", "Snapshot test"}};
    for (int endpoint = 1; endpoint <= count; ++endpoint) {
        const auto prefix = QString::number(endpoint);
        attrs.insert(prefix + "/29/0", QJsonArray{QJsonObject{{"0", 0x142}, {"1", 1}}});
        attrs.insert(prefix + "/29/1", QJsonArray{29, 57, 1361, 1363});
        attrs.insert(prefix + "/57/5", QStringLiteral("Camera %1").arg(endpoint));
        attrs.insert(prefix + "/57/17", true);
    }
    return {QJsonObject{{"node_id", 1}, {"available", true}, {"is_bridge", true}, {"attributes", attrs}}};
}
class Server : public QObject {
public:
    QWebSocketServer server{"Snapshot fixture", QWebSocketServer::NonSecureMode};
    QWebSocket *peer = nullptr;
    QJsonArray deviceNodes = nodes();
    QJsonObject caps = capabilities();
    QJsonObject snapshot = frame();
    QList<QJsonObject> requests;
    std::function<bool(const QJsonObject &)> intercept;
    Server() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QWebSocketServer::newConnection, this, [this] {
            peer = server.nextPendingConnection(); peer->setParent(this);
            connect(peer, &QWebSocket::textMessageReceived, this, [this](const QString &text) {
                auto request = QJsonDocument::fromJson(text.toUtf8()).object();
                requests.append(request);
                const auto command = request.value("command").toString();
                if (command == "start_listening") { reply(request, deviceNodes); return; }
                if (intercept && intercept(request)) return;
                if (command == "camera_get_capabilities") reply(request, caps);
                else if (command == "camera_snapshot") reply(request, snapshot);
                else reply(request, QJsonValue(QJsonValue::Null));
            });
            send({{"schema_version", 14}, {"min_supported_schema_version", 11}, {"sdk_version", "test"}});
        });
    }
    QUrl url() const { return QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(server.serverPort())); }
    void send(const QJsonObject &message) {
        if (peer) peer->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }
    void reply(const QJsonObject &request, QJsonValue result) { send({{"message_id", request.value("message_id")}, {"result", result}}); }
    QList<QJsonObject> calls(QString command) const {
        QList<QJsonObject> result;
        for (const auto &request : requests) if (request.value("command") == command) result.append(request);
        return result;
    }
};
}

class CameraSnapshotsTest : public QObject {
    Q_OBJECT
private slots:
    void jpegAndAllocatedStreamAreHandledWithoutLiveOrMicRequests() {
        Server server; MatterClient client;
        client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QSignalSpy errors(&client, &MatterClient::errorOccurred);
        CameraSnapshots snapshots(&client);
        snapshots.setVisibleDevices(client.devices());
        QTRY_VERIFY(!snapshots.state("1/1").image.isNull());
        QTRY_COMPARE(server.calls("camera_release_stream").size(), 1);
        const auto state = snapshots.state("1/1");
        QVERIFY(state.capturedAt.isValid()); QVERIFY(!state.stale && !state.loading); QVERIFY(state.message.isEmpty());
        QCOMPARE(state.image.size(), QSize(640, 360));
        const auto released = server.calls("camera_release_stream").first().value("args").toObject();
        QCOMPARE(released.value("kind").toString(), QStringLiteral("snapshot"));
        QCOMPARE(released.value("stream_id").toInt(), 4);
        QCOMPARE(server.requests.size(), 4); // subscribe, capabilities, snapshot, release only.
        QCOMPARE(errors.size(), 0);
    }
    void onlyLargeCapabilityUsesTargetWithoutHardCeiling() {
        Server server; server.caps = capabilities({1920, 1080});
        MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraSnapshots snapshots(&client); snapshots.setVisibleDevices(client.devices());
        QTRY_COMPARE(server.calls("camera_snapshot").size(), 1);
        const auto args = server.calls("camera_snapshot").first().value("args").toObject();
        QVERIFY(!args.contains("max_resolution"));
        QCOMPARE(args.value("codec").toString(), QStringLiteral("JPEG"));
        QCOMPARE(args.value("resolution").toObject(), (QJsonObject{{"width", 640}, {"height", 360}}));
    }
    void missingSnapshotOrJpegDoesNotIssueCapture() {
        for (int mode = 0; mode < 2; ++mode) {
            Server server;
            if (!mode) server.caps.insert("features", QJsonArray{"Video"});
            else {
                auto cap = server.caps.value("snapshot").toObject().value("capabilities").toArray().first().toObject();
                cap.insert("image_codec", "PNG");
                server.caps.insert("snapshot", QJsonObject{{"capabilities", QJsonArray{cap}}});
            }
            MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
            CameraSnapshots snapshots(&client); snapshots.setVisibleDevices(client.devices());
            QTRY_VERIFY(!snapshots.state("1/1").loading && snapshots.state("1/1").message.contains(QStringLiteral("不支持")));
            QCOMPARE(server.calls("camera_snapshot").size(), 0);
            QCOMPARE(server.calls("camera_release_stream").size(), 0);
        }
    }
    void invalidBase64CodecOrImageDimensionsNeverReachCardsButReleaseOwnStream() {
        for (int mode = 0; mode < 4; ++mode) {
            Server server;
            if (mode == 0) server.snapshot.insert("data", "!!!!");
            if (mode == 1) server.snapshot.insert("data", "AA==");
            if (mode == 2) server.snapshot = frame("allocated", 4, {64, 36}, "PNG");
            if (mode == 3) server.snapshot = frame("allocated", 4, {4097, 2});
            MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
            CameraSnapshots snapshots(&client); snapshots.setVisibleDevices(client.devices());
            QTRY_COMPARE(server.calls("camera_release_stream").size(), 1);
            QVERIFY(snapshots.state("1/1").image.isNull());
            QVERIFY(snapshots.state("1/1").message.contains(QStringLiteral("无效")));
        }
    }
    void reusedAndAdoptedStreamsAreNeverReleased() {
        for (const auto &provenance : {QStringLiteral("reused"), QStringLiteral("adopted")}) {
            Server server; server.snapshot = frame(provenance);
            MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
            CameraSnapshots snapshots(&client); snapshots.setVisibleDevices(client.devices());
            QTRY_VERIFY(!snapshots.state("1/1").image.isNull());
            QCOMPARE(server.calls("camera_release_stream").size(), 0);
        }
    }
    void failuresRetainPreviousImageAndManualRefreshIsThrottled() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraSnapshots snapshots(&client); snapshots.setVisibleDevices(client.devices());
        QTRY_COMPARE(server.calls("camera_release_stream").size(), 1);
        QTest::qWait(30);
        const auto first = snapshots.state("1/1");
        server.snapshot.insert("data", "bad!");
        snapshots.refresh("1/1");
        QTRY_COMPARE(server.calls("camera_snapshot").size(), 2);
        QTRY_VERIFY(snapshots.state("1/1").stale);
        QCOMPARE(snapshots.state("1/1").image, first.image);
        QCOMPARE(snapshots.state("1/1").capturedAt, first.capturedAt);
        snapshots.refresh("1/1"); snapshots.refresh("1/1");
        QTest::qWait(100);
        QCOMPARE(server.calls("camera_snapshot").size(), 2);
    }
    void serializesThroughCleanupBeforeNextCamera() {
        Server server; server.deviceNodes = nodes(2);
        QJsonObject heldSnapshot, heldRelease;
        server.intercept = [&](const QJsonObject &request) {
            const auto args = request.value("args").toObject();
            if (args.value("endpoint_id").toInt() == 1 && request.value("command") == "camera_snapshot") { heldSnapshot = request; return true; }
            if (args.value("endpoint_id").toInt() == 1 && request.value("command") == "camera_release_stream") { heldRelease = request; return true; }
            return false;
        };
        MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraSnapshots snapshots(&client); snapshots.setVisibleDevices(client.devices());
        QTRY_VERIFY(!heldSnapshot.isEmpty());
        QCOMPARE(server.calls("camera_get_capabilities").size(), 1);
        server.reply(heldSnapshot, frame());
        QTRY_VERIFY(!heldRelease.isEmpty());
        QCOMPARE(server.calls("camera_get_capabilities").size(), 1);
        server.reply(heldRelease, QJsonValue(QJsonValue::Null));
        QTRY_COMPARE(server.calls("camera_get_capabilities").size(), 2);
        QTRY_VERIFY(!snapshots.state("1/2").image.isNull());
    }
    void inactiveAndHiddenViewsDoNotScheduleRequests() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraSnapshots snapshots(&client); snapshots.setActive(false); snapshots.setVisibleDevices(client.devices());
        QTest::qWait(100); QCOMPARE(server.calls("camera_get_capabilities").size(), 0);
        snapshots.refresh("1/1"); QTest::qWait(25); QCOMPARE(server.calls("camera_get_capabilities").size(), 0);
        snapshots.setVisibleDevices({}); snapshots.setActive(true);
        QTest::qWait(100); QCOMPARE(server.calls("camera_get_capabilities").size(), 0);
        snapshots.setVisibleDevices(client.devices());
        QTRY_VERIFY(!snapshots.state("1/1").image.isNull());
        const auto saved = snapshots.state("1/1").image;
        snapshots.setVisibleDevices({}); QCOMPARE(snapshots.state("1/1").image, saved);
    }
    void hiddenCapabilityReplyDoesNotCaptureAndHiddenSnapshotStillCleansUp() {
        Server server; QJsonObject heldCaps, heldFrame;
        server.intercept = [&](const QJsonObject &request) {
            if (request.value("command") == "camera_get_capabilities") { heldCaps = request; return true; }
            if (request.value("command") == "camera_snapshot") { heldFrame = request; return true; }
            return false;
        };
        MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraSnapshots snapshots(&client); snapshots.setVisibleDevices(client.devices());
        QTRY_VERIFY(!heldCaps.isEmpty()); snapshots.setVisibleDevices({});
        server.reply(heldCaps, capabilities()); QTest::qWait(50);
        QCOMPARE(server.calls("camera_snapshot").size(), 0);
        heldCaps = {}; snapshots.setVisibleDevices(client.devices());
        QTRY_VERIFY(!heldCaps.isEmpty()); server.reply(heldCaps, capabilities());
        QTRY_VERIFY(!heldFrame.isEmpty()); snapshots.setVisibleDevices({});
        server.reply(heldFrame, frame());
        QTRY_COMPARE(server.calls("camera_release_stream").size(), 1);
        QVERIFY(snapshots.state("1/1").image.isNull());
    }
    void clientOwnedJobReleasesLateReplyAfterManagerDestruction() {
        Server server; QJsonObject held;
        server.intercept = [&](const QJsonObject &request) {
            if (request.value("command") == "camera_snapshot") { held = request; return true; }
            return false;
        };
        MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        auto *snapshots = new CameraSnapshots(&client); snapshots->setVisibleDevices(client.devices());
        QTRY_VERIFY(!held.isEmpty()); delete snapshots;
        server.reply(held, frame());
        QTRY_COMPARE(server.calls("camera_release_stream").size(), 1);
    }
    void offlineRetainsOldImageDisconnectClearsAndReconnectRefreshes() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraSnapshots snapshots(&client); snapshots.setVisibleDevices(client.devices());
        QTRY_COMPARE(server.calls("camera_release_stream").size(), 1);
        QTest::qWait(25);
        const auto saved = snapshots.state("1/1").image;
        server.send({{"event", "attribute_updated"}, {"data", QJsonArray{1, "1/57/17", false}}});
        QTRY_VERIFY(snapshots.state("1/1").stale);
        QCOMPARE(snapshots.state("1/1").image, saved);
        client.disconnectFromServer();
        QVERIFY(snapshots.state("1/1").image.isNull());
        QVERIFY(!snapshots.state("1/1").capturedAt.isValid());
        client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QTRY_COMPARE(server.calls("camera_get_capabilities").size(), 2);
        QTRY_VERIFY(!snapshots.state("1/1").image.isNull());
    }
    void oldControllerReplyCannotReleaseOnNewConnection() {
        Server first; Server second; QJsonObject held;
        first.intercept = [&](const QJsonObject &request) {
            if (request.value("command") == "camera_snapshot") { held = request; return true; }
            return false;
        };
        MatterClient client; client.connectToServer(first.url()); QTRY_VERIFY(client.isConnected());
        CameraSnapshots snapshots(&client); snapshots.setVisibleDevices(client.devices());
        QTRY_VERIFY(!held.isEmpty()); snapshots.setActive(false);
        client.connectToServer(second.url()); QTRY_VERIFY(client.isConnected());
        first.reply(held, frame()); QTest::qWait(60);
        QVERIFY(snapshots.state("1/1").image.isNull());
        QCOMPARE(second.calls("camera_release_stream").size(), 0);
        QCOMPARE(second.calls("camera_snapshot").size(), 0);
    }
    void removedAndReaddedEndpointDoesNotReceiveOldImage() {
        Server server; QList<QJsonObject> held;
        server.intercept = [&](const QJsonObject &request) {
            if (request.value("command") == "camera_snapshot") { held.append(request); return true; }
            return false;
        };
        MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraSnapshots snapshots(&client); snapshots.setVisibleDevices(client.devices());
        QTRY_COMPARE(held.size(), 1);
        server.send({{"event", "node_removed"}, {"data", 1}});
        QTRY_VERIFY(client.devices().isEmpty());
        server.send({{"event", "node_added"}, {"data", nodes().first()}});
        QTRY_COMPARE(client.devices().size(), 1);
        snapshots.setVisibleDevices(client.devices());
        server.reply(held.first(), frame());
        QTRY_COMPARE(server.calls("camera_release_stream").size(), 1);
        QTRY_COMPARE(held.size(), 2);
        QVERIFY(snapshots.state("1/1").image.isNull());
        server.reply(held.last(), frame());
        QTRY_VERIFY(!snapshots.state("1/1").image.isNull());
    }
    void pausedRequestHasNoMisleadingLoadingMessage() {
        Server server; QJsonObject held;
        server.intercept = [&](const QJsonObject &request) {
            if (request.value("command") == "camera_snapshot") { held = request; return true; }
            return false;
        };
        MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraSnapshots snapshots(&client); snapshots.setVisibleDevices(client.devices());
        QTRY_VERIFY(!held.isEmpty()); snapshots.setActive(false);
        QVERIFY(!snapshots.state("1/1").loading);
        QVERIFY(!snapshots.state("1/1").message.contains(QStringLiteral("正在")));
        server.reply(held, frame()); QTRY_COMPARE(server.calls("camera_release_stream").size(), 1);
        QVERIFY(!snapshots.state("1/1").loading);
        QVERIFY(!snapshots.state("1/1").message.contains(QStringLiteral("正在")));
    }
    void periodicRetryRechecksCapabilitiesAfterThirtySeconds() {
        Server server; server.caps.insert("features", QJsonArray{"Video"});
        MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraSnapshots snapshots(&client); snapshots.setVisibleDevices(client.devices());
        QTRY_VERIFY(!snapshots.state("1/1").loading && snapshots.state("1/1").message.contains(QStringLiteral("不支持")));
        QCOMPARE(server.calls("camera_get_capabilities").size(), 1);
        server.caps = capabilities();
        QTRY_COMPARE_WITH_TIMEOUT(server.calls("camera_get_capabilities").size(), 2, 32000);
        QTRY_VERIFY(!snapshots.state("1/1").image.isNull());
        QTRY_COMPARE(server.calls("camera_release_stream").size(), 1);
    }
    void privacyClearsPreviousImageAndSuppressesCaptureWhileEnabled() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraSnapshots snapshots(&client); snapshots.setVisibleDevices(client.devices());
        QTRY_COMPARE(server.calls("camera_release_stream").size(), 1); QTest::qWait(25);
        server.caps.insert("privacy", QJsonObject{{"soft_livestream_mode_enabled", true}});
        snapshots.refresh("1/1");
        QTRY_VERIFY(snapshots.state("1/1").image.isNull());
        QVERIFY(snapshots.state("1/1").message.contains(QStringLiteral("隐私")));
        QCOMPARE(server.calls("camera_snapshot").size(), 1);
    }
};

QTEST_GUILESS_MAIN(CameraSnapshotsTest)
#include "tst_camerasnapshots.moc"
