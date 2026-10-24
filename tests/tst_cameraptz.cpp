#include "matterclient.h"
#include "cameraptzcontrols.h"
#include <QJsonDocument>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QWebSocket>
#include <QWebSocketServer>
#include <functional>

namespace {
constexpr qint64 Cluster = 4294048769LL;
QJsonObject capabilities() { return {{"1/4294048769/0", true}, {"1/4294048769/1", true}, {"1/4294048769/2", 0}, {"1/4294048769/3", 15}, {"1/4294048769/65529", QJsonArray{0}}}; }
QJsonObject camera() {
    auto attrs = capabilities();
    attrs.insert("1/29/0", QJsonArray{QJsonObject{{"deviceType", 0x142}, {"revision", 1}}});
    attrs.insert("1/29/1", QJsonArray{29, 0x551, 0x553, Cluster});
    attrs.insert("1/4294048769/65529", QJsonArray{0});
    return {{"node_id", 7}, {"available", true}, {"attributes", attrs}};
}
class Server : public QObject {
public:
    QWebSocketServer server{"PTZ synthetic test", QWebSocketServer::NonSecureMode};
    QWebSocket *peer = nullptr;
    QJsonArray nodes{camera()};
    QList<QJsonObject> requests;
    std::function<void(const QJsonObject &)> handler;
    Server() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QWebSocketServer::newConnection, this, [this] {
            peer = server.nextPendingConnection(); peer->setParent(this);
            connect(peer, &QWebSocket::textMessageReceived, this, [this](const QString &text) {
                const auto request = QJsonDocument::fromJson(text.toUtf8()).object(); requests.append(request);
                if (request.value("command") == "start_listening") reply(request, nodes);
                else if (handler) handler(request);
            });
            send({{"schema_version", 14}, {"min_supported_schema_version", 11}});
        });
    }
    QUrl url() const { return QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(server.serverPort())); }
    void send(const QJsonObject &value) { if (peer) peer->sendTextMessage(QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact))); }
    void reply(const QJsonObject &request, const QJsonValue &result) { send({{"message_id", request.value("message_id")}, {"result", result}}); }
    int count(const QString &command) const { int n=0; for (const auto &request : requests) if (request.value("command") == command) ++n; return n; }
};
}
class CameraPtzTest : public QObject {
    Q_OBJECT
private slots:
    void strictCapabilitiesAndUnsignedCluster() {
        const auto parsed = devicesFromNodes({camera()});
        QCOMPARE(parsed.size(), 1);
        const auto device = parsed.first();
        QVERIFY(device.hasCamera && device.hasCameraPtz && device.cameraPtzKnown && device.cameraPtzAvailable);
        QCOMPARE(device.cameraPtzDirections, quint8(15));
        const QList<QPair<QString,QJsonValue>> invalid{{"1/4294048769/0", 1}, {"1/4294048769/1", QJsonValue(QJsonValue::Null)},
            {"1/4294048769/2", 1}, {"1/4294048769/3", 16}, {"1/4294048769/3", 1.5}, {"1/4294048769/65529", QJsonArray{}}, {"1/4294048769/65529", QJsonArray{0.5}}};
        for (const auto &change : invalid) {
            auto node = camera(); auto attrs = node.value("attributes").toObject(); attrs.insert(change.first, change.second); node.insert("attributes", attrs);
            const auto altered = devicesFromNodes({node}).first();
            QVERIFY(!altered.cameraPtzKnown && !altered.cameraPtzAvailable);
        }
        auto node = camera(); auto attrs = node.value("attributes").toObject();
        attrs.insert("1/29/1", QJsonArray{29,0x551,0x553, -917503}); node.insert("attributes", attrs);
        QVERIFY(!devicesFromNodes({node}).first().hasCameraPtz);
        attrs.insert("1/29/1", QJsonArray{29,0x551,0x553, 4294048769.5}); node.insert("attributes", attrs);
        QVERIFY(!devicesFromNodes({node}).first().hasCameraPtz);
    }
    void moveHasNamedDirectionAndNoQueue() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        const auto device = client.devices().first(); QSignalSpy responses(&client, &MatterClient::cameraPtzResponse);
        QElapsedTimer elapsed; elapsed.start();
        const auto id = client.moveCameraStep(device, 2);
        QTRY_COMPARE(server.count("device_command"), 1);
        const auto request = server.requests.last(), args = request.value("args").toObject();
        QCOMPARE(args.value("cluster_id").toInteger(), Cluster);
        QCOMPARE(args.value("endpoint_id").toInt(), 1);
        QCOMPARE(args.value("command_name").toString(), QStringLiteral("MoveStep"));
        QCOMPARE(args.value("payload").toObject(), QJsonObject({{"direction", 2}}));
        QVERIFY(!client.canMoveCameraStep(device, 0));
        client.moveCameraStep(device, 0); QTRY_COMPARE(responses.size(), 1); QCOMPARE(server.count("device_command"), 1);
        server.reply(request, QJsonValue(QJsonValue::Null)); QTRY_COMPARE(responses.size(), 2);
        QCOMPARE(responses.last().at(0).toString(), id); QVERIFY(responses.last().at(2).toBool());
        QVERIFY(responses.last().at(3).toString().contains(QStringLiteral("不代表")));
        QVERIFY(!client.canMoveCameraStep(device, 0));
        QTRY_VERIFY_WITH_TIMEOUT(client.canMoveCameraStep(device, 0), 2500); QVERIFY(elapsed.elapsed() >= 1800);
        QCOMPARE(server.count("device_command"), 1);
    }
    void synchronousSignalsCannotBypassSingleFlight() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        const auto device=client.devices().first(); QSignalSpy responses(&client,&MatterClient::cameraPtzResponse);
        bool reentered=false;
        connect(&client,&MatterClient::logMessage,&client,[&](const QString &) {
            if(reentered) return;
            reentered=true; client.moveCameraStep(device,1);
        });
        client.moveCameraStep(device,0); QTRY_COMPARE(server.count("device_command"),1);
        QTRY_COMPARE(responses.size(),1); QVERIFY(!responses.first().at(2).toBool());
        server.reply(server.requests.last(),QJsonValue(QJsonValue::Null)); QTRY_COMPARE(responses.size(),2);
        QCOMPARE(server.count("device_command"),1);
    }
    void readUsesPathMappingAndFailsClosed() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        auto device = client.devices().first(); QSignalSpy responses(&client, &MatterClient::cameraPtzResponse);
        client.readCameraPtz(device); QTRY_COMPARE(server.count("read_attribute"), 1);
        auto request = server.requests.last();
        QCOMPARE(request.value("args").toObject().value("attribute_path").toArray(),
            QJsonArray({"1/4294048769/0","1/4294048769/1","1/4294048769/2","1/4294048769/3","1/4294048769/65529"}));
        server.reply(request, QJsonObject{{"0",true},{"1",true},{"2",0},{"3",15}});
        QTRY_COMPARE(responses.size(), 1); QVERIFY(!responses.last().at(2).toBool()); QVERIFY(!client.canMoveCameraStep(device, 0));
        client.readCameraPtz(device); QTRY_COMPARE(server.count("read_attribute"), 2);
        server.reply(server.requests.last(), capabilities()); QTRY_COMPARE(responses.size(), 2);
        QVERIFY(responses.last().at(2).toBool()); QVERIFY(client.canMoveCameraStep(device, 0));
        server.send({{"event","attribute_updated"},{"data",QJsonArray{7,"1/4294048769/3", 1}}});
        QTRY_VERIFY(!client.canMoveCameraStep(device, 3)); QVERIFY(client.canMoveCameraStep(device, 0));
    }
    void uncachedAcceptedCommandsRecoverAndARevokedCommandRejectsALateRead() {
        Server server;
        auto node = camera(); auto attrs = node.value("attributes").toObject();
        attrs.remove("1/4294048769/3"); attrs.remove("1/4294048769/65529");
        node.insert("attributes", attrs); server.nodes = {node};
        MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        const auto device = client.devices().first(); QVERIFY(device.hasCameraPtz); QVERIFY(!device.cameraPtzKnown);
        QVERIFY(!client.canMoveCameraStep(device, 0));
        QSignalSpy responses(&client, &MatterClient::cameraPtzResponse);
        client.readCameraPtz(device); QTRY_COMPARE(server.count("read_attribute"), 1);
        const auto paths = server.requests.last().value("args").toObject().value("attribute_path").toArray();
        QVERIFY(paths.contains(QStringLiteral("1/4294048769/65529")));
        server.reply(server.requests.last(), capabilities()); QTRY_COMPARE(responses.size(), 1);
        QVERIFY(responses.last().at(2).toBool()); QVERIFY(client.devices().first().cameraPtzKnown);
        QVERIFY(client.canMoveCameraStep(device, 0));
        client.readCameraPtz(device); QTRY_COMPARE(server.count("read_attribute"), 2);
        const auto pending = server.requests.last();
        server.send({{"event", "attribute_updated"}, {"data", QJsonArray{7, "1/4294048769/65529", QJsonArray{}}}});
        QTRY_VERIFY(!client.devices().first().cameraPtzKnown);
        server.reply(pending, capabilities()); QTRY_COMPARE(responses.size(), 2);
        QVERIFY(!responses.last().at(2).toBool()); QVERIFY(!client.canMoveCameraStep(device, 0));
        client.readCameraPtz(device); QTRY_COMPARE(server.count("read_attribute"), 3);
        server.reply(server.requests.last(), capabilities()); QTRY_COMPARE(responses.size(), 3);
        QVERIFY(responses.last().at(2).toBool()); QVERIFY(client.canMoveCameraStep(device, 0));
        QCOMPARE(server.count("device_command"), 0);
    }
    void removedContextRejectsLateRead() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        const auto device = client.devices().first(); QSignalSpy responses(&client, &MatterClient::cameraPtzResponse);
        client.readCameraPtz(device); QTRY_COMPARE(server.count("read_attribute"), 1); const auto request = server.requests.last();
        server.send({{"event","node_removed"},{"data",7}}); QTRY_VERIFY(client.devices().isEmpty());
        server.send({{"event","node_added"},{"data",camera()}}); QTRY_COMPARE(client.devices().size(),1);
        server.reply(request, capabilities()); QTRY_COMPARE(responses.size(),1); QVERIFY(!responses.last().at(2).toBool());
        QCOMPARE(server.count("device_command"), 0);
    }
    void changedCapabilitiesRejectStaleRead() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        const auto device = client.devices().first(); QSignalSpy responses(&client, &MatterClient::cameraPtzResponse);
        client.readCameraPtz(device); QTRY_COMPARE(server.count("read_attribute"), 1); const auto request = server.requests.last();
        server.send({{"event","attribute_updated"},{"data",QJsonArray{7,"1/4294048769/0",false}}});
        QTRY_VERIFY(!client.devices().first().cameraPtzSupported);
        server.reply(request, capabilities()); QTRY_COMPARE(responses.size(),1); QVERIFY(!responses.last().at(2).toBool());
        QVERIFY(!client.canMoveCameraStep(device,0));
    }
    void oldDeviceCannotControlNewControllerWithSameIds() {
        Server first, second; MatterClient client; client.connectToServer(first.url()); QTRY_VERIFY(client.isConnected());
        const auto old=client.devices().first();
        client.connectToServer(second.url()); QTRY_VERIFY(client.isConnected());
        QVERIFY(client.devices().first().controllerGeneration != old.controllerGeneration);
        QVERIFY(!client.canMoveCameraStep(old,0));
        QSignalSpy responses(&client,&MatterClient::cameraPtzResponse);
        client.moveCameraStep(old,0); client.readCameraPtz(old); QTRY_COMPARE(responses.size(),2);
        QCOMPARE(second.count("device_command"),0); QCOMPARE(second.count("read_attribute"),0);
        QVERIFY(client.canMoveCameraStep(client.devices().first(),0));
    }
    void unrelatedNodeUpdatesDoNotInvalidateRead() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        const auto device=client.devices().first(); QSignalSpy responses(&client,&MatterClient::cameraPtzResponse);
        client.readCameraPtz(device); QTRY_COMPARE(server.count("read_attribute"),1); const auto request=server.requests.last();
        auto other=camera(); other.insert("node_id",8);
        server.send({{"event","node_updated"},{"data",other}}); QTRY_COMPARE(client.devices().size(),2);
        server.send({{"event","attribute_updated"},{"data",QJsonArray{7,"1/4294048769/0",true}}});
        server.reply(request,capabilities()); QTRY_COMPARE(responses.size(),1); QVERIFY(responses.last().at(2).toBool());
        QVERIFY(client.canMoveCameraStep(device,0));
    }
    void errorsAndTimeoutNeverRetry() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        const auto device = client.devices().first(); QSignalSpy responses(&client, &MatterClient::cameraPtzResponse);
        client.readCameraPtz(device); QTRY_COMPARE(server.count("read_attribute"), 1);
        QTRY_COMPARE_WITH_TIMEOUT(responses.size(),1,6500); QVERIFY(!responses.last().at(2).toBool());
        QVERIFY(!client.canMoveCameraStep(device,0)); QCOMPARE(server.count("read_attribute"),1);
        server.reply(server.requests.last(), capabilities()); QTest::qWait(30); QCOMPARE(responses.size(),1);
        QVERIFY(!client.canMoveCameraStep(device,0));
        client.disconnectFromServer(); client.moveCameraStep(device,0); QTRY_COMPARE(responses.size(),2); QCOMPARE(server.count("device_command"),0);
        client.setDemoMode(true); client.readCameraPtz(device); QTRY_COMPARE(responses.size(),3); QCOMPARE(server.count("read_attribute"),1);
    }
    void nonNullInvokeResultIsNotAccepted() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QSignalSpy responses(&client,&MatterClient::cameraPtzResponse);
        client.moveCameraStep(client.devices().first(),3); QTRY_COMPARE(server.count("device_command"),1);
        server.reply(server.requests.last(), QJsonObject{{"success",true}}); QTRY_COMPARE(responses.size(),1); QVERIFY(!responses.last().at(2).toBool());
        QVERIFY(!client.devices().first().cameraPtzKnown);
    }
    void nativeButtonsStayDisabledAfterCloseAndLateAck() {
        Server server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        CameraPtzControls controls(&client,client.devices().first()); controls.show();
        auto *up=controls.findChild<QPushButton*>("ptzUp"); QVERIFY(up); QVERIFY(!up->autoRepeat());
        QTRY_COMPARE(server.count("read_attribute"),1); QVERIFY(!up->isEnabled());
        server.reply(server.requests.last(),capabilities()); QTRY_VERIFY(up->isEnabled());
        const auto preview = qEnvironmentVariable("MATTERDESK_PTZ_PREVIEW");
        if (!preview.isEmpty()) { controls.resize(380, 170); QVERIFY(controls.grab().save(preview)); }
        QTest::mouseClick(up,Qt::LeftButton); QTRY_COMPARE(server.count("device_command"),1);
        QVERIFY(!up->isEnabled()); const auto request=server.requests.last(); controls.setClosing();
        server.reply(request,QJsonValue(QJsonValue::Null)); QTest::qWait(1900); QVERIFY(!up->isEnabled());
        controls.hide(); controls.show(); QTest::qWait(30); QVERIFY(!up->isEnabled());
        QCOMPARE(server.count("device_command"),1); QCOMPARE(server.count("read_attribute"),1);
    }
};
QTEST_MAIN(CameraPtzTest)
#include "tst_cameraptz.moc"
