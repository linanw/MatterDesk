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
#include <QTemporaryDir>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>

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
};

QTEST_MAIN(TuyaMetadataUiTest)
#include "tst_tuyametadataui.moc"
