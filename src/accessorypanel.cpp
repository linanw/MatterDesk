#include "accessorypanel.h"
#include "camerametadatapanel.h"
#include "homestore.h"
#include "matterclient.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>

namespace {
QString actionName(const QString &action) {
    static const QHash<QString, QString> names{{"power", "电源"}, {"brightness", "亮度"},
        {"colorTemperature", "色温"}, {"color", "颜色"}, {"lock", "门锁"}, {"position", "开合位置"},
        {"thermostatMode", "温控模式"}, {"heatSetpoint", "制热温度"}, {"coolSetpoint", "制冷温度"},
        {"fanPercent", "风速"}, {"fanMode", "风扇模式"}};
    return names.value(action, action);
}
QString modeName(const QString &mode) {
    static const QHash<QString, QString> names{{"off", "关闭"}, {"heat", "制热"}, {"cool", "制冷"},
        {"auto", "自动"}, {"low", "低速"}, {"medium", "中速"}, {"high", "高速"}, {"on", "开启"},
        {"smart", "智能"}};
    return names.value(mode, mode);
}
QString reportedValue(const MatterDevice &device, const QString &action) {
    const auto value = device.controlState.value(action);
    if (action == "lock") {
        const auto lock = device.controlState.value("lockState").toString();
        static const QHash<QString, QString> states{{"locked", "已上锁"}, {"unlocked", "已解锁"},
            {"notFullyLocked", "未完全锁定"}, {"unlatched", "门闩已释放"}};
        if (states.contains(lock)) return states.value(lock);
    }
    if (value.isUndefined() || value.isNull()) return "未知";
    if (action == "power") return value.toBool() ? "已开启" : "已关闭";
    if (action == "lock") return value.toBool() ? "已上锁" : "已解锁";
    if (action == "color") {
        const auto color = value.toObject();
        if (!color.contains("hue") || !color.contains("saturation")) return "未知";
        return QString("色相 %1° · 饱和度 %2%").arg(color.value("hue").toDouble(), 0, 'f', 0)
            .arg(color.value("saturation").toDouble(), 0, 'f', 0);
    }
    if (action == "colorTemperature") {
        const double mired = value.toDouble();
        return mired > 0 ? QString("%1 K · %2 mired").arg(qRound(1000000.0 / mired)).arg(mired, 0, 'f', 0) : "未知";
    }
    if (action.endsWith("Setpoint")) return QString::number(value.toDouble(), 'f', 1) + " °C";
    if (value.isDouble()) return QString::number(value.toDouble(), 'f', 0) + "%";
    if (value.isString()) return modeName(value.toString());
    return "未知";
}
QDoubleSpinBox *numberBox(const QString &action, const QJsonObject &limit, QWidget *parent) {
    auto *spin = new QDoubleSpinBox(parent);
    const bool temperature = action.endsWith("Setpoint");
    const double fallbackMin = temperature ? 5 : action == "colorTemperature" ? 100 : 0;
    const double fallbackMax = temperature ? 35 : action == "colorTemperature" ? 1000 : 100;
    spin->setRange(limit.value("min").toDouble(fallbackMin), limit.value("max").toDouble(fallbackMax));
    spin->setSingleStep(limit.value("step").toDouble(temperature ? 0.5 : 1));
    spin->setDecimals(temperature ? 2 : 0);
    spin->setSuffix(temperature ? " °C" : action == "colorTemperature" ? " mired" : "%");
    spin->setKeyboardTracking(false);
    spin->setMinimumWidth(112);
    spin->setAccessibleName("目标" + actionName(action));
    spin->setObjectName("target_" + action);
    return spin;
}
QFrame *section(QWidget *parent) {
    auto *frame = new QFrame(parent);
    frame->setObjectName("accessorySection");
    return frame;
}
}

struct AccessoryPanel::ControlRow {
    QString action;
    QLabel *current = nullptr;
    QWidget *targetHost = nullptr;
    QComboBox *choice = nullptr;
    QDoubleSpinBox *number = nullptr, *hue = nullptr, *saturation = nullptr;
    QSlider *slider = nullptr;
    QPushButton *apply = nullptr;
    QString request;
    bool feasible = true;
};

AccessoryPanel::AccessoryPanel(MatterClient *client, HomeStore *store, MatterDevice device, QWidget *parent)
    : QDialog(parent), m_client(client), m_store(store), m_device(std::move(device)) {
    // Cards may pass a display-name copy. Keep the controller name as the
    // fallback so clearing a custom name takes effect immediately.
    m_present = false;
    if (m_client) for (const auto &current : m_client->devices()) if (current.key == m_device.key) {
        m_device = current; m_present = true; break;
    }
    setObjectName("accessoryPanel");
    m_profile = m_store ? m_store->profile() : QString();
    setModal(false);
    setWindowTitle("配件详情");
    resize(610, 740);
    setMinimumSize(470, 420);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 22, 24, 18);
    outer->setSpacing(15);
    m_title = new QLabel(this); m_title->setObjectName("hero"); m_title->setWordWrap(true); m_title->setTextFormat(Qt::PlainText);
    outer->addWidget(m_title);
    m_connection = new QLabel(this); m_connection->setObjectName("accessoryConnection");
    m_connection->setTextFormat(Qt::PlainText);
    m_connection->setWordWrap(true); outer->addWidget(m_connection);
    auto *scroll = new QScrollArea(this); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget(scroll); auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 7, 0); bodyLayout->setSpacing(14);
    scroll->setWidget(body); outer->addWidget(scroll, 1);

    auto *controlSection = section(body); auto *controlLayout = new QVBoxLayout(controlSection);
    controlLayout->setContentsMargins(16, 15, 16, 15); controlLayout->setSpacing(12);
    auto *controlTitle = new QLabel("配件控制", controlSection); controlTitle->setObjectName("sectionTitle");
    controlLayout->addWidget(controlTitle);
    auto *controlHint = new QLabel("调整目标后点“应用”。当前值来自配件的实时回报。", controlSection);
    controlHint->setObjectName("muted"); controlHint->setWordWrap(true); controlLayout->addWidget(controlHint);
    m_controlsHost = new QWidget(controlSection); m_controlsLayout = new QVBoxLayout(m_controlsHost);
    m_controlsLayout->setContentsMargins(0, 0, 0, 0); m_controlsLayout->setSpacing(14);
    controlLayout->addWidget(m_controlsHost);
    m_readings = new QLabel(controlSection); m_readings->setWordWrap(true); m_readings->setObjectName("reading");
    m_readings->setTextFormat(Qt::PlainText);
    m_readings->setTextInteractionFlags(Qt::TextSelectableByMouse); controlLayout->addWidget(m_readings);
    m_feedback = new QLabel(controlSection); m_feedback->setObjectName("controlFeedback"); m_feedback->setWordWrap(true);
    m_feedback->setTextFormat(Qt::PlainText);
    m_feedback->setTextInteractionFlags(Qt::TextSelectableByMouse); controlLayout->addWidget(m_feedback);
    bodyLayout->addWidget(controlSection);

    if (m_client && m_device.hasCamera && CameraMetadataManager::supports(m_device)) {
        auto *cloudSection = section(body); auto *cloudLayout = new QVBoxLayout(cloudSection);
        cloudLayout->setContentsMargins(0, 0, 0, 0);
        m_cloudMetadata = new CameraMetadataPanel(m_client->cameraMetadata(), m_device, cloudSection);
        cloudLayout->addWidget(m_cloudMetadata); bodyLayout->addWidget(cloudSection);
    }

    auto *metadata = section(body); auto *metadataLayout = new QVBoxLayout(metadata); metadataLayout->setContentsMargins(16, 15, 16, 15);
    auto *metadataTitle = new QLabel("本机家庭设置", metadata); metadataTitle->setObjectName("sectionTitle"); metadataLayout->addWidget(metadataTitle);
    auto *form = new QFormLayout; form->setSpacing(10);
    m_name = new QLineEdit(metadata); m_name->setObjectName("accessoryName"); m_name->setMaxLength(80);
    m_name->setAccessibleName("本机别名"); m_name->setPlaceholderText(m_device.name);
    m_room = new QComboBox(metadata); m_room->setObjectName("accessoryRoom"); m_room->setAccessibleName("本机房间");
    form->addRow("本机别名", m_name); form->addRow("本机房间", m_room); metadataLayout->addLayout(form);
    auto *localHint = new QLabel("仅保存在这台电脑，不会修改米家 App 的设备名称或所属房间。", metadata);
    localHint->setObjectName("localMetadataHint"); localHint->setWordWrap(true); metadataLayout->addWidget(localHint);
    m_favorite = new QCheckBox("加入收藏", metadata); m_favorite->setObjectName("accessoryFavorite");
    m_hidden = new QCheckBox("在设备总览中隐藏", metadata); m_hidden->setObjectName("accessoryHidden");
    metadataLayout->addWidget(m_favorite); metadataLayout->addWidget(m_hidden);
    auto *saveLine = new QHBoxLayout; saveLine->addStretch();
    m_save = new QPushButton("保存本机设置", metadata); m_save->setObjectName("saveAccessory"); saveLine->addWidget(m_save);
    metadataLayout->addLayout(saveLine); bodyLayout->addWidget(metadata);
    connect(m_name, &QLineEdit::textEdited, this, [this] { m_metadataDirty = true; m_save->setEnabled(bool(m_store)); });
    connect(m_room, &QComboBox::activated, this, [this] { m_metadataDirty = true; m_save->setEnabled(bool(m_store)); });
    for (auto *check : {m_favorite, m_hidden}) connect(check, &QCheckBox::clicked, this, [this] { m_metadataDirty = true; m_save->setEnabled(bool(m_store)); });
    connect(m_save, &QPushButton::clicked, this, &AccessoryPanel::saveMetadata);

    auto *advanced = new QToolButton(body); advanced->setObjectName("accessoryAdvanced");
    advanced->setText("高级 · 原始 Matter 属性"); advanced->setCheckable(true); advanced->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advanced->setArrowType(Qt::RightArrow); bodyLayout->addWidget(advanced);
    m_raw = new QPlainTextEdit(body); m_raw->setObjectName("accessoryAttributes"); m_raw->setReadOnly(true);
    m_raw->setAccessibleName("原始 Matter 属性"); m_raw->setMinimumHeight(220); m_raw->hide();
    bodyLayout->addWidget(m_raw); bodyLayout->addStretch();
    connect(advanced, &QToolButton::toggled, this, [this, advanced](bool show) { m_raw->setVisible(show); advanced->setArrowType(show ? Qt::DownArrow : Qt::RightArrow); });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this); buttons->button(QDialogButtonBox::Close)->setText("关闭");
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close); outer->addWidget(buttons);
    if (m_client) {
        connect(m_client, &MatterClient::devicesChanged, this, &AccessoryPanel::refreshDevice);
        connect(m_client, &MatterClient::connectionChanged, this, &AccessoryPanel::refresh);
        connect(m_client, &MatterClient::commandFinished, this, [this](const QString &id, const QString &, bool ok, const QString &detail) {
            auto *row = m_pending.take(id); if (!row) return;
            row->request.clear();
            m_feedback->setProperty("error", !ok);
            m_feedback->setStyleSheet(ok ? "color:#3d7551" : "color:#a23e38");
            m_feedback->setText(ok ? actionName(row->action) + "命令已完成；以配件回报的当前值为准。" : actionName(row->action) + "失败：" + detail);
            refresh();
        });
    }
    if (m_store) connect(m_store, &HomeStore::changed, this, [this] {
        if (m_store->profile() != m_profile) { close(); return; }
        refreshDevice(); refreshMetadata();
    });
    refreshMetadata(); refresh();
}

AccessoryPanel::~AccessoryPanel() { qDeleteAll(m_rows); }

void AccessoryPanel::refreshDevice() {
    m_present = false;
    if (m_client) for (const auto &device : m_client->devices()) if (device.key == m_device.key) {
        m_device = device; m_present = true; break;
    }
    refresh();
}

void AccessoryPanel::refresh() {
    if (m_cloudMetadata) m_cloudMetadata->setDevice(m_device);
    m_title->setText(m_store ? m_store->displayName(m_device) : m_device.name);
    setWindowTitle(m_title->text() + " · 配件详情");
    const bool connected = m_client && m_client->isConnected();
    const bool online = connected && m_present && m_device.available && (!m_store || m_store->profile() == m_profile);
    const QString status = !connected ? "控制器未连接" : !m_present ? "配件已不在控制器中" : !m_device.available ? "配件离线" : "配件在线";
    m_connection->setText(QString("%1%2 · 节点 %3 / 端点 %4\n%5")
        .arg(m_client && m_client->isDemoMode() ? "演示 · " : "", status).arg(m_device.nodeId).arg(m_device.endpointId)
        .arg(m_device.nodeName));
    const auto signature = QString::fromUtf8(QJsonDocument(QJsonObject{{"controls", QJsonArray::fromStringList(m_device.controls)},
        {"limits", m_device.controlLimits}}).toJson(QJsonDocument::Compact));
    if (signature != m_controlSignature && m_pending.isEmpty()) { m_controlSignature = signature; rebuildControls(); }
    for (auto *row : m_rows) {
        if (row->number && row->action.endsWith("Setpoint")) {
            const auto limits = m_device.controlLimits.value(row->action).toObject();
            double low = limits.value("min").toDouble(5), high = limits.value("max").toDouble(35);
            const double deadband = m_device.controlLimits.value("setpointDeadband").toDouble(0);
            const auto other = m_device.controlState.value(row->action == "heatSetpoint" ? "coolSetpoint" : "heatSetpoint");
            if (other.isDouble()) {
                if (row->action == "heatSetpoint") high = qMin(high, other.toDouble() - deadband);
                else low = qMax(low, other.toDouble() + deadband);
            }
            row->feasible = low <= high;
            if (row->feasible) row->number->setRange(low, high);
            row->targetHost->setToolTip(other.isDouble() ? "目标范围同时遵循当前另一设定温度与配件温差限制。" : QString());
        }
        row->current->setText("当前：" + reportedValue(m_device, row->action));
        row->targetHost->setEnabled(online && row->request.isEmpty());
        row->apply->setText(row->request.isEmpty() ? "应用" : "正在发送…");
        row->apply->setEnabled(online && row->request.isEmpty() && validTarget(row));
    }
    m_readings->setText(m_device.readings.join("\n")); m_readings->setVisible(!m_device.readings.isEmpty());
    m_raw->setPlainText(QString::fromUtf8(QJsonDocument(m_device.attributes).toJson(QJsonDocument::Indented)));
}

void AccessoryPanel::rebuildControls() {
    qDeleteAll(m_rows); m_rows.clear();
    while (auto *item = m_controlsLayout->takeAt(0)) { delete item->widget(); delete item; }
    static const QStringList supported{"power", "brightness", "colorTemperature", "color", "lock", "position",
        "thermostatMode", "heatSetpoint", "coolSetpoint", "fanPercent", "fanMode"};
    for (const auto &action : supported) {
        if (!m_device.controls.contains(action)) continue;
        auto *row = new ControlRow; row->action = action;
        auto *host = new QWidget(m_controlsHost); auto *layout = new QVBoxLayout(host); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(6);
        auto *caption = new QHBoxLayout;
        auto *label = new QLabel(actionName(action), host); label->setObjectName("controlTitle"); caption->addWidget(label);
        row->current = new QLabel(host); row->current->setObjectName("current_" + action); row->current->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->current->setTextFormat(Qt::PlainText);
        row->current->setWordWrap(true); caption->addWidget(row->current, 1); layout->addLayout(caption);
        auto *line = new QHBoxLayout; line->setSpacing(8);
        row->targetHost = new QWidget(host); auto *target = new QHBoxLayout(row->targetHost); target->setContentsMargins(0, 0, 0, 0); target->setSpacing(8);
        auto *targetLabel = new QLabel("目标", row->targetHost); targetLabel->setObjectName("muted"); target->addWidget(targetLabel);
        const auto value = m_device.controlState.value(action);
        const auto limit = m_device.controlLimits.value(action).toObject();
        if (action == "power" || action == "lock" || action.endsWith("Mode")) {
            row->choice = new QComboBox(row->targetHost); row->choice->setObjectName("target_" + action); row->choice->setAccessibleName("目标" + actionName(action));
            row->choice->setPlaceholderText("选择目标");
            if (action == "power" || action == "lock") {
                row->choice->addItem(action == "lock" ? "上锁" : "开启", true);
                row->choice->addItem(action == "lock" ? "解锁" : "关闭", false);
                row->choice->setCurrentIndex(value.isBool() ? value.toBool() ? 0 : 1 : -1);
            } else {
                for (const auto &mode : m_device.controlLimits.value(action).toArray()) if (mode.isString()) row->choice->addItem(modeName(mode.toString()), mode.toString());
                row->choice->setCurrentIndex(row->choice->findData(value.toVariant()));
            }
            target->addWidget(row->choice, 1);
            connect(row->choice, &QComboBox::currentIndexChanged, this, [this, row] { row->apply->setEnabled(m_client && m_client->isConnected() && m_present && m_device.available && row->request.isEmpty() && validTarget(row)); });
        } else if (action == "color") {
            row->hue = new QDoubleSpinBox(row->targetHost); row->hue->setObjectName("target_colorHue"); row->hue->setAccessibleName("目标色相");
            row->hue->setRange(limit.value("hueMin").toDouble(0), limit.value("hueMax").toDouble(360)); row->hue->setDecimals(0); row->hue->setSuffix("°");
            row->saturation = new QDoubleSpinBox(row->targetHost); row->saturation->setObjectName("target_colorSaturation"); row->saturation->setAccessibleName("目标饱和度");
            row->saturation->setRange(limit.value("saturationMin").toDouble(0), limit.value("saturationMax").toDouble(100)); row->saturation->setDecimals(0); row->saturation->setSuffix("%");
            const auto color = value.toObject(); row->hue->setValue(color.value("hue").toDouble()); row->saturation->setValue(color.value("saturation").toDouble(100));
            row->hue->setToolTip("色相"); row->saturation->setToolTip("饱和度");
            target->addWidget(row->hue); target->addWidget(row->saturation);
            auto *picker = new QPushButton("选色", row->targetHost); picker->setObjectName("colorPickerButton"); picker->setAccessibleName("选择目标颜色"); target->addWidget(picker);
            connect(picker, &QPushButton::clicked, this, [this, row] {
                auto *dialog = new QColorDialog(QColor::fromHsvF(qMin(row->hue->value(), 359.99) / 360.0, row->saturation->value() / 100.0, 1.0), this);
                dialog->setWindowTitle("选择目标颜色"); dialog->setAttribute(Qt::WA_DeleteOnClose);
                const QPointer<QDoubleSpinBox> hue(row->hue), saturation(row->saturation);
                // Capability updates can rebuild the sheet while the picker is open.
                connect(dialog, &QColorDialog::colorSelected, this, [hue, saturation](const QColor &color) {
                    if (color.isValid() && hue && saturation) {
                        hue->setValue(qMax(0.0, color.hsvHueF()) * 360.0); saturation->setValue(color.hsvSaturationF() * 100.0);
                    }
                });
                dialog->open();
            });
        } else {
            row->number = numberBox(action, limit, row->targetHost);
            if (value.isDouble()) row->number->setValue(value.toDouble());
            if (action == "brightness" || action == "position" || action == "fanPercent") {
                row->slider = new QSlider(Qt::Horizontal, row->targetHost); row->slider->setObjectName("slider_" + action); row->slider->setAccessibleName("目标" + actionName(action));
                row->slider->setRange(qCeil(row->number->minimum()), qFloor(row->number->maximum())); row->slider->setValue(qRound(row->number->value()));
                row->slider->setSingleStep(qMax(1, qRound(row->number->singleStep())));
                connect(row->slider, &QSlider::valueChanged, row->number, &QDoubleSpinBox::setValue);
                connect(row->number, &QDoubleSpinBox::valueChanged, row->slider, [row](double number) { QSignalBlocker blocker(row->slider); row->slider->setValue(qRound(number)); });
                target->addWidget(row->slider, 1);
            } else target->addStretch();
            target->addWidget(row->number);
        }
        line->addWidget(row->targetHost, 1);
        row->apply = new QPushButton("应用", host); row->apply->setObjectName("apply_" + action); row->apply->setAccessibleName("应用" + actionName(action));
        row->apply->setMinimumWidth(78); line->addWidget(row->apply); layout->addLayout(line);
        if (action == "position") { auto *hint = new QLabel("0% 完全关闭 · 100% 完全打开", host); hint->setObjectName("muted"); layout->addWidget(hint); }
        if (action == "lock") { auto *hint = new QLabel("解锁仅在此处由你直接操作。", host); hint->setObjectName("muted"); layout->addWidget(hint); }
        connect(row->apply, &QPushButton::clicked, this, [this, row] { apply(row); });
        m_rows.append(row); m_controlsLayout->addWidget(host);
    }
    if (m_rows.isEmpty()) { auto *empty = new QLabel("此配件未提供可写控制。可在下方查看读数与原始属性。", m_controlsHost); empty->setObjectName("muted"); empty->setWordWrap(true); m_controlsLayout->addWidget(empty); }
}

QJsonValue AccessoryPanel::targetValue(const ControlRow *row) const {
    if (row->choice) return QJsonValue::fromVariant(row->choice->currentData());
    if (row->number) return row->number->value();
    if (row->hue) return QJsonObject{{"hue", row->hue->value()}, {"saturation", row->saturation->value()}};
    return {};
}
bool AccessoryPanel::validTarget(const ControlRow *row) const { return row->feasible && (!row->choice || row->choice->currentIndex() >= 0); }
void AccessoryPanel::apply(ControlRow *row) {
    if (!m_client || !m_client->isConnected() || !m_present || !m_device.available || !row->request.isEmpty() || !validTarget(row)
        || (m_store && m_store->profile() != m_profile)) return;
    row->request = m_client->control(m_device, row->action, targetValue(row));
    if (row->request.isEmpty()) { m_feedback->setText("无法发送命令。"); return; }
    m_pending.insert(row->request, row);
    m_feedback->setStyleSheet("color:#77756b"); m_feedback->setText("正在等待" + actionName(row->action) + "命令确认…"); refresh();
}

void AccessoryPanel::refreshMetadata() {
    if (m_metadataDirty) return;
    const auto metadata = m_store ? m_store->accessory(m_device.key) : QJsonObject{};
    const QSignalBlocker nameBlock(m_name), roomBlock(m_room), favoriteBlock(m_favorite), hiddenBlock(m_hidden);
    m_name->setText(metadata.value("name").toString()); m_name->setPlaceholderText(m_device.name);
    m_room->clear(); m_room->addItem("未分配房间", QString());
    if (m_store) for (const auto &entry : m_store->rooms()) { const auto room = entry.toObject(); m_room->addItem(room.value("name").toString(), room.value("id").toString()); }
    m_room->setCurrentIndex(qMax(0, m_room->findData(metadata.value("roomId").toString())));
    m_favorite->setChecked(metadata.value("favorite").toBool()); m_hidden->setChecked(metadata.value("hidden").toBool());
    for (auto *widget : {static_cast<QWidget *>(m_name), static_cast<QWidget *>(m_room), static_cast<QWidget *>(m_favorite), static_cast<QWidget *>(m_hidden)}) widget->setEnabled(bool(m_store));
    m_save->setEnabled(false);
}
void AccessoryPanel::saveMetadata() {
    if (!m_store || m_store->profile() != m_profile) return;
    auto metadata = m_store->accessory(m_device.key);
    metadata.insert("name", m_name->text().trimmed()); metadata.insert("roomId", m_room->currentData().toString());
    metadata.insert("favorite", m_favorite->isChecked()); metadata.insert("hidden", m_hidden->isChecked());
    if (!m_store->setAccessory(m_device.key, metadata)) { m_feedback->setStyleSheet("color:#a23e38"); m_feedback->setText("设置未保存：" + m_store->lastError()); return; }
    m_metadataDirty = false; refreshMetadata(); refresh();
    m_feedback->setStyleSheet("color:#3d7551"); m_feedback->setText("本机设置已保存。");
}
