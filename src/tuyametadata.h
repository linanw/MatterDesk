#pragma once

#include "matterdevice.h"
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <memory>

class MatterClient;
struct TuyaMetadataState {
    bool bound = false, known = false, available = false, busy = false, conflict = false;
    bool canRename = false, canMoveRoom = false, canRenameRoom = false, hasBaseline = false;
    QString fullName, managedName, homeName, roomName, status, observedAt;
    QString deviceId, roomId, lastError;
};

// This independent mirror never writes HomeStore, aliases, or Matter attributes.
QString tuyaMatterUniqueId(const MatterDevice &device);
bool parseTuyaMetadataSnapshot(const QJsonValue &value, QJsonObject *result, QString *error = nullptr);

class TuyaMetadataManager : public QObject {
    Q_OBJECT
public:
    explicit TuyaMetadataManager(MatterClient *client, QObject *parent = nullptr);
    TuyaMetadataManager(MatterClient *client, const QString &storageDirectory, QObject *parent = nullptr);
    ~TuyaMetadataManager() override;
    void setProfile(const QString &controllerScope);
    QString profile() const;
    bool configure(const QUrl &url, const QString &sessionToken);
    QUrl baseUrl() const;
    bool isConfigured() const;
    bool enabled() const;
    bool setEnabled(bool enabled);
    QString mode() const;
    bool setMode(const QString &mode);
    bool refresh();
    QJsonObject snapshot() const;
    QString statusText() const;
    bool requestInProgress() const;
    bool hasUncertainWrite() const;
    QList<MatterDevice> devices() const;
    TuyaMetadataState state(const MatterDevice &device) const;
    QString displayName(const MatterDevice &device) const;
    QString roomName(const MatterDevice &device) const;
    QString suggestedBinding(const MatterDevice &device) const;
    bool bindDevice(const MatterDevice &device, const QString &sourceDeviceId);
    bool followSource(const MatterDevice &device);
    bool rename(const MatterDevice &device, const QString &name);
    bool moveRoom(const MatterDevice &device, const QString &sourceRoomId);
    bool renameRoom(const QString &sourceRoomId, const QString &name);
    bool acknowledgeUncertain();
    bool setSourceHome(const QString &id);
    QString sourceHomeId() const;
    bool sourceHomeConfigured() const;
    bool resetSource();
signals:
    void changed();
    void deviceChanged(QString deviceKey);
    void statusChanged(QString status);
    void operationFinished(QString deviceKey, QString operation, bool success, QString detail);
private:
    struct Private;
    std::unique_ptr<Private> d;
};
