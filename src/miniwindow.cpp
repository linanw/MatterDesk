#include "miniwindow.h"
#include "camerasnapshots.h"
#include "camerametadata.h"
#include "camerametadatapanel.h"
#include "camerawindow.h"
#include "cameraptzcontrols.h"
#include "snapshotpreview.h"
#include "tuyametadata.h"
#include "tuyametadatapanel.h"

#include <QComboBox>
#include <QApplication>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QPainter>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QScreen>
#include <QShortcut>
#include <QResizeEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <QtMath>

namespace {
class OverlayPanel final : public QWidget {
public:
    explicit OverlayPanel(QWidget *parent) : QWidget(parent) {}
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(QColor(12, 22, 19, 180));
        painter.setPen(QColor(255, 255, 255, 38));
        painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 12, 12);
    }
};
// A quarter of the former 6 logical pixels is 1.5; native widget bounds round to 2.
constexpr int DrawerStrip = 2;
constexpr int DockDistance = 16;
const QSize DefaultWindowSize(800, 450);
QLabel *plainLabel(const QString &text, const char *name, QWidget *parent = nullptr) {
    auto *label = new QLabel(text, parent);
    label->setObjectName(QLatin1String(name));
    label->setTextFormat(Qt::PlainText);
    return label;
}
QString relativeTime(const QDateTime &capturedAt) {
    if (!capturedAt.isValid()) return QStringLiteral("尚无快照");
    const qint64 seconds = qMax<qint64>(0, capturedAt.secsTo(QDateTime::currentDateTimeUtc()));
    if (seconds < 5) return QStringLiteral("刚刚更新");
    if (seconds < 60) return QStringLiteral("%1 秒前更新").arg(seconds);
    if (seconds < 3600) return QStringLiteral("%1 分钟前更新").arg(seconds / 60);
    if (seconds < 86400) return QStringLiteral("%1 小时前更新").arg(seconds / 3600);
    return QStringLiteral("%1 天前更新").arg(seconds / 86400);
}
}

MiniWindow::MiniWindow(QUrl controller, QWidget *parent)
    : QMainWindow(parent), m_controller(std::move(controller)) {
    setObjectName(QStringLiteral("miniWindow"));
    setWindowFlag(Qt::FramelessWindowHint);
    m_alwaysOnTop = QSettings().value(QStringLiteral("mini/alwaysOnTop"), false).toBool();
    setWindowFlag(Qt::WindowStaysOnTopHint, m_alwaysOnTop);
    setWindowTitle(QStringLiteral("MatterCamViewer"));
    resize(DefaultWindowSize); setMinimumSize(400, 280);
    m_preferredKey = QSettings().value(QStringLiteral("mini/selectedCameraKey")).toString();
    m_tuyaMetadataManager = new TuyaMetadataManager(&m_client, this);
    m_tuyaMetadataManager->setObjectName(QStringLiteral("miniTuyaMetadataManager"));

    auto *root = new QWidget(this); root->setObjectName(QStringLiteral("miniRoot"));
    root->setStyleSheet(QStringLiteral("QWidget#miniRoot { background:#10221a; }"));
    auto *layout = new QVBoxLayout(root); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    m_pages = new QStackedWidget(root); m_pages->setObjectName(QStringLiteral("miniPages"));
    m_pages->setFrameShape(QFrame::NoFrame); layout->addWidget(m_pages);
    auto *empty = new QWidget(m_pages); empty->setStyleSheet(QStringLiteral("background:#10221a;"));
    auto *emptyLayout = new QVBoxLayout(empty); emptyLayout->setContentsMargins(24, 80, 24, 80);
    auto *emptyTitle = plainLabel(QString(), "miniEmpty");
    emptyTitle->setAlignment(Qt::AlignCenter); emptyTitle->setWordWrap(true);
    emptyTitle->setStyleSheet(QStringLiteral("color:#afc2b5; font-size:14px; background:transparent;"));
    emptyLayout->addWidget(emptyTitle); m_pages->addWidget(empty);
    m_preview = new SnapshotPreview(m_pages); m_preview->setFillMode(true); m_preview->setEnabled(false);
    m_preview->setAccessibleName(QStringLiteral("摄像头快照，单击显隐控制，双击恢复默认尺寸或全屏"));
    m_pages->addWidget(m_preview);

    const QString overlayStyle = QStringLiteral(
        "QLabel { color:#fff; background:transparent; }"
        "QPushButton { color:white; background:rgba(15,25,19,110); border:1px solid rgba(255,255,255,45); border-radius:17px; padding:0; font-size:18px; }"
        "QPushButton:hover { background:rgba(255,255,255,45); }"
        "QPushButton:checked { color:#b7ffda; background:rgba(40,116,79,180); border-color:rgba(183,255,218,160); }"
        "QPushButton:focus { border:1px solid white; }"
        "QPushButton:disabled { color:rgba(255,255,255,90); background:rgba(15,25,19,55); }"
        "QComboBox { color:white; background:rgba(15,25,19,150); border:1px solid rgba(255,255,255,40); border-radius:6px; padding:5px 9px; }"
        "QComboBox QAbstractItemView { color:white; background:#1b2923; selection-background-color:#3f5d4c; }");
    m_header = new QWidget(root); m_header->setObjectName(QStringLiteral("miniHeader"));
    m_header->setStyleSheet(overlayStyle);
    auto *headerBox = new QVBoxLayout(m_header); headerBox->setContentsMargins(12, 10, 12, 10); headerBox->setSpacing(7);
    auto *header = new QHBoxLayout; header->setSpacing(6);
    auto *heading = new QVBoxLayout; heading->setSpacing(4);
    m_title = plainLabel(QStringLiteral("摄像头"), "miniTitle");
    m_title->setStyleSheet(QStringLiteral("font-size:17px; font-weight:600; color:white;"));
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    heading->addWidget(m_title);
    m_connection = plainLabel(QStringLiteral("正在连接…"), "miniConnection");
    m_connection->setStyleSheet(QStringLiteral("font-size:11px; color:#d3dfd6;"));
    m_metadataLocation = plainLabel(QString(), "miniMijiaLocation"); m_metadataLocation->setWordWrap(true);
    m_metadataLocation->setStyleSheet(QStringLiteral("font-size:11px; color:#d3dfd6;"));
    m_metadataLocation->hide(); heading->addWidget(m_metadataLocation);
    for (QLabel *label : {m_title, m_connection, m_metadataLocation}) {
        auto *shadow = new QGraphicsDropShadowEffect(label);
        shadow->setBlurRadius(7); shadow->setOffset(0, 1); shadow->setColor(QColor(0, 0, 0, 210));
        label->setGraphicsEffect(shadow);
    }
    heading->addWidget(m_connection); header->addLayout(heading, 1);
    m_metadata = new QPushButton(QStringLiteral("米"), m_header); m_metadata->setObjectName(QStringLiteral("miniMijiaMetadata"));
    m_metadata->setFixedSize(34, 34); m_metadata->setAccessibleName(QStringLiteral("米家名称与房间"));
    m_metadata->setToolTip(QStringLiteral("米家名称与房间")); m_metadata->hide(); header->addWidget(m_metadata);
    m_tuyaMetadata = new QPushButton(QStringLiteral("涂"), m_header);
    m_tuyaMetadata->setObjectName(QStringLiteral("miniTuyaMetadata"));
    m_tuyaMetadata->setFixedSize(34, 34);
    m_tuyaMetadata->setAccessibleName(QStringLiteral("涂鸦名称与房间同步"));
    m_tuyaMetadata->setToolTip(QStringLiteral("涂鸦名称与房间同步"));
    m_tuyaMetadata->setCursor(Qt::PointingHandCursor); header->addWidget(m_tuyaMetadata);
    m_add = new QPushButton(QStringLiteral("＋"), m_header); m_add->setObjectName(QStringLiteral("miniAdd")); m_add->setFixedSize(34, 34);
    m_add->setAccessibleName(QStringLiteral("添加 Matter 摄像头")); m_add->setCursor(Qt::PointingHandCursor);
    m_add->setToolTip(QStringLiteral("添加摄像头")); m_add->setEnabled(false); header->addWidget(m_add); headerBox->addLayout(header);
    auto windowButton = [this, header](const QString &text, const char *name, const QString &label) {
        auto *button = new QPushButton(text, m_header); button->setObjectName(QLatin1String(name));
        button->setFixedSize(34, 34); button->setAccessibleName(label); button->setToolTip(label);
        button->setCursor(Qt::PointingHandCursor); header->addWidget(button); return button;
    };
    m_onTop = windowButton(QStringLiteral("钉"), "miniAlwaysOnTop", QStringLiteral("窗口置顶"));
    m_onTop->setCheckable(true); m_onTop->setChecked(m_alwaysOnTop);
    connect(m_onTop, &QPushButton::clicked, this, [this](bool enabled) { setAlwaysOnTop(enabled); });
    m_fullScreen = windowButton(QStringLiteral("⛶"), "miniFullScreen", QStringLiteral("全屏（F11）"));
    m_fullScreen->setCheckable(true);
    auto *minimize = windowButton(QStringLiteral("−"), "miniMinimize", QStringLiteral("最小化"));
    auto *close = windowButton(QStringLiteral("×"), "miniClose", QStringLiteral("关闭窗口"));
    connect(m_fullScreen, &QPushButton::clicked, this, &MiniWindow::toggleFullScreen);
    connect(minimize, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    m_selector = new QComboBox(m_header); m_selector->setObjectName(QStringLiteral("miniCameraSelector"));
    m_selector->setAccessibleName(QStringLiteral("选择摄像头")); m_selector->hide(); headerBox->addWidget(m_selector);

    m_footer = new OverlayPanel(root); m_footer->setObjectName(QStringLiteral("miniFooter")); m_footer->setStyleSheet(overlayStyle);
    auto *footerBox = new QVBoxLayout(m_footer); footerBox->setContentsMargins(12, 10, 12, 10); footerBox->setSpacing(4);
    auto *snapshotRow = new QHBoxLayout; snapshotRow->setSpacing(8);
    m_snapshotTime = plainLabel(QStringLiteral("尚无快照"), "miniSnapshotTime");
    m_snapshotTime->setStyleSheet(QStringLiteral("color:#e0e9e2; font-size:11px;"));
    snapshotRow->addWidget(m_snapshotTime, 1);
    m_play = new QPushButton(QStringLiteral("▶ 直播"), m_footer); m_play->setObjectName(QStringLiteral("miniPlay"));
    m_play->setFixedSize(84, 34); m_play->setStyleSheet(QStringLiteral("font-size:13px;"));
    m_play->setAccessibleName(QStringLiteral("播放实时画面")); m_play->setCursor(Qt::PointingHandCursor);
    snapshotRow->addWidget(m_play); connect(m_play, &QPushButton::clicked, this, &MiniWindow::openCamera);
    m_refresh = new QPushButton(QStringLiteral("↻"), m_footer); m_refresh->setObjectName(QStringLiteral("miniSnapshotRefresh")); m_refresh->setFixedSize(34, 34);
    m_refresh->setAccessibleName(QStringLiteral("刷新摄像头快照")); m_refresh->setCursor(Qt::PointingHandCursor);
    m_refresh->setToolTip(QStringLiteral("刷新快照")); m_refresh->setEnabled(false); snapshotRow->addWidget(m_refresh); footerBox->addLayout(snapshotRow);
    m_snapshotStatus = plainLabel(QString(), "miniSnapshotStatus"); m_snapshotStatus->setWordWrap(true);
    m_snapshotStatus->setStyleSheet(QStringLiteral("color:#e5dcc9; font-size:11px;"));
    m_snapshotStatus->hide(); footerBox->addWidget(m_snapshotStatus);
    m_ptz = new CameraPtzControls(&m_client, {}, m_footer); footerBox->addWidget(m_ptz);
    connect(m_ptz, &CameraPtzControls::presentationChanged, this, &MiniWindow::layoutOverlays);
    connect(m_ptz, &CameraPtzControls::stepAccepted, this, [this] {
        const auto device = selectedCamera();
        QTimer::singleShot(1800, this, [this, key = device.key, generation = device.controllerGeneration] {
            if (!m_destroying && !m_camera && isVisible() && !m_drawerHidden && m_snapshots
                && key == m_selectedKey && generation == selectedCamera().controllerGeneration) m_snapshots->refresh(key);
        });
    });
    m_drawerHandle = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus); m_drawerHandle->setObjectName(QStringLiteral("miniDrawerHandle"));
    m_drawerHandle->setStyleSheet(QStringLiteral("background:rgba(198,230,211,215); border-radius:3px;"));
    m_drawerHandle->setAttribute(Qt::WA_ShowWithoutActivating);
    m_drawerHandle->setAttribute(Qt::WA_QuitOnClose, false);
    m_drawerHandle->setCursor(Qt::PointingHandCursor);
    m_drawerHandle->setAccessibleName(QStringLiteral("悬停展开摄像头"));
    m_drawerHandle->hide();
    setCentralWidget(root);
    // Also observe the WebEngine render child: it receives live-video mouse events.
    qApp->installEventFilter(this);

    m_snapshots = new CameraSnapshots(&m_client, this); m_snapshots->setActive(false);
    m_clock = new QTimer(this); m_clock->setInterval(5000);
    m_visibility = new QTimer(this); m_visibility->setSingleShot(true); m_visibility->setInterval(0);
    m_clickTimer = new QTimer(this); m_clickTimer->setSingleShot(true);
    connect(m_clickTimer, &QTimer::timeout, this, &MiniWindow::togglePanels);
    m_panelAutoHide = new QTimer(this); m_panelAutoHide->setObjectName(QStringLiteral("miniPanelAutoHide"));
    m_panelAutoHide->setSingleShot(true); m_panelAutoHide->setInterval(10000); m_panelAutoHide->setTimerType(Qt::PreciseTimer);
    connect(m_panelAutoHide, &QTimer::timeout, this, [this] {
        if (m_destroying || !m_panelsVisible || !isVisible() || isMinimized() || m_drawerHidden) return;
        if (m_tuyaMetadataDialog && m_tuyaMetadataDialog->isVisible()) return;
        // Keep controls available while a press, drag, menu or permission dialog is in use.
        if (m_pointerPressed || m_dragging || QApplication::mouseButtons() != Qt::NoButton
            || QApplication::activePopupWidget() || QApplication::activeModalWidget()) {
            m_panelAutoHide->start(); return;
        }
        m_panelsVisible = false; updatePanelVisibility();
    });
    m_drawerWatch = new QTimer(this); m_drawerWatch->setInterval(100);
    connect(m_drawerWatch, &QTimer::timeout, this, &MiniWindow::watchDrawer);
    m_drawerAnimation = new QPropertyAnimation(this, "pos", this);
    m_drawerAnimation->setDuration(180); m_drawerAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_drawerAnimation, &QPropertyAnimation::finished, this, [this] {
        if (!m_drawerHidden || m_destroying) return;
        // Only the thin, separately pinned tab remains mapped at the edge.
        // Hiding the main window avoids an invisible full-length input strip.
        hide(); m_drawerHandle->show(); m_drawerHandle->raise();
    });
    auto *fullScreenShortcut = new QShortcut(QKeySequence(Qt::Key_F11), this);
    connect(fullScreenShortcut, &QShortcut::activated, this, &MiniWindow::toggleFullScreen);
    auto *escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escapeShortcut, &QShortcut::activated, this, [this] {
        if (isFullScreen()) toggleFullScreen();
        else if (m_camera) m_camera->close();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen *screen) {
        if (m_dockScreen == screen) clearDock();
    });
    setProperty("drawerHidden", false); setProperty("drawerEdge", QStringLiteral("none"));
    connect(m_clock, &QTimer::timeout, this, &MiniWindow::updateSnapshot);
    connect(m_visibility, &QTimer::timeout, this, &MiniWindow::updateSnapshotActivity);
    connect(m_snapshots, &CameraSnapshots::changed, this, [this](const QString &key) {
        if (!m_destroying && key == m_selectedKey) updateSnapshot();
    });
    connect(&m_client, &MatterClient::devicesChanged, this, &MiniWindow::syncCameras);
    connect(&m_client, &MatterClient::connectionChanged, this, [this] {
        if (m_destroying) return;
        updateConnection(); updateSnapshot(); updateSnapshotActivity();
    });
    connect(&m_client, &MatterClient::serverInfoChanged, this, [this] {
        if (!m_destroying) updateTuyaProfile();
    });
    connect(&m_client, &MatterClient::commandFinished, this,
        [this](const QString &id, const QString &, bool success, const QString &detail) {
            if (m_destroying || id != m_pairRequest || m_pairRequest.isEmpty()) return;
            m_pairRequest.clear(); updateConnection();
            if (!m_pairDialog) return;
            if (success) { m_pairDialog->accept(); return; }
            if (m_pairStatus) m_pairStatus->setText(QStringLiteral("配对失败：") + detail.left(240));
            if (m_pairCode) { m_pairCode->setEnabled(true); m_pairCode->setFocus(); }
            if (m_pairSubmit) { m_pairSubmit->setText(QStringLiteral("重试配对")); m_pairSubmit->setEnabled(m_client.isConnected()); }
        });
    connect(m_add, &QPushButton::clicked, this, &MiniWindow::showPairing);
    connect(m_metadata, &QPushButton::clicked, this, &MiniWindow::showMetadata);
    connect(m_tuyaMetadata, &QPushButton::clicked, this, &MiniWindow::showTuyaMetadata);
    connect(m_tuyaMetadataManager, &TuyaMetadataManager::changed, this, [this] {
        if (!m_destroying) updateMetadataPresentation();
    });
    connect(m_tuyaMetadataManager, &TuyaMetadataManager::statusChanged, this, [this](const QString &status) {
        if (!m_destroying) m_tuyaMetadata->setToolTip(QStringLiteral("涂鸦名称与房间同步 · ") + status);
    });
    connect(m_client.cameraMetadata(), &CameraMetadataManager::changed, this, [this](const QString &) {
        if (!m_destroying) updateMetadataPresentation();
    });
    connect(m_selector, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0) selectCamera(m_selector->itemData(index).toString());
    });
    // Keyboard activation follows the same panel action as a picture click.
    connect(m_preview, &QAbstractButton::clicked, this, &MiniWindow::togglePanels);
    connect(m_refresh, &QPushButton::clicked, this, [this] {
        if (m_snapshots && !m_selectedKey.isEmpty()) m_snapshots->refresh(m_selectedKey);
    });
    // The client owns reconnect backoff; opening the window never prompts for an address.
    QTimer::singleShot(0, this, [this] { if (!m_destroying) m_client.connectToServer(m_controller); });
}

MiniWindow::~MiniWindow() {
    m_destroying = true;
    qApp->removeEventFilter(this);
    m_clickTimer->stop(); m_drawerWatch->stop(); m_drawerAnimation->stop();
    m_panelAutoHide->stop();
    delete m_drawerHandle; m_drawerHandle = nullptr;
    m_clock->stop(); m_visibility->stop();
    disconnect(&m_client, nullptr, this, nullptr);
    disconnect(m_ptz, nullptr, this, nullptr);
    m_ptz->setClosing(); delete m_ptz; m_ptz = nullptr;
    if (m_pairDialog) delete m_pairDialog.data();
    if (m_metadataDialog) delete m_metadataDialog.data();
    if (m_tuyaMetadataDialog) delete m_tuyaMetadataDialog.data();
    delete m_tuyaMetadataManager; m_tuyaMetadataManager = nullptr;
    if (m_camera) {
        disconnect(m_camera, nullptr, this, nullptr);
        // Explicit destruction performs synchronous local microphone/peer cleanup
        // while the member client still owns the Matter session.
        delete m_camera.data();
    }
    if (m_snapshots) {
        disconnect(m_snapshots, nullptr, this, nullptr);
        m_snapshots->setActive(false); delete m_snapshots; m_snapshots = nullptr;
    }
    m_client.disconnectFromServer();
}

bool MiniWindow::event(QEvent *event) {
    const bool handled = QMainWindow::event(event);
    if (!m_destroying && m_visibility && (event->type() == QEvent::Show || event->type() == QEvent::Hide
        || event->type() == QEvent::WindowStateChange)) {
        m_visibility->start();
        updateWindowButtons();
        if (isFullScreen() || isMaximized()) clearDock(false);
        restartPanelAutoHide();
    }
    return handled;
}

bool MiniWindow::isPictureSurface(QObject *object) const {
    auto *widget = qobject_cast<QWidget *>(object);
    if (!widget || widget->window() != this) return false;
    if (widget == this) return true;
    if (!centralWidget() || (widget != centralWidget() && !centralWidget()->isAncestorOf(widget))) return false;
    for (auto *panel : {m_header, m_footer})
        if (widget == panel || panel->isAncestorOf(widget)) return false;
    if (m_camera) {
        auto *panel = m_camera->findChild<QWidget *>(QStringLiteral("cameraOverlay"));
        if (panel && (widget == panel || panel->isAncestorOf(widget))) return false;
    }
    return true;
}

bool MiniWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == centralWidget() && event->type() == QEvent::Resize) layoutOverlays();
    if (!m_destroying && m_panelAutoHide) {
        auto *widget = qobject_cast<QWidget *>(watched);
        const bool owned = widget && (widget == this || isAncestorOf(widget));
        const auto type = event->type();
        if (owned && (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease
            || type == QEvent::MouseButtonDblClick || type == QEvent::KeyPress || type == QEvent::ShortcutOverride
            || type == QEvent::Wheel || (type == QEvent::MouseMove && m_pointerPressed))) restartPanelAutoHide();
    }
    if (!m_destroying && watched == m_drawerHandle && m_drawerHidden) {
        if (event->type() == QEvent::Leave) m_drawerHoverArmed = true;
        if ((event->type() == QEvent::Enter && m_drawerHoverArmed) || event->type() == QEvent::MouseButtonPress) {
            revealDrawer(); return true;
        }
    }
    if (m_destroying || !m_clickTimer || !isPictureSurface(watched))
        return QMainWindow::eventFilter(watched, event);
    if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseMove
        && event->type() != QEvent::MouseButtonRelease && event->type() != QEvent::MouseButtonDblClick)
        return QMainWindow::eventFilter(watched, event);
    auto *mouse = static_cast<QMouseEvent *>(event);
    const QPoint global = mouse->globalPosition().toPoint();
    if (m_drawerHidden && event->type() == QEvent::MouseButtonPress) {
        revealDrawer(); return true;
    }
    if (event->type() == QEvent::MouseButtonDblClick && mouse->button() == Qt::LeftButton) {
        m_clickTimer->stop(); m_pointerPressed = false; m_dragging = false; m_ignoreRelease = true;
        pictureDoubleClick(); return true;
    }
    if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton) {
        m_clickTimer->stop();
        m_pointerPressed = true; m_dragging = false; m_ignoreRelease = false;
        m_pressGlobal = global; m_pressWindow = pos(); return true;
    }
    if (event->type() == QEvent::MouseMove && m_pointerPressed) {
        const QPoint delta = global - m_pressGlobal;
        if (!isFullScreen() && !isMaximized() && (m_dragging || delta.manhattanLength() >= QApplication::startDragDistance())) {
            if (!m_dragging) {
                m_dragging = true; clearDock(false);
                // Wayland delegates movement to the compositor; clients cannot set global positions.
                if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland")) && windowHandle())
                    windowHandle()->startSystemMove();
            }
            if (!QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) move(m_pressWindow + delta);
        }
        return true;
    }
    if (event->type() == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton) {
        if (m_ignoreRelease) { m_ignoreRelease = false; m_pointerPressed = false; return true; }
        if (!m_pointerPressed) return true;
        m_pointerPressed = false;
        if (m_dragging) { m_dragging = false; dockAtScreenEdge(); }
        else m_clickTimer->start(QApplication::doubleClickInterval());
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MiniWindow::togglePanels() {
    m_panelsVisible = !m_panelsVisible;
    updatePanelVisibility();
    restartPanelAutoHide();
}

void MiniWindow::restartPanelAutoHide() {
    if (m_destroying || !m_panelAutoHide) return;
    if (m_panelsVisible && isVisible() && !isMinimized() && !m_drawerHidden
        && !(m_pairDialog && m_pairDialog->isVisible())
        && !(m_metadataDialog && m_metadataDialog->isVisible())
        && !(m_tuyaMetadataDialog && m_tuyaMetadataDialog->isVisible())) m_panelAutoHide->start();
    else m_panelAutoHide->stop();
}

void MiniWindow::updatePanelVisibility() {
    const bool visible = m_panelsVisible && !m_drawerHidden;
    m_header->setVisible(visible);
    m_footer->setVisible(visible && !m_camera && !m_openingCamera);
    if (m_camera) m_camera->setOverlayVisible(visible);
    if (m_panelAutoHide && (!visible || !isVisible() || isMinimized())) m_panelAutoHide->stop();
    layoutOverlays();
}

void MiniWindow::updateWindowButtons() {
    if (!m_fullScreen) return;
    const QSignalBlocker blocker(m_fullScreen);
    m_fullScreen->setChecked(isFullScreen());
    m_fullScreen->setText(isFullScreen() ? QStringLiteral("⊡") : QStringLiteral("⛶"));
    const auto label = isFullScreen() ? QStringLiteral("退出全屏（Esc / F11）") : QStringLiteral("全屏（F11）");
    m_fullScreen->setToolTip(label); m_fullScreen->setAccessibleName(label);
    if (m_onTop) {
        const QSignalBlocker topBlocker(m_onTop);
        m_onTop->setChecked(m_alwaysOnTop);
        const auto topLabel = m_alwaysOnTop ? QStringLiteral("取消窗口置顶") : QStringLiteral("窗口置顶");
        m_onTop->setToolTip(topLabel); m_onTop->setAccessibleName(topLabel);
    }
}

void MiniWindow::setAlwaysOnTop(bool enabled) {
    if (m_alwaysOnTop == enabled) return;
    m_alwaysOnTop = enabled;
    const bool visible = isVisible();
    const QRect previousGeometry = geometry();
    const auto previousState = windowState();
    setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
    setGeometry(previousGeometry);
    if (visible) {
        if (previousState.testFlag(Qt::WindowFullScreen)) showFullScreen();
        else if (previousState.testFlag(Qt::WindowMaximized)) showMaximized();
        else if (previousState.testFlag(Qt::WindowMinimized)) showMinimized();
        else show();
    }
    QSettings().setValue(QStringLiteral("mini/alwaysOnTop"), enabled);
    updateWindowButtons();
}

void MiniWindow::restoreDefaultSize() {
    m_clickTimer->stop();
    QPoint center = isFullScreen() && m_normalGeometry.isValid() ? m_normalGeometry.center() : geometry().center();
    if (m_drawerHidden && m_dockGeometry.isValid()) center = m_dockGeometry.center();
    clearDock();
    auto *screen = QGuiApplication::screenAt(center);
    if (!screen) screen = this->screen();
    const QRect area = screen ? screen->availableGeometry() : QRect(center - QPoint(400,225), DefaultWindowSize);
    const QPoint position(qBound(area.left(), center.x() - (DefaultWindowSize.width()-1)/2, qMax(area.left(), area.right() - DefaultWindowSize.width() + 1)),
                          qBound(area.top(), center.y() - (DefaultWindowSize.height()-1)/2, qMax(area.top(), area.bottom() - DefaultWindowSize.height() + 1)));
    showNormal(); setGeometry(QRect(position, DefaultWindowSize));
    m_normalGeometry = geometry(); updateWindowButtons();
}

void MiniWindow::pictureDoubleClick() {
    if (isFullScreen() || isMaximized() || size() != DefaultWindowSize) restoreDefaultSize();
    else toggleFullScreen();
}

void MiniWindow::toggleFullScreen() {
    m_clickTimer->stop();
    if (isFullScreen()) {
        showNormal();
        if (m_normalGeometry.isValid()) setGeometry(m_normalGeometry);
    } else {
        clearDock();
        m_normalGeometry = geometry();
        showFullScreen();
    }
    updateWindowButtons();
}

void MiniWindow::layoutOverlays() {
    if (!centralWidget() || !m_header || !m_footer) return;
    const int width = centralWidget()->width(), height = centralWidget()->height();
    const int inset = 10, panelWidth = qMax(0, width - inset * 2);
    const int headerHeight = qMin(height, m_header->layout()->sizeHint().height());
    int footerHeight = m_footer->layout()->totalHeightForWidth(panelWidth);
    if (footerHeight < 0) footerHeight = m_footer->layout()->sizeHint().height();
    footerHeight = qMin(height, footerHeight);
    m_header->setGeometry(inset, inset, panelWidth, headerHeight);
    m_footer->setGeometry(inset, height - footerHeight - inset, panelWidth, footerHeight);
    m_header->raise(); m_footer->raise();
    if (m_drawerHandle && m_dockScreen && m_dockGeometry.isValid()) {
        const QRect area = m_dockScreen->availableGeometry();
        const int verticalLength = qMax(1, m_dockGeometry.height() - 16);
        const int horizontalLength = qMax(1, m_dockGeometry.width() - 16);
        const int x = m_dockGeometry.center().x() - (horizontalLength - 1) / 2;
        const int y = m_dockGeometry.center().y() - (verticalLength - 1) / 2;
        if (m_dockEdge == DockEdge::Left) m_drawerHandle->setGeometry(area.left(), y, DrawerStrip, verticalLength);
        else if (m_dockEdge == DockEdge::Right) m_drawerHandle->setGeometry(area.right() - DrawerStrip + 1, y, DrawerStrip, verticalLength);
        else if (m_dockEdge == DockEdge::Top) m_drawerHandle->setGeometry(x, area.top(), horizontalLength, DrawerStrip);
        else if (m_dockEdge == DockEdge::Bottom) m_drawerHandle->setGeometry(x, area.bottom() - DrawerStrip + 1, horizontalLength, DrawerStrip);
    }
}

void MiniWindow::moveDrawer(const QPoint &position, bool animated) {
    m_drawerAnimation->stop();
    if (!animated) { move(position); return; }
    m_drawerAnimation->setStartValue(pos()); m_drawerAnimation->setEndValue(position);
    m_drawerAnimation->start();
}

void MiniWindow::clearDock(bool restore) {
    if (!m_drawerWatch) return;
    const bool wasHidden = m_drawerHidden;
    m_drawerWatch->stop(); m_drawerAnimation->stop();
    disconnect(m_screenGeometryChanged);
    if (restore && m_dockGeometry.isValid()) {
        auto *screen = m_dockScreen ? m_dockScreen.data() : QGuiApplication::primaryScreen();
        QRect area = screen ? screen->availableGeometry() : m_dockGeometry;
        QPoint position(qBound(area.left(), m_dockGeometry.left(), qMax(area.left(), area.right() - width() + 1)),
                        qBound(area.top(), m_dockGeometry.top(), qMax(area.top(), area.bottom() - height() + 1)));
        move(position);
    }
    m_drawerHandle->hide();
    m_drawerHidden = false; m_dockEdge = DockEdge::None; m_dockScreen = nullptr; m_dockGeometry = {};
    setProperty("drawerHidden", false); setProperty("drawerEdge", QStringLiteral("none"));
    if (wasHidden && !isVisible()) {
        const bool noActivation = testAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_ShowWithoutActivating); show(); setAttribute(Qt::WA_ShowWithoutActivating, noActivation);
    }
    updatePanelVisibility(); updateSnapshotActivity();
}

void MiniWindow::dockAtScreenEdge() {
    // X11 permits off-screen placement. Wayland does not expose this operation.
    if (isFullScreen() || isMaximized() || QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) return;
    auto *screen = QGuiApplication::screenAt(frameGeometry().center());
    if (!screen) screen = QGuiApplication::screenAt(m_pressGlobal);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    const QRect area = screen->availableGeometry(), box = geometry();
    if (width() >= area.width() || height() >= area.height()) return;
    struct Candidate { DockEdge edge; int distance; const char *name; };
    const Candidate candidates[] = {
        {DockEdge::Left, box.left() - area.left(), "left"}, {DockEdge::Right, area.right() - box.right(), "right"},
        {DockEdge::Top, box.top() - area.top(), "top"}, {DockEdge::Bottom, area.bottom() - box.bottom(), "bottom"}
    };
    const Candidate *nearest = nullptr;
    for (const auto &candidate : candidates)
        if (candidate.distance <= DockDistance && (!nearest || candidate.distance < nearest->distance)) nearest = &candidate;
    if (!nearest) return;
    QRect docked(QPoint(qBound(area.left(), box.left(), area.right() - width() + 1),
                        qBound(area.top(), box.top(), area.bottom() - height() + 1)), size());
    QRect hidden = docked;
    if (nearest->edge == DockEdge::Left) { docked.moveLeft(area.left()); hidden.moveLeft(area.left() - width() + DrawerStrip); }
    if (nearest->edge == DockEdge::Right) { docked.moveRight(area.right()); hidden.moveLeft(area.right() - DrawerStrip + 1); }
    if (nearest->edge == DockEdge::Top) { docked.moveTop(area.top()); hidden.moveTop(area.top() - height() + DrawerStrip); }
    if (nearest->edge == DockEdge::Bottom) { docked.moveBottom(area.bottom()); hidden.moveTop(area.bottom() - DrawerStrip + 1); }
    // An internal monitor seam must never hide the body on another display.
    for (auto *other : QGuiApplication::screens())
        if (other != screen && other->geometry().intersects(hidden)) return;
    clearDock(false);
    m_dockEdge = nearest->edge; m_dockScreen = screen; m_dockGeometry = docked;
    setProperty("drawerEdge", QLatin1String(nearest->name));
    move(docked.topLeft());
    m_screenGeometryChanged = connect(screen, &QScreen::availableGeometryChanged, this, [this] { clearDock(); });
    m_drawerWatch->start(); collapseDrawer();
}

void MiniWindow::watchDrawer() {
    if (!m_dockScreen || m_dockEdge == DockEdge::None) { clearDock(); return; }
    if (m_drawerAnimation->state() == QAbstractAnimation::Running) return;
    const QPoint cursor = QCursor::pos();
    if (m_drawerHidden) {
        if (!m_drawerHandle->isVisible()) return;
        if (!m_drawerHandle->geometry().contains(cursor)) m_drawerHoverArmed = true;
        else if (m_drawerHoverArmed) revealDrawer();
        return;
    }
    if (!isVisible() || isMinimized() || isFullScreen() || m_pointerPressed || m_dragging
        || (m_pairDialog && m_pairDialog->isVisible()) || (m_metadataDialog && m_metadataDialog->isVisible())
        || (m_tuyaMetadataDialog && m_tuyaMetadataDialog->isVisible())
        || QApplication::activeModalWidget()) return;
    // A compositor or keyboard move away from the edge cancels docking.
    if (geometry() != m_dockGeometry) { clearDock(false); return; }
    if (!geometry().contains(cursor)) collapseDrawer();
}

void MiniWindow::collapseDrawer() {
    if (!m_dockScreen || m_dockEdge == DockEdge::None || !isVisible() || isMinimized() || isFullScreen()
        || m_pointerPressed || m_dragging || QApplication::activeModalWidget()
        || (m_pairDialog && m_pairDialog->isVisible()) || (m_metadataDialog && m_metadataDialog->isVisible())
        || (m_tuyaMetadataDialog && m_tuyaMetadataDialog->isVisible())) return;
    const QRect area = m_dockScreen->availableGeometry();
    QPoint hidden = m_dockGeometry.topLeft();
    if (m_dockEdge == DockEdge::Left) hidden.setX(area.left() - width() + DrawerStrip);
    if (m_dockEdge == DockEdge::Right) hidden.setX(area.right() - DrawerStrip + 1);
    if (m_dockEdge == DockEdge::Top) hidden.setY(area.top() - height() + DrawerStrip);
    if (m_dockEdge == DockEdge::Bottom) hidden.setY(area.bottom() - DrawerStrip + 1);
    m_drawerHidden = true; setProperty("drawerHidden", true);
    updatePanelVisibility(); updateSnapshotActivity();
    // A release already over the tab must not immediately reopen the drawer.
    m_drawerHoverArmed = !m_drawerHandle->geometry().contains(QCursor::pos());
    moveDrawer(hidden, true);
}

void MiniWindow::revealDrawer(bool animated) {
    if (!m_drawerHidden || !m_dockGeometry.isValid()) return;
    m_drawerHidden = false; setProperty("drawerHidden", false); m_drawerHandle->hide();
    if (!isVisible()) {
        const bool noActivation = testAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_ShowWithoutActivating); show(); raise(); setAttribute(Qt::WA_ShowWithoutActivating, noActivation);
    }
    moveDrawer(m_dockGeometry.topLeft(), animated);
    updatePanelVisibility(); updateSnapshotActivity();
}

MatterDevice MiniWindow::selectedCamera() const {
    for (const auto &device : m_cameras) if (device.key == m_selectedKey) return device;
    return {};
}

void MiniWindow::syncCameras() {
    if (m_destroying) return;
    m_cameras.clear();
    for (const auto &device : m_client.devices()) if (device.hasCamera) m_cameras.append(device);
    const QSignalBlocker block(m_selector); m_selector->clear();
    bool found = false;
    for (const auto &device : m_cameras) {
        m_selector->addItem(device.name, device.key);
        if (device.key == m_selectedKey) found = true;
    }
    QString key = m_selectedKey;
    if (!found) {
        key.clear();
        for (const auto &device : m_cameras) if (device.key == m_preferredKey) { key = device.key; break; }
        if (key.isEmpty() && !m_cameras.isEmpty()) key = m_cameras.first().key;
    }
    selectCamera(key);
}

void MiniWindow::selectCamera(const QString &key) {
    if (m_destroying) return;
    const bool changed = m_selectedKey != key;
    m_selectedKey = key;
    if (!key.isEmpty()) {
        m_preferredKey = key; QSettings().setValue(QStringLiteral("mini/selectedCameraKey"), key);
    }
    const QSignalBlocker block(m_selector);
    m_selector->setCurrentIndex(m_selector->findData(key));
    if (changed && m_metadataDialog) m_metadataDialog->close();
    if (changed && m_tuyaMetadataDialog) m_tuyaMetadataDialog->close();
    if (changed) m_preview->setImage({});
    const auto device = selectedCamera();
    m_ptz->setDevice(device);
    if (!m_camera) m_pages->setCurrentIndex(device.hasCamera ? 1 : 0);
    else if (changed || !device.hasCamera) m_camera->close();
    updateMetadataPresentation();
    updateConnection(); updateSnapshotActivity(); updateSnapshot();
}

void MiniWindow::updateMetadataPresentation() {
    if (m_destroying) return;
    const auto device = selectedCamera();
    const auto metadata = m_client.cameraMetadata()->state(device);
    const bool incomplete = cameraMetadataUnavailable(metadata);
    const auto name = cameraDisplayName(device);
    m_title->setText(device.hasCamera ? name : QStringLiteral("摄像头"));
    m_title->setToolTip(device.hasCamera ? name : QString());
    QStringList location;
    if (metadata.known && !metadata.snapshot.homeName.isEmpty()) location << metadata.snapshot.homeName;
    if (metadata.known && !metadata.snapshot.roomName.isEmpty()) location << metadata.snapshot.roomName;
    const auto tuyaRoom = !incomplete && location.isEmpty() && m_tuyaMetadataManager
        ? m_tuyaMetadataManager->roomName(device) : QString();
    m_metadataLocation->setText(incomplete ? QStringLiteral("米家信息暂不可用")
        : !location.isEmpty() ? QStringLiteral("米家 · ") + location.join(QStringLiteral(" / "))
        : !tuyaRoom.isEmpty() ? QStringLiteral("涂鸦 · ") + tuyaRoom : QString());
    m_metadataLocation->setVisible(incomplete || !location.isEmpty() || !tuyaRoom.isEmpty());
    const QSignalBlocker block(m_selector);
    for (const auto &camera : m_cameras) {
        const int index = m_selector->findData(camera.key);
        if (index >= 0) m_selector->setItemText(index, cameraDisplayName(camera));
    }
    layoutOverlays();
}
QString MiniWindow::cameraDisplayName(const MatterDevice &device) {
    const auto mijia = m_client.cameraMetadata()->state(device);
    if (cameraMetadataUnavailable(mijia)) return QStringLiteral("摄像头");
    if (mijia.known && !mijia.snapshot.name.isEmpty()) return mijia.snapshot.name;
    const auto tuya = m_tuyaMetadataManager ? m_tuyaMetadataManager->displayName(device) : QString();
    return tuya.isEmpty() ? device.name : tuya;
}
void MiniWindow::updateTuyaProfile() {
    if (m_destroying || !m_tuyaMetadataManager) return;
    QString profile;
    if (m_client.isConnected() && !m_client.isDemoMode()) {
        const auto info = m_client.serverInfo(); QStringList identity;
        for (const auto &key : {"fabric_id", "compressed_fabric_id"}) {
            if (info.contains(key)) identity << key + QString::fromUtf8(
                QJsonDocument(QJsonArray{info.value(key)}).toJson(QJsonDocument::Compact));
        }
        profile = m_client.serverUrl().adjusted(QUrl::NormalizePathSegments | QUrl::RemoveFragment).toString()
            + QStringLiteral("|") + identity.join(QStringLiteral("|"));
    }
    if (profile == m_tuyaMetadataManager->profile()) return;
    if (m_tuyaMetadataDialog) m_tuyaMetadataDialog->close();
    m_tuyaMetadataManager->setProfile(profile);
    updateMetadataPresentation();
}
void MiniWindow::showTuyaMetadata() {
    if (m_destroying || !m_tuyaMetadataManager) return;
    if (m_tuyaMetadataDialog) {
        m_tuyaMetadataDialog->show(); m_tuyaMetadataDialog->raise(); m_tuyaMetadataDialog->activateWindow(); return;
    }
    auto *dialog = new QDialog(this); m_tuyaMetadataDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("miniTuyaMetadataDialog"));
    dialog->setWindowTitle(QStringLiteral("涂鸦名称与房间同步")); dialog->resize(620, 680);
    auto *layout = new QVBoxLayout(dialog);
    const auto device = selectedCamera();
    if (device.hasCamera) layout->addWidget(new TuyaMetadataPanel(m_tuyaMetadataManager, device, dialog));
    else layout->addWidget(new TuyaMetadataPanel(m_tuyaMetadataManager, dialog));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(dialog, &QDialog::finished, this, [this] { restartPanelAutoHide(); });
    dialog->show(); restartPanelAutoHide();
}
void MiniWindow::showMetadata() {
    const auto device = selectedCamera();
    if (!hasCameraMetadata(device)) return;
    if (m_metadataDialog) { m_metadataDialog->show(); m_metadataDialog->raise(); return; }
    auto *dialog = new QDialog(this); m_metadataDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setObjectName(QStringLiteral("miniMetadataDialog"));
    dialog->setWindowTitle(QStringLiteral("米家名称与房间")); dialog->resize(560, 640);
    auto *layout = new QVBoxLayout(dialog);
    layout->addWidget(new CameraMetadataPanel(m_client.cameraMetadata(), device, dialog));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(dialog, &QDialog::finished, this, [this] { restartPanelAutoHide(); });
    dialog->show(); restartPanelAutoHide();
}
void MiniWindow::updateConnection() {
    if (m_destroying) return;
    updateTuyaProfile();
    const bool ready = m_client.isConnected() && !m_client.isDemoMode();
    const auto device = selectedCamera();
    m_connection->setText(!ready ? QStringLiteral("正在连接…") : !device.hasCamera ? QStringLiteral("已连接")
        : device.available ? QStringLiteral("● 在线") : QStringLiteral("○ 摄像头离线"));
    m_add->setEnabled(ready && m_pairRequest.isEmpty() && !m_camera);
    m_add->setVisible(!m_camera);
    m_metadata->setVisible(hasCameraMetadata(device));
    m_metadata->setEnabled(ready && device.available && hasCameraMetadata(device));
    m_tuyaMetadata->setEnabled(true);
    m_tuyaMetadata->setToolTip(QStringLiteral("涂鸦名称与房间同步 · ") + m_tuyaMetadataManager->statusText());
    m_selector->setVisible(m_cameras.size() > 1 && !m_camera);
    m_selector->setEnabled(!m_camera && !m_openingCamera);
    if (m_camera && (!ready || !device.available)) m_camera->close();
    if (m_pairDialog && m_pairRequest.isEmpty() && m_pairSubmit) m_pairSubmit->setEnabled(ready);
    layoutOverlays();
}

void MiniWindow::updateSnapshot() {
    if (m_destroying || !m_snapshots) return;
    const auto device = selectedCamera();
    const auto state = m_snapshots->state(m_selectedKey);
    const bool online = device.hasCamera && device.available && m_client.isConnected() && !m_client.isDemoMode();
    const bool hasImage = !state.image.isNull();
    const bool stale = hasImage && (state.stale || !online);
    m_preview->setImage(state.image);
    m_preview->setEnabled(true);
    m_play->setEnabled(online && !m_camera && !m_openingCamera);
    m_preview->setAccessibleName(device.name + QStringLiteral("快照，单击显隐控制，双击恢复默认尺寸或全屏，拖动移动窗口"));
    m_refresh->setEnabled(online && !state.loading && !m_camera && !m_openingCamera);
    m_refresh->setText(state.loading ? QStringLiteral("…") : QStringLiteral("↻"));
    m_refresh->setToolTip(state.loading ? QStringLiteral("正在刷新快照") : QStringLiteral("刷新快照"));
    const auto time = hasImage ? relativeTime(state.capturedAt) : QStringLiteral("尚无快照");
    m_snapshotTime->setText(time);
    QString status;
    if (!device.hasCamera) status = QStringLiteral("点击右上角 ＋ 添加摄像头");
    else if (!m_client.isConnected()) status = QStringLiteral("等待本机控制器连接");
    else if (!online) status = hasImage ? QStringLiteral("摄像头离线 · 显示旧快照") : QStringLiteral("摄像头离线");
    else if (m_camera || m_openingCamera) status = QStringLiteral("实时画面已打开，快照已暂停");
    else if (state.loading) status = hasImage ? QStringLiteral("正在更新快照…") : QStringLiteral("正在获取快照…");
    else if (!state.message.isEmpty()) status = state.message.left(180) + (stale ? QStringLiteral(" · 显示旧快照") : QString());
    else if (stale) status = QStringLiteral("显示上次快照，等待更新");
    m_snapshotStatus->setText(status); m_snapshotStatus->setVisible(!status.isEmpty());
    const QString empty = !online ? QStringLiteral("摄像头离线") : state.loading ? QStringLiteral("正在获取快照…") : QStringLiteral("点击面板内「直播」查看实时画面");
    m_preview->setPreviewState(state.loading, stale, empty);
    m_preview->setAccessibleDescription(time + (status.isEmpty() ? QString() : QStringLiteral("，") + status));
    updatePanelVisibility();
    layoutOverlays();
}

void MiniWindow::updateSnapshotActivity() {
    if (m_destroying || !m_snapshots) return;
    const auto device = selectedCamera();
    const bool active = isVisible() && !isMinimized() && !m_drawerHidden && !m_camera && !m_openingCamera
        && m_client.isConnected() && !m_client.isDemoMode() && device.hasCamera;
    m_snapshots->setActive(active);
    m_snapshots->setVisibleDevices(active ? QList<MatterDevice>{device} : QList<MatterDevice>{});
    if (active) m_clock->start(); else m_clock->stop();
}

void MiniWindow::openCamera() {
    if (m_destroying) return;
    if (m_camera) { m_pages->setCurrentWidget(m_camera); layoutOverlays(); return; }
    const auto device = selectedCamera();
    if (!device.hasCamera || !device.available || !m_client.isConnected() || m_client.isDemoMode()) return;
    m_openingCamera = true; updateSnapshotActivity();
    m_camera = new CameraWindow(&m_client, device, m_pages); m_camera->setAttribute(Qt::WA_DeleteOnClose);
    m_camera->setCompactMode(true);
    m_camera->setEmbeddedMode(true);
    m_pages->addWidget(m_camera);
    connect(m_camera, &QObject::destroyed, this, [this] {
        if (m_destroying) return;
        m_camera = nullptr;
        m_pages->setCurrentIndex(selectedCamera().hasCamera ? 1 : 0);
        updateConnection(); updateSnapshotActivity(); updateSnapshot();
    });
    m_openingCamera = false; m_pages->setCurrentWidget(m_camera);
    updateConnection(); updateSnapshot(); m_camera->show(); m_camera->setOverlayVisible(m_panelsVisible); m_camera->play(); layoutOverlays();
}

void MiniWindow::showPairing() {
    if (m_destroying || !m_client.isConnected() || m_client.isDemoMode()) return;
    if (m_pairDialog) { m_pairDialog->show(); m_pairDialog->raise(); m_pairDialog->activateWindow(); return; }
    auto *dialog = new QDialog(this); m_pairDialog = dialog;
    dialog->setObjectName(QStringLiteral("miniPairDialog")); dialog->setWindowTitle(QStringLiteral("添加摄像头"));
    dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowModality(Qt::WindowModal); dialog->setMinimumWidth(420);
    auto *box = new QVBoxLayout(dialog); box->setContentsMargins(24, 24, 24, 24); box->setSpacing(14);
    auto *hint = plainLabel(QStringLiteral("输入摄像头的 Matter 配对码或 MT: 二维码内容。"), "miniPairHint");
    hint->setWordWrap(true); box->addWidget(hint);
    auto *code = new QLineEdit(dialog); m_pairCode = code; code->setObjectName(QStringLiteral("miniPairCode"));
    code->setEchoMode(QLineEdit::Password); code->setMaxLength(1024); code->setClearButtonEnabled(true);
    code->setPlaceholderText(QStringLiteral("11 / 21 位配对码，或 MT:…")); code->setAccessibleName(QStringLiteral("Matter 配对码"));
    box->addWidget(code);
    auto *status = plainLabel(QStringLiteral("摄像头可能使用独立于 Homebridge 桥接器的配对码。"), "miniPairStatus");
    m_pairStatus = status; status->setWordWrap(true); status->setStyleSheet(QStringLiteral("font-size:11px; color:#858d82;")); box->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("关闭"));
    auto *submit = buttons->addButton(QStringLiteral("添加"), QDialogButtonBox::ActionRole); m_pairSubmit = submit;
    submit->setObjectName(QStringLiteral("miniPairSubmit")); submit->setDefault(true); box->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(submit, &QPushButton::clicked, dialog, [this, code, submit, status] {
        if (!m_pairRequest.isEmpty()) return;
        QString value = code->text().trimmed();
        if (value.isEmpty()) { status->setText(QStringLiteral("请先输入 Matter 配对码。")); code->setFocus(); return; }
        // Only MatterClient retains the code long enough to send and redact it.
        // The UI never writes codes to settings or a log.
        code->clear(); m_pairRequest = m_client.commission(value); value.fill(QChar()); value.clear();
        if (m_pairRequest.isEmpty()) return;
        code->setEnabled(false); submit->setEnabled(false); status->setText(QStringLiteral("正在配对，请保持摄像头的配对窗口开启…"));
        updateConnection();
    });
    connect(code, &QLineEdit::returnPressed, submit, &QPushButton::click);
    connect(dialog, &QDialog::finished, code, &QLineEdit::clear);
    connect(dialog, &QDialog::finished, this, [this] { restartPanelAutoHide(); });
    dialog->open(); code->setFocus(); restartPanelAutoHide();
}
