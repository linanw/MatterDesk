#pragma once

#include "matterclient.h"
#include <QMainWindow>
#include <QPointer>
#include <QRect>

class CameraSnapshots;
class CameraWindow;
class CameraPtzControls;
class SnapshotPreview;
class QComboBox;
class QDialog;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTimer;
class QPropertyAnimation;
class QScreen;

class MiniWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MiniWindow(QUrl controller = QUrl(QStringLiteral("ws://127.0.0.1:5580/ws")), QWidget *parent = nullptr);
    ~MiniWindow() override;
    MatterClient *client() { return &m_client; }
    QWidget *drawerHandle() const { return m_drawerHandle; }
public slots:
    void togglePanels();
    void toggleFullScreen();
    void pictureDoubleClick();
protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void layoutOverlays();
    bool isPictureSurface(QObject *object) const;
    void updatePanelVisibility();
    void restartPanelAutoHide();
    void dockAtScreenEdge();
    void watchDrawer();
    void collapseDrawer();
    void revealDrawer(bool animated = true);
    void clearDock(bool restore = true);
    void moveDrawer(const QPoint &position, bool animated);
    void updateWindowButtons();
    void restoreDefaultSize();
    void setAlwaysOnTop(bool enabled);
    void syncCameras();
    void selectCamera(const QString &key);
    void updateConnection();
    void updateSnapshot();
    void updateSnapshotActivity();
    void showPairing();
    void showMetadata();
    void updateMetadataPresentation();
    void openCamera();
    MatterDevice selectedCamera() const;

    MatterClient m_client;
    QUrl m_controller;
    CameraSnapshots *m_snapshots = nullptr;
    CameraPtzControls *m_ptz = nullptr;
    QPointer<CameraWindow> m_camera;
    QPointer<QDialog> m_pairDialog;
    QPointer<QDialog> m_metadataDialog;
    QPointer<QLineEdit> m_pairCode;
    QPointer<QPushButton> m_pairSubmit;
    QPointer<QLabel> m_pairStatus;
    QList<MatterDevice> m_cameras;
    QString m_selectedKey, m_preferredKey, m_pairRequest;
    QLabel *m_title = nullptr, *m_connection = nullptr, *m_snapshotTime = nullptr, *m_snapshotStatus = nullptr;
    QPushButton *m_add = nullptr, *m_refresh = nullptr, *m_play = nullptr, *m_fullScreen = nullptr, *m_onTop = nullptr;
    QPushButton *m_metadata = nullptr;
    QLabel *m_metadataLocation = nullptr;
    QComboBox *m_selector = nullptr;
    QStackedWidget *m_pages = nullptr;
    QWidget *m_header = nullptr, *m_footer = nullptr, *m_drawerHandle = nullptr;
    SnapshotPreview *m_preview = nullptr;
    QTimer *m_clock = nullptr, *m_visibility = nullptr;
    QTimer *m_clickTimer = nullptr, *m_drawerWatch = nullptr, *m_panelAutoHide = nullptr;
    QPropertyAnimation *m_drawerAnimation = nullptr;
    enum class DockEdge { None, Left, Right, Top, Bottom };
    DockEdge m_dockEdge = DockEdge::None;
    QPointer<QScreen> m_dockScreen;
    QMetaObject::Connection m_screenGeometryChanged;
    QRect m_dockGeometry, m_normalGeometry;
    QPoint m_pressGlobal, m_pressWindow;
    bool m_panelsVisible = true, m_pointerPressed = false, m_dragging = false;
    bool m_ignoreRelease = false, m_drawerHidden = false;
    bool m_alwaysOnTop = false, m_drawerHoverArmed = false;
    bool m_destroying = false, m_openingCamera = false;
};
