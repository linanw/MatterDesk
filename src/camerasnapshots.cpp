#include "camerasnapshots.h"
#include "matterclient.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QHash>
#include <QImageReader>
#include <QJsonDocument>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <algorithm>
#include <functional>

namespace {
constexpr qint64 RefreshInterval = 30000;
constexpr qint64 ManualInterval = 3000;
constexpr qsizetype MaxCompressedBytes = 16 * 1024 * 1024;
constexpr qint64 MaxPixels = 16 * 1024 * 1024;
constexpr int MaxDimension = 4096;

bool positiveInteger(const QJsonValue &value, int &number) {
    number = value.toInt(-1);
    return value.isDouble() && number > 0 && value.toDouble() == number;
}
QSize resolutionOf(const QJsonObject &object) {
    int width = 0, height = 0;
    if (!positiveInteger(object.value("width"), width) || !positiveInteger(object.value("height"), height)) return {};
    return {width, height};
}

QSize snapshotTarget(const QJsonObject &capabilities) {
    const auto features = capabilities.value("features");
    if (features.isArray()) {
        bool supported = false;
        for (const auto &feature : features.toArray())
            supported |= feature.toString().compare("Snapshot", Qt::CaseInsensitive) == 0;
        if (!supported) return {};
    }
    QList<QSize> jpeg;
    for (const auto &entry : capabilities.value("snapshot").toObject().value("capabilities").toArray()) {
        const auto object = entry.toObject();
        if (object.value("image_codec").toString().compare("JPEG", Qt::CaseInsensitive) != 0) continue;
        const auto size = resolutionOf(object.value("resolution").toObject());
        if (size.isValid()) jpeg.append(size);
    }
    if (jpeg.isEmpty()) return {};
    // Prefer the largest advertised thumbnail capability. If all capabilities
    // are larger, pick the smallest and ask the server to scale its frame down.
    std::sort(jpeg.begin(), jpeg.end(), [](QSize a, QSize b) {
        return qint64(a.width()) * a.height() < qint64(b.width()) * b.height();
    });
    QSize chosen = jpeg.first();
    for (const auto &size : jpeg)
        if (size.width() <= 640 && size.height() <= 480) chosen = size;
    if (chosen.width() <= 640 && chosen.height() <= 360) return chosen;
    return chosen.scaled(640, 360, Qt::KeepAspectRatio);
}
bool privateCamera(const QJsonObject &capabilities) {
    const auto privacy = capabilities.value("privacy").toObject();
    return privacy.value("hard_mode_on").toBool(false)
        || privacy.value("soft_livestream_mode_enabled").toBool(false);
}
QString failureText(const QJsonObject &error) {
    switch (error.value("error_code").toInt()) {
    case 102: return QStringLiteral("设备暂时无法提供 JPEG 快照");
    case 103: return QStringLiteral("摄像头资源正忙，稍后重试");
    case 105: return QStringLiteral("设备不支持摄像头快照");
    case 106: return QStringLiteral("隐私模式已开启");
    default: return QStringLiteral("快照获取失败，稍后重试");
    }
}
QImage decodedSnapshot(const QJsonObject &result) {
    if (result.value("codec").toString().compare("JPEG", Qt::CaseInsensitive) != 0) return {};
    const auto value = result.value("data");
    if (!value.isString()) return {};
    const auto encoded = value.toString();
    constexpr qsizetype maxEncoded = ((MaxCompressedBytes + 2) / 3) * 4;
    if (encoded.isEmpty() || encoded.size() > maxEncoded || encoded.size() % 4 != 0) return {};
    // Strict, canonical base64: no Unicode substitutions, whitespace, ignored
    // invalid characters or nonzero padding bits can reach an image decoder.
    int padding = 0;
    for (qsizetype i = 0; i < encoded.size(); ++i) {
        const auto c = encoded.at(i).unicode();
        if (c == '=') {
            ++padding;
            if (padding > 2 || i < encoded.size() - 2) return {};
        } else if (padding || !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '+' || c == '/')) return {};
    }
    const auto ascii = encoded.toLatin1();
    auto decoded = QByteArray::fromBase64Encoding(ascii, QByteArray::AbortOnBase64DecodingErrors);
    if (decoded.decodingStatus != QByteArray::Base64DecodingStatus::Ok
        || decoded.decoded.size() > MaxCompressedBytes || decoded.decoded.toBase64() != ascii) return {};
    QBuffer buffer(&decoded.decoded);
    if (!buffer.open(QIODevice::ReadOnly)) return {};
    QImageReader reader(&buffer);
    const auto format = reader.format().toLower();
    if (format != "jpeg" && format != "jpg") return {};
    const auto size = reader.size();
    if (!size.isValid() || size.width() > MaxDimension || size.height() > MaxDimension
        || qint64(size.width()) * size.height() > MaxPixels) return {};
    reader.setAutoTransform(true);
    reader.setScaledSize(size.scaled(640, 360, Qt::KeepAspectRatio));
    return reader.read();
}

// The job belongs to MatterClient rather than the view/manager. A hidden card or
// a closed window must not abandon an allocated stream when its reply is late.
// A WebSocket disconnect invalidates the job before any old id can be released
// on a replacement controller/fabric.
class SnapshotJob : public QObject {
public:
    using Wanted = std::function<bool()>;
    using Result = std::function<void(const QJsonObject &, const QString &, bool)>;
    using Finished = std::function<void()>;
    SnapshotJob(MatterClient *source, MatterDevice device, Wanted shouldCapture, Result onResult, Finished onFinished)
        : QObject(source), client(source), base{{"node_id", device.nodeId}, {"endpoint_id", device.endpointId}},
          wanted(std::move(shouldCapture)), result(std::move(onResult)), finished(std::move(onFinished)) {
        connect(source, &MatterClient::cameraResponse, this,
            [this](const QString &id, const QString &, bool success, const QJsonValue &value, const QJsonObject &error) {
                if (id != requestId || done) return;
                requestId.clear();
                if (error.value("reason").toString() == "connection-closed") sessionValid = false;
                if (stage == Release) { complete(); return; }
                if (!success) { publish({}, failureText(error), error.value("error_code").toInt() == 106); complete(); return; }
                if (stage == Capabilities) {
                    if (!wanted || !wanted()) { complete(); return; }
                    const auto caps = value.toObject();
                    if (privateCamera(caps)) { publish({}, QStringLiteral("隐私模式已开启"), true); complete(); return; }
                    const auto target = snapshotTarget(caps);
                    if (!target.isValid()) { publish({}, QStringLiteral("设备不支持 JPEG 快照"), false); complete(); return; }
                    auto args = base;
                    args.insert("codec", "JPEG");
                    // resolution is a target; max_resolution would exclude a
                    // camera that advertises only 1080p/4K capabilities.
                    args.insert("resolution", QJsonObject{{"width", target.width()}, {"height", target.height()}});
                    stage = Snapshot;
                    requestId = this->client->cameraRequest("camera_snapshot", args, 30000, true);
                } else if (stage == Snapshot) {
                    const auto payload = value.toObject();
                    publish(payload, {}, false);
                    const auto stream = payload.value("stream_id");
                    const int streamId = stream.toInt(-1);
                    if (payload.value("provenance").toString() == "allocated" && stream.isDouble()
                        && streamId >= 0 && streamId <= 65535 && stream.toDouble() == streamId
                        && sessionValid && client && client->isConnected() && !client->isDemoMode()) {
                        auto args = base;
                        args.insert("kind", "snapshot"); args.insert("stream_id", streamId);
                        stage = Release;
                        requestId = this->client->cameraRequest("camera_release_stream", args, 15000, true);
                    } else complete();
                }
            });
        connect(source, &MatterClient::connectionChanged, this, [this] {
            if (!this->client || !this->client->isConnected() || this->client->isDemoMode()) {
                sessionValid = false; complete();
            }
        });
    }
    void start() { requestId = client->cameraRequest("camera_get_capabilities", base, 15000, true); }
    bool capturing() const { return !done && !imageSettled; }
private:
    enum Stage { Capabilities, Snapshot, Release } stage = Capabilities;
    void publish(const QJsonObject &payload, const QString &message, bool privacy) {
        imageSettled = true;
        if (result) result(payload, message, privacy);
    }
    void complete() {
        if (done) return;
        done = true;
        if (finished) finished();
        wanted = {}; result = {}; finished = {};
        deleteLater();
    }
    QPointer<MatterClient> client;
    QJsonObject base;
    QString requestId;
    Wanted wanted;
    Result result;
    Finished finished;
    bool sessionValid = true;
    bool done = false;
    bool imageSettled = false;
};
}

struct CameraSnapshots::Impl {
    struct Entry {
        MatterDevice device;
        SnapshotState state;
        qint64 due = 0;
        qint64 lastManual = -ManualInterval;
        bool visible = false;
        quint64 token = 0;
    };
    CameraSnapshots *q;
    QPointer<MatterClient> client;
    QHash<QString, Entry> entries;
    QStringList order;
    QPointer<SnapshotJob> job;
    QString jobKey;
    quint64 jobToken = 0;
    QTimer poll;
    QElapsedTimer clock;
    quint64 generation = 0;
    quint64 nextEntryToken = 0;
    bool active = true;
    bool connected = false;
    bool pumpQueued = false;

    explicit Impl(CameraSnapshots *owner, MatterClient *source) : q(owner), client(source) {
        clock.start();
        poll.setInterval(1000);
        QObject::connect(&poll, &QTimer::timeout, q, [this] { tick(); });
        poll.start();
        if (!client) return;
        connected = client->isConnected() && !client->isDemoMode();
        QObject::connect(client, &MatterClient::connectionChanged, q, [this] { connectionChanged(); });
        QObject::connect(client, &MatterClient::devicesChanged, q, [this] { syncDevices(); });
        QObject::connect(client, &QObject::destroyed, q, [this] {
            client = nullptr; reset(QStringLiteral("控制器已关闭"));
        });
    }
    void notify(const QString &key) { emit q->changed(key); }
    bool wanted(const QString &key, quint64 epoch, quint64 token) const {
        const auto it = entries.constFind(key);
        return epoch == generation && connected && active && client && client->isConnected()
            && !client->isDemoMode() && it != entries.cend() && it->token == token
            && it->visible && it->device.available && it->device.hasCamera;
    }
    void queuePump() {
        if (pumpQueued) return;
        pumpQueued = true;
        QTimer::singleShot(0, q, [this] { pumpQueued = false; pump(); });
    }
    void reset(const QString &message) {
        ++generation;
        connected = false;
        // The client-owned helper remains responsible for same-session cleanup.
        job = nullptr; jobKey.clear(); jobToken = 0;
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            it->state = {}; it->state.message = message; it->due = 0;
            notify(it.key());
        }
    }
    void connectionChanged() {
        const bool now = client && client->isConnected() && !client->isDemoMode();
        if (!now) {
            if (connected || job) reset(QStringLiteral("控制器未连接"));
            return;
        }
        if (!connected) {
            connected = true; ++generation;
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                it->due = 0; it->state.message = QStringLiteral("等待快照"); notify(it.key());
            }
        }
        queuePump();
    }
    void syncDevices() {
        if (!client || !connected) return;
        const auto devices = client->devices();
        QHash<QString, MatterDevice> actual;
        for (const auto &device : devices) if (device.hasCamera) actual.insert(device.key, device);
        const auto keys = entries.keys();
        for (const auto &key : keys) {
            if (!actual.contains(key)) {
                entries.remove(key); order.removeAll(key); notify(key); continue;
            }
            auto &entry = entries[key];
            const bool wasAvailable = entry.device.available;
            entry.device = actual.value(key);
            if (!entry.device.available) {
                entry.state.loading = false; entry.state.stale = !entry.state.image.isNull();
                entry.state.message = QStringLiteral("摄像头离线"); notify(key);
            } else if (!wasAvailable) {
                entry.due = 0; entry.state.message = QStringLiteral("等待快照"); notify(key);
            }
        }
        queuePump();
    }
    void tick() {
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (!it->state.image.isNull() && !it->state.stale
                && it->state.capturedAt.msecsTo(QDateTime::currentDateTimeUtc()) >= RefreshInterval) {
                it->state.stale = true; notify(it.key());
            }
        }
        queuePump();
    }
    void pump() {
        if (job || !connected || !active || !client || !client->isConnected() || client->isDemoMode()) return;
        const auto now = clock.elapsed();
        for (const auto &key : order) {
            auto it = entries.find(key);
            if (it == entries.end() || !it->visible || !it->device.available || !it->device.hasCamera || it->due > now) continue;
            const auto epoch = generation;
            const auto device = it->device;
            const auto token = it->token;
            it->state.loading = true; it->state.message = QStringLiteral("正在更新快照…"); notify(key);
            jobKey = key; jobToken = token;
            QPointer<CameraSnapshots> owner(q);
            auto *created = new SnapshotJob(client, device,
                [owner, key, epoch, token] { return owner && owner->d->wanted(key, epoch, token); },
                [owner, key, epoch, token](const QJsonObject &payload, const QString &message, bool privacy) {
                    if (!owner) return;
                    auto &self = *owner->d;
                    if (epoch != self.generation || !self.entries.contains(key) || self.entries[key].token != token) return;
                    auto &entry = self.entries[key];
                    entry.state.loading = false; entry.due = self.clock.elapsed() + RefreshInterval;
                    if (privacy) {
                        entry.state.image = {}; entry.state.capturedAt = {}; entry.state.stale = false;
                        entry.state.message = QStringLiteral("隐私模式已开启");
                    } else if (!self.wanted(key, epoch, token)) {
                        entry.state.stale = !entry.state.image.isNull();
                        entry.state.message = entry.device.available
                            ? QStringLiteral("预览已暂停") : QStringLiteral("摄像头离线");
                    } else if (!message.isEmpty()) {
                        entry.state.message = message; entry.state.stale = !entry.state.image.isNull();
                    } else {
                        auto image = decodedSnapshot(payload);
                        if (image.isNull()) {
                            entry.state.message = QStringLiteral("快照数据无效，稍后重试"); entry.state.stale = !entry.state.image.isNull();
                        } else {
                            entry.state.image = std::move(image); entry.state.capturedAt = QDateTime::currentDateTimeUtc();
                            entry.state.message.clear(); entry.state.stale = false;
                        }
                    }
                    self.notify(key);
                },
                [owner, key, epoch, token] {
                    if (!owner) return;
                    auto &self = *owner->d;
                    if (epoch != self.generation) return;
                    self.job = nullptr; self.jobKey.clear(); self.jobToken = 0;
                    if (self.entries.contains(key) && self.entries[key].token == token) {
                        auto &entry = self.entries[key];
                        if (entry.state.loading) { entry.state.loading = false; self.notify(key); }
                        entry.due = self.clock.elapsed() + RefreshInterval;
                    }
                    self.queuePump();
                });
            job = created;
            created->start();
            return;
        }
    }
};

CameraSnapshots::CameraSnapshots(MatterClient *client, QObject *parent)
    : QObject(parent), d(std::make_unique<Impl>(this, client)) {}
CameraSnapshots::~CameraSnapshots() = default;

void CameraSnapshots::setVisibleDevices(QList<MatterDevice> devices) {
    QSet<QString> previouslyVisible, incoming;
    for (const auto &device : devices) if (device.hasCamera) incoming.insert(device.key);
    for (auto it = d->entries.cbegin(); it != d->entries.cend(); ++it)
        if (it->visible) previouslyVisible.insert(it.key());
    for (auto it = d->entries.begin(); it != d->entries.end(); ++it) {
        it->visible = false;
        if (!incoming.contains(it.key()) && it->state.loading) {
            it->state.loading = false; it->state.message = QStringLiteral("预览已暂停"); d->notify(it.key());
        }
    }
    QStringList next;
    for (const auto &device : devices) {
        if (!device.hasCamera) continue;
        const bool known = d->entries.contains(device.key);
        auto &entry = d->entries[device.key];
        const bool wasAvailable = entry.device.available;
        if ((!previouslyVisible.contains(device.key)
                && (entry.state.image.isNull() || entry.state.message == QStringLiteral("预览已暂停")))
            || (!wasAvailable && device.available)) entry.due = 0;
        entry.device = device; entry.visible = true;
        if (!known) {
            entry.token = ++d->nextEntryToken;
            entry.due = 0; entry.state.message = QStringLiteral("等待快照");
        }
        if (!device.available) {
            entry.state.message = QStringLiteral("摄像头离线"); entry.state.stale = !entry.state.image.isNull();
        }
        if (d->jobKey == device.key && entry.token == d->jobToken && d->job && d->job->capturing()
            && device.available && d->active && d->connected) {
            entry.state.loading = true; entry.state.message = QStringLiteral("正在更新快照…");
        }
        next.append(device.key); d->notify(device.key);
    }
    d->order = next;
    d->queuePump();
}

void CameraSnapshots::setActive(bool active) {
    if (d->active == active) return;
    d->active = active;
    if (!active) {
        for (auto it = d->entries.begin(); it != d->entries.end(); ++it)
            if (it->state.loading) {
                it->state.loading = false; it->state.message = QStringLiteral("预览已暂停"); d->notify(it.key());
            }
    } else {
        for (auto it = d->entries.begin(); it != d->entries.end(); ++it)
            if (it->state.image.isNull() || it->state.message == QStringLiteral("预览已暂停")) it->due = 0;
    }
    d->queuePump();
}

void CameraSnapshots::refresh(QString key) {
    auto it = d->entries.find(key);
    if (it == d->entries.end() || !it->visible || !d->active || !it->device.available || !d->connected) return;
    const auto now = d->clock.elapsed();
    if (now - it->lastManual < ManualInterval || (d->job && d->jobKey == key)) return;
    it->lastManual = now; it->due = 0;
    d->queuePump();
}

SnapshotState CameraSnapshots::state(QString key) const {
    const auto it = d->entries.constFind(key);
    return it == d->entries.cend() ? SnapshotState{} : it->state;
}
