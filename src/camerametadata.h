#pragma once

#include "matterclient.h"
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

struct CameraMetadataSnapshot {
    QString identity, sourceEpoch, name, homeKey, homeName, roomId, roomName, observedAt;
    quint64 revision = 0;
    bool operator==(const CameraMetadataSnapshot &other) const;
};
struct CameraMetadataRoom {
    QString id, name;
    bool operator==(const CameraMetadataRoom &other) const { return id == other.id && name == other.name; }
};
struct CameraMetadataCapabilities {
    bool read = false, rename = false, move = false, syncConfig = false;
};
struct CameraMetadataState {
    CameraMetadataSnapshot snapshot;
    QList<CameraMetadataRoom> rooms;
    CameraMetadataCapabilities capabilities;
    bool discovered = false, known = false, stale = true, loading = false, busy = false;
    bool available = false, syncEnabled = false, confirmed = false, conflict = false;
    QString status, lastError, operation;
};

// The parser accepts the verified OHF TagBased numeric struct fields. A malformed
// snapshot never partially mutates the caller's value.
bool parseCameraMetadataSnapshot(const QJsonValue &, CameraMetadataSnapshot *, QString *error = nullptr);
bool parseCameraMetadataRooms(const QJsonValue &, QList<CameraMetadataRoom> *, QString *error = nullptr);
bool parseCameraMetadataCapabilities(const QJsonValue &, CameraMetadataCapabilities *, QString *error = nullptr);
bool hasCameraMetadata(const MatterDevice &);
// A selected source may be known before its first cloud observation is ready.
bool cameraMetadataUnavailable(const CameraMetadataState &);

class CameraMetadataManager : public QObject {
    Q_OBJECT
public:
    explicit CameraMetadataManager(MatterClient *client, QObject *parent = nullptr);
    CameraMetadataManager(MatterClient *client, const QString &storageDirectory, QObject *parent = nullptr);
    CameraMetadataState state(const MatterDevice &device) const;
    QString profile() const { return m_profile; }
    void setProfile(const QString &profile);
    bool read(const MatterDevice &device);
    static bool supports(const MatterDevice &device) { return hasCameraMetadata(device); }
    bool refresh(const MatterDevice &device);
    bool refresh(const MatterDevice &device, const CameraMetadataSnapshot &expected);
    bool setSync(const MatterDevice &device, bool enabled);
    bool setSync(const MatterDevice &device, bool enabled, const CameraMetadataSnapshot &expected);
    bool rename(const MatterDevice &device, const QString &name);
    bool rename(const MatterDevice &device, const QString &name, const CameraMetadataSnapshot &expected);
    bool moveRoom(const MatterDevice &device, const QString &roomId);
    bool moveRoom(const MatterDevice &device, const QString &roomId, const CameraMetadataSnapshot &expected);
    bool canRead(const MatterDevice &device) const;
    bool canRename(const MatterDevice &device) const;
    bool canMoveRoom(const MatterDevice &device) const;
    bool canSetSync(const MatterDevice &device) const;
signals:
    void changed(QString deviceKey);
    // For commands success means authoritative readback matched the requested
    // target. null command acknowledgements are never reported as success.
    void operationFinished(QString deviceKey, QString operation, bool success, QString detail);
private:
    struct Entry {
        CameraMetadataState state;
        MatterDevice device;
        QJsonObject observedAttributes;
        quint64 generation = 0;
        bool needsRead = false;
    };
    struct Pending {
        QString key, id, operation, stage, profile;
        MatterDevice device;
        quint64 generation = 0;
        CameraMetadataSnapshot before, baseline;
        QJsonObject middle;
        QJsonValue target;
        int confirmReads = 0;
    };
    bool startRead(const MatterDevice &device, const QString &operation = {});
    bool startCommand(const MatterDevice &device, const QString &operation, const QJsonValue &target = {}, const CameraMetadataSnapshot *expected = nullptr);
    bool usable(const MatterDevice &device) const;
    void requestStage(const QString &stage);
    void response(const QString &id, bool success, const QJsonValue &result, const QJsonObject &error);
    void finish(bool success, const QString &detail, bool conflict = false);
    void observe();
    void invalidate(const QString &detail);
    bool publish(const CameraMetadataSnapshot &after, const QJsonObject &middle, QString *error);
    void persist();
    void load();
    MatterClient *m_client;
    QString m_directory, m_profile, m_path;
    QHash<QString, Entry> m_entries;
    QJsonObject m_mirrors;
    Pending m_pending;
    QTimer m_confirmTimer;
    quint64 m_generation = 0;
    bool m_observing = false;
};
