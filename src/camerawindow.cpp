#include "camerawindow.h"
#include "cameraptzcontrols.h"
#include "matterclient.h"
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QSet>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QWebEnginePermission>
#endif

namespace {
const QUrl PlayerUrl(QStringLiteral("qrc:///matterdesk/camera/player.html"));
QJsonArray releasableStreams(const QJsonValue &result) {
    QJsonArray streams;
    const auto object = result.toObject();
    for (const auto &kind : {QStringLiteral("video"), QStringLiteral("audio")}) {
        const auto track = object.value(kind).toObject();
        if (QStringList{"allocated", "reused"}.contains(track.value("provenance").toString()) && track.value("stream_id").isDouble())
            streams.append(QJsonObject{{"kind", kind}, {"stream_id", track.value("stream_id")}});
    }
    return streams;
}
class CameraPage final : public QWebEnginePage {
public:
    CameraPage(QWebEngineProfile *profile, QObject *parent) : QWebEnginePage(profile, parent) {}
protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType, bool mainFrame) override {
        return mainFrame && url == PlayerUrl;
    }
    QWebEnginePage *createWindow(WebWindowType) override { return nullptr; }
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString &, int, const QString &) override {
        // SDP, ICE and browser error objects must never spill into the application log.
    }
};

// Survives the dialog, so closing a window during camera_start_stream can still
// cancel its pending start and release controller-owned streams. Reused streams
// can be released only after the last viewer; the device enforces reference counts.
class CameraCleanup final : public QObject {
public:
    CameraCleanup(MatterClient *client, QJsonObject base, QString key, QString starting,
                  bool startSettled, QJsonArray streams)
        : QObject(client), m_client(client), m_base(std::move(base)), m_start(std::move(starting)),
          m_streams(std::move(streams)), m_startSettled(startSettled) {
        connect(client, &MatterClient::cameraResponse, this,
                [this](const QString &id, const QString &, bool ok, const QJsonValue &result, const QJsonObject &) {
            if (id == m_start) { m_startSettled = true; if (ok) m_streams = releasableStreams(result); }
            if (id == m_stop) { m_stopSettled = true; m_stopSucceeded = ok; }
            m_releasing.remove(id);
            continueCleanup();
        });
        connect(client, &MatterClient::connectionChanged, this, [this] {
            if (!m_client || !m_client->isConnected()) deleteLater();
        });
        auto args = m_base; args.insert("client_session_key", key);
        m_stop = client->cameraRequest("camera_stop_stream", args, 15000, true);
        QTimer::singleShot(20000, this, &QObject::deleteLater);
    }
private:
    void continueCleanup() {
        if (!m_startSettled || !m_stopSettled) return;
        if (!m_stopSucceeded || !m_client || !m_client->isConnected()) { deleteLater(); return; }
        if (!m_released) {
            m_released = true;
            for (const auto &stream : m_streams) {
                auto args = m_base;
                const auto ref = stream.toObject();
                args.insert("kind", ref.value("kind")); args.insert("stream_id", ref.value("stream_id"));
                m_releasing.insert(m_client->cameraRequest("camera_release_stream", args, 10000, true));
            }
        }
        if (m_releasing.isEmpty()) deleteLater();
    }
    QPointer<MatterClient> m_client;
    QJsonObject m_base;
    QString m_start, m_stop;
    QJsonArray m_streams;
    QSet<QString> m_releasing;
    bool m_startSettled, m_stopSettled = false, m_stopSucceeded = false, m_released = false;
};
}

CameraBridge::CameraBridge(MatterClient *client, MatterDevice device, CameraWindow *owner)
    : QObject(owner), m_client(client), m_owner(owner), m_device(std::move(device)) {
    connect(client, &MatterClient::cameraResponse, this,
            [this](const QString &id, const QString &command, bool success, const QJsonValue &result, const QJsonObject &error) {
        if (!m_requests.contains(id)) return;
        const auto pending = m_requests.take(id);
        if (id == m_startRequest) { m_startSettled = true; if (success) m_allocated = releasableStreams(result); }
        // QWebChannel does not reliably unwrap nested QJsonValue objects. All
        // camera results are objects or null; void results use an empty object.
        emit response(pending.token, command, success, result.toObject(), error);
    });
    connect(client, &MatterClient::cameraEvent, this, [this](const QString &name, const QJsonObject &data) {
        if (data.value("node_id").toInteger(-1) == m_device.nodeId && data.value("endpoint_id").toInt(-1) == m_device.endpointId)
            emit serverEvent(name, data);
    });
    connect(client, &MatterClient::connectionChanged, this, [this] {
        const bool connected = m_client && m_client->isConnected() && !m_client->isDemoMode();
        if (!connected) { m_sessionKey.clear(); m_allocated = {}; }
        emit controllerState(connected);
    });
    connect(client, &MatterClient::devicesChanged, this, [this] {
        bool available = false;
        if (m_client) for (const auto &device : m_client->devices())
            if (device.key == m_device.key && device.available && device.hasCamera) available = true;
        if (!available) emit action("stop", false);
    });
}
void CameraBridge::ready() {
    emit configured(QJsonObject{{"name", m_device.name}, {"node_id", m_device.nodeId}, {"endpoint_id", m_device.endpointId}});
    emit controllerState(m_client && m_client->isConnected() && !m_client->isDemoMode());
    if (m_owner) m_owner->playerReady();
}
void CameraBridge::request(QString token, QString command, QJsonObject args) {
    if (!m_client || token.isEmpty() || token.size() > 128) return;
    args.insert("node_id", m_device.nodeId); args.insert("endpoint_id", m_device.endpointId);
    const auto id = m_client->cameraRequest(command, args, command == "camera_start_stream" ? 45000 : 15000, true);
    m_requests.insert(id, {token, command});
    if (command == "camera_start_stream") {
        m_sessionKey = args.value("client_session_key").toString(); m_startRequest = id;
        m_startSettled = false; m_allocated = {};
    }
}
void CameraBridge::cancelSession() {
    if (m_client && m_client->isConnected() && !m_sessionKey.isEmpty()) {
        new CameraCleanup(m_client, QJsonObject{{"node_id", m_device.nodeId}, {"endpoint_id", m_device.endpointId}},
                          m_sessionKey, m_startRequest, m_startSettled, m_allocated);
    }
    m_sessionKey.clear();
}
void CameraBridge::retired(QString key) {
    // A stop acknowledgement does not finish the following stream releases.
    // Keep native cleanup available until JS finishes them, and ignore old
    // attempts whose completion arrives after a newer start.
    if (key.isEmpty() || key != m_sessionKey) return;
    m_sessionKey.clear(); m_allocated = {};
}
void CameraBridge::playerState(QString state, QString detail, bool talkAvailable, bool micActive, bool audioAvailable) {
    if (m_owner) m_owner->setPlayerState(state, detail.left(400), talkAvailable, micActive, audioAvailable);
}
void CameraBridge::microphoneState(QString state) { if (m_owner) m_owner->setMicrophoneState(state); }

CameraWindow::CameraWindow(MatterClient *client, const MatterDevice &device, QWidget *parent)
    : QDialog(parent), m_client(client), m_device(device) {
    m_dialogFlags = windowFlags();
    setWindowTitle(device.name + " · Matter 摄像头"); resize(1060, 740); setMinimumSize(700, 500);
    auto *layout = new QVBoxLayout(this); m_layout = layout; layout->setContentsMargins(22, 20, 22, 20); layout->setSpacing(13);
    m_title = new QLabel(device.name); m_title->setTextFormat(Qt::PlainText); m_title->setObjectName("emptyTitle"); layout->addWidget(m_title);
    m_origin = new QLabel(QString("Matter 摄像头 · 节点 %1 / 端点 %2 · H.264 / Opus").arg(device.nodeId).arg(device.endpointId));
    m_origin->setTextFormat(Qt::PlainText); m_origin->setObjectName("cameraOrigin"); layout->addWidget(m_origin);
    m_profile = new QWebEngineProfile(this);
    m_profile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
    m_view = new QWebEngineView(this); m_view->setObjectName("cameraVideo");
    // Own the page directly. QWebEngineView can defer deletion of a page while
    // detaching its render adapter, so relying on view teardown races its profile.
    auto *page = new CameraPage(m_profile, this); m_page = page; m_view->setPage(page);
    page->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    // Playback is still started only by the native Start/Sound buttons. A native
    // button action is not a Chromium DOM user gesture, so permit that playback.
    page->settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
    auto *channel = new QWebChannel(page); m_bridge = new CameraBridge(client, device, this);
    channel->registerObject("cameraBridge", m_bridge); page->setWebChannel(channel);
    auto authorizeMicrophone = [this, page](const QUrl &origin, bool requestedAudio) {
        QPointer<QWebEnginePage> guardedPage(page); QPointer<CameraWindow> self(this);
        bool grant = false;
        if (requestedAudio && m_microphoneRequested && !m_closeQueued && page->url() == PlayerUrl && origin.scheme() == "qrc") {
            grant = QMessageBox::question(this, "麦克风权限", "允许 MatterDesk 使用本机麦克风向这台摄像头对讲？\n停止对讲或关闭窗口会立即释放麦克风。", QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
        }
        return self && guardedPage && grant && self->m_microphoneRequested && !self->m_closeQueued;
    };
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    connect(page, &QWebEnginePage::permissionRequested, this, [authorizeMicrophone](QWebEnginePermission permission) {
        const bool grant = authorizeMicrophone(permission.origin(), permission.permissionType() == QWebEnginePermission::PermissionType::MediaAudioCapture);
        if (!permission.isValid()) return;
        if (grant) permission.grant(); else permission.deny();
    });
#else
    connect(page, &QWebEnginePage::featurePermissionRequested, this,
            [page, authorizeMicrophone](const QUrl &origin, QWebEnginePage::Feature feature) {
        QPointer<QWebEnginePage> guardedPage(page);
        const bool grant = authorizeMicrophone(origin, feature == QWebEnginePage::MediaAudioCapture);
        if (!guardedPage) return;
        guardedPage->setFeaturePermission(origin, feature, grant
                                  ? QWebEnginePage::PermissionGrantedByUser : QWebEnginePage::PermissionDeniedByUser);
    });
#endif
    connect(m_view, &QWebEngineView::loadFinished, this, [this](bool ok) {
        if (!ok) setPlayerState("error", "本地摄像头播放器加载失败。", false, false, false);
    });
    layout->addWidget(m_view, 1);
    m_controls = new QWidget(this); m_controls->setObjectName("cameraOverlay");
    m_controlsLayout = new QVBoxLayout(m_controls); m_controlsLayout->setContentsMargins(0, 0, 0, 0); m_controlsLayout->setSpacing(13);
    auto *controls = new QHBoxLayout;
    m_start = new QPushButton("开始直播"); m_start->setObjectName("primary");
    m_start->setProperty("cameraAction", "start");
    m_stop = new QPushButton("停止"); m_stop->setObjectName("secondary");
    m_stop->setProperty("cameraAction", "stop");
    m_sound = new QPushButton("收听声音"); m_sound->setCheckable(true); m_sound->setObjectName("secondary");
    m_sound->setProperty("cameraAction", "sound");
    m_talk = new QPushButton("开启对讲"); m_talk->setCheckable(true); m_talk->setObjectName("secondary");
    m_talk->setProperty("cameraAction", "talk");
    controls->addWidget(m_start); controls->addWidget(m_stop); controls->addStretch(); controls->addWidget(m_sound); controls->addWidget(m_talk); m_controlsLayout->addLayout(controls);
    m_ptz = new CameraPtzControls(client, device, m_controls);
    m_controlsLayout->addWidget(m_ptz);
    connect(m_ptz, &CameraPtzControls::presentationChanged, this, &CameraWindow::positionOverlay);
    m_status = new QLabel("正在加载本地播放器…"); m_status->setTextFormat(Qt::PlainText); m_status->setWordWrap(true); m_status->setObjectName("muted"); m_controlsLayout->addWidget(m_status);
    m_status->setAccessibleName("cameraStatus");
    m_microphone = new QLabel("麦克风关闭 · 只有点击「开启对讲」并授权后才会采集声音"); m_microphone->setObjectName("mutedSmall"); m_controlsLayout->addWidget(m_microphone);
    layout->addWidget(m_controls);
    connect(m_start, &QPushButton::clicked, this, [this] { emit m_bridge->action("start", true); });
    connect(m_stop, &QPushButton::clicked, this, [this] {
        if (m_compact || m_embedded) { close(); return; }
        m_microphoneRequested = false; emit m_bridge->action("stop", false);
    });
    connect(m_sound, &QPushButton::toggled, this, [this](bool enabled) { m_sound->setText(enabled ? "关闭声音" : "收听声音"); emit m_bridge->action("sound", enabled); });
    connect(m_talk, &QPushButton::toggled, this, [this](bool enabled) {
        m_microphoneRequested = enabled; emit m_bridge->action("talk", enabled); updateControls();
    });
    connect(client, &MatterClient::connectionChanged, this, &CameraWindow::updateControls);
    connect(client, &MatterClient::devicesChanged, this, &CameraWindow::updateControls);
    m_view->setUrl(PlayerUrl); updateControls();
}
CameraWindow::~CameraWindow() {
    m_ptz->setClosing();
    m_destroying = true; m_playPending = false; m_microphoneRequested = false; m_bridge->cancelSession();
    // Destroy the page before its off-the-record profile. Chromium releases any
    // outstanding permission request and capture even after an abnormal close.
    m_view->setPage(nullptr);
    delete m_page; m_page = nullptr;
    delete m_view; m_view = nullptr;
    delete m_profile; m_profile = nullptr;
}
void CameraWindow::play() {
    if (m_destroying || m_closeQueued || m_playAttempted) return;
    m_playPending = true;
    if (!m_ready) return;
    m_playPending = false; m_playAttempted = true;
    updateControls();
    if (m_start->isEnabled()) m_start->click();
}
void CameraWindow::setCompactMode(bool compact) {
    m_compact = compact; m_origin->setVisible(!compact && !m_embedded);
    m_start->setText(compact ? QStringLiteral("重试") : QStringLiteral("开始直播"));
    m_stop->setText(m_embedded ? QStringLiteral("快照") : compact ? QStringLiteral("返回") : QStringLiteral("停止"));
    if (m_embedded) setMinimumSize(0, 0);
    else if (compact) { setMinimumSize(560, 400); resize(760, 560); }
    else { setMinimumSize(700, 500); resize(1060, 740); }
    updateControls();
}
void CameraWindow::setEmbeddedMode(bool embedded) {
    if (m_embedded == embedded) return;
    m_embedded = embedded;
    // QDialog normally remains a separate window even when it has a parent.
    // Retain its close/escape lifecycle while making the Mini player a child.
    setWindowFlags(embedded ? Qt::WindowFlags(Qt::Widget) : m_dialogFlags);
    QWidget *widgets[] = {m_title, m_origin, m_view, m_controls};
    for (QWidget *widget : widgets)
        m_layout->removeWidget(widget);
    m_microphone->setWordWrap(embedded);
    m_title->setVisible(!embedded); m_origin->setVisible(!embedded && !m_compact);
    if (embedded) {
        setMinimumSize(0, 0);
        m_layout->setContentsMargins(0, 0, 0, 0); m_layout->setSpacing(0);
        m_layout->addWidget(m_view);
        m_controlsLayout->setContentsMargins(10, 9, 10, 9); m_controlsLayout->setSpacing(5);
        m_controls->setStyleSheet(QStringLiteral(
            "QWidget#cameraOverlay { background:rgba(12,22,19,205); border-radius:10px; }"
            "QWidget#cameraOverlay QLabel { color:#e2eae4; background:transparent; font-size:11px; }"
            "QWidget#cameraOverlay QPushButton { color:#eef4ef; background:rgba(255,255,255,26); border:1px solid rgba(255,255,255,54); border-radius:7px; padding:7px 9px; font-size:12px; }"
            "QWidget#cameraOverlay QPushButton:hover { background:rgba(255,255,255,45); }"
            "QWidget#cameraOverlay QPushButton:checked { background:#397959; }"
            "QWidget#cameraOverlay QPushButton:disabled { color:#87948c; background:rgba(255,255,255,10); border-color:rgba(255,255,255,18); }"));
    } else {
        m_layout->setContentsMargins(22, 20, 22, 20); m_layout->setSpacing(13);
        m_controlsLayout->setContentsMargins(0, 0, 0, 0); m_controlsLayout->setSpacing(13);
        m_controls->setStyleSheet({});
        m_layout->addWidget(m_title); m_layout->addWidget(m_origin); m_layout->addWidget(m_view, 1); m_layout->addWidget(m_controls);
    }
    setCompactMode(m_compact);
    m_view->show(); m_controls->show();
    updatePresentation(); positionOverlay();
}
void CameraWindow::setVideoFit(bool fit) {
    m_videoFit = fit;
    updatePresentation();
}
void CameraWindow::setOverlayVisible(bool visible) {
    if (m_embedded) m_controls->setVisible(visible);
}
void CameraWindow::updatePresentation() {
    if (!m_ready || m_destroying || m_closeQueued) return;
    // This presentation-only switch never accepts device strings or media data.
    m_page->runJavaScript(QStringLiteral("window.MatterDeskCamera && window.MatterDeskCamera.setPresentation({embedded:%1,fit:%2})")
        .arg(m_embedded ? QStringLiteral("true") : QStringLiteral("false"),
             m_videoFit ? QStringLiteral("true") : QStringLiteral("false")));
}
void CameraWindow::positionOverlay() {
    if (!m_embedded || !m_controls) return;
    const int overlayWidth = qMax(0, width() - 16);
    m_controlsLayout->invalidate();
    const int preferredHeight = m_controlsLayout->totalHeightForWidth(overlayWidth);
    const int overlayHeight = qMin(qMax(0, height() - 16), preferredHeight >= 0 ? preferredHeight : m_controls->sizeHint().height());
    m_controls->setGeometry(8, height() - overlayHeight - 8, overlayWidth, overlayHeight);
    m_controls->raise();
}
void CameraWindow::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    positionOverlay();
}
void CameraWindow::playerReady() {
    m_ready = true; updatePresentation(); updateControls();
    if (m_playPending) play();
}
void CameraWindow::setPlayerState(const QString &state, const QString &detail, bool talkAvailable, bool micActive, bool audioAvailable) {
    m_state = state; m_talkAvailable = talkAvailable; m_audioAvailable = audioAvailable; m_microphoneActive = micActive;
    m_status->setText(detail);
    if (state == "idle" || state == "error" || state == "disconnected") {
        m_microphoneRequested = false; m_microphoneActive = false;
    }
    updateControls();
}
void CameraWindow::setMicrophoneState(const QString &state) {
    m_microphoneRequested = state == "requesting" || state == "active";
    m_microphoneActive = state == "active";
    if (state == "denied") m_status->setText("麦克风未授权或不可用，视频和收听仍可继续。");
    updateControls();
}
void CameraWindow::updateControls() {
    const bool connected = m_client && m_client->isConnected() && !m_client->isDemoMode();
    bool available = false;
    if (m_client) for (const auto &device : m_client->devices())
        if (device.key == m_device.key && device.available && device.hasCamera) available = true;
    const bool active = m_state == "connecting" || m_state == "streaming" || m_state == "stopping";
    m_start->setVisible(!m_compact || m_state == "error");
    m_start->setEnabled(m_ready && connected && available && !active && !m_closeQueued);
    m_stop->setEnabled(m_compact || m_embedded ? !m_closeQueued : active && m_state != "stopping" && !m_closeQueued);
    m_sound->setEnabled(connected && m_state == "streaming" && m_audioAvailable && !m_closeQueued);
    m_talk->setEnabled(connected && m_state == "streaming" && m_talkAvailable && !m_closeQueued);
    QSignalBlocker block(m_talk); m_talk->setChecked(m_microphoneRequested || m_microphoneActive);
    m_talk->setText(m_microphoneRequested || m_microphoneActive ? "结束对讲" : "开启对讲");
    m_microphone->setText(m_microphoneActive ? "● 麦克风正在对讲 · 点击「结束对讲」即可停止" : m_microphoneRequested ? "正在请求麦克风权限…" : "麦克风关闭 · 只有点击「开启对讲」并授权后才会采集声音");
    m_microphone->setVisible(!m_embedded || m_microphoneRequested || m_microphoneActive);
    m_microphone->setStyleSheet(m_microphoneActive ? (m_embedded ? "color:#ffc59d;" : "color:#a34d31;") : "");
    positionOverlay();
}
void CameraWindow::closeEvent(QCloseEvent *event) {
    if (m_closeReady) { QDialog::closeEvent(event); return; }
    event->ignore();
    if (m_closeQueued) return;
    m_ptz->setClosing();
    m_closeQueued = true; m_playPending = false; m_microphoneRequested = false; m_bridge->cancelSession(); updateControls();
    QPointer<CameraWindow> self(this);
    m_view->page()->runJavaScript("window.MatterDeskCamera && window.MatterDeskCamera.dispose()", [self](const QVariant &) {
        if (self && !self->m_destroying) { self->m_closeReady = true; self->close(); }
    });
    QTimer::singleShot(700, this, [self] { if (self && !self->m_destroying) { self->m_closeReady = true; self->close(); } });
}
void CameraWindow::reject() {
    // Escape normally hides a QDialog directly. Route it through the same media
    // teardown as the title-bar close button instead of leaving hidden capture.
    if (m_closeReady) QDialog::reject();
    else close();
}
