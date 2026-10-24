#include "mainwindow.h"
#include "devicecard.h"
#include "accessorypanel.h"
#include "homestore.h"

#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QPointer>
#include <QStandardPaths>
#include <QUuid>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>

namespace {
QList<DeviceCard *> visibleCards(MainWindow &window) {
    QList<DeviceCard *> cards;
    for (auto *card : window.findChildren<DeviceCard *>())
        if (card->isVisible()) cards.append(card);
    return cards;
}
DeviceCard *cardForEndpoint(MainWindow &window, int endpoint) {
    for (auto *card : window.findChildren<DeviceCard *>())
        if (card->device().endpointId == endpoint) return card;
    return nullptr;
}
QPushButton *navButton(MainWindow &window, const QString &text) {
    for (auto *button : window.findChildren<QPushButton *>("navigation"))
        if (button->text().contains(text)) return button;
    return nullptr;
}
QPushButton *navForPage(MainWindow &window, const QString &page) {
    for (auto *button : window.findChildren<QPushButton *>("navigation"))
        if (button->property("page").toString() == page && !button->isHidden()) return button;
    return nullptr;
}
QSlider *sliderWithName(DeviceCard *card, const QString &name) {
    for (auto *slider : card->findChildren<QSlider *>())
        if (slider->accessibleName() == name) return slider;
    return nullptr;
}
// Protocol fixtures only. No camera is added to demoNodes and no media is opened.
QJsonArray cameraUiNodes() {
    QJsonObject attributes{
        {"0/40/5", "协议测试桥"},
        {"1/29/0", QJsonArray{QJsonObject{{"deviceType", 0x0142}, {"revision", 1}}}},
        {"1/29/1", QJsonArray{29, 57, 0x0551, 0x0553}},
        {"1/57/5", "协议测试摄像头"}, {"1/57/17", true},
        {"2/29/0", QJsonArray{QJsonObject{{"deviceType", 0x010a}, {"revision", 1}}}},
        {"2/29/1", QJsonArray{29, 57, 6}},
        {"2/57/5", "协议测试开关"}, {"2/57/17", true}, {"2/6/0", false}
    };
    return {QJsonObject{{"node_id", 9001}, {"available", true}, {"attributes", attributes}}};
}
class UiFixtureServer : public QObject {
public:
    QWebSocketServer socket{QStringLiteral("MatterDesk UI fixture"), QWebSocketServer::NonSecureMode};
    QWebSocket *peer = nullptr;
    QJsonObject info{{"schema_version", 14}, {"min_supported_schema_version", 11}, {"sdk_version", "UI fixture"}, {"fabric_id", "fixture-fabric-a"}};
    UiFixtureServer() {
        socket.listen(QHostAddress::LocalHost, 0);
        connect(&socket, &QWebSocketServer::newConnection, this, [this] {
            peer = socket.nextPendingConnection(); peer->setParent(this);
            connect(peer, &QWebSocket::textMessageReceived, this, [this](const QString &message) {
                const auto request = QJsonDocument::fromJson(message.toUtf8()).object();
                const auto command = request.value("command").toString();
                if (command == "start_listening" || command == "get_nodes")
                    send({{"message_id", request.value("message_id")}, {"result", cameraUiNodes()}});
                else
                    send({{"message_id", request.value("message_id")}, {"error_code", 8}, {"details", "Media access is not part of this UI test"}});
            });
            send(info);
        });
    }
    QUrl url() const { return QUrl(QString("ws://127.0.0.1:%1/ws").arg(socket.serverPort())); }
    void send(const QJsonObject &message) {
        if (peer) peer->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }
};
}

class MatterUiTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("MatterDeskTests");
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName("MatterUi-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        QVERIFY(m_settings.isValid());
        m_appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, m_settings.path());
    }

    void init() {
        QSettings().clear();
        if (QDir(m_appData).exists()) QVERIFY(QDir(m_appData).removeRecursively());
    }
    void cleanupTestCase() {
        QSettings().clear();
        if (QDir(m_appData).exists()) QVERIFY(QDir(m_appData).removeRecursively());
    }

    void emptyThenDemoThenExit() {
        MainWindow window; window.show();
        QVERIFY(window.findChild<QWidget *>("empty")->isVisible());
        QCOMPARE(window.findChildren<DeviceCard *>().size(), 0);
        QVERIFY(!window.client()->isConnected());
        window.startDemo();
        QTRY_COMPARE(visibleCards(window).size(), 12);
        QVERIFY(window.client()->isConnected());
        QVERIFY(window.client()->isDemoMode());
        QVERIFY(window.findChild<QLabel *>("demoBanner")->isVisible());
        QCOMPARE(window.client()->devices().size(), 12);
        auto *offline = cardForEndpoint(window, 7);
        QVERIFY(offline);
        QVERIFY(!offline->findChild<QPushButton *>("power")->isEnabled());
        window.client()->setDemoMode(false);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_COMPARE(window.findChildren<DeviceCard *>().size(), 0);
        QVERIFY(window.findChild<QWidget *>("empty")->isVisible());
        QVERIFY(!window.findChild<QLabel *>("demoBanner")->isVisible());
    }

    void categorySearchAndAvailabilityFilters() {
        MainWindow window; window.show(); window.startDemo();
        auto *light = navButton(window, "灯光"); QVERIFY(light);
        QTest::mouseClick(light, Qt::LeftButton);
        QCOMPARE(visibleCards(window).size(), 4);
        auto *all = navButton(window, "设备总览"); QVERIFY(all);
        QTest::mouseClick(all, Qt::LeftButton);
        auto *filter = window.findChild<QComboBox *>("deviceFilter"); QVERIFY(filter);
        filter->setCurrentIndex(1);
        QCOMPARE(visibleCards(window).size(), 11);
        filter->setCurrentIndex(2);
        QCOMPARE(visibleCards(window).size(), 1);
        QCOMPARE(visibleCards(window).first()->device().endpointId, 7);
        filter->setCurrentIndex(0);
        QLineEdit *search = nullptr;
        for (auto *edit : window.findChildren<QLineEdit *>())
            if (edit->accessibleName() == "搜索设备") search = edit;
        QVERIFY(search);
        search->setText("咖啡");
        QCOMPARE(visibleCards(window).size(), 1);
        QCOMPARE(visibleCards(window).first()->device().endpointId, 3);
        search->setText("does not exist");
        QCOMPARE(visibleCards(window).size(), 0);
        QVERIFY(window.findChild<QWidget *>("empty")->isVisible());
        search->clear();
        QCOMPARE(visibleCards(window).size(), 12);
    }

    void controlsRoundTripThroughClient() {
        MainWindow window; window.show(); window.startDemo();
        auto *outlet = cardForEndpoint(window, 3); QVERIFY(outlet);
        QVERIFY(!outlet->device().on);
        QSignalSpy finished(window.client(), &MatterClient::commandFinished);
        QTest::mouseClick(outlet->findChild<QPushButton *>("power"), Qt::LeftButton);
        // Demo uses asynchronous protocol-like updates; no optimistic state.
        QVERIFY(!outlet->device().on);
        QTRY_VERIFY(outlet->device().on);
        QCOMPARE(finished.count(), 1);
        QVERIFY(finished.last().at(2).toBool());
        auto *lamp = cardForEndpoint(window, 2); QVERIFY(lamp);
        auto *level = sliderWithName(lamp, "亮度百分比"); QVERIFY(level); QVERIFY(level->isHidden());
        auto *temperature = sliderWithName(lamp, "色温"); QVERIFY(temperature); QVERIFY(temperature->isHidden());
        QTest::mouseClick(lamp->findChild<QPushButton *>("more"), Qt::LeftButton);
        QPointer<AccessoryPanel> panel = window.findChild<AccessoryPanel *>(); QVERIFY(panel); QVERIFY(panel->isVisible());
        auto *targetLevel = panel->findChild<QDoubleSpinBox *>("target_brightness"); QVERIFY(targetLevel);
        auto *applyLevel = panel->findChild<QPushButton *>("apply_brightness"); QVERIFY(applyLevel);
        targetLevel->setValue(100); QCOMPARE(finished.count(), 1);
        QTest::mouseClick(applyLevel, Qt::LeftButton);
        QVERIFY(lamp->device().level != 254);
        QTRY_COMPARE(lamp->device().level, 254);
        auto *targetTemperature = panel->findChild<QDoubleSpinBox *>("target_colorTemperature"); QVERIFY(targetTemperature);
        const int minimum = qRound(targetTemperature->minimum()); targetTemperature->setValue(minimum);
        QTest::mouseClick(panel->findChild<QPushButton *>("apply_colorTemperature"), Qt::LeftButton);
        QTRY_COMPARE(lamp->device().colorTemperature, minimum);
        window.client()->disconnectFromServer();
        QVERIFY(!outlet->findChild<QPushButton *>("power")->isEnabled());
        QTRY_VERIFY(!panel || !panel->isVisible());
    }

    void homeMetadataFavoritesRoomsAndPersistence() {
        QString roomId;
        {
            MainWindow window; window.show(); window.startDemo();
            QCOMPARE(window.home()->rooms().size(), 5); QCOMPARE(window.home()->zones().size(), 1);
            QCOMPARE(window.home()->scenes().size(), 3); QCOMPARE(window.home()->rules().size(), 1);
            QVERIFY(!window.home()->rules().first().toObject().value("enabled").toBool());
            auto *favorites = navForPage(window, "favorite"); QVERIFY(favorites); QTest::mouseClick(favorites, Qt::LeftButton);
            QCOMPARE(visibleCards(window).size(), 6);
            auto *all = navForPage(window, "all"); QVERIFY(all); QTest::mouseClick(all, Qt::LeftButton);
            auto *outlet = cardForEndpoint(window, 3); QVERIFY(outlet);
            auto *favorite = outlet->findChild<QPushButton *>("favorite"); QVERIFY(favorite); QVERIFY(!favorite->isChecked());
            QTest::mouseClick(favorite, Qt::LeftButton);
            QVERIFY(window.home()->accessory("1/3").value("favorite").toBool());
            roomId = window.home()->saveRoom({{"name", "咖啡角"}, {"zoneId", "indoor"}}); QVERIFY(!roomId.isEmpty());
            QVERIFY(window.home()->setAccessory("1/3", {{"name", "<b>自定义咖啡机</b>"}, {"roomId", roomId}}));
            auto *name = outlet->findChild<QLabel *>("deviceName"); QVERIFY(name);
            QCOMPARE(name->text(), QStringLiteral("<b>自定义咖啡机</b>")); QCOMPARE(name->textFormat(), Qt::PlainText);
            QPushButton *room = nullptr; QTRY_VERIFY((room = navForPage(window, "room:" + roomId))); QTest::mouseClick(room, Qt::LeftButton);
            QCOMPARE(visibleCards(window).size(), 1); QCOMPARE(visibleCards(window).first()->device().key, QStringLiteral("1/3"));
            auto *zone = navForPage(window, "zone:indoor"); QVERIFY(zone); QTest::mouseClick(zone, Qt::LeftButton);
            QCOMPARE(visibleCards(window).size(), 12);
            favorites = navForPage(window, "favorite"); QVERIFY(favorites); QTest::mouseClick(favorites, Qt::LeftButton);
            QCOMPARE(visibleCards(window).size(), 7);
            auto *search = window.findChild<QLineEdit *>("deviceSearch"); QVERIFY(search); search->setText("自定义");
            QCOMPARE(visibleCards(window).size(), 1);
            QVERIFY(window.home()->setAccessory("1/3", {{"hidden", true}})); QCOMPARE(visibleCards(window).size(), 0);
            QVERIFY(window.home()->setAccessory("1/3", {{"hidden", false}})); QCOMPARE(visibleCards(window).size(), 1);
        }
        MainWindow restored; restored.show(); restored.startDemo();
        QCOMPARE(restored.home()->rooms().size(), 6); QCOMPARE(restored.home()->scenes().size(), 3);
        const auto metadata = restored.home()->accessory("1/3"); QCOMPARE(metadata.value("roomId").toString(), roomId);
        QVERIFY(metadata.value("favorite").toBool()); QCOMPARE(metadata.value("name").toString(), QStringLiteral("<b>自定义咖啡机</b>"));
    }

    void clearingAccessoryNameRestoresReportedNameImmediately() {
        MainWindow window; window.show(); window.startDemo();
        QVERIFY(window.home()->setAccessory("1/3", {{"name", "自定义咖啡机"}}));
        auto *outlet = cardForEndpoint(window, 3); QVERIFY(outlet);
        QTest::mouseClick(outlet->findChild<QPushButton *>("more"), Qt::LeftButton);
        auto *panel = window.findChild<AccessoryPanel *>(); QVERIFY(panel);
        auto *name = panel->findChild<QLineEdit *>("accessoryName"); QVERIFY(name);
        QCOMPARE(name->text(), QStringLiteral("自定义咖啡机"));
        name->setFocus(); name->selectAll(); QTest::keyClick(name, Qt::Key_Backspace);
        auto *save = panel->findChild<QPushButton *>("saveAccessory"); QVERIFY(save); QVERIFY(save->isEnabled());
        QTest::mouseClick(save, Qt::LeftButton);
        QVERIFY(!window.home()->accessory("1/3").contains("name"));
        QTRY_COMPARE(panel->findChild<QLabel *>("hero")->text(), QStringLiteral("咖啡机插座"));
        QCOMPARE(outlet->findChild<QLabel *>("deviceName")->text(), QStringLiteral("咖啡机插座"));
        QCOMPARE(name->placeholderText(), QStringLiteral("咖啡机插座"));
    }

    void homeSceneExecutesRichDemoAndNavigationStaysConsistent() {
        MainWindow window; window.show(); window.startDemo();
        const auto sceneId = window.home()->saveScene({{"name", "测试家庭场景"}, {"actions", QJsonArray{
            QJsonObject{{"deviceKey", "1/3"}, {"action", "power"}, {"value", true}},
            QJsonObject{{"deviceKey", "1/2"}, {"action", "brightness"}, {"value", 37}},
            QJsonObject{{"deviceKey", "1/10"}, {"action", "position"}, {"value", 25}},
            QJsonObject{{"deviceKey", "1/12"}, {"action", "fanPercent"}, {"value", 62}},
            QJsonObject{{"deviceKey", "1/11"}, {"action", "heatSetpoint"}, {"value", 20}}}}});
        QVERIFY(!sceneId.isEmpty());
        auto *scenes = navForPage(window, "scenes"); QVERIFY(scenes); QTest::mouseClick(scenes, Qt::LeftButton);
        QVERIFY(window.findChild<QWidget *>("scenePage")->isVisible()); QCOMPARE(visibleCards(window).size(), 0);
        QSignalSpy finished(window.home(), &HomeStore::executionFinished);
        QVERIFY(window.home()->runScene(sceneId)); QVERIFY(window.home()->isExecuting());
        QTRY_VERIFY([&] {
            int visibleRuns = 0;
            for (auto *run : window.findChildren<QPushButton *>()) if (run->property("sceneId").isValid() && run->isVisible()) {
                ++visibleRuns; if (run->isEnabled()) return false;
            }
            return visibleRuns == 4;
        }());
        QTRY_COMPARE(finished.size(), 1); QVERIFY(finished.last().at(1).toBool()); QVERIFY(!window.home()->isExecuting());
        QVERIFY(cardForEndpoint(window, 3)->device().on);
        QCOMPARE(cardForEndpoint(window, 2)->device().controlState.value("brightness").toInt(), 37);
        QCOMPARE(cardForEndpoint(window, 10)->device().controlState.value("position").toDouble(), 25.0);
        QCOMPARE(cardForEndpoint(window, 12)->device().controlState.value("fanPercent").toInt(), 62);
        QCOMPARE(cardForEndpoint(window, 11)->device().controlState.value("heatSetpoint").toDouble(), 20.0);
        QVERIFY(window.home()->activity().first().toObject().value("success").toBool());
        auto *rules = navForPage(window, "automations"); QVERIFY(rules); QTest::mouseClick(rules, Qt::LeftButton);
        QVERIFY(window.findChild<QWidget *>("automationPage")->isVisible());
        auto *activity = navForPage(window, "activity"); QVERIFY(activity); QTest::mouseClick(activity, Qt::LeftButton);
        QVERIFY(window.findChild<QWidget *>("activityPage")->isVisible());
        auto *all = navForPage(window, "all"); QVERIFY(all); QTest::mouseClick(all, Qt::LeftButton);
        QCOMPARE(visibleCards(window).size(), 12);
    }

    void controllerFabricProfilesDoNotShareDeviceMetadata() {
        UiFixtureServer server; QVERIFY(server.socket.isListening());
        MainWindow window; window.show(); window.client()->connectToServer(server.url());
        QTRY_VERIFY(window.client()->isConnected()); QTRY_VERIFY(!window.home()->profile().isEmpty());
        const auto originalProfile = window.home()->profile(); const auto originalInfo = server.info;
        QVERIFY(!originalProfile.startsWith("demo")); QVERIFY(window.home()->rooms().isEmpty()); QVERIFY(window.home()->scenes().isEmpty());
        QVERIFY(window.home()->setAccessory("9001/2", {{"name", "原家庭开关"}, {"favorite", true}}));
        auto *card = cardForEndpoint(window, 2); QVERIFY(card);
        QTest::mouseClick(card->findChild<QPushButton *>("more"), Qt::LeftButton);
        QPointer<AccessoryPanel> oldPanel = window.findChild<AccessoryPanel *>(); QVERIFY(oldPanel); QVERIFY(oldPanel->isVisible());
        server.info.insert("fabric_id", "fixture-fabric-b");
        server.send({{"event", "server_info_updated"}, {"data", server.info}});
        QTRY_VERIFY(window.client()->isConnected() && !window.home()->profile().isEmpty() && window.home()->profile() != originalProfile);
        QVERIFY(window.home()->accessory("9001/2").isEmpty());
        QCOMPARE(cardForEndpoint(window, 2)->device().name, QStringLiteral("协议测试开关"));
        QTRY_VERIFY(!oldPanel || !oldPanel->isVisible());
        server.info = originalInfo;
        server.send({{"event", "server_info_updated"}, {"data", server.info}});
        QTRY_COMPARE(window.home()->profile(), originalProfile);
        QCOMPARE(window.home()->accessory("9001/2").value("name").toString(), QStringLiteral("原家庭开关"));
        QVERIFY(window.home()->accessory("9001/2").value("favorite").toBool());
        window.client()->disconnectFromServer(); QTRY_VERIFY(window.home()->profile().isEmpty());
    }

    void longHouseholdNamesFitMinimumWindowWidth() {
        MainWindow window; window.show(); window.startDemo();
        const QString longName(80, QChar(0x5BB6));
        QVERIFY(window.home()->setHomeName(longName));
        auto room = window.home()->rooms().first().toObject(); room.insert("name", longName);
        QVERIFY(!window.home()->saveRoom(room).isEmpty());
        QVERIFY(window.home()->setAccessory("1/2", {{"name", longName}}));
        const auto originals = window.home()->scenes();
        for (const auto &entry : originals) {
            auto scene = entry.toObject(); scene.insert("name", longName); QVERIFY(!window.home()->saveScene(scene).isEmpty());
        }
        QVERIFY(!window.home()->saveScene({{"name", longName}, {"actions", QJsonArray{
            QJsonObject{{"deviceKey", "1/3"}, {"action", "power"}, {"value", true}}}}}).isEmpty());
        window.resize(940, 680); QTest::qWait(150);
        QCOMPARE(window.size(), QSize(940, 680)); QCOMPARE(visibleCards(window).size(), 12);
        auto *scroll = window.findChild<QScrollArea *>("deviceScroll"); QVERIFY(scroll);
        auto *grid = window.findChild<QWidget *>("gridHost"); QVERIFY(grid);
        QVERIFY2(grid->width() <= scroll->viewport()->width(), "Long names must wrap inside the device viewport.");
        int quickButtons = 0;
        for (auto *button : window.findChildren<QPushButton *>("quickScene")) if (button->isVisible()) {
            ++quickButtons; QVERIFY(button->width() <= scroll->viewport()->width());
            QCOMPARE(button->toolTip(), longName);
        }
        QCOMPARE(quickButtons, 4);
        auto *scenes = navForPage(window, "scenes"); QVERIFY(scenes); QTest::mouseClick(scenes, Qt::LeftButton);
        QTest::qWait(30); QCOMPARE(window.width(), 940); QVERIFY(window.findChild<QWidget *>("scenePage")->isVisible());
    }

    void invalidServerAddressDoesNotLeaveDemo() {
        MainWindow window; window.show(); window.startDemo();
        window.setServerUrl("http://homebridge.local:8581");
        auto *connect = window.findChild<QPushButton *>("connectButton"); QVERIFY(connect);
        QTest::mouseClick(connect, Qt::LeftButton);
        QVERIFY(window.client()->isDemoMode());
        QVERIFY(window.findChild<QLabel *>("notice")->isVisible());
        QVERIFY(window.findChild<QLabel *>("notice")->text().contains("ws://"));
    }

    void sensorRefreshDoesNotLosePendingKeyboardLevel() {
        const auto device = devicesFromNodes(demoNodes()).first();
        DeviceCard card(device); card.updateDevice(device, true); card.show();
        auto *level = sliderWithName(&card, "亮度百分比"); QVERIFY(level);
        int requested = -1;
        connect(&card, &DeviceCard::levelRequested, &card, [&](const MatterDevice &, int percent) { requested = percent; });
        level->triggerAction(QAbstractSlider::SliderToMaximum);
        QCOMPARE(level->value(), 100);
        // An unrelated sensor report makes MainWindow synchronize every card.
        card.updateDevice(device, true);
        QTRY_COMPARE(requested, 100);
    }

    void cameraActionRequiresCameraCapabilityAndOnlineConnection() {
        const auto devices = devicesFromNodes(cameraUiNodes());
        QCOMPARE(devices.size(), 2);
        const auto camera = devices.first();
        QVERIFY(camera.hasCamera);
        QCOMPARE(camera.kind, QStringLiteral("camera"));
        DeviceCard card(camera); card.updateDevice(camera, true); card.show();
        auto *open = card.findChild<QPushButton *>("cameraOpen"); QVERIFY(open);
        QVERIFY(open->isVisible()); QVERIFY(open->isEnabled());
        QString requested;
        connect(&card, &DeviceCard::cameraRequested, &card, [&](const MatterDevice &device) { requested = device.key; });
        QTest::mouseClick(open, Qt::LeftButton);
        QCOMPARE(requested, camera.key);
        auto offline = camera; offline.available = false;
        card.updateDevice(offline, true);
        QVERIFY(open->isVisible()); QVERIFY(!open->isEnabled());
        card.updateDevice(camera, false);
        QVERIFY(!open->isEnabled());
        const auto ordinary = devices.last(); QVERIFY(!ordinary.hasCamera);
        DeviceCard switchCard(ordinary); switchCard.updateDevice(ordinary, true); switchCard.show();
        QVERIFY(switchCard.findChild<QPushButton *>("cameraOpen")->isHidden());
        QVERIFY(switchCard.findChild<QPushButton *>("power")->isVisible());
    }

    void cameraNavigationShowsOnlyGenuineCameraEndpoints() {
        UiFixtureServer server; QVERIFY(server.socket.isListening());
        MainWindow window; window.show(); window.client()->connectToServer(server.url());
        QTRY_VERIFY(window.client()->isConnected());
        QTRY_COMPARE(visibleCards(window).size(), 2);
        auto *cameraNav = navButton(window, "摄像头"); QVERIFY(cameraNav);
        QTest::mouseClick(cameraNav, Qt::LeftButton);
        QCOMPARE(visibleCards(window).size(), 1);
        auto *cameraCard = visibleCards(window).first();
        QVERIFY(cameraCard->device().hasCamera);
        QCOMPARE(cameraCard->device().endpointId, 1);
        QVERIFY(cameraCard->findChild<QPushButton *>("cameraOpen")->isEnabled());
        // Live reachability reports disable viewing without pretending a feed exists.
        server.send({{"event", "attribute_updated"}, {"data", QJsonArray{9001, "1/57/17", false}}});
        QTRY_VERIFY(!cameraCard->findChild<QPushButton *>("cameraOpen")->isEnabled());
        auto *all = navButton(window, "设备总览"); QVERIFY(all);
        QTest::mouseClick(all, Qt::LeftButton);
        QCOMPARE(visibleCards(window).size(), 2);
        window.client()->disconnectFromServer();
    }

    void cameraDemoHasNoFabricatedCamera() {
        MainWindow window; window.show(); window.startDemo();
        for (const auto &device : window.client()->devices()) QVERIFY(!device.hasCamera);
        auto *cameraNav = navButton(window, "摄像头"); QVERIFY(cameraNav);
        QTest::mouseClick(cameraNav, Qt::LeftButton);
        QCOMPARE(visibleCards(window).size(), 0);
        QVERIFY(window.findChild<QWidget *>("empty")->isVisible());
    }

private:
    QTemporaryDir m_settings;
    QString m_appData;
};

QTEST_MAIN(MatterUiTest)
#include "tst_matterui.moc"
