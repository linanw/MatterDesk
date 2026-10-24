#include "accessorypanel.h"
#include "homeeditors.h"
#include "homestore.h"
#include "matterclient.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QTimeEdit>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>

namespace {
class ControlsServer : public QObject {
public:
    QWebSocketServer socket{QStringLiteral("Accessory controls local fixture"), QWebSocketServer::NonSecureMode};
    QWebSocket *peer = nullptr;
    QJsonArray nodes = homeDemoNodes();
    QList<QJsonObject> writes;
    ControlsServer() {
        socket.listen(QHostAddress::LocalHost, 0);
        connect(&socket, &QWebSocketServer::newConnection, this, [this] {
            peer = socket.nextPendingConnection(); peer->setParent(this);
            connect(peer, &QWebSocket::textMessageReceived, this, [this](const QString &message) {
                const auto request = QJsonDocument::fromJson(message.toUtf8()).object();
                const auto command = request.value("command").toString();
                if (command == "start_listening" || command == "get_nodes") reply(request, nodes);
                else if (command == "get_node") reply(request, nodes.first());
                else writes.append(request); // Deliberately hold Invoke acknowledgments.
            });
            send({{"schema_version", 14}, {"min_supported_schema_version", 11}, {"sdk_version", "local UI fixture"}});
        });
    }
    QUrl url() const { return QUrl(QString("ws://127.0.0.1:%1/ws").arg(socket.serverPort())); }
    void send(QJsonObject object) { if (peer) peer->sendTextMessage(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact))); }
    void reply(QJsonObject request, QJsonValue result) { send({{"message_id", request.value("message_id")}, {"result", result}}); }
    void report(const QString &path, const QJsonValue &value) {
        auto node = nodes.first().toObject(); auto attributes = node.value("attributes").toObject(); attributes.insert(path, value); node.insert("attributes", attributes); nodes.replace(0, node);
        send({{"event", "attribute_updated"}, {"data", QJsonArray{1, path, value}}});
    }
};
MatterDevice endpoint(const QList<MatterDevice> &devices, int id) {
    for (const auto &device : devices) if (device.endpointId == id) return device;
    return {};
}
QDialog *editor(const QString &name) {
    for (auto *widget : QApplication::topLevelWidgets()) if (widget->objectName() == name) {
        auto *dialog = qobject_cast<QDialog *>(widget);
        // A failed assertion inside a queued UI step must not strand exec().
        if (dialog) QTimer::singleShot(5000, dialog, &QDialog::reject);
        return dialog;
    }
    return nullptr;
}
}

class HomeControlsTest : public QObject {
    Q_OBJECT
private slots:
    void targetsNeedApplyAndReportedStateRemainsAuthoritative() {
        ControlsServer server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QTemporaryDir temporary; HomeStore store(&client, temporary.path()); store.setProfile("fixture");
        AccessoryPanel panel(&client, &store, endpoint(client.devices(), 2)); panel.show();
        auto *target = panel.findChild<QDoubleSpinBox *>("target_brightness"); QVERIFY(target);
        auto *slider = panel.findChild<QSlider *>("slider_brightness"); QVERIFY(slider);
        auto *current = panel.findChild<QLabel *>("current_brightness"); QVERIFY(current);
        auto *apply = panel.findChild<QPushButton *>("apply_brightness"); QVERIFY(apply);
        const QString initial = current->text();
        target->setValue(35); slider->setFocus(); QTest::keyClick(slider, Qt::Key_End);
        QTest::qWait(80); QCOMPARE(server.writes.size(), 0); QCOMPARE(current->text(), initial);
        QTest::mouseClick(apply, Qt::LeftButton); QTRY_COMPARE(server.writes.size(), 1);
        QVERIFY(!apply->isEnabled()); QCOMPARE(current->text(), initial);
        server.reply(server.writes.first(), QJsonValue(QJsonValue::Null)); QTRY_VERIFY(apply->isEnabled());
        QCOMPARE(current->text(), initial); // Invoke success is never fabricated as a state update.
        server.report("2/8/0", 254); QTRY_COMPARE(current->text(), QString("当前：100%"));
        client.disconnectFromServer(); QTRY_VERIFY(!apply->isEnabled()); QVERIFY(!target->isEnabled());
    }

    void unknownOfflineAndBounds() {
        ControlsServer server;
        auto node = server.nodes.first().toObject(); auto attributes = node.value("attributes").toObject(); attributes.insert("2/6/0", QJsonValue(QJsonValue::Null)); node.insert("attributes", attributes); server.nodes.replace(0, node);
        MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QTemporaryDir temporary; HomeStore store(&client, temporary.path()); store.setProfile("fixture");
        AccessoryPanel panel(&client, &store, endpoint(client.devices(), 2)); panel.show();
        auto *choice = panel.findChild<QComboBox *>("target_power"); QVERIFY(choice); QCOMPARE(choice->currentIndex(), -1);
        QCOMPARE(panel.findChild<QLabel *>("current_power")->text(), QString("当前：未知"));
        QVERIFY(!panel.findChild<QPushButton *>("apply_power")->isEnabled());
        auto *temperature = panel.findChild<QDoubleSpinBox *>("target_colorTemperature"); QVERIFY(temperature);
        QCOMPARE(temperature->minimum(), 153.0); QCOMPARE(temperature->maximum(), 500.0);
        server.report("2/57/17", false); QTRY_VERIFY(!temperature->isEnabled());
        QVERIFY(!panel.findChild<QPushButton *>("apply_colorTemperature")->isEnabled()); QCOMPARE(server.writes.size(), 0);
    }

    void failedCommandIsVisibleAndDoesNotChangeCurrentValue() {
        ControlsServer server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        AccessoryPanel panel(&client, nullptr, endpoint(client.devices(), 3)); panel.show();
        auto *target = panel.findChild<QComboBox *>("target_power"); target->setCurrentIndex(target->findData(true));
        auto *apply = panel.findChild<QPushButton *>("apply_power"); QTest::mouseClick(apply, Qt::LeftButton); QTRY_COMPARE(server.writes.size(), 1);
        server.send({{"message_id", server.writes.first().value("message_id")}, {"error_code", 8}, {"details", "fixture command denied"}});
        QTRY_VERIFY(panel.findChild<QLabel *>("controlFeedback")->text().contains("失败"));
        QVERIFY(apply->isEnabled()); QCOMPARE(panel.findChild<QLabel *>("current_power")->text(), QString("当前：已关闭"));
    }

    void metadataIsLocalAndProfileChangeClosesSheet() {
        MatterClient client; client.setHomeDemoMode(true); QTemporaryDir temporary;
        HomeStore store(&client, temporary.path()); store.setProfile("first"); const auto roomId = store.saveRoom({{"name", "客厅"}}); QVERIFY(!roomId.isEmpty());
        AccessoryPanel panel(&client, &store, endpoint(client.devices(), 2)); panel.show();
        auto *name = panel.findChild<QLineEdit *>("accessoryName"); QTest::keyClicks(name, "Desk lamp");
        auto *room = panel.findChild<QComboBox *>("accessoryRoom"); room->setCurrentIndex(room->findData(roomId));
        auto *favorite = panel.findChild<QCheckBox *>("accessoryFavorite");
        QTest::mouseClick(favorite, Qt::LeftButton, Qt::NoModifier, QPoint(8, favorite->height() / 2));
        QSignalSpy commands(&client, &MatterClient::commandFinished);
        QTest::mouseClick(panel.findChild<QPushButton *>("saveAccessory"), Qt::LeftButton);
        QCOMPARE(store.accessory("1/2").value("name").toString(), QString("Desk lamp"));
        QCOMPARE(store.accessory("1/2").value("roomId").toString(), roomId); QVERIFY(store.accessory("1/2").value("favorite").toBool());
        QCOMPARE(commands.count(), 0); store.setProfile("second"); QTRY_VERIFY(!panel.isVisible());
        QVERIFY(store.accessory("1/2").isEmpty());
    }

    void extendedControlsUseAdvertisedModesAndColor() {
        MatterClient client; client.setHomeDemoMode(true);
        AccessoryPanel color(&client, nullptr, endpoint(client.devices(), 8));
        QVERIFY(color.findChild<QDoubleSpinBox *>("target_colorHue")); QVERIFY(color.findChild<QDoubleSpinBox *>("target_colorSaturation"));
        AccessoryPanel thermostat(&client, nullptr, endpoint(client.devices(), 11));
        auto *mode = thermostat.findChild<QComboBox *>("target_thermostatMode"); QVERIFY(mode);
        const auto limits = endpoint(client.devices(), 11).controlLimits.value("thermostatMode").toArray();
        QCOMPARE(mode->count(), limits.size()); for (int i = 0; i < mode->count(); ++i) QVERIFY(limits.contains(QJsonValue::fromVariant(mode->itemData(i))));
        auto *heat = thermostat.findChild<QDoubleSpinBox *>("target_heatSetpoint"); QVERIFY(heat);
        QCOMPARE(heat->minimum(), endpoint(client.devices(), 11).controlLimits.value("heatSetpoint").toObject().value("min").toDouble());
    }

    void colorPickerSurvivesCapabilityRebuildAndClosesWithProfile() {
        ControlsServer server; MatterClient client; client.connectToServer(server.url()); QTRY_VERIFY(client.isConnected());
        QTemporaryDir temporary; HomeStore store(&client, temporary.path()); store.setProfile("first");
        AccessoryPanel panel(&client, &store, endpoint(client.devices(), 8)); panel.show();
        QPointer<QDoubleSpinBox> oldHue = panel.findChild<QDoubleSpinBox *>("target_colorHue"); QVERIFY(oldHue);
        QTest::mouseClick(panel.findChild<QPushButton *>("colorPickerButton"), Qt::LeftButton);
        QPointer<QColorDialog> picker = panel.findChild<QColorDialog *>(); QVERIFY(picker); QTRY_VERIFY(picker->isVisible());
        server.report("8/768/65532", 16); QTRY_VERIFY(oldHue.isNull());
        QVERIFY(picker); picker->setCurrentColor(Qt::green); picker->accept();
        QTRY_VERIFY(picker.isNull()); QCOMPARE(server.writes.size(), 0);
        server.report("8/768/65532", 19); QTRY_VERIFY(endpoint(client.devices(), 8).controls.contains("color"));
        QPointer<AccessoryPanel> owned = new AccessoryPanel(&client, &store, endpoint(client.devices(), 8));
        owned->setAttribute(Qt::WA_DeleteOnClose); owned->show();
        QTest::mouseClick(owned->findChild<QPushButton *>("colorPickerButton"), Qt::LeftButton);
        QPointer<QColorDialog> closing = owned->findChild<QColorDialog *>(); QVERIFY(closing);
        store.setProfile("second"); QTRY_VERIFY(owned.isNull()); QTRY_VERIFY(closing.isNull());
        QCOMPARE(server.writes.size(), 0);
    }

    void sceneDraftSupportsTypedActionsWithoutCommands() {
        MatterClient client; client.setHomeDemoMode(true); QSignalSpy commands(&client, &MatterClient::commandFinished);
        QTimer::singleShot(0, this, [] {
            auto *dialog = editor("sceneEditor"); QVERIFY(dialog);
            dialog->findChild<QLineEdit *>("sceneName")->setText("Quiet evening");
            QTest::mouseClick(dialog->findChild<QPushButton *>("addSceneAction"), Qt::LeftButton);
            auto *device = dialog->findChild<QComboBox *>("actionDevice"); device->setCurrentIndex(device->findData("1/8"));
            auto *action = dialog->findChild<QComboBox *>("actionKind"); action->setCurrentIndex(action->findData("color"));
            dialog->findChild<QDoubleSpinBox *>("actionHue")->setValue(200); dialog->findChild<QDoubleSpinBox *>("actionSaturation")->setValue(70);
            auto *save = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save); QVERIFY(save->isEnabled()); QTest::mouseClick(save, Qt::LeftButton);
        });
        const auto draft = HomeEditors::scene(nullptr, {}, client.devices()); QCOMPARE(draft.value("name").toString(), QString("Quiet evening"));
        const auto action = draft.value("actions").toArray().first().toObject(); QCOMPARE(action.value("deviceKey").toString(), QString("1/8"));
        QCOMPARE(action.value("action").toString(), QString("color")); QCOMPARE(action.value("value").toObject().value("hue").toDouble(), 200.0); QCOMPARE(commands.count(), 0);
    }

    void automationDraftRoundTripsDaysAndObservedUnlock() {
        const auto devices = devicesFromNodes(homeDemoNodes()); const QJsonArray scenes{QJsonObject{{"id", "scene-1"}, {"name", "Lights"}}};
        QTimer::singleShot(0, this, [] {
            auto *dialog = editor("automationEditor"); QVERIFY(dialog);
            dialog->findChild<QLineEdit *>("automationName")->setText("Weekdays"); dialog->findChild<QTimeEdit *>("automationTime")->setTime(QTime(6, 45));
            dialog->findChild<QCheckBox *>("automationDay6")->setChecked(false); dialog->findChild<QCheckBox *>("automationDay7")->setChecked(false);
            QTest::mouseClick(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save), Qt::LeftButton);
        });
        auto draft = HomeEditors::automation(nullptr, {}, devices, scenes); QVERIFY(!draft.isEmpty());
        const auto time = draft.value("trigger").toObject(); QCOMPARE(time.value("time").toString(), QString("06:45")); QCOMPARE(time.value("days").toArray(), QJsonArray({1, 2, 3, 4, 5})); QVERIFY(!draft.value("enabled").toBool());
        QTimer::singleShot(0, this, [] {
            auto *dialog = editor("automationEditor"); QVERIFY(dialog);
            auto *type = dialog->findChild<QComboBox *>("automationTrigger"); type->setCurrentIndex(type->findData("device"));
            auto *device = dialog->findChild<QComboBox *>("actionDevice"); device->setCurrentIndex(device->findData("1/9"));
            auto *action = dialog->findChild<QComboBox *>("actionKind"); action->setCurrentIndex(action->findData("lock"));
            auto *value = dialog->findChild<QComboBox *>("actionValue"); value->setCurrentIndex(value->findData(false));
            QTest::mouseClick(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save), Qt::LeftButton);
        });
        draft = HomeEditors::automation(nullptr, draft, devices, scenes);
        const auto trigger = draft.value("trigger").toObject(); QCOMPARE(trigger.value("type").toString(), QString("device")); QCOMPARE(trigger.value("deviceKey").toString(), QString("1/9")); QCOMPARE(trigger.value("value"), QJsonValue(false));
    }

    void canceledEditorReturnsEmptyDraft() {
        QTimer::singleShot(0, this, [] { auto *dialog = editor("sceneEditor"); QVERIFY(dialog); dialog->reject(); });
        QVERIFY(HomeEditors::scene(nullptr, {}, devicesFromNodes(homeDemoNodes())).isEmpty());
    }
};

QTEST_MAIN(HomeControlsTest)
#include "tst_homecontrols.moc"
