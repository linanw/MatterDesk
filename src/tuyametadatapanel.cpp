#include "tuyametadatapanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {
QPushButton *button(const QString &text, const char *name, QWidget *parent) {
    auto *result = new QPushButton(text,parent); result->setObjectName(QLatin1String(name)); return result;
}
QLabel *label(QWidget *parent) {
    auto *result = new QLabel(parent); result->setWordWrap(true); result->setTextFormat(Qt::PlainText); result->setTextInteractionFlags(Qt::TextSelectableByMouse); return result;
}
}

TuyaMetadataPanel::TuyaMetadataPanel(TuyaMetadataManager *manager, QWidget *parent)
    : QWidget(parent), m_manager(manager) { build(); }
TuyaMetadataPanel::TuyaMetadataPanel(TuyaMetadataManager *manager, const MatterDevice &device, QWidget *parent)
    : QWidget(parent), m_manager(manager), m_fixed(device) { build(); }

void TuyaMetadataPanel::build() {
    auto *layout = new QVBoxLayout(this);
    auto *intro = label(this);
    intro->setText(QStringLiteral("涂鸦元数据使用独立服务。需在 Homebridge 显式启用该服务并配置令牌。这里只管理独立的涂鸦镜像；本地别名、家庭房间、米家资料、Matter 配对与设备控制保持独立。"));
    layout->addWidget(intro);
    auto *connection = new QFormLayout;
    m_url = new QLineEdit(this); m_url->setObjectName("tuyaUrl"); m_url->setPlaceholderText("https://bridge.example/ 或 http://127.0.0.1:端口");
    m_token = new QLineEdit(this); m_token->setObjectName("tuyaToken"); m_token->setEchoMode(QLineEdit::Password); m_token->setPlaceholderText(QStringLiteral("至少32位；仅保存在本次会话内"));
    connection->addRow(QStringLiteral("元数据服务"),m_url); connection->addRow(QStringLiteral("会话令牌"),m_token); layout->addLayout(connection);
    auto *connectionButtons = new QHBoxLayout;
    m_connect = button(QStringLiteral("连接并读取"),"tuyaConnect",this); m_refresh = button(QStringLiteral("刷新缓存资料"),"tuyaRefresh",this);
    m_reset = button(QStringLiteral("重置涂鸦来源"),"tuyaReset",this);
    connectionButtons->addWidget(m_connect); connectionButtons->addWidget(m_refresh); connectionButtons->addWidget(m_reset); layout->addLayout(connectionButtons);
    m_status = label(this); m_status->setObjectName("tuyaStatus"); layout->addWidget(m_status);
    auto *options = new QFormLayout;
    m_enabled = new QCheckBox(QStringLiteral("启用已明确绑定设备的涂鸦显示镜像（默认关闭）"),this); m_enabled->setObjectName("tuyaEnabled"); options->addRow(m_enabled);
    m_mode = new QComboBox(this); m_mode->setObjectName("tuyaMode");
    m_mode->addItem(QStringLiteral("手动确认来源变化"),"manual"); m_mode->addItem(QStringLiteral("跟随涂鸦来源"),"followSource"); m_mode->addItem(QStringLiteral("双向管理独立镜像（写入须支持并确认）"),"bidirectional");
    options->addRow(QStringLiteral("同步方式"),m_mode);
    m_home = new QComboBox(this); m_home->setObjectName("tuyaHome"); options->addRow(QStringLiteral("明确选择涂鸦家庭"),m_home);
    m_device = new QComboBox(this); m_device->setObjectName("tuyaDevice"); options->addRow(QStringLiteral("Matter 设备"),m_device);
    m_source = new QComboBox(this); m_source->setObjectName("tuyaSourceDevice"); options->addRow(QStringLiteral("涂鸦源设备"),m_source);
    layout->addLayout(options);
    auto *bindingButtons = new QHBoxLayout;
    m_bind = button(QStringLiteral("确认设备绑定"),"tuyaBind",this); m_follow = button(QStringLiteral("采用来源值 / 建立基线"),"tuyaFollow",this);
    bindingButtons->addWidget(m_bind); bindingButtons->addWidget(m_follow); layout->addLayout(bindingButtons);
    m_values = label(this); m_values->setObjectName("tuyaValues"); layout->addWidget(m_values);
    auto *editor = new QFormLayout;
    m_name = new QLineEdit(this); m_name->setObjectName("tuyaName"); m_name->setPlaceholderText(QStringLiteral("完整涂鸦名称；不会修改本地别名"));
    editor->addRow(QStringLiteral("独立同步名称草稿"),m_name);
    m_room = new QComboBox(this); m_room->setObjectName("tuyaRoom"); editor->addRow(QStringLiteral("来源房间"),m_room); layout->addLayout(editor);
    auto *writeButtons = new QHBoxLayout;
    m_rename = button(QStringLiteral("确认写入涂鸦名称"),"tuyaRename",this); m_move = button(QStringLiteral("确认移动来源房间"),"tuyaMove",this);
    m_rebase = button(QStringLiteral("确认新版本，保留草稿"),"tuyaRebase",this);
    writeButtons->addWidget(m_rename); writeButtons->addWidget(m_move); writeButtons->addWidget(m_rebase); layout->addLayout(writeButtons);
    m_hint = label(this); m_hint->setObjectName("tuyaDraftHint"); layout->addWidget(m_hint);
    m_ack = button(QStringLiteral("已核实来源：解除未知写入锁定（不重发）"),"tuyaAcknowledge",this); layout->addWidget(m_ack);
    layout->addStretch();

    connect(m_connect,&QPushButton::clicked,this,[this] {
        if (!m_manager) return;
        const auto token = m_token->text();
        if (m_manager->configure(QUrl(m_url->text().trimmed(),QUrl::StrictMode),token)) m_token->clear();
        updateState();
    });
    connect(m_refresh,&QPushButton::clicked,this,[this] { if (m_manager) m_manager->refresh(); });
    connect(m_reset,&QPushButton::clicked,this,[this] { if (m_manager && m_manager->resetSource()) { m_token->clear(); m_draftDevice.clear(); updateState(); } });
    connect(m_enabled,&QCheckBox::toggled,this,[this](bool on) { if (!m_updating && m_manager) m_manager->setEnabled(on); });
    connect(m_mode,&QComboBox::currentIndexChanged,this,[this] { if (!m_updating && m_manager) m_manager->setMode(m_mode->currentData().toString()); });
    connect(m_home,&QComboBox::activated,this,[this] { if (!m_updating && m_manager && m_home->currentData().isValid()) { m_manager->setSourceHome(m_home->currentData().toString()); updateState(); } });
    connect(m_device,&QComboBox::currentIndexChanged,this,[this] { if (!m_updating) { m_draftDevice.clear(); updateState(); } });
    connect(m_source,&QComboBox::currentIndexChanged,this,[this] { if (!m_updating) updateState(); });
    connect(m_bind,&QPushButton::clicked,this,[this] {
        if (m_manager && m_manager->bindDevice(selectedDevice(),m_source->currentData().toString())) { m_draftDevice.clear(); updateState(); }
    });
    connect(m_follow,&QPushButton::clicked,this,[this] {
        if (m_manager && m_manager->followSource(selectedDevice())) { m_draftDevice.clear(); updateState(); }
    });
    connect(m_rename,&QPushButton::clicked,this,[this] {
        if (m_manager && m_manager->snapshot().value("revision").toString() == m_draftRevision) m_manager->rename(selectedDevice(),m_name->text());
    });
    connect(m_move,&QPushButton::clicked,this,[this] {
        if (m_manager && m_manager->snapshot().value("revision").toString() == m_draftRevision) m_manager->moveRoom(selectedDevice(),m_room->currentData().toString());
    });
    connect(m_rebase,&QPushButton::clicked,this,[this] {
        if (m_manager) { m_draftRevision = m_manager->snapshot().value("revision").toString(); updateState(); }
    });
    connect(m_name,&QLineEdit::textEdited,this,[this] { updateState(); });
    connect(m_ack,&QPushButton::clicked,this,[this] { if (m_manager) m_manager->acknowledgeUncertain(); });
    if (m_manager) {
        connect(m_manager,&TuyaMetadataManager::changed,this,&TuyaMetadataPanel::updateState);
        connect(m_manager,&TuyaMetadataManager::operationFinished,this,[this](const QString &,const QString &,bool success,const QString &) { if (success) m_draftDevice.clear(); updateState(); });
    }
    updateState();
}

MatterDevice TuyaMetadataPanel::selectedDevice() const {
    if (!m_fixed.key.isEmpty()) {
        if (m_manager) for (const auto &device : m_manager->devices()) if (device.key == m_fixed.key) return device;
        return m_fixed;
    }
    if (m_manager) for (const auto &device : m_manager->devices()) if (device.key == m_device->currentData().toString()) return device;
    return {};
}

void TuyaMetadataPanel::setDevice(const MatterDevice &device) { m_fixed = device; m_draftDevice.clear(); updateState(); }

void TuyaMetadataPanel::updateState() {
    if (m_updating || !m_manager) return;
    m_updating = true;
    if (m_panelProfile != m_manager->profile()) { m_panelProfile = m_manager->profile(); m_draftDevice.clear(); m_draftRevision.clear(); m_token->clear(); m_url->clear(); }
    const auto snap = m_manager->snapshot();
    const auto oldKey = m_fixed.key.isEmpty() ? m_device->currentData().toString() : m_fixed.key;
    m_device->clear();
    if (!m_fixed.key.isEmpty()) m_device->addItem(m_fixed.name.isEmpty() ? m_fixed.key : m_fixed.name,m_fixed.key);
    else for (const auto &device : m_manager->devices()) m_device->addItem((device.name.isEmpty() ? device.key : device.name) + " · " + device.key,device.key);
    auto selected = m_device->findData(oldKey); if (selected >= 0) m_device->setCurrentIndex(selected);
    const auto device = selectedDevice(); const auto state = m_manager->state(device);
    m_device->setEnabled(m_fixed.key.isEmpty());
    if (!m_url->hasFocus() && !m_manager->baseUrl().isEmpty()) m_url->setText(m_manager->baseUrl().toString(QUrl::FullyEncoded));
    m_enabled->setChecked(m_manager->enabled()); m_mode->setCurrentIndex(m_mode->findData(m_manager->mode()));
    m_enabled->setEnabled(!m_manager->profile().isEmpty()); m_mode->setEnabled(!m_manager->profile().isEmpty());
    m_status->setText(m_manager->statusText());
    m_connect->setEnabled(!m_manager->profile().isEmpty() && !m_manager->requestInProgress());
    m_refresh->setEnabled(m_manager->isConfigured() && !m_manager->requestInProgress());
    m_home->clear(); m_home->addItem(QStringLiteral("请选择明确来源家庭"),QVariant());
    for (auto value : snap.value("homes").toArray()) { const auto home = value.toObject(); m_home->addItem(home.value("name").toString(),home.value("id").toString()); }
    if (snap.value("sourceKind") == "tuya-project-custom" && !snap.value("roomsAvailable").toBool()) m_home->addItem(QStringLiteral("Custom 项目：仅设备名称，无 App 家庭/房间"),QString());
    if (m_manager->sourceHomeConfigured()) m_home->setCurrentIndex(m_home->findData(m_manager->sourceHomeId()));
    m_home->setEnabled(!snap.isEmpty() && !state.busy);
    auto oldSource = m_source->currentData().toString();
    if (m_draftDevice != device.key || oldSource.isEmpty()) oldSource = state.bound ? state.deviceId : m_manager->suggestedBinding(device);
    m_source->clear(); m_source->addItem(QStringLiteral("请选择源设备；uniqueId 建议仍需确认"),QVariant());
    const auto suggestion = m_manager->suggestedBinding(device);
    for (auto value : snap.value("devices").toArray()) {
        const auto src = value.toObject();
        if (!m_manager->sourceHomeConfigured() || (!m_manager->sourceHomeId().isEmpty() && src.value("homeId").toString() != m_manager->sourceHomeId())) continue;
        const auto id = src.value("id").toString();
        m_source->addItem(src.value("name").toString() + (id == suggestion ? QStringLiteral(" [uniqueId 精确建议]") : QString()),id);
    }
    selected = m_source->findData(oldSource); if (selected >= 0) m_source->setCurrentIndex(selected);
    const bool present = !device.key.isEmpty();
    m_bind->setEnabled(present && m_manager->isConfigured() && m_manager->sourceHomeConfigured() && m_source->currentData().isValid() && !state.busy);
    m_follow->setEnabled(state.available && !state.busy && m_manager->enabled());
    if (m_draftDevice != device.key) {
        m_draftDevice = device.key; m_draftRevision = snap.value("revision").toString();
        m_name->setText(state.managedName.isEmpty() ? state.fullName : state.managedName);
    }
    const auto roomChoice = m_room->currentData().toString(); m_room->clear();
    m_room->addItem(QStringLiteral("仅选择已知来源房间；不支持猜测/批量替换"),QVariant());
    for (auto value : snap.value("rooms").toArray()) {
        const auto room = value.toObject(); if (room.value("homeId").toString() == m_manager->sourceHomeId()) m_room->addItem(room.value("name").toString(),room.value("id").toString());
    }
    selected = m_room->findData(roomChoice.isEmpty() ? state.roomId : roomChoice); if (selected >= 0) m_room->setCurrentIndex(selected);
    const bool stale = !m_draftRevision.isEmpty() && m_draftRevision != snap.value("revision").toString();
    m_rename->setEnabled(state.canRename && !stale && !m_name->text().trimmed().isEmpty());
    m_move->setEnabled(state.canMoveRoom && !stale && m_room->currentData().isValid());
    m_rebase->setEnabled(stale && !state.busy && state.available);
    m_name->setEnabled(present && state.bound && !state.busy); m_room->setEnabled(state.canMoveRoom && !stale);
    m_values->setText(QStringLiteral("完整来源名称：%1\n独立镜像名称：%2\n家庭：%3\n房间：%4\n最后权威观测：%5\n设备绑定：%6\n写入能力：名称 %7 / 房间 %8")
        .arg(state.fullName,state.managedName,state.homeName,
             snap.value("roomsAvailable").toBool() ? state.roomName : QStringLiteral("未知 / 此来源不提供房间；保留旧资料"),
             state.observedAt,state.bound ? QStringLiteral("已明确绑定") : QStringLiteral("未绑定"),
             state.canRename ? QStringLiteral("可用") : QStringLiteral("不可用"),state.canMoveRoom ? QStringLiteral("可用") : QStringLiteral("不可用")));
    QString hint;
    if (m_manager->profile().isEmpty()) hint = QStringLiteral("请先连接正式 OHF Matter 控制器；演示模式不能建立云同步绑定。");
    else if (stale) hint = QStringLiteral("来源版本已变化。草稿保留；检查当前资料后明确确认新版本，再提交。采用来源会替换此独立草稿。");
    else if (state.conflict) hint = QStringLiteral("基线与来源存在冲突。请明确采用来源，或核实最新来源后重新编辑；不会自动反写。");
    else if (!state.hasBaseline) hint = QStringLiteral("先确认源设备，再采用来源建立基线。标准 Matter 短标签不作为云名称编辑；本地别名也不会触发云写入。");
    else if (!state.canRename) hint = QStringLiteral("此来源未声明可验证的名称写入，或同步尚未启用。可读取和跟随资料；不猜测云接口。令牌不会写入配置。");
    else hint = QStringLiteral("编辑此独立名称并明确确认后才发送一次条件写入。来源变化会阻止旧草稿；超时/未知结果不会自动重试。");
    m_hint->setText(hint); m_ack->setEnabled(m_manager->hasUncertainWrite() && state.available && !m_manager->requestInProgress());
    m_reset->setEnabled(!state.busy);
    m_updating = false;
}
