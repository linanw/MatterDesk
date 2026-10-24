#pragma once
#include "matterclient.h"
#include <QMainWindow>
#include <QMap>
#include <QPointer>
class HomeStore;
class CameraWindow;
class CameraSnapshots;
class CameraMetadataManager;
class TuyaMetadataManager;
class DeviceCard;
class QLabel;
class QLineEdit;
class QPushButton;
class QGridLayout;
class QVBoxLayout;
class QHBoxLayout;
class QPlainTextEdit;
class QComboBox;
class QTimer;
class QScrollArea;
class QStackedWidget;
class QButtonGroup;
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    MatterClient *client() { return &m_client; }
    HomeStore *home() { return m_home; }
    TuyaMetadataManager *tuyaMetadata() { return m_tuyaMetadata; }
    void setServerUrl(const QString &url);
    void startDemo();
protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
private:
    void buildUi();
    void syncDevices();
    void syncConnection();
    void syncHome();
    void seedDemo();
    void selectPage(const QString &category);
    void relayoutCards();
    void syncSnapshotVisibility();
    void connectServer();
    void showPairing();
    void showSettings();
    void editScene(QJsonObject scene = {});
    void editRule(QJsonObject rule = {});
    void renderCollections();
    void renderActivity();
    QString roomName(const QString &id) const;
    QString displayName(const MatterDevice &device) const;
    QString roomCaption(const MatterDevice &device) const;
    void showDetails(const MatterDevice &device);
    void showCamera(const MatterDevice &device);
    void closeCamera();
    void appendLog(const QString &message);
    void notify(const QString &message, bool error = false);
    MatterClient m_client;
    HomeStore *m_home = nullptr;
    CameraSnapshots *m_snapshots = nullptr;
    CameraMetadataManager *m_metadata = nullptr;
    TuyaMetadataManager *m_tuyaMetadata = nullptr;
    QLineEdit *m_url = nullptr, *m_search = nullptr;
    QPushButton *m_connect = nullptr, *m_pair = nullptr, *m_refresh = nullptr, *m_demo = nullptr, *m_pageAction = nullptr;
    QPushButton *m_climateChip = nullptr, *m_securityChip = nullptr, *m_lightChip = nullptr, *m_energyChip = nullptr;
    QLabel *m_status = nullptr, *m_serverNote = nullptr, *m_mode = nullptr, *m_notice = nullptr, *m_summary = nullptr, *m_deviceCount = nullptr, *m_sectionTitle = nullptr, *m_homeTitle = nullptr, *m_sideHome = nullptr;
    QComboBox *m_filter = nullptr;
    QWidget *m_gridHost = nullptr, *m_empty = nullptr, *m_connection = nullptr, *m_sceneStripHost = nullptr, *m_roomHost = nullptr;
    QScrollArea *m_deviceScroll = nullptr;
    QLabel *m_emptyTitle = nullptr, *m_emptyHint = nullptr;
    QPushButton *m_emptyDemo = nullptr;
    QGridLayout *m_grid = nullptr;
    QHBoxLayout *m_sceneStrip = nullptr;
    QVBoxLayout *m_roomsLayout = nullptr, *m_scenesLayout = nullptr, *m_rulesLayout = nullptr, *m_activityLayout = nullptr;
    QStackedWidget *m_pages = nullptr, *m_mainPanels = nullptr;
    QWidget *m_dashboard = nullptr;
    QButtonGroup *m_navGroup = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QMap<QString, DeviceCard *> m_cards;
    QPointer<CameraWindow> m_camera;
    QString m_cameraKey, m_pendingCameraKey;
    QString m_category = "all", m_presentedProfile;
    int m_columns = 0;
    bool m_seeding = false, m_showHidden = false, m_cameraClosing = false, m_destroying = false;
    QList<QWidget *> m_gridTitles;
    QTimer *m_layoutTimer = nullptr;
    QTimer *m_snapshotTimer = nullptr;
};
