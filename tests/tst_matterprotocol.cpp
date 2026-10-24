#include "matterclient.h"
#include "matterdevice.h"

#include <QJsonDocument>
#include <QSignalSpy>
#include <QTest>
#include <QWebSocket>
#include <QWebSocketServer>
#include <functional>

namespace {
QString encode(QJsonObject object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}
class FakeServer : public QObject {
public:
    QWebSocketServer server{QStringLiteral("MatterDesk protocol test"), QWebSocketServer::NonSecureMode};
    QWebSocket *peer = nullptr;
    QList<QJsonObject> requests;
    QJsonArray nodes = demoNodes();
    bool greeting = true;
    QJsonObject info{{"schema_version", 14}, {"min_supported_schema_version", 11}, {"sdk_version", "test"}};
    std::function<void(const QJsonObject &)> handler;
    FakeServer() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QWebSocketServer::newConnection, this, [this] {
            peer = server.nextPendingConnection();
            peer->setParent(this);
            connect(peer, &QWebSocket::textMessageReceived, this, [this](const QString &message) {
                const auto request = QJsonDocument::fromJson(message.toUtf8()).object();
                requests.append(request);
                if (handler) { handler(request); return; }
                const auto command = request.value("command").toString();
                reply(request, command == "start_listening" || command == "get_nodes"
                    ? QJsonValue(nodes) : QJsonValue(QJsonValue::Null));
            });
            if (greeting) send(info);
        });
    }
    QUrl url() const { return QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(server.serverPort())); }
    void send(QJsonObject message) { if (peer) peer->sendTextMessage(encode(message)); }
    void reply(QJsonObject request, QJsonValue result) {
        send({{"message_id", request.value("message_id")}, {"result", result}});
    }
};
QJsonArray alteredDemo(const std::function<void(QJsonObject &)> &alter) {
    auto node = demoNodes().first().toObject();
    auto attrs = node.value("attributes").toObject();
    alter(attrs);
    node.insert("attributes", attrs);
    return {node};
}
}

class MatterProtocolTest : public QObject {
    Q_OBJECT
private slots:
    void bridgeExpandsIntoDevices() {
        const auto devices = devicesFromNodes(demoNodes());
        QCOMPARE(devices.size(), 6);
        QCOMPARE(devices[0].key, QStringLiteral("1/2"));
        QCOMPARE(devices[0].name, QStringLiteral("客厅落地灯"));
        QVERIFY(devices[0].hasOnOff && devices[0].hasLevel && devices[0].hasColorTemperature);
        QCOMPARE(devices[0].level, 183);
        QCOMPARE(devices[0].colorTemperature, 333);
        QCOMPARE(devices[1].kind, QStringLiteral("outlet"));
        QCOMPARE(devices[2].kind, QStringLiteral("sensor"));
        QVERIFY(devices[2].readings.join(' ').contains("24.6"));
        QVERIFY(devices[2].readings.join(' ').contains("58.0"));
        QVERIFY(!devices.last().available);
    }
    void onlyServerClustersEnableControls() {
        const auto nodes = alteredDemo([](QJsonObject &attrs) {
            attrs.insert("2/29/1", QJsonArray{29, 57});
            attrs.insert("2/29/2", QJsonArray{6, 8, 768});
        });
        const auto device = devicesFromNodes(nodes).first();
        QVERIFY(!device.hasOnOff && !device.hasLevel && !device.hasColorTemperature);
        QVERIFY(device.onKnown); // A cached attribute never implies control permission.
    }
    void nullableMeasurementsRemainUnknown() {
        const auto nodes = alteredDemo([](QJsonObject &attrs) {
            attrs.insert("2/6/0", QJsonValue(QJsonValue::Null));
            attrs.insert("2/8/0", 255);
            attrs.insert("2/768/7", QJsonValue(QJsonValue::Null));
            attrs.insert("4/1026/0", QJsonValue(QJsonValue::Null));
            attrs.insert("4/1029/0", 65535);
            attrs.insert("4/47/12", 255);
        });
        const auto devices = devicesFromNodes(nodes);
        QVERIFY(!devices[0].onKnown && !devices[0].levelKnown && !devices[0].colorTemperatureKnown);
        QCOMPARE(devices[2].readings.size(), 2);
        QVERIFY(devices[2].readings[0].contains(QStringLiteral("未知")));
        QVERIFY(devices[2].readings[1].contains(QStringLiteral("未知")));
    }
    void bridgeReachabilityPropagates() {
        auto nodes = alteredDemo([](QJsonObject &attrs) { attrs.insert("1/57/17", false); });
        for (const auto &device : devicesFromNodes(nodes)) QVERIFY(!device.available);
        auto node = demoNodes().first().toObject();
        node.insert("available", false);
        for (const auto &device : devicesFromNodes({node})) QVERIFY(!device.available);
    }
    void legacyBridgeReachabilityPropagates() {
        auto node = alteredDemo([](QJsonObject &attrs) { attrs.insert("1/57/17", false); }).first().toObject();
        node.remove("endpoint_parents");
        for (const auto &device : devicesFromNodes({node})) QVERIFY(!device.available);
    }
    void waitsForGreetingThenSubscribes() {
        FakeServer server;
        QVERIFY(server.server.isListening());
        server.greeting = false;
        MatterClient client;
        client.connectToServer(server.url());
        QTRY_VERIFY(server.peer);
        QVERIFY(!client.isConnected());
        QVERIFY(server.requests.isEmpty());
        server.send(server.info);
        QTRY_VERIFY(client.isConnected());
        QCOMPARE(server.requests.first().value("command").toString(), QStringLiteral("start_listening"));
        QCOMPARE(client.devices().size(), 6);
    }
    void incompatibleGreetingStopsConnection() {
        FakeServer server;
        server.info.insert("min_supported_schema_version", 99);
        MatterClient client;
        QSignalSpy errors(&client, &MatterClient::errorOccurred);
        client.connectToServer(server.url());
        QTRY_COMPARE(errors.count(), 1);
        QVERIFY(!client.isConnected());
        QVERIFY(server.requests.isEmpty());
    }
    void liveEventsUpdateAndRemoveEndpoints() {
        FakeServer server;
        MatterClient client;
        client.connectToServer(server.url());
        QTRY_VERIFY(client.isConnected());
        server.send({{"event", "attribute_updated"}, {"data", QJsonArray{1, "2/6/0", false}}});
        QTRY_VERIFY(!client.devices().first().on);
        server.send({{"event", "attribute_updated"}, {"data", QJsonArray{1, "2/8/0", QJsonValue(QJsonValue::Null)}}});
        QTRY_VERIFY(!client.devices().first().levelKnown);
        server.send({{"event", "endpoint_removed"}, {"data", QJsonObject{{"node_id", 1}, {"endpoint_id", 2}}}});
        QTRY_COMPARE(client.devices().size(), 5);
        QCOMPARE(client.devices().first().endpointId, 3);
        server.send({{"event", "node_removed"}, {"data", 1}});
        QTRY_VERIFY(client.devices().isEmpty());
    }
    void invokePayloadAndAuthoritativeState() {
        FakeServer server;
        MatterClient client;
        QSignalSpy finished(&client, &MatterClient::commandFinished);
        client.connectToServer(server.url());
        QTRY_VERIFY(client.isConnected());
        auto device = client.devices().first();
        const auto id = client.setOnOff(device, false);
        QTRY_VERIFY(server.requests.size() >= 2);
        const auto args = server.requests.last().value("args").toObject();
        QCOMPARE(args.value("cluster_id").toInt(), 6);
        QCOMPARE(args.value("endpoint_id").toInt(), 2);
        QCOMPARE(args.value("node_id").toInteger(), qint64(1));
        QCOMPARE(args.value("command_name").toString(), QStringLiteral("Off"));
        QTRY_VERIFY(finished.size() >= 2);
        QCOMPARE(finished.last().at(0).toString(), id);
        QVERIFY(finished.last().at(2).toBool());
        QVERIFY(client.devices().first().on); // Success does not invent state.
        client.setLevel(device, 50);
        QTRY_VERIFY(server.requests.size() >= 3);
        const auto levelArgs = server.requests.last().value("args").toObject();
        QCOMPARE(levelArgs.value("command_name").toString(), QStringLiteral("MoveToLevelWithOnOff"));
        QCOMPARE(levelArgs.value("payload").toObject().value("level").toInt(), 127);
        client.setColorTemperature(device, 400);
        QTRY_VERIFY(server.requests.size() >= 4);
        const auto tempArgs = server.requests.last().value("args").toObject();
        QCOMPARE(tempArgs.value("command_name").toString(), QStringLiteral("MoveToColorTemperature"));
        QCOMPARE(tempArgs.value("payload").toObject().value("colorTemperatureMireds").toInt(), 400);
    }
    void rejectsOfflineAndStaleDevices() {
        MatterClient client;
        client.setDemoMode(true);
        QSignalSpy finished(&client, &MatterClient::commandFinished);
        client.setOnOff(client.devices().last(), true);
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.last().at(2).toBool());
        auto stale = client.devices().first(); stale.key = "1/999";
        client.setOnOff(stale, true);
        QTRY_COMPARE(finished.size(), 2);
        QVERIFY(!finished.last().at(2).toBool());
    }
    void commissionNormalizesAndRedactsCode() {
        FakeServer server;
        server.handler = [&server](const QJsonObject &request) {
            if (request.value("command") == "start_listening") { server.reply(request, server.nodes); return; }
            server.send({{"message_id", request.value("message_id")}, {"error_code", 8},
                         {"details", "invalid code 35325335079"}});
        };
        MatterClient client;
        QSignalSpy errors(&client, &MatterClient::errorOccurred);
        client.connectToServer(server.url());
        QTRY_VERIFY(client.isConnected());
        client.commission("3532-533-5079");
        QTRY_COMPARE(server.requests.size(), 2);
        const auto args = server.requests.last().value("args").toObject();
        QCOMPARE(args.value("code").toString(), QStringLiteral("35325335079"));
        QVERIFY(args.value("network_only").toBool());
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(!errors.first().first().toString().contains("35325335079"));
    }
    void disconnectFailsPendingAndDisablesControls() {
        FakeServer server;
        server.handler = [&server](const QJsonObject &request) {
            if (request.value("command") == "start_listening") server.reply(request, server.nodes);
        };
        MatterClient client;
        client.connectToServer(server.url());
        QTRY_VERIFY(client.isConnected());
        QSignalSpy finished(&client, &MatterClient::commandFinished);
        const auto id = client.setOnOff(client.devices().first(), false);
        client.disconnectFromServer();
        QVERIFY(!client.isConnected());
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), id);
        QVERIFY(!finished.first().at(2).toBool());
        for (const auto &device : client.devices()) QVERIFY(!device.available);
    }
    void delayedRefreshCannotCrossServerSessions() {
        FakeServer firstServer;
        FakeServer secondServer;
        MatterClient client;
        client.connectToServer(firstServer.url());
        QTRY_VERIFY(client.isConnected());
        QSignalSpy finished(&client, &MatterClient::commandFinished);
        client.setOnOff(client.devices().first(), false);
        QTRY_COMPARE(finished.size(), 1);
        client.connectToServer(secondServer.url());
        QTRY_VERIFY(client.isConnected());
        QTest::qWait(450);
        QCOMPARE(secondServer.requests.size(), 1);
        QCOMPARE(secondServer.requests.first().value("command").toString(), QStringLiteral("start_listening"));
    }
    void controllerSwitchIsUnreadyBeforeExposingNewContext() {
        FakeServer first, second;
        second.greeting = false;
        first.handler = [&first](const QJsonObject &r) {
            if (r.value("command") == "start_listening") first.reply(r, first.nodes);
        };
        MatterClient client; client.connectToServer(first.url()); QTRY_VERIFY(client.isConnected());
        const auto oldDevice = client.devices().first();
        const auto pending = client.control(oldDevice, "power", false);
        QTRY_COMPARE(first.requests.size(), 2);
        bool contextSafe = true, requestFailedUnready = false;
        connect(&client, &MatterClient::serverInfoChanged, this, [&] {
            if (client.serverUrl() == second.url()) contextSafe &= !client.isConnected();
        });
        connect(&client, &MatterClient::commandFinished, this, [&](const QString &id, const QString &, bool ok, const QString &) {
            if (id == pending) requestFailedUnready = !ok && !client.isConnected();
        });
        client.connectToServer(second.url());
        QVERIFY(!client.isConnected()); QVERIFY(contextSafe); QVERIFY(requestFailedUnready);
        QVERIFY(client.devices().isEmpty());
        QTRY_VERIFY(second.peer); QCOMPARE(second.requests.size(),0);
    }
    void partialServerInfoUpdatesPreserveKnownIdentity() {
        FakeServer server; server.info.insert("fabric_id", "fabric-a"); server.info.insert("compressed_fabric_id", "compressed-a");
        MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QSignalSpy connections(&client, &MatterClient::connectionChanged);
        server.send({{"event","server_info_updated"},{"data",QJsonObject{{"sdk_version","updated"}}}});
        QTRY_COMPARE(client.serverInfo().value("sdk_version").toString(),QStringLiteral("updated"));
        QCOMPARE(client.serverInfo().value("fabric_id").toString(),QStringLiteral("fabric-a"));
        QCOMPARE(client.serverInfo().value("compressed_fabric_id").toString(),QStringLiteral("compressed-a"));
        server.send({{"event","server_info_updated"},{"data",QJsonObject{{"sdk_version","second"},{"fabric_id",QJsonValue(QJsonValue::Null)},{"compressed_fabric_id","compressed-a"}}}});
        QTRY_COMPARE(client.serverInfo().value("sdk_version").toString(),QStringLiteral("second"));
        QCOMPARE(client.serverInfo().value("fabric_id").toString(),QStringLiteral("fabric-a"));
        QVERIFY(client.isConnected()); QCOMPARE(server.requests.size(),1); QCOMPARE(connections.size(),0);
    }
    void inPlaceFabricChangeInvalidatesPendingAndResubscribesFresh() {
        for (const auto &identityKey : {QStringLiteral("fabric_id"),QStringLiteral("compressed_fabric_id")}) {
            FakeServer server; server.info.insert(identityKey,"old-fabric");
            int subscriptions = 0; QJsonObject freshSubscription;
            server.handler = [&server,&subscriptions,&freshSubscription](const QJsonObject &r) {
                if (r.value("command") != "start_listening") return;
                if (++subscriptions == 1) server.reply(r,server.nodes);
                else freshSubscription = r; // Hold the replacement subscription.
            };
            MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
            const auto oldDevice = client.devices().first();
            QSignalSpy finished(&client,&MatterClient::commandFinished), camera(&client,&MatterClient::cameraResponse);
            const auto controlId=client.control(oldDevice,"power",false);
            const auto cameraId=client.cameraRequest("camera_get_capabilities",{{"node_id",1},{"endpoint_id",2}});
            QTRY_COMPARE(server.requests.size(),3);
            server.info.insert(identityKey,"new-fabric");
            server.nodes = {QJsonObject{{"node_id",42},{"available",true},{"attributes",QJsonObject{
                {"0/40/5","New household"},{"2/29/0",QJsonArray{QJsonObject{{"deviceType",0x100}}}},
                {"2/29/1",QJsonArray{29,6}},{"2/6/0",false}}}}};
            server.send({{"event","server_info_updated"},{"data",QJsonObject{{identityKey,"new-fabric"}}}});
            QTRY_COMPARE(subscriptions,2);
            QVERIFY(!client.isConnected()); QVERIFY(client.devices().isEmpty());
            QCOMPARE(finished.size(),2); QCOMPARE(camera.size(),1);
            QSet<QString> failed;
            for(const auto &f:finished) { QVERIFY(!f.at(2).toBool()); failed.insert(f.at(0).toString()); }
            QCOMPARE(failed,(QSet<QString>{controlId,cameraId}));
            QCOMPARE(camera.first().at(0).toString(),cameraId); QVERIFY(!camera.first().at(2).toBool());
            server.send({{"message_id",controlId},{"result",QJsonValue(QJsonValue::Null)}});
            server.send({{"message_id",cameraId},{"result",QJsonObject{{"unexpected","old session"}}}});
            QTest::qWait(30); QCOMPARE(finished.size(),2); QCOMPARE(camera.size(),1); QVERIFY(client.devices().isEmpty());
            server.reply(freshSubscription,server.nodes); QTRY_VERIFY(client.isConnected());
            QCOMPARE(client.devices().size(),1); QCOMPARE(client.devices().first().nodeId,qint64(42));
            QCOMPARE(client.serverInfo().value(identityKey).toString(),QStringLiteral("new-fabric"));
            const int requests = server.requests.size(); client.control(oldDevice,"power",true);
            QTRY_COMPARE(finished.size(),4); QVERIFY(!finished.last().at(2).toBool()); QCOMPARE(server.requests.size(),requests);
        }
    }
    void droppedConnectionResubscribesAndManualDisconnectStopsRetries() {
        FakeServer server;
        MatterClient client;
        client.connectToServer(server.url());
        QTRY_VERIFY(client.isConnected());
        server.peer->close();
        QTRY_VERIFY(!client.isConnected());
        QVERIFY(client.isConnecting());
        for (const auto &device : client.devices()) QVERIFY(!device.available);
        QTRY_VERIFY_WITH_TIMEOUT(client.isConnected(), 2500);
        QCOMPARE(server.requests.size(), 2);
        QCOMPARE(server.requests.last().value("command").toString(), QStringLiteral("start_listening"));
        client.disconnectFromServer();
        QVERIFY(!client.isConnecting());
        QTest::qWait(1100);
        QCOMPARE(server.requests.size(), 2);
    }
    void genuineCameraRequiresTypeAndMandatoryServerClusters() {
        auto cameraNodes = alteredDemo([](QJsonObject &attrs) {
            attrs.insert("2/29/0", QJsonArray{QJsonObject{{"deviceType", 0x0142}, {"revision", 1}}});
            attrs.insert("2/29/1", QJsonArray{29, 57, 0x0551, 0x0553, 1030});
            attrs.insert("2/1030/0", 1);
        });
        const auto camera = devicesFromNodes(cameraNodes).first();
        QVERIFY(camera.hasCamera);
        QCOMPARE(camera.kind, QStringLiteral("camera"));
        QVERIFY(!camera.hasOnOff && !camera.hasLevel);
        QVERIFY(!devicesFromNodes(demoNodes()).first().hasCamera);
        for (const auto &servers : {QJsonArray{29, 57, 0x0551}, QJsonArray{29, 57, 0x0553}, QJsonArray{29, 57}}) {
            auto node = cameraNodes.first().toObject();
            auto attrs = node.value("attributes").toObject();
            attrs.insert("2/29/1", servers);
            attrs.insert("2/29/2", QJsonArray{0x0551, 0x0553}); // Client clusters do not qualify.
            node.insert("attributes", attrs);
            QVERIFY(!devicesFromNodes({node}).first().hasCamera);
        }
        auto noType = alteredDemo([](QJsonObject &attrs) {
            attrs.insert("2/29/1", QJsonArray{29, 57, 6, 0x0551, 0x0553});
            attrs.insert("2/57/5", "Named camera switch");
        });
        QVERIFY(!devicesFromNodes(noType).first().hasCamera);
    }
    void numericTlvDeviceTypesMatchActualServerWireFormat() {
        // Captured wire shape: Descriptor.DeviceTypeList uses numeric struct
        // tags, including the camera endpoint returned by matter-server 1.5.3.
        const QJsonObject cameraAttrs{
            {"0/40/5", "QA bridge"},
            {"1/29/0", QJsonArray{QJsonObject{{"0", 322}, {"1", 1}}}},
            {"1/29/1", QJsonArray{1361, 1363, 29}}
        };
        const auto camera = devicesFromNodes({QJsonObject{{"node_id", 1}, {"available", true}, {"attributes", cameraAttrs}}});
        QCOMPARE(camera.size(), 1);
        QCOMPARE(camera.first().endpointId, 1);
        QVERIFY(camera.first().hasCamera && camera.first().available);
        QCOMPARE(camera.first().kind, QStringLiteral("camera"));
        auto node = demoNodes().first().toObject();
        auto attrs = node.value("attributes").toObject();
        const auto keys = attrs.keys();
        for (const auto &key : keys) {
            if (!key.endsWith("/29/0")) continue;
            QJsonArray tagged;
            for (const auto &entry : attrs.value(key).toArray()) {
                const auto type = entry.toObject();
                tagged.append(QJsonObject{{"0", type.value("deviceType")}, {"1", type.value("revision")}});
            }
            attrs.insert(key, tagged);
        }
        node.insert("attributes", attrs);
        const auto devices = devicesFromNodes({node});
        QCOMPARE(devices.size(), 6); // Numeric-tag aggregator remains hidden.
        QCOMPARE(devices[0].kind, QStringLiteral("light"));
        QCOMPARE(devices[1].kind, QStringLiteral("outlet"));
        QCOMPARE(devices[2].kind, QStringLiteral("sensor"));
        QCOMPARE(devices[4].kind, QStringLiteral("sensor"));
        QVERIFY(devices[4].readings.join(' ').contains(QStringLiteral("门窗  关闭")));
        for (const auto &invalid : {QJsonValue("322"), QJsonValue(322.5), QJsonValue(true), QJsonValue(QJsonValue::Null)}) {
            auto invalidAttrs = cameraAttrs;
            invalidAttrs.insert("1/29/0", QJsonArray{QJsonObject{{"0", invalid}, {"1", 1}}});
            const auto invalidDevices = devicesFromNodes({QJsonObject{{"node_id", 1}, {"available", true}, {"attributes", invalidAttrs}}});
            QCOMPARE(invalidDevices.size(), 1);
            QVERIFY(!invalidDevices.first().hasCamera);
        }
    }
    void cameraReadinessSchemaAndWhitelistRejectAsynchronously() {
        MatterClient client;
        QSignalSpy responses(&client, &MatterClient::cameraResponse);
        const auto offlineId = client.cameraRequest("camera_snapshot", {{"node_id", 1}, {"endpoint_id", 2}});
        QCOMPARE(responses.size(), 0);
        QTRY_COMPARE(responses.size(), 1);
        QCOMPARE(responses.last().at(0).toString(), offlineId);
        QVERIFY(!responses.last().at(2).toBool());
        QCOMPARE(responses.last().at(4).toJsonObject().value("error_code").toInt(), 7);
        client.setDemoMode(true);
        client.cameraRequest("camera_start_stream", {});
        QTRY_COMPARE(responses.size(), 2);
        QVERIFY(!responses.last().at(2).toBool());
        FakeServer server;
        server.info.insert("schema_version", 13);
        client.connectToServer(server.url());
        QTRY_VERIFY(client.isConnected());
        client.cameraRequest("camera_get_capabilities", {{"node_id", 1}, {"endpoint_id", 2}});
        QTRY_COMPARE(responses.size(), 3);
        QCOMPARE(responses.last().at(4).toJsonObject().value("error_code").toInt(), 6);
        client.cameraRequest("device_command", {{"command_name", "On"}});
        QTRY_COMPARE(responses.size(), 4);
        QCOMPARE(responses.last().at(4).toJsonObject().value("error_code").toInt(), 9);
        QCOMPARE(server.requests.size(), 1); // Only the normal state subscription reached the server.
    }
    void cameraCommandsKeepWireArgumentsAndRawResults() {
        FakeServer server;
        QJsonValue result;
        server.handler = [&server, &result](const QJsonObject &request) {
            if (request.value("command") == "start_listening") { server.reply(request, server.nodes); return; }
            server.reply(request, result);
        };
        MatterClient client;
        client.connectToServer(server.url());
        QTRY_VERIFY(client.isConnected());
        QSignalSpy responses(&client, &MatterClient::cameraResponse);
        QSignalSpy logs(&client, &MatterClient::logMessage);
        const QList<QPair<QString, QJsonObject>> calls{
            {"camera_get_capabilities", {{"node_id", 1}, {"endpoint_id", 2}}},
            {"camera_start_stream", {{"node_id", 1}, {"endpoint_id", 2}, {"stream_usage", "LiveView"},
                {"sdp", "v=0\r\nm=video PRIVATE_SDP"}, {"video", QJsonObject{{"codecs", QJsonArray{"H264"}}}},
                {"audio", false}, {"client_session_key", "player-1"}, {"allow_eviction", false}}},
            {"camera_stop_stream", {{"node_id", 1}, {"endpoint_id", 2}, {"client_session_key", "player-1"}}},
            {"camera_provide_answer", {{"node_id", 1}, {"endpoint_id", 2}, {"webrtc_session_id", 42}, {"sdp", "v=0 PRIVATE_ANSWER"}}},
            {"camera_provide_ice_candidates", {{"node_id", 1}, {"endpoint_id", 2}, {"webrtc_session_id", 42},
                {"ice_candidates", QJsonArray{QJsonObject{{"candidate", "candidate:PRIVATE_ICE"}, {"sdpMid", "0"}, {"sdpMLineIndex", 0}}}}}},
            {"camera_snapshot", {{"node_id", 1}, {"endpoint_id", 2}, {"codec", "JPEG"},
                {"max_resolution", QJsonObject{{"width", 640}, {"height", 480}}}}},
            {"camera_release_stream", {{"node_id", 1}, {"endpoint_id", 2}, {"kind", "video"}, {"stream_id", 7}}}
        };
        for (const auto &call : calls) {
            result = call.first == "camera_start_stream"
                ? QJsonValue(QJsonObject{{"webrtc_session_id", 42}, {"mode", "provide_offer"},
                    {"video", QJsonObject{{"stream_id", 7}, {"codec", "H264"}, {"provenance", "allocated"}}},
                    {"audio", QJsonValue(QJsonValue::Null)}, {"talkback", false}})
                : call.first == "camera_snapshot"
                ? QJsonValue(QJsonObject{{"data", "PRIVATE_SNAPSHOT_BYTES"}, {"codec", "JPEG"}})
                : QJsonValue(QJsonValue::Null);
            const auto count = responses.size();
            const auto id = client.cameraRequest(call.first, call.second);
            QCOMPARE(responses.size(), count);
            QTRY_COMPARE(responses.size(), count + 1);
            QCOMPARE(server.requests.last().value("command").toString(), call.first);
            QCOMPARE(server.requests.last().value("args").toObject(), call.second);
            const auto response = responses.last();
            QCOMPARE(response.at(0).toString(), id);
            QCOMPARE(response.at(1).toString(), call.first);
            QVERIFY(response.at(2).toBool());
            QCOMPARE(response.at(3).value<QJsonValue>(), result);
            QVERIFY(response.at(4).toJsonObject().isEmpty());
        }
        QCOMPARE(logs.size(), 0); // Media and signalling are never activity-log payloads.
    }
    void cameraEventsPreserveSessionRoutingWithoutLogging() {
        FakeServer server;
        MatterClient client;
        client.connectToServer(server.url());
        QTRY_VERIFY(client.isConnected());
        QSignalSpy events(&client, &MatterClient::cameraEvent);
        QSignalSpy logs(&client, &MatterClient::logMessage);
        const QJsonObject offer{{"node_id", 1}, {"endpoint_id", 2}, {"webrtc_session_id", 42},
            {"fabric_index", 1}, {"event_type", "offer"}, {"client_session_key", "player-1"},
            {"data", QJsonObject{{"sdp", "v=0\r\nm=video PRIVATE_OFFER"}}}};
        const QJsonObject ended{{"node_id", 1}, {"endpoint_id", 2}, {"webrtc_session_id", 42},
            {"cause", "peer"}, {"reason", 0}, {"client_session_key", "player-1"}};
        server.send({{"event", "webrtc_callback"}, {"data", offer}});
        server.send({{"event", "camera_session_ended"}, {"data", ended}});
        QTRY_COMPARE(events.size(), 2);
        QCOMPARE(events[0].at(0).toString(), QStringLiteral("webrtc_callback"));
        QCOMPARE(events[0].at(1).toJsonObject(), offer);
        QCOMPARE(events[1].at(0).toString(), QStringLiteral("camera_session_ended"));
        QCOMPARE(events[1].at(1).toJsonObject(), ended);
        QCOMPARE(logs.size(), 0);
        server.send({{"event", "webrtc_callback"}, {"data", QJsonArray{1, 2}}});
        server.send({{"event", "unknown_media_event"}, {"data", offer}});
        QTest::qWait(25);
        QCOMPARE(events.size(), 2);
    }
    void cameraErrorsKeepStructuredFactsAndRedactPrivateData() {
        FakeServer server;
        const QJsonObject detail{{"message", "No codec supported by both the camera and caller"},
            {"reason", "codec"}, {"track", "video"}, {"device", QJsonArray{"H264"}}, {"requested", QJsonArray{"H265"}},
            {"sdp", "PRIVATE_SDP"}, {"ice_candidates", QJsonArray{"PRIVATE_ICE"}}};
        server.handler = [&server, detail](const QJsonObject &request) {
            if (request.value("command") == "start_listening") { server.reply(request, server.nodes); return; }
            server.send({{"message_id", request.value("message_id")}, {"error_code", 102},
                {"details", encode(detail)}, {"status", 129}, {"cluster_status", 1}});
        };
        MatterClient client;
        client.connectToServer(server.url());
        QTRY_VERIFY(client.isConnected());
        QSignalSpy responses(&client, &MatterClient::cameraResponse);
        QSignalSpy errors(&client, &MatterClient::errorOccurred);
        QSignalSpy logs(&client, &MatterClient::logMessage);
        client.cameraRequest("camera_start_stream", {{"node_id", 1}, {"endpoint_id", 2}, {"stream_usage", "LiveView"}});
        QTRY_COMPARE(responses.size(), 1);
        QVERIFY(!responses.first().at(2).toBool());
        const auto error = responses.first().at(4).toJsonObject();
        QCOMPARE(error.value("error_code").toInt(), 102);
        QCOMPARE(error.value("status").toInt(), 129);
        QCOMPARE(error.value("cluster_status").toInt(), 1);
        const auto parsed = QJsonDocument::fromJson(error.value("details").toString().toUtf8()).object();
        QCOMPARE(parsed.value("reason").toString(), QStringLiteral("codec"));
        QCOMPARE(parsed.value("track").toString(), QStringLiteral("video"));
        QCOMPARE(parsed.value("device").toArray(), QJsonArray{"H264"});
        QVERIFY(!encode(error).contains("PRIVATE_"));
        QCOMPARE(errors.size(), 1);
        QVERIFY(!errors.first().first().toString().contains("PRIVATE_"));
        QVERIFY(!errors.first().first().toString().contains("SDP"));
        QCOMPARE(logs.size(), 0);
    }
    void backgroundCameraErrorsStayOnDedicatedChannel() {
        FakeServer server;
        server.handler = [&server](const QJsonObject &request) {
            if (request.value("command") == "start_listening") server.reply(request, server.nodes);
            else server.send({{"message_id", request.value("message_id")}, {"error_code", 106}, {"details", "privacy"}});
        };
        MatterClient client;
        client.connectToServer(server.url());
        QTRY_VERIFY(client.isConnected());
        QSignalSpy responses(&client, &MatterClient::cameraResponse);
        QSignalSpy errors(&client, &MatterClient::errorOccurred);
        QSignalSpy finished(&client, &MatterClient::commandFinished);
        QSignalSpy logs(&client, &MatterClient::logMessage);
        client.cameraRequest("camera_snapshot", {{"node_id", 1}, {"endpoint_id", 2}}, 1000, true);
        QTRY_COMPARE(responses.size(), 1);
        QCOMPARE(responses.last().at(4).toJsonObject().value("error_code").toInt(), 106);
        server.handler = [](const QJsonObject &) {};
        client.cameraRequest("camera_snapshot", {{"node_id", 1}, {"endpoint_id", 2}}, 20, true);
        QTRY_COMPARE(responses.size(), 2);
        QCOMPARE(responses.last().at(4).toJsonObject().value("reason").toString(), QStringLiteral("timeout"));
        client.disconnectFromServer();
        client.cameraRequest("camera_snapshot", {}, 1000, true);
        QTRY_COMPARE(responses.size(), 3);
        QCOMPARE(errors.size(), 0);
        QCOMPARE(finished.size(), 0);
        QCOMPARE(logs.size(), 0);
    }
    void cameraTimeoutAndConnectionSwitchFailExactlyOnce() {
        FakeServer server;
        server.handler = [&server](const QJsonObject &request) {
            if (request.value("command") == "start_listening") server.reply(request, server.nodes);
        };
        MatterClient client;
        client.connectToServer(server.url());
        QTRY_VERIFY(client.isConnected());
        QSignalSpy responses(&client, &MatterClient::cameraResponse);
        const auto timeoutId = client.cameraRequest("camera_snapshot", {{"node_id", 1}, {"endpoint_id", 2}}, 30);
        QTRY_COMPARE(responses.size(), 1);
        QCOMPARE(responses.last().at(0).toString(), timeoutId);
        QCOMPARE(responses.last().at(4).toJsonObject().value("reason").toString(), QStringLiteral("timeout"));
        FakeServer second;
        const auto canceledId = client.cameraRequest("camera_start_stream", {{"node_id", 1}, {"endpoint_id", 2}, {"stream_usage", "LiveView"}}, 90000);
        client.connectToServer(second.url());
        QCOMPARE(responses.size(), 2);
        QCOMPARE(responses.last().at(0).toString(), canceledId);
        QCOMPARE(responses.last().at(4).toJsonObject().value("reason").toString(), QStringLiteral("connection-closed"));
        QTRY_VERIFY(client.isConnected());
        server.send({{"message_id", canceledId}, {"result", QJsonObject{{"webrtc_session_id", 999}}}});
        QTest::qWait(60);
        QCOMPARE(responses.size(), 2);
        QCOMPARE(second.requests.size(), 1); // Camera commands never replay on the new controller.
    }
    void richerHomeDemoPreservesMiniAndParsesCapabilities() {
        QCOMPARE(devicesFromNodes(demoNodes()).size(), 6);
        const auto devices = devicesFromNodes(homeDemoNodes());
        QCOMPARE(devices.size(), 12);
        const auto color = devices.at(6), lock = devices.at(7), cover = devices.at(8), thermostat = devices.at(9), fan = devices.at(10), air = devices.at(11);
        QVERIFY(color.controls.contains("color"));
        QVERIFY(qAbs(color.controlState.value("color").toObject().value("hue").toDouble()-210) < 0.1);
        QCOMPARE(lock.kind, QStringLiteral("lock")); QVERIFY(lock.controlState.value("lock").toBool());
        QCOMPARE(cover.kind, QStringLiteral("cover")); QCOMPARE(cover.controlState.value("position").toDouble(), 65.0);
        QCOMPARE(thermostat.kind, QStringLiteral("thermostat"));
        QCOMPARE(thermostat.controlLimits.value("thermostatMode").toArray(), (QJsonArray{"off","heat","cool","auto"}));
        QCOMPARE(thermostat.controlState.value("heatSetpoint").toDouble(), 21.0);
        QCOMPARE(fan.controlLimits.value("fanMode").toArray(), (QJsonArray{"off","low","medium","high","auto"}));
        QCOMPARE(air.controlState.value("co2").toDouble(), 650.0);
        QCOMPARE(air.controlState.value("pm25").toDouble(), 8.0);
        QVERIFY(devices.at(1).readings.join(' ').contains("1.245 kWh"));
        QCOMPARE(devices.at(2).controlState.value("temperature").toDouble(), 24.6);
    }
    void unknownAndReservedHomeStatesAreNeverCoerced() {
        auto node = homeDemoNodes().first().toObject(); auto attrs = node.value("attributes").toObject();
        attrs.insert("9/257/0", 255); attrs.insert("10/258/14", QJsonValue(QJsonValue::Null));
        attrs.insert("11/513/28", 99); attrs.insert("11/513/0", -32768); attrs.insert("12/514/2", QJsonValue(QJsonValue::Null));
        attrs.insert("13/1037/8", 255); attrs.insert("13/1066/0", QJsonValue(QJsonValue::Null));
        node.insert("attributes", attrs); const auto devices = devicesFromNodes({node});
        QVERIFY(!devices.at(7).controlState.contains("lock")); QCOMPARE(devices.at(7).controlState.value("lockState").toString(), QStringLiteral("unknown"));
        QVERIFY(!devices.at(8).controlState.contains("position"));
        QVERIFY(!devices.at(9).controlState.contains("thermostatMode")); QVERIFY(!devices.at(9).controlState.contains("temperature"));
        QVERIFY(!devices.at(10).controlState.contains("fanPercent"));
        QVERIFY(!devices.at(11).controlState.contains("co2")); QVERIFY(!devices.at(11).controlState.contains("pm25"));
        attrs.insert("9/257/0", 0); node.insert("attributes", attrs);
        const auto lock = devicesFromNodes({node}).at(7);
        QVERIFY(!lock.controlState.contains("lock")); QCOMPARE(lock.controlState.value("lockState").toString(), QStringLiteral("notFullyLocked"));
    }
    void serverFeaturesAndAcceptedCommandsGateNewControls() {
        auto node = homeDemoNodes().first().toObject(); auto attrs = node.value("attributes").toObject();
        attrs.insert("8/768/65529", QJsonArray{10}); // no hue invoke despite cached attributes
        attrs.insert("9/257/2", false);
        attrs.insert("10/258/65532", 1); // Lift without position awareness
        attrs.insert("11/513/65532", 1); attrs.insert("11/513/27", 2);
        attrs.insert("12/514/1", 5);
        node.insert("attributes", attrs); const auto devices = devicesFromNodes({node});
        QVERIFY(!devices.at(6).controls.contains("color")); QVERIFY(!devices.at(7).controls.contains("lock"));
        QVERIFY(!devices.at(8).controls.contains("position"));
        QCOMPARE(devices.at(9).controlLimits.value("thermostatMode").toArray(), (QJsonArray{"off","heat"}));
        QVERIFY(!devices.at(9).controls.contains("coolSetpoint"));
        QCOMPARE(devices.at(10).controlLimits.value("fanMode").toArray(), (QJsonArray{"off","high"}));
        attrs.insert("10/29/1", QJsonArray{29,57}); attrs.insert("10/29/2", QJsonArray{258});
        attrs.insert("12/29/1", QJsonArray{29,57}); attrs.insert("12/29/2", QJsonArray{514});
        node.insert("attributes", attrs); const auto clients = devicesFromNodes({node});
        QVERIFY(clients.at(8).controls.isEmpty()); QVERIFY(clients.at(10).controls.isEmpty());
    }
    void newInvokeWirePayloadsAndNoOptimisticState() {
        FakeServer server; server.nodes = homeDemoNodes();
        MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        auto invoke = [&](int index, const QString &action, QJsonValue target) {
            const int before = server.requests.size(); client.control(client.devices().at(index), action, target);
            QTest::qWait(35); QCOMPARE(server.requests.size(), before+1);
        };
        invoke(7,"lock",false);
        auto args = server.requests.last().value("args").toObject();
        QCOMPARE(args.value("command_name").toString(), QStringLiteral("UnlockDoor"));
        QCOMPARE(args.value("timed_request_timeout_ms").toInt(), 10000);
        QVERIFY(args.value("payload").toObject().isEmpty()); QVERIFY(client.devices().at(7).controlState.value("lock").toBool());
        invoke(8,"position",25);
        args = server.requests.last().value("args").toObject();
        QCOMPARE(args.value("cluster_id").toInt(),258);
        QCOMPARE(args.value("command_name").toString(),QStringLiteral("GoToLiftPercentage"));
        QCOMPARE(args.value("payload").toObject().value("liftPercent100thsValue").toInt(),7500);
        QCOMPARE(client.devices().at(8).controlState.value("position").toDouble(),65.0);
        invoke(6,"color",QJsonObject{{"hue",180},{"saturation",50}});
        args = server.requests.last().value("args").toObject();
        QCOMPARE(args.value("command_name").toString(),QStringLiteral("EnhancedMoveToHueAndSaturation"));
        QCOMPARE(args.value("payload").toObject().value("enhancedHue").toInt(),32768);
        QCOMPARE(args.value("payload").toObject().value("saturation").toInt(),127);
    }
    void hueFallsBackToStandardWhenEnhancedCommandIsNotAccepted() {
        FakeServer server; auto node = homeDemoNodes().first().toObject(); auto attrs = node.value("attributes").toObject();
        attrs.insert("8/768/65529",QJsonArray{6,10}); node.insert("attributes",attrs); server.nodes = {node};
        MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        client.control(client.devices().at(6),"color",QJsonObject{{"hue",180},{"saturation",100}});
        QTRY_COMPARE(server.requests.size(),2);
        const auto args = server.requests.last().value("args").toObject();
        QCOMPARE(args.value("command_name").toString(),QStringLiteral("MoveToHueAndSaturation"));
        QCOMPARE(args.value("payload").toObject().value("hue").toInt(),127);
    }
    void writesValidatePathStatusAndAwaitSubscriptionState() {
        FakeServer server; server.nodes = homeDemoNodes();
        server.handler = [&server](const QJsonObject &r) {
            if (r.value("command") == "start_listening") { server.reply(r,server.nodes); return; }
            if (r.value("command") != "write_attribute") { server.reply(r,QJsonValue(QJsonValue::Null)); return; }
            const auto a = r.value("args").toObject(); const auto p = a.value("attribute_path").toString().split('/');
            server.reply(r,QJsonArray{QJsonObject{{"Path",QJsonObject{{"EndpointId",p[0].toInt()},{"ClusterId",p[1].toInt()},{"AttributeId",p[2].toInt()}}},{"Status",0}}});
        };
        MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QSignalSpy finished(&client,&MatterClient::commandFinished);
        const auto id = client.control(client.devices().at(9),"heatSetpoint",22.5);
        QTRY_COMPARE(finished.size(),1); QVERIFY(finished.first().at(2).toBool()); QCOMPARE(finished.first().at(0).toString(),id);
        auto args = server.requests.last().value("args").toObject();
        QCOMPARE(args.value("attribute_path").toString(),QStringLiteral("11/513/18")); QCOMPARE(args.value("value").toInt(),2250);
        QCOMPARE(client.devices().at(9).controlState.value("heatSetpoint").toDouble(),21.0);
        client.control(client.devices().at(9),"thermostatMode","heat"); QTRY_COMPARE(finished.size(),2);
        args = server.requests.last().value("args").toObject(); QCOMPARE(args.value("attribute_path").toString(),QStringLiteral("11/513/28")); QCOMPARE(args.value("value").toInt(),4);
        client.control(client.devices().at(10),"fanPercent",60); QTRY_COMPARE(finished.size(),3);
        args = server.requests.last().value("args").toObject(); QCOMPARE(args.value("attribute_path").toString(),QStringLiteral("12/514/2")); QCOMPARE(args.value("value").toInt(),60);
        client.control(client.devices().at(10),"fanMode","auto"); QTRY_COMPARE(finished.size(),4);
        args = server.requests.last().value("args").toObject(); QCOMPARE(args.value("attribute_path").toString(),QStringLiteral("12/514/0")); QCOMPARE(args.value("value").toInt(),5);
        server.send({{"event","attribute_updated"},{"data",QJsonArray{1,"11/513/18",2250}}});
        QTRY_COMPARE(client.devices().at(9).controlState.value("heatSetpoint").toDouble(),22.5);
    }
    void failedOrMalformedWriteStatusesAreFailures() {
        for (const auto &result : {QJsonValue(QJsonArray{QJsonObject{{"Path",QJsonObject{{"EndpointId",12},{"ClusterId",514},{"AttributeId",2}}},{"Status",135}}}),QJsonValue(QJsonArray{}),QJsonValue(QJsonValue::Null),
                QJsonValue(QJsonArray{QJsonObject{{"Path",QJsonObject{{"EndpointId",99},{"ClusterId",514},{"AttributeId",2}}},{"Status",0}}})}) {
            FakeServer server; server.nodes=homeDemoNodes(); server.handler = [&server,result](const QJsonObject &r) {
                server.reply(r,r.value("command")=="start_listening"?QJsonValue(server.nodes):result);
            };
            MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
            QSignalSpy finished(&client,&MatterClient::commandFinished); client.control(client.devices().at(10),"fanPercent",50);
            QTRY_COMPARE(finished.size(),1); QVERIFY(!finished.first().at(2).toBool());
            QCOMPARE(client.devices().at(10).controlState.value("fanPercent").toInt(),35);
        }
    }
    void invalidControlsCannotReachWireAndCurrentLimitsWin() {
        FakeServer server; server.nodes=homeDemoNodes(); MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QSignalSpy finished(&client,&MatterClient::commandFinished);
        const auto d=client.devices();
        const QList<QPair<int,QPair<QString,QJsonValue>>> bad{{7,{"lock",0}},{8,{"position",101}},{9,{"thermostatMode","emergencyHeat"}},
            {9,{"heatSetpoint",24}},{9,{"coolSetpoint",18}},{10,{"fanMode","turbo"}},{10,{"fanPercent",-1}},
            {6,{"color",QJsonObject{{"hue",361},{"saturation",50}}}},{6,{"color",QJsonObject{{"hue",180}}}},{0,{"brightness",1.5}},
            {0,{"power",QStringLiteral("true")}},{0,{"write_attribute",QJsonObject{{"value",1}}}}};
        for(const auto &b:bad) client.control(d.at(b.first),b.second.first,b.second.second);
        auto forged=d.first(); forged.nodeId=99; client.control(forged,"power",false);
        auto stale=d.first(); stale.attributes.insert("2/768/16396",600);
        server.send({{"event","attribute_updated"},{"data",QJsonArray{1,"2/768/16396",400}}});
        QTRY_COMPARE(client.devices().first().controlLimits.value("colorTemperature").toObject().value("max").toInt(),400);
        client.control(stale,"colorTemperature",450);
        QTRY_COMPARE(finished.size(),bad.size()+2);
        for(const auto &f:finished) QVERIFY(!f.at(2).toBool());
        QCOMPARE(server.requests.size(),1);
        server.send({{"event","attribute_updated"},{"data",QJsonArray{1,"9/57/17",false}}}); QTRY_VERIFY(!client.devices().at(7).available);
        client.control(d.at(7),"lock",false); QTRY_COMPARE(finished.size(),bad.size()+3); QCOMPARE(server.requests.size(),1);
    }
    void richDemoControlUpdatesUnderlyingAttributes() {
        MatterClient client;
        bool readyHasAllDevices = true;
        connect(&client, &MatterClient::connectionChanged, this, [&] {
            if (client.isConnected()) readyHasAllDevices &= client.devices().size() == 12;
        });
        client.setHomeDemoMode(true); QCOMPARE(client.devices().size(),12); QVERIFY(readyHasAllDevices);
        QSignalSpy finished(&client,&MatterClient::commandFinished);
        client.control(client.devices().at(7),"lock",false); client.control(client.devices().at(8),"position",20);
        client.control(client.devices().at(9),"heatSetpoint",22); client.control(client.devices().at(10),"fanMode","auto");
        client.control(client.devices().at(6),"color",QJsonObject{{"hue",90},{"saturation",50}});
        QVERIFY(client.devices().at(7).controlState.value("lock").toBool());
        QTRY_COMPARE(finished.size(),5); for(const auto &f:finished) QVERIFY(f.at(2).toBool());
        QVERIFY(!client.devices().at(7).controlState.value("lock").toBool());
        QCOMPARE(client.devices().at(8).controlState.value("position").toDouble(),20.0);
        QCOMPARE(client.devices().at(9).controlState.value("heatSetpoint").toDouble(),22.0);
        QCOMPARE(client.devices().at(10).controlState.value("fanMode").toString(),QStringLiteral("auto"));
        QVERIFY(qAbs(client.devices().at(6).controlState.value("color").toObject().value("hue").toDouble()-90)<0.01);
        client.setDemoMode(true); QCOMPARE(client.devices().size(),6);
    }
    void demoUsesStateUpdatesAndCannotCommission() {
        MatterClient client;
        client.setDemoMode(true);
        QVERIFY(client.isConnected() && client.isDemoMode());
        QSignalSpy finished(&client, &MatterClient::commandFinished);
        client.setOnOff(client.devices().first(), false);
        QVERIFY(client.devices().first().on);
        QTRY_VERIFY(!client.devices().first().on);
        QVERIFY(finished.last().at(2).toBool());
        client.commission("35325335079");
        QTRY_COMPARE(finished.size(), 2);
        QVERIFY(!finished.last().at(2).toBool());
        client.setDemoMode(false);
        QVERIFY(!client.isDemoMode() && !client.isConnected());
        QVERIFY(client.devices().isEmpty());
    }
};

QTEST_GUILESS_MAIN(MatterProtocolTest)
#include "tst_matterprotocol.moc"
