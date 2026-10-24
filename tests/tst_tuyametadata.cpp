#include "tuyametadata.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <memory>

namespace {
const QString HomeId = "h_" + QString(64, 'b');
const QString RoomId = "r_" + QString(64, 'c');
const QString OtherRoomId = "r_" + QString(64, 'e');
const QString DeviceId = "d_" + QString(64, 'a');
const QString NativeId = QStringLiteral("test-tuya-native-unique-id");
const QString Token = QStringLiteral("synthetic-tuya-metadata-token-longer-than-32-characters");

MatterDevice endpoint(const QString &identity = NativeId) {
    MatterDevice device;
    device.key = "7/1";
    device.nodeId = 7;
    device.endpointId = 1;
    device.name = "Short native label";
    device.available = true;
    device.attributes = {{"1/57/18", identity}};
    return device;
}

QJsonObject sourceSnapshot() {
    const QJsonObject bindings{{"hapSerialNumber", QJsonValue(QJsonValue::Null)},
        {"hapAccessoryUuid", QJsonValue(QJsonValue::Null)}, {"matterUniqueIds", QJsonArray{NativeId}}};
    const QJsonObject device{{"id", DeviceId}, {"name", "Full Tuya Unicode name 灯"},
        {"homeId", HomeId}, {"roomId", RoomId}, {"roomStatus", "assigned"}, {"bindings", bindings}};
    return {{"schemaVersion", 1}, {"serverId", "8c4eec7f-7f15-4507-b7ac-5189b3b821a0"},
        {"sourceEpoch", "6b25f764-1d0e-43c0-a129-4ab09b158c83"}, {"revision", "1"},
        {"observedAt", "2026-10-24T09:00:00.000Z"}, {"available", true}, {"roomsAvailable", true},
        {"sourceKind", "tuya-project-paas"},
        {"capabilities", QJsonObject{{"read", true}, {"renameDevice", true}, {"moveDevice", true}, {"renameRoom", true}}},
        {"limitations", QJsonObject{}},
        {"homes", QJsonArray{QJsonObject{{"id", HomeId}, {"name", "Source home"}}}},
        {"rooms", QJsonArray{QJsonObject{{"id", RoomId}, {"homeId", HomeId}, {"name", "Study"}},
            QJsonObject{{"id", OtherRoomId}, {"homeId", HomeId}, {"name", "Living room"}}}},
        {"devices", QJsonArray{device}}};
}

struct Request {
    QByteArray method, path;
    QJsonObject body;
};

class MetadataServer : public QObject {
public:
    QTcpServer server;
    QJsonObject snapshot = sourceSnapshot();
    QList<Request> requests;
    QHash<QString, QJsonObject> receipts, commands;
    int posts = 0, receiptPolls = 0, queuedPolls = 1;
    bool wrongAuthorization = false, conflict = false, uncertain = false;
    QUrl redirect;

    MetadataServer() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                socket->setParent(this);
                auto bytes = std::make_shared<QByteArray>();
                auto handled = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, bytes, handled] {
                    bytes->append(socket->readAll());
                    if (*handled) return;
                    const int boundary = bytes->indexOf("\r\n\r\n");
                    if (boundary < 0) return;
                    const auto lines = bytes->left(boundary).split('\n');
                    int length = 0;
                    bool authorized = false;
                    for (auto line : lines) {
                        line = line.trimmed();
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                        if (line.toLower().startsWith("authorization:"))
                            authorized = line.mid(14).trimmed() == "Bearer " + Token.toUtf8();
                    }
                    if (bytes->size() < boundary + 4 + length) return;
                    *handled = true;
                    const auto first = lines.first().trimmed().split(' ');
                    const Request request{first.value(0), first.value(1),
                        QJsonDocument::fromJson(bytes->mid(boundary + 4, length)).object()};
                    requests.append(request);
                    if (!authorized) {
                        wrongAuthorization = true;
                        reply(socket, 401, {{"schemaVersion", 1}, {"code", "permission_denied"}});
                        return;
                    }
                    handle(socket, request);
                });
            }
        });
    }

    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1").arg(server.serverPort())); }

    void reply(QTcpSocket *socket, int status, const QJsonObject &value, const QByteArray &extra = {}) {
        const auto body = QJsonDocument(value).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Synthetic\r\nContent-Type: application/json\r\nConnection: close\r\n"
            + extra + "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    void handle(QTcpSocket *socket, const Request &request) {
        if (request.method == "GET" && request.path == "/v1/capabilities") {
            auto capabilities = snapshot;
            for (const auto &key : {"homes", "rooms", "devices"}) capabilities.remove(key);
            reply(socket, 200, capabilities);
        } else if (request.method == "GET" && request.path == "/v1/metadata") {
            if (!redirect.isEmpty()) reply(socket, 302, {}, "Location: " + redirect.toEncoded() + "\r\n");
            else reply(socket, 200, snapshot);
        } else if (request.method == "POST" && request.path == "/v1/metadata/commands") {
            ++posts;
            const auto expected = request.body.value("expected").toObject();
            const QJsonObject guard{{"serverId", snapshot.value("serverId")},
                {"sourceEpoch", snapshot.value("sourceEpoch")}, {"revision", snapshot.value("revision")}};
            if (conflict || expected != guard) {
                reply(socket, 409, {{"schemaVersion", 1}, {"code", "stale_revision"}});
                return;
            }
            const auto id = request.body.value("commandId").toString();
            commands.insert(id, request.body);
            const QJsonObject receipt{{"schemaVersion", 1}, {"commandId", id},
                {"status", uncertain ? "uncertain" : "queued"},
                {"code", uncertain ? QJsonValue("write_uncertain") : QJsonValue(QJsonValue::Null)}, {"snapshot", snapshot}};
            receipts.insert(id, receipt);
            reply(socket, 202, receipt);
        } else if (request.method == "GET" && request.path.startsWith("/v1/metadata/commands/")) {
            const auto id = QString::fromUtf8(request.path.mid(QByteArray("/v1/metadata/commands/").size()));
            if (!receipts.contains(id)) { reply(socket, 404, {{"code", "not_found"}}); return; }
            ++receiptPolls;
            auto receipt = receipts.value(id);
            if (receipt.value("status") == "queued" && receiptPolls > queuedPolls) {
                const auto operation = commands.value(id).value("operation").toObject();
                auto devices = snapshot.value("devices").toArray();
                if (operation.value("type") == "device.rename") {
                    auto device = devices.first().toObject();
                    device.insert("name", operation.value("name"));
                    devices.replace(0, device);
                    snapshot.insert("devices", devices);
                }
                snapshot.insert("revision", QString::number(snapshot.value("revision").toString().toULongLong() + 1));
                receipt.insert("status", "confirmed");
                receipt.insert("snapshot", snapshot);
                receipts.insert(id, receipt);
            }
            reply(socket, 200, receipt);
        } else reply(socket, 404, {{"schemaVersion", 1}, {"code", "not_found"}});
    }
};

bool establish(TuyaMetadataManager &manager, const MatterDevice &device) {
    return manager.setSourceHome(HomeId) && manager.setMode("followSource")
        && manager.bindDevice(device, DeviceId) && manager.followSource(device);
}

QByteArray persistedFiles(const QString &directory) {
    QByteArray bytes;
    QDirIterator iterator(directory, QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        QFile file(iterator.next());
        if (file.open(QIODevice::ReadOnly)) bytes += file.readAll();
    }
    return bytes;
}
}

class TuyaMetadataTest : public QObject {
    Q_OBJECT
private slots:
    void parserIsAtomicAndUnknownFieldsAreIgnored() {
        QJsonObject parsed;
        auto valid = sourceSnapshot(); valid.insert("futureField", QJsonObject{{"ignored", true}});
        QVERIFY(parseTuyaMetadataSnapshot(valid, &parsed));
        QVERIFY(!parsed.contains("futureField"));
        const auto previous = parsed;
        auto invalid = valid; invalid.insert("revision", 1);
        QVERIFY(!parseTuyaMetadataSnapshot(invalid, &parsed)); QCOMPARE(parsed, previous);
        invalid = valid; invalid.insert("devices", QJsonArray{valid.value("devices").toArray().first(), valid.value("devices").toArray().first()});
        QVERIFY(!parseTuyaMetadataSnapshot(invalid, &parsed)); QCOMPARE(parsed, previous);
    }

    void connectionPolicyRejectsCredentialsAndRemotePlaintext() {
        QTemporaryDir directory; TuyaMetadataManager manager(nullptr, directory.path());
        manager.setProfile("test-controller");
        QVERIFY(!manager.enabled()); QVERIFY(!manager.isConfigured());
        for (const auto &url : {"http://example.com", "http://127.0.0.2", "http://localhost.evil", "ftp://localhost",
                 "http://user:password@127.0.0.1", "http://127.0.0.1?token=secret", "http://127.0.0.1/#fragment"}) {
            QVERIFY2(!manager.configure(QUrl(url), Token), url);
        }
        QVERIFY(!manager.configure(QUrl("http://127.0.0.1:1"), "short"));
        QVERIFY(!manager.isConfigured());
    }

    void cachedReadsAndExplicitOverlayNeverWriteNativeLabelsOrSecrets() {
        QTemporaryDir directory; MetadataServer server; TuyaMetadataManager manager(nullptr, directory.path());
        manager.setProfile("controller-one");
        QVERIFY(manager.configure(server.url(), Token)); QVERIFY(manager.setEnabled(true));
        QTRY_VERIFY(manager.snapshot().value("available").toBool());
        const auto device = endpoint();
        QVERIFY(manager.setSourceHome(HomeId));
        QCOMPARE(tuyaMatterUniqueId(device), NativeId);
        QCOMPARE(manager.suggestedBinding(device), DeviceId);
        QVERIFY(!manager.state(device).hasBaseline);
        QVERIFY(establish(manager, device));
        const auto state = manager.state(device);
        QVERIFY(state.bound && state.known && state.hasBaseline);
        QCOMPARE(state.fullName, QStringLiteral("Full Tuya Unicode name 灯"));
        QCOMPARE(manager.displayName(device), state.fullName);
        QCOMPARE(device.name, QStringLiteral("Short native label"));
        QCOMPARE(server.posts, 0); QVERIFY(!server.wrongAuthorization);
        auto reassigned = device; reassigned.attributes.insert("1/57/18", "another-physical-device");
        QVERIFY(!manager.state(reassigned).available); QVERIFY(!manager.state(reassigned).canRename);
        QVERIFY(!manager.rename(reassigned, "Never rename the previous physical device"));
        const auto files = persistedFiles(directory.path());
        QVERIFY(!files.contains(Token.toUtf8())); QVERIFY(!files.contains("Bearer "));
    }

    void duplicateOrMissingUniqueIdsDoNotGuessBindingsByName() {
        MetadataServer server;
        auto devices = server.snapshot.value("devices").toArray();
        auto duplicate = devices.first().toObject(); duplicate.insert("id", "d_" + QString(64, 'f'));
        devices.append(duplicate); server.snapshot.insert("devices", devices);
        QTemporaryDir directory; TuyaMetadataManager manager(nullptr, directory.path());
        manager.setProfile("controller-two"); QVERIFY(manager.configure(server.url(), Token)); QVERIFY(manager.setEnabled(true));
        QTRY_VERIFY(manager.snapshot().value("available").toBool());
        QVERIFY(manager.setSourceHome(HomeId));
        QVERIFY(manager.suggestedBinding(endpoint()).isEmpty());
        auto missing = endpoint(); missing.attributes = {}; missing.name = "Full Tuya Unicode name 灯";
        QVERIFY(manager.suggestedBinding(missing).isEmpty());
        QVERIFY(!manager.state(missing).bound); QCOMPARE(server.posts, 0);
    }

    void conditionalRenamePostsOnceAndWaitsForTerminalReceipt() {
        QTemporaryDir directory; MetadataServer server; TuyaMetadataManager manager(nullptr, directory.path());
        manager.setProfile("rename-controller"); QVERIFY(manager.configure(server.url(), Token)); QVERIFY(manager.setEnabled(true));
        QTRY_VERIFY(manager.snapshot().value("available").toBool());
        const auto device = endpoint(); QVERIFY(establish(manager, device));
        const auto before = manager.snapshot(); QSignalSpy finished(&manager, &TuyaMetadataManager::operationFinished);
        QVERIFY(manager.rename(device, "Renamed study light"));
        QTRY_COMPARE(server.posts, 1);
        QVERIFY(!manager.rename(device, "Do not enqueue"));
        const auto command = server.commands.values().first();
        QCOMPARE(command.size(), 4);
        QCOMPARE(command.value("schemaVersion").toInt(), 1);
        QVERIFY(!QUuid(command.value("commandId").toString()).isNull());
        QCOMPARE(command.value("expected").toObject(), QJsonObject({{"serverId", before.value("serverId")},
            {"sourceEpoch", before.value("sourceEpoch")}, {"revision", before.value("revision")}}));
        QCOMPARE(command.value("operation").toObject(), QJsonObject({{"type", "device.rename"}, {"targetId", DeviceId}, {"name", "Renamed study light"}}));
        QTRY_VERIFY_WITH_TIMEOUT(!manager.state(device).busy, 12000);
        QTRY_COMPARE(manager.state(device).fullName, QStringLiteral("Renamed study light"));
        QVERIFY(server.receiptPolls >= 2); QCOMPARE(server.posts, 1); QVERIFY(!server.wrongAuthorization);
        QTRY_VERIFY(!finished.isEmpty()); QVERIFY(finished.last().at(2).toBool());
    }

    void staleRevisionConflictPreservesBaselineWithoutAutomaticRetry() {
        QTemporaryDir directory; MetadataServer server; TuyaMetadataManager manager(nullptr, directory.path());
        manager.setProfile("conflict-controller"); QVERIFY(manager.configure(server.url(), Token)); QVERIFY(manager.setEnabled(true));
        QTRY_VERIFY(manager.snapshot().value("available").toBool());
        const auto device = endpoint(); QVERIFY(establish(manager, device)); server.conflict = true;
        QVERIFY(manager.rename(device, "Unconfirmed draft")); QTRY_COMPARE(server.posts, 1);
        QTRY_VERIFY(!manager.state(device).busy);
        QVERIFY(manager.state(device).conflict);
        QCOMPARE(manager.state(device).fullName, QStringLiteral("Full Tuya Unicode name 灯"));
        QTest::qWait(100); QCOMPARE(server.posts, 1);
    }

    void uncertainReceiptSurvivesRestartAndNeverResends() {
        QTemporaryDir directory; MetadataServer server; server.uncertain = true; const auto device = endpoint();
        {
            TuyaMetadataManager manager(nullptr, directory.path()); manager.setProfile("uncertain-controller");
            QVERIFY(manager.configure(server.url(), Token)); QVERIFY(manager.setEnabled(true));
            QTRY_VERIFY(manager.snapshot().value("available").toBool()); QVERIFY(establish(manager, device));
            QSignalSpy finished(&manager, &TuyaMetadataManager::operationFinished);
            QVERIFY(manager.rename(device, "Unknown outcome")); QTRY_COMPARE(server.posts, 1);
            QTRY_COMPARE(finished.size(), 1); QVERIFY(!finished.first().at(2).toBool());
            QVERIFY(!manager.rename(device, "No automatic repeat"));
        }
        const auto persisted = persistedFiles(directory.path()); QVERIFY(!persisted.contains(Token.toUtf8()));
        TuyaMetadataManager restored(nullptr, directory.path()); restored.setProfile("uncertain-controller");
        QVERIFY(!restored.isConfigured());
        QVERIFY(restored.configure(server.url(), Token)); QVERIFY(restored.setEnabled(true));
        QTRY_VERIFY(restored.state(device).available);
        QVERIFY(!restored.rename(device, "Still blocked")); QTest::qWait(100); QCOMPARE(server.posts, 1);
        QVERIFY(restored.acknowledgeUncertain()); QTest::qWait(50); QCOMPARE(server.posts, 1);
    }

    void namesAreValidatedBeforeAnyNetworkMutation() {
        QTemporaryDir directory; MetadataServer server; TuyaMetadataManager manager(nullptr, directory.path());
        manager.setProfile("name-policy-controller"); QVERIFY(manager.configure(server.url(), Token)); QVERIFY(manager.setEnabled(true));
        QTRY_VERIFY(manager.snapshot().value("available").toBool()); const auto device = endpoint();
        QVERIFY(establish(manager, device));
        const auto emoji = QString(QChar(0xd83d)) + QString(QChar(0xdca1));
        for (const auto &invalid : {QStringLiteral("  "), QString(51, 'x'), QString(QChar(0xd800)),
                 QStringLiteral("line\nbreak"), emoji.repeated(41)}) {
            QVERIFY(!manager.rename(device, invalid));
        }
        QCOMPARE(server.posts, 0);
    }

    void externalRevisionChangeRejectsTheDraftBeforePosting() {
        QTemporaryDir directory; MetadataServer server; TuyaMetadataManager manager(nullptr, directory.path());
        manager.setProfile("preflight-controller"); QVERIFY(manager.configure(server.url(), Token)); QVERIFY(manager.setEnabled(true));
        QTRY_VERIFY(manager.snapshot().value("available").toBool()); const auto device = endpoint();
        QVERIFY(establish(manager, device));
        auto devices = server.snapshot.value("devices").toArray(); auto changed = devices.first().toObject();
        changed.insert("name", "Changed independently in Tuya"); devices.replace(0, changed);
        server.snapshot.insert("devices", devices); server.snapshot.insert("revision", "2");
        QSignalSpy finished(&manager, &TuyaMetadataManager::operationFinished);
        QVERIFY(manager.rename(device, "Draft based on revision one"));
        QTRY_COMPARE(finished.size(), 1); QVERIFY(!finished.first().at(2).toBool());
        QCOMPARE(server.posts, 0);
        QCOMPARE(manager.state(device).fullName, QStringLiteral("Changed independently in Tuya"));
    }

    void sourceFollowingUsesThreeWayBaselineAndEpochChangesNeedReview() {
        QTemporaryDir directory; MetadataServer server; TuyaMetadataManager manager(nullptr, directory.path());
        manager.setProfile("baseline-controller"); QVERIFY(manager.configure(server.url(), Token)); QVERIFY(manager.setEnabled(true));
        QTRY_VERIFY(manager.snapshot().value("available").toBool()); const auto device = endpoint();
        QVERIFY(establish(manager, device));
        auto devices = server.snapshot.value("devices").toArray(); auto changed = devices.first().toObject();
        changed.insert("name", "New authoritative source name"); devices.replace(0, changed);
        server.snapshot.insert("devices", devices); server.snapshot.insert("revision", "2");
        QVERIFY(manager.refresh());
        QTRY_COMPARE(manager.displayName(device), QStringLiteral("New authoritative source name"));
        QCOMPARE(server.posts, 0);
        QVERIFY(manager.setMode("manual"));
        changed.insert("name", "Manual mode requires review"); devices.replace(0, changed);
        server.snapshot.insert("devices", devices); server.snapshot.insert("revision", "3");
        QVERIFY(manager.refresh());
        QTRY_VERIFY(manager.state(device).conflict); QVERIFY(!manager.state(device).canRename);
        QCOMPARE(manager.state(device).managedName, QStringLiteral("New authoritative source name"));
        QVERIFY(manager.followSource(device)); QVERIFY(!manager.state(device).conflict);
        server.snapshot.insert("sourceEpoch", QUuid::createUuid().toString(QUuid::WithoutBraces));
        QVERIFY(manager.refresh()); QTRY_VERIFY(manager.state(device).conflict);
        QVERIFY(!manager.rename(device, "No stale epoch write")); QCOMPARE(server.posts, 0);
    }

    void redirectsNeverForwardAuthorization() {
        MetadataServer destination, server; server.redirect = destination.url().resolved(QUrl("/v1/metadata"));
        QTemporaryDir directory; TuyaMetadataManager manager(nullptr, directory.path()); manager.setProfile("redirect-controller");
        QVERIFY(manager.configure(server.url(), Token)); QVERIFY(manager.setEnabled(true));
        QTRY_VERIFY(!server.requests.isEmpty()); QTest::qWait(150);
        QCOMPARE(destination.requests.size(), 0); QVERIFY(!manager.snapshot().value("available").toBool());
        QCOMPARE(server.posts, 0);
    }

    void customProjectKeepsNamesWithoutInventingRooms() {
        MetadataServer server; server.snapshot.insert("sourceKind", "tuya-project-custom");
        server.snapshot.insert("roomsAvailable", false); server.snapshot.insert("homes", QJsonArray{}); server.snapshot.insert("rooms", QJsonArray{});
        auto devices = server.snapshot.value("devices").toArray(); auto source = devices.first().toObject();
        source.insert("homeId", QJsonValue(QJsonValue::Null)); source.insert("roomId", QJsonValue(QJsonValue::Null)); source.insert("roomStatus", "unsupported");
        devices.replace(0, source); server.snapshot.insert("devices", devices);
        server.snapshot.insert("capabilities", QJsonObject{{"read", true}, {"renameDevice", false}, {"moveDevice", false}, {"renameRoom", false}});
        QTemporaryDir directory; TuyaMetadataManager manager(nullptr, directory.path()); manager.setProfile("custom-controller");
        QVERIFY(manager.configure(server.url(), Token)); QVERIFY(manager.setEnabled(true));
        QTRY_VERIFY(manager.snapshot().value("available").toBool());
        QVERIFY(manager.setSourceHome(QString()));
        const auto device = endpoint(); QVERIFY(manager.bindDevice(device, DeviceId));
        const auto state = manager.state(device); QVERIFY(state.known);
        QCOMPARE(state.fullName, QStringLiteral("Full Tuya Unicode name 灯"));
        QVERIFY(state.roomName.isEmpty()); QVERIFY(!state.canMoveRoom && !state.canRenameRoom);
        QVERIFY(!manager.moveRoom(device, RoomId)); QCOMPARE(server.posts, 0);
    }
};

QTEST_GUILESS_MAIN(TuyaMetadataTest)
#include "tst_tuyametadata.moc"
