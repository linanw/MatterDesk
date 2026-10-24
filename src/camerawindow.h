#pragma once
#include "matterdevice.h"
#include <QDialog>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QPointer>

class MatterClient;
class QWebEngineView;
class QWebEnginePage;
class QWebEngineProfile;
class QLabel;
class QPushButton;
class QCloseEvent;
class QResizeEvent;
class QVBoxLayout;
class CameraWindow;
class CameraPtzControls;

class CameraBridge : public QObject {
    Q_OBJECT
public:
    CameraBridge(MatterClient *client, MatterDevice device, CameraWindow *owner);
    void cancelSession();
    Q_INVOKABLE void ready();
    Q_INVOKABLE void request(QString token, QString command, QJsonObject args);
    Q_INVOKABLE void retired(QString key);
    Q_INVOKABLE void playerState(QString state, QString detail, bool talkAvailable, bool micActive, bool audioAvailable);
    Q_INVOKABLE void microphoneState(QString state);
signals:
    void configured(QJsonObject config);
    void response(QString token, QString command, bool success, QJsonObject result, QJsonObject error);
    void serverEvent(QString name, QJsonObject data);
    void action(QString name, bool enabled);
    void controllerState(bool connected);
private:
    struct Request { QString token; QString command; };
    QPointer<MatterClient> m_client;
    QPointer<CameraWindow> m_owner;
    MatterDevice m_device;
    QMap<QString, Request> m_requests;
    QString m_sessionKey, m_startRequest;
    QJsonArray m_allocated;
    bool m_startSettled = true;
};

class CameraWindow : public QDialog {
    Q_OBJECT
public:
    explicit CameraWindow(MatterClient *client, const MatterDevice &device, QWidget *parent = nullptr);
    ~CameraWindow() override;
    // Called only after a native user action; each window consumes this once.
    void play();
    void setCompactMode(bool compact);
    // Configure while hidden, before adding this player to an in-window stack.
    void setEmbeddedMode(bool embedded = true);
    // Fit the entire frame inside an embedded panel; Mini keeps aspect-fill by default.
    void setVideoFit(bool fit);
    void setOverlayVisible(bool visible);
    void setPlayerState(const QString &state, const QString &detail, bool talkAvailable, bool micActive, bool audioAvailable);
    void setMicrophoneState(const QString &state);
    void playerReady();
    void reject() override;
protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
private:
    void updateControls();
    void positionOverlay();
    void updatePresentation();
    QPointer<MatterClient> m_client;
    MatterDevice m_device;
    CameraBridge *m_bridge;
    CameraPtzControls *m_ptz;
    QWebEngineView *m_view;
    QWebEnginePage *m_page;
    QWebEngineProfile *m_profile;
    QLabel *m_status, *m_microphone, *m_origin, *m_title;
    QVBoxLayout *m_layout, *m_controlsLayout;
    QWidget *m_controls;
    QPushButton *m_start, *m_stop, *m_sound, *m_talk;
    QString m_state = "loading";
    bool m_ready = false, m_talkAvailable = false, m_audioAvailable = false;
    bool m_microphoneRequested = false, m_microphoneActive = false;
    bool m_closeQueued = false, m_closeReady = false, m_destroying = false;
    bool m_playPending = false, m_playAttempted = false, m_compact = false, m_embedded = false, m_videoFit = false;
    Qt::WindowFlags m_dialogFlags;
};
