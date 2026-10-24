#include "matterclient.h"
#include "camerametadata.h"

#include <QAbstractSocket>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QWebSocket>
#include <QtMath>
#include <cmath>

namespace {
constexpr int ClientSchema = 14;
constexpr quint32 CameraPtzCluster = 0xFFF1FC01u;
constexpr int CameraPtzCooldownMs = 1800;
QString ptzPath(int attribute) { return QStringLiteral("1/4294048769/%1").arg(attribute); }
bool isCameraCommand(const QString &command) {
    static const QSet<QString> commands{
        "camera_get_capabilities", "camera_start_stream", "camera_stop_stream",
        "camera_provide_answer", "camera_provide_ice_candidates", "camera_snapshot", "camera_release_stream"
    };
    return commands.contains(command);
}
QString commandLabel(const QString &command) {
    if (isCameraCommand(command)) return QStringLiteral("摄像头操作");
    if (command == "commission_with_code") return QStringLiteral("配对");
    if (command == "device_command" || command == "write_attribute") return QStringLiteral("设备控制");
    if (command == "start_listening") return QStringLiteral("状态订阅");
    if (command == "get_nodes" || command == "get_node") return QStringLiteral("设备刷新");
    return command;
}
}

MatterClient::MatterClient(QObject *parent) : QObject(parent) {
    m_ptzCooldown.setSingleShot(true);
    connect(&m_ptzCooldown, &QTimer::timeout, this, [this] {
        const auto remaining = CameraPtzCooldownMs - m_ptzElapsed.elapsed();
        if (remaining > 0) m_ptzCooldown.start(int(remaining));
        else emit cameraPtzStateChanged();
    });
    m_reconnect.setSingleShot(true);
    m_handshake.setSingleShot(true);
    m_ping.setInterval(30000);
    m_pongDeadline.setSingleShot(true);
    connect(&m_reconnect, &QTimer::timeout, this, &MatterClient::openSocket);
    connect(&m_handshake, &QTimer::timeout, this, [this] {
        if (m_ready || !m_wantConnection) return;
        emit errorOccurred(QStringLiteral("连接或协议握手超时，请检查 Matter Server 地址。"));
        if (m_socket) m_socket->abort();
        scheduleReconnect();
    });
    connect(&m_ping, &QTimer::timeout, this, [this] {
        if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) return;
        m_socket->ping("MatterDesk");
        m_pongDeadline.start(10000);
    });
    connect(&m_pongDeadline, &QTimer::timeout, this, [this] {
        emit logMessage(QStringLiteral("服务器心跳超时，重新连接。"));
        if (m_socket) m_socket->abort();
    });
}

void MatterClient::setStatus(const QString &status, bool ready) {
    const bool changed = m_ready != ready;
    m_status = status;
    m_ready = ready;
    emit connectionChanged();
    if (changed) emit devicesChanged();
}

void MatterClient::destroySocket() {
    ++m_generation;
    ++m_ptzEpoch;
    m_ptzInvalidated.clear();
    m_ptzRevisions.clear();
    m_metadataRevisions.clear();
    m_handshake.stop(); m_ping.stop(); m_pongDeadline.stop();
    if (!m_socket) return;
    auto *old = m_socket;
    m_socket = nullptr;
    old->disconnect(this);
    old->abort();
    old->deleteLater();
}

void MatterClient::connectToServer(QUrl url) {
    if (!url.isValid() || (url.scheme() != "ws" && url.scheme() != "wss") || url.host().isEmpty()
        || !url.userInfo().isEmpty() || url.hasFragment() || url.hasQuery()) {
        emit errorOccurred(QStringLiteral("请输入有效的 ws:// 或 wss:// Matter Server 地址，例如 ws://127.0.0.1:5580/ws。"));
        return;
    }
    m_wantConnection = false;
    m_reconnect.stop();
    destroySocket();
    // Mark the old context unusable before failing requests or exposing the new
    // URL/info. Signal handlers must never see a new controller as already ready.
    setStatus(QStringLiteral("正在连接…"), false);
    failPending(QStringLiteral("连接已切换"));
    m_demo = false;
    m_nodes = {}; m_info = {};
    m_url = url;
    m_retries = 0;
    m_wantConnection = true;
    emit devicesChanged(); emit serverInfoChanged();
    openSocket();
}

void MatterClient::disconnectFromServer() {
    m_wantConnection = false;
    m_reconnect.stop();
    destroySocket();
    failPending(QStringLiteral("连接已断开"));
    m_demo = false;
    setStatus(QStringLiteral("已断开"));
    m_info = {};
    emit serverInfoChanged();
}

void MatterClient::setDemoMode(bool enabled) { enterDemo(enabled, demoNodes()); }
void MatterClient::setHomeDemoMode(bool enabled) { enterDemo(enabled, homeDemoNodes()); }

void MatterClient::enterDemo(bool enabled, const QJsonArray &nodes) {
    disconnectFromServer();
    if (!enabled) {
        m_nodes = {};
        emit devicesChanged();
        return;
    }
    m_demo = true;
    m_nodes = nodes;
    m_info = {{"sdk_version", "MatterDesk Demo · 本地模拟"}, {"schema_version", ClientSchema}};
    setStatus(QStringLiteral("演示模式 · 模拟设备"), true);
    emit serverInfoChanged(); emit devicesChanged();
    emit logMessage(QStringLiteral("已进入演示模式。设备与控制结果均为本地模拟。"));
}

void MatterClient::openSocket() {
    if (!m_wantConnection || m_demo) return;
    destroySocket();
    auto *socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    socket->setMaxAllowedIncomingMessageSize(16 * 1024 * 1024);
    m_socket = socket;
    connect(socket, &QWebSocket::connected, this, [this, socket] {
        if (socket != m_socket) return;
        setStatus(QStringLiteral("正在同步设备…"));
        emit logMessage(QStringLiteral("WebSocket 已连接，等待 Matter Server 握手。"));
    });
    connect(socket, &QWebSocket::textMessageReceived, this, [this, socket](const QString &message) {
        if (socket == m_socket) receive(message);
    });
    connect(socket, &QWebSocket::disconnected, this, [this, socket] {
        if (socket != m_socket) return;
        m_handshake.stop(); m_ping.stop(); m_pongDeadline.stop();
        failPending(QStringLiteral("连接中断；命令未自动重发，请确认设备状态。"));
        setStatus(QStringLiteral("连接已中断"));
        scheduleReconnect();
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const auto errorSignal = &QWebSocket::errorOccurred;
#else
    const auto errorSignal = QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error);
#endif
    connect(socket, errorSignal, this, [this, socket](QAbstractSocket::SocketError) {
        if (socket != m_socket) return;
        const auto detail = sanitized(socket->errorString());
        emit errorOccurred(QStringLiteral("连接失败：%1").arg(detail));
        if (socket->state() == QAbstractSocket::UnconnectedState) scheduleReconnect();
    });
    connect(socket, &QWebSocket::pong, this, [this, socket](quint64, const QByteArray &) {
        if (socket == m_socket) m_pongDeadline.stop();
    });
    setStatus(m_retries ? QStringLiteral("正在重新连接…") : QStringLiteral("正在连接…"));
    m_handshake.start(15000);
    socket->open(m_url);
}

void MatterClient::scheduleReconnect() {
    if (!m_wantConnection || m_demo || m_reconnect.isActive()) return;
    const int seconds = qMin(30, 1 << qMin(m_retries++, 5));
    setStatus(QStringLiteral("%1 秒后重连").arg(seconds));
    m_reconnect.start(seconds * 1000);
}

QList<MatterDevice> MatterClient::devices() const {
    auto result = devicesFromNodes(m_nodes);
    for (auto &device : result) {
        device.controllerGeneration = m_generation;
        if (!m_ready) device.available = false;
        if (m_ptzInvalidated.contains(device.key)) {
            device.cameraPtzKnown = false;
            device.cameraPtzAvailable = false;
        }
    }
    return result;
}

QString MatterClient::sanitized(QString value) const {
    for (const auto &secret : m_secrets) value.replace(secret, QStringLiteral("[配对码已隐藏]"));
    // Error text from a remote server must not accidentally expose onboarding codes.
    value.replace(QRegularExpression(QStringLiteral("MT:[A-Za-z0-9.\\-:+/]+")), QStringLiteral("[配对码已隐藏]"));
    return value;
}

QJsonValue MatterClient::sanitizedCameraValue(const QJsonValue &value, const QString &field) const {
    static const QSet<QString> privateFields{
        "sdp", "candidate", "ice_candidates", "ice_servers", "credential", "password",
        "image", "image_data", "data_base64", "base64"
    };
    if (privateFields.contains(field.toLower())) return QStringLiteral("[媒体或连接数据已隐藏]");
    if (value.isObject()) {
        QJsonObject result;
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            result.insert(it.key(), sanitizedCameraValue(it.value(), it.key()));
        return result;
    }
    if (value.isArray()) {
        QJsonArray result;
        for (const auto &item : value.toArray()) result.append(sanitizedCameraValue(item));
        return result;
    }
    if (!value.isString()) return value;
    const auto text = value.toString();
    // Schema-14 camera errors carry structured JSON encoded in details. Preserve
    // that wire type while redacting its nested private fields.
    const auto parsed = QJsonDocument::fromJson(text.toUtf8());
    if (parsed.isObject()) return QString::fromUtf8(QJsonDocument(sanitizedCameraValue(parsed.object()).toObject()).toJson(QJsonDocument::Compact));
    if (parsed.isArray()) return QString::fromUtf8(QJsonDocument(sanitizedCameraValue(parsed.array()).toArray()).toJson(QJsonDocument::Compact));
    if (text.size() > 2048 || text.contains("candidate:", Qt::CaseInsensitive)
        || text.startsWith("v=0") || text.contains("\nm=") || text.contains("\na="))
        return QStringLiteral("[媒体或连接数据已隐藏]");
    return sanitized(text);
}

QString MatterClient::rejected(const QString &command, const QString &message, bool camera, int errorCode, bool quiet) {
    const auto id = QString::number(++m_sequence);
    QTimer::singleShot(0, this, [this, id, command, message, camera, errorCode, quiet] {
        if (camera) emit cameraResponse(id, command, false, QJsonValue(QJsonValue::Null),
            {{"error_code", errorCode}, {"details", message}});
        if (!quiet) { emit errorOccurred(message); emit commandFinished(id, command, false, message); }
    });
    return id;
}

QString MatterClient::sendRequest(const QString &command, QJsonObject args, int timeoutMs, bool quiet, const Pending *ptzContext) {
    if (!m_demo && (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)) {
        if (ptzContext && ptzContext->metadata) return rejectCameraMetadata(QStringLiteral("连接已失效；未发送请求。"));
        return rejected(command, QStringLiteral("尚未连接 Matter Server。"), isCameraCommand(command), 7, quiet);
    }
    const auto id = QString::number(++m_sequence);
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    Pending pending; pending.command = command; pending.args = args; pending.timer = timer; pending.quiet = quiet;
    if (ptzContext && ptzContext->metadata) {
        pending.metadata = true; pending.metadataKey = ptzContext->metadataKey; pending.metadataGeneration = m_generation;
        pending.metadataRevision = m_metadataRevisions.value(pending.metadataKey);
    } else if (ptzContext) {
        pending.ptzOperation = ptzContext->ptzOperation; pending.ptzKey = ptzContext->ptzKey;
        pending.ptzGeneration = m_generation; pending.ptzEpoch = m_ptzEpoch; pending.ptzRevision = m_ptzRevisions.value(pending.ptzKey);
        m_ptzRequest = id;
        if (pending.ptzOperation == "move") { m_ptzElapsed.restart(); m_ptzCooldown.start(CameraPtzCooldownMs); }
    }
    m_pending.insert(id, pending);
    connect(timer, &QTimer::timeout, this, [this, id] {
        if (!m_pending.contains(id)) return;
        const auto pending = m_pending.take(id);
        pending.timer->deleteLater();
        if (pending.metadata) {
            finishCameraMetadata(id, pending, {{"error_code", 7}, {"details", QStringLiteral("元数据请求超时；未自动重试。")}});
            return;
        }
        if (!pending.ptzOperation.isEmpty()) {
            finishCameraPtz(id, pending, {{"error_code", 7}, {"details", QStringLiteral("云台请求超时；未自动重试。")}});
            return;
        }
        QString message = commandLabel(pending.command) + QStringLiteral("超时；请刷新并确认设备状态。");
        if (isCameraCommand(pending.command))
            emit cameraResponse(id, pending.command, false, QJsonValue(QJsonValue::Null),
                {{"error_code", 7}, {"details", message}, {"reason", "timeout"}});
        if (!pending.quiet) { emit errorOccurred(message); emit commandFinished(id, pending.command, false, message); }
        if (pending.command == "start_listening" && m_socket) m_socket->abort();
    });
    timer->start(timeoutMs);
    if (m_demo) {
        const auto generation = m_generation;
        QTimer::singleShot(180, this, [this, id, command, args, generation] {
            if (generation == m_generation && m_demo && m_pending.contains(id)) simulateCommand(id, command, args);
        });
    } else {
        QJsonObject request{{"message_id", id}, {"command", command}, {"args", args}};
        m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact)));
    }
    if (command == "commission_with_code" || command == "device_command" || command == "write_attribute")
        emit logMessage(commandLabel(command) + QStringLiteral("请求已发送。"));
    return id;
}

void MatterClient::failPending(const QString &reason) {
    const auto pending = m_pending;
    m_pending.clear();
    for (auto it = pending.begin(); it != pending.end(); ++it) {
        it->timer->stop(); it->timer->deleteLater();
        if (it->metadata) {
            finishCameraMetadata(it.key(), *it, {{"error_code", 7}, {"details", reason}});
            continue;
        }
        if (!it->ptzOperation.isEmpty()) {
            finishCameraPtz(it.key(), *it, {{"error_code", 7}, {"details", reason}});
            continue;
        }
        if (isCameraCommand(it->command))
            emit cameraResponse(it.key(), it->command, false, QJsonValue(QJsonValue::Null),
                {{"error_code", 7}, {"details", reason}, {"reason", "connection-closed"}});
        if (!it->quiet) emit commandFinished(it.key(), it->command, false, reason);
    }
}

void MatterClient::receive(const QString &message) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(message.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        emit logMessage(QStringLiteral("忽略无效的服务器 JSON 消息。"));
        return;
    }
    const auto object = document.object();
    if (object.contains("message_id")) {
        const auto id = object.value("message_id").toString();
        if (!m_pending.contains(id)) return;
        const auto pending = m_pending.take(id);
        pending.timer->stop(); pending.timer->deleteLater();
        if (pending.metadata) { finishCameraMetadata(id, pending, object); return; }
        if (!pending.ptzOperation.isEmpty()) { finishCameraPtz(id, pending, object); return; }
        if (object.contains("error_code")) {
            QString detail = sanitized(object.value("details").toString());
            if (detail.isEmpty()) detail = QStringLiteral("服务器错误 %1").arg(object.value("error_code").toInt());
            const bool camera = isCameraCommand(pending.command);
            // Camera data belongs only on the dedicated response channel. Global
            // activity logs must never receive SDP, ICE candidates or image bytes.
            const auto message = camera
                ? QStringLiteral("摄像头操作失败（错误 %1）。").arg(object.value("error_code").toInt())
                : QStringLiteral("%1失败：%2").arg(commandLabel(pending.command), detail);
            if (camera) emit cameraResponse(id, pending.command, false, QJsonValue(QJsonValue::Null),
                sanitizedCameraValue(object).toObject());
            if (!pending.quiet) { emit errorOccurred(message); emit commandFinished(id, pending.command, false, message); }
            if (pending.command == "start_listening") {
                m_wantConnection = false;
                destroySocket();
                setStatus(QStringLiteral("协议订阅失败"));
            }
            return;
        }
        if (!object.contains("result")) {
            if (isCameraCommand(pending.command))
                emit cameraResponse(id, pending.command, false, QJsonValue(QJsonValue::Null),
                    {{"error_code", 7}, {"details", QStringLiteral("服务器响应缺少 result。")}});
            if (!pending.quiet) emit commandFinished(id, pending.command, false, QStringLiteral("服务器响应缺少 result。"));
            return;
        }
        const auto result = object.value("result");
        if (pending.command == "write_attribute") {
            const auto expected = pending.args.value("attribute_path").toString().split('/');
            bool success = result.isArray() && result.toArray().size() == 1 && expected.size() == 3;
            const auto response = result.toArray().isEmpty() ? QJsonObject{} : result.toArray().first().toObject();
            const auto status = response.value("Status");
            const auto p = response.value("Path").toObject();
            success = success && status.isDouble() && status.toDouble() == 0.0
                && p.value("EndpointId").toInt(-1) == expected.value(0).toInt()
                && p.value("ClusterId").toInt(-1) == expected.value(1).toInt()
                && p.value("AttributeId").toInt(-1) == expected.value(2).toInt();
            if (!success) {
                const auto detail = QStringLiteral("设备属性写入失败（Matter 状态 %1）；请刷新并确认状态。").arg(status.toInt(-1));
                if (!pending.quiet) { emit errorOccurred(detail); emit commandFinished(id, pending.command, false, detail); }
                return;
            }
        }
        if (pending.command == "start_listening" || pending.command == "get_nodes") {
            if (!result.isArray()) {
                emit errorOccurred(QStringLiteral("服务器未返回有效设备列表。"));
                emit commandFinished(id, pending.command, false, QStringLiteral("无效设备列表"));
                if (pending.command == "start_listening" && m_socket) m_socket->abort();
                return;
            }
            replaceNodes(result.toArray());
            if (pending.command == "start_listening") {
                m_retries = 0;
                m_handshake.stop();
                m_ping.start();
                setStatus(QStringLiteral("已连接 · 实时同步"), true);
                emit logMessage(QStringLiteral("设备已同步，正在接收实时状态。"));
            }
        } else if ((pending.command == "commission_with_code" || pending.command == "get_node") && result.isObject()) {
            upsertNode(result.toObject());
        }
        if ((pending.command == "device_command" || pending.command == "write_attribute") && !m_demo) {
            // A successful Invoke response is not a state report. Refresh the node,
            // while continuing to accept authoritative subscription updates.
            const auto nodeId = pending.args.value("node_id");
            const auto generation = m_generation;
            QTimer::singleShot(350, this, [this, nodeId, generation] {
                if (generation == m_generation && m_ready && !m_demo) sendRequest("get_node", {{"node_id", nodeId}});
            });
        }
        if (isCameraCommand(pending.command)) emit cameraResponse(id, pending.command, true, result, {});
        const auto detail = m_demo ? QStringLiteral("模拟操作已完成") : QStringLiteral("操作已完成");
        if (!pending.quiet) emit commandFinished(id, pending.command, true, detail);
        if (pending.command == "commission_with_code") emit logMessage(QStringLiteral("Matter 桥接器配对成功。"));
        return;
    }
    if (object.contains("event")) {
        handleEvent(object);
        return;
    }
    if (object.contains("schema_version")) {
        m_info = object;
        emit serverInfoChanged();
        const int schema = object.value("schema_version").toInt();
        const int minimum = object.value("min_supported_schema_version").toInt(0);
        if (schema < 6 || minimum > ClientSchema) {
            m_wantConnection = false;
            destroySocket();
            setStatus(QStringLiteral("服务器协议版本不兼容"));
            emit errorOccurred(QStringLiteral("Matter Server 协议不兼容（schema %1，最低客户端 %2）。").arg(schema).arg(minimum));
            return;
        }
        if (!m_ready) {
            bool listening = false;
            for (const auto &pending : m_pending) listening |= pending.command == "start_listening";
            if (!listening) sendRequest("start_listening");
        }
    }
}

void MatterClient::replaceNodes(const QJsonArray &nodes) {
    QMap<qint64, QJsonObject> before, after;
    for (const auto &entry : m_nodes) before.insert(entry.toObject().value("node_id").toInteger(-1), entry.toObject());
    for (const auto &entry : nodes) after.insert(entry.toObject().value("node_id").toInteger(-1), entry.toObject());
    for (auto it = before.begin(); it != before.end(); ++it) trackCameraPtzRevision(it.value(), after.value(it.key()));
    for (auto it = after.begin(); it != after.end(); ++it)
        if (!before.contains(it.key())) trackCameraPtzRevision({}, it.value());
    m_nodes = nodes;
    emit devicesChanged();
}

void MatterClient::upsertNode(const QJsonObject &node) {
    if (!node.value("node_id").isDouble()) return;
    const auto nodeId = node.value("node_id").toInteger();
    for (qsizetype i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes.at(i).toObject().value("node_id").toInteger(-1) == nodeId) {
            trackCameraPtzRevision(m_nodes.at(i).toObject(), node);
            m_nodes.replace(i, node); emit devicesChanged(); return;
        }
    }
    trackCameraPtzRevision({}, node);
    m_nodes.append(node);
    emit devicesChanged();
}

void MatterClient::updateAttribute(qint64 nodeId, const QString &attributePath, const QJsonValue &value) {
    if (!QRegularExpression(QStringLiteral("^\\d+/\\d+/\\d+$")).match(attributePath).hasMatch()) return;
    for (qsizetype i = 0; i < m_nodes.size(); ++i) {
        auto node = m_nodes.at(i).toObject();
        if (node.value("node_id").toInteger(-1) != nodeId) continue;
        auto attrs = node.value("attributes").toObject();
        attrs.insert(attributePath, value);
        node.insert("attributes", attrs);
        trackCameraPtzRevision(m_nodes.at(i).toObject(), node); m_nodes.replace(i, node);
        emit devicesChanged(); return;
    }
}

void MatterClient::handleEvent(const QJsonObject &message) {
    const auto event = message.value("event").toString();
    const auto data = message.value("data");
    if (event == "webrtc_callback" || event == "camera_session_ended") {
        if (!m_demo && m_ready && data.isObject()) emit cameraEvent(event, data.toObject());
        return;
    }
    if (event == "node_added" || event == "node_updated") {
        upsertNode(data.toObject());
    } else if (event == "node_removed") {
        ++m_ptzEpoch;
        const auto nodeId = data.toInteger(-1);
        for (qsizetype i = m_nodes.size(); i-- > 0;)
            if (m_nodes.at(i).toObject().value("node_id").toInteger(-2) == nodeId) m_nodes.removeAt(i);
        emit devicesChanged();
    } else if (event == "attribute_updated") {
        const auto parts = data.toArray();
        if (parts.size() == 3 && parts.at(0).isDouble() && parts.at(1).isString())
            updateAttribute(parts.at(0).toInteger(), parts.at(1).toString(), parts.at(2));
    } else if (event == "endpoint_removed") {
        ++m_ptzEpoch;
        const auto object = data.toObject();
        const auto nodeId = object.value("node_id").toInteger(-1);
        const auto endpoint = object.value("endpoint_id").toInt(-1);
        if (endpoint == 1) ++m_metadataRevisions[QStringLiteral("%1/1").arg(nodeId)];
        for (qsizetype i = 0; i < m_nodes.size(); ++i) {
            auto node = m_nodes.at(i).toObject();
            if (node.value("node_id").toInteger(-2) != nodeId) continue;
            auto attrs = node.value("attributes").toObject();
            const auto prefix = QString::number(endpoint) + '/';
            const auto keys = attrs.keys();
            for (const auto &key : keys) if (key.startsWith(prefix)) attrs.remove(key);
            node.insert("attributes", attrs); m_nodes.replace(i, node);
        }
        emit devicesChanged();
    } else if (event == "endpoint_added") {
        if (m_ready) sendRequest("get_node", {{"node_id", data.toObject().value("node_id")}});
    } else if (event == "server_info_updated") {
        const auto updated = data.toObject();
        const auto knownIdentity = [](const QJsonValue &value) {
            return (value.isString() && !value.toString().trimmed().isEmpty())
                || (value.isDouble() && std::isfinite(value.toDouble()));
        };
        static const QStringList identityKeys{"fabric_id", "compressed_fabric_id"};
        bool changedFabric = false;
        for (const auto &key : identityKeys) {
            const auto previous = m_info.value(key), next = updated.value(key);
            if (knownIdentity(previous) && knownIdentity(next) && previous != next) changedFabric = true;
        }
        if (m_ready && !m_demo && changedFabric) {
            // A server can replace its Fabric without dropping this WebSocket.
            // Cached endpoint IDs and pending media sessions then belong to the
            // old Fabric. Reconnect before exposing any part of the new context.
            emit logMessage(QStringLiteral("控制器 Fabric 已改变，重新连接并同步设备。"));
            connectToServer(m_url);
            return;
        }
        for (auto it = updated.begin(); it != updated.end(); ++it) {
            // Events may contain only changed fields. Missing/null identifiers
            // are not evidence of a new Fabric and must not erase known identity.
            if (identityKeys.contains(it.key()) && !knownIdentity(it.value())) continue;
            m_info.insert(it.key(), it.value());
        }
        emit serverInfoChanged();
    } else if (event == "server_shutdown") {
        emit logMessage(QStringLiteral("Matter Server 正在关闭。"));
        if (m_socket) m_socket->abort();
    }
}

QString MatterClient::commission(QString code) {
    code = code.trimmed();
    if (m_demo) return rejected("commission_with_code", QStringLiteral("请先连接真实 Matter Server，再进行配对。"));
    if (!m_ready) return rejected("commission_with_code", QStringLiteral("请先连接 Matter Server。"));
    if (!code.startsWith("MT:", Qt::CaseInsensitive)) {
        code.remove(QRegularExpression(QStringLiteral("[\\s-]")));
        if (!QRegularExpression(QStringLiteral("^(?:[0-9]{11}|[0-9]{21})$")).match(code).hasMatch())
            return rejected("commission_with_code", QStringLiteral("请输入 11 位或 21 位 Matter 手动配对码，或 MT: 开头的二维码文本。"));
    } else if (code.size() < 6 || code.size() > 1024) {
        return rejected("commission_with_code", QStringLiteral("Matter 二维码文本长度不正确。"));
    }
    if (code.startsWith("MT:", Qt::CaseInsensitive)) code.replace(0, 3, QStringLiteral("MT:"));
    m_secrets.append(code);
    return sendRequest("commission_with_code", {{"code", code}, {"network_only", true}}, 300000);
}

MatterDevice MatterClient::currentCameraPtz(const MatterDevice &device) const {
    if (device.controllerGeneration != m_generation) return {};
    for (const auto &current : devices())
        if (current.key == device.key && current.nodeId == device.nodeId && current.endpointId == device.endpointId
            && current.hasCamera && current.hasCameraPtz && current.endpointId == 1) return current;
    return {};
}

bool MatterClient::cameraPtzBusy() const {
    return !m_ptzRequest.isEmpty() || (m_ptzElapsed.isValid() && m_ptzElapsed.elapsed() < CameraPtzCooldownMs);
}

bool MatterClient::canMoveCameraStep(const MatterDevice &device, int direction) const {
    if (direction < 0 || direction > 3 || !m_ready || m_demo || cameraPtzBusy()) return false;
    const auto current = currentCameraPtz(device);
    return current.available && current.cameraPtzKnown && current.cameraPtzSupported
        && current.cameraPtzAvailable && (current.cameraPtzDirections & (1u << direction));
}

QString MatterClient::rejectCameraPtz(const QString &operation, const QString &message) {
    const auto id = QString::number(++m_sequence);
    QTimer::singleShot(0, this, [this, id, operation, message] {
        emit cameraPtzResponse(id, operation, false, message);
    });
    return id;
}

QString MatterClient::readCameraPtz(const MatterDevice &device) {
    const auto current = currentCameraPtz(device);
    if (!m_ready || m_demo || !current.available || !m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return rejectCameraPtz("read", QStringLiteral("摄像头离线、处于演示模式或未提供云台能力。"));
    if (!m_ptzRequest.isEmpty()) return rejectCameraPtz("read", QStringLiteral("云台请求尚未完成；未排队。"));
    QJsonArray paths;
    for (int attribute : {0, 1, 2, 3, 65529}) paths.append(ptzPath(attribute));
    Pending context; context.ptzOperation = "read"; context.ptzKey = current.key;
    const auto id = sendRequest("read_attribute", {{"node_id", current.nodeId}, {"attribute_path", paths}}, 5000, true, &context);
    emit cameraPtzStateChanged();
    return id;
}

QString MatterClient::moveCameraStep(const MatterDevice &device, int direction) {
    if (!canMoveCameraStep(device, direction) || !m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return rejectCameraPtz("move", QStringLiteral("方向不可用、云台正忙或仍在冷却；未发送或排队。"));
    const auto current = currentCameraPtz(device);
    const QJsonObject args{{"node_id", current.nodeId}, {"endpoint_id", 1},
        {"cluster_id", qint64(CameraPtzCluster)}, {"command_name", "MoveStep"},
        {"payload", QJsonObject{{"direction", direction}}}};
    Pending context; context.ptzOperation = "move"; context.ptzKey = current.key;
    const auto id = sendRequest("device_command", args, 5000, true, &context);
    emit cameraPtzStateChanged();
    return id;
}

void MatterClient::trackCameraPtzRevision(const QJsonObject &before, const QJsonObject &after) {
    const auto metadataFingerprint = [](const QJsonObject &node) {
        const auto attrs = node.value("attributes").toObject();
        QJsonObject result{{"available", node.value("available")}};
        for (const auto &p : {QStringLiteral("1/29/0"), QStringLiteral("1/29/1")}) result.insert(p, attrs.value(p));
        for (int a = 0; a <= 6; ++a) {
            const auto p = QStringLiteral("1/4294048770/%1").arg(a); result.insert(p, attrs.value(p));
        }
        return result;
    };
    if (metadataFingerprint(before) != metadataFingerprint(after)) {
        const auto id = after.contains("node_id") ? after.value("node_id") : before.value("node_id");
        if (id.isDouble()) ++m_metadataRevisions[QStringLiteral("%1/1").arg(id.toInteger(-1))];
    }
    const auto fingerprint = [](const QJsonObject &node) {
        const auto attributes = node.value("attributes").toObject();
        QJsonObject result{{"available", node.value("available")}};
        static const QStringList paths{"1/29/0", "1/29/1", "1/57/17", "1/4294048769/0", "1/4294048769/1",
            "1/4294048769/2", "1/4294048769/3", "1/4294048769/65529"};
        for (const auto &path : paths) result.insert(path, attributes.value(path));
        return result;
    };
    if (fingerprint(before) == fingerprint(after)) return;
    const auto id = after.contains("node_id") ? after.value("node_id") : before.value("node_id");
    if (id.isDouble()) ++m_ptzRevisions[QStringLiteral("%1/1").arg(id.toInteger(-1))];
}

void MatterClient::invalidateCameraPtz(const QString &key) {
    m_ptzInvalidated.insert(key);
    emit devicesChanged();
}

void MatterClient::finishCameraPtz(const QString &id, const Pending &pending, const QJsonObject &response) {
    if (m_ptzRequest == id) m_ptzRequest.clear();
    MatterDevice identity;
    identity.key = pending.ptzKey;
    identity.nodeId = pending.args.value("node_id").toInteger(-1);
    identity.endpointId = 1;
    identity.controllerGeneration = pending.ptzGeneration;
    const auto current = currentCameraPtz(identity);
    const bool contextValid = m_ready && !m_demo && pending.ptzGeneration == m_generation
        && pending.ptzEpoch == m_ptzEpoch && current.available;
    bool success = contextValid && !response.contains("error_code") && response.contains("result");
    QString detail = QStringLiteral("云台上下文已失效；未自动重试。 ");
    if (contextValid) {
        detail = response.value("details").toString();
        if (detail.isEmpty()) detail = QStringLiteral("云台响应无效；请重新读取能力。 ");
    }
    if (success && pending.ptzOperation == "read") {
        const auto result = response.value("result");
        const auto attrs = result.toObject();
        const auto supported = attrs.value(ptzPath(0)), available = attrs.value(ptzPath(1));
        const auto mode = attrs.value(ptzPath(2)), directions = attrs.value(ptzPath(3));
        success = result.isObject() && supported.isBool() && available.isBool()
            && mode.isDouble() && mode.toDouble() == 0
            && directions.isDouble() && directions.toDouble() >= 0 && directions.toDouble() <= 15
            && std::floor(directions.toDouble()) == directions.toDouble()
            && attrs.value(ptzPath(65529)).isArray() && attrs.value(ptzPath(65529)).toArray().contains(QJsonValue(0))
            && pending.ptzRevision == m_ptzRevisions.value(pending.ptzKey);
        if (success) {
            for (qsizetype i = 0; i < m_nodes.size(); ++i) {
                auto node = m_nodes.at(i).toObject();
                if (node.value("node_id").toInteger(-1) != current.nodeId) continue;
                auto attributes = node.value("attributes").toObject();
                for (int a : {0, 1, 2, 3, 65529}) attributes.insert(ptzPath(a), attrs.value(ptzPath(a)));
                node.insert("attributes", attributes); m_nodes.replace(i, node);
                break;
            }
            ++m_ptzRevisions[current.key];
            m_ptzInvalidated.remove(current.key);
            emit devicesChanged();
            detail = !supported.toBool() ? QStringLiteral("此摄像头暂不支持方向步进。")
                : !available.toBool() ? QStringLiteral("摄像头云台当前不可用。")
                : directions.toInt() == 0 ? QStringLiteral("摄像头未提供可用方向。")
                : QStringLiteral("已读取云台能力。 ");
        }
    } else if (success && pending.ptzOperation == "move") {
        // This status-only command has no camera-position report. null confirms
        // acceptance of the request only, never physical movement or completion.
        success = response.value("result").isNull();
        if (success) detail = QStringLiteral("方向请求已接受；不代表镜头已完成移动。 ");
    }
    if (!success && contextValid) invalidateCameraPtz(current.key);
    emit cameraPtzResponse(id, pending.ptzOperation, success, sanitized(detail).trimmed());
    emit cameraPtzStateChanged();
}

CameraMetadataManager *MatterClient::cameraMetadata() {
    if (!m_metadata) m_metadata = new CameraMetadataManager(this, this);
    return m_metadata;
}

MatterDevice MatterClient::currentCameraMetadata(const MatterDevice &device) const {
    if (device.controllerGeneration != m_generation || !m_ready || m_demo) return {};
    for (const auto &current : devices())
        if (current.key == device.key && current.nodeId == device.nodeId && current.endpointId == 1
            && current.available && hasCameraMetadata(current)) return current;
    return {};
}

QString MatterClient::rejectCameraMetadata(const QString &detail) {
    const auto id = QString::number(++m_sequence);
    QTimer::singleShot(0, this, [this, id, detail] {
        emit cameraMetadataResponse(id, false, QJsonValue(QJsonValue::Null), {{"error_code", 8}, {"details", detail}});
    });
    return id;
}

QString MatterClient::readCameraMetadataAttributes(const MatterDevice &device, const QList<int> &attributes) {
    const auto current = currentCameraMetadata(device);
    if (current.key.isEmpty() || attributes.isEmpty() || attributes.size() > 7)
        return rejectCameraMetadata(QStringLiteral("元数据读取上下文不可用。"));
    QJsonArray paths; QSet<int> seen;
    for (const int attribute : attributes) {
        if (attribute < 0 || attribute > 6 || seen.contains(attribute))
            return rejectCameraMetadata(QStringLiteral("无效元数据读取范围。"));
        seen.insert(attribute); paths.append(QStringLiteral("1/4294048770/%1").arg(attribute));
    }
    Pending context; context.metadata = true; context.metadataKey = current.key;
    return sendRequest("read_attribute", {{"node_id", current.nodeId}, {"attribute_path", paths}}, 5000, true, &context);
}

QString MatterClient::invokeCameraMetadata(const MatterDevice &device, const QString &operation, const QJsonObject &payload) {
    for (const auto &request : m_pending) if (request.metadata && request.metadataGeneration == m_generation
        && request.metadataRevision == m_metadataRevisions.value(request.metadataKey))
        return rejectCameraMetadata(QStringLiteral("元数据请求尚未完成；未排队。"));
    const auto current = currentCameraMetadata(device);
    CameraMetadataSnapshot snapshot; CameraMetadataCapabilities caps;
    const auto attrs = current.attributes;
    const auto fail = [this] { return rejectCameraMetadata(QStringLiteral("元数据能力、守卫或目标已失效；未发送或排队。")); };
    if (current.key.isEmpty() || !parseCameraMetadataSnapshot(attrs.value("1/4294048770/0"), &snapshot)
        || !parseCameraMetadataCapabilities(attrs.value("1/4294048770/2"), &caps)
        || !attrs.value("1/4294048770/4").isBool() || (operation != "refresh" && !attrs.value("1/4294048770/4").toBool())
        || !attrs.value("1/4294048770/5").isBool() || attrs.value("1/4294048770/5").toBool()) return fail();
    if (payload.value("expectedIdentity") != snapshot.identity || payload.value("expectedSourceEpoch") != snapshot.sourceEpoch
        || !payload.value("expectedRevision").isDouble() || payload.value("expectedRevision").toDouble() != double(snapshot.revision)) return fail();
    QString command, field;
    if (operation == "refresh" && caps.read) command = "RefreshMetadata";
    else if (operation == "sync" && caps.syncConfig && payload.value("enabled").isBool()) { command = "SetSyncEnabled"; field = "enabled"; }
    else if (operation == "rename" && caps.rename && payload.value("name").isString()) {
        const auto name = payload.value("name").toString();
        if (name.trimmed().isEmpty() || name.toUtf8().size() > 160) return fail();
        for (qsizetype i = 0; i < name.size(); ++i) {
            const auto ch = name.at(i);
            if (ch.isHighSurrogate()) {
                if (i + 1 >= name.size() || !name.at(i + 1).isLowSurrogate()) return fail();
                ++i;
            } else if (ch.isLowSurrogate() || ch.category() == QChar::Other_Control) return fail();
        }
        command = "RenameDevice"; field = "name";
    } else if (operation == "move" && caps.move && payload.value("roomId").isString()) {
        QList<CameraMetadataRoom> rooms;
        if (!parseCameraMetadataRooms(attrs.value("1/4294048770/1"), &rooms)) return fail();
        const auto roomId = payload.value("roomId").toString();
        bool found = false; for (const auto &room : rooms) found |= !roomId.isEmpty() && room.id == roomId;
        if (!found) return fail();
        command = "MoveDevice"; field = "roomId";
    } else return fail();
    if (payload.size() != (field.isEmpty() ? 3 : 4)) return fail();
    Pending context; context.metadata = true; context.metadataKey = current.key;
    return sendRequest("device_command", {{"node_id", current.nodeId}, {"endpoint_id", 1},
        {"cluster_id", qint64(0xFFF1FC02u)}, {"command_name", command}, {"payload", payload}}, 5000, true, &context);
}

void MatterClient::finishCameraMetadata(const QString &id, const Pending &pending, const QJsonObject &response) {
    MatterDevice reference; reference.key = pending.metadataKey; reference.nodeId = pending.args.value("node_id").toInteger(-1);
    reference.endpointId = 1; reference.controllerGeneration = pending.metadataGeneration;
    const auto current = currentCameraMetadata(reference);
    if (current.key.isEmpty() || pending.metadataGeneration != m_generation) {
        emit cameraMetadataResponse(id, false, QJsonValue(QJsonValue::Null), {{"error_code", 7}, {"details", QStringLiteral("元数据上下文已失效。")}});
        return;
    }
    if (response.contains("error_code") || !response.contains("result")) {
        auto error = response; error.remove("message_id"); error.remove("result");
        // Surface bounded status only. Untrusted remote error strings are never
        // copied into persistent metadata or the global application activity log.
        error.insert("details", QStringLiteral("元数据请求失败（错误 %1）；未自动重试。").arg(response.value("error_code").toInt(-1)));
        emit cameraMetadataResponse(id, false, QJsonValue(QJsonValue::Null), error); return;
    }
    const auto result = response.value("result");
    if (pending.command == "read_attribute" && pending.metadataRevision != m_metadataRevisions.value(pending.metadataKey)) {
        emit cameraMetadataResponse(id, false, QJsonValue(QJsonValue::Null), {{"error_code", 7}, {"details", QStringLiteral("读取期间元数据已改变；忽略旧响应。")}});
        return;
    }
    if (pending.command == "read_attribute" && result.isObject()) {
        const auto values = result.toObject();
        for (qsizetype i = 0; i < m_nodes.size(); ++i) {
            auto node = m_nodes.at(i).toObject();
            if (node.value("node_id").toInteger(-1) != current.nodeId) continue;
            auto attrs = node.value("attributes").toObject();
            for (const auto &path : pending.args.value("attribute_path").toArray())
                if (values.contains(path.toString())) attrs.insert(path.toString(), values.value(path.toString()));
            node.insert("attributes", attrs); m_nodes.replace(i, node); break;
        }
    }
    emit cameraMetadataResponse(id, true, result, {});
}

QString MatterClient::sendDeviceCommand(const MatterDevice &device, quint32 cluster, const QString &name, QJsonObject payload) {
    QJsonObject args{{"node_id", device.nodeId}, {"endpoint_id", device.endpointId},
        {"cluster_id", qint64(cluster)}, {"command_name", name}, {"payload", payload}};
    // Door Lock's mandatory Lock/Unlock commands require a Timed Interaction.
    // PINs are deliberately absent: a PIN-required lock reports its server error.
    if (cluster == 257) args.insert("timed_request_timeout_ms", 10000);
    return sendRequest("device_command", args);
}

QString MatterClient::control(const MatterDevice &device, const QString &action, const QJsonValue &value) {
    const auto reject = [this] { return rejected("device_command", QStringLiteral("控制值无效或超出设备当前支持范围。")); };
    if (!m_ready || !device.available) return rejected("device_command", QStringLiteral("设备离线或尚未连接。"));
    MatterDevice current;
    bool found = false;
    for (const auto &candidate : devices()) {
        if (candidate.key == device.key && candidate.nodeId == device.nodeId && candidate.endpointId == device.endpointId && candidate.available) {
            current = candidate; found = true; break;
        }
    }
    if (!found || !current.controls.contains(action)) return rejected("device_command", QStringLiteral("设备已移除或不支持此控制。"));
    auto numeric = [&](const QString &a, bool integral) {
        const auto limits = current.controlLimits.value(a).toObject();
        return value.isDouble() && std::isfinite(value.toDouble()) && limits.value("min").isDouble() && limits.value("max").isDouble()
            && value.toDouble() >= limits.value("min").toDouble() && value.toDouble() <= limits.value("max").toDouble()
            && (!integral || std::floor(value.toDouble()) == value.toDouble());
    };
    auto write = [&](int cluster, int attribute, QJsonValue target) {
        return sendRequest("write_attribute", {{"node_id", current.nodeId},
            {"attribute_path", QStringLiteral("%1/%2/%3").arg(current.endpointId).arg(cluster).arg(attribute)}, {"value", target}});
    };
    if (action == "power") {
        if (!value.isBool()) return reject();
        return sendDeviceCommand(current, 6, value.toBool() ? "On" : "Off", {});
    }
    if (action == "brightness") {
        if (!numeric(action, true)) return reject();
        return sendDeviceCommand(current, 8, "MoveToLevelWithOnOff", {{"level", qRound(value.toDouble()*254.0/100.0)},
            {"transitionTime", 5}, {"optionsMask", 0}, {"optionsOverride", 0}});
    }
    if (action == "colorTemperature") {
        if (!numeric(action, true)) return reject();
        return sendDeviceCommand(current, 768, "MoveToColorTemperature", {{"colorTemperatureMireds", value},
            {"transitionTime", 5}, {"optionsMask", 0}, {"optionsOverride", 0}});
    }
    if (action == "color") {
        const auto color = value.toObject();
        const auto hue = color.value("hue"), saturation = color.value("saturation");
        if (!value.isObject() || color.size() != 2 || !hue.isDouble() || !saturation.isDouble()
            || !std::isfinite(hue.toDouble()) || !std::isfinite(saturation.toDouble())
            || hue.toDouble() < 0 || hue.toDouble() > 360 || saturation.toDouble() < 0 || saturation.toDouble() > 100) return reject();
        const auto prefix = QString::number(current.endpointId) + "/768/";
        const auto f = current.attributes.value(prefix + "65532");
        const int features = f.isDouble() ? f.toInt() : current.attributes.value(prefix+"16394").toInt();
        const auto accepted = current.attributes.value(prefix+"65529");
        const bool enhanced = (features & 2) && (accepted.isUndefined() || accepted.toArray().contains(67));
        QJsonObject payload{{enhanced ? "enhancedHue" : "hue", enhanced ? qRound(std::fmod(hue.toDouble(),360.0)*65536.0/360.0)%65536
            : qRound(std::fmod(hue.toDouble(),360.0)*254.0/360.0)},
            {"saturation", qRound(saturation.toDouble()*254.0/100.0)}, {"transitionTime", 5}, {"optionsMask", 0}, {"optionsOverride", 0}};
        return sendDeviceCommand(current, 768, enhanced ? "EnhancedMoveToHueAndSaturation" : "MoveToHueAndSaturation", payload);
    }
    if (action == "lock") {
        if (!value.isBool()) return reject();
        return sendDeviceCommand(current, 257, value.toBool() ? "LockDoor" : "UnlockDoor", {});
    }
    if (action == "position") {
        if (!numeric(action, false)) return reject();
        return sendDeviceCommand(current, 258, "GoToLiftPercentage", {{"liftPercent100thsValue", qRound((100.0-value.toDouble())*100.0)}});
    }
    if (action == "thermostatMode" || action == "fanMode") {
        if (!value.isString() || !current.controlLimits.value(action).toArray().contains(value)) return reject();
        static const QMap<QString,int> thermostat{{"off",0},{"auto",1},{"cool",3},{"heat",4}};
        static const QMap<QString,int> fan{{"off",0},{"low",1},{"medium",2},{"high",3},{"auto",5}};
        return action == "fanMode" ? write(514, 0, fan.value(value.toString())) : write(513, 28, thermostat.value(value.toString()));
    }
    if (action == "heatSetpoint" || action == "coolSetpoint") {
        if (!numeric(action, false)) return reject();
        // Do not let the device silently move the other setpoint to repair a
        // violated deadband. An unknown companion setpoint is not assumed.
        const auto band = current.controlLimits.value("setpointDeadband");
        const bool heat = action == "heatSetpoint";
        const auto other = current.controlState.value(heat ? "coolSetpoint" : "heatSetpoint");
        if (band.isDouble() && (!other.isDouble() || (heat ? value.toDouble()+band.toDouble() > other.toDouble()+0.00001
            : value.toDouble()-band.toDouble() < other.toDouble()-0.00001))) return reject();
        return write(513, heat ? 18 : 17, qRound(value.toDouble()*100));
    }
    if (action == "fanPercent") {
        if (!numeric(action, true)) return reject();
        return write(514, 2, value);
    }
    return reject();
}

QString MatterClient::setOnOff(const MatterDevice &device, bool on) { return control(device, "power", on); }
QString MatterClient::setLevel(const MatterDevice &device, int percent) { return control(device, "brightness", percent); }
QString MatterClient::setColorTemperature(const MatterDevice &device, int mired) { return control(device, "colorTemperature", mired); }

QString MatterClient::cameraRequest(QString command, QJsonObject args, int timeoutMs, bool quiet) {
    if (!isCameraCommand(command))
        return rejected(command, QStringLiteral("不支持的摄像头命令。"), true, 9, quiet);
    if (m_demo || !m_ready)
        return rejected(command, QStringLiteral("请先连接真实 Matter Server，再使用摄像头。"), true, 7, quiet);
    if (m_info.value("schema_version").toInt() < 14)
        return rejected(command, QStringLiteral("此摄像头接口需要 schema 14 或更新的 Matter Server。"), true, 6, quiet);
    if (timeoutMs <= 0 || timeoutMs > 300000)
        return rejected(command, QStringLiteral("摄像头请求超时须在 1 到 300000 毫秒之间。"), true, 8, quiet);
    // Argument validation and allocation policy stay with the pinned server. In
    // particular, stop/ICE may use a client_session_key while start is pending.
    return sendRequest(command, args, timeoutMs, quiet);
}

void MatterClient::refresh() {
    if (!m_ready) {
        emit errorOccurred(QStringLiteral("请先连接 Matter Server。")); return;
    }
    sendRequest("get_nodes");
}

void MatterClient::simulateCommand(const QString &id, const QString &command, const QJsonObject &args) {
    QJsonValue result(QJsonValue::Null);
    const auto node = args.value("node_id").toInteger();
    const int endpoint = args.value("endpoint_id").toInt();
    auto update = [&](int cluster, int attribute, QJsonValue value) {
        updateAttribute(node, QStringLiteral("%1/%2/%3").arg(endpoint).arg(cluster).arg(attribute), value);
    };
    if (command == "device_command") {
        const int cluster = args.value("cluster_id").toInt();
        const auto payload = args.value("payload").toObject();
        const auto name = args.value("command_name").toString();
        if (cluster == 6) update(6, 0, name == "On");
        if (cluster == 8) { update(8, 0, payload.value("level")); update(6, 0, payload.value("level").toInt() > 0); }
        if (cluster == 768 && name == "MoveToColorTemperature") update(768, 7, payload.value("colorTemperatureMireds"));
        if (cluster == 768 && (name == "MoveToHueAndSaturation" || name == "EnhancedMoveToHueAndSaturation")) {
            const bool enhanced = name.startsWith("Enhanced");
            const double hue = enhanced ? payload.value("enhancedHue").toDouble()/65536.0 : payload.value("hue").toDouble()/254.0;
            update(768, 0, qRound(hue*254.0)); update(768, 16384, qRound(hue*65536.0)%65536);
            update(768, 1, payload.value("saturation")); update(768, 8, 0);
        }
        if (cluster == 257) update(257, 0, name == "LockDoor" ? 1 : 2);
        if (cluster == 258) update(258, 14, payload.value("liftPercent100thsValue"));
    } else if (command == "write_attribute") {
        const auto p = args.value("attribute_path").toString();
        const auto parts = p.split('/');
        updateAttribute(node, p, args.value("value"));
        const int ep = parts.value(0).toInt(), cluster = parts.value(1).toInt(), attribute = parts.value(2).toInt();
        if (cluster == 514 && attribute == 2) updateAttribute(node, QStringLiteral("%1/514/3").arg(ep), args.value("value"));
        result = QJsonArray{QJsonObject{{"Path", QJsonObject{{"EndpointId",ep},{"ClusterId",cluster},{"AttributeId",attribute}}},{"Status",0}}};
    } else if (command == "get_nodes") result = m_nodes;
    QJsonObject response{{"message_id", id}, {"result", result}};
    receive(QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact)));
}
