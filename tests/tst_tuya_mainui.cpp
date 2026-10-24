#include "mainwindow.h"
#include "devicecard.h"
#include "homestore.h"
#include "tuyametadata.h"
#include "tuyametadatapanel.h"

#include <QApplication>
#include <QDialog>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>
#include <memory>

namespace {
const QString SourceHome = "h_" + QString(64, 'b');
const QString SourceRoom = "r_" + QString(64, 'c');
const QString SourceDevice = "d_" + QString(64, 'a');
const QString NativeIdentity = QStringLiteral("main-tuya-native-unique-id");
const QString Token = QStringLiteral("main-ui-fixture-bearer-token-more-than-32-characters");

class ControllerFixture : public QObject {
public:
    QWebSocketServer server{QStringLiteral("Tuya main UI Matter fixture"), QWebSocketServer::NonSecureMode};
    QPointer<QWebSocket> peer;
    QList<QJsonObject> mutations;
    QJsonArray nodes;

    ControllerFixture() {
        nodes = QJsonArray{QJsonObject{{"node_id", 77}, {"available", true}, {"attributes", QJsonObject{
            {"0/40/5", "Fixture bridge"},
            {"1/29/0", QJsonArray{QJsonObject{{"deviceType", 0x010a}, {"revision", 1}}}},
            {"1/29/1", QJsonArray{29, 57, 6}}, {"1/57/5", "Native endpoint name"},
            {"1/57/18", NativeIdentity}, {"1/57/17", true}, {"1/6/0", false}
        }}}};
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QWebSocketServer::newConnection, this, [this] {
            peer = server.nextPendingConnection(); peer->setParent(this);
            connect(peer, &QWebSocket::textMessageReceived, this, [this](const QString &text) {
                const auto request = QJsonDocument::fromJson(text.toUtf8()).object();
                const auto command = request.value("command").toString();
                QJsonValue result = QJsonValue(QJsonValue::Null);
                if (command == "start_listening" || command == "get_nodes") result = nodes;
                else if (command == "get_node") result = nodes.first();
                else mutations.append(request);
                send({{"message_id", request.value("message_id")}, {"result", result}});
            });
            send({{"schema_version", 14}, {"min_supported_schema_version", 11},
                {"sdk_version", "loopback metadata UI fixture"}, {"fabric_id", "tuya-main-fixture"}});
        });
    }
    void send(const QJsonObject &message) {
        if (peer) peer->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }
    QUrl url() const { return QUrl(QString("ws://127.0.0.1:%1/ws").arg(server.serverPort())); }
};

class MetadataFixture : public QObject {
public:
    QTcpServer server;
    QJsonObject snapshot;
    QHash<QString, QJsonObject> receipts;
    int mutations = 0;
    bool wrongAuthorization = false;
    MetadataFixture() {
        snapshot = {{"schemaVersion", 1}, {"serverId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"sourceEpoch", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"revision", "1"},
            {"observedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {"available", true}, {"roomsAvailable", true},
            {"sourceKind", "tuya-project-paas"},
            {"capabilities", QJsonObject{{"read", true}, {"renameDevice", false}, {"moveDevice", false}, {"renameRoom", false}}},
            {"limitations", QJsonObject{{"renameDevice", "unverified_tuya_device_write_contract"},
                {"moveDevice", "unverified_tuya_room_write_contract"}, {"renameRoom", "unverified_tuya_room_write_contract"}}},
            {"homes", QJsonArray{QJsonObject{{"id", SourceHome}, {"name", "Source home"}}}},
            {"rooms", QJsonArray{QJsonObject{{"id", SourceRoom}, {"homeId", SourceHome}, {"name", "Tuya study"}}}},
            {"devices", QJsonArray{QJsonObject{{"id", SourceDevice}, {"name", "Full Tuya source name"},
                {"homeId", SourceHome}, {"roomId", SourceRoom}, {"roomStatus", "assigned"},
                {"bindings", QJsonObject{{"hapSerialNumber", QJsonValue(QJsonValue::Null)},
                    {"hapAccessoryUuid", QJsonValue(QJsonValue::Null)}, {"matterUniqueIds", QJsonArray{NativeIdentity}}}}}}}};
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            auto *socket = server.nextPendingConnection(); socket->setParent(this);
            auto buffer = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
                buffer->append(socket->readAll());
                const int end = buffer->indexOf("\r\n\r\n");
                if (end < 0) return;
                const auto headers = buffer->left(end).split('\n');
                int length = 0; bool authorized = false;
                for (auto header : headers) {
                    header = header.trimmed();
                    if (header.toLower().startsWith("content-length:")) length = header.mid(15).trimmed().toInt();
                    if (header.toLower().startsWith("authorization:")) authorized = header.mid(14).trimmed() == "Bearer " + Token.toUtf8();
                }
                if (buffer->size() < end + 4 + length) return;
                socket->disconnect(this);
                const auto requestLine = headers.first().trimmed().split(' ');
                const QByteArray method = requestLine.value(0), path = requestLine.value(1);
                int status = authorized ? 200 : 401;
                QJsonObject result;
                if (!authorized) { wrongAuthorization = true; result = {{"schemaVersion", 1}, {"code", "permission_denied"}}; }
                else if (method == "GET" && path == "/v1/capabilities") {
                    result = snapshot; result.remove("homes"); result.remove("rooms"); result.remove("devices");
                } else if (method == "GET" && path == "/v1/metadata") result = snapshot;
                else if (method == "POST" && path == "/v1/metadata/commands") {
                    const auto command = QJsonDocument::fromJson(buffer->mid(end + 4, length)).object();
                    const auto operation = command.value("operation").toObject();
                    if (operation.value("type") != "refresh") ++mutations;
                    result = {{"schemaVersion", 1}, {"commandId", command.value("commandId")},
                        {"status", "confirmed"}, {"code", QJsonValue(QJsonValue::Null)}, {"snapshot", snapshot}};
                    receipts.insert(command.value("commandId").toString(), result); status = 202;
                } else if (path.startsWith("/v1/metadata/commands/")) {
                    result = receipts.value(QString::fromUtf8(path.mid(22)));
                    if (result.isEmpty()) status = 404;
                } else status = 404;
                const auto body = QJsonDocument(result).toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 " + QByteArray::number(status) + " Response\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                    + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1").arg(server.serverPort())); }
};

DeviceCard *onlyCard(MainWindow &window) {
    const auto cards = window.findChildren<DeviceCard *>();
    return cards.size() == 1 ? cards.first() : nullptr;
}
}

class TuyaMainUiTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("MatterDeskTests");
        QCoreApplication::setApplicationName("TuyaMainUi-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(settings.isValid());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settings.path());
    }
    void init() { QSettings().clear(); }
    void metadataSettingsAreSeparateFromLocalHomeEditing() {
        MainWindow window;
        QVERIFY(window.tuyaMetadata());
        QVERIFY(!window.tuyaMetadata()->isConfigured());
        QPushButton *settingsButton = nullptr;
        for (auto *button : window.findChildren<QPushButton *>("sideAction"))
            if (button->text().contains(QStringLiteral("家庭设置"))) settingsButton = button;
        QVERIFY(settingsButton);
        bool inspected = false;
        QTimer::singleShot(0, &window, [&] {
            for (auto *dialog : window.findChildren<QDialog *>()) {
                auto *panel = dialog->findChild<TuyaMetadataPanel *>("mainTuyaMetadataSettings");
                if (!panel) continue;
                QTimer::singleShot(3000, dialog, &QDialog::reject);
                auto *tabs = dialog->findChild<QTabWidget *>();
                QVERIFY(tabs);
                QCOMPARE(tabs->tabText(tabs->indexOf(panel)), QStringLiteral("涂鸦同步"));
                QVERIFY(!window.tuyaMetadata()->isConfigured());
                inspected = true; dialog->reject();
            }
        });
        settingsButton->click();
        QVERIFY(inspected);
    }
    void sourceOverlayPreservesLocalAliasesAndNeverChangesTheMatterFabric() {
        ControllerFixture controller; MetadataFixture metadata;
        MainWindow window; window.client()->connectToServer(controller.url());
        QTRY_VERIFY(window.client()->isConnected());
        QTRY_VERIFY(onlyCard(window));
        auto *manager = window.tuyaMetadata();
        QCOMPARE(manager->profile(), window.home()->profile());
        QVERIFY(manager->configure(metadata.url(), Token));
        QVERIFY(manager->setEnabled(true));
        QTRY_VERIFY(manager->snapshot().value("available").toBool());
        manager->setSourceHome(SourceHome);
        QVERIFY(manager->setMode(QStringLiteral("followSource")));
        const auto device = window.client()->devices().first();
        QVERIFY(manager->bindDevice(device, SourceDevice));
        QVERIFY(manager->followSource(device));
        QTRY_COMPARE(onlyCard(window)->device().name, QStringLiteral("Full Tuya source name"));
        QVERIFY(!manager->state(device).canRename);
        QVERIFY(!manager->rename(device, QStringLiteral("Unsupported PaaS rename")));
        QVERIFY(!manager->moveRoom(device, SourceRoom));
        QCOMPARE(metadata.mutations, 0);
        QCOMPARE(controller.mutations.size(), 0);
        const auto localRoom = window.home()->saveRoom({{"name", "Local room"}});
        QVERIFY(!localRoom.isEmpty());
        QVERIFY(window.home()->setAccessory(device.key, {{"name", "Private local alias"}, {"roomId", localRoom}}));
        QTRY_COMPARE(onlyCard(window)->device().name, QStringLiteral("Private local alias"));
        QCOMPARE(window.home()->accessory(device.key).value("roomId").toString(), localRoom);
        QCOMPARE(metadata.mutations, 0);
        QCOMPARE(controller.mutations.size(), 0);
        QVERIFY(!metadata.wrongAuthorization);
        window.client()->disconnectFromServer();
        QTRY_VERIFY(manager->profile().isEmpty());
        QVERIFY(!manager->isConfigured());
    }
private:
    QTemporaryDir settings;
};

QTEST_MAIN(TuyaMainUiTest)
#include "tst_tuya_mainui.moc"
