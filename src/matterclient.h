#pragma once

#include "matterdevice.h"

#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QObject>
#include <QTimer>
#include <QUrl>

class QWebSocket;
class CameraMetadataManager;

class MatterClient : public QObject {
    Q_OBJECT
public:
    explicit MatterClient(QObject *parent = nullptr);
    void connectToServer(QUrl url);
    void disconnectFromServer();
    void setDemoMode(bool enabled);
    void setHomeDemoMode(bool enabled);
    bool isDemoMode() const { return m_demo; }
    bool isConnected() const { return m_ready; }
    bool isConnecting() const { return m_wantConnection && !m_ready; }
    QString statusText() const { return m_status; }
    QList<MatterDevice> devices() const;
    QJsonObject serverInfo() const { return m_info; }
    QUrl serverUrl() const { return m_url; }
    QString commission(QString code);
    QString control(const MatterDevice &device, const QString &action, const QJsonValue &value);
    QString setOnOff(const MatterDevice &device, bool on);
    QString setLevel(const MatterDevice &device, int percent);
    QString setColorTemperature(const MatterDevice &device, int mired);
    void refresh();
    // Uses the same WebSocket that owns the camera's WebRTC session.
    // Background previews report failures only through cameraResponse.
    QString cameraRequest(QString command, QJsonObject args, int timeoutMs = 30000, bool quiet = false);
    // Only endpoint 1's fixed vendor step contract is exposed; arbitrary invokes remain private.
    QString readCameraPtz(const MatterDevice &device);
    QString moveCameraStep(const MatterDevice &device, int direction);
    bool canMoveCameraStep(const MatterDevice &device, int direction) const;
    bool cameraPtzBusy() const;
    CameraMetadataManager *cameraMetadata();
    quint64 controllerGeneration() const { return m_generation; }
    // Fixed endpoint/vendor contract; no arbitrary commands or paths are exposed.
    QString readCameraMetadataAttributes(const MatterDevice &device, const QList<int> &attributes);
    QString invokeCameraMetadata(const MatterDevice &device, const QString &operation, const QJsonObject &payload);
signals:
    void devicesChanged();
    void connectionChanged();
    void logMessage(QString message);
    void errorOccurred(QString message);
    void commandFinished(QString id, QString command, bool success, QString detail);
    void serverInfoChanged();
    void cameraResponse(QString id, QString command, bool success, QJsonValue result, QJsonObject error);
    void cameraEvent(QString event, QJsonObject data);
    void cameraPtzResponse(QString id, QString operation, bool success, QString detail);
    void cameraPtzStateChanged();
    void cameraMetadataResponse(QString id, bool success, QJsonValue result, QJsonObject error);
private:
    struct Pending {
        QString command;
        QJsonObject args;
        QTimer *timer = nullptr;
        bool quiet = false;
        QString ptzOperation, ptzKey;
        quint64 ptzGeneration = 0, ptzEpoch = 0, ptzRevision = 0;
        bool metadata = false;
        QString metadataKey;
        quint64 metadataGeneration = 0, metadataRevision = 0;
    };
    void enterDemo(bool enabled, const QJsonArray &nodes);
    void openSocket();
    void destroySocket();
    void receive(const QString &message);
    void handleEvent(const QJsonObject &message);
    void replaceNodes(const QJsonArray &nodes);
    void upsertNode(const QJsonObject &node);
    void updateAttribute(qint64 nodeId, const QString &path, const QJsonValue &value);
    void failPending(const QString &reason);
    void setStatus(const QString &status, bool ready = false);
    void scheduleReconnect();
    QString sendRequest(const QString &command, QJsonObject args = {}, int timeoutMs = 30000, bool quiet = false, const Pending *ptzContext = nullptr);
    QString rejected(const QString &command, const QString &message, bool camera = false, int errorCode = 8, bool quiet = false);
    QString rejectCameraPtz(const QString &operation, const QString &message);
    MatterDevice currentCameraPtz(const MatterDevice &device) const;
    void finishCameraPtz(const QString &id, const Pending &pending, const QJsonObject &response);
    void invalidateCameraPtz(const QString &key);
    void trackCameraPtzRevision(const QJsonObject &before, const QJsonObject &after);
    QString rejectCameraMetadata(const QString &detail);
    MatterDevice currentCameraMetadata(const MatterDevice &device) const;
    void finishCameraMetadata(const QString &id, const Pending &pending, const QJsonObject &response);
    CameraMetadataManager *m_metadata = nullptr;
    QString sendDeviceCommand(const MatterDevice &device, quint32 cluster, const QString &name, QJsonObject payload);
    void simulateCommand(const QString &id, const QString &command, const QJsonObject &args);
    QString sanitized(QString text) const;
    QJsonValue sanitizedCameraValue(const QJsonValue &value, const QString &field = {}) const;
    QWebSocket *m_socket = nullptr;
    QTimer m_reconnect;
    QTimer m_handshake;
    QTimer m_ping;
    QTimer m_pongDeadline;
    QUrl m_url;
    QJsonArray m_nodes;
    QJsonObject m_info;
    QHash<QString, Pending> m_pending;
    QStringList m_secrets;
    QString m_ptzRequest;
    QSet<QString> m_ptzInvalidated;
    QElapsedTimer m_ptzElapsed;
    QTimer m_ptzCooldown;
    quint64 m_ptzEpoch = 0;
    QHash<QString, quint64> m_ptzRevisions;
    QHash<QString, quint64> m_metadataRevisions;
    QString m_status = QStringLiteral("尚未连接");
    quint64 m_sequence = 0;
    quint64 m_generation = 0;
    int m_retries = 0;
    bool m_demo = false;
    bool m_ready = false;
    bool m_wantConnection = false;
};
