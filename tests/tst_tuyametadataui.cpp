#include "matterclient.h"
#include "tuyametadata.h"
#include "tuyametadatapanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>
#include <memory>

namespace {
class Controller : public QObject {
public:
    QWebSocketServer server{QStringLiteral("Tuya panel controller fixture"), QWebSocketServer::NonSecureMode};
    QPointer<QWebSocket> peer;
    QJsonObject node;
    int mutations = 0;
    Controller() {
        QJsonObject attributes{{"0/40/5", "Synthetic bridge"}};
        for (int endpoint : {1, 2}) {
            const auto prefix = QString::number(endpoint) + '/';
            attributes.insert(prefix + "29/0", QJsonArray{QJsonObject{{"deviceType", 0x010a}, {"revision", 1}}});
            attributes.insert(prefix + "29/1", QJsonArray{29, 57, 6});
            attributes.insert(prefix + "57/5", QString("Native endpoint %1").arg(endpoint));
            attributes.insert(prefix + "57/18", QString("tuya-panel-native-%1").arg(endpoint));
            attributes.insert(prefix + "57/17", true);
            attributes.insert(prefix + "6/0", false);
        }
        node = {{"node_id", 7}, {"available", true}, {"attributes", attributes}};
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QWebSocketServer::newConnection, this, [this] {
            peer = server.nextPendingConnection(); peer->setParent(this);
            connect(peer, &QWebSocket::textMessageReceived, this, [this](const QString &text) {
                const auto request = QJsonDocument::fromJson(text.toUtf8()).object();
                const auto command = request.value("command").toString();
                QJsonValue result = QJsonValue(QJsonValue::Null);
                if (command == "start_listening" || command == "get_nodes") result = QJsonArray{node};
                else if (command == "get_node") result = node;
                else ++mutations;
                send({{"message_id", request.value("message_id")}, {"result", result}});
            });
            send({{"schema_version", 14}, {"min_supported_schema_version", 11}, {"fabric_id", "tuya-panel-test-fabric"}});
        });
    }
    void send(const QJsonObject &message) {
        if (peer) peer->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }
    QUrl url() const { return QUrl(QString("ws://127.0.0.1:%1/ws").arg(server.serverPort())); }
};

const QString SourceHome = "h_" + QString(64, 'b');
const QString SourceRoom = "r_" + QString(64, 'c');
const QString SourceDevice = "d_" + QString(64, 'a');
const QString ForeignHome = "h_" + QString(64, 'd');
const QString ForeignRoom = "r_" + QString(64, 'e');
const QString RoomToken = QStringLiteral("synthetic-room-panel-token-with-at-least-32-characters");

MatterDevice roomEndpoint() {
    MatterDevice device;
    device.key = "7/1"; device.nodeId = 7; device.endpointId = 1;
    device.name = "Native short label"; device.available = true;
    device.attributes = {{"1/57/18", "room-panel-native-identity"}};
    return device;
}

QJsonObject roomSnapshot() {
    return {{"schemaVersion", 1}, {"serverId", "8c4eec7f-7f15-4507-b7ac-5189b3b821a0"},
        {"sourceEpoch", "6b25f764-1d0e-43c0-a129-4ab09b158c83"}, {"revision", "1"},
        {"observedAt", "2026-10-24T09:00:00.000Z"}, {"available", true}, {"roomsAvailable", true},
        {"sourceKind", "tuya-project-paas"},
        {"capabilities", QJsonObject{{"read", true}, {"renameDevice", false}, {"moveDevice", false}, {"renameRoom", true}}},
        {"limitations", QJsonObject{}},
        {"homes", QJsonArray{QJsonObject{{"id", SourceHome}, {"name", "Selected home"}}, QJsonObject{{"id", ForeignHome}, {"name", "Other home"}}}},
        {"rooms", QJsonArray{QJsonObject{{"id", SourceRoom}, {"homeId", SourceHome}, {"name", "Study"}},
            QJsonObject{{"id", ForeignRoom}, {"homeId", ForeignHome}, {"name", "Foreign room"}}}},
        {"devices", QJsonArray{QJsonObject{{"id", SourceDevice}, {"name", "Source light"}, {"homeId", SourceHome},
            {"roomId", SourceRoom}, {"roomStatus", "assigned"}, {"bindings", QJsonObject{
                {"hapSerialNumber", QJsonValue(QJsonValue::Null)}, {"hapAccessoryUuid", QJsonValue(QJsonValue::Null)},
                {"matterUniqueIds", QJsonArray{"room-panel-native-identity"}}}}}}}};
}

class RoomMetadataServer : public QObject {
public:
    QTcpServer server;
    QJsonObject snapshot = roomSnapshot();
    QList<QJsonObject> commands;
    bool uncertain = false;
    RoomMetadataServer() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection(); socket->setParent(this);
                auto bytes = std::make_shared<QByteArray>(); auto handled = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, bytes, handled] {
                    bytes->append(socket->readAll()); if (*handled) return;
                    const int boundary = bytes->indexOf("\r\n\r\n"); if (boundary < 0) return;
                    const auto lines = bytes->left(boundary).split('\n'); int length = 0; bool authorized = false;
                    for (auto line : lines) {
                        line = line.trimmed();
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                        if (line.toLower().startsWith("authorization:")) authorized = line.mid(14).trimmed() == "Bearer " + RoomToken.toUtf8();
                    }
                    if (bytes->size() < boundary + 4 + length) return;
                    *handled = true;
                    if (!authorized) { reply(socket, 401, {{"schemaVersion", 1}, {"code", "permission_denied"}}); return; }
                    const auto first = lines.first().trimmed().split(' ');
                    const auto method = first.value(0), path = first.value(1);
                    if (method == "GET" && path == "/v1/capabilities") {
                        auto caps = snapshot; for (const auto &key : {"homes", "rooms", "devices"}) caps.remove(key);
                        reply(socket, 200, caps);
                    } else if (method == "GET" && path == "/v1/metadata") reply(socket, 200, snapshot);
                    else if (method == "POST" && path == "/v1/metadata/commands") {
                        const auto command = QJsonDocument::fromJson(bytes->mid(boundary + 4, length)).object();
                        commands.append(command);
                        const auto operation = command.value("operation").toObject();
                        const QJsonObject expected{{"serverId", snapshot.value("serverId")}, {"sourceEpoch", snapshot.value("sourceEpoch")}, {"revision", snapshot.value("revision")}};
                        if (command.value("expected").toObject() != expected || operation.value("type") != "room.rename" ||
                            operation.value("targetId") != SourceRoom || !snapshot.value("capabilities").toObject().value("renameRoom").toBool()) {
                            reply(socket, 409, {{"schemaVersion", 1}, {"code", "stale_revision"}}); return;
                        }
                        if (!uncertain) {
                            auto rooms = snapshot.value("rooms").toArray(); auto room = rooms.first().toObject();
                            room.insert("name", operation.value("name")); rooms.replace(0, room); snapshot.insert("rooms", rooms);
                            snapshot.insert("revision", QString::number(snapshot.value("revision").toString().toULongLong() + 1));
                        }
                        reply(socket, 202, {{"schemaVersion", 1}, {"commandId", command.value("commandId")},
                            {"status", uncertain ? "uncertain" : "confirmed"},
                            {"code", uncertain ? QJsonValue("write_uncertain") : QJsonValue(QJsonValue::Null)}, {"snapshot", snapshot}});
                    } else reply(socket, 404, {{"schemaVersion", 1}, {"code", "not_found"}});
                });
            }
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1").arg(server.serverPort())); }
    void reply(QTcpSocket *socket, int status, const QJsonObject &value) {
        const auto body = QJsonDocument(value).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Synthetic\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
};

bool establishRoom(TuyaMetadataManager &manager, const MatterDevice &device) {
    return manager.setSourceHome(SourceHome) && manager.setMode("followSource") &&
        manager.bindDevice(device, SourceDevice) && manager.followSource(device);
}
}

class TuyaMetadataUiTest : public QObject {
    Q_OBJECT
private slots:
    void defaultsAreOptInAndTokenIsMasked() {
        QTemporaryDir directory; TuyaMetadataManager manager(nullptr, directory.path());
        manager.setProfile("panel-controller");
        TuyaMetadataPanel panel(&manager); panel.show();
        const auto enabled = panel.findChild<QCheckBox *>("tuyaEnabled");
        const auto token = panel.findChild<QLineEdit *>("tuyaToken");
        const auto mode = panel.findChild<QComboBox *>("tuyaMode");
        const auto rename = panel.findChild<QPushButton *>("tuyaRename");
        QVERIFY(enabled && token && mode && rename);
        QVERIFY(!enabled->isChecked()); QVERIFY(!manager.enabled()); QVERIFY(!manager.isConfigured());
        QCOMPARE(token->echoMode(), QLineEdit::Password);
        QVERIFY(token->text().isEmpty()); QCOMPARE(manager.mode(), QStringLiteral("manual"));
        QVERIFY(!rename->isEnabled());
    }

    void connectionActionClearsTheEnteredToken() {
        QTemporaryDir directory; TuyaMetadataManager manager(nullptr, directory.path()); manager.setProfile("connection-panel");
        TuyaMetadataPanel panel(&manager); panel.show();
        auto *url = panel.findChild<QLineEdit *>("tuyaUrl");
        auto *token = panel.findChild<QLineEdit *>("tuyaToken");
        auto *connect = panel.findChild<QPushButton *>("tuyaConnect");
        QVERIFY(url && token && connect);
        url->setText("http://127.0.0.1:1");
        token->setText("synthetic-panel-token-with-at-least-32-characters");
        QTest::mouseClick(connect, Qt::LeftButton);
        QTRY_VERIFY(token->text().isEmpty());
        QVERIFY(manager.isConfigured());
    }

    void switchingEndpointsClearsThePreviousDraftAndNeverInvokesMatterCommands() {
        Controller controller; MatterClient client; client.connectToServer(controller.url());
        QTRY_VERIFY(client.isConnected()); QTRY_COMPARE(client.devices().size(), 2);
        QTemporaryDir directory; TuyaMetadataManager manager(&client, directory.path()); manager.setProfile("selected-controller");
        TuyaMetadataPanel panel(&manager); panel.show();
        auto *devices = panel.findChild<QComboBox *>("tuyaDevice");
        auto *draft = panel.findChild<QLineEdit *>("tuyaName");
        QVERIFY(devices && draft); QTRY_COMPARE(devices->count(), 2);
        const auto first = client.devices().first(); const auto second = client.devices().last();
        panel.setDevice(first); draft->setText("Unsaved first endpoint name");
        panel.setDevice(second);
        QVERIFY(draft->text() != QStringLiteral("Unsaved first endpoint name"));
        QCOMPARE(controller.mutations, 0);
        QVERIFY(!manager.enabled()); QVERIFY(!manager.isConfigured());
    }

    void roomRenameRequiresDeclaredCapabilityAndNeverMovesDevices() {
        RoomMetadataServer server; auto caps = server.snapshot.value("capabilities").toObject();
        caps.insert("renameRoom", false); server.snapshot.insert("capabilities", caps);
        QTemporaryDir directory; TuyaMetadataManager manager(nullptr, directory.path()); manager.setProfile("room-readonly-panel");
        QVERIFY(manager.configure(server.url(), RoomToken)); QVERIFY(manager.setEnabled(true));
        QTRY_VERIFY(manager.snapshot().value("available").toBool()); const auto device = roomEndpoint();
        QVERIFY(establishRoom(manager, device)); TuyaMetadataPanel panel(&manager, device); panel.show();
        auto *draft = panel.findChild<QLineEdit *>("tuyaRoomName");
        auto *rename = panel.findChild<QPushButton *>("tuyaRenameRoom");
        auto *move = panel.findChild<QPushButton *>("tuyaMove");
        QVERIFY(draft && rename && move); QCOMPARE(draft->text(), QStringLiteral("Study"));
        QVERIFY(!draft->isEnabled()); QVERIFY(!rename->isEnabled()); QVERIFY(!move->isEnabled());
        draft->setText("Never write without capability"); QTest::mouseClick(rename, Qt::LeftButton);
        QTest::qWait(50); QCOMPARE(server.commands.size(), 0);
    }

    void roomRenamePostsExactlyOnceOnlyAfterExplicitConfirmation() {
        RoomMetadataServer server; QTemporaryDir directory; TuyaMetadataManager manager(nullptr, directory.path());
        manager.setProfile("room-write-panel"); QVERIFY(manager.configure(server.url(), RoomToken)); QVERIFY(manager.setEnabled(true));
        QTRY_VERIFY(manager.snapshot().value("available").toBool()); const auto device = roomEndpoint();
        QVERIFY(establishRoom(manager, device)); TuyaMetadataPanel panel(&manager, device); panel.show();
        auto *rooms = panel.findChild<QComboBox *>("tuyaRoom"); auto *draft = panel.findChild<QLineEdit *>("tuyaRoomName");
        auto *rename = panel.findChild<QPushButton *>("tuyaRenameRoom"); auto *move = panel.findChild<QPushButton *>("tuyaMove");
        QVERIFY(rooms && draft && rename && move); QCOMPARE(rooms->currentData().toString(), SourceRoom);
        QCOMPARE(rooms->findData(ForeignRoom), -1); QVERIFY(rooms->isEnabled()); QVERIFY(!move->isEnabled());
        QCOMPARE(server.commands.size(), 0); draft->setText(QStringLiteral("New study 房间"));
        QVERIFY(rename->isEnabled()); QTest::qWait(50); QCOMPARE(server.commands.size(), 0);
        const auto before = manager.snapshot(); QTest::mouseClick(rename, Qt::LeftButton); QTest::mouseClick(rename, Qt::LeftButton);
        QTRY_COMPARE(server.commands.size(), 1); QTRY_VERIFY(!manager.requestInProgress());
        QCOMPARE(server.commands.first().value("operation").toObject(), QJsonObject({{"type", "room.rename"}, {"targetId", SourceRoom}, {"name", QStringLiteral("New study 房间")}}));
        QCOMPARE(server.commands.first().value("expected").toObject(), QJsonObject({{"serverId", before.value("serverId")}, {"sourceEpoch", before.value("sourceEpoch")}, {"revision", before.value("revision")}}));
        QTRY_COMPARE(draft->text(), QStringLiteral("New study 房间")); QVERIFY(!rename->isEnabled());
        QCOMPARE(manager.state(device).fullName, QStringLiteral("Source light"));
        QCOMPARE(device.name, QStringLiteral("Native short label"));
    }

    void staleRoomDraftRequiresExplicitRebaseAndPreservesTheText() {
        RoomMetadataServer server; QTemporaryDir directory; TuyaMetadataManager manager(nullptr, directory.path());
        manager.setProfile("stale-room-panel"); QVERIFY(manager.configure(server.url(), RoomToken)); QVERIFY(manager.setEnabled(true));
        QTRY_VERIFY(manager.snapshot().value("available").toBool()); const auto device = roomEndpoint();
        QVERIFY(establishRoom(manager, device)); TuyaMetadataPanel panel(&manager, device); panel.show();
        auto *draft = panel.findChild<QLineEdit *>("tuyaRoomName"); auto *rename = panel.findChild<QPushButton *>("tuyaRenameRoom");
        auto *rebase = panel.findChild<QPushButton *>("tuyaRebase"); QVERIFY(draft && rename && rebase);
        draft->setText("Preserved room draft"); server.snapshot.insert("revision", "2"); QVERIFY(manager.refresh());
        QTRY_COMPARE(manager.snapshot().value("revision").toString(), QStringLiteral("2"));
        QCOMPARE(draft->text(), QStringLiteral("Preserved room draft")); QVERIFY(!rename->isEnabled());
        QTest::mouseClick(rename, Qt::LeftButton); QCOMPARE(server.commands.size(), 0);
        QVERIFY(rebase->isEnabled()); QTest::mouseClick(rebase, Qt::LeftButton);
        QCOMPARE(draft->text(), QStringLiteral("Preserved room draft")); QVERIFY(rename->isEnabled());
        QTest::mouseClick(rename, Qt::LeftButton); QTRY_COMPARE(server.commands.size(), 1);
        QCOMPARE(server.commands.first().value("expected").toObject().value("revision").toString(), QStringLiteral("2"));
    }

    void epochChangeAndPartialRoomsBlockTheDirtyRoomDraft() {
        RoomMetadataServer server; QTemporaryDir directory; TuyaMetadataManager manager(nullptr, directory.path());
        manager.setProfile("epoch-room-panel"); QVERIFY(manager.configure(server.url(), RoomToken)); QVERIFY(manager.setEnabled(true));
        QTRY_VERIFY(manager.snapshot().value("available").toBool()); const auto device = roomEndpoint();
        QVERIFY(establishRoom(manager, device)); TuyaMetadataPanel panel(&manager, device); panel.show();
        auto *draft = panel.findChild<QLineEdit *>("tuyaRoomName"); auto *rename = panel.findChild<QPushButton *>("tuyaRenameRoom");
        QVERIFY(draft && rename); draft->setText("Keep this room draft");
        server.snapshot.insert("sourceEpoch", "7b25f764-1d0e-43c0-a129-4ab09b158c83"); QVERIFY(manager.refresh());
        QTRY_COMPARE(manager.snapshot().value("sourceEpoch").toString(), QStringLiteral("7b25f764-1d0e-43c0-a129-4ab09b158c83"));
        QCOMPARE(draft->text(), QStringLiteral("Keep this room draft")); QVERIFY(!rename->isEnabled());
        QTest::mouseClick(rename, Qt::LeftButton); QCOMPARE(server.commands.size(), 0);
        server.snapshot.insert("roomsAvailable", false); server.snapshot.insert("revision", "2");
        auto sources = server.snapshot.value("devices").toArray(); auto source = sources.first().toObject();
        source.insert("roomStatus", "unknown"); source.insert("roomId", QJsonValue(QJsonValue::Null)); source.insert("homeId", QJsonValue(QJsonValue::Null));
        sources.replace(0, source); server.snapshot.insert("devices", sources); QVERIFY(manager.refresh());
        QTRY_VERIFY(!manager.snapshot().value("roomsAvailable").toBool());
        QCOMPARE(draft->text(), QStringLiteral("Keep this room draft")); QVERIFY(!rename->isEnabled()); QCOMPARE(server.commands.size(), 0);
    }

    void uncertainRoomWriteKeepsDraftAndNeverOffersAnAutomaticRetry() {
        RoomMetadataServer server; server.uncertain = true; QTemporaryDir directory;
        TuyaMetadataManager manager(nullptr, directory.path()); manager.setProfile("unknown-room-panel");
        QVERIFY(manager.configure(server.url(), RoomToken)); QVERIFY(manager.setEnabled(true));
        QTRY_VERIFY(manager.snapshot().value("available").toBool()); const auto device = roomEndpoint();
        QVERIFY(establishRoom(manager, device)); TuyaMetadataPanel panel(&manager, device); panel.show();
        auto *draft = panel.findChild<QLineEdit *>("tuyaRoomName"); auto *rename = panel.findChild<QPushButton *>("tuyaRenameRoom");
        auto *rebase = panel.findChild<QPushButton *>("tuyaRebase"); QVERIFY(draft && rename && rebase);
        draft->setText("Unknown room draft"); QTest::mouseClick(rename, Qt::LeftButton);
        QTRY_COMPARE(server.commands.size(), 1); QTRY_VERIFY(manager.hasUncertainWrite());
        QCOMPARE(draft->text(), QStringLiteral("Unknown room draft")); QVERIFY(!rename->isEnabled()); QVERIFY(!rebase->isEnabled());
        QVERIFY(manager.refresh()); QTRY_VERIFY(!manager.requestInProgress());
        QCOMPARE(draft->text(), QStringLiteral("Unknown room draft")); QVERIFY(!rename->isEnabled());
        QTest::mouseClick(rename, Qt::LeftButton); QTest::qWait(100); QCOMPARE(server.commands.size(), 1);
    }
};

QTEST_MAIN(TuyaMetadataUiTest)
#include "tst_tuyametadataui.moc"
