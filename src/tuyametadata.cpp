#include "tuyametadata.h"
#include "matterclient.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <functional>

namespace {
constexpr qint64 MaxResponse = 4 * 1024 * 1024;
constexpr qint64 MaxStoredState = 8 * 1024 * 1024;
bool text(const QJsonValue &v, int limit = 4096) {
    if (!v.isString() || v.toString().size() > limit) return false;
    const auto s = v.toString();
    for (int i = 0; i < s.size(); ++i) {
        const auto c = s.at(i);
        if (c.isHighSurrogate()) { if (++i >= s.size() || !s.at(i).isLowSurrogate()) return false; }
        else if (c.isLowSurrogate() || c.category() == QChar::Other_Control) return false;
    }
    return true;
}
bool identifier(const QJsonValue &v) { return text(v, 256) && !v.toString().isEmpty(); }
bool uuid(const QJsonValue &v) {
    static const QRegularExpression re(QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return v.isString() && re.match(v.toString()).hasMatch();
}
bool header(const QJsonObject &o) {
    static const QRegularExpression revision(QStringLiteral("^(0|[1-9][0-9]{0,39})$"));
    if (o.value("schemaVersion") != 1 || !uuid(o.value("serverId")) || !uuid(o.value("sourceEpoch")) ||
        !o.value("revision").isString() || !revision.match(o.value("revision").toString()).hasMatch() ||
        !o.value("available").isBool() || !o.value("roomsAvailable").isBool() || !identifier(o.value("sourceKind"))) return false;
    const auto caps = o.value("capabilities").toObject();
    for (const auto &key : {"read", "renameDevice", "moveDevice", "renameRoom"}) if (!caps.value(key).isBool()) return false;
    const auto observed = o.value("observedAt");
    if (!observed.isNull() && (!observed.isString() || !observed.toString().endsWith('Z') ||
        !QDateTime::fromString(observed.toString(), Qt::ISODateWithMs).isValid())) return false;
    return true;
}
bool validName(const QString &s) {
    if (!text(s) || s.trimmed().isEmpty() || s.toUtf8().size() > 160) return false;
    return s.toUcs4().size() <= 50;
}
bool authoritative(const QJsonObject &snapshot) {
    return snapshot.value("available").toBool() && snapshot.value("capabilities").toObject().value("read").toBool() &&
        snapshot.value("observedAt").isString() && QDateTime::fromString(snapshot.value("observedAt").toString(),Qt::ISODateWithMs).isValid();
}
QJsonObject find(const QJsonArray &array, const QString &id) {
    for (const auto &value : array) if (value.toObject().value("id") == id) return value.toObject();
    return {};
}
bool validBase(const QUrl &url) {
    if (!url.isValid() || url.isRelative() || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) return false;
    if (url.scheme() != "https" && url.scheme() != "http") return false;
    if (url.scheme() == "http" && url.host() != "localhost" && url.host() != "127.0.0.1" && url.host() != "::1") return false;
    const auto path = url.path(QUrl::FullyEncoded);
    return !path.contains('%') && !path.contains("//") && !path.split('/').contains("..") && !path.split('/').contains(".");
}
bool validToken(const QString &token) {
    if (token.size() < 32 || token.size() > 4096) return false;
    for (auto c : token) if (c.unicode() < 33 || c.unicode() > 126) return false;
    return true;
}
QString safeCode(const QJsonValue &value) {
    const QSet<QString> codes{"stale_revision", "wrong_source", "unsupported_operation", "source_unavailable", "permission_denied",
        "confirmation_failed", "write_uncertain", "invalid_request", "queue_full"};
    return codes.contains(value.toString()) ? value.toString() : QStringLiteral("request_failed");
}
}

QString tuyaMatterUniqueId(const MatterDevice &device) {
    const auto value = device.attributes.value(QStringLiteral("%1/57/18").arg(device.endpointId));
    return identifier(value) ? value.toString() : QString();
}

bool parseTuyaMetadataSnapshot(const QJsonValue &value, QJsonObject *result, QString *error) {
    auto fail = [error] { if (error) *error = QStringLiteral("invalid_snapshot"); return false; };
    if (!value.isObject()) return fail();
    const auto o = value.toObject();
    if (!header(o) || !o.value("homes").isArray() || !o.value("rooms").isArray() || !o.value("devices").isArray()) return fail();
    QSet<QString> homeIds, roomIds, deviceIds;
    QHash<QString, QString> roomHomes;
    for (auto v : o.value("homes").toArray()) {
        const auto h = v.toObject();
        if (!v.isObject() || !identifier(h.value("id")) || !text(h.value("name")) || homeIds.contains(h.value("id").toString())) return fail();
        homeIds.insert(h.value("id").toString());
    }
    for (auto v : o.value("rooms").toArray()) {
        const auto r = v.toObject(); const auto id = r.value("id").toString(), home = r.value("homeId").toString();
        if (!v.isObject() || !identifier(r.value("id")) || !identifier(r.value("homeId")) || !text(r.value("name")) || roomIds.contains(id) ||
            (o.value("roomsAvailable").toBool() && !homeIds.contains(home))) return fail();
        roomIds.insert(id); roomHomes.insert(id, home);
    }
    for (auto v : o.value("devices").toArray()) {
        const auto device = v.toObject(); const auto id = device.value("id").toString();
        const auto home = device.value("homeId"), room = device.value("roomId");
        const auto status = device.value("roomStatus").toString();
        if (!v.isObject() || !identifier(device.value("id")) || !text(device.value("name")) || deviceIds.contains(id) ||
            (!home.isNull() && !identifier(home)) || (!room.isNull() && !identifier(room)) ||
            !QStringList{"assigned", "unassigned", "unknown", "unsupported"}.contains(status)) return fail();
        if (status == "assigned" && (home.isNull() || room.isNull())) return fail();
        if (o.value("roomsAvailable").toBool() && ((!home.isNull() && !homeIds.contains(home.toString())) ||
            (status == "assigned" && (!roomIds.contains(room.toString()) || roomHomes.value(room.toString()) != home.toString())))) return fail();
        const auto bindings = device.value("bindings").toObject();
        if (!device.value("bindings").isObject() || !bindings.value("matterUniqueIds").isArray()) return fail();
        QSet<QString> uniques;
        for (auto unique : bindings.value("matterUniqueIds").toArray()) {
            if (!identifier(unique) || uniques.contains(unique.toString())) return fail();
            uniques.insert(unique.toString());
        }
        deviceIds.insert(id);
    }
    // Copy only protocol data. Unknown fields cannot carry credentials into persistence.
    QJsonObject normalized;
    for (const auto &key : {"schemaVersion", "serverId", "sourceEpoch", "revision", "observedAt", "available", "roomsAvailable", "sourceKind", "capabilities"}) normalized.insert(key, o.value(key));
    QJsonObject limitations;
    for (const auto &key : {"read", "renameDevice", "moveDevice", "renameRoom"}) {
        const auto v = o.value("limitations").toObject().value(key);
        static const QRegularExpression reason(QStringLiteral("^[a-z][a-z0-9_]{0,95}$"));
        if (v.isString() && reason.match(v.toString()).hasMatch()) limitations.insert(key, v);
    }
    normalized.insert("limitations", limitations);
    QJsonArray homes, rooms, devices;
    for (auto v : o.value("homes").toArray()) { const auto h = v.toObject(); homes.append(QJsonObject{{"id",h.value("id")},{"name",h.value("name")}}); }
    for (auto v : o.value("rooms").toArray()) { const auto r = v.toObject(); rooms.append(QJsonObject{{"id",r.value("id")},{"homeId",r.value("homeId")},{"name",r.value("name")}}); }
    for (auto v : o.value("devices").toArray()) {
        const auto a = v.toObject(); QJsonObject clean;
        for (const auto &key : {"id", "name", "homeId", "roomId", "roomStatus"}) clean.insert(key,a.value(key));
        clean.insert("bindings", QJsonObject{{"matterUniqueIds",a.value("bindings").toObject().value("matterUniqueIds")}});
        devices.append(clean);
    }
    normalized.insert("homes", homes); normalized.insert("rooms", rooms); normalized.insert("devices", devices);
    if (result) *result = normalized;
    return true;
}

struct TuyaMetadataManager::Private {
    TuyaMetadataManager *q;
    QPointer<MatterClient> client;
    QNetworkAccessManager network;
    QPointer<QNetworkReply> reply;
    QTimer poll;
    QString directory, profile, token, status = QStringLiteral("涂鸦同步尚未配置"), sourceHome, serverId, epoch;
    QUrl base;
    QJsonObject snap, bindings, pending;
    bool enabled = false, homeSelected = false, fresh = false, uncertain = false, busy = false, storageBlocked = false;
    QString mode = QStringLiteral("manual");
    quint64 generation = 0;
    qint64 receiptDeadline = 0;

    Private(TuyaMetadataManager *owner, MatterClient *matter, QString dir) : q(owner), client(matter), network(owner), directory(std::move(dir)) {
        poll.setInterval(60000);
        QObject::connect(&poll, &QTimer::timeout, q, [this] { if (!busy && !base.isEmpty() && !token.isEmpty()) q->refresh(); });
        if (client) QObject::connect(client, &MatterClient::devicesChanged, q, [this] { emit q->changed(); });
    }
    QString path() const { return directory + '/' + QString::fromLatin1(QCryptographicHash::hash(profile.toUtf8(), QCryptographicHash::Sha256).toHex()) + ".json"; }
    void notify(const QString &value = {}) {
        if (!value.isEmpty()) { status = value; emit q->statusChanged(status); }
        emit q->changed();
    }
    bool save() {
        if (profile.isEmpty() || storageBlocked) return false;
        if (!QDir().mkpath(directory)) return false;
        QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QSaveFile file(path());
        if (!file.open(QIODevice::WriteOnly)) return false;
        if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) { file.cancelWriting(); return false; }
        const QJsonObject data{{"schemaVersion",1},{"enabled",enabled},{"mode",mode},{"baseUrl",base.toString(QUrl::FullyEncoded)},
            {"serverId",serverId},{"sourceEpoch",epoch},{"homeSelected",homeSelected},{"sourceHomeId", sourceHome.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(sourceHome)},
            {"bindings",bindings},{"snapshot",snap},{"pending",pending}};
        const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
        if (bytes.size() > MaxStoredState) { file.cancelWriting(); return false; }
        if (file.write(bytes) != bytes.size()) { file.cancelWriting(); return false; }
        return file.commit();
    }
    bool current(const MatterDevice &device) const {
        if (device.key.isEmpty() || profile.isEmpty()) return false;
        if (!client) return true;
        for (const auto &candidate : client->devices()) if (candidate.key == device.key && candidate.controllerGeneration == device.controllerGeneration && tuyaMatterUniqueId(candidate) == tuyaMatterUniqueId(device)) return true;
        return false;
    }
    bool identityMatches(const MatterDevice &device, const QJsonObject &record) const {
        if (!current(device) || !record.contains("matterUniqueId")) return false;
        MatterDevice live = device;
        if (client) for (const auto &candidate : client->devices()) if (candidate.key == device.key) { live = candidate; break; }
        if (tuyaMatterUniqueId(live) != record.value("matterUniqueId").toString()) return false;
        // Explicit bindings without a stable identifier are scoped to this connection.
        return !tuyaMatterUniqueId(live).isEmpty() || record.value("controllerGeneration").toString() == QString::number(live.controllerGeneration);
    }
    bool homeValid() const {
        if (!homeSelected) return false;
        if (sourceHome.isEmpty()) return snap.value("sourceKind") == "tuya-project-custom" && !snap.value("roomsAvailable").toBool();
        return !find(snap.value("homes").toArray(), sourceHome).isEmpty();
    }
    QJsonObject source(const QString &key) const {
        auto item = find(snap.value("devices").toArray(), bindings.value(key).toObject().value("sourceId").toString());
        if (item.isEmpty() || !homeValid()) return {};
        if (!sourceHome.isEmpty() && item.value("homeId").toString() != sourceHome) return {};
        return item;
    }
    QJsonObject expected() const { return {{"serverId",snap.value("serverId")},{"sourceEpoch",snap.value("sourceEpoch")},{"revision",snap.value("revision")}}; }
    void cancel() {
        ++generation; poll.stop();
        if (reply) { QObject::disconnect(reply, nullptr, q, nullptr); reply->abort(); reply->deleteLater(); reply = nullptr; }
        busy = false;
    }
    using Done = std::function<void(bool,QJsonObject)>;
    void request(const QString &route, const QJsonObject *body, Done done) {
        if (reply || profile.isEmpty() || token.isEmpty()) { done(false, {}); return; }
        QUrl url = base; QString prefix = url.path(); while (prefix.endsWith('/')) prefix.chop(1); url.setPath(prefix + route);
        QNetworkRequest req(url);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        req.setTransferTimeout(5000);
        req.setRawHeader("Authorization", "Bearer " + token.toLatin1()); req.setRawHeader("Accept", "application/json");
        QNetworkReply *active;
        if (body) { req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json"); active = network.post(req, QJsonDocument(*body).toJson(QJsonDocument::Compact)); }
        else active = network.get(req);
        reply = active;
        auto *deadline = new QTimer(active);
        deadline->setSingleShot(true);
        QObject::connect(deadline, &QTimer::timeout, active, &QNetworkReply::abort);
        deadline->start(5000);
        const auto gen = generation;
        auto bytes = std::make_shared<QByteArray>(); auto oversized = std::make_shared<bool>(false);
        QObject::connect(active, &QNetworkReply::readyRead, q, [active, bytes, oversized] {
            if (active->bytesAvailable() > MaxResponse - bytes->size()) { *oversized = true; active->abort(); return; }
            bytes->append(active->readAll());
        });
        QObject::connect(active, &QNetworkReply::metaDataChanged, q, [active, oversized] {
            bool ok = false; const auto length = active->header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok);
            if (ok && (length < 0 || length > MaxResponse)) { *oversized = true; active->abort(); }
        });
        QObject::connect(active, &QNetworkReply::finished, q, [this, active, bytes, oversized, gen, done = std::move(done), post = body != nullptr] {
            if (gen != generation) { active->deleteLater(); return; }
            reply = nullptr;
            if (active->bytesAvailable() <= MaxResponse - bytes->size()) bytes->append(active->readAll()); else *oversized = true;
            const int code = active->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto type = active->header(QNetworkRequest::ContentTypeHeader).toString().section(';',0,0).trimmed().toLower();
            const bool ok = !*oversized && active->error() == QNetworkReply::NoError && code == (post ? 202 : 200) && type == "application/json";
            const auto doc = !*oversized && type == "application/json" ? QJsonDocument::fromJson(*bytes) : QJsonDocument();
            QJsonObject response = ok ? doc.object() : QJsonObject{};
            // A well-formed protocol rejection proves a POST was refused. Never
            // treat transport errors, proxy pages, or missing receipts as refusal.
            if (!ok && post && code >= 400 && code < 500 && doc.isObject() && doc.object().value("schemaVersion") == 1 && safeCode(doc.object().value("code")) != "request_failed")
                response = {{"_httpStatus",code},{"code",safeCode(doc.object().value("code"))}};
            active->deleteLater(); done(ok && doc.isObject(), response);
        });
    }
    bool accept(const QJsonObject &value) {
        QJsonObject normalized;
        if (!parseTuyaMetadataSnapshot(value, &normalized)) return false;
        if (!serverId.isEmpty() && serverId != normalized.value("serverId").toString()) { notify(QStringLiteral("来源身份不同，请显式重置来源")); return false; }
        const bool changedEpoch = !epoch.isEmpty() && epoch != normalized.value("sourceEpoch").toString();
        serverId = normalized.value("serverId").toString(); epoch = normalized.value("sourceEpoch").toString();
        snap = normalized; fresh = true;
        if (changedEpoch) {
            if (!pending.isEmpty()) { pending.insert("status", "uncertain"); uncertain = true; }
            for (auto it = bindings.begin(); it != bindings.end(); ++it) { auto record = it.value().toObject(); record.insert("conflict",true); it.value() = record; }
        }
        if (authoritative(snap)) {
            for (auto it = bindings.begin(); it != bindings.end(); ++it) {
                auto record = it.value().toObject(); const auto src = source(it.key());
                if (src.isEmpty() || !record.value("following").toBool() || !record.contains("baselineName") || record.value("conflict").toBool()) continue;
                const auto baseline = record.value("baselineName").toString();
                const auto managed = record.value("managedName").toString(), name = src.value("name").toString();
                if (name != baseline && managed != baseline && managed != name) record.insert("conflict",true);
                else if (name != baseline && mode != "manual") {
                    record.insert("managedName",name); record.insert("baselineName",name);
                }
                if (snap.value("roomsAvailable").toBool() && mode != "manual") {
                    const auto room = find(snap.value("rooms").toArray(), src.value("roomId").toString());
                    if (src.value("roomStatus") == "assigned") record.insert("roomName",room.value("name"));
                    else if (src.value("roomStatus") == "unassigned") record.insert("roomName",QString());
                }
                it.value() = record;
            }
        }
        if (!save()) { fresh = false; notify(QStringLiteral("无法安全保存同步状态")); return false; }
        return true;
    }
    void finish(bool success, const QString &detail, bool keep = false) {
        const auto original = pending;
        const auto key = pending.value("deviceKey").toString(), type = pending.value("operation").toObject().value("type").toString();
        if (keep) { uncertain = true; fresh = false; pending.insert("status","uncertain"); }
        else { pending = {}; uncertain = false; }
        busy = false;
        if (!save()) { pending = original; pending.insert("status","uncertain"); uncertain = true; fresh = false; success = false; }
        notify(detail); emit q->operationFinished(key,type,success,detail);
    }
    bool confirms(const QJsonObject &snapshotValue, const QJsonObject &operation) {
        const auto type = operation.value("type").toString(), id = operation.value("targetId").toString();
        if (type == "device.rename") return find(snapshotValue.value("devices").toArray(),id).value("name") == operation.value("name");
        if (type == "device.move") { const auto item = find(snapshotValue.value("devices").toArray(),id); return snapshotValue.value("roomsAvailable").toBool() && item.value("roomStatus") == "assigned" && item.value("roomId") == operation.value("roomId"); }
        if (type == "room.rename") return snapshotValue.value("roomsAvailable").toBool() && find(snapshotValue.value("rooms").toArray(),id).value("name") == operation.value("name");
        return false;
    }
    void receipt(bool ok, const QJsonObject &value) {
        if (!ok && value.value("_httpStatus").toInt() >= 400) {
            const auto code = safeCode(value.value("code"));
            if (code == "stale_revision" || code == "wrong_source") {
                const auto key = pending.value("deviceKey").toString(); auto record = bindings.value(key).toObject();
                if (!record.isEmpty()) { record.insert("conflict",true); bindings.insert(key,record); }
            }
            finish(false,QStringLiteral("来源拒绝写入：") + code); return;
        }
        if (!ok || value.value("schemaVersion") != 1 || value.value("commandId") != pending.value("commandId")) { finish(false,QStringLiteral("写入结果未知；不会自动重试"),true); return; }
        const auto state = value.value("status").toString();
        if (state == "queued" || state == "running") {
            if (QDateTime::currentMSecsSinceEpoch() >= receiptDeadline) { finish(false,QStringLiteral("写入确认超时；不会自动重试"),true); return; }
            pending.insert("status",state); save(); const auto gen = generation;
            QTimer::singleShot(250, q, [this,gen] { if (gen == generation && busy && !pending.isEmpty()) request("/v1/metadata/commands/" + pending.value("commandId").toString(),nullptr,[this](bool good,QJsonObject response){ receipt(good,response); }); });
            return;
        }
        if (state == "confirmed") {
            QJsonObject normalized; const auto operation = pending.value("operation").toObject();
            if (!parseTuyaMetadataSnapshot(value.value("snapshot"),&normalized) || normalized.value("serverId") != pending.value("expected").toObject().value("serverId") ||
                normalized.value("sourceEpoch") != pending.value("expected").toObject().value("sourceEpoch") || !authoritative(normalized) || !confirms(normalized,operation) || !accept(normalized)) {
                finish(false,QStringLiteral("缺少权威写入确认；不会自动重试"),true); return;
            }
            const auto key = pending.value("deviceKey").toString();
            if (operation.value("type") == "device.rename") {
                auto record = bindings.value(key).toObject(); record.insert("managedName",operation.value("name")); record.insert("baselineName",operation.value("name"));
                record.insert("following",true); record.insert("conflict",false); bindings.insert(key,record);
            } else if (operation.value("type") == "device.move") {
                auto record = bindings.value(key).toObject(); record.insert("roomName",find(snap.value("rooms").toArray(),operation.value("roomId").toString()).value("name")); bindings.insert(key,record);
            }
            finish(true,QStringLiteral("来源已确认")); return;
        }
        if (state == "failed" || state == "conflict") {
            if (state == "conflict") { const auto key = pending.value("deviceKey").toString(); auto record = bindings.value(key).toObject(); if (!record.isEmpty()) { record.insert("conflict",true); bindings.insert(key,record); } }
            finish(false,QStringLiteral("来源拒绝写入：") + safeCode(value.value("code"))); return;
        }
        finish(false,QStringLiteral("写入结果未知；不会自动重试"),true);
    }
    bool submit(const QString &key, const QJsonObject &operation, const MatterDevice &device = {}) {
        if (busy || uncertain || !pending.isEmpty() || !fresh || !enabled || !homeValid() || token.isEmpty() || !authoritative(snap)) return false;
        busy = true;
        const auto guard = expected(); const auto gen = generation;
        request("/v1/metadata",nullptr,[this,key,operation,guard,gen,device](bool ok,QJsonObject value) {
            if (gen != generation) return;
            const auto binding = bindings.value(key).toObject();
            if (!ok || !accept(value) || expected() != guard || !authoritative(snap) || !enabled || !homeValid() ||
                (!key.isEmpty() && (!identityMatches(device,binding) || binding.value("sourceId") != operation.value("targetId")))) {
                busy = false; notify(QStringLiteral("来源或设备身份已变化，请刷新并重新确认草稿")); emit q->operationFinished(key,operation.value("type").toString(),false,status); return;
            }
            const auto caps = snap.value("capabilities").toObject(); const auto type = operation.value("type").toString();
            if ((type == "device.rename" && !caps.value("renameDevice").toBool()) || (type == "device.move" && (!caps.value("moveDevice").toBool() || !snap.value("roomsAvailable").toBool())) ||
                (type == "room.rename" && (!caps.value("renameRoom").toBool() || !snap.value("roomsAvailable").toBool()))) { busy = false; notify(QStringLiteral("来源不支持此写入")); return; }
            pending = {{"schemaVersion",1},{"commandId",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"expected",guard},{"operation",operation},{"deviceKey",key},{"status","queued"}};
            if (!save()) { pending = {}; busy = false; notify(QStringLiteral("无法保存写入账本；操作未发送")); return; }
            const QJsonObject command{{"schemaVersion",1},{"commandId",pending.value("commandId")},{"expected",guard},{"operation",operation}};
            receiptDeadline = QDateTime::currentMSecsSinceEpoch() + 15000;
            request("/v1/metadata/commands",&command,[this](bool good,QJsonObject response) { receipt(good,response); }); notify(QStringLiteral("等待来源确认"));
        });
        notify(QStringLiteral("正在检查来源版本")); return true;
    }
};

TuyaMetadataManager::TuyaMetadataManager(MatterClient *client, QObject *parent)
    : TuyaMetadataManager(client,QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/tuya-metadata",parent) {}
TuyaMetadataManager::TuyaMetadataManager(MatterClient *client, const QString &directory, QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this,client,directory)) {}
TuyaMetadataManager::~TuyaMetadataManager() { d->cancel(); }

void TuyaMetadataManager::setProfile(const QString &scope) {
    if (scope == d->profile) return;
    if (!d->pending.isEmpty()) { d->pending.insert("status","uncertain"); d->save(); }
    d->cancel(); d->profile = scope; d->token.clear(); d->base = QUrl(); d->snap = {}; d->bindings = {}; d->pending = {};
    d->serverId.clear(); d->epoch.clear(); d->sourceHome.clear(); d->homeSelected = false; d->fresh = false; d->uncertain = false; d->enabled = false; d->mode = "manual"; d->storageBlocked = false;
    if (!scope.isEmpty()) {
        QFile file(d->path());
        if (file.exists()) {
            const bool readable = file.open(QIODevice::ReadOnly) && file.size() <= MaxStoredState;
            const auto document = readable ? QJsonDocument::fromJson(file.readAll()) : QJsonDocument();
            const auto data = document.object();
            const QUrl storedBase(data.value("baseUrl").toString());
            const bool validState = document.isObject() && data.value("schemaVersion") == 1 && data.value("enabled").isBool() &&
                QStringList{"manual","followSource","bidirectional"}.contains(data.value("mode").toString()) && data.value("baseUrl").isString() &&
                (storedBase.isEmpty() || validBase(storedBase)) && data.value("homeSelected").isBool() &&
                (data.value("sourceHomeId").isNull() || identifier(data.value("sourceHomeId"))) &&
                (data.value("serverId").toString().isEmpty() || uuid(data.value("serverId"))) &&
                (data.value("sourceEpoch").toString().isEmpty() || uuid(data.value("sourceEpoch"))) &&
                data.value("bindings").isObject() && data.value("pending").isObject() && data.value("snapshot").isObject();
            QJsonObject cached;
            const bool validSnapshot = data.value("snapshot").toObject().isEmpty() || parseTuyaMetadataSnapshot(data.value("snapshot"),&cached);
            if (validState && validSnapshot) {
                const QUrl base(data.value("baseUrl").toString()); if (validBase(base)) d->base = base;
                d->enabled = data.value("enabled").toBool();
                if (QStringList{"manual","followSource","bidirectional"}.contains(data.value("mode").toString())) d->mode = data.value("mode").toString();
                d->homeSelected = data.value("homeSelected").toBool(); d->sourceHome = data.value("sourceHomeId").toString();
                if (uuid(data.value("serverId"))) d->serverId = data.value("serverId").toString();
                if (uuid(data.value("sourceEpoch"))) d->epoch = data.value("sourceEpoch").toString();
                d->snap = cached;
                d->bindings = data.value("bindings").toObject(); d->pending = data.value("pending").toObject();
                if (!d->pending.isEmpty()) { d->pending.insert("status","uncertain"); d->uncertain = true; d->save(); }
            } else d->storageBlocked = true;
        }
    }
    d->notify(d->storageBlocked ? QStringLiteral("同步账本损坏或不可读取；请先核查未知写入，再显式重置来源") : (scope.isEmpty() ? QStringLiteral("请先连接正式 Matter 控制器") : QStringLiteral("请提供本次会话的涂鸦服务令牌")));
}
QString TuyaMetadataManager::profile() const { return d->profile; }
bool TuyaMetadataManager::configure(const QUrl &url, const QString &token) {
    if (d->storageBlocked) { d->notify(QStringLiteral("账本不可读取，禁止覆盖；请先核查未知写入，再显式重置来源")); return false; }
    if (d->profile.isEmpty() || !validBase(url) || !validToken(token) || d->busy) { d->notify(QStringLiteral("需正式控制器、有效 HTTPS/回环地址和至少32位令牌")); return false; }
    if (!d->base.isEmpty() && d->base != url && (!d->bindings.isEmpty() || !d->pending.isEmpty())) { d->notify(QStringLiteral("更改服务前请先显式重置来源")); return false; }
    d->base = url; d->token = token; d->fresh = false;
    if (!d->save()) { d->token.clear(); d->notify(QStringLiteral("无法安全保存配置")); return false; }
    d->poll.start(); return refresh();
}
QUrl TuyaMetadataManager::baseUrl() const { return d->base; }
bool TuyaMetadataManager::isConfigured() const { return !d->profile.isEmpty() && !d->base.isEmpty() && !d->token.isEmpty(); }
bool TuyaMetadataManager::enabled() const { return d->enabled; }
bool TuyaMetadataManager::setEnabled(bool enabled) { if (d->profile.isEmpty() || d->storageBlocked) return false; d->enabled = enabled; const bool saved = d->save(); d->notify(); return saved; }
QString TuyaMetadataManager::mode() const { return d->mode; }
bool TuyaMetadataManager::setMode(const QString &mode) { if (d->profile.isEmpty() || d->storageBlocked || !QStringList{"manual","followSource","bidirectional"}.contains(mode)) return false; d->mode = mode; const bool saved = d->save(); d->notify(); return saved; }
bool TuyaMetadataManager::refresh() {
    if (!isConfigured() || d->busy || d->reply) return false;
    d->busy = true; d->notify(QStringLiteral("正在读取涂鸦元数据"));
    d->request("/v1/capabilities",nullptr,[this](bool ok,QJsonObject caps) {
        if (!ok || !header(caps)) { d->busy = false; d->fresh = false; d->notify(QStringLiteral("服务读取失败；保留现有资料")); return; }
        d->request("/v1/metadata",nullptr,[this,caps](bool good,QJsonObject value) {
            d->busy = false;
            if (!good || value.value("serverId") != caps.value("serverId") || value.value("sourceEpoch") != caps.value("sourceEpoch") || !d->accept(value)) { d->fresh = false; d->notify(QStringLiteral("服务资料无效或身份不符；保留现有资料")); return; }
            d->notify(d->uncertain ? QStringLiteral("有未知写入；请检查来源后明确确认") : (d->snap.value("available").toBool() ? QStringLiteral("已读取；本地别名与米家资料保持独立") : QStringLiteral("来源暂不可用；未将未知房间视为空")));
        });
    }); return true;
}
QJsonObject TuyaMetadataManager::snapshot() const { return d->snap; }
QString TuyaMetadataManager::statusText() const { return d->status; }
bool TuyaMetadataManager::requestInProgress() const { return d->busy; }
bool TuyaMetadataManager::hasUncertainWrite() const { return d->uncertain; }
QList<MatterDevice> TuyaMetadataManager::devices() const { return d->client ? d->client->devices() : QList<MatterDevice>{}; }
TuyaMetadataState TuyaMetadataManager::state(const MatterDevice &device) const {
    TuyaMetadataState state;
    const auto record = d->bindings.value(device.key).toObject(); state.bound = !record.isEmpty(); state.deviceId = record.value("sourceId").toString();
    state.busy = d->busy || d->uncertain; state.status = d->status; state.managedName = record.value("managedName").toString(); state.hasBaseline = record.contains("baselineName");
    const auto src = d->source(device.key); state.known = !src.isEmpty(); state.fullName = src.value("name").toString(); state.roomId = src.value("roomId").toString();
    state.homeName = find(d->snap.value("homes").toArray(),src.value("homeId").toString()).value("name").toString();
    state.roomName = src.value("roomStatus") == "assigned" && d->snap.value("roomsAvailable").toBool() ? find(d->snap.value("rooms").toArray(),state.roomId).value("name").toString() : record.value("roomName").toString();
    state.observedAt = d->snap.value("observedAt").toString();
    const bool identityMatches = d->identityMatches(device,record);
    state.available = identityMatches && state.known && d->fresh && authoritative(d->snap);
    state.conflict = record.value("conflict").toBool() || (state.hasBaseline && state.known && d->mode == "manual" && state.fullName != record.value("baselineName").toString());
    const auto caps = d->snap.value("capabilities").toObject(); const bool can = state.available && !state.busy && d->enabled && state.hasBaseline && !state.conflict && isConfigured();
    state.canRename = can && caps.value("renameDevice").toBool(); state.canMoveRoom = can && d->snap.value("roomsAvailable").toBool() && caps.value("moveDevice").toBool();
    state.canRenameRoom = can && d->snap.value("roomsAvailable").toBool() && caps.value("renameRoom").toBool();
    return state;
}
QString TuyaMetadataManager::displayName(const MatterDevice &device) const {
    const auto s = state(device); return d->enabled && s.available && s.hasBaseline && !s.conflict && d->bindings.value(device.key).toObject().value("following").toBool() ? s.managedName : QString();
}
QString TuyaMetadataManager::roomName(const MatterDevice &device) const {
    const auto s = state(device); return d->enabled && s.available && s.hasBaseline && !s.conflict && d->bindings.value(device.key).toObject().value("following").toBool() ? s.roomName : QString();
}
QString TuyaMetadataManager::suggestedBinding(const MatterDevice &device) const {
    const auto identity = tuyaMatterUniqueId(device); if (identity.isEmpty() || !d->fresh || !d->homeValid() || !d->current(device)) return {};
    int controllerMatches = 0;
    for (const auto &candidate : devices()) if (tuyaMatterUniqueId(candidate) == identity) ++controllerMatches;
    if (d->client && controllerMatches != 1) return {};
    QString result; int sourceMatches = 0;
    for (auto value : d->snap.value("devices").toArray()) {
        const auto src = value.toObject(); if (!d->sourceHome.isEmpty() && src.value("homeId") != d->sourceHome) continue;
        if (src.value("bindings").toObject().value("matterUniqueIds").toArray().contains(identity)) { result = src.value("id").toString(); ++sourceMatches; }
    }
    return sourceMatches == 1 ? result : QString();
}
bool TuyaMetadataManager::bindDevice(const MatterDevice &device, const QString &id) {
    if (!d->current(device) || !d->fresh || !d->homeValid() || d->busy || !d->pending.isEmpty()) return false;
    const auto src = find(d->snap.value("devices").toArray(),id);
    if (src.isEmpty() || (!d->sourceHome.isEmpty() && src.value("homeId") != d->sourceHome)) return false;
    const auto identity = tuyaMatterUniqueId(device);
    for (auto it = d->bindings.begin(); it != d->bindings.end(); ++it) {
        if (it.key() == device.key || it.value().toObject().value("sourceId") != id) continue;
        const auto other = it.value().toObject().value("matterUniqueId").toString();
        const auto sourceIds = src.value("bindings").toObject().value("matterUniqueIds").toArray();
        if (identity.isEmpty() || other.isEmpty() || identity == other || !sourceIds.contains(identity) || !sourceIds.contains(other)) return false;
    }
    d->bindings.insert(device.key,QJsonObject{{"sourceId",id},{"matterUniqueId",identity},{"controllerGeneration",QString::number(device.controllerGeneration)},{"following",false},{"conflict",false}});
    const bool saved = d->save(); d->notify(); emit deviceChanged(device.key); return saved;
}
bool TuyaMetadataManager::followSource(const MatterDevice &device) {
    if (!d->identityMatches(device,d->bindings.value(device.key).toObject()) || !d->fresh || !d->enabled || d->busy || !d->pending.isEmpty() || !authoritative(d->snap)) return false;
    const auto src = d->source(device.key); if (src.isEmpty()) return false;
    auto record = d->bindings.value(device.key).toObject(); record.insert("following",true); record.insert("managedName",src.value("name")); record.insert("baselineName",src.value("name")); record.insert("conflict",false);
    if (d->snap.value("roomsAvailable").toBool()) {
        if (src.value("roomStatus") == "assigned") record.insert("roomName",find(d->snap.value("rooms").toArray(),src.value("roomId").toString()).value("name"));
        else if (src.value("roomStatus") == "unassigned") record.insert("roomName",QString());
    }
    d->bindings.insert(device.key,record); const bool saved = d->save(); d->notify(); emit deviceChanged(device.key); return saved;
}
bool TuyaMetadataManager::rename(const MatterDevice &device, const QString &name) {
    if (!validName(name) || !state(device).canRename) return false;
    if (name == state(device).fullName) return followSource(device);
    return d->submit(device.key,{{"type","device.rename"},{"targetId",state(device).deviceId},{"name",name}},device);
}
bool TuyaMetadataManager::moveRoom(const MatterDevice &device, const QString &id) {
    if (!state(device).canMoveRoom) return false;
    const auto room = find(d->snap.value("rooms").toArray(),id), src = d->source(device.key);
    if (room.isEmpty() || room.value("homeId") != src.value("homeId")) return false;
    return d->submit(device.key,{{"type","device.move"},{"targetId",state(device).deviceId},{"roomId",id}},device);
}
bool TuyaMetadataManager::renameRoom(const QString &id, const QString &name) {
    if (!validName(name) || !d->homeValid() || !d->snap.value("roomsAvailable").toBool() || !d->snap.value("capabilities").toObject().value("renameRoom").toBool()) return false;
    bool bound = false;
    for (auto it = d->bindings.begin(); it != d->bindings.end(); ++it) if (d->source(it.key()).value("roomId") == id) bound = true;
    const auto room = find(d->snap.value("rooms").toArray(),id);
    if (!bound || room.isEmpty() || room.value("homeId") != d->sourceHome) return false;
    return d->submit({},{{"type","room.rename"},{"targetId",id},{"name",name}});
}
bool TuyaMetadataManager::acknowledgeUncertain() {
    if (!d->uncertain || !d->fresh || d->busy || !authoritative(d->snap)) return false;
    const auto previous = d->pending; d->pending = {}; d->uncertain = false; const bool saved = d->save();
    if (!saved) { d->pending = previous; d->uncertain = true; }
    d->notify(QStringLiteral("已人工确认未知写入；不会重发旧命令")); return saved;
}
bool TuyaMetadataManager::setSourceHome(const QString &id) {
    if (d->profile.isEmpty() || d->storageBlocked || (d->busy && !d->snap.isEmpty()) || !d->pending.isEmpty()) return false;
    if (!d->bindings.isEmpty() && (id != d->sourceHome || !d->homeSelected)) return false;
    if (!d->snap.isEmpty() && ((id.isEmpty() && (d->snap.value("sourceKind") != "tuya-project-custom" || d->snap.value("roomsAvailable").toBool())) ||
        (!id.isEmpty() && find(d->snap.value("homes").toArray(),id).isEmpty()))) return false;
    d->sourceHome = id; d->homeSelected = true; const bool saved = d->save(); d->notify(); return saved;
}
QString TuyaMetadataManager::sourceHomeId() const { return d->sourceHome; }
bool TuyaMetadataManager::sourceHomeConfigured() const { return d->homeSelected && d->homeValid(); }
bool TuyaMetadataManager::resetSource() {
    if (d->busy || !d->pending.isEmpty()) return false;
    d->storageBlocked = false;
    d->cancel(); d->token.clear(); d->base = QUrl(); d->snap = {}; d->bindings = {}; d->sourceHome.clear(); d->homeSelected = false; d->fresh = false;
    d->serverId.clear(); d->epoch.clear(); d->enabled = false; d->mode = "manual";
    const bool saved = d->save(); d->notify(QStringLiteral("已重置独立涂鸦来源；本地家庭资料保持独立")); return saved;
}
