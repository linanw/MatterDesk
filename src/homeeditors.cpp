#include "homeeditors.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimeEdit>
#include <QVBoxLayout>
#include <functional>

namespace {
QString actionName(const QString &action) {
    static const QHash<QString, QString> names{{"power", "电源"}, {"brightness", "亮度"}, {"colorTemperature", "色温"},
        {"color", "颜色"}, {"lock", "上锁"}, {"position", "开合位置"}, {"thermostatMode", "温控模式"},
        {"heatSetpoint", "制热温度"}, {"coolSetpoint", "制冷温度"}, {"fanPercent", "风速"}, {"fanMode", "风扇模式"},
        {"temperature", "温度"}, {"humidity", "湿度"}, {"contact", "门窗状态"}, {"occupancy", "有人 / 运动"},
        {"illuminance", "照度"}, {"battery", "电池"}, {"airQuality", "空气质量"}, {"co2", "二氧化碳"},
        {"pm25", "PM2.5"}, {"powerWatts", "功率"}, {"energyKwh", "累计电量"}};
    return names.value(action, action);
}
QString modeName(const QString &mode) {
    static const QHash<QString, QString> names{{"off", "关闭"}, {"heat", "制热"}, {"cool", "制冷"},
        {"auto", "自动"}, {"low", "低速"}, {"medium", "中速"}, {"high", "高速"}, {"on", "开启"}, {"smart", "智能"}};
    return names.value(mode, mode);
}
const QStringList writable{"power", "brightness", "colorTemperature", "color", "lock", "position",
    "thermostatMode", "heatSetpoint", "coolSetpoint", "fanPercent", "fanMode"};
const QStringList readable{"temperature", "humidity", "contact", "occupancy", "illuminance", "battery",
    "airQuality", "co2", "pm25", "powerWatts", "energyKwh"};
QString actionSummary(const QJsonObject &action, const QList<MatterDevice> &devices) {
    QString name = "已移除的配件";
    for (const auto &device : devices) if (device.key == action.value("deviceKey").toString()) { name = device.name; break; }
    const QString key = action.value("action").toString();
    const auto value = action.value("value"); QString text;
    if (value.isBool()) text = key == "lock" ? value.toBool() ? "上锁" : "解锁" : value.toBool() ? "开启" : "关闭";
    else if (value.isString()) text = modeName(value.toString());
    else if (value.isObject()) { const auto color = value.toObject(); text = QString("%1° / %2%").arg(color.value("hue").toDouble()).arg(color.value("saturation").toDouble()); }
    else text = QString::number(value.toDouble()) + (key.endsWith("Setpoint") || key == "temperature" ? " °C" : key == "colorTemperature" ? " mired" : key == "position" || key == "brightness" || key == "fanPercent" || key == "humidity" || key == "battery" ? "%" : "");
    return name + " · " + actionName(key) + " → " + text;
}

// Shared local-draft input for a writable action or an observed equality value.
class ActionInput : public QWidget {
public:
    ActionInput(QList<MatterDevice> devices, bool observed, QWidget *parent)
        : QWidget(parent), m_devices(std::move(devices)), m_observed(observed) {
        auto *layout = new QFormLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(10);
        m_device = new QComboBox(this); m_device->setObjectName("actionDevice"); m_device->setAccessibleName("动作配件");
        for (const auto &device : m_devices) {
            bool supported = false;
            for (const auto &action : device.controls) if (writable.contains(action)) supported = true;
            if (m_observed) for (const auto &key : readable) if (device.controlState.contains(key)) supported = true;
            if (supported) m_device->addItem(device.name + (device.available ? "" : " · 离线"), device.key);
        }
        m_action = new QComboBox(this); m_action->setObjectName("actionKind"); m_action->setAccessibleName(observed ? "观察状态" : "动作类型");
        m_values = new QWidget(this); m_valuesLayout = new QHBoxLayout(m_values); m_valuesLayout->setContentsMargins(0, 0, 0, 0);
        layout->addRow("配件", m_device); layout->addRow(observed ? "状态" : "动作", m_action); layout->addRow(observed ? "等于" : "目标", m_values);
        connect(m_device, &QComboBox::currentIndexChanged, this, [this] { if (m_loading) return; rebuildActions(); emitChanged(); });
        connect(m_action, &QComboBox::currentIndexChanged, this, [this] { if (m_loading) return; rebuildValue(); emitChanged(); });
        rebuildActions();
    }
    std::function<void()> changed;
    QJsonObject value() const {
        QJsonValue result;
        if (m_choice) result = QJsonValue::fromVariant(m_choice->currentData());
        else if (m_number) result = m_number->value();
        else if (m_hue) result = QJsonObject{{"hue", m_hue->value()}, {"saturation", m_saturation->value()}};
        return {{"deviceKey", m_device->currentData().toString()}, {"action", m_action->currentData().toString()}, {"value", result}};
    }
    bool valid() const { return m_device->currentIndex() >= 0 && m_action->currentIndex() >= 0 && (!m_choice || m_choice->currentIndex() >= 0); }
    void setValue(const QJsonObject &value) {
        m_loading = true;
        m_device->setCurrentIndex(m_device->findData(value.value("deviceKey").toString()));
        m_loading = false; rebuildActions();
        m_loading = true; m_action->setCurrentIndex(m_action->findData(value.value("action").toString()));
        m_loading = false; rebuildValue();
        m_loading = true;
        const auto target = value.value("value");
        if (m_choice) m_choice->setCurrentIndex(m_choice->findData(target.toVariant()));
        if (m_number && target.isDouble()) m_number->setValue(target.toDouble());
        if (m_hue) { m_hue->setValue(target.toObject().value("hue").toDouble()); m_saturation->setValue(target.toObject().value("saturation").toDouble()); }
        m_loading = false;
    }
    const QList<MatterDevice> &devices() const { return m_devices; }
private:
    MatterDevice device() const {
        for (const auto &device : m_devices) if (device.key == m_device->currentData().toString()) return device;
        return {};
    }
    void emitChanged() { if (!m_loading && changed) changed(); }
    void rebuildActions() {
        const bool previous = m_loading; m_loading = true;
        m_action->clear(); const auto current = device();
        for (const auto &action : writable) if (current.controls.contains(action) && !(m_observed && action == "color")) m_action->addItem(m_observed && action == "lock" ? "门锁状态" : actionName(action), action);
        if (m_observed) for (const auto &action : readable) if (current.controlState.contains(action)) m_action->addItem(actionName(action), action);
        m_loading = previous; rebuildValue();
    }
    void rebuildValue() {
        while (auto *item = m_valuesLayout->takeAt(0)) { delete item->widget(); delete item; }
        m_choice = nullptr; m_number = nullptr; m_hue = nullptr; m_saturation = nullptr;
        const auto action = m_action->currentData().toString(); const auto current = device();
        const auto limit = current.controlLimits.value(action).toObject();
        const auto state = current.controlState.value(action);
        if (action.isEmpty()) { m_valuesLayout->addWidget(new QLabel("无可用状态或动作", m_values)); return; }
        if (action == "power" || action == "lock" || action == "contact" || action == "occupancy" || action.endsWith("Mode") || action == "airQuality") {
            m_choice = new QComboBox(m_values); m_choice->setObjectName("actionValue"); m_choice->setAccessibleName("动作目标值");
            if (action == "lock") {
                m_choice->addItem(m_observed ? "已上锁" : "上锁", true);
                if (m_observed) m_choice->addItem("已解锁", false);
            }
            else if (action == "power" || action == "contact" || action == "occupancy") {
                m_choice->addItem(action == "contact" ? "已关闭" : action == "occupancy" ? "有人 / 检测到运动" : "开启", true);
                m_choice->addItem(action == "contact" ? "已打开" : action == "occupancy" ? "无人 / 未检测到运动" : "关闭", false);
            } else if (action == "airQuality") {
                const QStringList names{"优", "良好", "一般", "较差", "差", "非常差"};
                for (int i = 0; i < names.size(); ++i) m_choice->addItem(names.at(i), i + 1);
            } else for (const auto &mode : current.controlLimits.value(action).toArray()) if (mode.isString()) m_choice->addItem(modeName(mode.toString()), mode.toString());
            const int index = m_choice->findData(state.toVariant()); if (index >= 0) m_choice->setCurrentIndex(index);
            m_valuesLayout->addWidget(m_choice, 1);
            connect(m_choice, &QComboBox::currentIndexChanged, this, [this] { emitChanged(); });
        } else if (action == "color") {
            m_hue = new QDoubleSpinBox(m_values); m_hue->setObjectName("actionHue"); m_hue->setAccessibleName("目标色相");
            m_hue->setRange(limit.value("hueMin").toDouble(0), limit.value("hueMax").toDouble(360)); m_hue->setDecimals(0); m_hue->setSuffix("°");
            m_saturation = new QDoubleSpinBox(m_values); m_saturation->setObjectName("actionSaturation"); m_saturation->setAccessibleName("目标饱和度");
            m_saturation->setRange(limit.value("saturationMin").toDouble(0), limit.value("saturationMax").toDouble(100)); m_saturation->setDecimals(0); m_saturation->setSuffix("%");
            m_hue->setValue(state.toObject().value("hue").toDouble()); m_saturation->setValue(state.toObject().value("saturation").toDouble(100));
            m_valuesLayout->addWidget(new QLabel("色相", m_values)); m_valuesLayout->addWidget(m_hue);
            m_valuesLayout->addWidget(new QLabel("饱和度", m_values)); m_valuesLayout->addWidget(m_saturation);
            connect(m_hue, &QDoubleSpinBox::valueChanged, this, [this] { emitChanged(); });
            connect(m_saturation, &QDoubleSpinBox::valueChanged, this, [this] { emitChanged(); });
        } else {
            m_number = new QDoubleSpinBox(m_values); m_number->setObjectName("actionValue"); m_number->setAccessibleName("动作目标值");
            const bool temperature = action.endsWith("Setpoint") || action == "temperature";
            const bool percent = action == "brightness" || action == "position" || action == "fanPercent" || action == "humidity" || action == "battery";
            m_number->setDecimals(temperature || readable.contains(action) ? 2 : 0);
            const double fallbackMin = action.endsWith("Setpoint") ? 5 : action == "temperature" ? -273.15 : action == "powerWatts" ? -1e9 : action == "colorTemperature" ? 100 : 0;
            const double fallbackMax = action.endsWith("Setpoint") ? 35 : action == "temperature" ? 1000 : percent ? 100 : action == "colorTemperature" ? 1000 : 1e9;
            m_number->setRange(limit.value("min").toDouble(fallbackMin), limit.value("max").toDouble(fallbackMax));
            m_number->setSingleStep(limit.value("step").toDouble(temperature ? 0.5 : 1));
            m_number->setSuffix(temperature ? " °C" : percent ? "%" : action == "colorTemperature" ? " mired" : action == "illuminance" ? " lx" : action == "co2" ? " ppm" : action == "pm25" ? " µg/m³" : action == "powerWatts" ? " W" : action == "energyKwh" ? " kWh" : "");
            if (state.isDouble()) m_number->setValue(state.toDouble());
            m_number->setKeyboardTracking(false); m_valuesLayout->addWidget(m_number, 1);
            connect(m_number, &QDoubleSpinBox::valueChanged, this, [this] { emitChanged(); });
        }
    }
    QList<MatterDevice> m_devices;
    bool m_observed, m_loading = false;
    QComboBox *m_device, *m_action, *m_choice = nullptr;
    QWidget *m_values;
    QHBoxLayout *m_valuesLayout;
    QDoubleSpinBox *m_number = nullptr, *m_hue = nullptr, *m_saturation = nullptr;
};

void setupDialog(QDialog &dialog, const QString &title, const QString &subtitle, QVBoxLayout *layout) {
    dialog.setWindowTitle(title); dialog.setMinimumWidth(520);
    layout->setContentsMargins(24, 22, 24, 20); layout->setSpacing(14);
    auto *heading = new QLabel(title, &dialog); heading->setObjectName("sectionTitle"); layout->addWidget(heading);
    auto *hint = new QLabel(subtitle, &dialog); hint->setObjectName("muted"); hint->setWordWrap(true); layout->addWidget(hint);
}
bool draftValid(const QJsonObject &draft, const QList<MatterDevice> &devices, bool observed) {
    const auto action = draft.value("action").toString();
    if (!observed && action == "lock" && draft.value("value") != QJsonValue(true)) return false;
    if (observed && action == "color") return false;
    for (const auto &device : devices) if (device.key == draft.value("deviceKey").toString()) {
        if (!(device.controls.contains(action) && writable.contains(action)) && !(observed && readable.contains(action) && device.controlState.contains(action))) return false;
        const auto value = draft.value("value");
        if (action == "power" || action == "lock" || action == "contact" || action == "occupancy") return value.isBool();
        if (action.endsWith("Mode")) return device.controlLimits.value(action).toArray().contains(value);
        if (action == "color") {
            const auto color = value.toObject();
            return color.value("hue").isDouble() && color.value("saturation").isDouble() && color.value("hue").toDouble() >= 0 && color.value("hue").toDouble() <= 360 && color.value("saturation").toDouble() >= 0 && color.value("saturation").toDouble() <= 100;
        }
        if (!value.isDouble()) return false;
        const auto limit = device.controlLimits.value(action).toObject();
        return (!limit.contains("min") || value.toDouble() >= limit.value("min").toDouble()) && (!limit.contains("max") || value.toDouble() <= limit.value("max").toDouble());
    }
    return false;
}
}

QJsonObject HomeEditors::scene(QWidget *parent, QJsonObject existing, QList<MatterDevice> devices) {
    QDialog dialog(parent); dialog.setObjectName("sceneEditor"); auto *layout = new QVBoxLayout(&dialog);
    setupDialog(dialog, existing.isEmpty() ? "新建场景" : "编辑场景", "把多个配件动作保存为一个场景。编辑只修改草稿，保存后可手动运行。", layout);
    auto *name = new QLineEdit(existing.value("name").toString(), &dialog); name->setObjectName("sceneName"); name->setAccessibleName("场景名称"); name->setPlaceholderText("例如：晚安"); name->setMaxLength(80);
    auto *nameForm = new QFormLayout; nameForm->addRow("名称", name); layout->addLayout(nameForm);
    auto *list = new QListWidget(&dialog); list->setObjectName("sceneActions"); list->setAccessibleName("场景动作列表"); list->setMinimumHeight(150); list->setWordWrap(true); layout->addWidget(list);
    auto *tools = new QHBoxLayout; auto *add = new QPushButton("添加动作", &dialog); add->setObjectName("addSceneAction");
    auto *remove = new QPushButton("移除动作", &dialog); remove->setObjectName("removeSceneAction"); tools->addWidget(add); tools->addWidget(remove); tools->addStretch();
    auto *up = new QPushButton("上移", &dialog); up->setObjectName("moveSceneActionUp"); tools->addWidget(up);
    auto *down = new QPushButton("下移", &dialog); down->setObjectName("moveSceneActionDown"); tools->addWidget(down); layout->addLayout(tools);
    auto *input = new ActionInput(devices, false, &dialog); layout->addWidget(input);
    auto *hint = new QLabel("选择列表中的动作可编辑其目标。所有动作按列表顺序运行；场景不会自动解锁门锁。", &dialog); hint->setObjectName("muted"); hint->setWordWrap(true); layout->addWidget(hint);
    auto *error = new QLabel(&dialog); error->setObjectName("editorError"); error->setStyleSheet("color:#a23e38"); error->setWordWrap(true); layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog); buttons->button(QDialogButtonBox::Save)->setText("保存场景"); buttons->button(QDialogButtonBox::Cancel)->setText("取消"); layout->addWidget(buttons);
    QJsonArray actions = existing.value("actions").toArray();
    bool loading = false;
    const auto refresh = [&] {
        const int selected = list->currentRow(); const bool selection = selected >= 0 && selected < actions.size();
        input->setEnabled(selection); remove->setEnabled(selection); up->setEnabled(selection && selected > 0); down->setEnabled(selection && selected + 1 < actions.size());
        bool valid = !name->text().trimmed().isEmpty() && !actions.isEmpty() && actions.size() <= 64;
        bool invalidAction = false; for (const auto &action : actions) if (!draftValid(action.toObject(), devices, false)) { valid = false; invalidAction = true; }
        add->setEnabled(input->valid() && actions.size() < 64); buttons->button(QDialogButtonBox::Save)->setEnabled(valid);
        error->setText(invalidAction ? "有动作的配件或能力已不可用，或目标超出当前范围。请编辑或移除该动作。" : actions.isEmpty() ? "添加至少一个配件动作。" : name->text().trimmed().isEmpty() ? "请填写场景名称。" : QString());
    };
    const auto rebuildList = [&] {
        const int selected = list->currentRow(); loading = true; list->clear();
        for (const auto &action : actions) list->addItem(actionSummary(action.toObject(), devices));
        list->setCurrentRow(qMin(selected, actions.size() - 1)); loading = false;
    };
    for (const auto &action : actions) list->addItem(actionSummary(action.toObject(), devices));
    QObject::connect(list, &QListWidget::currentRowChanged, &dialog, [&](int row) { if (loading) return; if (row >= 0 && row < actions.size()) input->setValue(actions.at(row).toObject()); refresh(); });
    input->changed = [&] { const int row = list->currentRow(); if (row >= 0 && row < actions.size()) { actions[row] = input->value(); list->item(row)->setText(actionSummary(input->value(), devices)); } refresh(); };
    QObject::connect(add, &QPushButton::clicked, &dialog, [&] { if (!input->valid()) return; actions.append(input->value()); list->addItem(actionSummary(input->value(), devices)); list->setCurrentRow(actions.size() - 1); refresh(); });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [&] { const int row = list->currentRow(); if (row < 0) return; actions.removeAt(row); rebuildList(); if (list->currentRow() >= 0) input->setValue(actions.at(list->currentRow()).toObject()); refresh(); });
    const auto move = [&](int delta) { const int row = list->currentRow(); const int target = row + delta; if (row < 0 || target < 0 || target >= actions.size()) return; const auto action = actions.takeAt(row); actions.insert(target, action); rebuildList(); list->setCurrentRow(target); refresh(); };
    QObject::connect(up, &QPushButton::clicked, &dialog, [&] { move(-1); }); QObject::connect(down, &QPushButton::clicked, &dialog, [&] { move(1); });
    QObject::connect(name, &QLineEdit::textChanged, &dialog, [&] { refresh(); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (!actions.isEmpty()) list->setCurrentRow(0);
    refresh();
    if (dialog.exec() != QDialog::Accepted) return {};
    existing.insert("name", name->text().trimmed()); existing.insert("actions", actions); return existing;
}

QJsonObject HomeEditors::automation(QWidget *parent, QJsonObject existing, QList<MatterDevice> devices, QJsonArray scenes) {
    QDialog dialog(parent); dialog.setObjectName("automationEditor"); auto *layout = new QVBoxLayout(&dialog);
    setupDialog(dialog, existing.isEmpty() ? "新建自动化" : "编辑自动化", "在选定时间或配件状态变化时运行一个场景。规则只在此应用运行且控制器连接时生效。", layout);
    auto *form = new QFormLayout; form->setSpacing(11);
    auto *name = new QLineEdit(existing.value("name").toString(), &dialog); name->setObjectName("automationName"); name->setAccessibleName("自动化名称"); name->setPlaceholderText("例如：每天清晨开灯"); name->setMaxLength(80); form->addRow("名称", name);
    auto *scene = new QComboBox(&dialog); scene->setObjectName("automationScene"); scene->setAccessibleName("运行的场景");
    for (const auto &value : scenes) { const auto item = value.toObject(); scene->addItem(item.value("name").toString(), item.value("id").toString()); }
    if (!existing.isEmpty()) scene->setCurrentIndex(scene->findData(existing.value("sceneId").toString()));
    form->addRow("运行场景", scene);
    auto *kind = new QComboBox(&dialog); kind->setObjectName("automationTrigger"); kind->setAccessibleName("触发方式"); kind->addItem("指定时间", "time"); kind->addItem("配件状态变为", "device"); form->addRow("触发方式", kind); layout->addLayout(form);
    auto *pages = new QStackedWidget(&dialog); layout->addWidget(pages);
    auto *timePage = new QWidget(pages); auto *timeLayout = new QVBoxLayout(timePage); timeLayout->setContentsMargins(0, 0, 0, 0);
    auto *timeForm = new QFormLayout; auto *time = new QTimeEdit(QTime(7, 0), timePage); time->setDisplayFormat("HH:mm"); time->setObjectName("automationTime"); time->setAccessibleName("自动化时间"); timeForm->addRow("本地时间", time); timeLayout->addLayout(timeForm);
    auto *dayLine = new QHBoxLayout; QList<QCheckBox *> days;
    const QStringList dayNames{"一", "二", "三", "四", "五", "六", "日"};
    for (int i = 0; i < 7; ++i) { auto *day = new QCheckBox(dayNames.at(i), timePage); day->setObjectName("automationDay" + QString::number(i + 1)); day->setAccessibleName("星期" + dayNames.at(i)); day->setChecked(true); dayLine->addWidget(day); days.append(day); }
    timeLayout->addLayout(dayLine);
    auto *timeHint = new QLabel("使用此电脑的本地时区。错过的触发时间不会补跑。", timePage); timeHint->setObjectName("muted"); timeHint->setWordWrap(true); timeLayout->addWidget(timeHint); pages->addWidget(timePage);
    auto *devicePage = new QWidget(pages); auto *deviceLayout = new QVBoxLayout(devicePage); deviceLayout->setContentsMargins(0, 0, 0, 0);
    auto *input = new ActionInput(devices, true, devicePage); deviceLayout->addWidget(input);
    auto *deviceHint = new QLabel("当已知状态从其他值变为此值时运行一次。首次读取与重新连接不会触发；数值使用精确相等。", devicePage); deviceHint->setObjectName("muted"); deviceHint->setWordWrap(true); deviceLayout->addWidget(deviceHint); pages->addWidget(devicePage);
    auto *enabled = new QCheckBox("启用此自动化", &dialog); enabled->setObjectName("automationEnabled"); enabled->setChecked(existing.value("enabled").toBool(false)); layout->addWidget(enabled);
    auto *error = new QLabel(&dialog); error->setObjectName("editorError"); error->setStyleSheet("color:#a23e38"); error->setWordWrap(true); layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog); buttons->button(QDialogButtonBox::Save)->setText("保存自动化"); buttons->button(QDialogButtonBox::Cancel)->setText("取消"); layout->addWidget(buttons);
    const auto trigger = existing.value("trigger").toObject();
    if (trigger.value("type").toString() == "device") { kind->setCurrentIndex(1); input->setValue(trigger); }
    else if (!trigger.isEmpty()) {
        const auto savedTime = QTime::fromString(trigger.value("time").toString(), "HH:mm"); if (savedTime.isValid()) time->setTime(savedTime);
        const auto savedDays = trigger.value("days").toArray(); for (int i = 0; i < days.size(); ++i) days.at(i)->setChecked(savedDays.contains(i + 1));
    }
    const auto refresh = [&] {
        pages->setCurrentIndex(kind->currentIndex()); bool someDay = false; for (auto *day : days) someDay |= day->isChecked();
        const bool validInput = kind->currentIndex() == 0 ? someDay : input->valid() && draftValid(input->value(), devices, true);
        buttons->button(QDialogButtonBox::Save)->setEnabled(!name->text().trimmed().isEmpty() && scene->currentIndex() >= 0 && validInput);
        error->setText(scene->currentIndex() < 0 ? "请先创建一个场景，再选择要运行的场景。" : name->text().trimmed().isEmpty() ? "请填写自动化名称。" : !validInput ? kind->currentIndex() == 0 ? "至少选择一个星期。" : "请选择有效的配件状态与目标值。" : QString());
    };
    QObject::connect(name, &QLineEdit::textChanged, &dialog, [&] { refresh(); }); QObject::connect(scene, &QComboBox::currentIndexChanged, &dialog, [&] { refresh(); });
    QObject::connect(kind, &QComboBox::currentIndexChanged, &dialog, [&] { refresh(); }); for (auto *day : days) QObject::connect(day, &QCheckBox::toggled, &dialog, [&] { refresh(); }); input->changed = refresh;
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); refresh();
    if (dialog.exec() != QDialog::Accepted) return {};
    QJsonObject resultTrigger;
    if (kind->currentIndex() == 0) { QJsonArray selectedDays; for (int i = 0; i < days.size(); ++i) if (days.at(i)->isChecked()) selectedDays.append(i + 1); resultTrigger = {{"type", "time"}, {"time", time->time().toString("HH:mm")}, {"days", selectedDays}}; }
    else { resultTrigger = input->value(); resultTrigger.insert("type", "device"); }
    existing.insert("name", name->text().trimmed()); existing.insert("sceneId", scene->currentData().toString()); existing.insert("enabled", enabled->isChecked()); existing.insert("trigger", resultTrigger); return existing;
}
