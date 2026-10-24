#pragma once

#include "matterdevice.h"

#include <QDateTime>
#include <QImage>
#include <QObject>
#include <memory>

class MatterClient;

struct SnapshotState {
    QImage image;
    QDateTime capturedAt;
    QString message;
    bool loading = false;
    bool stale = false;
};

class CameraSnapshots : public QObject {
    Q_OBJECT
public:
    explicit CameraSnapshots(MatterClient *client, QObject *parent = nullptr);
    ~CameraSnapshots() override;
    void setVisibleDevices(QList<MatterDevice> devices);
    void setActive(bool active);
    void refresh(QString key);
    SnapshotState state(QString key) const;
signals:
    void changed(QString key);
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
