#include "mainwindow.h"
#include "accessorypanel.h"
#include "homeeditors.h"
#include "homestore.h"
#include "devicecard.h"
#include "camerawindow.h"
#include "camerasnapshots.h"
#include "camerametadata.h"
#include "tuyametadata.h"
#include "tuyametadatapanel.h"
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFontMetrics>
#include <QGridLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QLabel *label(const QString &text, const char *name, QWidget *parent = nullptr) {
    auto *result = new QLabel(text, parent); result->setObjectName(name); result->setTextFormat(Qt::PlainText);
    if (QStringList{"hero","collectionTitle","sideEyebrow","roomHeading","sectionTitle"}.contains(QString::fromLatin1(name))) {
        result->setMinimumWidth(0);result->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);result->setWordWrap(QString::fromLatin1(name)!="hero" && QString::fromLatin1(name)!="sectionTitle");result->setToolTip(text);
    }
    return result;
}
QPushButton *button(const QString &text, const char *name) {
    auto *result = new QPushButton(text); result->setObjectName(name); result->setCursor(Qt::PointingHandCursor); return result;
}
void clearLayout(QLayout *layout) {
    while (auto *item = layout->takeAt(0)) {
        if (item->widget()) { item->widget()->hide(); item->widget()->deleteLater(); }
        if (item->layout()) clearLayout(item->layout());
        delete item;
    }
}
QString triggerDescription(const QJsonObject &trigger) {
    if (trigger.value("type") == "time") {
        QStringList days; const QStringList names{"一", "二", "三", "四", "五", "六", "日"};
        for (const auto &day : trigger.value("days").toArray()) if (day.toInt() >= 1 && day.toInt() <= 7) days << names.at(day.toInt()-1);
        return trigger.value("time").toString() + " · 周" + days.join("、");
    }
    return QStringLiteral("设备状态变化时 · 等于设定值");
}
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("MatterDesk · 家庭"); resize(1340, 960); setMinimumSize(940, 680);
    m_home = new HomeStore(&m_client, this);
    m_metadata = m_client.cameraMetadata();
    m_tuyaMetadata = new TuyaMetadataManager(&m_client, this);
    buildUi();
    m_snapshots = new CameraSnapshots(&m_client, this);
    m_snapshotTimer = new QTimer(this); m_snapshotTimer->setSingleShot(true); m_snapshotTimer->setInterval(40);
    connect(m_snapshotTimer, &QTimer::timeout, this, &MainWindow::syncSnapshotVisibility);
    connect(m_snapshots, &CameraSnapshots::changed, this, [this](const QString &key) {
        if (auto *card = m_cards.value(key)) {
            const auto state = m_snapshots->state(key);
            card->setSnapshot(state.image, state.capturedAt, state.message, state.loading, state.stale);
        }
    });
    connect(m_refresh, &QPushButton::clicked, this, [this] {
        for (auto *card : m_cards) if (card->device().hasCamera) m_snapshots->refresh(card->device().key);
    });
    m_layoutTimer = new QTimer(this); m_layoutTimer->setSingleShot(true); m_layoutTimer->setInterval(70);
    connect(m_layoutTimer, &QTimer::timeout, this, &MainWindow::relayoutCards);
    connect(&m_client, &MatterClient::devicesChanged, this, &MainWindow::syncDevices);
    connect(m_metadata, &CameraMetadataManager::changed, this, [this](const QString &) {
        if (!m_destroying) syncDevices();
    });
    connect(m_tuyaMetadata, &TuyaMetadataManager::changed, this, [this] {
        if (!m_destroying) syncDevices();
    });
    connect(&m_client, &MatterClient::connectionChanged, this, &MainWindow::syncConnection);
    connect(&m_client, &MatterClient::serverInfoChanged, this, &MainWindow::syncConnection);
    connect(&m_client, &MatterClient::logMessage, this, &MainWindow::appendLog);
    connect(&m_client, &MatterClient::errorOccurred, this, [this](const QString &s) { notify(s, true); });
    connect(&m_client, &MatterClient::commandFinished, this, [this](const QString &, const QString &command, bool ok, const QString &detail) {
        if ((command == "device_command" || command == "write_attribute") && ok && !m_home->isExecuting()) notify("指令已确认，设备状态以实际回报为准");
        if (!ok && !command.startsWith("camera_")) notify(detail, true);
    });
    connect(m_home, &HomeStore::changed, this, &MainWindow::syncHome);
    connect(m_home, &HomeStore::activityChanged, this, &MainWindow::renderActivity);
    connect(m_home, &HomeStore::errorOccurred, this, [this](const QString &s) { notify(s, true); });
    connect(m_home, &HomeStore::executionStarted, this, [this] { renderCollections(); notify("正在执行场景…"); });
    connect(m_home, &HomeStore::executionFinished, this, [this](const QString &, bool ok, const QString &detail) {
        renderCollections(); notify(ok ? "场景指令已全部确认" : "执行已停止：" + detail, !ok);
    });
    QSettings settings; m_url->setText(settings.value("serverUrl", "ws://127.0.0.1:5580/ws").toString());
    syncConnection(); syncHome();
}
MainWindow::~MainWindow() {
    m_destroying = true; m_pendingCameraKey.clear();
    m_snapshotTimer->stop(); m_snapshots->setActive(false); delete m_snapshots; m_snapshots = nullptr;
    if (m_camera) { disconnect(m_camera, nullptr, this, nullptr); m_camera->close(); delete m_camera.data(); }
    for (auto *panel : findChildren<AccessoryPanel *>()) delete panel;
    delete m_tuyaMetadata; m_tuyaMetadata = nullptr;
    delete m_home; m_home = nullptr;
}
void MainWindow::buildUi() {
    auto *root = new QWidget; root->setObjectName("root"); setCentralWidget(root);
    auto *rootLayout = new QHBoxLayout(root); rootLayout->setContentsMargins(0,0,0,0); rootLayout->setSpacing(0);
    auto *sidebar = new QFrame; sidebar->setObjectName("sidebar"); sidebar->setFixedWidth(205);
    auto *side = new QVBoxLayout(sidebar); side->setContentsMargins(16,25,16,18); side->setSpacing(5);
    auto *brand = new QHBoxLayout; auto *logo = new Symbol("home"); logo->setActive(true); brand->addWidget(logo);
    brand->addWidget(label("家庭", "brand")); brand->addStretch(); side->addLayout(brand); side->addSpacing(16);
    m_sideHome = label("我的家", "sideEyebrow"); side->addWidget(m_sideHome);
    m_navGroup = new QButtonGroup(this); m_navGroup->setExclusive(true);
    const QList<QPair<QString, QString>> navs{{"all","⌂   设备总览"},{"favorite","☆   常用配件"},{"camera","▣   摄像头"},{"light","☼   灯光"},{"climate","◉   温控与空气"},{"security","◇   安全"},{"power","⏻   插座与开关"},{"sensor","⌁   传感器"},{"scenes","◈   场景"},{"automations","◷   自动化"},{"activity","≡   活动记录"}};
    for (const auto &item : navs) {
        auto *nav = button(item.second, "navigation"); nav->setCheckable(true); nav->setChecked(item.first == "all"); nav->setProperty("page", item.first); nav->setMinimumHeight(35);
        m_navGroup->addButton(nav); side->addWidget(nav); connect(nav, &QPushButton::clicked, this, [this, key=item.first] { selectPage(key); });
    }
    auto *roomScroll = new QScrollArea; roomScroll->setWidgetResizable(true); roomScroll->setFrameShape(QFrame::NoFrame); roomScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_roomHost = new QWidget; m_roomHost->setObjectName("roomNav"); m_roomsLayout = new QVBoxLayout(m_roomHost); m_roomsLayout->setContentsMargins(0,8,0,0); m_roomsLayout->setSpacing(3); m_roomsLayout->setAlignment(Qt::AlignTop);
    roomScroll->setWidget(m_roomHost); side->addWidget(roomScroll, 1);
    auto *settings = button("⚙   家庭设置", "sideAction"); side->addWidget(settings); connect(settings, &QPushButton::clicked, this, &MainWindow::showSettings);
    auto *connection = button("⌁   控制器与日志", "sideAction"); side->addWidget(connection); connect(connection, &QPushButton::clicked, this, [this] { closeCamera(); m_connection->setVisible(m_connection->isHidden()); m_log->setVisible(m_connection->isVisible()); });
    m_demo = button("◌   体验演示", "sideAction"); side->addWidget(m_demo); connect(m_demo, &QPushButton::clicked, this, [this] { if (m_client.isDemoMode()) m_client.setHomeDemoMode(false); else startDemo(); });
    side->addWidget(label("MatterDesk   0.6   ·   Linux", "sidebarCaption")); rootLayout->addWidget(sidebar);
    m_mainPanels = new QStackedWidget; m_mainPanels->setObjectName("mainPanels");
    rootLayout->addWidget(m_mainPanels,1);
    auto *main = new QWidget; main->setObjectName("main"); m_dashboard = main; m_mainPanels->addWidget(main);
    auto *body = new QVBoxLayout(main); body->setContentsMargins(30,27,30,20); body->setSpacing(14);
    auto *heading = new QHBoxLayout; auto *titles = new QVBoxLayout; titles->setSpacing(5);
    m_homeTitle = label("我的家", "hero"); titles->addWidget(m_homeTitle);
    m_summary = label("连接家中的配件", "homeSummary"); titles->addWidget(m_summary); heading->addLayout(titles); heading->addStretch();
    m_refresh = button("↻  刷新", "secondary"); heading->addWidget(m_refresh); connect(m_refresh, &QPushButton::clicked, &m_client, &MatterClient::refresh);
    m_pair = button("＋ 添加配件", "primary"); heading->addWidget(m_pair); connect(m_pair, &QPushButton::clicked, this, &MainWindow::showPairing); body->addLayout(heading);
    auto *categories = new QHBoxLayout; categories->setSpacing(10);
    auto addChip=[this,categories](const QString &text,const QString &page){auto *chip=button(text,"categoryChip");chip->setMinimumHeight(52);categories->addWidget(chip,1);connect(chip,&QPushButton::clicked,this,[this,page]{selectPage(page);});return chip;};
    m_climateChip=addChip("◉ 温控与空气","climate");m_securityChip=addChip("◇ 安全","security");m_lightChip=addChip("☼ 灯光","light");m_energyChip=addChip("⏻ 能源","power");body->addLayout(categories);
    m_mode = label("演示家庭 · 所有设备均为模拟数据", "demoBanner"); body->addWidget(m_mode);
    m_connection = new QFrame; m_connection->setObjectName("connectionPanel"); auto *conn = new QVBoxLayout(m_connection); conn->setContentsMargins(15,12,15,12);
    auto *connRow = new QHBoxLayout; m_status = label("○ 未连接", "connectionStatus"); connRow->addWidget(m_status);
    m_url = new QLineEdit; m_url->setObjectName("serverUrl"); m_url->setAccessibleName("Matter Server 地址"); m_url->setPlaceholderText("ws://127.0.0.1:5580/ws"); connRow->addWidget(m_url,1);
    m_connect = button("连接", "connectButton"); connRow->addWidget(m_connect); conn->addLayout(connRow);
    m_serverNote = label("", "muted"); m_serverNote->setWordWrap(true); conn->addWidget(m_serverNote); body->addWidget(m_connection);
    connect(m_connect, &QPushButton::clicked, this, &MainWindow::connectServer); connect(m_url, &QLineEdit::returnPressed, this, &MainWindow::connectServer);
    m_notice = label("", "notice"); m_notice->setWordWrap(true); m_notice->hide(); body->addWidget(m_notice);
    auto *toolbar = new QHBoxLayout; m_sectionTitle = label("全部配件", "sectionTitle");m_sectionTitle->setMinimumWidth(110); toolbar->addWidget(m_sectionTitle,1);
    m_deviceCount = label("", "homeSmall"); toolbar->addWidget(m_deviceCount);
    m_search = new QLineEdit; m_search->setObjectName("deviceSearch"); m_search->setPlaceholderText("搜索配件或房间"); m_search->setClearButtonEnabled(true); m_search->setMaximumWidth(235); m_search->setAccessibleName("搜索设备"); toolbar->addWidget(m_search);
    m_filter = new QComboBox; m_filter->setObjectName("deviceFilter"); m_filter->addItems({"全部状态","仅在线","仅离线"}); m_filter->setAccessibleName("设备状态筛选"); toolbar->addWidget(m_filter);
    m_pageAction = button("＋ 新建场景", "secondary"); m_pageAction->hide(); toolbar->addWidget(m_pageAction); connect(m_pageAction, &QPushButton::clicked, this, [this] {
        if (m_category == "scenes") editScene(); else if (m_category == "automations") editRule(); else if (m_category == "activity") m_home->clearActivity();
    }); body->addLayout(toolbar);
    connect(m_search, &QLineEdit::textChanged, this, &MainWindow::syncDevices); connect(m_filter, &QComboBox::currentIndexChanged, this, &MainWindow::syncDevices);
    m_sceneStripHost = new QWidget; m_sceneStripHost->setObjectName("sceneStrip"); m_sceneStrip = new QHBoxLayout(m_sceneStripHost); m_sceneStrip->setContentsMargins(0,0,0,0); m_sceneStrip->setSpacing(10); body->addWidget(m_sceneStripHost);
    m_pages = new QStackedWidget; m_pages->setObjectName("homePages"); body->addWidget(m_pages,1);
    m_deviceScroll = new QScrollArea; m_deviceScroll->setWidgetResizable(true); m_deviceScroll->setFrameShape(QFrame::NoFrame); m_deviceScroll->setObjectName("deviceScroll"); m_deviceScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_deviceScroll->viewport()->installEventFilter(this); connect(m_deviceScroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { if (m_snapshotTimer) m_snapshotTimer->start(); });
    m_gridHost = new QWidget; m_gridHost->setObjectName("gridHost"); m_grid = new QGridLayout(m_gridHost); m_grid->setContentsMargins(0,0,2,0); m_grid->setSpacing(14); m_grid->setAlignment(Qt::AlignTop); m_deviceScroll->setWidget(m_gridHost); m_pages->addWidget(m_deviceScroll);
    auto addPage = [this](const char *name, QVBoxLayout **layout) {
        auto *scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *host = new QWidget; host->setObjectName(name); *layout = new QVBoxLayout(host); (*layout)->setContentsMargins(0,0,3,0); (*layout)->setSpacing(12); (*layout)->setAlignment(Qt::AlignTop); scroll->setWidget(host); m_pages->addWidget(scroll);
    };
    addPage("scenePage", &m_scenesLayout); addPage("automationPage", &m_rulesLayout); addPage("activityPage", &m_activityLayout);
    m_empty = new QFrame; m_empty->setObjectName("empty"); auto *emptyBox = new QVBoxLayout(m_empty); emptyBox->setContentsMargins(30,40,30,40); emptyBox->setSpacing(16);
    m_emptyTitle = label("欢迎回家", "emptyTitle"); m_emptyTitle->setAlignment(Qt::AlignCenter); emptyBox->addWidget(m_emptyTitle);
    m_emptyHint = label("", "muted"); m_emptyHint->setWordWrap(true); m_emptyHint->setAlignment(Qt::AlignCenter); emptyBox->addWidget(m_emptyHint);
    m_emptyDemo = button("体验演示家庭", "secondary"); emptyBox->addWidget(m_emptyDemo,0,Qt::AlignHCenter); connect(m_emptyDemo,&QPushButton::clicked,this,&MainWindow::startDemo);
    m_log = new QPlainTextEdit; m_log->setObjectName("logs"); m_log->setReadOnly(true); m_log->setMaximumBlockCount(200); m_log->setFixedHeight(120); m_log->hide(); body->addWidget(m_log);
}
void MainWindow::setServerUrl(const QString &url) { m_url->setText(url); }
void MainWindow::startDemo() { m_notice->hide(); m_client.setHomeDemoMode(true); }
void MainWindow::connectServer() {
    if ((m_client.isConnected() || m_client.isConnecting()) && !m_client.isDemoMode()) { m_client.disconnectFromServer(); return; }
    const QUrl url(m_url->text().trimmed());
    if (!url.isValid() || (url.scheme() != "ws" && url.scheme() != "wss") || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) { notify("请输入有效的 ws:// 或 wss:// Matter Server 地址（不含用户名密码）。",true); return; }
    QSettings settings; settings.setValue("serverUrl",url.toString()); m_notice->hide(); m_client.connectToServer(url);
}
void MainWindow::syncConnection() {
    const bool demo=m_client.isDemoMode(), ready=m_client.isConnected();
    if (!ready || demo) closeCamera();
    QString profile;
    if (demo) profile="demo:home-v1";
    else if (ready) {
        const auto info=m_client.serverInfo(); QStringList identity;
        for (const auto &key : {"fabric_id","compressed_fabric_id"}) if (info.contains(key)) identity << key+QString::fromUtf8(QJsonDocument(QJsonArray{info.value(key)}).toJson(QJsonDocument::Compact));
        profile=m_client.serverUrl().adjusted(QUrl::NormalizePathSegments|QUrl::RemoveFragment).toString()+"|"+identity.join("|");
    }
    m_home->setProfile(profile);
    m_tuyaMetadata->setProfile(demo ? QString() : profile);
    if (demo) seedDemo();
    m_mode->setVisible(demo); m_demo->setText(demo ? "◌   退出演示" : "◌   体验演示"); m_status->setText((ready ? "● " : "○ ")+m_client.statusText());
    m_connect->setText(ready&&!demo ? "断开" : m_client.isConnecting() ? "取消连接" : demo ? "连接真实控制器" : "连接");
    m_url->setEnabled((!ready&&!m_client.isConnecting())||demo); m_pair->setEnabled(ready&&!demo); m_refresh->setEnabled(ready);
    m_serverNote->setText(demo ? "演示家庭只更新模拟设备；可随时连接真实控制器。" : ready ? "本地控制器已连接 · 状态实时同步" : "连接本机 Matter Server，再用 Homebridge 的 Matter 配对码添加配件。Homebridge 的 IP 不提供 5580 服务。");
    if (ready) { m_connection->hide(); m_log->hide(); } else if (!m_client.isConnecting()) m_connection->show();
    renderCollections(); syncDevices();
}
void MainWindow::seedDemo() {
    if (m_seeding || m_home->homeName()!=QStringLiteral("我的家") || !m_home->rooms().isEmpty() || !m_home->zones().isEmpty() || !m_home->scenes().isEmpty() || !m_home->rules().isEmpty()) return;
    for (const auto &d:m_client.devices()) if (!m_home->accessory(d.key).isEmpty()) return;
    m_seeding=true;
    m_home->setHomeName("湖畔之家");
    m_home->saveZone({{"id","indoor"},{"name","室内"}});
    const QList<QPair<QString,QString>> rooms{{"living","客厅"},{"bedroom","卧室"},{"kitchen","厨房"},{"entry","玄关"},{"study","书房"}};
    for (const auto &room : rooms) m_home->saveRoom({{"id",room.first},{"name",room.second},{"zoneId","indoor"}});
    const QMap<int,QString> assignments{{2,"living"},{3,"kitchen"},{4,"living"},{5,"study"},{6,"entry"},{7,"bedroom"},{8,"living"},{9,"entry"},{10,"living"},{11,"bedroom"},{12,"bedroom"},{13,"living"}};
    for (const auto &d : m_client.devices()) m_home->setAccessory(d.key,{{"roomId",assignments.value(d.endpointId)},{"favorite",QList<int>{2,8,9,10,11,12}.contains(d.endpointId)}});
    auto action=[](int endpoint,const char *key,QJsonValue value){return QJsonObject{{"deviceKey",QString("1/%1").arg(endpoint)},{"action",key},{"value",value}};};
    m_home->saveScene({{"id","arrive"},{"name","回家"},{"actions",QJsonArray{action(2,"power",true),action(8,"power",true),action(8,"brightness",80),action(10,"position",100)}}});
    m_home->saveScene({{"id","movie"},{"name","观影时光"},{"actions",QJsonArray{action(2,"brightness",20),action(8,"brightness",15),action(10,"position",10)}}});
    m_home->saveScene({{"id","night"},{"name","晚安"},{"actions",QJsonArray{action(2,"power",false),action(8,"power",false),action(9,"lock",true)}}});
    m_home->saveRule({{"id","night-time"},{"name","每晚的晚安"},{"enabled",false},{"sceneId","night"},{"trigger",QJsonObject{{"type","time"},{"time","22:30"},{"days",QJsonArray{1,2,3,4,5,6,7}}}}});
    m_seeding=false; syncHome();
}
QString MainWindow::roomName(const QString &id) const {
    for (const auto &room : m_home->rooms()) if (room.toObject().value("id")==id) return room.toObject().value("name").toString();
    return "未分配房间";
}
QString MainWindow::displayName(const MatterDevice &device) const {
    const auto local = m_home->accessory(device.key).value("name").toString();
    if (!local.isEmpty()) return local;
    const auto metadata = m_metadata->state(device);
    if (cameraMetadataUnavailable(metadata)) return QStringLiteral("摄像头");
    if (metadata.known && !metadata.snapshot.name.isEmpty()) return metadata.snapshot.name;
    const auto tuya = m_tuyaMetadata->displayName(device);
    return tuya.isEmpty() ? device.name : tuya;
}
QString MainWindow::roomCaption(const MatterDevice &device) const {
    const auto local = m_home->accessory(device.key).value("roomId").toString();
    if (!local.isEmpty()) return roomName(local);
    const auto metadata = m_metadata->state(device);
    if (cameraMetadataUnavailable(metadata)) return QStringLiteral("米家信息暂不可用");
    if (metadata.known) {
        QStringList parts;
        if (!metadata.snapshot.homeName.isEmpty()) parts << metadata.snapshot.homeName;
        if (!metadata.snapshot.roomName.isEmpty()) parts << metadata.snapshot.roomName;
        if (!parts.isEmpty()) return QStringLiteral("米家 · ") + parts.join(QStringLiteral(" / "));
    }
    const auto tuya = m_tuyaMetadata->roomName(device);
    if (!tuya.isEmpty()) return QStringLiteral("涂鸦 · ") + tuya;
    return roomName({});
}
void MainWindow::syncHome() {
    if (m_seeding) return;
    if (m_presentedProfile!=m_home->profile()) {
        m_presentedProfile=m_home->profile();
        closeCamera();
    }
    m_homeTitle->setText(m_home->homeName());m_homeTitle->setToolTip(m_home->homeName()); m_sideHome->setText(m_home->homeName());m_sideHome->setToolTip(m_home->homeName());
    clearLayout(m_roomsLayout);
    if (!m_home->rooms().isEmpty()) m_roomsLayout->addWidget(label("房间", "sideEyebrow"));
    auto addRoomNav=[this](const QString &key,const QString &name){
        auto *nav=button(QFontMetrics(font()).elidedText(name,Qt::ElideRight,150),"navigation");nav->setToolTip(name);nav->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred); nav->setCheckable(true); nav->setProperty("page",key); nav->setChecked(m_category==key); m_navGroup->addButton(nav); m_roomsLayout->addWidget(nav);
        connect(nav,&QPushButton::clicked,this,[this,key]{selectPage(key);});
    };
    for (const auto &zone:m_home->zones()) {
        const auto z=zone.toObject(); addRoomNav("zone:"+z.value("id").toString(),"⌑   "+z.value("name").toString());
        for (const auto &room:m_home->rooms()) if (room.toObject().value("zoneId")==z.value("id")) addRoomNav("room:"+room.toObject().value("id").toString(),"    "+room.toObject().value("name").toString());
    }
    for (const auto &room:m_home->rooms()) if (room.toObject().value("zoneId").toString().isEmpty()) addRoomNav("room:"+room.toObject().value("id").toString(),"    "+room.toObject().value("name").toString());
    renderCollections(); syncDevices();
}
void MainWindow::selectPage(const QString &category) {
    closeCamera();
    m_category=category;
    for (auto *nav:m_navGroup->buttons()) nav->setChecked(nav->property("page").toString()==category);
    const int page=category=="scenes"?1:category=="automations"?2:category=="activity"?3:0;
    m_pages->setCurrentIndex(page); m_search->setVisible(page==0); m_filter->setVisible(page==0); m_pageAction->setVisible(page>0);
    m_pageAction->setText(page==1?"＋ 新建场景":page==2?"＋ 新建自动化":"清空记录"); m_pageAction->setEnabled(!m_home->profile().isEmpty());
    syncDevices(); renderCollections(); if (m_snapshotTimer) m_snapshotTimer->start();
}
void MainWindow::syncDevices() {
    const auto devices=m_client.devices(); QSet<QString> keys; int online=0,on=0,lightsOn=0,locks=0,unlocked=0; double watts=0; bool energyKnown=false; QString climate;
    if (m_camera) {
        bool available=false;
        for (const auto &device:devices) if (device.key==m_cameraKey && device.available && device.hasCamera) available=true;
        if (!available) closeCamera();
    }
    for (const auto &d:devices) {
        keys.insert(d.key); online+=d.available&&m_client.isConnected(); on+=d.available&&m_client.isConnected()&&d.hasOnOff&&d.onKnown&&d.on;
        if(d.available&&m_client.isConnected()) {
            if(d.kind=="light"&&d.onKnown&&d.on) ++lightsOn;
            if(d.controlState.contains("lock")) {++locks;unlocked+=!d.controlState.value("lock").toBool();}
            if(climate.isEmpty()&&d.controlState.contains("temperature")) climate=QString::number(d.controlState.value("temperature").toDouble(),'f',1)+"°C";
            if(d.controlState.contains("powerWatts")) {watts+=d.controlState.value("powerWatts").toDouble();energyKnown=true;}
        }
        if (!m_cards.contains(d.key)) {
            auto *card=new DeviceCard(d,m_gridHost); m_cards.insert(d.key,card);
            connect(card,&DeviceCard::powerRequested,this,[this](const MatterDevice &device,bool value){m_client.control(device,"power",value);});
            connect(card,&DeviceCard::detailsRequested,this,&MainWindow::showDetails); connect(card,&DeviceCard::cameraRequested,this,&MainWindow::showCamera);
            connect(card,&DeviceCard::snapshotRequested,this,[this](const MatterDevice &device){m_snapshots->refresh(device.key);});
            connect(card,&DeviceCard::favoriteRequested,this,[this](const MatterDevice &device,bool value){auto meta=m_home->accessory(device.key);meta.insert("favorite",value);m_home->setAccessory(device.key,meta);});
        }
        auto display=d; display.name=displayName(d); m_cards[d.key]->updateDevice(display,m_client.isConnected());
        const auto meta=m_home->accessory(d.key); m_cards[d.key]->setHomePresentation(roomCaption(d),meta.value("favorite").toBool());
        m_cards[d.key]->findChild<QPushButton *>("favorite")->setEnabled(!m_home->profile().isEmpty());
    }
    for (auto it=m_cards.begin();it!=m_cards.end();) {
        if (!keys.contains(it.key())) { auto *card=it.value();m_grid->removeWidget(card);card->hide();card->deleteLater();it=m_cards.erase(it); } else ++it;
    }
    m_summary->setText(QString("%1 个配件在线  ·  %2 个已开启").arg(online).arg(on)+(m_client.isConnected()?"  ·  "+(m_client.isDemoMode()?QStringLiteral("演示家庭"):QStringLiteral("本地连接")):"  ·  控制器未连接"));
    m_climateChip->setText("◉  温控与空气\n"+(climate.isEmpty()?QStringLiteral("查看环境"):climate));
    m_securityChip->setText("◇  安全\n"+(locks?unlocked?QString("%1 个门锁未上锁").arg(unlocked):QString("%1 个门锁已上锁").arg(locks):QStringLiteral("查看传感器")));
    m_lightChip->setText(QString("☼  灯光\n%1 盏已开启").arg(lightsOn));
    m_energyChip->setText("⏻  能源\n"+(energyKnown?QString::number(watts,'f',1)+" W":QStringLiteral("插座与开关")));
    const QMap<QString,QString> titles{{"all","家庭总览"},{"favorite","常用配件"},{"camera","摄像头"},{"light","灯光"},{"climate","温控与空气"},{"security","安全"},{"power","插座与开关"},{"sensor","传感器"},{"scenes","场景"},{"automations","自动化"},{"activity","活动记录"}};
    QString title=titles.value(m_category);
    if (m_category.startsWith("room:")) title=roomName(m_category.mid(5));
    if (m_category.startsWith("zone:")) for (const auto &z:m_home->zones()) if (z.toObject().value("id").toString()==m_category.mid(5)) title=z.toObject().value("name").toString();
    m_sectionTitle->setText(title);m_sectionTitle->setToolTip(title); m_deviceCount->setVisible(m_pages->currentIndex()==0);
    m_sceneStripHost->setVisible((m_category=="all"||m_category=="favorite")&&!m_home->scenes().isEmpty()); relayoutCards();
}
void MainWindow::relayoutCards() {
    if (!m_grid) return;
    m_homeTitle->setText(m_homeTitle->fontMetrics().elidedText(m_home->homeName(),Qt::ElideRight,qMax(40,m_homeTitle->width())));
    while (auto *item=m_grid->takeAt(0)) delete item;
    for (auto *title:m_gridTitles) {title->hide();title->deleteLater();} m_gridTitles.clear();
    const int columns=qBound(1,(m_gridHost->width()+14)/220,4);
    for (int i=0;i<qMax(columns,m_columns);++i) m_grid->setColumnStretch(i,i<columns?1:0);
    m_columns=columns;
    QList<DeviceCard *> visible; const auto search=m_search->text().trimmed();
    for (auto *card:m_cards) {
        const auto d=card->device();const auto meta=m_home->accessory(d.key);const auto room=meta.value("roomId").toString();const bool online=d.available&&m_client.isConnected();
        bool category=m_category=="all"||m_category==d.kind||(m_category=="favorite"&&meta.value("favorite").toBool())||(m_category=="power"&&(d.kind=="outlet"||d.kind=="switch"))||(m_category=="climate"&&(d.kind=="thermostat"||d.kind=="fan"||d.controlState.contains("temperature")||d.controlState.contains("airQuality")))||(m_category=="security"&&(d.kind=="lock"||d.controlState.contains("contact")||d.controlState.contains("occupancy")));
        if (m_category.startsWith("room:")) category=room==m_category.mid(5);
        if (m_category.startsWith("zone:")) {category=false;for (const auto &r:m_home->rooms()) if (r.toObject().value("id")==room&&r.toObject().value("zoneId").toString()==m_category.mid(5)) category=true;}
        const bool status=m_filter->currentIndex()==0||(m_filter->currentIndex()==1&&online)||(m_filter->currentIndex()==2&&!online);
        const bool match=category&&status&&(!meta.value("hidden").toBool()||m_showHidden)&&(d.name.contains(search,Qt::CaseInsensitive)||d.nodeName.contains(search,Qt::CaseInsensitive)||roomCaption(d).contains(search,Qt::CaseInsensitive));
        card->setVisible(match); if (match) visible<<card;
    }
    QMap<QString,int> roomOrder;int roomIndex=0;
    for(const auto &room:m_home->rooms()) roomOrder.insert(room.toObject().value("id").toString(),roomIndex++);
    std::stable_sort(visible.begin(),visible.end(),[this,roomOrder](DeviceCard *a,DeviceCard *b){
        if (a->device().hasCamera!=b->device().hasCamera) return a->device().hasCamera;
        const auto am=m_home->accessory(a->device().key),bm=m_home->accessory(b->device().key);
        const auto ar=roomOrder.value(am.value("roomId").toString(),999),br=roomOrder.value(bm.value("roomId").toString(),999);
        if (ar!=br) return ar<br;
        return am.value("order").toInt()<bm.value("order").toInt();
    });
    int row=0,col=0; QString group;
    for (auto *card:visible) {
        const auto room=card->device().hasCamera?QStringLiteral("摄像头"):roomName(m_home->accessory(card->device().key).value("roomId").toString());
        if (group!=room) {
            if (col) {++row;col=0;} group=room;
            auto *title=label(room,"roomHeading",m_gridHost); m_gridTitles<<title;m_grid->addWidget(title,row++,0,1,columns);
        }
        const int span=card->device().hasCamera?qMin(2,columns):1;
        if (col+span>columns) {++row;col=0;}
        m_grid->addWidget(card,row,col,1,span,Qt::AlignTop);col+=span;if (col>=columns) {++row;col=0;}
    }
    m_empty->setVisible(visible.isEmpty()); if (visible.isEmpty()) m_grid->addWidget(m_empty,0,0,1,columns);
    m_emptyTitle->setText(m_cards.isEmpty()?"欢迎回家":"这里还没有配件");
    m_emptyHint->setText(m_cards.isEmpty()?"连接本地 Matter 控制器，然后添加 Homebridge 桥接器。\n已配对设备会自动出现在这里。":"调整搜索与分类，或在配件设置中添加收藏、分配房间。双击配件可打开完整控制面板。");
    m_emptyDemo->setVisible(m_cards.isEmpty()&&!m_client.isDemoMode()); m_deviceCount->setText(QString("%1 个配件").arg(visible.size()));
    if (m_snapshotTimer) m_snapshotTimer->start();
}
void MainWindow::renderCollections() {
    clearLayout(m_sceneStrip); clearLayout(m_scenesLayout); clearLayout(m_rulesLayout);
    const bool ready=m_client.isConnected()&&!m_home->isExecuting();
    int quick=0;
    for (const auto &item:m_home->scenes()) {
        const auto scene=item.toObject(); const auto id=scene.value("id").toString(); const auto name=scene.value("name").toString();
        if (++quick<=4) {
            auto *run=button("◈  "+name,"quickScene");run->setToolTip(name);run->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);run->setMinimumWidth(0);run->setMinimumHeight(52);run->setEnabled(ready);m_sceneStrip->addWidget(run,1);connect(run,&QPushButton::clicked,this,[this,id]{m_home->runScene(id);});
        }
        auto *frame=new QFrame;frame->setObjectName("collectionCard");auto *row=new QHBoxLayout(frame);row->setContentsMargins(20,18,20,18);
        auto *text=new QVBoxLayout;text->addWidget(label(name,"collectionTitle"));text->addWidget(label(QString("%1 个设备动作").arg(scene.value("actions").toArray().size()),"muted"));row->addLayout(text,1);
        auto *run=button("执行","primary");run->setProperty("sceneId",id);run->setEnabled(ready);row->addWidget(run);connect(run,&QPushButton::clicked,this,[this,id]{m_home->runScene(id);});
        auto *edit=button("编辑","secondary");row->addWidget(edit);connect(edit,&QPushButton::clicked,this,[this,scene]{editScene(scene);});
        auto *remove=button("删除","quiet");row->addWidget(remove);connect(remove,&QPushButton::clicked,this,[this,id]{
            const auto profile=m_home->profile();
            if (QMessageBox::question(this,"删除场景","删除此场景及引用它的自动化？",QMessageBox::Yes|QMessageBox::No)==QMessageBox::Yes && profile==m_home->profile()) m_home->removeScene(id);
        });m_scenesLayout->addWidget(frame);
    }
    if (m_home->scenes().isEmpty()) {
        auto *hint=label("为回家、观影或晚安设置一组配件动作。\n点击「新建场景」开始。","emptyTitle");hint->setWordWrap(true);m_scenesLayout->addWidget(hint);
    }
    if (m_home->isExecuting()) {
        auto *cancel=button("停止后续动作","secondary");m_scenesLayout->addWidget(cancel);connect(cancel,&QPushButton::clicked,m_home,&HomeStore::cancelExecution);
    }
    auto *info=label("自动化仅在应用运行且控制器连接时执行。离线期间错过的定时任务不会补跑。","homeSmall");info->setWordWrap(true);m_rulesLayout->addWidget(info);
    for (const auto &item:m_home->rules()) {
        const auto rule=item.toObject();const auto id=rule.value("id").toString();
        auto *frame=new QFrame;frame->setObjectName("collectionCard");auto *row=new QHBoxLayout(frame);row->setContentsMargins(20,18,20,18);
        auto *enabled=new QCheckBox;enabled->setAccessibleName("启用自动化 "+rule.value("name").toString());enabled->setChecked(rule.value("enabled").toBool());row->addWidget(enabled);
        connect(enabled,&QCheckBox::toggled,this,[this,rule](bool checked){auto updated=rule;updated.insert("enabled",checked);m_home->saveRule(updated);});
        auto *text=new QVBoxLayout;text->addWidget(label(rule.value("name").toString(),"collectionTitle"));text->addWidget(label(triggerDescription(rule.value("trigger").toObject()),"muted"));row->addLayout(text,1);
        auto *run=button("运行一次","secondary");run->setEnabled(ready);row->addWidget(run);connect(run,&QPushButton::clicked,this,[this,id]{m_home->runRule(id);});
        auto *edit=button("编辑","secondary");row->addWidget(edit);connect(edit,&QPushButton::clicked,this,[this,rule]{editRule(rule);});
        auto *remove=button("删除","quiet");row->addWidget(remove);connect(remove,&QPushButton::clicked,this,[this,id]{
            const auto profile=m_home->profile();
            if (QMessageBox::question(this,"删除自动化","删除此自动化？",QMessageBox::Yes|QMessageBox::No)==QMessageBox::Yes && profile==m_home->profile()) m_home->removeRule(id);
        });m_rulesLayout->addWidget(frame);
    }
    if (m_home->rules().isEmpty()) {auto *hint=label("让家庭按你的节奏运行。先创建场景，再选择时间或设备状态触发。","emptyTitle");hint->setWordWrap(true);m_rulesLayout->addWidget(hint);}
}
void MainWindow::renderActivity() {
    clearLayout(m_activityLayout);const auto activity=m_home->activity();
    if (activity.isEmpty()) m_activityLayout->addWidget(label("配件、场景和自动化的执行结果会出现在这里。","homeSmall"));
    for (int i=0;i<activity.size();++i) {
        const auto entry=activity.at(i).toObject();auto *frame=new QFrame;frame->setObjectName("collectionCard");auto *row=new QHBoxLayout(frame);row->setContentsMargins(18,14,18,14);
        auto *state=label(entry.value("success").toBool()?"✓":"!","activityState");state->setStyleSheet(entry.value("success").toBool()?"color:#45856b;font-size:22px;":"color:#bd604f;font-size:22px;");row->addWidget(state);
        auto *text=new QVBoxLayout;text->addWidget(label(entry.value("name").toString(),"collectionTitle"));
        const auto time=QDateTime::fromString(entry.value("timestamp").toString(),Qt::ISODateWithMs);text->addWidget(label(time.toLocalTime().toString("MM-dd HH:mm:ss"),"muted"));row->addLayout(text,1);
        row->addWidget(label(entry.value("success").toBool()?"指令已确认":"执行失败","muted"));m_activityLayout->addWidget(frame);
    }
}
void MainWindow::editScene(QJsonObject scene) {
    if (m_home->profile().isEmpty()) {notify("请先连接家庭或进入演示模式",true);return;}
    const auto profile=m_home->profile();
    auto devices=m_client.devices();for (auto &d:devices) d.name=displayName(d);
    const auto draft=HomeEditors::scene(this,scene,devices);
    if (!draft.isEmpty()&&profile==m_home->profile()) m_home->saveScene(draft);
}
void MainWindow::editRule(QJsonObject rule) {
    if (m_home->profile().isEmpty()) {notify("请先连接家庭或进入演示模式",true);return;}
    const auto profile=m_home->profile();auto devices=m_client.devices();for (auto &d:devices) d.name=displayName(d);
    const auto draft=HomeEditors::automation(this,rule,devices,m_home->scenes());
    if (!draft.isEmpty()&&profile==m_home->profile()) m_home->saveRule(draft);
}
void MainWindow::showSettings() {
    QDialog dialog(this);dialog.setWindowTitle("家庭设置");dialog.resize(620,570);auto *box=new QVBoxLayout(&dialog);box->setContentsMargins(24,24,24,24);box->setSpacing(14);
    const auto profile=m_home->profile();connect(m_home,&HomeStore::changed,&dialog,[this,&dialog,profile]{if(m_home->profile()!=profile)dialog.reject();});
    auto *tabs=new QTabWidget;box->addWidget(tabs,1);
    auto *general=new QWidget;auto *form=new QFormLayout(general);form->setContentsMargins(18,20,18,20);form->setVerticalSpacing(18);
    auto *name=new QLineEdit(m_home->homeName());name->setMaxLength(80);form->addRow("家庭名称",name);auto *save=button("保存名称","primary");save->setEnabled(!profile.isEmpty());form->addRow("",save);
    connect(save,&QPushButton::clicked,&dialog,[this,name,profile]{if(profile==m_home->profile()&&!profile.isEmpty()&&m_home->setHomeName(name->text()))notify("家庭名称已保存");});
    auto *hidden=new QCheckBox("显示隐藏的配件");hidden->setChecked(m_showHidden);form->addRow(hidden);connect(hidden,&QCheckBox::toggled,&dialog,[this](bool value){m_showHidden=value;syncDevices();});
    auto *guide=label("双击配件可调整名称、房间、收藏和控制设置。\n家庭资料仅保存在这台电脑，并按控制器隔离。","muted");guide->setWordWrap(true);form->addRow(guide);tabs->addTab(general,"家庭");
    auto *organization=new QWidget;auto *org=new QVBoxLayout(organization);org->setContentsMargins(16,16,16,16);
    auto *lists=new QHBoxLayout;auto *rooms=new QListWidget;auto *zones=new QListWidget;rooms->setAccessibleName("房间列表");zones->setAccessibleName("区域列表");lists->addWidget(rooms);lists->addWidget(zones);org->addWidget(label("房间                                        区域","muted"));org->addLayout(lists,1);
    auto reload=[this,rooms,zones]{rooms->clear();zones->clear();for(const auto &r:m_home->rooms()){auto *item=new QListWidgetItem(r.toObject().value("name").toString(),rooms);item->setData(Qt::UserRole,QVariant::fromValue(r.toObject()));}for(const auto &z:m_home->zones()){auto *item=new QListWidgetItem(z.toObject().value("name").toString(),zones);item->setData(Qt::UserRole,QVariant::fromValue(z.toObject()));}};reload();
    connect(m_home,&HomeStore::changed,&dialog,reload);
    auto *row=new QHBoxLayout;auto *addRoom=button("＋ 房间","secondary"),*editRoom=button("编辑房间","secondary"),*removeRoom=button("删除房间","quiet");row->addWidget(addRoom);row->addWidget(editRoom);row->addWidget(removeRoom);org->addLayout(row);
    auto editRoomFn=[this,&dialog,profile](QJsonObject existing){
        QDialog editor(&dialog);editor.setWindowTitle("房间");auto *layout=new QFormLayout(&editor);auto *title=new QLineEdit(existing.value("name").toString());title->setMaxLength(80);layout->addRow("名称",title);auto *zone=new QComboBox;zone->addItem("无区域","");for(const auto &z:m_home->zones())zone->addItem(z.toObject().value("name").toString(),z.toObject().value("id"));zone->setCurrentIndex(qMax(0,zone->findData(existing.value("zoneId").toString())));layout->addRow("区域",zone);
        auto *buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);layout->addRow(buttons);connect(buttons,&QDialogButtonBox::accepted,&editor,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&editor,&QDialog::reject);
        if(editor.exec()==QDialog::Accepted && !profile.isEmpty() && profile==m_home->profile()){existing.insert("name",title->text());existing.insert("zoneId",zone->currentData().toString());m_home->saveRoom(existing);}
    };
    connect(addRoom,&QPushButton::clicked,&dialog,[editRoomFn]{editRoomFn({});});connect(editRoom,&QPushButton::clicked,&dialog,[rooms,editRoomFn]{if(rooms->currentItem())editRoomFn(rooms->currentItem()->data(Qt::UserRole).toJsonObject());});
    connect(removeRoom,&QPushButton::clicked,&dialog,[this,rooms,&dialog,profile]{
        if(!rooms->currentItem()) return;
        const auto id=rooms->currentItem()->data(Qt::UserRole).toJsonObject().value("id").toString();
        if(QMessageBox::question(&dialog,"删除房间","删除房间？其中的配件会移至未分配房间。",QMessageBox::Yes|QMessageBox::No)==QMessageBox::Yes && profile==m_home->profile()) m_home->removeRoom(id);
    });
    auto *zoneRow=new QHBoxLayout;auto *addZone=button("＋ 区域","secondary"),*editZone=button("编辑区域","secondary"),*removeZone=button("删除区域","quiet");zoneRow->addWidget(addZone);zoneRow->addWidget(editZone);zoneRow->addWidget(removeZone);org->addLayout(zoneRow);
    auto editZoneFn=[this,&dialog,profile](QJsonObject existing){bool ok=false;const auto name=QInputDialog::getText(&dialog,"区域","名称",QLineEdit::Normal,existing.value("name").toString(),&ok);if(ok && !profile.isEmpty() && profile==m_home->profile()){existing.insert("name",name);m_home->saveZone(existing);}};
    connect(addZone,&QPushButton::clicked,&dialog,[editZoneFn]{editZoneFn({});});connect(editZone,&QPushButton::clicked,&dialog,[zones,editZoneFn]{if(zones->currentItem())editZoneFn(zones->currentItem()->data(Qt::UserRole).toJsonObject());});
    connect(removeZone,&QPushButton::clicked,&dialog,[this,zones,&dialog,profile]{
        if(!zones->currentItem()) return;
        const auto id=zones->currentItem()->data(Qt::UserRole).toJsonObject().value("id").toString();
        if(QMessageBox::question(&dialog,"删除区域","删除区域？房间会保留。",QMessageBox::Yes|QMessageBox::No)==QMessageBox::Yes && profile==m_home->profile()) m_home->removeZone(id);
    });
    organization->setEnabled(!profile.isEmpty());tabs->addTab(organization,"房间与区域");
    auto *tuyaPanel = new TuyaMetadataPanel(m_tuyaMetadata, &dialog);
    tuyaPanel->setObjectName(QStringLiteral("mainTuyaMetadataSettings"));
    tabs->addTab(tuyaPanel, QStringLiteral("涂鸦同步"));
    auto *about=new QWidget;auto *aboutBox=new QVBoxLayout(about);aboutBox->setContentsMargins(18,20,18,20);
    auto *capabilities=label("MatterDesk 0.6\n\n支持 Matter 灯光、彩光、插座、门锁、窗帘、恒温器、风扇及传感器。可用控制取决于配件报告的功能。\n\n摄像头支持快照、直播、收听、显式开启的双向语音，以及具备能力时的云台方向步进，需控制器提供摄像头接口。\n\nApple 的 Siri、家庭钥匙、iCloud 家庭同步、HomeKit 安防录像与 Apple 家庭中枢服务无法通过当前 Linux Matter 客户端提供。\n\n自动化在应用运行时执行；场景为顺序指令，设备失败会停止，已执行动作不会自动回滚。","muted");capabilities->setWordWrap(true);aboutBox->addWidget(capabilities);aboutBox->addStretch();tabs->addTab(about,"支持范围");
    auto *close=new QDialogButtonBox(QDialogButtonBox::Close);connect(close,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);box->addWidget(close);dialog.exec();
}
void MainWindow::showDetails(const MatterDevice &device) {
    auto *panel=new AccessoryPanel(&m_client,m_home,device,this);panel->setAttribute(Qt::WA_DeleteOnClose);panel->show();
}
void MainWindow::showCamera(const MatterDevice &device) {
    if (m_destroying || !m_client.isConnected() || m_client.isDemoMode()) return;
    // Resolve again against the active controller. An old card's queued click
    // must not open an accessory which has since disappeared or gone offline.
    MatterDevice current;
    for (const auto &candidate:m_client.devices()) if (candidate.key==device.key) { current=candidate; break; }
    if (!current.hasCamera || !current.available) return;
    if (m_camera) {
        if (m_cameraKey==current.key && !m_cameraClosing) { m_mainPanels->setCurrentWidget(m_camera); return; }
        closeCamera(); m_pendingCameraKey=current.key; return;
    }
    current.name=displayName(current);
    m_cameraKey=current.key; m_cameraClosing=false;
    auto *window=new CameraWindow(&m_client,current,m_mainPanels); m_camera=window;
    window->setObjectName("mainCameraPlayer"); window->setCompactMode(true); window->setEmbeddedMode(true); window->setVideoFit(true);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->installEventFilter(this);
    for (auto *control:window->findChildren<QPushButton *>()) if (control->property("cameraAction").toString()=="stop") {
        control->setObjectName("cameraBackButton"); control->setText("← 返回配件");
    }
    m_mainPanels->addWidget(window);
    connect(window,&QObject::destroyed,this,[this] {
        if (m_destroying) return;
        m_camera=nullptr; m_cameraKey.clear(); m_cameraClosing=false;
        m_mainPanels->setCurrentWidget(m_dashboard);
        const auto next=m_pendingCameraKey; m_pendingCameraKey.clear();
        if (!next.isEmpty()) {
            for (const auto &device:m_client.devices()) if (device.key==next) { showCamera(device); break; }
        }
        syncSnapshotVisibility();
    });
    m_mainPanels->setCurrentWidget(window); syncSnapshotVisibility(); window->show(); window->play();
}
void MainWindow::closeCamera() {
    m_pendingCameraKey.clear();
    if (m_mainPanels && m_dashboard) m_mainPanels->setCurrentWidget(m_dashboard);
    if (!m_camera || m_cameraClosing) return;
    m_cameraClosing=true;
    // Show the selected home page immediately; the hidden player keeps its
    // bounded asynchronous stop/release lifecycle until it can be destroyed.
    m_camera->hide(); m_camera->close();
}
void MainWindow::syncSnapshotVisibility() {
    if (!m_snapshots||!m_deviceScroll) return;
    const bool active=isVisible()&&!isMinimized()&&!m_camera&&m_mainPanels->currentWidget()==m_dashboard&&m_pages->currentIndex()==0;m_snapshots->setActive(active);QList<MatterDevice> visible;
    if (active) for (auto *card:m_cards) {const QRect bounds(card->mapTo(m_deviceScroll->viewport(),QPoint()),card->size());if(card->device().hasCamera&&card->isVisible()&&bounds.intersects(m_deviceScroll->viewport()->rect()))visible.append(card->device());}
    m_snapshots->setVisibleDevices(visible);
}
bool MainWindow::event(QEvent *event) {if(event->type()==QEvent::Close)closeCamera();const bool result=QMainWindow::event(event);if(m_snapshotTimer&&(event->type()==QEvent::Show||event->type()==QEvent::Hide||event->type()==QEvent::WindowStateChange))m_snapshotTimer->start();return result;}
bool MainWindow::eventFilter(QObject *watched,QEvent *event) {
    if (!m_destroying && watched==m_camera.data() && event->type()==QEvent::Close) {
        // Native Back/Escape also return immediately. The player's final Close
        // arrives after disposal; it must retain any camera queued meanwhile.
        if (!m_cameraClosing) { m_pendingCameraKey.clear(); m_cameraClosing=true; }
        m_mainPanels->setCurrentWidget(m_dashboard);
    }
    if(m_snapshotTimer&&m_deviceScroll&&watched==m_deviceScroll->viewport()&&(event->type()==QEvent::Resize||event->type()==QEvent::Show||event->type()==QEvent::Hide))m_snapshotTimer->start();
    return QMainWindow::eventFilter(watched,event);
}
void MainWindow::resizeEvent(QResizeEvent *event) {QMainWindow::resizeEvent(event);if(m_layoutTimer)m_layoutTimer->start();}
void MainWindow::notify(const QString &message,bool error) {m_notice->setText(message);m_notice->setStyleSheet(error?"color:#8e352b;background:#fbe8e1;border-radius:10px;padding:10px;":"color:#245e46;background:#e5f4eb;border-radius:10px;padding:10px;");m_notice->show();}
void MainWindow::appendLog(const QString &message) {m_log->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss")+"  "+message);}
void MainWindow::showPairing() {
    if (!m_client.isConnected() || m_client.isDemoMode()) return;
    const auto profile=m_home->profile();
    QDialog dialog(this);
    connect(m_home,&HomeStore::changed,&dialog,[this,&dialog,profile]{if(m_home->profile()!=profile)dialog.reject();});
    dialog.setWindowTitle("添加 Homebridge Matter 桥接器"); dialog.setMinimumWidth(490);
    auto *box = new QVBoxLayout(&dialog); box->setContentsMargins(26, 25, 26, 25); box->setSpacing(17);
    box->addWidget(label("把 Homebridge 连接到这里", "emptyTitle"));
    auto *hint = label("在 Homebridge 为主桥接器或子桥接器启用 Matter，再复制 Matter 配对码。已有其他 Matter 控制器时，请从现有控制器打开新的配对窗口并获取配对码。", "muted"); hint->setWordWrap(true); box->addWidget(hint);
    auto *code = new QLineEdit; code->setPlaceholderText("11 / 21 位数字，或 MT: 开头的二维码内容"); code->setEchoMode(QLineEdit::Password); code->setMaxLength(256); code->setAccessibleName("Matter 配对码"); box->addWidget(code);
    auto *show = new QCheckBox("显示配对码"); box->addWidget(show); connect(show, &QCheckBox::toggled, code, [code](bool checked) { code->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password); });
    auto *progress = label("通过局域网配对 · 控制器与 Homebridge 需互通 IPv6 / mDNS", "mutedSmall"); progress->setWordWrap(true); box->addWidget(progress);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); buttons->button(QDialogButtonBox::Cancel)->setText("关闭"); auto *submit = buttons->addButton("开始配对", QDialogButtonBox::ActionRole); submit->setObjectName("primary"); box->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QString request;
    connect(submit, &QPushButton::clicked, &dialog, [&, code, submit, progress] {
        if(profile.isEmpty() || profile!=m_home->profile() || !m_client.isConnected() || m_client.isDemoMode()) {dialog.reject();return;}
        QString value = code->text().trimmed();
        if (!value.startsWith("MT:")) { value.remove(' '); value.remove('-'); }
        if (value.isEmpty()) { progress->setText("请先填写 Homebridge 的 Matter 配对码。"); return; }
        // Clear the secret field immediately; it is never saved to settings or logs.
        code->clear(); request = m_client.commission(value);
        if (!request.isEmpty()) { submit->setEnabled(false); code->setEnabled(false); progress->setText("正在发现和配对桥接器，请保持配对窗口开启…"); }
    });
    connect(&m_client, &MatterClient::commandFinished, &dialog, [&, submit, code, progress](const QString &id, const QString &, bool ok, const QString &detail) {
        if (id != request || request.isEmpty()) return;
        submit->setEnabled(true); code->setEnabled(true); progress->setText(ok ? "配对成功。设备将通过实时订阅出现在列表里。" : "配对失败：" + detail);
        if (ok) { submit->hide(); notify("Homebridge Matter 桥接器已添加"); }
    });
    dialog.exec();
}
