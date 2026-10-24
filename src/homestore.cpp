#include "homestore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace {
constexpr int MaxItems = 128;
constexpr int MaxAccessories = 2000;
constexpr int MaxActions = 64;
constexpr int MaxHistory = 200;
constexpr qint64 MaxFileBytes = 2 * 1024 * 1024;
QJsonObject blankDocument() {
    return {{"version", 1}, {"homeName", QStringLiteral("我的家")}, {"rooms", QJsonArray{}},
            {"zones", QJsonArray{}}, {"scenes", QJsonArray{}}, {"rules", QJsonArray{}},
            {"accessories", QJsonObject{}}, {"activity", QJsonArray{}}, {"runtime", QJsonObject{}}};
}
bool validText(const QString &text, int maximum = 80) {
    if (text.trimmed().isEmpty() || text.size() > maximum) return false;
    for (const auto ch : text) if (ch.category() == QChar::Other_Control) return false;
    return true;
}
bool validId(const QString &id) {
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9_-]{1,80}$"));
    return expression.match(id).hasMatch();
}
bool validDeviceKey(const QString &key) {
    static const QRegularExpression expression(QStringLiteral("^[0-9]{1,20}/[0-9]{1,5}$"));
    return expression.match(key).hasMatch();
}
bool containsId(const QJsonArray &items, const QString &id) {
    for (const auto &item : items) if (item.toObject().value("id").toString() == id) return true;
    return false;
}
bool number(const QJsonValue &value, double minimum, double maximum) {
    return value.isDouble() && std::isfinite(value.toDouble())
        && value.toDouble() >= minimum && value.toDouble() <= maximum;
}
bool validAction(const QJsonObject &action, QString *error) {
    const auto field = action.value("action").toString();
    const auto value = action.value("value");
    if (!validDeviceKey(action.value("deviceKey").toString())) {
        *error = QStringLiteral("请选择有效的设备。"); return false;
    }
    bool valid = false;
    if (field == "power") valid = value.isBool();
    else if (field == "lock") valid = value.isBool() && value.toBool();
    else if (field == "brightness" || field == "fanPercent") valid = number(value, 0, 100) && std::floor(value.toDouble()) == value.toDouble();
    else if (field == "position") valid = number(value, 0, 100);
    else if (field == "colorTemperature") valid = number(value, 1, 65534) && std::floor(value.toDouble()) == value.toDouble();
    else if (field == "heatSetpoint" || field == "coolSetpoint") valid = number(value, -273.15, 327.67);
    else if (field == "color") {
        const auto color = value.toObject();
        valid = value.isObject() && color.size() == 2 && number(color.value("hue"), 0, 360) && number(color.value("saturation"), 0, 100);
    } else if (field == "thermostatMode" || field == "fanMode") valid = value.isString() && validText(value.toString(), 40);
    if (!valid) {
        *error = field == "lock" ? QStringLiteral("场景和自动化仅支持上锁；开锁请在设备页确认。")
                                : QStringLiteral("设备动作或目标值无效。");
        return false;
    }
    return true;
}
bool validTriggerValue(const QString &field, const QJsonValue &value) {
    if (QStringList{"power", "lock", "contact", "occupancy"}.contains(field)) return value.isBool();
    if (field == "thermostatMode" || field == "fanMode") return value.isString() && validText(value.toString(), 40);
    if (QStringList{"brightness", "position", "fanPercent", "humidity", "battery"}.contains(field)) return number(value, 0, 100);
    if (field == "colorTemperature") return number(value, 1, 65534);
    if (field == "temperature" || field == "heatSetpoint" || field == "coolSetpoint") return number(value, -273.15, 327.67);
    if (field == "airQuality") return number(value, 1, 6) && std::floor(value.toDouble()) == value.toDouble();
    if (field == "powerWatts") return number(value, -1e12, 1e12);
    if (QStringList{"co2", "pm25", "illuminance", "energyKwh"}.contains(field)) return number(value, 0, 1e12);
    return false;
}
QJsonArray cleanedActions(const QJsonArray &actions) {
    QJsonArray result;
    for (const auto &value : actions) {
        const auto action = value.toObject();
        result.append(QJsonObject{{"deviceKey", action.value("deviceKey")}, {"action", action.value("action")}, {"value", action.value("value")}});
    }
    return result;
}
}

HomeStore::HomeStore(MatterClient *client, QObject *parent)
    : HomeStore(client, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/homes"), parent) {}

HomeStore::HomeStore(MatterClient *client, const QString &storageDirectory, QObject *parent)
    : QObject(parent), m_client(client), m_directory(storageDirectory), m_document(blankDocument()) {
    Q_ASSERT(m_client);
    m_scheduleTimer.setInterval(15000);
    connect(&m_scheduleTimer, &QTimer::timeout, this, [this] { evaluateAutomations(); });
    m_scheduleTimer.start();
    m_commandTimer.setSingleShot(true);
    m_commandTimer.setInterval(35000);
    connect(&m_commandTimer, &QTimer::timeout, this, [this] {
        finishExecution(false, QStringLiteral("场景操作超时；请确认设备状态。"));
    });
    connect(m_client, &MatterClient::connectionChanged, this, [this] {
        resetObservation();
        if (!m_client->isConnected() && isExecuting()) finishExecution(false, QStringLiteral("控制器已断开；场景已停止。"));
        if (m_client->isConnected()) observeDevices();
    });
    connect(m_client, &MatterClient::devicesChanged, this, &HomeStore::observeDevices);
    connect(m_client, &MatterClient::commandFinished, this,
        [this](const QString &id, const QString &command, bool success, const QString &detail) {
        if (m_abandonedCommands.remove(id)) return;
        if (id == m_pendingCommand && isExecuting()) {
            m_commandTimer.stop(); m_pendingCommand.clear();
            if (!success) { finishExecution(false, detail); return; }
            ++m_actionIndex;
            const auto generation = m_executionGeneration;
            QTimer::singleShot(0, this, [this, generation] { if (generation == m_executionGeneration) sendNextAction(); });
        } else if ((command == "device_command" || command == "write_attribute") && !m_profile.isEmpty()) {
            recordActivity(QStringLiteral("control"), QStringLiteral("设备操作"), success);
        }
    });
}

bool HomeStore::fail(const QString &message) {
    m_lastError = message;
    emit errorOccurred(message);
    return false;
}

void HomeStore::setProfile(const QString &profile) {
    if (m_profile == profile) return;
    if (isExecuting()) finishExecution(false, QStringLiteral("家庭已切换；场景已停止。"));
    m_profile = profile;
    m_document = blankDocument();
    m_path.clear(); m_lastError.clear(); m_loadFailed = false;
    resetObservation(); m_edgeHoldUntil = {};
    if (!profile.isEmpty()) {
        const auto hash = QCryptographicHash::hash(profile.toUtf8(), QCryptographicHash::Sha256).toHex();
        m_path = QDir(m_directory).filePath(QString::fromLatin1(hash) + QStringLiteral(".json"));
        QFile file(m_path);
        if (file.exists()) {
            QString error;
            if (!file.open(QIODevice::ReadOnly) || file.size() > MaxFileBytes) {
                error = QStringLiteral("家庭配置无法读取或超过大小限制；原文件已保留。");
            } else {
                QJsonParseError parseError;
                const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
                if (parseError.error != QJsonParseError::NoError || !document.isObject()
                    || !validateDocument(document.object(), &error)) {
                    if (error.isEmpty()) error = QStringLiteral("家庭配置已损坏；原文件已保留。");
                } else m_document = document.object();
            }
            if (!error.isEmpty()) { m_loadFailed = true; fail(error); }
        }
    }
    emit changed(); emit activityChanged();
    if (m_client->isConnected()) observeDevices();
}

QString HomeStore::homeName() const { return m_document.value("homeName").toString(QStringLiteral("我的家")); }
QJsonArray HomeStore::rooms() const { return m_document.value("rooms").toArray(); }
QJsonArray HomeStore::zones() const { return m_document.value("zones").toArray(); }
QJsonArray HomeStore::scenes() const { return m_document.value("scenes").toArray(); }
QJsonArray HomeStore::rules() const { return m_document.value("rules").toArray(); }
QJsonArray HomeStore::activity() const { return m_document.value("activity").toArray(); }
QJsonObject HomeStore::accessory(const QString &key) const { return m_document.value("accessories").toObject().value(key).toObject(); }
QString HomeStore::displayName(const MatterDevice &device) const {
    return accessory(device.key).value("name").toString(device.name);
}
bool HomeStore::setHomeName(const QString &name) {
    if (!validText(name)) return fail(QStringLiteral("家庭名称须为 1–80 个字符。"));
    auto document = m_document; document.insert("homeName", name.trimmed()); return commit(document);
}
bool HomeStore::persist(const QJsonObject &document) {
    if (m_profile.isEmpty()) return fail(QStringLiteral("请先连接一个家庭或进入演示模式。"));
    if (m_loadFailed) return fail(QStringLiteral("家庭配置读取失败，无法覆盖原文件。"));
    if (!QDir().mkpath(m_directory)) return fail(QStringLiteral("无法创建家庭配置目录。"));
    const auto bytes = QJsonDocument(document).toJson(QJsonDocument::Compact);
    if (bytes.size() > MaxFileBytes) return fail(QStringLiteral("家庭配置超过大小限制。"));
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail(QStringLiteral("家庭配置保存失败，请检查磁盘空间与目录权限。"));
    return true;
}
bool HomeStore::commit(QJsonObject document, bool notify) {
    if (!persist(document)) return false;
    m_document = std::move(document); m_lastError.clear();
    if (notify) emit changed();
    return true;
}

bool HomeStore::validateItem(const QString &collection, QJsonObject &item, const QJsonObject &document, QString *error) const {
    const auto id = item.value("id").toString();
    const auto name = item.value("name").toString().trimmed();
    if (!validId(id) || !validText(name)) { *error = QStringLiteral("名称或标识无效。"); return false; }
    QJsonObject clean{{"id", id}, {"name", name}};
    if (collection == "rooms") {
        const auto zone = item.value("zoneId").toString();
        if (!zone.isEmpty() && !containsId(document.value("zones").toArray(), zone)) {
            *error = QStringLiteral("所选区域已不存在。"); return false;
        }
        if (!zone.isEmpty()) clean.insert("zoneId", zone);
    } else if (collection == "scenes") {
        const auto actions = item.value("actions").toArray();
        if (!item.value("actions").isArray() || actions.isEmpty() || actions.size() > MaxActions) {
            *error = QStringLiteral("场景须包含 1–64 个动作。"); return false;
        }
        for (const auto &action : actions) {
            if (!action.isObject()) { *error = QStringLiteral("场景动作格式无效。"); return false; }
            if (!validAction(action.toObject(), error)) return false;
        }
        clean.insert("actions", cleanedActions(actions));
    } else if (collection == "rules") {
        const auto sceneId = item.value("sceneId").toString();
        if (!containsId(document.value("scenes").toArray(), sceneId)) {
            *error = QStringLiteral("请选择现有场景。"); return false;
        }
        if (item.contains("enabled") && !item.value("enabled").isBool()) { *error = QStringLiteral("自动化启用状态无效。"); return false; }
        const auto trigger = item.value("trigger").toObject();
        const auto type = trigger.value("type").toString();
        QJsonObject cleanTrigger{{"type", type}};
        if (type == "time") {
            const auto time = trigger.value("time").toString();
            const auto days = trigger.value("days").toArray();
            if (!QRegularExpression(QStringLiteral("^([01][0-9]|2[0-3]):[0-5][0-9]$")).match(time).hasMatch()
                || days.isEmpty() || days.size() > 7) { *error = QStringLiteral("请选择有效时间和星期。"); return false; }
            QSet<int> seen;
            for (const auto &day : days) {
                if (!number(day, 1, 7) || day.toInt() != day.toDouble() || seen.contains(day.toInt())) {
                    *error = QStringLiteral("星期须为不重复的 1–7。"); return false;
                }
                seen.insert(day.toInt());
            }
            cleanTrigger.insert("time", time); cleanTrigger.insert("days", days);
        } else if (type == "device") {
            const auto key = trigger.value("deviceKey").toString();
            const auto action = trigger.value("action").toString();
            const auto value = trigger.value("value");
            static const QSet<QString> fields{"power", "brightness", "colorTemperature", "lock", "position", "thermostatMode", "heatSetpoint", "coolSetpoint", "fanPercent", "fanMode", "temperature", "humidity", "occupancy", "contact", "airQuality", "co2", "pm25", "illuminance", "battery", "powerWatts", "energyKwh"};
            if (!validDeviceKey(key) || !fields.contains(action) || !validTriggerValue(action, value)) {
                *error = QStringLiteral("设备触发条件无效。"); return false;
            }
            cleanTrigger.insert("deviceKey", key); cleanTrigger.insert("action", action); cleanTrigger.insert("value", value);
        } else { *error = QStringLiteral("请选择时间或设备状态触发。"); return false; }
        clean.insert("enabled", item.value("enabled").toBool(false));
        clean.insert("sceneId", sceneId); clean.insert("trigger", cleanTrigger);
    }
    item = clean;
    return true;
}

QString HomeStore::saveItem(const QString &collection, QJsonObject item) {
    if (!item.contains("id") || item.value("id").toString().isEmpty()) item.insert("id", QUuid::createUuid().toString(QUuid::WithoutBraces));
    QString error;
    if (!validateItem(collection, item, m_document, &error)) { fail(error); return {}; }
    auto document = m_document;
    auto items = document.value(collection).toArray();
    int index = -1;
    for (int i = 0; i < items.size(); ++i) if (items[i].toObject().value("id") == item.value("id")) { index = i; break; }
    if (index < 0 && items.size() >= MaxItems) { fail(QStringLiteral("项目数量已达 128 项上限。")); return {}; }
    if (index < 0) items.append(item); else items.replace(index, item);
    document.insert(collection, items);
    if (!commit(document)) return {};
    if (collection == "rules") { resetObservation(); observeDevices(); }
    return item.value("id").toString();
}
QString HomeStore::saveRoom(QJsonObject item) { return saveItem(QStringLiteral("rooms"), item); }
QString HomeStore::saveZone(QJsonObject item) { return saveItem(QStringLiteral("zones"), item); }
QString HomeStore::saveScene(QJsonObject item) { return saveItem(QStringLiteral("scenes"), item); }
QString HomeStore::saveRule(QJsonObject item) { return saveItem(QStringLiteral("rules"), item); }
QJsonObject HomeStore::findItem(const QString &collection, const QString &id) const {
    for (const auto &item : m_document.value(collection).toArray()) if (item.toObject().value("id").toString() == id) return item.toObject();
    return {};
}
bool HomeStore::removeItem(const QString &collection, const QString &id) {
    auto document = m_document;
    auto items = document.value(collection).toArray();
    bool found = false;
    for (int i = 0; i < items.size(); ++i) if (items[i].toObject().value("id").toString() == id) { items.removeAt(i); found = true; break; }
    if (!found) return fail(QStringLiteral("项目已不存在。"));
    document.insert(collection, items);
    if (collection == "rooms") {
        auto metadata = document.value("accessories").toObject();
        for (auto it = metadata.begin(); it != metadata.end(); ++it) {
            auto entry = it.value().toObject();
            if (entry.value("roomId").toString() == id) { entry.remove("roomId"); it.value() = entry; }
        }
        document.insert("accessories", metadata);
    } else if (collection == "zones") {
        auto roomItems = document.value("rooms").toArray();
        for (int i = 0; i < roomItems.size(); ++i) {
            auto room = roomItems[i].toObject();
            if (room.value("zoneId").toString() == id) { room.remove("zoneId"); roomItems.replace(i, room); }
        }
        document.insert("rooms", roomItems);
    } else if (collection == "scenes") {
        // A rule cannot silently point to a different scene after deletion.
        auto ruleItems = document.value("rules").toArray();
        auto runtime = document.value("runtime").toObject();
        for (int i = ruleItems.size() - 1; i >= 0; --i) if (ruleItems[i].toObject().value("sceneId").toString() == id) {
            runtime.remove(ruleItems[i].toObject().value("id").toString()); ruleItems.removeAt(i);
        }
        document.insert("rules", ruleItems); document.insert("runtime", runtime);
    } else if (collection == "rules") {
        auto runtime = document.value("runtime").toObject(); runtime.remove(id); document.insert("runtime", runtime);
    }
    if (!commit(document)) return false;
    resetObservation(); return true;
}
bool HomeStore::removeRoom(const QString &id) { return removeItem(QStringLiteral("rooms"), id); }
bool HomeStore::removeZone(const QString &id) { return removeItem(QStringLiteral("zones"), id); }
bool HomeStore::removeScene(const QString &id) { return removeItem(QStringLiteral("scenes"), id); }
bool HomeStore::removeRule(const QString &id) { return removeItem(QStringLiteral("rules"), id); }

bool HomeStore::setAccessory(const QString &key, QJsonObject patch) {
    if (!validDeviceKey(key)) return fail(QStringLiteral("设备标识无效。"));
    auto metadata = m_document.value("accessories").toObject();
    if (!metadata.contains(key) && metadata.size() >= MaxAccessories) return fail(QStringLiteral("设备备注数量已达上限。"));
    auto entry = metadata.value(key).toObject();
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        const auto field = it.key(); const auto value = it.value();
        if (field == "name") {
            if (!value.isString() || (!value.toString().isEmpty() && !validText(value.toString()))) return fail(QStringLiteral("设备名称须为 1–80 个字符。"));
            if (value.toString().trimmed().isEmpty()) entry.remove(field); else entry.insert(field, value.toString().trimmed());
        } else if (field == "roomId") {
            if (!value.isString() || (!value.toString().isEmpty() && !containsId(rooms(), value.toString()))) return fail(QStringLiteral("所选房间已不存在。"));
            if (value.toString().isEmpty()) entry.remove(field); else entry.insert(field, value);
        } else if (field == "favorite" || field == "hidden") {
            if (!value.isBool()) return fail(QStringLiteral("设备备注格式无效。"));
            entry.insert(field, value);
        } else if (field == "order") {
            if (!number(value, 0, 100000) || value.toDouble() != value.toInt()) return fail(QStringLiteral("设备排序值无效。"));
            entry.insert(field, value);
        } else return fail(QStringLiteral("设备备注包含不支持的字段。"));
    }
    if (entry.isEmpty()) metadata.remove(key); else metadata.insert(key, entry);
    auto document = m_document; document.insert("accessories", metadata); return commit(document);
}

bool HomeStore::validateDocument(const QJsonObject &document, QString *error) const {
    if (document.size() != 9 || document.value("version").toInt() != 1 || !validText(document.value("homeName").toString())) {
        *error = QStringLiteral("家庭配置版本或名称无效；原文件已保留。"); return false;
    }
    for (const auto &collection : {QStringLiteral("zones"), QStringLiteral("rooms"), QStringLiteral("scenes"), QStringLiteral("rules")}) {
        const auto value = document.value(collection);
        const auto items = value.toArray();
        if (!value.isArray() || items.size() > MaxItems) { *error = QStringLiteral("家庭配置项目数量或格式无效。"); return false; }
        QSet<QString> seen;
        for (const auto &entry : items) {
            auto item = entry.toObject();
            if (!entry.isObject() || seen.contains(item.value("id").toString()) || !validateItem(collection, item, document, error) || item != entry.toObject()) {
                if (error->isEmpty()) *error = QStringLiteral("家庭配置含重复或无效项目。");
                return false;
            }
            seen.insert(item.value("id").toString());
        }
    }
    const auto metadata = document.value("accessories").toObject();
    if (!document.value("accessories").isObject() || metadata.size() > MaxAccessories) {
        *error = QStringLiteral("家庭配置设备备注无效。"); return false;
    }
    for (auto it = metadata.begin(); it != metadata.end(); ++it) {
        const auto entry = it.value().toObject();
        if (!validDeviceKey(it.key()) || !it.value().isObject()) { *error = QStringLiteral("家庭配置设备标识无效。"); return false; }
        for (auto field = entry.begin(); field != entry.end(); ++field) {
            bool valid = false;
            if (field.key() == "name") valid = field.value().isString() && validText(field.value().toString());
            else if (field.key() == "roomId") valid = field.value().isString() && containsId(document.value("rooms").toArray(), field.value().toString());
            else if (field.key() == "favorite" || field.key() == "hidden") valid = field.value().isBool();
            else if (field.key() == "order") valid = number(field.value(), 0, 100000) && field.value().toDouble() == field.value().toInt();
            if (!valid) { *error = QStringLiteral("家庭配置设备备注字段无效。"); return false; }
        }
    }
    const auto history = document.value("activity").toArray();
    if (!document.value("activity").isArray() || history.size() > MaxHistory || !document.value("runtime").isObject()) {
        *error = QStringLiteral("家庭配置活动记录无效。"); return false;
    }
    for (const auto &entry : history) {
        const auto record = entry.toObject();
        if (!entry.isObject() || !validText(record.value("name").toString())
            || !record.value("success").isBool() || !QDateTime::fromString(record.value("timestamp").toString(), Qt::ISODateWithMs).isValid()
            || !QStringList{"scene", "automation", "control"}.contains(record.value("type").toString()) || record.size() != 4) {
            *error = QStringLiteral("家庭配置活动记录字段无效。"); return false;
        }
    }
    const auto runtime = document.value("runtime").toObject();
    if (runtime.size() > MaxItems) { *error = QStringLiteral("自动化运行记录超过上限。"); return false; }
    for (auto it = runtime.begin(); it != runtime.end(); ++it) {
        const auto state = it.value().toObject();
        if (!containsId(document.value("rules").toArray(), it.key()) || !it.value().isObject() || state.size() > 2
            || !QDateTime::fromString(state.value("lastRun").toString(), Qt::ISODateWithMs).isValid()
            || (state.contains("lastDay") && !QDate::fromString(state.value("lastDay").toString(), Qt::ISODate).isValid())) {
            *error = QStringLiteral("自动化运行记录无效。"); return false;
        }
    }
    return true;
}

bool HomeStore::preflight(const QJsonArray &actions, QString *error) const {
    if (m_profile.isEmpty() || !m_client->isConnected()) { *error = QStringLiteral("家庭控制器尚未就绪。"); return false; }
    const auto devices = m_client->devices();
    for (const auto &entry : actions) {
        const auto action = entry.toObject();
        if (!validAction(action, error)) return false;
        const auto key = action.value("deviceKey").toString();
        const auto found = std::find_if(devices.cbegin(), devices.cend(), [&key](const MatterDevice &device) { return device.key == key; });
        if (found == devices.cend() || !found->available) { *error = QStringLiteral("目标设备已离线或不再存在；未发送后续动作。"); return false; }
        const auto field = action.value("action").toString();
        if (!found->controls.contains(field)) { *error = QStringLiteral("目标设备不支持所选动作。"); return false; }
        const auto value = action.value("value");
        const auto limit = found->controlLimits.value(field);
        if (limit.isArray() && !limit.toArray().contains(value)) { *error = QStringLiteral("目标设备不支持所选模式。"); return false; }
        if (limit.isObject() && value.isDouble()) {
            const auto bounds = limit.toObject();
            if ((bounds.contains("min") && value.toDouble() < bounds.value("min").toDouble())
                || (bounds.contains("max") && value.toDouble() > bounds.value("max").toDouble())) {
                *error = QStringLiteral("目标值超出设备允许范围。"); return false;
            }
        }
        if (field == "color" && limit.isObject()) {
            const auto bounds = limit.toObject(); const auto color = value.toObject();
            if (!number(color.value("hue"), bounds.value("hueMin").toDouble(0), bounds.value("hueMax").toDouble(360))
                || !number(color.value("saturation"), bounds.value("saturationMin").toDouble(0), bounds.value("saturationMax").toDouble(100))) {
                *error = QStringLiteral("目标颜色超出设备允许范围。"); return false;
            }
        }
        if (field == "heatSetpoint" || field == "coolSetpoint") {
            const auto deadband = found->controlLimits.value("setpointDeadband");
            const bool heat = field == "heatSetpoint";
            const auto other = found->controlState.value(heat ? "coolSetpoint" : "heatSetpoint");
            if (deadband.isDouble() && (!other.isDouble() || (heat
                ? value.toDouble() + deadband.toDouble() > other.toDouble() + 0.00001
                : value.toDouble() - deadband.toDouble() < other.toDouble() - 0.00001))) {
                *error = QStringLiteral("目标温度不满足设备的制热与制冷间隔；请先确认另一目标温度。"); return false;
            }
        }
    }
    return true;
}

bool HomeStore::startExecution(const QString &id, const QString &name, const QJsonArray &actions, bool automation) {
    if (isExecuting()) return fail(QStringLiteral("已有场景正在执行，请等待完成。"));
    QString error;
    if (!preflight(actions, &error)) { recordActivity(automation ? QStringLiteral("automation") : QStringLiteral("scene"), name, false); return fail(error); }
    m_executionId = id; m_executionName = name; m_executionActions = actions;
    m_actionIndex = 0; m_executionAutomation = automation; m_pendingCommand.clear();
    const auto generation = ++m_executionGeneration;
    m_edgeHoldUntil = QDateTime::currentDateTime().addSecs(60);
    emit executionStarted(id);
    QTimer::singleShot(0, this, [this, generation] { if (generation == m_executionGeneration) sendNextAction(); });
    return true;
}
bool HomeStore::runScene(const QString &id) {
    const auto scene = findItem(QStringLiteral("scenes"), id);
    if (scene.isEmpty()) return fail(QStringLiteral("场景已不存在。"));
    return startExecution(id, scene.value("name").toString(), scene.value("actions").toArray(), false);
}
bool HomeStore::runRule(const QString &id) {
    const auto rule = findItem(QStringLiteral("rules"), id);
    if (rule.isEmpty()) return fail(QStringLiteral("自动化已不存在。"));
    const auto scene = findItem(QStringLiteral("scenes"), rule.value("sceneId").toString());
    if (scene.isEmpty()) return fail(QStringLiteral("自动化引用的场景已不存在。"));
    if (!startExecution(id, rule.value("name").toString(), scene.value("actions").toArray(), true)) return false;
    if (!markRuleRun(id, QDateTime::currentDateTime(), false)) { finishExecution(false, lastError()); return false; }
    return true;
}
void HomeStore::sendNextAction() {
    if (!isExecuting()) return;
    if (m_actionIndex >= m_executionActions.size()) { finishExecution(true, QStringLiteral("全部动作已由控制器确认。")); return; }
    const auto action = m_executionActions[m_actionIndex].toObject();
    QString error;
    if (!preflight({action}, &error)) { finishExecution(false, error); return; }
    for (const auto &device : m_client->devices()) if (device.key == action.value("deviceKey").toString()) {
        m_pendingCommand = m_client->control(device, action.value("action").toString(), action.value("value"));
        m_commandTimer.start(); return;
    }
    finishExecution(false, QStringLiteral("目标设备已不存在。"));
}
void HomeStore::finishExecution(bool success, const QString &detail) {
    if (!isExecuting()) return;
    const auto id = m_executionId;
    const auto name = m_executionName;
    const auto type = m_executionAutomation ? QStringLiteral("automation") : QStringLiteral("scene");
    ++m_executionGeneration;
    if (!m_pendingCommand.isEmpty()) m_abandonedCommands.insert(m_pendingCommand);
    m_executionId.clear(); m_executionName.clear(); m_pendingCommand.clear();
    m_executionActions = {}; m_commandTimer.stop();
    recordActivity(type, name, success);
    emit executionFinished(id, success, detail);
}
void HomeStore::cancelExecution() { finishExecution(false, QStringLiteral("执行已取消；已发送的动作无法撤回。")); }
void HomeStore::recordActivity(const QString &type, const QString &name, bool success) {
    if (m_profile.isEmpty() || m_loadFailed) return;
    auto history = activity();
    history.prepend(QJsonObject{{"timestamp", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
                               {"type", type}, {"name", name.left(80)}, {"success", success}});
    while (history.size() > MaxHistory) history.removeLast();
    auto document = m_document; document.insert("activity", history);
    if (commit(document, false)) emit activityChanged();
}
void HomeStore::clearActivity() {
    auto document = m_document; document.insert("activity", QJsonArray{});
    if (commit(document, false)) emit activityChanged();
}

void HomeStore::resetObservation() { m_observed.clear(); m_observationReady = false; }
QJsonValue HomeStore::observedValue(const QJsonObject &trigger) const {
    for (const auto &device : m_client->devices()) if (device.key == trigger.value("deviceKey").toString() && device.available)
        return device.controlState.value(trigger.value("action").toString());
    return QJsonValue(QJsonValue::Undefined);
}
bool HomeStore::ruleCoolingDown(const QString &id, const QDateTime &now) const {
    const auto state = m_document.value("runtime").toObject().value(id).toObject();
    const auto lastRun = QDateTime::fromString(state.value("lastRun").toString(), Qt::ISODateWithMs);
    return lastRun.isValid() && lastRun.secsTo(now) < 60;
}
bool HomeStore::markRuleRun(const QString &id, const QDateTime &now, bool daily) {
    auto document = m_document; auto runtime = document.value("runtime").toObject();
    auto state = runtime.value(id).toObject(); state.insert("lastRun", now.toString(Qt::ISODateWithMs));
    if (daily) state.insert("lastDay", now.date().toString(Qt::ISODate));
    runtime.insert(id, state); document.insert("runtime", runtime); return commit(document, false);
}
void HomeStore::observeDevices() {
    if (m_profile.isEmpty() || !m_client->isConnected()) { resetObservation(); return; }
    const auto now = QDateTime::currentDateTime();
    QString triggerId;
    for (const auto &entry : rules()) {
        const auto rule = entry.toObject(); const auto trigger = rule.value("trigger").toObject();
        if (trigger.value("type") != "device") continue;
        const auto id = rule.value("id").toString();
        const auto current = observedValue(trigger);
        const auto previous = m_observed.value(id, QJsonValue(QJsonValue::Undefined));
        m_observed.insert(id, current);
        if (m_observationReady && rule.value("enabled").toBool() && triggerId.isEmpty() && !isExecuting()
            && (!m_edgeHoldUntil.isValid() || now >= m_edgeHoldUntil) && !ruleCoolingDown(id, now)
            && !previous.isUndefined() && !current.isUndefined() && previous != current && current == trigger.value("value")) triggerId = id;
    }
    m_observationReady = true;
    if (!triggerId.isEmpty()) runRule(triggerId);
}
void HomeStore::evaluateAutomations(const QDateTime &now) {
    if (!now.isValid() || m_profile.isEmpty() || !m_client->isConnected() || isExecuting() || m_loadFailed) return;
    for (const auto &entry : rules()) {
        const auto rule = entry.toObject(); const auto trigger = rule.value("trigger").toObject();
        if (!rule.value("enabled").toBool() || trigger.value("type") != "time"
            || trigger.value("time").toString() != now.time().toString(QStringLiteral("HH:mm"))
            || !trigger.value("days").toArray().contains(now.date().dayOfWeek())) continue;
        const auto id = rule.value("id").toString();
        const auto state = m_document.value("runtime").toObject().value(id).toObject();
        if (state.value("lastDay").toString() == now.date().toString(Qt::ISODate) || ruleCoolingDown(id, now)) continue;
        const auto scene = findItem(QStringLiteral("scenes"), rule.value("sceneId").toString());
        if (scene.isEmpty()) continue;
        // Persist the occurrence before queueing a command so a restart cannot
        // replay this minute. Failed/partial attempts are not retried automatically.
        if (!markRuleRun(id, now, true)) return;
        startExecution(id, rule.value("name").toString(), scene.value("actions").toArray(), true);
        return;
    }
}
