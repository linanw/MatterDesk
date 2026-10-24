#include "camerametadata.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <cmath>

namespace {
constexpr double MaxRevision = 9007199254740991.0;
constexpr qint64 MaxStoreBytes = 2 * 1024 * 1024;
QString path(int id) { return QStringLiteral("1/4294048770/%1").arg(id); }
bool text(const QJsonValue &v, int limit, bool empty = true) {
    if (!v.isString() || v.toString().size() > limit || (limit == 160 && v.toString().toUtf8().size() > 160) || (!empty && v.toString().trimmed().isEmpty())) return false;
    const auto value=v.toString();
    for (qsizetype i=0;i<value.size();++i) {
        const auto ch=value.at(i);
        if (ch.isHighSurrogate()) {
            if (i+1>=value.size() || !value.at(i+1).isLowSurrogate()) return false;
            ++i;
        } else if (ch.isLowSurrogate() || ch.category()==QChar::Other_Control) return false;
    }
    return true;
}
bool failure(QString *error, const QString &detail) { if (error) *error = detail; return false; }
bool clearedData(const CameraMetadataSnapshot &snapshot) {
    return snapshot.name.isEmpty() && snapshot.homeName.isEmpty() && snapshot.roomId.isEmpty() && snapshot.roomName.isEmpty();
}
bool validObservation(const QString &value) {
    static const QRegularExpression isoUtc(QStringLiteral("^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{3}Z$"));
    return isoUtc.match(value).hasMatch() && QDateTime::fromString(value,Qt::ISODateWithMs).isValid();
}
bool sameGuard(const CameraMetadataSnapshot &a, const CameraMetadataSnapshot &b) {
    return a.identity == b.identity && a.sourceEpoch == b.sourceEpoch && a.revision == b.revision && a.homeKey == b.homeKey;
}
bool sameSource(const CameraMetadataSnapshot &a, const CameraMetadataSnapshot &b) {
    return a.identity == b.identity && a.sourceEpoch == b.sourceEpoch && a.homeKey == b.homeKey;
}
QJsonObject snapshotJson(const CameraMetadataSnapshot &s) {
    return {{"0", s.identity}, {"1", s.sourceEpoch}, {"2", double(s.revision)}, {"3", s.name}, {"4", s.homeKey},
        {"5", s.homeName}, {"6", s.roomId}, {"7", s.roomName}, {"8", s.observedAt}};
}
QJsonArray roomsJson(const QList<CameraMetadataRoom> &rooms) {
    QJsonArray result; for (const auto &room : rooms) result.append(QJsonObject{{"0", room.id}, {"1", room.name}}); return result;
}
QString controllerProfile(MatterClient *client) {
    if (client->isDemoMode() || client->serverUrl().isEmpty()) return {};
    QStringList parts{client->serverUrl().toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment)};
    const auto info = client->serverInfo();
    for (const auto &key : {"fabric_id", "compressed_fabric_id"})
        if (info.contains(key)) parts << QString::fromLatin1(key) + QString::fromUtf8(QJsonDocument(QJsonArray{info.value(key)}).toJson(QJsonDocument::Compact));
    return parts.join('|');
}
QJsonObject metadataFingerprint(const MatterDevice &device) {
    QJsonObject result; for (int i = 0; i <= 6; ++i) result.insert(path(i), device.attributes.value(path(i)));
    return result;
}
}

bool CameraMetadataSnapshot::operator==(const CameraMetadataSnapshot &o) const {
    return sameGuard(*this, o) && name == o.name && homeName == o.homeName && roomId == o.roomId
        && roomName == o.roomName && observedAt == o.observedAt;
}
bool cameraMetadataUnavailable(const CameraMetadataState &state) {
    return state.known && !state.available && clearedData(state.snapshot) && state.rooms.isEmpty();
}

bool parseCameraMetadataSnapshot(const QJsonValue &value, CameraMetadataSnapshot *output, QString *error) {
    if (!output || !value.isObject()) return failure(error, QStringLiteral("米家快照格式无效。"));
    const auto o = value.toObject();
    static const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));
    static const QRegularExpression epoch(QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    if (o.size() != 9 || !text(o.value("0"),64,false) || !digest.match(o.value("0").toString()).hasMatch()
        || !text(o.value("1"),36,false) || !epoch.match(o.value("1").toString()).hasMatch()
        || !o.value("2").isDouble() || !std::isfinite(o.value("2").toDouble()) || o.value("2").toDouble() < 0
        || o.value("2").toDouble() > MaxRevision || std::floor(o.value("2").toDouble()) != o.value("2").toDouble()
        || !text(o.value("3"),160) || !text(o.value("4"),64,false) || !digest.match(o.value("4").toString()).hasMatch()
        || !text(o.value("5"),160) || !text(o.value("6"),30) || !text(o.value("7"),160) || !text(o.value("8"),32)
        || (!o.value("8").toString().isEmpty() && !validObservation(o.value("8").toString())))
        return failure(error, QStringLiteral("米家快照字段无效或超出范围。"));
    CameraMetadataSnapshot parsed;
    parsed.identity=o.value("0").toString(); parsed.sourceEpoch=o.value("1").toString(); parsed.revision=quint64(o.value("2").toDouble());
    parsed.name=o.value("3").toString(); parsed.homeKey=o.value("4").toString(); parsed.homeName=o.value("5").toString();
    parsed.roomId=o.value("6").toString(); parsed.roomName=o.value("7").toString(); parsed.observedAt=o.value("8").toString();
    if (parsed.roomId.isEmpty() && !parsed.roomName.isEmpty()) return failure(error, QStringLiteral("未分房快照包含不一致房间名称。"));
    if (parsed.observedAt.isEmpty() && !clearedData(parsed)) return failure(error, QStringLiteral("缺少读取时间的快照必须清空全部来源数据。"));
    *output = parsed; return true;
}

bool parseCameraMetadataRooms(const QJsonValue &value, QList<CameraMetadataRoom> *output, QString *error) {
    if (!output || !value.isArray() || value.toArray().size() > 1000) return failure(error, QStringLiteral("米家房间列表无效。"));
    QList<CameraMetadataRoom> parsed; QSet<QString> ids;
    for (const auto &item : value.toArray()) {
        const auto o = item.toObject();
        if (!item.isObject() || o.size()!=2 || !text(o.value("0"),30,false) || !text(o.value("1"),160,false)
            || ids.contains(o.value("0").toString())) return failure(error, QStringLiteral("米家房间字段或标识重复。"));
        const auto id=o.value("0").toString(); ids.insert(id); parsed.append({id,o.value("1").toString()});
    }
    *output=parsed; return true;
}

bool parseCameraMetadataCapabilities(const QJsonValue &value, CameraMetadataCapabilities *output, QString *error) {
    if (!output || !value.isObject()) return failure(error, QStringLiteral("米家能力格式无效。"));
    const auto o=value.toObject();
    if (o.size()!=4 || !o.value("0").isBool() || !o.value("1").isBool() || !o.value("2").isBool() || !o.value("3").isBool())
        return failure(error, QStringLiteral("米家能力字段不完整。"));
    *output={o.value("0").toBool(),o.value("1").toBool(),o.value("2").toBool(),o.value("3").toBool()}; return true;
}

bool hasCameraMetadata(const MatterDevice &device) {
    if (!device.hasCamera || device.endpointId!=1) return false;
    const auto servers=device.attributes.value("1/29/1");
    if (!servers.isArray()) return false;
    for (const auto &v : servers.toArray()) if (v.isDouble() && v.toDouble()==double(0xFFF1FC02u)) return true;
    return false;
}

CameraMetadataManager::CameraMetadataManager(MatterClient *client,QObject *parent)
    : CameraMetadataManager(client,QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/camera-metadata",parent) {}

CameraMetadataManager::CameraMetadataManager(MatterClient *client,const QString &directory,QObject *parent)
    : QObject(parent),m_client(client),m_directory(directory),m_generation(client->controllerGeneration()) {
    m_confirmTimer.setSingleShot(true);
    connect(&m_confirmTimer,&QTimer::timeout,this,[this]{ if (!m_pending.key.isEmpty()) requestStage("before"); });
    connect(client,&MatterClient::cameraMetadataResponse,this,[this](const QString &id,bool ok,const QJsonValue &value,const QJsonObject &error){ response(id,ok,value,error); });
    connect(client,&MatterClient::devicesChanged,this,&CameraMetadataManager::observe);
    connect(client,&MatterClient::connectionChanged,this,&CameraMetadataManager::observe);
    connect(client,&MatterClient::serverInfoChanged,this,&CameraMetadataManager::observe);
    observe();
}

void CameraMetadataManager::setProfile(const QString &profile) {
    if (profile==m_profile) return;
    invalidate(QStringLiteral("控制器配置已切换；旧请求已失效。"));
    const auto old=m_entries.keys(); m_entries.clear(); m_mirrors={}; m_profile=profile;
    m_path=profile.isEmpty() || m_directory.isEmpty() ? QString() : QDir(m_directory).filePath(QString::fromLatin1(QCryptographicHash::hash(profile.toUtf8(),QCryptographicHash::Sha256).toHex())+".json");
    load(); for (const auto &key : old) emit changed(key);
}

CameraMetadataState CameraMetadataManager::state(const MatterDevice &device) const {
    auto result=m_entries.value(device.key).state;
    if (device.controllerGeneration!=m_client->controllerGeneration() || !hasCameraMetadata(device)) {
        result={}; result.status=QStringLiteral("此摄像头尚未提供米家元数据。");
    } else if (!m_client->isConnected() || !device.available) {
        result.stale=true; result.available=false; result.busy=false; result.loading=false;
        result.status=QStringLiteral("离线；保留上次读取内容。");
    }
    result.discovered=hasCameraMetadata(device); return result;
}

bool CameraMetadataManager::usable(const MatterDevice &device) const {
    if (!m_client->isConnected() || m_client->isDemoMode() || !device.available || !hasCameraMetadata(device)
        || device.controllerGeneration!=m_client->controllerGeneration()) return false;
    for (const auto &current:m_client->devices())
        if (current.key==device.key && current.nodeId==device.nodeId && current.controllerGeneration==device.controllerGeneration
            && current.available && hasCameraMetadata(current)) return true;
    return false;
}
bool CameraMetadataManager::canRead(const MatterDevice &device) const {
    return usable(device) && m_pending.key.isEmpty();
}
bool CameraMetadataManager::canRename(const MatterDevice &device) const {
    const auto s=state(device); return canRead(device)&&s.known&&!s.stale&&!s.loading&&s.available&&s.capabilities.rename;
}
bool CameraMetadataManager::canMoveRoom(const MatterDevice &device) const {
    const auto s=state(device); return canRead(device)&&s.known&&!s.stale&&!s.loading&&s.available&&s.capabilities.move;
}
bool CameraMetadataManager::canSetSync(const MatterDevice &device) const {
    const auto s=state(device); return canRead(device)&&s.known&&!s.stale&&!s.loading&&s.available&&s.capabilities.syncConfig;
}
bool CameraMetadataManager::read(const MatterDevice &d) { return startRead(d); }
bool CameraMetadataManager::refresh(const MatterDevice &d) { return startCommand(d,"refresh"); }
bool CameraMetadataManager::refresh(const MatterDevice &d,const CameraMetadataSnapshot &e) { return startCommand(d,"refresh",{},&e); }
bool CameraMetadataManager::setSync(const MatterDevice &d,bool enabled) { return startCommand(d,"sync",enabled); }
bool CameraMetadataManager::setSync(const MatterDevice &d,bool enabled,const CameraMetadataSnapshot &e) { return startCommand(d,"sync",enabled,&e); }
bool CameraMetadataManager::rename(const MatterDevice &d,const QString &name) { return startCommand(d,"rename",name); }
bool CameraMetadataManager::rename(const MatterDevice &d,const QString &name,const CameraMetadataSnapshot &e) { return startCommand(d,"rename",name,&e); }
bool CameraMetadataManager::moveRoom(const MatterDevice &d,const QString &id) { return startCommand(d,"move",id); }
bool CameraMetadataManager::moveRoom(const MatterDevice &d,const QString &id,const CameraMetadataSnapshot &e) { return startCommand(d,"move",id,&e); }

bool CameraMetadataManager::startRead(const MatterDevice &device,const QString &operation) {
    if (!canRead(device)) return false;
    auto &entry=m_entries[device.key]; entry.device=device; entry.generation=m_generation; entry.needsRead=false;
    m_pending={}; m_pending.key=device.key; m_pending.device=device; m_pending.profile=m_profile;
    m_pending.generation=m_generation; m_pending.operation=operation.isEmpty()?"read":operation;
    entry.state.busy=true; entry.state.loading=true; entry.state.confirmed=false; entry.state.conflict=false;
    entry.state.operation=m_pending.operation; entry.state.status=QStringLiteral("正在读取米家名称和房间…");
    requestStage("before"); emit changed(device.key); return true;
}

bool CameraMetadataManager::startCommand(const MatterDevice &device,const QString &operation,const QJsonValue &target,const CameraMetadataSnapshot *expected) {
    auto s=state(device); bool allowed=false;
    if (operation=="refresh") allowed=canRead(device)&&s.known&&!s.stale&&!s.loading&&s.capabilities.read;
    else if (operation=="sync") allowed=canSetSync(device)&&target.isBool();
    else if (operation=="rename") allowed=canRename(device)&&text(target,160,false);
    else if (operation=="move") {
        allowed=false;
        if (canMoveRoom(device)&&target.isString()&&!target.toString().isEmpty()) for (const auto &room:s.rooms) if (room.id==target.toString()) allowed=true;
    }
    if (expected && !sameGuard(s.snapshot,*expected)) {
        auto &entry=m_entries[device.key]; entry.state.conflict=true; entry.state.confirmed=false;
        entry.state.status=QStringLiteral("米家内容已变化；请保留草稿并确认最新版本后再提交。");
        emit changed(device.key); emit operationFinished(device.key,operation,false,entry.state.status); return false;
    }
    if (!allowed) return false;
    m_pending={}; m_pending.key=device.key; m_pending.device=device; m_pending.profile=m_profile;
    m_pending.generation=m_generation; m_pending.operation=operation; m_pending.baseline=s.snapshot; m_pending.target=target; m_pending.stage="command";
    auto &entry=m_entries[device.key]; entry.state.busy=true; entry.state.confirmed=false; entry.state.conflict=false;
    entry.state.operation=operation; entry.state.status=QStringLiteral("请求正在提交；尚未确认米家内容。");
    QJsonObject payload{{"expectedIdentity",s.snapshot.identity},{"expectedSourceEpoch",s.snapshot.sourceEpoch},{"expectedRevision",double(s.snapshot.revision)}};
    if (operation=="sync") payload.insert("enabled",target);
    else if (operation=="rename") payload.insert("name",target);
    else if (operation=="move") payload.insert("roomId",target);
    m_pending.id=m_client->invokeCameraMetadata(device,operation,payload);
    emit changed(device.key); return true;
}

void CameraMetadataManager::requestStage(const QString &stage) {
    if (m_pending.key.isEmpty()) return;
    if (m_pending.generation!=m_client->controllerGeneration() || m_pending.profile!=m_profile || !usable(m_pending.device)) {
        finish(false,QStringLiteral("控制器或摄像头上下文已失效。")); return;
    }
    m_pending.stage=stage;
    m_pending.id=m_client->readCameraMetadataAttributes(m_pending.device,stage=="middle"?QList<int>{1,2,3,4,5,6}:QList<int>{0});
}

void CameraMetadataManager::response(const QString &id,bool success,const QJsonValue &result,const QJsonObject &error) {
    if (m_pending.key.isEmpty() || id!=m_pending.id) return;
    if (m_pending.generation!=m_client->controllerGeneration() || m_pending.profile!=m_profile || !usable(m_pending.device)) {
        finish(false,QStringLiteral("控制器或摄像头上下文已失效。")); return;
    }
    if (!success) {
        const bool conflict=error.value("status").toInt(-1)==0x92 || error.value("error_code").toInt(-1)==8;
        finish(false,QStringLiteral("米家元数据请求失败；未自动重试。"),conflict); return;
    }
    auto &entry=m_entries[m_pending.key];
    if (m_pending.stage=="command") {
        if (!result.isNull()) { finish(false,QStringLiteral("命令回执无效；未自动重试。")); return; }
        entry.state.status=QStringLiteral("请求已接受，正在等待米家权威内容确认…");
        entry.state.loading=true; emit changed(m_pending.key); requestStage("before"); return;
    }
    if (!result.isObject()) { finish(false,QStringLiteral("元数据读取响应无效。")); return; }
    const auto values=result.toObject(); QString errorText;
    if (m_pending.stage=="before") {
        if (values.size()!=1 || !parseCameraMetadataSnapshot(values.value(path(0)),&m_pending.before,&errorText)) { finish(false,errorText); return; }
        requestStage("middle"); return;
    }
    if (m_pending.stage=="middle") {
        if (values.size()!=6) { finish(false,QStringLiteral("元数据属性不完整。")); return; }
        for (int a=1;a<=6;++a) if (!values.contains(path(a))) { finish(false,QStringLiteral("元数据属性不完整。")); return; }
        m_pending.middle=values; requestStage("after"); return;
    }
    CameraMetadataSnapshot after;
    if (values.size()!=1 || !parseCameraMetadataSnapshot(values.value(path(0)),&after,&errorText)) { finish(false,errorText); return; }
    if (!sameGuard(m_pending.before,after)) {
        finish(false,QStringLiteral("读取期间米家版本发生变化；未采用混合房间列表。"),true); return;
    }
    if (entry.state.known && sameSource(entry.state.snapshot,after) && after.revision<entry.state.snapshot.revision) {
        finish(false,QStringLiteral("米家快照版本倒退；已保留先前内容。"),true); return;
    }
    if (m_pending.operation!="read" && !sameSource(m_pending.baseline,after)) {
        finish(false,QStringLiteral("米家摄像头或家庭已切换；旧请求失效。"),true); return;
    }
    if (!publish(after,m_pending.middle,&errorText)) { finish(false,errorText); return; }
    if (m_pending.operation=="read") { finish(true,entry.state.lastError.isEmpty()?QStringLiteral("已读取米家名称和房间。"):QStringLiteral("已读取；米家来源报告错误：")+entry.state.lastError); return; }
    if (!entry.state.lastError.isEmpty()) { finish(false,QStringLiteral("米家来源报告错误：")+entry.state.lastError); return; }
    const bool fresh=validObservation(after.observedAt) && (m_pending.baseline.observedAt.isEmpty()
        || QDateTime::fromString(after.observedAt,Qt::ISODateWithMs)
            > QDateTime::fromString(m_pending.baseline.observedAt,Qt::ISODateWithMs));
    bool matched=!entry.state.loading && entry.state.available && fresh && after.revision>=m_pending.baseline.revision;
    if (m_pending.operation=="rename") matched&=after.name==m_pending.target.toString();
    else if (m_pending.operation=="move") matched&=after.roomId==m_pending.target.toString();
    else if (m_pending.operation=="sync") matched&=entry.state.syncEnabled==m_pending.target.toBool();
    if (matched) { finish(true,QStringLiteral("已通过米家权威内容回读确认。")); return; }
    if (!entry.state.loading && after.revision>m_pending.baseline.revision && m_pending.operation!="refresh") {
        finish(false,QStringLiteral("米家内容已被其他操作更新；请保留草稿并确认最新版本。"),true); return;
    }
    if (++m_pending.confirmReads>=4) { finish(false,QStringLiteral("请求已接受，但尚未确认目标内容；未自动重发写入。")); return; }
    entry.state.busy=true; entry.state.status=QStringLiteral("请求已接受，等待权威内容更新…");
    m_confirmTimer.start(1000); emit changed(m_pending.key);
}

bool CameraMetadataManager::publish(const CameraMetadataSnapshot &snapshot,const QJsonObject &attrs,QString *error) {
    QList<CameraMetadataRoom> rooms; CameraMetadataCapabilities caps;
    if (!parseCameraMetadataRooms(attrs.value(path(1)),&rooms,error) || !parseCameraMetadataCapabilities(attrs.value(path(2)),&caps,error)
        || !attrs.value(path(3)).isBool() || !attrs.value(path(4)).isBool() || !attrs.value(path(5)).isBool() || !text(attrs.value(path(6)),40))
        return failure(error,QStringLiteral("米家能力或同步状态字段无效。"));
    const bool incomplete=attrs.value(path(6)).toString()=="metadata_incomplete";
    const bool unavailable=!attrs.value(path(4)).toBool() && rooms.isEmpty() && clearedData(snapshot);
    if (incomplete && !unavailable)
        return failure(error,QStringLiteral("米家信息不可用快照必须清空全部来源数据与房间。"));
    if (!unavailable && (clearedData(snapshot) || !validObservation(snapshot.observedAt))) {
        return failure(error,QStringLiteral("米家来源快照尚未提供完整有效数据。"));
    }
    if (!snapshot.roomId.isEmpty()) {
        bool found=false; for (const auto &room:rooms) if (room.id==snapshot.roomId && room.name==snapshot.roomName) found=true;
        if (!found) return failure(error,QStringLiteral("米家快照与房间列表不一致。"));
    }
    auto &s=m_entries[m_pending.key].state;
    s.snapshot=snapshot; s.rooms=rooms; s.capabilities=caps; s.known=true; s.stale=false; s.discovered=true;
    s.syncEnabled=attrs.value(path(3)).toBool(); s.available=attrs.value(path(4)).toBool(); s.loading=attrs.value(path(5)).toBool();
    s.lastError=attrs.value(path(6)).toString();
    if (unavailable) m_mirrors.remove(snapshot.identity);
    else m_mirrors.insert(snapshot.identity,QJsonObject{{"snapshot",snapshotJson(snapshot)},{"rooms",roomsJson(rooms)}});
    persist(); return true;
}

void CameraMetadataManager::finish(bool success,const QString &detail,bool conflict) {
    if (m_pending.key.isEmpty()) return;
    const auto key=m_pending.key, operation=m_pending.operation;
    m_confirmTimer.stop(); m_pending={};
    if (success && m_entries.contains(key)) {
        for (const auto &current:m_client->devices()) if (current.key==key) m_entries[key].observedAttributes=metadataFingerprint(current);
    }
    auto &s=m_entries[key].state; s.busy=false; s.confirmed=success; s.conflict=conflict; s.status=detail;
    if (!success) { s.stale=true; s.available=false; s.loading=false; }
    emit changed(key); emit operationFinished(key,operation,success,detail);
    QTimer::singleShot(0,this,[this] {
        if (!m_pending.key.isEmpty()) return;
        for (auto it=m_entries.begin();it!=m_entries.end();++it)
            if (it->needsRead && canRead(it->device)) { read(it->device); break; }
    });
}
void CameraMetadataManager::invalidate(const QString &detail) {
    if (!m_pending.key.isEmpty()) finish(false,detail);
    m_confirmTimer.stop();
    for (auto it=m_entries.begin();it!=m_entries.end();++it) {
        it->state.stale=true; it->state.available=false; it->state.busy=false; it->state.loading=false;
        it->state.confirmed=false; it->state.status=detail; emit changed(it.key());
    }
}

void CameraMetadataManager::observe() {
    if (m_observing) return;
    m_observing=true;
    const auto profile=controllerProfile(m_client);
    if (profile!=m_profile) setProfile(profile);
    if (m_generation!=m_client->controllerGeneration()) {
        invalidate(QStringLiteral("控制器连接已切换；旧请求失效。"));
        const auto keys=m_entries.keys(); m_entries.clear(); m_generation=m_client->controllerGeneration();
        for (const auto &key:keys) emit changed(key);
    }
    if (!m_client->isConnected() || m_client->isDemoMode()) {
        invalidate(QStringLiteral("离线；保留上次读取内容。")); m_observing=false; return;
    }
    QSet<QString> seen; QList<MatterDevice> dirty;
    for (const auto &device:m_client->devices()) {
        if (!hasCameraMetadata(device)) continue;
        seen.insert(device.key); const bool fresh=!m_entries.contains(device.key); auto &entry=m_entries[device.key]; entry.device=device; entry.generation=m_generation;
        entry.state.discovered=true;
        if (!device.available) {
            if (m_pending.key==device.key) finish(false,QStringLiteral("摄像头离线；旧请求失效。"));
            entry.state.stale=true; entry.state.available=false; emit changed(device.key); continue;
        }
        const auto fingerprint=metadataFingerprint(device);
        CameraMetadataSnapshot cached;
        const bool valid=parseCameraMetadataSnapshot(device.attributes.value(path(0)),&cached);
        if (valid && entry.state.known && !sameSource(entry.state.snapshot,cached)) {
            if (m_pending.key==device.key) finish(false,QStringLiteral("摄像头身份或来源已切换；旧请求失效。"),true);
            entry.state={}; entry.state.discovered=true;
        }
        if (!entry.state.known && valid && !clearedData(cached) && m_mirrors.contains(cached.identity)) {
            const auto mirror=m_mirrors.value(cached.identity).toObject(); CameraMetadataSnapshot saved; QList<CameraMetadataRoom> rooms;
            if (parseCameraMetadataSnapshot(mirror.value("snapshot"),&saved) && !clearedData(saved) && validObservation(saved.observedAt) && sameSource(saved,cached)
                && parseCameraMetadataRooms(mirror.value("rooms"),&rooms)) {
                entry.state.snapshot=saved; entry.state.rooms=rooms; entry.state.known=true; entry.state.stale=true;
                entry.state.status=QStringLiteral("上次读取内容；正在验证来源。");
            }
        }
        if (fresh || entry.observedAttributes!=fingerprint) {
            entry.observedAttributes=fingerprint;
            if (m_pending.key!=device.key) { entry.state.stale=true; entry.state.available=false; entry.needsRead=true; dirty.append(device); }
            else if (m_pending.operation=="read") entry.needsRead=true;
            emit changed(device.key);
        }
    }
    const auto keys=m_entries.keys();
    for (const auto &key:keys) if (!seen.contains(key)) {
        if (m_pending.key==key) finish(false,QStringLiteral("摄像头已移除或元数据能力已消失。"));
        m_entries.remove(key); emit changed(key);
    }
    m_observing=false;
    if (m_pending.key.isEmpty() && !dirty.isEmpty()) {
        const auto device=dirty.first(); QTimer::singleShot(0,this,[this,device] { if (canRead(device)) read(device); });
    }
}

void CameraMetadataManager::persist() {
    if (m_path.isEmpty()) return;
    while (m_mirrors.size()>1000) m_mirrors.remove(m_mirrors.begin().key());
    const auto bytes=QJsonDocument(QJsonObject{{"version",1},{"mirrors",m_mirrors}}).toJson(QJsonDocument::Compact);
    if (bytes.size()>MaxStoreBytes) return;
    QDir dir(m_directory); if (!dir.exists() && !dir.mkpath(".")) return;
    QFile::setPermissions(m_directory,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner);
    QSaveFile file(m_path); if (!file.open(QIODevice::WriteOnly)) return;
    file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
    if (file.write(bytes)!=bytes.size()) { file.cancelWriting(); return; }
    file.commit();
}
void CameraMetadataManager::load() {
    if (m_path.isEmpty()) return;
    QFile file(m_path); if (!file.exists() || file.size()>MaxStoreBytes || !file.open(QIODevice::ReadOnly)) return;
    const auto document=QJsonDocument::fromJson(file.readAll()); const auto root=document.object();
    if (!document.isObject() || root.size()!=2 || root.value("version")!=1 || !root.value("mirrors").isObject()) return;
    const auto mirrors=root.value("mirrors").toObject(); if (mirrors.size()>1000) return;
    QJsonObject valid;
    for (auto it=mirrors.begin();it!=mirrors.end();++it) {
        const auto entry=it.value().toObject(); CameraMetadataSnapshot snapshot; QList<CameraMetadataRoom> rooms;
        if (entry.size()!=2 || !parseCameraMetadataSnapshot(entry.value("snapshot"),&snapshot) || snapshot.identity!=it.key()
            || clearedData(snapshot) || !validObservation(snapshot.observedAt) || !parseCameraMetadataRooms(entry.value("rooms"),&rooms)) return;
        valid.insert(it.key(),entry);
    }
    m_mirrors=valid;
}
