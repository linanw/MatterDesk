#include "homestore.h"

#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWebSocket>
#include <QWebSocketServer>

namespace {
class FakeServer : public QObject {
public:
    QWebSocketServer server{QStringLiteral("HomeStore test"), QWebSocketServer::NonSecureMode};
    QWebSocket *peer = nullptr;
    QJsonArray nodes = demoNodes();
    QList<QJsonObject> commands;
    int failCommand = -1;
    bool holdCommands = false;
    FakeServer() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QWebSocketServer::newConnection, this, [this] {
            peer = server.nextPendingConnection(); peer->setParent(this);
            connect(peer, &QWebSocket::textMessageReceived, this, [this](const QString &text) {
                const auto request = QJsonDocument::fromJson(text.toUtf8()).object();
                const auto command = request.value("command").toString();
                if (command == "device_command") {
                    commands.append(request);
                    if (holdCommands) return;
                    if (commands.size() == failCommand) {
                        send({{"message_id", request.value("message_id")}, {"error_code", 4}, {"details", "fixture failure"}}); return;
                    }
                }
                send({{"message_id", request.value("message_id")},
                      {"result", command == "start_listening" || command == "get_nodes" ? QJsonValue(nodes) : QJsonValue(QJsonValue::Null)}});
            });
            send({{"schema_version", 14}, {"min_supported_schema_version", 11}, {"sdk_version", "test"}});
        });
    }
    QUrl url() const { return QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(server.serverPort())); }
    void send(const QJsonObject &message) { if (peer) peer->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact))); }
    void power(bool on) { send({{"event", "attribute_updated"}, {"data", QJsonArray{1, "2/6/0", on}}}); }
};
QJsonObject scene(const QString &name = QStringLiteral("阅读"), const QJsonArray &actions = {}) {
    return {{"name", name}, {"actions", actions.isEmpty()
        ? QJsonArray{QJsonObject{{"deviceKey", "1/2"}, {"action", "power"}, {"value", true}}} : actions}};
}
QJsonObject dailyRule(const QString &sceneId, const QDateTime &when, bool enabled = true) {
    return {{"name", QStringLiteral("定时阅读")}, {"enabled", enabled}, {"sceneId", sceneId},
            {"trigger", QJsonObject{{"type", "time"}, {"time", when.time().toString(QStringLiteral("HH:mm"))},
                                    {"days", QJsonArray{when.date().dayOfWeek()}}}}};
}
}

class HomeStoreTest : public QObject {
    Q_OBJECT
private slots:
    void metadataPersistsAndProfilesAreIsolated() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        MatterClient client;
        {
            HomeStore store(&client, directory.path()); store.setProfile(QStringLiteral("controller-one/fabric-a"));
            QVERIFY(store.rooms().isEmpty()); QVERIFY(store.scenes().isEmpty());
            QVERIFY(store.setHomeName(QStringLiteral("林家")));
            const auto zone = store.saveZone({{"name", "楼上"}}); QVERIFY(!zone.isEmpty());
            const auto room = store.saveRoom({{"name", "卧室"}, {"zoneId", zone}}); QVERIFY(!room.isEmpty());
            QVERIFY(store.setAccessory(QStringLiteral("1/2"), {{"name", "床头灯"}, {"roomId", room}, {"favorite", true}}));
            QVERIFY(store.setAccessory(QStringLiteral("1/2"), {{"hidden", true}}));
            QVERIFY(store.accessory(QStringLiteral("1/2")).value("favorite").toBool());
            store.setProfile(QStringLiteral("controller-two/fabric-b"));
            QVERIFY(store.rooms().isEmpty()); QVERIFY(store.accessory(QStringLiteral("1/2")).isEmpty());
            store.setProfile(QStringLiteral("controller-one/fabric-a"));
            QCOMPARE(store.homeName(), QStringLiteral("林家")); QCOMPARE(store.rooms().size(), 1);
        }
        HomeStore restored(&client, directory.path()); restored.setProfile(QStringLiteral("controller-one/fabric-a"));
        QCOMPARE(restored.homeName(), QStringLiteral("林家"));
        QVERIFY(restored.accessory(QStringLiteral("1/2")).value("hidden").toBool());
        QVERIFY(restored.removeZone(restored.zones().first().toObject().value("id").toString()));
        QVERIFY(!restored.rooms().first().toObject().contains("zoneId"));
        QVERIFY(restored.removeRoom(restored.rooms().first().toObject().value("id").toString()));
        QVERIFY(!restored.accessory(QStringLiteral("1/2")).contains("roomId"));
        restored.setProfile({}); QVERIFY(restored.rooms().isEmpty());
        QVERIFY(!restored.setAccessory(QStringLiteral("1/2"), {{"favorite", true}}));
    }
    void validatesConfigurationAndPreservesCorruptFile() {
        QTemporaryDir directory; MatterClient client;
        HomeStore store(&client, directory.path()); store.setProfile(QStringLiteral("validation"));
        QVERIFY(store.saveRoom({{"name", "\n"}}).isEmpty());
        QVERIFY(store.saveRoom({{"name", "房间"}, {"zoneId", "missing"}}).isEmpty());
        QVERIFY(!store.setAccessory(QStringLiteral("../../etc/passwd"), {{"name", "bad"}}));
        QVERIFY(!store.setAccessory(QStringLiteral("1/2"), {{"favorite", "yes"}}));
        QVERIFY(store.saveScene(scene(QStringLiteral("开锁"), {QJsonObject{{"deviceKey", "1/2"}, {"action", "lock"}, {"value", false}}})).isEmpty());
        QVERIFY(store.saveScene(scene(QStringLiteral("小数亮度"), {QJsonObject{{"deviceKey", "1/2"}, {"action", "brightness"}, {"value", 30.5}}})).isEmpty());
        const auto saved = store.saveScene(scene()); QVERIFY(!saved.isEmpty());
        QVERIFY(store.saveRule({{"name", "rule"}, {"sceneId", saved}, {"trigger", QJsonObject{{"type", "time"}, {"time", "24:00"}, {"days", QJsonArray{1}}}}}).isEmpty());
        const auto files = QDir(directory.path()).entryList({"*.json"}, QDir::Files); QCOMPARE(files.size(), 1);
        QFile file(QDir(directory.path()).filePath(files.first())); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("{broken"); file.close();
        HomeStore restored(&client, directory.path()); QSignalSpy errors(&restored, &HomeStore::errorOccurred);
        restored.setProfile(QStringLiteral("validation")); QCOMPARE(errors.size(), 1);
        QVERIFY(!restored.setHomeName(QStringLiteral("replacement")));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("{broken"));
    }
    void sceneRunsSequentiallyAndReportsRealFailure() {
        QTemporaryDir directory; FakeServer server; MatterClient client;
        HomeStore store(&client, directory.path()); store.setProfile(QStringLiteral("scene-run"));
        client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        server.failCommand = 2;
        const auto id = store.saveScene(scene(QStringLiteral("三步场景"), {
            QJsonObject{{"deviceKey", "1/2"}, {"action", "power"}, {"value", false}},
            QJsonObject{{"deviceKey", "1/2"}, {"action", "brightness"}, {"value", 30}},
            QJsonObject{{"deviceKey", "1/2"}, {"action", "power"}, {"value", true}}}));
        QVERIFY(!id.isEmpty()); QSignalSpy finished(&store, &HomeStore::executionFinished);
        QVERIFY(store.runScene(id)); QVERIFY(store.isExecuting());
        QTRY_COMPARE(finished.size(), 1); QVERIFY(!finished.first()[1].toBool());
        QCOMPARE(server.commands.size(), 2); QVERIFY(!store.isExecuting());
        QCOMPARE(store.activity().size(), 1); QVERIFY(!store.activity().first().toObject().value("success").toBool());
        const auto history = QJsonDocument(store.activity()).toJson();
        QVERIFY(!history.contains("fixture failure")); QVERIFY(!history.contains("deviceKey")); QVERIFY(!history.contains("value"));
        server.failCommand = -1; QVERIFY(store.runScene(id)); QTRY_COMPARE(finished.size(), 2);
        QVERIFY(finished.last()[1].toBool()); QCOMPARE(server.commands.size(), 5);
    }
    void preflightRejectsWholeSceneBeforeSendingAnyCommand() {
        QTemporaryDir directory; FakeServer server; MatterClient client;
        HomeStore store(&client, directory.path()); store.setProfile(QStringLiteral("preflight"));
        client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        const auto id = store.saveScene(scene(QStringLiteral("不可执行"), {
            QJsonObject{{"deviceKey", "1/2"}, {"action", "power"}, {"value", false}},
            QJsonObject{{"deviceKey", "999/1"}, {"action", "power"}, {"value", true}}}));
        QVERIFY(!id.isEmpty()); QVERIFY(!store.runScene(id)); QCOMPARE(server.commands.size(), 0);
        const auto unsupported = store.saveScene(scene(QStringLiteral("错误能力"), {
            QJsonObject{{"deviceKey", "1/3"}, {"action", "brightness"}, {"value", 30}}}));
        QVERIFY(!store.runScene(unsupported)); QCOMPARE(server.commands.size(), 0);
        server.nodes = homeDemoNodes(); client.refresh(); QTRY_VERIFY(client.devices().size() > 6);
        const auto deadband = store.saveScene(scene(QStringLiteral("温差过小"), {
            QJsonObject{{"deviceKey", "1/2"}, {"action", "power"}, {"value", false}},
            QJsonObject{{"deviceKey", "1/11"}, {"action", "heatSetpoint"}, {"value", 24}}}));
        QVERIFY(!store.runScene(deadband)); QCOMPARE(server.commands.size(), 0);
    }
    void contextSwitchCancelsRemainingCommands() {
        QTemporaryDir directory; FakeServer server; MatterClient client;
        HomeStore store(&client, directory.path()); store.setProfile(QStringLiteral("before"));
        client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected()); server.holdCommands = true;
        const auto id = store.saveScene(scene(QStringLiteral("切换测试"), {
            QJsonObject{{"deviceKey", "1/2"}, {"action", "power"}, {"value", false}},
            QJsonObject{{"deviceKey", "1/2"}, {"action", "power"}, {"value", true}}}));
        QSignalSpy finished(&store, &HomeStore::executionFinished); QVERIFY(store.runScene(id));
        QTRY_COMPARE(server.commands.size(), 1); store.setProfile(QStringLiteral("after"));
        QCOMPARE(finished.size(), 1); QVERIFY(!finished.first()[1].toBool());
        server.send({{"message_id", server.commands.first().value("message_id")}, {"result", QJsonValue(QJsonValue::Null)}});
        QTest::qWait(80); QCOMPARE(server.commands.size(), 1); QVERIFY(store.activity().isEmpty());
    }
    void disconnectStopsExecutionAndHistoryIsBounded() {
        QTemporaryDir directory; FakeServer server; MatterClient client;
        HomeStore store(&client, directory.path()); store.setProfile(QStringLiteral("disconnect"));
        client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        const auto device = client.devices().first();
        QSignalSpy commands(&client, &MatterClient::commandFinished);
        for (int i = 0; i < 205; ++i) client.control(device, QStringLiteral("power"), bool(i % 2));
        QTRY_COMPARE(commands.size(), 205); QCOMPARE(store.activity().size(), 200);
        const auto entry = store.activity().first().toObject(); QCOMPARE(entry.size(), 4);
        QVERIFY(entry.value("success").toBool());
        store.clearActivity(); QVERIFY(store.activity().isEmpty());
        server.holdCommands = true;
        const auto id = store.saveScene(scene()); QSignalSpy finished(&store, &HomeStore::executionFinished);
        QVERIFY(store.runScene(id)); QTRY_COMPARE(server.commands.size(), 206);
        client.disconnectFromServer(); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.first()[1].toBool()); QVERIFY(!store.isExecuting());
        QCOMPARE(store.activity().size(), 1); QVERIFY(!store.activity().first().toObject().value("success").toBool());
    }
    void timeAutomationRunsOncePerDayAcrossRestart() {
        QTemporaryDir directory; FakeServer server; MatterClient client;
        client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        const auto when = QDateTime(QDate(2030, 1, 7), QTime(19, 30));
        QString ruleId;
        {
            HomeStore store(&client, directory.path()); store.setProfile(QStringLiteral("daily"));
            const auto sceneId = store.saveScene(scene());
            auto rule = dailyRule(sceneId, when); rule.remove("enabled");
            ruleId = store.saveRule(rule); QVERIFY(!ruleId.isEmpty());
            store.evaluateAutomations(when); QTest::qWait(30); QCOMPARE(server.commands.size(), 0);
            rule = store.rules().first().toObject(); rule.insert("enabled", true); QVERIFY(!store.saveRule(rule).isEmpty());
            QSignalSpy finished(&store, &HomeStore::executionFinished);
            store.evaluateAutomations(when); QTRY_COMPARE(finished.size(), 1); QCOMPARE(server.commands.size(), 1);
            store.evaluateAutomations(when.addSecs(15)); QTest::qWait(30); QCOMPARE(server.commands.size(), 1);
        }
        HomeStore restored(&client, directory.path()); restored.setProfile(QStringLiteral("daily"));
        restored.evaluateAutomations(when.addSecs(30)); QTest::qWait(30); QCOMPARE(server.commands.size(), 1);
        auto rule = restored.rules().first().toObject(); auto trigger = rule.value("trigger").toObject();
        trigger.insert("days", QJsonArray{when.addDays(1).date().dayOfWeek()}); rule.insert("trigger", trigger);
        QVERIFY(!restored.saveRule(rule).isEmpty());
        QSignalSpy finished(&restored, &HomeStore::executionFinished);
        restored.evaluateAutomations(when.addDays(1)); QTRY_COMPARE(finished.size(), 1); QCOMPARE(server.commands.size(), 2);
    }
    void firstActualEdgeAfterReconnectOrProfileLoadIsNotLost() {
        for (const bool reconnect : {false, true}) {
            QTemporaryDir directory; FakeServer server; MatterClient client;
            auto node = server.nodes.first().toObject(); auto attrs = node.value("attributes").toObject();
            attrs.insert("2/6/0", false); node.insert("attributes", attrs); server.nodes.replace(0, node);
            HomeStore store(&client, directory.path()); store.setProfile(QStringLiteral("baseline"));
            client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
            QVERIFY(!client.devices().first().on);
            const auto sceneId = store.saveScene(scene());
            QVERIFY(!store.saveRule({{"name", "首次真实变化"}, {"enabled", true}, {"sceneId", sceneId},
                {"trigger", QJsonObject{{"type", "device"}, {"deviceKey", "1/2"}, {"action", "power"}, {"value", true}}}}).isEmpty());
            if (reconnect) {
                client.disconnectFromServer(); client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
            } else {
                store.setProfile(QStringLiteral("another-profile")); store.setProfile(QStringLiteral("baseline"));
            }
            QTest::qWait(30); QCOMPARE(server.commands.size(), 0);
            QSignalSpy finished(&store, &HomeStore::executionFinished);
            server.power(true); QTRY_COMPARE(finished.size(), 1);
            QVERIFY(finished.first()[1].toBool()); QCOMPARE(server.commands.size(), 1);
        }
    }
    void deviceAutomationRequiresEdgeAndHasCooldown() {
        QTemporaryDir directory; FakeServer server; MatterClient client;
        HomeStore store(&client, directory.path()); store.setProfile(QStringLiteral("edge"));
        client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        const auto sceneId = store.saveScene(scene());
        const auto ruleId = store.saveRule({{"name", "灯亮触发"}, {"enabled", true}, {"sceneId", sceneId},
            {"trigger", QJsonObject{{"type", "device"}, {"deviceKey", "1/2"}, {"action", "power"}, {"value", true}}}});
        QVERIFY(!ruleId.isEmpty()); QSignalSpy finished(&store, &HomeStore::executionFinished);
        server.power(true); QTest::qWait(30); QCOMPARE(server.commands.size(), 0);
        server.power(false); QTRY_VERIFY(!client.devices().first().on);
        server.power(true); QTRY_COMPARE(finished.size(), 1); QCOMPARE(server.commands.size(), 1);
        server.power(false); QTRY_VERIFY(!client.devices().first().on);
        server.power(true); QTRY_VERIFY(client.devices().first().on); QTest::qWait(30); QCOMPARE(server.commands.size(), 1);
        // Initial true state on a reconnect is a baseline, not a rising edge.
        client.disconnectFromServer(); client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QTest::qWait(30); QCOMPARE(server.commands.size(), 1);
    }
};
QTEST_GUILESS_MAIN(HomeStoreTest)
#include "tst_homestore.moc"
