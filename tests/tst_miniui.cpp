#include "miniwindow.h"
#include "camerasnapshots.h"
#include "camerawindow.h"
#include "cameraptzcontrols.h"
#include "camerametadata.h"

#include <QAbstractButton>
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QDialog>
#include <QCursor>
#include <QMouseEvent>
#include <QScreen>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFont>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>
#include <cstdio>
#include <memory>

namespace {
const QColor Magenta(220, 30, 110);
const QColor Cobalt(30, 100, 225);
const QString TestCode = QStringLiteral("MT:MINIUI-TEST-ONLY");

QJsonObject cameraNode() {
    const QJsonObject attributes{
        {"0/40/5", "极简界面测试桥"},
        {"1/29/0", QJsonArray{QJsonObject{{"deviceType", 0x0142}, {"revision", 1}}}},
        {"1/29/1", QJsonArray{29, 57, 0x0551, 0x0553}},
        {"1/57/5", "合成测试摄像头"}, {"1/57/17", true},
        {"2/29/0", QJsonArray{QJsonObject{{"deviceType", 0x010a}, {"revision", 1}}}},
        {"2/29/1", QJsonArray{29, 57, 6}},
        {"2/57/5", "不应显示的测试插座"}, {"2/57/17", true}, {"2/6/0", false}
    };
    return {{"node_id", 9001}, {"available", true}, {"attributes", attributes}};
}

QJsonObject ptzCameraNode(bool cached = true) {
    auto node = cameraNode(); auto attrs = node.value("attributes").toObject();
    attrs.insert("1/29/1", QJsonArray{29,57,0x0551,0x0553,qint64(4294048769LL)});
    attrs.insert("1/4294048769/0",true); attrs.insert("1/4294048769/1",true); attrs.insert("1/4294048769/2",0);
    if(cached) { attrs.insert("1/4294048769/3",15); attrs.insert("1/4294048769/65529",QJsonArray{0}); }
    node.insert("attributes",attrs); return node;
}

QImage syntheticRoom() {
    QImage image(640, 360, QImage::Format_RGB32);
    QPainter p(&image); p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient wall(0, 0, 640, 250); wall.setColorAt(0, QColor("#eee8d7")); wall.setColorAt(1, QColor("#d6d8c6"));
    p.fillRect(image.rect(), wall); p.fillRect(0, 242, 640, 118, QColor("#b39774"));
    p.setPen(QPen(QColor("#9d8467"), 1));
    for (int x = -240; x < 950; x += 50) p.drawLine(320, 238, x, 360);
    p.setPen(Qt::NoPen); p.setBrush(QColor("#f6f4e9")); p.drawRoundedRect(48, 35, 155, 158, 4, 4);
    p.fillRect(57, 44, 137, 140, QColor("#afc1ae"));
    p.setBrush(QColor("#7e9d7a")); p.drawEllipse(60, 99, 97, 96); p.drawEllipse(113, 81, 90, 109);
    p.setPen(QPen(QColor("#f5f4e9"), 6)); p.drawLine(124, 44, 124, 184); p.drawLine(57, 113, 194, 113);
    p.setPen(Qt::NoPen); p.setBrush(QColor("#d8cfb6")); p.drawEllipse(120, 281, 408, 70);
    p.setBrush(QColor("#5e7563")); p.drawRoundedRect(250, 155, 277, 109, 15, 15);
    p.setBrush(QColor("#748b72")); p.drawRoundedRect(238, 209, 302, 66, 12, 12);
    p.setBrush(QColor("#435e4e")); p.drawRoundedRect(232, 181, 34, 88, 10, 10); p.drawRoundedRect(514, 181, 34, 88, 10, 10);
    p.setBrush(QColor("#c5a96a")); p.drawRoundedRect(290, 175, 54, 48, 7, 7);
    p.setBrush(QColor("#aab19a")); p.drawRoundedRect(431, 173, 55, 49, 7, 7);
    p.setBrush(QColor("#795b44")); p.drawRoundedRect(304, 282, 161, 18, 8, 8);
    p.fillRect(320, 299, 8, 33, QColor("#795b44")); p.fillRect(442, 299, 8, 33, QColor("#795b44"));
    p.setBrush(QColor("#f1e4bb")); p.drawEllipse(359, 282, 43, 9);
    p.setPen(QPen(QColor("#596447"), 6)); p.drawLine(582, 260, 579, 153);
    p.setPen(Qt::NoPen); p.setBrush(QColor("#64815b"));
    p.drawEllipse(547, 167, 33, 59); p.drawEllipse(580, 150, 32, 62); p.drawEllipse(565, 121, 24, 57);
    p.setBrush(QColor("#b37955")); p.drawRoundedRect(556, 250, 50, 49, 6, 6);
    p.setBrush(QColor(19, 39, 29, 175)); p.drawRoundedRect(14, 70, 212, 29, 6, 6);
    p.setPen(QColor("#ffffff")); QFont font = p.font(); font.setPixelSize(14); p.setFont(font);
    p.drawText(QRect(23, 71, 194, 26), Qt::AlignVCenter, QStringLiteral("合成画面 · 极简界面验收"));
    return image;
}

class MiniFixtureServer : public QObject {
public:
    QWebSocketServer server{QStringLiteral("Mini UI isolated fixture"), QWebSocketServer::NonSecureMode};
    QWebSocket *peer = nullptr;
    QJsonArray nodes{cameraNode()};
    QList<QJsonObject> requests, held;
    int connections = 0, snapshots = 0;
    bool holdSnapshots = false, scene = false, ptz = false;
    QColor color = Magenta;
    QByteArray encoded;

    MiniFixtureServer() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QWebSocketServer::newConnection, this, [this] {
            ++connections; peer = server.nextPendingConnection(); peer->setParent(this);
            connect(peer, &QWebSocket::textMessageReceived, this, [this](const QString &message) {
                const auto request = QJsonDocument::fromJson(message.toUtf8()).object(); requests.append(request);
                const QString command = request.value("command").toString();
                if (command == "start_listening" || command == "get_nodes") reply(request, nodes);
                else if (command == "camera_get_capabilities") reply(request, capabilities());
                else if (command == "camera_snapshot") {
                    ++snapshots;
                    if (holdSnapshots) held.append(request); else answerSnapshot(request);
                } else if (command == "read_attribute") answerAttributeRead(request);
                else if (command == "device_command" && ptz) reply(request,QJsonValue(QJsonValue::Null));
                else if (command == "camera_release_stream") reply(request, QJsonValue(QJsonValue::Null));
                else if (command == "commission_with_code") { nodes = {cameraNode()}; reply(request, cameraNode()); }
                else send({{"message_id", request.value("message_id")}, {"error_code", 8}, {"details", "This isolated fixture never starts live media"}});
            });
            send({{"schema_version", 14}, {"min_supported_schema_version", 11}, {"sdk_version", "Mini UI synthetic fixture"}});
        });
    }
    QUrl url() const { return QUrl(QString("ws://127.0.0.1:%1/ws").arg(server.serverPort())); }
    QJsonObject capabilities() const {
        return {
            {"features", QJsonArray{"Video", "Audio", "Speaker", "Snapshot"}}, {"privacy", QJsonObject{}},
            {"video", QJsonObject{{"codecs", QJsonArray{"H264"}}, {"rate_distortion_points", QJsonArray{}}}},
            {"audio", QJsonObject{{"codecs", QJsonArray{"OPUS"}}, {"two_way_talk_support", "FullDuplex"}}},
            {"snapshot", QJsonObject{{"capabilities", QJsonArray{QJsonObject{
                {"resolution", QJsonObject{{"width", 640}, {"height", 360}}}, {"max_frame_rate", 1},
                {"image_codec", "JPEG"}, {"requires_encoded_pixels", false}, {"requires_hardware_encoder", false}
            }}}}}, {"sessions", QJsonArray{}}
        };
    }
    void enableMetadata() {
        auto node = nodes.at(0).toObject(); auto attrs = node.value("attributes").toObject();
        auto clusters = attrs.value("1/29/1").toArray(); clusters.append(double(0xfff1fc02)); attrs.insert("1/29/1", clusters);
        attrs.insert("1/4294048770/0", metadataSnapshot(1, QStringLiteral("米家合成摄像头"), QStringLiteral("客厅")));
        attrs.insert("1/4294048770/1", QJsonArray{QJsonObject{{"0", "42"}, {"1", "客厅"}}, QJsonObject{{"0", "43"}, {"1", "书房"}}});
        attrs.insert("1/4294048770/2", QJsonObject{{"0", true}, {"1", false}, {"2", false}, {"3", false}});
        attrs.insert("1/4294048770/3", true); attrs.insert("1/4294048770/4", true);
        attrs.insert("1/4294048770/5", false); attrs.insert("1/4294048770/6", "");
        node.insert("attributes", attrs); nodes = {node};
    }
    QJsonObject metadataSnapshot(int revision, const QString &name, const QString &room) const {
        return {{"0", QString(64, 'a')}, {"1", "00000000-0000-4000-8000-000000000001"},
            {"2", revision}, {"3", name}, {"4", QString(64, 'b')}, {"5", "合成家庭"},
            {"6", room == QStringLiteral("书房") ? "43" : "42"}, {"7", room}, {"8", "2026-10-24T00:00:00.000Z"}};
    }
    void updateMetadata(const QString &name, const QString &room) {
        auto node = nodes.at(0).toObject(); auto attrs = node.value("attributes").toObject();
        const auto before = attrs.value("1/4294048770/0").toObject();
        const auto after = metadataSnapshot(before.value("2").toInt() + 1, name, room);
        attrs.insert("1/4294048770/0", after); node.insert("attributes", attrs); nodes = {node};
        send({{"event", "attribute_updated"}, {"data", QJsonArray{9001, "1/4294048770/0", after}}});
    }
    void invalidateMetadataProjection(const QString &error = QStringLiteral("metadata_incomplete")) {
        auto node = nodes.at(0).toObject(); auto attrs = node.value("attributes").toObject();
        auto snapshot = attrs.value("1/4294048770/0").toObject();
        snapshot.insert("2", snapshot.value("2").toInt() + 1);
        for (const auto &tag : {"3", "5", "6", "7", "8"}) snapshot.insert(tag, "");
        attrs.insert("1/4294048770/0", snapshot); attrs.insert("1/4294048770/1", QJsonArray{});
        attrs.insert("1/4294048770/4", false); attrs.insert("1/4294048770/6", error);
        node.insert("attributes", attrs); nodes = {node};
        for (const auto &tag : {"0", "1", "4", "6"}) {
            const QString path = QStringLiteral("1/4294048770/") + QString::fromLatin1(tag);
            send({{"event", "attribute_updated"}, {"data", QJsonArray{9001, path, attrs.value(path)}}});
        }
    }
    void answerAttributeRead(const QJsonObject &request) {
        const auto requested = request.value("args").toObject().value("attribute_path");
        const auto paths = requested.isArray() ? requested.toArray() : QJsonArray{requested};
        const auto attrs = nodes.at(0).toObject().value("attributes").toObject(); QJsonObject result;
        for (const auto &path : paths) if (attrs.contains(path.toString())) result.insert(path.toString(), attrs.value(path.toString()));
        // A previously uncached PTZ model is read through the same pinned API.
        if (ptz) for (const auto &path : paths) {
            const auto key = path.toString();
            if (key.startsWith("1/4294048769/")) {
                const QJsonObject ptzAttrs{{"1/4294048769/0",true},{"1/4294048769/1",true},{"1/4294048769/2",0},
                    {"1/4294048769/3",15},{"1/4294048769/65529",QJsonArray{0}}};
                if (ptzAttrs.contains(key)) result.insert(key, ptzAttrs.value(key));
            }
        }
        reply(request, result);
    }
    void answerSnapshot(const QJsonObject &request) {
        QImage image = scene ? syntheticRoom() : QImage(96, 54, QImage::Format_RGB32);
        if (!scene) image.fill(color);
        QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "JPEG", 90);
        encoded = bytes.toBase64();
        reply(request, QJsonObject{{"data", QString::fromLatin1(encoded)}, {"codec", "JPEG"},
            {"resolution", QJsonObject{{"width", image.width()}, {"height", image.height()}}},
            {"degraded", false}, {"stream_id", 77}, {"provenance", "adopted"}});
    }
    void send(const QJsonObject &message) {
        if (peer && peer->state() == QAbstractSocket::ConnectedState)
            peer->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }
    void reply(const QJsonObject &request, QJsonValue result) { send({{"message_id", request.value("message_id")}, {"result", result}}); }
    int count(const QString &command) const {
        int total = 0; for (const auto &request : requests) total += request.value("command").toString() == command; return total;
    }
};

bool showsColor(QWidget *widget, QColor color) {
    if (!widget || widget->size().isEmpty()) return false;
    const auto image = widget->grab().toImage().convertToFormat(QImage::Format_RGB32);
    int matching = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto *pixels = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            if (qAbs(qRed(pixels[x]) - color.red()) < 42 && qAbs(qGreen(pixels[x]) - color.green()) < 42
                && qAbs(qBlue(pixels[x]) - color.blue()) < 42) ++matching;
    }
    return matching > 128;
}
QJsonObject inspectCamera(CameraWindow *window) {
    auto *view = window->findChild<QWebEngineView *>("cameraVideo");
    if (!view) return {};
    auto result = std::make_shared<QJsonObject>(); QEventLoop loop; QPointer<QEventLoop> guarded(&loop);
    view->page()->runJavaScript("window.MatterDeskCamera ? JSON.stringify(window.MatterDeskCamera.inspect()) : ''",
        [result, guarded](const QVariant &value) { *result = QJsonDocument::fromJson(value.toString().toUtf8()).object(); if (guarded) guarded->quit(); });
    QTimer::singleShot(3000, &loop, &QEventLoop::quit); loop.exec(); return *result;
}
QJsonObject inspectVideoLayout(CameraWindow *window) {
    auto *view = window->findChild<QWebEngineView *>("cameraVideo");
    if (!view) return {};
    auto result = std::make_shared<QJsonObject>(); QEventLoop loop; QPointer<QEventLoop> guarded(&loop);
    view->page()->runJavaScript(QStringLiteral(
        "(() => { const video = document.getElementById('video'); if (!video) return '';"
        "const box = video.getBoundingClientRect(); return JSON.stringify({"
        "objectFit: getComputedStyle(video).objectFit, x: box.x, y: box.y,"
        "width: box.width, height: box.height, viewportWidth: innerWidth, viewportHeight: innerHeight}); })()"),
        [result, guarded](const QVariant &value) { *result = QJsonDocument::fromJson(value.toString().toUtf8()).object(); if (guarded) guarded->quit(); });
    QTimer::singleShot(3000, &loop, &QEventLoop::quit); loop.exec(); return *result;
}
QRect boxIn(QWidget *widget, QWidget *ancestor) {
    return QRect(widget->mapTo(ancestor, QPoint()), widget->size());
}
void sendPointer(QWidget *target, QEvent::Type type, const QPoint &global, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, QPointF(target->mapFromGlobal(global)), QPointF(global), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(target, &event);
}
void dragPicture(QWidget *target, const QPoint &delta) {
    const QPoint start = target->mapToGlobal(target->rect().center()), end = start + delta;
    sendPointer(target, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    sendPointer(target, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    sendPointer(target, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
}
int topLevelCameraWindows() {
    int count = 0;
    for (QWidget *widget : QApplication::topLevelWidgets())
        if (qobject_cast<CameraWindow *>(widget)) ++count;
    return count;
}
}

class MiniUiTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("MatterDeskTests"); QCoreApplication::setApplicationName("MiniUi");
        QVERIFY(m_settings.isValid()); QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, m_settings.path());
    }

    void init() { QSettings().remove(QStringLiteral("mini/alwaysOnTop")); }

    void automaticallyConnectsAndDisplaysOnlyCameraSnapshot() {
        MiniFixtureServer fixture; QVERIFY(fixture.server.isListening());
        QStringList logs;
        MiniWindow window(fixture.url());
        connect(window.client(), &MatterClient::logMessage, &window, [&](const QString &line) { logs.append(line); });
        QTRY_VERIFY(window.client()->isConnected()); QCOMPARE(fixture.connections, 1);
        window.show();
        auto *preview = window.findChild<QAbstractButton *>("cameraSnapshot"); QVERIFY(preview);
        QTRY_VERIFY(showsColor(preview, Magenta));
        QCOMPARE(fixture.snapshots, 1); QCOMPARE(fixture.count("camera_start_stream"), 0);
        QVERIFY(window.findChild<QPushButton *>("miniAdd"));
        QVERIFY(window.findChild<QWidget *>("sidebar") == nullptr);
        QVERIFY(window.findChild<QWidget *>("stat") == nullptr);
        QVERIFY(window.findChild<QWidget *>("serverUrl") == nullptr);
        QVERIFY(window.findChildren<QLineEdit *>().isEmpty());
        auto *selector = window.findChild<QComboBox *>("miniCameraSelector"); QVERIFY(selector); QVERIFY(selector->isHidden());
        auto *refresh = window.findChild<QPushButton *>("miniSnapshotRefresh"); QVERIFY(refresh);
        fixture.color = Cobalt; QTest::mouseClick(refresh, Qt::LeftButton);
        QTRY_VERIFY(showsColor(preview, Cobalt)); QCOMPARE(fixture.snapshots, 2);
        QVERIFY(!fixture.encoded.isEmpty()); QVERIFY(!logs.join('\n').contains(QString::fromLatin1(fixture.encoded.left(100))));
    }

    void metadataEntryShowsCloudLabelsAndDisablesUnverifiedWritesWithoutRestartingPreview() {
        MiniFixtureServer fixture; fixture.scene = true; fixture.enableMetadata();
        MiniWindow window(fixture.url()); window.show();
        QTRY_VERIFY(window.client()->isConnected());
        auto *preview = window.findChild<QAbstractButton *>("cameraSnapshot"); QVERIFY(preview);
        auto *title = window.findChild<QLabel *>("miniTitle"); QVERIFY(title);
        auto *location = window.findChild<QLabel *>("miniMijiaLocation"); QVERIFY(location);
        auto *entry = window.findChild<QPushButton *>("miniMijiaMetadata"); QVERIFY(entry);
        QTRY_VERIFY(entry->isVisible()); QTRY_VERIFY(entry->isEnabled());
        QTRY_COMPARE(title->text(), QStringLiteral("米家合成摄像头"));
        QTRY_COMPARE(location->text(), QStringLiteral("米家 · 合成家庭 / 客厅"));
        QTRY_COMPARE(fixture.snapshots, 1); const int snapshots = fixture.snapshots;
        QTest::mouseClick(entry, Qt::LeftButton);
        QTRY_VERIFY(window.findChild<QDialog *>("miniMetadataDialog"));
        auto *dialog = window.findChild<QDialog *>("miniMetadataDialog"); QVERIFY(dialog->isVisible());
        auto *name = dialog->findChild<QLabel *>("cloudMetadataName"); QVERIFY(name);
        auto *room = dialog->findChild<QLabel *>("cloudMetadataRoom"); QVERIFY(room);
        QTRY_COMPARE(name->text(), QStringLiteral("米家合成摄像头"));
        QTRY_COMPARE(room->text(), QStringLiteral("客厅"));
        auto *rename = dialog->findChild<QPushButton *>("cloudMetadataRename"); QVERIFY(rename); QVERIFY(!rename->isEnabled());
        auto *move = dialog->findChild<QPushButton *>("cloudMetadataMove"); QVERIFY(move); QVERIFY(!move->isEnabled());
        QCOMPARE(fixture.count("device_command"), 0);
        const QString capture = qEnvironmentVariable("MATTERDESK_MINI_METADATA_CAPTURE_DIR");
        if (!capture.isEmpty()) {
            QVERIFY(QDir().mkpath(capture)); QTest::qWait(80);
            QVERIFY(window.grab().save(QDir(capture).filePath("metadata-preview.png")));
            QVERIFY(dialog->grab().save(QDir(capture).filePath("metadata-details.png")));
        }
        fixture.updateMetadata(QStringLiteral("米家更新摄像头"), QStringLiteral("书房"));
        QTRY_COMPARE(title->text(), QStringLiteral("米家更新摄像头"));
        QTRY_COMPARE(location->text(), QStringLiteral("米家 · 合成家庭 / 书房"));
        QTRY_COMPARE(room->text(), QStringLiteral("书房"));
        QCOMPARE(fixture.snapshots, snapshots); QCOMPARE(fixture.connections, 1);
        QCOMPARE(fixture.count("camera_start_stream"), 0); QCOMPARE(fixture.count("device_command"), 0);
        QTest::mouseClick(entry, Qt::LeftButton); QCOMPARE(window.findChildren<QDialog *>("miniMetadataDialog").size(), 1);
        fixture.invalidateMetadataProjection();
        QTRY_COMPARE(title->text(), QStringLiteral("摄像头"));
        QTRY_COMPARE(location->text(), QStringLiteral("米家信息暂不可用"));
        QVERIFY(!title->text().contains(QStringLiteral("米家更新摄像头")));
        QCOMPARE(fixture.snapshots, snapshots); QCOMPARE(fixture.count("camera_start_stream"), 0);
        QCOMPARE(fixture.count("device_command"), 0);
        dialog->close();
    }

    void metadataUpdatesKeepTheSameEmbeddedPlayerAndNeverRestartTheStream() {
        MiniFixtureServer fixture; fixture.enableMetadata(); MiniWindow window(fixture.url()); window.show();
        QTRY_VERIFY(window.client()->isConnected());
        auto *title = window.findChild<QLabel *>("miniTitle"); QVERIFY(title);
        QTRY_COMPARE(title->text(), QStringLiteral("米家合成摄像头"));
        QTest::mouseClick(window.findChild<QPushButton *>("miniPlay"), Qt::LeftButton);
        QTRY_COMPARE(window.findChildren<CameraWindow *>().size(), 1);
        QPointer<CameraWindow> live = window.findChild<CameraWindow *>(); QVERIFY(live);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.count("camera_start_stream") == 1, 15000);
        auto *view = live->findChild<QWebEngineView *>("cameraVideo"); QVERIFY(view);
        auto *page = view->page(); QVERIFY(page);
        QJsonObject before; QTRY_VERIFY_WITH_TIMEOUT(!(before = inspectCamera(live)).isEmpty(), 15000);
        QTRY_COMPARE((before = inspectCamera(live)).value("peerState").toString(), QStringLiteral("closed"));
        const int snapshots = fixture.snapshots; const int stops = fixture.count("camera_stop_stream");
        fixture.updateMetadata(QStringLiteral("直播期间米家新名称"), QStringLiteral("书房"));
        QTRY_COMPARE(title->text(), QStringLiteral("直播期间米家新名称"));
        QCOMPARE(window.findChild<CameraWindow *>(), live.data()); QVERIFY(live->isVisible());
        QCOMPARE(live->findChild<QWebEngineView *>("cameraVideo"), view); QCOMPARE(view->page(), page);
        QCOMPARE(fixture.count("camera_start_stream"), 1); QCOMPARE(fixture.count("camera_stop_stream"), stops);
        QCOMPARE(fixture.snapshots, snapshots); QCOMPARE(fixture.connections, 1); QCOMPARE(fixture.count("device_command"), 0);
        QVERIFY(!inspectCamera(live).value("micActive").toBool());
        fixture.invalidateMetadataProjection();
        QTRY_COMPARE(title->text(), QStringLiteral("摄像头"));
        QTRY_COMPARE(window.findChild<QLabel *>("miniMijiaLocation")->text(), QStringLiteral("米家信息暂不可用"));
        QCOMPARE(window.findChild<CameraWindow *>(), live.data()); QCOMPARE(view->page(), page);
        QCOMPARE(fixture.count("camera_start_stream"), 1); QCOMPARE(fixture.count("camera_stop_stream"), stops);
        QCOMPARE(fixture.count("device_command"), 0);
    }

    void firstUnavailableSourceAllowsOnlyAnExplicitGuardedRefresh() {
        MiniFixtureServer fixture; fixture.enableMetadata(); fixture.invalidateMetadataProjection(QString());
        MiniWindow window(fixture.url()); window.show(); QTRY_VERIFY(window.client()->isConnected());
        auto *title = window.findChild<QLabel *>("miniTitle"); QVERIFY(title);
        QTRY_COMPARE(title->text(), QStringLiteral("摄像头"));
        QTRY_COMPARE(window.findChild<QLabel *>("miniMijiaLocation")->text(), QStringLiteral("米家信息暂不可用"));
        auto *entry = window.findChild<QPushButton *>("miniMijiaMetadata"); QVERIFY(entry); QTRY_VERIFY(entry->isEnabled());
        entry->click(); QTRY_VERIFY(window.findChild<QDialog *>("miniMetadataDialog"));
        auto *dialog = window.findChild<QDialog *>("miniMetadataDialog");
        auto *refresh = dialog->findChild<QPushButton *>("cloudMetadataRefresh"); QVERIFY(refresh);
        QTRY_VERIFY(refresh->isEnabled());
        auto *rename = dialog->findChild<QPushButton *>("cloudMetadataRename"); QVERIFY(rename); QVERIFY(!rename->isEnabled());
        auto *move = dialog->findChild<QPushButton *>("cloudMetadataMove"); QVERIFY(move); QVERIFY(!move->isEnabled());
        auto *sync = dialog->findChild<QCheckBox *>("cloudMetadataSync"); QVERIFY(sync); QVERIFY(!sync->isEnabled());
        QCOMPARE(fixture.count("device_command"), 0);
        refresh->click(); QTRY_COMPARE(fixture.count("device_command"), 1);
        QJsonObject args;
        for (const auto &request : fixture.requests) if (request.value("command").toString() == "device_command") args = request.value("args").toObject();
        QCOMPARE(args.value("command_name").toString(), QStringLiteral("RefreshMetadata"));
        QCOMPARE(args.value("cluster_id").toDouble(), double(0xfff1fc02));
        const auto payload = args.value("payload").toObject(); QCOMPARE(payload.size(), 3);
        QCOMPARE(payload.value("expectedIdentity").toString(), QString(64, 'a'));
        QCOMPARE(payload.value("expectedSourceEpoch").toString(), QStringLiteral("00000000-0000-4000-8000-000000000001"));
        QCOMPARE(payload.value("expectedRevision").toInt(), 2);
        QTest::qWait(100); QCOMPARE(fixture.count("device_command"), 1);
        QCOMPARE(fixture.count("camera_start_stream"), 0); QCOMPARE(fixture.connections, 1);
        dialog->close();
    }

    void emptyViewPairsCameraWithMaskedCodeAndAutomaticallyShowsSnapshot() {
        MiniFixtureServer fixture; fixture.nodes = {}; QVERIFY(fixture.server.isListening());
        QStringList logs;
        MiniWindow window(fixture.url()); window.show();
        connect(window.client(), &MatterClient::logMessage, &window, [&](const QString &line) { logs.append(line); });
        QTRY_VERIFY(window.client()->isConnected()); QCOMPARE(fixture.snapshots, 0);
        auto *add = window.findChild<QPushButton *>("miniAdd"); QVERIFY(add); QTRY_VERIFY(add->isEnabled());
        bool sawDialog = false, masked = false, submitted = false, cleared = false;
        QTimer populate; populate.setInterval(10);
        connect(&populate, &QTimer::timeout, &window, [&] {
            auto *dialog = window.findChild<QDialog *>("miniPairDialog");
            if (!dialog || !dialog->isVisible()) return;
            populate.stop(); sawDialog = true;
            auto *code = dialog->findChild<QLineEdit *>("miniPairCode");
            auto *submit = dialog->findChild<QPushButton *>("miniPairSubmit");
            masked = code && code->echoMode() == QLineEdit::Password;
            if (code && submit) { code->setText(TestCode); submit->click(); submitted = true; cleared = code->text().isEmpty(); }
            QTimer::singleShot(200, dialog, &QDialog::close);
        });
        populate.start();
        // Escape the nested modal loop even if a future dialog regression changes its fields.
        QTimer::singleShot(2500, &window, [&window] { if (auto *dialog = window.findChild<QDialog *>("miniPairDialog")) dialog->close(); });
        QTest::mouseClick(add, Qt::LeftButton);
        QTRY_VERIFY(sawDialog); QVERIFY(masked); QVERIFY(submitted); QVERIFY(cleared);
        QTRY_COMPARE(fixture.count("commission_with_code"), 1);
        QJsonObject args;
        for (const auto &request : fixture.requests) if (request.value("command").toString() == "commission_with_code") args = request.value("args").toObject();
        QVERIFY(args.value("code").toString() == TestCode); QVERIFY(args.value("network_only").toBool());
        auto *preview = window.findChild<QAbstractButton *>("cameraSnapshot"); QVERIFY(preview);
        QTRY_VERIFY(showsColor(preview, Magenta));
        QVERIFY(!logs.join('\n').contains(TestCode)); QCOMPARE(fixture.count("camera_start_stream"), 0);
    }

    void previewStartsLiveInSameWindowOnceAndReturnRestoresSnapshots() {
        MiniFixtureServer fixture; MiniWindow window(fixture.url()); window.show();
        QTRY_VERIFY(window.client()->isConnected());
        auto *preview = window.findChild<QAbstractButton *>("cameraSnapshot"); QVERIFY(preview);
        QWidget *const originalRoot = window.centralWidget(); QVERIFY(originalRoot);
        QWidget *const originalTopLevel = window.window();
        QTRY_VERIFY(showsColor(preview, Magenta));
        QCOMPARE(topLevelCameraWindows(), 0);
        QTest::mouseClick(window.findChild<QPushButton *>("miniPlay"), Qt::LeftButton);
        QTRY_COMPARE(window.findChildren<CameraWindow *>().size(), 1);
        QPointer<CameraWindow> live = window.findChild<CameraWindow *>(); QVERIFY(live);
        QTRY_VERIFY(live->isVisible());
        QVERIFY(!live->isWindow()); QCOMPARE(live->window(), originalTopLevel);
        QCOMPARE(topLevelCameraWindows(), 0);
        QCOMPARE(window.centralWidget(), originalRoot); QVERIFY(window.isVisible());
        auto *video = live->findChild<QWebEngineView *>("cameraVideo"); QVERIFY(video);
        auto *overlay = live->findChild<QWidget *>("cameraOverlay"); QVERIFY(overlay);
        auto *pages = window.findChild<QStackedWidget *>("miniPages"); QVERIFY(pages);
        QCOMPARE(pages->currentWidget(), live.data());
        QPushButton *back = nullptr;
        for (auto *button : live->findChildren<QPushButton *>())
            if (button->property("cameraAction").toString() == "stop") back = button;
        QVERIFY(back); QTRY_VERIFY(back->isVisible()); QVERIFY(back->isEnabled());
        // Clicking the panel play button authorizes playback. The fixture refuses the
        // start and supplies no remote SDP, image stream, sound or microphone.
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_start_stream"), 1, 15000);
        QJsonObject state; QTRY_VERIFY_WITH_TIMEOUT(!(state = inspectCamera(live)).isEmpty(), 15000);
        QTRY_VERIFY((state = inspectCamera(live)).value("peerState").toString() == "closed");
        QVERIFY(!state.value("micActive").toBool()); QCOMPARE(state.value("receivedVideoFrames").toInt(), 0);
        for (auto *button : live->findChildren<QPushButton *>())
            if (button->property("cameraAction").toString() == "sound" || button->property("cameraAction").toString() == "talk") QVERIFY(!button->isChecked());
        QJsonObject videoLayout;
        QTRY_COMPARE((videoLayout = inspectVideoLayout(live)).value("objectFit").toString(), QStringLiteral("cover"));
        for (const QSize size : {QSize(720, 600), QSize(500, 420), QSize(1200, 700), QSize(600, 900), QSize(400, 280)}) {
            window.resize(size); QTest::qWait(50);
            QCOMPARE(boxIn(live, originalRoot), originalRoot->rect());
            QCOMPARE(boxIn(video, live), live->rect());
            QVERIFY(overlay->isVisible()); QVERIFY(live->rect().contains(boxIn(overlay, live)));
            QVERIFY(back->isVisible()); QVERIFY(live->rect().contains(boxIn(back, live)));
            videoLayout = inspectVideoLayout(live);
            QCOMPARE(videoLayout.value("x").toDouble(), 0.0); QCOMPARE(videoLayout.value("y").toDouble(), 0.0);
            QCOMPARE(videoLayout.value("width").toDouble(), videoLayout.value("viewportWidth").toDouble());
            QCOMPARE(videoLayout.value("height").toDouble(), videoLayout.value("viewportHeight").toDouble());
            QCOMPARE(videoLayout.value("objectFit").toString(), QStringLiteral("cover"));
        }
        QSignalSpy deviceChanges(window.client(), &MatterClient::devicesChanged);
        fixture.send({{"event", "attribute_updated"}, {"data", QJsonArray{9001, "1/57/5", "更新的合成测试摄像头"}}});
        QTRY_VERIFY(!deviceChanges.isEmpty());
        QCOMPARE(pages->currentWidget(), live.data()); QVERIFY(live->isVisible());
        QCOMPARE(window.findChild<CameraWindow *>(), live.data());
        QCOMPARE(fixture.count("camera_start_stream"), 1);
        window.findChild<QPushButton *>("miniPlay")->click(); QTest::qWait(50); QCOMPARE(window.findChildren<CameraWindow *>().size(), 1);
        QCOMPARE(window.findChild<CameraWindow *>(), live.data());
        QCOMPARE(fixture.count("camera_start_stream"), 1);
        fixture.color = Cobalt; QTest::mouseClick(back, Qt::LeftButton); QTRY_VERIFY_WITH_TIMEOUT(live.isNull(), 4000);
        QVERIFY(window.isVisible()); QCOMPARE(window.window(), originalTopLevel); QCOMPARE(window.centralWidget(), originalRoot);
        QCOMPARE(topLevelCameraWindows(), 0); QTRY_VERIFY(preview->isVisible());
        QCOMPARE(boxIn(preview, originalRoot), originalRoot->rect());
        auto *refresh = window.findChild<QPushButton *>("miniSnapshotRefresh"); QVERIFY(refresh);
        QTRY_VERIFY(refresh->isEnabled()); QTest::qWait(80); refresh->click();
        QTRY_VERIFY(showsColor(preview, Cobalt)); QCOMPARE(fixture.count("camera_start_stream"), 1);
    }

    void snapshotPtzReadsUnknownCapabilitiesMovesOnceAndRefreshesPreview() {
        MiniFixtureServer fixture; fixture.ptz=true; fixture.nodes={ptzCameraNode(false)};
        MiniWindow window(fixture.url()); window.resize(400,280); window.show();
        QTRY_VERIFY(window.client()->isConnected());
        auto *ptz=window.findChild<CameraPtzControls *>(); QVERIFY(ptz);
        auto *up=ptz->findChild<QPushButton *>("ptzUp"); QVERIFY(up);
        QTRY_VERIFY(up->isEnabled()); QVERIFY(ptz->isVisible());
        QTRY_COMPARE(fixture.count("read_attribute"),1);
        QCOMPARE(fixture.count("device_command"),0); QCOMPARE(fixture.count("camera_start_stream"),0);
        const auto area=window.centralWidget()->rect();
        for(const QSize size:{QSize(400,280),QSize(800,450),QSize(450,800)}) {
            window.resize(size); QTest::qWait(30);
            QVERIFY(window.centralWidget()->rect().contains(boxIn(ptz,window.centralWidget())));
            for(auto *button:ptz->findChildren<QPushButton *>())
                QVERIFY(window.centralWidget()->rect().contains(boxIn(button,window.centralWidget())));
        }
        Q_UNUSED(area);
        auto *preview=window.findChild<QAbstractButton *>("cameraSnapshot"); QTRY_VERIFY(showsColor(preview,Magenta));
        const int before=fixture.snapshots; fixture.color=Cobalt;
        QTest::mouseClick(up,Qt::LeftButton); QTRY_COMPARE(fixture.count("device_command"),1);
        const auto args=fixture.requests.last().value("args").toObject();
        QCOMPARE(args.value("cluster_id").toInteger(),4294048769LL);
        QCOMPARE(args.value("payload").toObject(),QJsonObject({{"direction",0}}));
        QTest::mouseClick(up,Qt::LeftButton); QTest::qWait(80); QCOMPARE(fixture.count("device_command"),1);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.snapshots>before,3000); QTRY_VERIFY(showsColor(preview,Cobalt));
        window.togglePanels(); QVERIFY(!ptz->isVisible()); up->click(); QTest::qWait(30);
        QCOMPARE(fixture.count("device_command"),1);
        window.togglePanels(); QTRY_VERIFY(ptz->isVisible());
        fixture.send({{"event","attribute_updated"},{"data",QJsonArray{9001,"1/4294048769/1",false}}});
        QTRY_VERIFY(!up->isEnabled());
        fixture.send({{"event","attribute_updated"},{"data",QJsonArray{9001,"1/57/17",false}}});
        QTRY_VERIFY(!up->isEnabled()); QCOMPARE(fixture.count("device_command"),1);
    }

    void livePtzSharesTheOverlayAndDoesNotRestartMedia() {
        MiniFixtureServer fixture; fixture.ptz=true; fixture.nodes={ptzCameraNode()};
        MiniWindow window(fixture.url()); window.resize(400,280); window.show(); QTRY_VERIFY(window.client()->isConnected());
        auto *previewPtz=window.findChild<CameraPtzControls *>(); QVERIFY(previewPtz);
        QTRY_VERIFY(previewPtz->findChild<QPushButton *>("ptzUp")->isEnabled());
        window.findChild<QPushButton *>("miniPlay")->click();
        QTRY_VERIFY(window.findChild<CameraWindow *>()); QPointer<CameraWindow> live=window.findChild<CameraWindow *>();
        auto *ptz=live->findChild<CameraPtzControls *>(); QVERIFY(ptz);
        auto *left=ptz->findChild<QPushButton *>("ptzLeft"); QVERIFY(left);
        QTRY_VERIFY(left->isEnabled());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_start_stream"),1,15000);
        QVERIFY(!previewPtz->isVisible()); QVERIFY(ptz->isVisible());
        for(const QSize size:{QSize(400,280),QSize(800,450),QSize(450,800)}) {
            window.resize(size); QTest::qWait(30);
            auto *overlay=live->findChild<QWidget *>("cameraOverlay");
            QVERIFY(live->rect().contains(boxIn(overlay,live)));
            for(auto *button:ptz->findChildren<QPushButton *>()) {
                QVERIFY(overlay->rect().contains(boxIn(button,overlay)));
                const auto center=button->mapTo(live,button->rect().center());
                QCOMPARE(live->childAt(center),button);
            }
        }
        QTest::mouseClick(left,Qt::LeftButton); QTRY_COMPARE(fixture.count("device_command"),1);
        const auto args=fixture.requests.last().value("args").toObject();
        QCOMPARE(args.value("payload").toObject(),QJsonObject({{"direction",2}}));
        QCOMPARE(window.findChild<CameraWindow *>(),live.data()); QCOMPARE(fixture.count("camera_start_stream"),1);
        window.togglePanels(); QVERIFY(!ptz->isVisible()); left->click(); QTest::qWait(40);
        QCOMPARE(fixture.count("device_command"),1); window.togglePanels(); QVERIFY(ptz->isVisible());
        live->close(); QTRY_VERIFY_WITH_TIMEOUT(live.isNull(),4000);
        QVERIFY(previewPtz->isVisible()); QCOMPARE(fixture.count("device_command"),1);
    }

    void framelessPictureClickTogglesAllPanelsWithoutPlayingEvenAfterDeviceUpdates() {
        MiniFixtureServer fixture; MiniWindow window(fixture.url()); window.show();
        QVERIFY(window.windowFlags().testFlag(Qt::FramelessWindowHint));
        auto *preview = window.findChild<QAbstractButton *>("cameraSnapshot");
        auto *header = window.findChild<QWidget *>("miniHeader");
        auto *footer = window.findChild<QWidget *>("miniFooter");
        QTRY_VERIFY(showsColor(preview, Magenta));
        QTest::mouseClick(preview, Qt::LeftButton);
        QVERIFY(header->isVisible()); // Wait for the double-click interval before toggling.
        QTRY_VERIFY(!header->isVisible()); QVERIFY(!footer->isVisible());
        fixture.send({{"event", "attribute_updated"}, {"data", QJsonArray{9001, "1/57/5", "更新名称"}}});
        QTRY_COMPARE(window.findChild<QLabel *>("miniTitle")->text(), QStringLiteral("更新名称"));
        QVERIFY(!header->isVisible()); QVERIFY(!footer->isVisible());
        QCOMPARE(fixture.count("camera_start_stream"), 0);
        QTest::mouseClick(preview, Qt::LeftButton);
        QTRY_VERIFY(header->isVisible()); QVERIFY(footer->isVisible());
        auto *refresh = window.findChild<QPushButton *>("miniSnapshotRefresh");
        QTest::mouseClick(refresh, Qt::LeftButton); QTest::qWait(QApplication::doubleClickInterval() + 40);
        QVERIFY(header->isVisible()); QVERIFY(footer->isVisible());
        fixture.server.close(); fixture.peer->close();
        QTRY_VERIFY(!window.client()->isConnected());
        QTest::mouseClick(preview, Qt::LeftButton); QTRY_VERIFY(!header->isVisible());
        QTest::mouseClick(preview, Qt::LeftButton); QTRY_VERIFY(header->isVisible());
        QCOMPARE(fixture.count("camera_start_stream"), 0);
    }

    void previewControlsHideAfterTenSecondsDespiteBackgroundUpdates() {
        MiniFixtureServer fixture; fixture.enableMetadata(); MiniWindow window(fixture.url()); window.show();
        auto *preview=window.findChild<QAbstractButton *>("cameraSnapshot");
        auto *header=window.findChild<QWidget *>("miniHeader");
        auto *footer=window.findChild<QWidget *>("miniFooter");
        QTRY_VERIFY(showsColor(preview,Magenta));
        int updatesSent=0; QTimer updates; updates.setInterval(400);
        connect(&updates,&QTimer::timeout,&window,[&] {
            fixture.updateMetadata(QStringLiteral("后台更新 %1").arg(++updatesSent), QStringLiteral("客厅"));
        }); updates.start();
        QElapsedTimer elapsed; elapsed.start();
        QTest::qWait(9200); QVERIFY(header->isVisible()); QVERIFY(footer->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!header->isVisible(),1800);
        QVERIFY(!footer->isVisible()); QVERIFY(elapsed.elapsed()>=9700); QVERIFY(updatesSent>=20);
        QTRY_COMPARE(window.findChild<QLabel *>("miniTitle")->text(), QStringLiteral("后台更新 %1").arg(updatesSent));
        QTest::qWait(500); QVERIFY(!header->isVisible()); QVERIFY(!footer->isVisible());
        QCOMPARE(fixture.count("camera_start_stream"),0);
    }

    void liveAutoHideResetsAfterInputAndClickCanRevealAgain() {
        MiniFixtureServer fixture; MiniWindow window(fixture.url()); window.show();
        QTRY_VERIFY(window.client()->isConnected());
        auto *play=window.findChild<QPushButton *>("miniPlay"); QTRY_VERIFY(play->isEnabled());
        QTest::mouseClick(play,Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_start_stream"),1,15000);
        QPointer<CameraWindow> live=window.findChild<CameraWindow *>(); QVERIFY(live);
        auto *view=live->findChild<QWebEngineView *>("cameraVideo"); QVERIFY(view); QTRY_VERIFY(view->focusProxy());
        auto *header=window.findChild<QWidget *>("miniHeader");
        auto *overlay=live->findChild<QWidget *>("cameraOverlay"); QVERIFY(overlay);
        window.activateWindow(); QTest::qWait(30); QTest::keyClick(&window,Qt::Key_Right);
        QTest::qWait(8000); QVERIFY(header->isVisible()); QVERIFY(overlay->isVisible());
        QTest::keyClick(&window,Qt::Key_Right);
        QTest::qWait(2500); QVERIFY(header->isVisible()); QVERIFY(overlay->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!header->isVisible(),8500); QVERIFY(!overlay->isVisible());
        QCOMPARE(window.findChild<CameraWindow *>(),live.data()); QCOMPARE(fixture.count("camera_start_stream"),1);
        QTest::mouseClick(view->focusProxy(),Qt::LeftButton);
        QTRY_VERIFY(header->isVisible()); QVERIFY(overlay->isVisible());
        QTest::qWait(1200); QVERIFY(header->isVisible()); QVERIFY(overlay->isVisible());
        QTest::mouseClick(view->focusProxy(),Qt::LeftButton);
        QTRY_VERIFY(!header->isVisible()); QVERIFY(!overlay->isVisible());
        live->close(); QTRY_VERIFY_WITH_TIMEOUT(live.isNull(),4000);
        QVERIFY(!header->isVisible()); QVERIFY(!window.findChild<QWidget *>("miniFooter")->isVisible());
        QCOMPARE(fixture.count("camera_start_stream"),1);
    }

    void doubleClickRestoresDefaultSizeThenFullScreenAndNeverTogglesControls() {
        MiniFixtureServer fixture; MiniWindow window(fixture.url()); window.resize(480, 340); window.move(90, 120); window.show();
        auto *preview = window.findChild<QAbstractButton *>("cameraSnapshot");
        auto *header = window.findChild<QWidget *>("miniHeader");
        auto *full = window.findChild<QPushButton *>("miniFullScreen"); QVERIFY(full);
        QTRY_VERIFY(showsColor(preview, Magenta));
        QTest::mouseClick(preview, Qt::LeftButton);
        QTest::mouseDClick(preview, Qt::LeftButton); QTest::mouseRelease(preview, Qt::LeftButton);
        QVERIFY(!window.isFullScreen()); QCOMPARE(window.size(), QSize(800,450));
        const QRect defaultGeometry = window.geometry();
        QTest::qWait(QApplication::doubleClickInterval()+40); QVERIFY(header->isVisible());
        QTest::mouseDClick(preview, Qt::LeftButton); QTest::mouseRelease(preview, Qt::LeftButton);
        QTRY_VERIFY(window.isFullScreen()); QVERIFY(full->isChecked());
        QTest::mouseDClick(preview, Qt::LeftButton); QTest::mouseRelease(preview, Qt::LeftButton);
        QTRY_VERIFY(!window.isFullScreen()); QCOMPARE(window.size(), QSize(800,450));
        QCOMPARE(window.geometry(), defaultGeometry);
        // Full-screen entry from a custom size still double-clicks back to default.
        window.resize(520,360); QTest::mouseClick(full, Qt::LeftButton); QTRY_VERIFY(window.isFullScreen());
        QTest::mouseDClick(preview, Qt::LeftButton); QTest::mouseRelease(preview, Qt::LeftButton);
        QTRY_VERIFY(!window.isFullScreen()); QCOMPARE(window.size(),QSize(800,450));
        const QRect normal = window.geometry();
        QTest::mouseClick(full, Qt::LeftButton); QTRY_VERIFY(window.isFullScreen());
        window.activateWindow(); QTest::qWait(30); QTest::keyClick(&window, Qt::Key_Escape);
        QTRY_VERIFY(!window.isFullScreen()); QCOMPARE(window.geometry(), normal);
        QTest::keyClick(&window, Qt::Key_F11); QTRY_VERIFY(window.isFullScreen());
        QTest::keyClick(&window, Qt::Key_F11); QTRY_VERIFY(!window.isFullScreen());
        QCOMPARE(window.geometry(), normal); QVERIFY(header->isVisible());
        QCOMPARE(fixture.count("camera_start_stream"),0);
        QTest::mouseClick(window.findChild<QPushButton *>("miniMinimize"), Qt::LeftButton);
        QTRY_VERIFY(window.isMinimized()); window.showNormal();
        QTest::mouseClick(window.findChild<QPushButton *>("miniClose"), Qt::LeftButton);
        QTRY_VERIFY(!window.isVisible());
    }

    void topControlsHaveNoPanelBackgroundAndPinPreservesGeometryAndPreference() {
        MiniFixtureServer fixture;
        {
            MiniWindow window(fixture.url()); window.resize(480,340); window.move(80,100); window.show();
            auto *preview = window.findChild<QAbstractButton *>("cameraSnapshot");
            QTRY_VERIFY(showsColor(preview,Magenta));
            auto *header = window.findChild<QWidget *>("miniHeader");
            const QPoint sample(header->geometry().center().x(),header->geometry().bottom()-2);
            const QColor composited = window.centralWidget()->grab().toImage().pixelColor(sample);
            const QColor imageOnly = preview->grab().toImage().pixelColor(sample);
            QVERIFY(qAbs(composited.red()-imageOnly.red())<10);
            QVERIFY(qAbs(composited.green()-imageOnly.green())<10);
            QVERIFY(qAbs(composited.blue()-imageOnly.blue())<10);
            auto *pin = window.findChild<QPushButton *>("miniAlwaysOnTop"); QVERIFY(pin);
            QVERIFY(!pin->isChecked()); QVERIFY(!window.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
            const QRect original = window.geometry();
            QTest::mouseClick(pin,Qt::LeftButton);
            QVERIFY(window.windowFlags().testFlag(Qt::WindowStaysOnTopHint)); QVERIFY(pin->isChecked());
            QVERIFY(window.isVisible()); QCOMPARE(window.geometry(),original);
            QVERIFY(QSettings().value("mini/alwaysOnTop").toBool());
            window.toggleFullScreen(); QTRY_VERIFY(window.isFullScreen());
            QTest::mouseClick(pin,Qt::LeftButton);
            QVERIFY(!window.windowFlags().testFlag(Qt::WindowStaysOnTopHint)); QVERIFY(window.isFullScreen());
            QTest::mouseClick(pin,Qt::LeftButton);
            QVERIFY(window.windowFlags().testFlag(Qt::WindowStaysOnTopHint)); QVERIFY(window.isFullScreen());
            window.toggleFullScreen(); QTRY_VERIFY(!window.isFullScreen()); QCOMPARE(window.geometry(),original);
        }
        MiniWindow restored(fixture.url()); restored.show();
        auto *pin = restored.findChild<QPushButton *>("miniAlwaysOnTop"); QVERIFY(pin);
        QVERIFY(pin->isChecked()); QVERIFY(restored.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        QTest::mouseClick(pin,Qt::LeftButton);
        QVERIFY(!pin->isChecked()); QVERIFY(!restored.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        QVERIFY(!QSettings().value("mini/alwaysOnTop").toBool());
    }

    void dragPictureMovesWindowWithoutClickingOrPlaying() {
        MiniFixtureServer fixture; MiniWindow window(fixture.url()); window.resize(400, 280); window.move(140, 160); window.show();
        auto *preview = window.findChild<QAbstractButton *>("cameraSnapshot");
        QTRY_VERIFY(showsColor(preview, Magenta));
        const QPoint original = window.pos(), delta(47, 35);
        dragPicture(preview, delta);
        QCOMPARE(window.pos(), original + delta);
        QTest::qWait(QApplication::doubleClickInterval() + 40);
        QVERIFY(window.findChild<QWidget *>("miniHeader")->isVisible());
        QCOMPARE(fixture.count("camera_start_stream"), 0);
        QCOMPARE(window.property("drawerEdge").toString(), QStringLiteral("none"));
    }

    void edgeReleaseImmediatelyHidesToFullLengthThinPinnedTab_data() {
        QTest::addColumn<QString>("edge"); QTest::addColumn<bool>("pinned");
        for (const auto &edge : {"left","right","top","bottom"}) {
            QTest::newRow(qPrintable(QString(edge)+"-normal")) << QString::fromLatin1(edge) << false;
            QTest::newRow(qPrintable(QString(edge)+"-pinned")) << QString::fromLatin1(edge) << true;
        }
    }
    void edgeReleaseImmediatelyHidesToFullLengthThinPinnedTab() {
        QFETCH(QString,edge); QFETCH(bool,pinned);
        MiniFixtureServer fixture; MiniWindow window(fixture.url()); window.resize(400,280); window.show();
        auto *preview=window.findChild<QAbstractButton *>("cameraSnapshot");
        QTRY_VERIFY(showsColor(preview,Magenta));
        if(pinned) window.findChild<QPushButton *>("miniAlwaysOnTop")->click();
        auto *screen=window.screen(); QVERIFY(screen); const QRect area=screen->availableGeometry();
        QVERIFY(area.width()>window.width()); QVERIFY(area.height()>window.height());
        const QPoint center=area.center()-QPoint(window.width()/2,window.height()/2);
        window.move(center); QPoint target=center;
        if(edge=="left") target.setX(area.left()+5);
        if(edge=="right") target.setX(area.right()-window.width()-4);
        if(edge=="top") target.setY(area.top()+5);
        if(edge=="bottom") target.setY(area.bottom()-window.height()-4);
        QCursor::setPos(target+QPoint(window.width()/2,window.height()/2));
        dragPicture(preview,target-center);
        QCOMPARE(window.property("drawerEdge").toString(),edge);
        QVERIFY(window.property("drawerHidden").toBool()); // No wait for pointer departure.
        const QRect docked=window.geometry(); QVERIFY(docked.contains(QCursor::pos()));
        auto *handle=window.drawerHandle(); QVERIFY(handle);
        QTRY_VERIFY_WITH_TIMEOUT(handle->isVisible(),1500);
        QVERIFY(handle->isWindow()); QVERIFY(handle->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        QVERIFY(!window.isVisible());
        QVERIFY(!window.findChild<QWidget *>("miniHeader")->isVisible());
        const bool vertical=edge=="left" || edge=="right";
        QCOMPARE(handle->size(),vertical ? QSize(2,docked.height()-16) : QSize(docked.width()-16,2));
        QVERIFY(area.contains(handle->geometry()));
        // The entire original length responds, with a narrowed input thickness.
        const QPoint nearEnd=vertical ? QPoint(handle->geometry().center().x(),docked.top()+10)
                                      : QPoint(docked.left()+10,handle->geometry().center().y());
        QVERIFY(handle->geometry().contains(nearEnd));
        QPoint outside=nearEnd;
        if(edge=="left") outside.rx()+=2;
        if(edge=="right") outside.rx()-=2;
        if(edge=="top") outside.ry()+=2;
        if(edge=="bottom") outside.ry()-=2;
        QVERIFY(!handle->geometry().contains(outside)); QCursor::setPos(outside); QTest::qWait(180);
        QVERIFY(window.property("drawerHidden").toBool()); QVERIFY(!window.isVisible());
        QCursor::setPos(nearEnd);
        QTRY_VERIFY_WITH_TIMEOUT(!window.property("drawerHidden").toBool(),1500);
        QTRY_COMPARE(window.geometry(),docked); QVERIFY(window.isVisible()); QVERIFY(!handle->isVisible());
        QCOMPARE(window.windowFlags().testFlag(Qt::WindowStaysOnTopHint),pinned);
        QVERIFY(window.findChild<QWidget *>("miniHeader")->isVisible());
        if(edge=="left") {
            window.toggleFullScreen(); QTRY_VERIFY(window.isFullScreen());
            QCOMPARE(window.property("drawerEdge").toString(),QStringLiteral("none"));
            QTest::qWait(200); QVERIFY(!window.property("drawerHidden").toBool());
            window.toggleFullScreen(); QTRY_COMPARE(window.geometry(),docked);
        }
        QCursor::setPos(center+QPoint(window.width()/2,window.height()/2));
        dragPicture(preview,center-window.pos());
        QCOMPARE(window.property("drawerEdge").toString(),QStringLiteral("none"));
        QCOMPARE(window.pos(),center); QCOMPARE(fixture.count("camera_start_stream"),0);
    }

    void releaseAlreadyOverTheTabStaysCollapsedUntilFreshHover() {
        MiniFixtureServer fixture; MiniWindow window(fixture.url()); window.resize(400,280); window.show();
        auto *preview=window.findChild<QAbstractButton *>("cameraSnapshot");
        QTRY_VERIFY(showsColor(preview,Magenta));
        const QRect area=window.screen()->availableGeometry();
        const QPoint original=area.center()-QPoint(200,140); window.move(original);
        const QPoint start=preview->mapToGlobal(QPoint(1,preview->height()/2));
        const QPoint end(area.left()+1,start.y()); QCursor::setPos(end);
        sendPointer(preview,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
        sendPointer(preview,QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);
        sendPointer(preview,QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);
        QVERIFY(window.property("drawerHidden").toBool());
        auto *tab=window.drawerHandle(); QTRY_VERIFY(tab->isVisible());
        QVERIFY(tab->geometry().contains(end)); QTest::qWait(250);
        QVERIFY(window.property("drawerHidden").toBool()); QVERIFY(!window.isVisible());
        QCursor::setPos(end+QPoint(25,0)); QTest::qWait(150);
        QCursor::setPos(tab->geometry().center());
        QTRY_VERIFY(!window.property("drawerHidden").toBool()); QTRY_VERIFY(window.isVisible());
    }

    void liveRenderSurfaceSharesPanelFullScreenAndDragGestures() {
        MiniFixtureServer fixture; MiniWindow window(fixture.url()); window.resize(480,340); window.move(100,120); window.show();
        QTRY_VERIFY(window.client()->isConnected());
        auto *play = window.findChild<QPushButton *>("miniPlay"); QTRY_VERIFY(play->isEnabled());
        QTest::mouseClick(play, Qt::LeftButton);
        QTRY_VERIFY(window.findChild<CameraWindow *>());
        auto *live = window.findChild<CameraWindow *>();
        auto *view = live->findChild<QWebEngineView *>("cameraVideo"); QVERIFY(view);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count("camera_start_stream"), 1, 15000);
        auto *pin = window.findChild<QPushButton *>("miniAlwaysOnTop");
        QTest::mouseClick(pin,Qt::LeftButton); QVERIFY(window.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        QTest::mouseClick(pin,Qt::LeftButton); QVERIFY(!window.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        QCOMPARE(window.findChild<CameraWindow *>(),live); QCOMPARE(fixture.count("camera_start_stream"),1);
        // Hiding the main window to its thin tab preserves the live player.
        auto *dragSurface=view->focusProxy(); QTRY_VERIFY(dragSurface);
        const QRect area=window.screen()->availableGeometry();
        const QPoint target(area.left()+5,window.y());
        QCursor::setPos(target+QPoint(window.width()/2,window.height()/2));
        dragPicture(dragSurface,target-window.pos());
        QVERIFY(window.property("drawerHidden").toBool());
        QTRY_VERIFY(window.drawerHandle()->isVisible()); QVERIFY(!window.isVisible());
        QCOMPARE(window.findChild<CameraWindow *>(),live); QCOMPARE(fixture.count("camera_start_stream"),1);
        QCursor::setPos(window.drawerHandle()->geometry().center());
        QTRY_VERIFY(window.isVisible()); QTRY_VERIFY(!window.property("drawerHidden").toBool());
        QTest::qWait(200); QCOMPARE(window.findChild<CameraWindow *>(),live);
        QCOMPARE(fixture.count("camera_start_stream"),1);
        auto *overlay = live->findChild<QWidget *>("cameraOverlay");
        auto *header = window.findChild<QWidget *>("miniHeader");
        QTRY_VERIFY(view->focusProxy()); auto *surface = view->focusProxy();
        QTest::mouseClick(surface, Qt::LeftButton); QTRY_VERIFY(!header->isVisible()); QVERIFY(!overlay->isVisible());
        live->setPlayerState("error", "合成错误状态", false, false, false);
        QVERIFY(!overlay->isVisible());
        QTest::mouseDClick(surface, Qt::LeftButton); QTest::mouseRelease(surface, Qt::LeftButton);
        QVERIFY(!window.isFullScreen()); QCOMPARE(window.size(),QSize(800,450));
        QTest::mouseDClick(surface,Qt::LeftButton); QTest::mouseRelease(surface,Qt::LeftButton);
        QTRY_VERIFY(window.isFullScreen()); QTest::qWait(QApplication::doubleClickInterval()+40);
        QVERIFY(!overlay->isVisible()); QVERIFY(!header->isVisible());
        QTest::mouseDClick(surface, Qt::LeftButton); QTest::mouseRelease(surface, Qt::LeftButton);
        QTRY_VERIFY(!window.isFullScreen());
        const QPoint old = window.pos(); dragPicture(surface, QPoint(45,35)); QCOMPARE(window.pos(),old+QPoint(45,35));
        QTest::qWait(QApplication::doubleClickInterval()+40); QVERIFY(!header->isVisible());
        QTest::mouseClick(surface, Qt::LeftButton); QTRY_VERIFY(header->isVisible()); QVERIFY(overlay->isVisible());
        QCOMPARE(fixture.count("camera_start_stream"), 1); QCOMPARE(topLevelCameraWindows(),0);
        window.activateWindow(); QTest::qWait(30); QTest::keyClick(surface, Qt::Key_Escape);
        QTRY_VERIFY(!window.findChild<CameraWindow *>());
        QVERIFY(window.findChild<QWidget *>("miniFooter")->isVisible());
    }

    void minimizedHiddenAndOfflineStatesPauseOrClearPreviewThenReconnect() {
        MiniFixtureServer fixture; MiniWindow window(fixture.url()); window.show();
        QTRY_VERIFY(window.client()->isConnected());
        auto *preview = window.findChild<QAbstractButton *>("cameraSnapshot"); QVERIFY(preview);
        auto *refresh = window.findChild<QPushButton *>("miniSnapshotRefresh"); QVERIFY(refresh);
        QTRY_VERIFY(showsColor(preview, Magenta)); QCOMPARE(fixture.snapshots, 1);
        window.showMinimized(); QTRY_VERIFY(window.isMinimized()); QTest::qWait(80);
        refresh->click(); QTest::qWait(120); QCOMPARE(fixture.snapshots, 1);
        window.showNormal(); window.hide(); QTest::qWait(80);
        refresh->click(); QTest::qWait(120); QCOMPARE(fixture.snapshots, 1);
        window.show();
        const quint16 port = fixture.server.serverPort(); fixture.server.close(); fixture.peer->close();
        QTRY_VERIFY(!window.client()->isConnected()); QTRY_VERIFY(!showsColor(preview, Magenta));
        QVERIFY(!refresh->isEnabled());
        auto *connection = window.findChild<QLabel *>("miniConnection"); QVERIFY(connection); QVERIFY(!connection->text().isEmpty());
        QVERIFY(!connection->text().contains("已连接"));
        fixture.color = Cobalt; QVERIFY(fixture.server.listen(QHostAddress::LocalHost, port));
        QTRY_VERIFY_WITH_TIMEOUT(window.client()->isConnected(), 6000);
        QTRY_VERIFY(showsColor(preview, Cobalt)); QVERIFY(fixture.connections >= 2);
        QCOMPARE(fixture.count("camera_start_stream"), 0);
    }

    void deletionWhileSnapshotReplyIsPendingDestroysObserverBeforeClient() {
        MiniFixtureServer fixture; fixture.holdSnapshots = true;
        auto *window = new MiniWindow(fixture.url()); window->show();
        QTRY_VERIFY(window->client()->isConnected()); QTRY_COMPARE(fixture.held.size(), 1);
        auto *manager = window->findChild<CameraSnapshots *>(); QVERIFY(manager);
        QPointer<CameraSnapshots> guardedManager(manager); QPointer<MatterClient> guardedClient(window->client());
        QStringList order;
        connect(manager, &QObject::destroyed, this, [&] { order.append("observer"); });
        connect(window->client(), &QObject::destroyed, this, [&] { order.append("client"); });
        delete window;
        QVERIFY(guardedManager.isNull()); QVERIFY(guardedClient.isNull());
        QCOMPARE(order, QStringList({"observer", "client"}));
        QTest::qWait(100);
    }

    void resizedLayoutsFillClientAreaAndFloatControlsOverPreview() {
        MiniFixtureServer fixture; fixture.ptz = true; fixture.nodes = {ptzCameraNode()}; fixture.enableMetadata();
        MiniWindow window(fixture.url()); window.show();
        QTRY_VERIFY(window.client()->isConnected());
        auto *preview = window.findChild<QAbstractButton *>("cameraSnapshot"); QVERIFY(preview);
        auto *refresh = window.findChild<QPushButton *>("miniSnapshotRefresh"); QVERIFY(refresh);
        auto *time = window.findChild<QLabel *>("miniSnapshotTime"); QVERIFY(time);
        auto *add = window.findChild<QPushButton *>("miniAdd"); QVERIFY(add);
        auto *title = window.findChild<QLabel *>("miniTitle"); QVERIFY(title);
        auto *connection = window.findChild<QLabel *>("miniConnection"); QVERIFY(connection);
        auto *header = window.findChild<QWidget *>("miniHeader"); QVERIFY(header);
        auto *footer = window.findChild<QWidget *>("miniFooter"); QVERIFY(footer);
        auto *root = window.centralWidget(); QVERIFY(root);
        auto *metadata = window.findChild<QPushButton *>("miniMijiaMetadata"); QVERIFY(metadata);
        auto *location = window.findChild<QLabel *>("miniMijiaLocation"); QVERIFY(location);
        auto *ptz = window.findChild<CameraPtzControls *>(); QVERIFY(ptz);
        QTRY_COMPARE(location->text(), QStringLiteral("米家 · 合成家庭 / 客厅"));
        const QList<QWidget *> controls{header, footer, add, refresh, time, title, connection, metadata, location, ptz};
        QTRY_VERIFY(showsColor(preview, Magenta));
        for (const QSize size : {QSize(720, 600), QSize(500, 420), QSize(1200, 700), QSize(600, 900), QSize(400, 280)}) {
            window.resize(size); QTest::qWait(50);
            const QRect previewBox = boxIn(preview, root);
            QCOMPARE(previewBox, root->rect());
            for (QWidget *control : controls) {
                QVERIFY2(control->isVisible(), qPrintable(control->objectName()));
                const QRect controlBox = boxIn(control, root);
                QVERIFY2(!controlBox.isEmpty(), qPrintable(control->objectName()));
                QVERIFY2(previewBox.contains(controlBox), qPrintable(control->objectName()));
            }
            QVERIFY(boxIn(add, root).center().y() < root->height() / 2);
            QVERIFY(boxIn(refresh, root).center().y() > root->height() / 2);
        }
    }

private:
    QTemporaryDir m_settings;
};

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", qgetenv("QTWEBENGINE_CHROMIUM_FLAGS") + " --disable-gpu");
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    app.setStyle("Fusion"); app.setFont(QFont("Noto Sans CJK SC", 10));
    if (app.arguments().contains("--serve")) {
        MiniFixtureServer fixture; fixture.scene = true; fixture.ptz = true; fixture.nodes = {ptzCameraNode(false)}; fixture.enableMetadata();
        if (!fixture.server.isListening()) return 2;
        std::puts(QJsonDocument(QJsonObject{{"url", fixture.url().toString()}, {"synthetic", true}, {"expiresSeconds", 120}}).toJson(QJsonDocument::Compact).constData());
        std::fflush(stdout); QTimer::singleShot(120000, &app, &QApplication::quit); return app.exec();
    }
    MiniUiTest test; return QTest::qExec(&test, argc, argv);
}

#include "tst_miniui.moc"
