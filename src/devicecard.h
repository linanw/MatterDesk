#pragma once
#include "matterdevice.h"
#include <QFrame>
#include <QDateTime>
#include <QImage>
class QLabel;
class QPushButton;
class QSlider;
class QTimer;
class SnapshotPreview;

class Symbol : public QWidget {
public:
    explicit Symbol(QString kind, QWidget *parent = nullptr);
    void setActive(bool active);
    void setKind(const QString &kind);
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QString m_kind;
    bool m_active = false;
};

class DeviceCard : public QFrame {
    Q_OBJECT
public:
    explicit DeviceCard(const MatterDevice &device, QWidget *parent = nullptr);
    void updateDevice(const MatterDevice &device, bool connected);
    void setHomePresentation(const QString &room, bool favorite);
    void setSnapshot(const QImage &image, const QDateTime &capturedAt, const QString &message, bool loading, bool stale);
    MatterDevice device() const { return m_device; }
signals:
    void powerRequested(MatterDevice device, bool on);
    void levelRequested(MatterDevice device, int percent);
    void temperatureRequested(MatterDevice device, int mired);
    void detailsRequested(MatterDevice device);
    void cameraRequested(MatterDevice device);
    void snapshotRequested(MatterDevice device);
    void favoriteRequested(MatterDevice device, bool favorite);
protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override;
private:
    void refreshSnapshotState();
    MatterDevice m_device;
    Symbol *m_icon;
    QLabel *m_status, *m_name, *m_origin, *m_readings, *m_levelLabel, *m_temperatureLabel;
    QPushButton *m_power, *m_camera, *m_favorite, *m_moreControls;
    QSlider *m_level, *m_temperature;
    QTimer *m_levelTimer, *m_temperatureTimer;
    QWidget *m_snapshotPanel;
    SnapshotPreview *m_snapshotPreview;
    QLabel *m_snapshotTime, *m_snapshotStatus;
    QPushButton *m_snapshotRefresh;
    QTimer *m_snapshotClock;
    QDateTime m_snapshotCapturedAt;
    QString m_snapshotMessage;
    bool m_snapshotLoading = false, m_snapshotStale = false;
    bool m_connected = false;
    bool m_home = false;
    QString m_room;
};
