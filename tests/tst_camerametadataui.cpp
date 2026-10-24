#include "accessorypanel.h"
#include "camerametadatapanel.h"
#include "homestore.h"
#include "matterclient.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QWebSocket>
#include <QWebSocketServer>

namespace {
const QString Prefix = QStringLiteral("1/4294048770/");
QJsonObject snapshot(int revision = 1, const QString &name = QStringLiteral("Cloud camera"),
                     const QString &identity = QString(64, QLatin1Char('a'))) {
    return {{"0", identity}, {"1", "12345678-1234-4234-8234-123456789abc"}, {"2", revision},
        {"3", name}, {"4", QString(64, QLatin1Char('b'))}, {"5", "Cloud home"},
        {"6", "10"}, {"7", "Cloud room"}, {"8", QStringLiteral("2026-10-24T08:00:00.%1Z").arg(qMax(0, revision - 1), 3, 10, QLatin1Char('0'))}};
}
QJsonObject initialAttributes(bool writable) {
    return {{"1/29/0", QJsonArray{QJsonObject{{"deviceType", 0x142}, {"revision", 1}}}},
        {"1/29/1", QJsonArray{29, 0x551, 0x553, 4294048770LL}},
        {Prefix + "0", snapshot()},
        {Prefix + "1", QJsonArray{QJsonObject{{"0", "10"}, {"1", "Cloud room"}}, QJsonObject{{"0", "20"}, {"1", "Study"}}}},
        {Prefix + "2", QJsonObject{{"0", true}, {"1", writable}, {"2", writable}, {"3", true}}},
        {Prefix + "3", true}, {Prefix + "4", true}, {Prefix + "5", false}, {Prefix + "6", ""},
        {Prefix + "65529", QJsonArray{0, 1, 2, 3}}};
}
class MetadataServer : public QObject {
public:
    QWebSocketServer server{QStringLiteral("Metadata UI synthetic fixture"), QWebSocketServer::NonSecureMode};
    QWebSocket *peer = nullptr;
    QJsonObject attributes;
    QList<QJsonObject> requests, commands, heldReads;
    bool holdReadsAfterCommand = false;
    explicit MetadataServer(bool writable = true) : attributes(initialAttributes(writable)) {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QWebSocketServer::newConnection, this, [this] {
            peer = server.nextPendingConnection(); peer->setParent(this);
            connect(peer, &QWebSocket::textMessageReceived, this, [this](const QString &text) {
                const auto request = QJsonDocument::fromJson(text.toUtf8()).object(); requests.append(request);
                const auto command = request.value("command").toString();
                if (command == QStringLiteral("start_listening") || command == QStringLiteral("get_nodes")) reply(request, QJsonArray{node()});
                else if (command == QStringLiteral("get_node")) reply(request, node());
                else if (command == QStringLiteral("read_attribute")) {
                    if (holdReadsAfterCommand && !commands.isEmpty()) heldReads.append(request); else replyRead(request);
                } else if (command == QStringLiteral("device_command")) commands.append(request);
            });
            send({{"schema_version", 14}, {"min_supported_schema_version", 11}});
        });
    }
    QJsonObject node() const { return {{"node_id", 7}, {"available", true}, {"attributes", attributes}}; }
    QUrl url() const { return QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(server.serverPort())); }
    void send(const QJsonObject &value) { if (peer) peer->sendTextMessage(QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact))); }
    void reply(const QJsonObject &request, const QJsonValue &result) { send({{"message_id", request.value("message_id")}, {"result", result}}); }
    void replyRead(const QJsonObject &request) {
        QJsonObject result;
        const auto paths = request.value("args").toObject().value("attribute_path");
        if (paths.isArray()) for (const auto &path : paths.toArray()) {
            const auto key = path.toString(); if (attributes.contains(key)) result.insert(key, attributes.value(key));
        }
        else if (paths.isString() && attributes.contains(paths.toString())) result.insert(paths.toString(), attributes.value(paths.toString()));
        reply(request, result);
    }
    void releaseReads() {
        holdReadsAfterCommand = false;
        const auto pending = heldReads; heldReads.clear(); for (const auto &request : pending) replyRead(request);
    }
    void reportSnapshot(const QJsonObject &value) {
        attributes.insert(Prefix + "0", value);
        send({{"event", "attribute_updated"}, {"data", QJsonArray{7, Prefix + "0", value}}});
    }
};
QLineEdit *nameDraft(CameraMetadataPanel &panel) { return panel.findChild<QLineEdit *>(QStringLiteral("cloudMetadataNameDraft")); }
QPushButton *renameButton(CameraMetadataPanel &panel) { return panel.findChild<QPushButton *>(QStringLiteral("cloudMetadataRename")); }
void editName(CameraMetadataPanel &panel, const QString &name) {
    auto *edit = nameDraft(panel); edit->selectAll(); QTest::keyClicks(edit, name);
}
}

class CameraMetadataUiTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void cloudInformationIsSeparateAndCapabilitiesDisableWrites() {
        MetadataServer server(false); MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QTemporaryDir temporary; CameraMetadataManager manager(&client, temporary.path());
        const auto device = client.devices().first(); CameraMetadataPanel panel(&manager, device); panel.show();
        QTRY_VERIFY(manager.state(device).known);
        QCOMPARE(panel.findChild<QLabel *>("cloudMetadataName")->text(), QString("Cloud camera"));
        QCOMPARE(panel.findChild<QLabel *>("cloudMetadataHome")->text(), QString("Cloud home"));
        QCOMPARE(panel.findChild<QLabel *>("cloudMetadataRoom")->text(), QString("Cloud room"));
        QVERIFY(!nameDraft(panel)->isEnabled()); QVERIFY(!renameButton(panel)->isEnabled());
        QVERIFY(!panel.findChild<QComboBox *>("cloudMetadataRoomDraft")->isEnabled());
        QVERIFY(panel.findChild<QCheckBox *>("cloudMetadataSync")->isChecked());
        QCOMPARE(server.commands.size(), 0);
        const QString preview = qEnvironmentVariable("MATTERDESK_METADATA_PREVIEW");
        if (!preview.isEmpty()) {
            QDir().mkpath(preview); panel.resize(640, 580); QTest::qWait(20);
            QVERIFY(panel.grab().save(QDir(preview).filePath("metadata-cloud-panel.png")));
        }
        panel.hide(); panel.show(); QTest::qWait(50); QCOMPARE(server.commands.size(), 0);
    }
    void newerRevisionPreservesDraftAndRequiresExplicitRebase() {
        MetadataServer server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QTemporaryDir temporary; CameraMetadataManager manager(&client, temporary.path());
        const auto device = client.devices().first(); CameraMetadataPanel panel(&manager, device); panel.show(); QTRY_VERIFY(manager.state(device).known);
        editName(panel, "My draft"); QVERIFY(renameButton(panel)->isEnabled());
        server.reportSnapshot(snapshot(2, "Cloud renamed elsewhere"));
        QTRY_COMPARE(manager.state(device).snapshot.revision, quint64(2));
        QCOMPARE(nameDraft(panel)->text(), QString("My draft")); QVERIFY(!renameButton(panel)->isEnabled());
        auto *rebase = panel.findChild<QPushButton *>("cloudMetadataRebase"); QVERIFY(rebase->isVisible());
        QVERIFY(panel.findChild<QLabel *>("cloudMetadataDraftStatus")->text().contains(QStringLiteral("草稿已保留")));
        QTest::mouseClick(rebase, Qt::LeftButton); QVERIFY(renameButton(panel)->isEnabled());
        QCOMPARE(nameDraft(panel)->text(), QString("My draft")); QCOMPARE(server.commands.size(), 0);
    }
    void sourceReplacementInvalidatesOldDraft() {
        MetadataServer server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QTemporaryDir temporary; CameraMetadataManager manager(&client, temporary.path());
        const auto device = client.devices().first(); CameraMetadataPanel panel(&manager, device); panel.show(); QTRY_VERIFY(manager.state(device).known);
        editName(panel, "Old camera target"); QVERIFY(renameButton(panel)->isEnabled());
        server.reportSnapshot(snapshot(1, "Replacement camera", QString(64, QLatin1Char('c'))));
        QTRY_COMPARE(manager.state(device).snapshot.identity, QString(64, QLatin1Char('c')));
        QTRY_COMPARE(nameDraft(panel)->text(), QString("Replacement camera")); QVERIFY(!renameButton(panel)->isEnabled());
        QCOMPARE(server.commands.size(), 0);
    }
    void acknowledgementWaitsForAuthoritativeSnapshot() {
        MetadataServer server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QTemporaryDir temporary; CameraMetadataManager manager(&client, temporary.path());
        const auto device = client.devices().first(); CameraMetadataPanel panel(&manager, device); panel.show(); QTRY_VERIFY(manager.state(device).known);
        editName(panel, "Confirmed name"); server.holdReadsAfterCommand = true;
        QTest::mouseClick(renameButton(panel), Qt::LeftButton); QTRY_COMPARE(server.commands.size(), 1);
        server.reply(server.commands.first(), QJsonValue(QJsonValue::Null)); QTRY_VERIFY(!server.heldReads.isEmpty());
        QVERIFY(manager.state(device).busy); QVERIFY(!manager.state(device).confirmed);
        QCOMPARE(panel.findChild<QLabel *>("cloudMetadataName")->text(), QString("Cloud camera"));
        QVERIFY(!renameButton(panel)->isEnabled());
        QVERIFY(panel.findChild<QLabel *>("cloudMetadataDraftStatus")->text().contains(QStringLiteral("不等于")));
        server.attributes.insert(Prefix + "0", snapshot(2, "Confirmed name")); server.releaseReads();
        QTRY_VERIFY(!manager.state(device).busy); QVERIFY(manager.state(device).confirmed);
        QCOMPARE(panel.findChild<QLabel *>("cloudMetadataName")->text(), QString("Confirmed name"));
        QCOMPARE(server.commands.size(), 1); QVERIFY(!renameButton(panel)->isEnabled());
    }
    void idempotentTargetStillNeedsFreshObservedAt() {
        MetadataServer server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QTemporaryDir temporary; CameraMetadataManager manager(&client, temporary.path());
        const auto device = client.devices().first(); CameraMetadataPanel panel(&manager, device); panel.show(); QTRY_VERIFY(manager.state(device).known);
        editName(panel, "Cloud camera"); server.holdReadsAfterCommand = true;
        QTest::mouseClick(renameButton(panel), Qt::LeftButton); QTRY_COMPARE(server.commands.size(), 1);
        server.reply(server.commands.first(), QJsonValue(QJsonValue::Null)); QTRY_VERIFY(!server.heldReads.isEmpty());
        server.releaseReads(); QTest::qWait(100);
        QVERIFY(manager.state(device).busy); QVERIFY(!manager.state(device).confirmed);
        auto fresh = snapshot(); fresh.insert("8", "2026-10-24T08:00:00.001Z");
        server.attributes.insert(Prefix + "0", fresh);
        QTRY_VERIFY_WITH_TIMEOUT(!manager.state(device).busy, 2500); QVERIFY(manager.state(device).confirmed);
        QCOMPARE(manager.state(device).snapshot.revision, quint64(1)); QCOMPARE(server.commands.size(), 1);
        QVERIFY(!renameButton(panel)->isEnabled());
    }
    void failedWriteKeepsDraftAndRefreshRecoversWithoutRetryingWrite() {
        MetadataServer server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QTemporaryDir temporary; CameraMetadataManager manager(&client, temporary.path());
        const auto device = client.devices().first(); CameraMetadataPanel panel(&manager, device); panel.show(); QTRY_VERIFY(manager.state(device).known);
        editName(panel, "Keep this target"); QTest::mouseClick(renameButton(panel), Qt::LeftButton); QTRY_COMPARE(server.commands.size(), 1);
        server.send({{"message_id", server.commands.first().value("message_id")}, {"error_code", 8}, {"status", 146}, {"details", "Synthetic revision conflict"}});
        QTRY_VERIFY(!manager.state(device).busy); QVERIFY(manager.state(device).stale);
        QCOMPARE(nameDraft(panel)->text(), QString("Keep this target")); QVERIFY(!renameButton(panel)->isEnabled());
        auto *refresh = panel.findChild<QPushButton *>("cloudMetadataRefresh"); QVERIFY(refresh->isEnabled());
        QTest::mouseClick(refresh, Qt::LeftButton); QTRY_VERIFY(!manager.state(device).stale);
        QCOMPARE(nameDraft(panel)->text(), QString("Keep this target")); QCOMPARE(server.commands.size(), 1);
        QVERIFY(renameButton(panel)->isEnabled());
    }
    void explicitRoomChangeUsesCloudRoomIdAndAuthoritativeConfirmation() {
        MetadataServer server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QTemporaryDir temporary; CameraMetadataManager manager(&client, temporary.path());
        const auto device = client.devices().first(); CameraMetadataPanel panel(&manager, device); panel.show(); QTRY_VERIFY(manager.state(device).known);
        auto *room = panel.findChild<QComboBox *>("cloudMetadataRoomDraft"); QVERIFY(room->isEnabled());
        room->setFocus(); QTest::keyClick(room, Qt::Key_End);
        QCOMPARE(room->currentData().toString(), QString("20"));
        auto *move = panel.findChild<QPushButton *>("cloudMetadataMove"); QVERIFY(move->isEnabled());
        server.holdReadsAfterCommand = true; QTest::mouseClick(move, Qt::LeftButton); QTRY_COMPARE(server.commands.size(), 1);
        const auto args = server.commands.first().value("args").toObject();
        QCOMPARE(args.value("cluster_id").toInteger(), qint64(4294048770LL));
        QCOMPARE(args.value("command_name").toString(), QString("MoveDevice"));
        QCOMPARE(args.value("payload").toObject().value("roomId").toString(), QString("20"));
        server.reply(server.commands.first(), QJsonValue(QJsonValue::Null)); QTRY_VERIFY(!server.heldReads.isEmpty());
        QCOMPARE(panel.findChild<QLabel *>("cloudMetadataRoom")->text(), QString("Cloud room"));
        auto after = snapshot(2); after.insert("6", "20"); after.insert("7", "Study");
        server.attributes.insert(Prefix + "0", after); server.releaseReads(); QTRY_VERIFY(!manager.state(device).busy);
        QVERIFY(manager.state(device).confirmed); QCOMPARE(panel.findChild<QLabel *>("cloudMetadataRoom")->text(), QString("Study"));
        QVERIFY(!move->isEnabled()); QCOMPARE(server.commands.size(), 1);
    }
    void utf8NameLimitAndUnassignedRoomCannotSubmit() {
        MetadataServer server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QTemporaryDir temporary; CameraMetadataManager manager(&client, temporary.path());
        const auto device = client.devices().first(); CameraMetadataPanel panel(&manager, device); panel.show(); QTRY_VERIFY(manager.state(device).known);
        const QString tooLong(54, QChar(0x6444)); // 162 UTF-8 bytes, within QLineEdit's character limit.
        nameDraft(panel)->setText(tooLong);
        QVERIFY(QMetaObject::invokeMethod(nameDraft(panel), "textEdited", Qt::DirectConnection, Q_ARG(QString, tooLong)));
        QCOMPARE(nameDraft(panel)->text(), tooLong); QVERIFY(!renameButton(panel)->isEnabled());
        QVERIFY(panel.findChild<QLabel *>("cloudMetadataDraftStatus")->text().contains(QStringLiteral("160")));
        const QString exactLimit = QString(53, QChar(0x6444)) + QLatin1Char('a');
        nameDraft(panel)->setText(exactLimit);
        QVERIFY(QMetaObject::invokeMethod(nameDraft(panel), "textEdited", Qt::DirectConnection, Q_ARG(QString, exactLimit)));
        QVERIFY(renameButton(panel)->isEnabled());
        auto *room = panel.findChild<QComboBox *>("cloudMetadataRoomDraft");
        room->setCurrentIndex(room->findData(QString()));
        QVERIFY(QMetaObject::invokeMethod(room, "activated", Qt::DirectConnection, Q_ARG(int, room->currentIndex())));
        QVERIFY(!panel.findChild<QPushButton *>("cloudMetadataMove")->isEnabled());
        QCOMPARE(server.commands.size(), 0);
    }
    void incompleteMetadataClearsActualValuesButRetainsSameSourceDraft() {
        MetadataServer server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QTemporaryDir temporary; CameraMetadataManager manager(&client, temporary.path());
        const auto device = client.devices().first(); CameraMetadataPanel panel(&manager, device); panel.show(); QTRY_VERIFY(manager.state(device).known);
        editName(panel, "Keep local draft"); QVERIFY(renameButton(panel)->isEnabled());
        auto incomplete = snapshot(2); for (const auto key : {"3", "5", "6", "7", "8"}) incomplete.insert(key, "");
        server.attributes.insert(Prefix + "1", QJsonArray{}); server.attributes.insert(Prefix + "4", false);
        server.attributes.insert(Prefix + "6", "metadata_incomplete"); server.reportSnapshot(incomplete);
        QTRY_COMPARE(manager.state(device).lastError, QStringLiteral("metadata_incomplete"));
        QVERIFY(manager.state(device).known); QVERIFY(!manager.state(device).available);
        QCOMPARE(manager.state(device).snapshot.name, QString());
        QCOMPARE(panel.findChild<QLabel *>("cloudMetadataName")->text(), QStringLiteral("暂不可用"));
        QCOMPARE(panel.findChild<QLabel *>("cloudMetadataHome")->text(), QStringLiteral("暂不可用"));
        QCOMPARE(panel.findChild<QLabel *>("cloudMetadataRoom")->text(), QStringLiteral("暂不可用"));
        QCOMPARE(panel.findChild<QLabel *>("cloudMetadataObservedAt")->text(), QStringLiteral("暂不可用"));
        QCOMPARE(nameDraft(panel)->text(), QStringLiteral("Keep local draft")); QVERIFY(!renameButton(panel)->isEnabled());
        QCOMPARE(panel.findChild<QComboBox *>("cloudMetadataRoomDraft")->currentIndex(), -1);
        QVERIFY(!panel.findChild<QPushButton *>("cloudMetadataMove")->isEnabled());
        QVERIFY(!panel.findChild<QCheckBox *>("cloudMetadataSync")->isEnabled()); QCOMPARE(server.commands.size(), 0);
    }
    void localSaveDoesNotWriteCloud() {
        MetadataServer server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QTemporaryDir temporary; HomeStore store(&client, temporary.path()); store.setProfile("fixture-local");
        const auto device = client.devices().first(); AccessoryPanel panel(&client, &store, device); panel.show();
        auto *name = panel.findChild<QLineEdit *>("accessoryName"); QVERIFY(name); QTest::keyClicks(name, "Local only");
        auto *save = panel.findChild<QPushButton *>("saveAccessory"); QVERIFY(save); QTest::mouseClick(save, Qt::LeftButton);
        QCOMPARE(store.accessory(device.key).value("name").toString(), QString("Local only"));
        QVERIFY(panel.findChild<QLabel *>("localMetadataHint")->text().contains(QStringLiteral("不会修改米家")));
        QVERIFY(panel.findChild<CameraMetadataPanel *>()); QTest::qWait(50); QCOMPARE(server.commands.size(), 0);
        const QString preview = qEnvironmentVariable("MATTERDESK_METADATA_PREVIEW");
        if (!preview.isEmpty()) {
            panel.resize(680, 950); QTest::qWait(20);
            QVERIFY(panel.grab().save(QDir(preview).filePath("metadata-accessory-panel.png")));
        }
        QCOMPARE(server.attributes.value(Prefix + "0").toObject().value("3").toString(), QString("Cloud camera"));
    }
    void profileChangeClearsCloudDrafts() {
        MetadataServer server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QTemporaryDir temporary; CameraMetadataManager manager(&client, temporary.path());
        const auto device = client.devices().first(); CameraMetadataPanel panel(&manager, device); panel.show(); QTRY_VERIFY(manager.state(device).known);
        editName(panel, "Private draft"); manager.setProfile("second");
        QTRY_VERIFY(nameDraft(panel)->text() != QStringLiteral("Private draft")); QVERIFY(!renameButton(panel)->isEnabled());
        QCOMPARE(server.commands.size(), 0);
    }
};
QTEST_MAIN(CameraMetadataUiTest)
#include "tst_camerametadataui.moc"
