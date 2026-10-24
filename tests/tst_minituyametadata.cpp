#include "miniwindow.h"
#include "camerawindow.h"
#include "tuyametadata.h"

#include <QApplication>
#include <QBuffer>
#include <QDialog>
#include <QDirIterator>
#include <QFile>
#include <QFont>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>
#include <memory>

namespace {
const QString Token = QStringLiteral("MINI-TEST-ONLY-PRIVATE-BEARER-TOKEN-0123456789");
const QString UniqueId = QStringLiteral("mini-test-native-matter-unique-id");
const QString SourceDeviceId = QStringLiteral("d_") + QString(64, 'a');
const QString SourceHomeId = QStringLiteral("h_") + QString(64, 'b');
const QString SourceRoomId = QStringLiteral("r_") + QString(64, 'c');

QJsonObject cameraNode(bool mijia = false) {
    QJsonObject attributes{
        {"0/40/5", QStringLiteral("OHF 测试桥")},
        {"1/29/0", QJsonArray{QJsonObject{{"deviceType", 0x0142}, {"revision", 1}}}},
        {"1/29/1", QJsonArray{29, 57, 0x0551, 0x0553}},
        {"1/57/5", QStringLiteral("OHF 原始摄像头")},
        {"1/57/17", true}, {"1/57/18", UniqueId}
    };
    if (mijia) {
        attributes.insert("1/29/1", QJsonArray{29, 57, 0x0551, 0x0553, qint64(4294048770LL)});
        attributes.insert("1/4294048770/0", QJsonObject{
            {"0", QString(64, 'e')}, {"1", "00000000-0000-4000-8000-000000000001"},
            {"2", 1}, {"3", QStringLiteral("米家优先名称")}, {"4", QString(64, 'f')},
            {"5", QStringLiteral("米家家庭")}, {"6", "42"}, {"7", QStringLiteral("米家房间")},
            {"8", "2026-10-24T00:00:00.000Z"}
        });
        attributes.insert("1/4294048770/1", QJsonArray{QJsonObject{{"0", "42"}, {"1", QStringLiteral("米家房间")}}});
        attributes.insert("1/4294048770/2", QJsonObject{{"0", true}, {"1", false}, {"2", false}, {"3", false}});
        attributes.insert("1/4294048770/3", true); attributes.insert("1/4294048770/4", true);
        attributes.insert("1/4294048770/5", false); attributes.insert("1/4294048770/6", "");
    }
    return {{"node_id", 9101}, {"available", true}, {"attributes", attributes}};
}

class ControllerFixture : public QObject {
public:
    QWebSocketServer server{QStringLiteral("Mini Tuya metadata fixture"), QWebSocketServer::NonSecureMode};
    QPointer<QWebSocket> peer;
    QJsonArray nodes{cameraNode()};
    QList<QJsonObject> requests;
    int connections = 0;

    ControllerFixture() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QWebSocketServer::newConnection, this, [this] {
            ++connections; peer = server.nextPendingConnection(); peer->setParent(this);
            connect(peer, &QWebSocket::textMessageReceived, this, [this](const QString &text) {
                const QJsonObject request = QJsonDocument::fromJson(text.toUtf8()).object();
                requests.append(request);
                const QString command = request.value("command").toString();
                if (command == "start_listening" || command == "get_nodes") reply(request, nodes);
                else if (command == "camera_get_capabilities") reply(request, capabilities());
                else if (command == "camera_snapshot") {
                    QImage image(96, 54, QImage::Format_RGB32); image.fill(QColor("#416f89"));
                    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "JPEG");
                    reply(request, QJsonObject{{"data", QString::fromLatin1(bytes.toBase64())}, {"codec", "JPEG"},
                        {"resolution", QJsonObject{{"width", 96}, {"height", 54}}}, {"degraded", false},
                        {"stream_id", 77}, {"provenance", "adopted"}});
                } else if (command == "read_attribute") {
                    const auto requested = request.value("args").toObject().value("attribute_path");
                    const auto paths = requested.isArray() ? requested.toArray() : QJsonArray{requested};
                    const auto attributes = nodes.isEmpty() ? QJsonObject{} : nodes[0].toObject().value("attributes").toObject();
                    QJsonObject result;
                    for (const auto &path : paths) if (attributes.contains(path.toString())) result.insert(path.toString(), attributes.value(path.toString()));
                    reply(request, result);
                } else if (command == "camera_release_stream" || command == "camera_stop_stream") {
                    reply(request, QJsonValue(QJsonValue::Null));
                } else {
                    // Exercise the actual player setup once, without opening live
                    // media or pretending this fixture is a camera stream.
                    send({{"message_id", request.value("message_id")}, {"error_code", 8},
                          {"details", "This fixture does not open live media"}});
                }
            });
            send({{"schema_version", 14}, {"min_supported_schema_version", 11},
                  {"sdk_version", "Mini metadata test fixture"}, {"fabric_id", 42}});
        });
    }
    QUrl url() const { return QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(server.serverPort())); }
    void send(const QJsonObject &value) {
        if (peer && peer->state() == QAbstractSocket::ConnectedState)
            peer->sendTextMessage(QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)));
    }
    void reply(const QJsonObject &request, const QJsonValue &value) {
        send({{"message_id", request.value("message_id")}, {"result", value}});
    }
    int count(const QString &command) const {
        int total = 0;
        for (const auto &request : requests) total += request.value("command").toString() == command;
        return total;
    }
    QJsonObject capabilities() const {
        return {{"features", QJsonArray{"Video", "Audio", "Snapshot"}}, {"privacy", QJsonObject{}},
            {"video", QJsonObject{{"codecs", QJsonArray{"H264"}}, {"rate_distortion_points", QJsonArray{}}}},
            {"audio", QJsonObject{{"codecs", QJsonArray{"OPUS"}}, {"two_way_talk_support", "None"}}},
            {"snapshot", QJsonObject{{"capabilities", QJsonArray{QJsonObject{
                {"resolution", QJsonObject{{"width", 96}, {"height", 54}}}, {"max_frame_rate", 1},
                {"image_codec", "JPEG"}, {"requires_encoded_pixels", false}, {"requires_hardware_encoder", false}
            }}}}}, {"sessions", QJsonArray{}}};
    }
};

class MetadataFixture : public QObject {
public:
    struct Request { QByteArray method, path, authorization; QJsonObject body; };
    QTcpServer server;
    QList<Request> requests;
    QString name = QStringLiteral("涂鸦完整摄像头名称"), room = QStringLiteral("涂鸦书房");
    QString uniqueId = UniqueId;
    int revision = 1;

    MetadataFixture() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection(); socket->setParent(this);
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, bytes] {
                    bytes->append(socket->readAll());
                    const int separator = bytes->indexOf("\r\n\r\n");
                    if (separator < 0) return;
                    const auto lines = bytes->left(separator).split('\n');
                    const auto first = lines[0].trimmed().split(' ');
                    if (first.size() < 2) return;
                    int length = 0; QByteArray authorization;
                    for (const auto &line : lines) {
                        const int colon = line.indexOf(':'); if (colon < 0) continue;
                        const auto key = line.left(colon).trimmed().toLower();
                        const auto value = line.mid(colon + 1).trimmed();
                        if (key == "content-length") length = value.toInt();
                        if (key == "authorization") authorization = value;
                    }
                    if (bytes->size() < separator + 4 + length) return;
                    const Request request{first[0], first[1], authorization,
                        QJsonDocument::fromJson(bytes->mid(separator + 4, length)).object()};
                    requests.append(request); bytes->clear();
                    const bool authorized = authorization == QByteArray("Bearer ") + Token.toUtf8();
                    const bool read = request.method == "GET" && (request.path == "/v1/capabilities" || request.path == "/v1/metadata");
                    const int status = !authorized ? 401 : read ? 200 : 405;
                    const auto body = QJsonDocument(status == 200 ? snapshot() : QJsonObject{{"code", status == 401 ? "unauthorized" : "unsupported_operation"}})
                        .toJson(QJsonDocument::Compact);
                    socket->write(QByteArray("HTTP/1.1 ") + QByteArray::number(status) + " Fixture\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())); }
    QJsonObject snapshot() const {
        return {{"schemaVersion", 1}, {"serverId", "8c4eec7f-7f15-4507-b7ac-5189b3b821a0"},
            {"sourceEpoch", "6b25f764-1d0e-43c0-a129-4ab09b158c83"}, {"revision", QString::number(revision)},
            {"observedAt", "2026-10-24T08:00:00.000Z"}, {"available", true}, {"roomsAvailable", true},
            {"sourceKind", "tuya-project-paas"},
            {"capabilities", QJsonObject{{"read", true}, {"renameDevice", false}, {"moveDevice", false}, {"renameRoom", false}}},
            {"limitations", QJsonObject{{"renameDevice", "unverified_tuya_device_write_contract"},
                {"moveDevice", "unverified_tuya_room_write_contract"}, {"renameRoom", "unverified_tuya_room_write_contract"}}},
            {"homes", QJsonArray{QJsonObject{{"id", SourceHomeId}, {"name", QStringLiteral("涂鸦家庭")}}}},
            {"rooms", QJsonArray{QJsonObject{{"id", SourceRoomId}, {"homeId", SourceHomeId}, {"name", room}}}},
            {"devices", QJsonArray{QJsonObject{{"id", SourceDeviceId}, {"name", name}, {"homeId", SourceHomeId},
                {"roomId", SourceRoomId}, {"roomStatus", "assigned"},
                {"bindings", QJsonObject{{"hapSerialNumber", QJsonValue(QJsonValue::Null)},
                    {"hapAccessoryUuid", QJsonValue(QJsonValue::Null)}, {"matterUniqueIds", QJsonArray{uniqueId}}}}}}}};
    }
    int mutations() const {
        int count = 0; for (const auto &request : requests) count += request.method != "GET"; return count;
    }
};

MatterDevice firstCamera(MiniWindow &window) {
    for (const auto &device : window.client()->devices()) if (device.hasCamera) return device;
    return {};
}
TuyaMetadataManager *metadataManager(MiniWindow &window) {
    return window.findChild<TuyaMetadataManager *>(QStringLiteral("miniTuyaMetadataManager"));
}
}

class MiniTuyaMetadataTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("MatterDeskTests");
        QCoreApplication::setApplicationName("MiniTuyaMetadata");
        QVERIFY(m_storage.isValid());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, m_storage.path());
        qputenv("XDG_DATA_HOME", (m_storage.path() + "/data").toUtf8());
    }
    void init() {
        QSettings().clear();
        QDir(m_storage.path() + "/data").removeRecursively();
    }

    void settingsEntryWorksWithoutCameraAndAfterControllerDisconnect() {
        ControllerFixture controller; controller.nodes = {};
        MiniWindow window(controller.url()); window.show();
        QTRY_VERIFY(window.client()->isConnected());
        auto *entry = window.findChild<QPushButton *>("miniTuyaMetadata"); QVERIFY(entry);
        QVERIFY(entry->isVisible()); QVERIFY(entry->isEnabled());
        QTest::mouseClick(entry, Qt::LeftButton);
        QTRY_VERIFY(window.findChild<QDialog *>("miniTuyaMetadataDialog"));
        auto *dialog = window.findChild<QDialog *>("miniTuyaMetadataDialog"); QVERIFY(dialog->isVisible());
        QVERIFY(dialog->findChild<QLineEdit *>("tuyaUrl"));
        auto *token = dialog->findChild<QLineEdit *>("tuyaToken"); QVERIFY(token);
        QCOMPARE(token->echoMode(), QLineEdit::Password);
        QTest::mouseClick(entry, Qt::LeftButton);
        QCOMPARE(window.findChildren<QDialog *>("miniTuyaMetadataDialog").size(), 1);
        QCOMPARE(window.findChild<QDialog *>("miniTuyaMetadataDialog"), dialog);
        QCOMPARE(controller.connections, 1); QCOMPARE(controller.count("commission_with_code"), 0);
        controller.server.close(); if (controller.peer) controller.peer->close();
        QTRY_VERIFY(!window.client()->isConnected());
        QTRY_VERIFY(window.findChildren<QDialog *>("miniTuyaMetadataDialog").isEmpty());
        QVERIFY(entry->isEnabled());
        QTest::mouseClick(entry, Qt::LeftButton);
        QTRY_VERIFY(window.findChild<QDialog *>("miniTuyaMetadataDialog"));
        QVERIFY(window.findChild<QDialog *>("miniTuyaMetadataDialog")->isVisible());
        QCOMPARE(controller.count("camera_start_stream"), 0);
        QCOMPARE(controller.count("commission_with_code"), 0);
    }

    void metadataConnectUsesSessionTokenWithoutWritingItToSettings() {
        ControllerFixture controller; controller.nodes = {}; MetadataFixture source;
        MiniWindow window(controller.url()); window.show();
        QTRY_VERIFY(window.client()->isConnected());
        auto *manager = metadataManager(window); QVERIFY(manager);
        auto *entry = window.findChild<QPushButton *>("miniTuyaMetadata"); QVERIFY(entry); entry->click();
        auto *dialog = window.findChild<QDialog *>("miniTuyaMetadataDialog"); QVERIFY(dialog);
        auto *url = dialog->findChild<QLineEdit *>("tuyaUrl"); QVERIFY(url);
        auto *token = dialog->findChild<QLineEdit *>("tuyaToken"); QVERIFY(token);
        auto *connect = dialog->findChild<QPushButton *>("tuyaConnect"); QVERIFY(connect);
        url->setText(source.url().toString()); token->setText(Token); connect->click();
        QTRY_VERIFY(manager->isConfigured());
        QTRY_VERIFY(!manager->snapshot().isEmpty());
        QVERIFY(!source.requests.isEmpty());
        for (const auto &request : source.requests) QCOMPARE(request.authorization, QByteArray("Bearer ") + Token.toUtf8());
        QSettings settings; settings.sync();
        for (const auto &key : settings.allKeys()) QVERIFY(!settings.value(key).toString().contains(Token));
        QDirIterator files(m_storage.path(), QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            QFile file(files.next()); if (file.open(QIODevice::ReadOnly)) QVERIFY(!file.readAll().contains(Token.toUtf8()));
        }
        QCOMPARE(source.mutations(), 0); QCOMPARE(controller.count("commission_with_code"), 0);
    }

    void explicitFollowChangesOnlyOverlayAndUnverifiedOperationsCannotWrite() {
        ControllerFixture controller; MetadataFixture source;
        MiniWindow window(controller.url()); window.show();
        QTRY_VERIFY(window.client()->isConnected()); QTRY_VERIFY(firstCamera(window).hasCamera);
        const MatterDevice before = firstCamera(window);
        QCOMPARE(tuyaMatterUniqueId(before), UniqueId);
        auto *manager = metadataManager(window); QVERIFY(manager);
        QVERIFY(manager->configure(source.url(), Token));
        QTRY_VERIFY(!manager->snapshot().isEmpty());
        QVERIFY(manager->setSourceHome(SourceHomeId)); QVERIFY(manager->setEnabled(true));
        QVERIFY(manager->setMode("followSource"));
        QTRY_VERIFY(!manager->state(before).busy);
        QVERIFY(manager->bindDevice(before, SourceDeviceId));
        QVERIFY(manager->followSource(before));
        auto *title = window.findChild<QLabel *>("miniTitle"); QVERIFY(title);
        auto *location = window.findChild<QLabel *>("miniMijiaLocation"); QVERIFY(location);
        QTRY_COMPARE(title->text(), source.name);
        QTRY_VERIFY(location->text().contains(source.room));
        QTRY_VERIFY(manager->state(before).known);
        QVERIFY(manager->state(before).bound); QVERIFY(!manager->state(before).canRename);
        QVERIFY(!manager->state(before).canMoveRoom);
        QVERIFY(!manager->rename(before, QStringLiteral("禁止写入")));
        QVERIFY(!manager->moveRoom(before, SourceRoomId));
        QCOMPARE(source.mutations(), 0);
        const int snapshots = controller.count("camera_snapshot");
        source.name = QStringLiteral("涂鸦刷新后的完整名称"); source.room = QStringLiteral("涂鸦新房间"); ++source.revision;
        QTRY_VERIFY(!manager->state(before).busy); QVERIFY(manager->refresh());
        QTRY_COMPARE(title->text(), source.name); QTRY_VERIFY(location->text().contains(source.room));
        const MatterDevice after = firstCamera(window);
        QCOMPARE(after.key, before.key); QCOMPARE(after.nodeId, before.nodeId); QCOMPARE(after.endpointId, before.endpointId);
        QCOMPARE(after.controllerGeneration, before.controllerGeneration);
        QCOMPARE(after.name, before.name); QCOMPARE(tuyaMatterUniqueId(after), UniqueId);
        QCOMPARE(controller.connections, 1); QCOMPARE(controller.count("camera_snapshot"), snapshots);
        QCOMPARE(controller.count("camera_start_stream"), 0); QCOMPARE(controller.count("camera_stop_stream"), 0);
        QCOMPARE(controller.count("commission_with_code"), 0); QCOMPARE(controller.count("device_command"), 0);
        QCOMPARE(controller.count("write_attribute"), 0);
    }

    void unmatchedIdentityDoesNotAutomaticallyOverlayOrWrite() {
        ControllerFixture controller; MetadataFixture source; source.uniqueId = QStringLiteral("unmatched-source-identity");
        MiniWindow window(controller.url()); window.show();
        QTRY_VERIFY(window.client()->isConnected()); QTRY_VERIFY(firstCamera(window).hasCamera);
        const MatterDevice camera = firstCamera(window);
        auto *manager = metadataManager(window); QVERIFY(manager);
        QVERIFY(manager->configure(source.url(), Token)); QTRY_VERIFY(!manager->snapshot().isEmpty());
        QVERIFY(manager->setSourceHome(SourceHomeId)); QVERIFY(manager->setEnabled(true)); QVERIFY(manager->setMode("followSource"));
        QVERIFY(manager->suggestedBinding(camera).isEmpty());
        QVERIFY(!manager->state(camera).bound);
        QVERIFY(!manager->followSource(camera));
        QVERIFY(!manager->rename(camera, QStringLiteral("不得根据同名猜测")));
        QCOMPARE(window.findChild<QLabel *>("miniTitle")->text(), camera.name);
        QCOMPARE(source.mutations(), 0); QCOMPARE(controller.count("device_command"), 0);
        QCOMPARE(controller.count("commission_with_code"), 0);
    }

    void sourceProfileChangeClearsOverlayAndDoesNotReuseOldSessionToken() {
        ControllerFixture controller; MetadataFixture source;
        MiniWindow window(controller.url()); window.show();
        QTRY_VERIFY(window.client()->isConnected()); QTRY_VERIFY(firstCamera(window).hasCamera);
        const MatterDevice camera = firstCamera(window);
        auto *manager = metadataManager(window); QVERIFY(manager);
        QVERIFY(manager->configure(source.url(), Token)); QTRY_VERIFY(!manager->snapshot().isEmpty());
        QVERIFY(manager->setSourceHome(SourceHomeId)); QVERIFY(manager->setEnabled(true));
        QVERIFY(manager->bindDevice(camera, SourceDeviceId)); QVERIFY(manager->followSource(camera));
        QTRY_COMPARE(window.findChild<QLabel *>("miniTitle")->text(), source.name);
        const int requests = source.requests.size();
        manager->setProfile(QStringLiteral("a-different-OHF-controller-and-fabric"));
        QVERIFY(!manager->state(camera).bound); QVERIFY(manager->displayName(camera).isEmpty());
        QVERIFY(!manager->isConfigured());
        QVERIFY(!manager->refresh());
        QCOMPARE(source.requests.size(), requests);
        QTRY_COMPARE(window.findChild<QLabel *>("miniTitle")->text(), camera.name);
        QCOMPARE(controller.connections, 1); QCOMPARE(controller.count("commission_with_code"), 0);
    }

    void mijiaLabelsKeepPriorityWhenTheSameCameraHasTuyaMetadata() {
        ControllerFixture controller; controller.nodes = {cameraNode(true)}; MetadataFixture source;
        MiniWindow window(controller.url()); window.show();
        QTRY_VERIFY(window.client()->isConnected()); QTRY_VERIFY(firstCamera(window).hasCamera);
        auto *title = window.findChild<QLabel *>("miniTitle"); QVERIFY(title);
        auto *location = window.findChild<QLabel *>("miniMijiaLocation"); QVERIFY(location);
        QTRY_COMPARE(title->text(), QStringLiteral("米家优先名称"));
        const MatterDevice camera = firstCamera(window);
        auto *manager = metadataManager(window); QVERIFY(manager);
        QVERIFY(manager->configure(source.url(), Token)); QTRY_VERIFY(!manager->snapshot().isEmpty());
        QVERIFY(manager->setSourceHome(SourceHomeId)); QVERIFY(manager->setEnabled(true));
        QVERIFY(manager->bindDevice(camera, SourceDeviceId)); QVERIFY(manager->followSource(camera));
        QTRY_COMPARE(manager->displayName(camera), source.name);
        QCOMPARE(title->text(), QStringLiteral("米家优先名称"));
        QVERIFY(location->text().contains(QStringLiteral("米家房间")));
        QVERIFY(!location->text().contains(source.room));
        QCOMPARE(controller.connections, 1); QCOMPARE(controller.count("device_command"), 0);
        QCOMPARE(controller.count("commission_with_code"), 0);
    }

    void tuyaRefreshKeepsTheSameEmbeddedPlayerWithoutRestartingItsSession() {
        ControllerFixture controller; MetadataFixture source;
        MiniWindow window(controller.url()); window.show();
        QTRY_VERIFY(window.client()->isConnected()); QTRY_VERIFY(firstCamera(window).hasCamera);
        const MatterDevice camera = firstCamera(window);
        auto *manager = metadataManager(window); QVERIFY(manager);
        QVERIFY(manager->configure(source.url(), Token)); QTRY_VERIFY(!manager->snapshot().isEmpty());
        QVERIFY(manager->setSourceHome(SourceHomeId)); QVERIFY(manager->setEnabled(true));
        QVERIFY(manager->setMode("followSource"));
        QVERIFY(manager->bindDevice(camera, SourceDeviceId)); QVERIFY(manager->followSource(camera));
        auto *play = window.findChild<QPushButton *>("miniPlay"); QVERIFY(play); QTRY_VERIFY(play->isEnabled());
        play->click(); QTRY_COMPARE(window.findChildren<CameraWindow *>().size(), 1);
        QPointer<CameraWindow> live = window.findChild<CameraWindow *>(); QVERIFY(live);
        QTRY_COMPARE_WITH_TIMEOUT(controller.count("camera_start_stream"), 1, 15000);
        auto *view = live->findChild<QWebEngineView *>("cameraVideo"); QVERIFY(view);
        auto *page = view->page(); QVERIFY(page);
        const int snapshots = controller.count("camera_snapshot"), stops = controller.count("camera_stop_stream");
        source.name = QStringLiteral("播放期间更新涂鸦标签"); ++source.revision;
        QTRY_VERIFY(!manager->state(camera).busy); QVERIFY(manager->refresh());
        QTRY_COMPARE(window.findChild<QLabel *>("miniTitle")->text(), source.name);
        QCOMPARE(window.findChild<CameraWindow *>(), live.data()); QCOMPARE(live->findChild<QWebEngineView *>("cameraVideo"), view);
        QCOMPARE(view->page(), page); QCOMPARE(controller.count("camera_start_stream"), 1);
        QCOMPARE(controller.count("camera_stop_stream"), stops); QCOMPARE(controller.count("camera_snapshot"), snapshots);
        QCOMPARE(controller.connections, 1); QCOMPARE(controller.count("commission_with_code"), 0);
        QCOMPARE(controller.count("device_command"), 0); QCOMPARE(source.mutations(), 0);
    }
private:
    QTemporaryDir m_storage;
};

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", qgetenv("QTWEBENGINE_CHROMIUM_FLAGS") + " --disable-gpu");
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    app.setStyle("Fusion"); app.setFont(QFont("Noto Sans CJK SC", 10));
    MiniTuyaMetadataTest test; return QTest::qExec(&test, argc, argv);
}

#include "tst_minituyametadata.moc"
