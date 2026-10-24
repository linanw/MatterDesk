# 涂鸦名称与房间 / Tuya metadata sync preview

This is an unreleased integration for **both MatterDesk and MatterCamViewer**. It connects directly to the optional Metadata Sync v1 HTTPS service in [@linanw/homebridge-tuya-platform](https://github.com/linanw/tuya-homebridge/pull/1). Native Matter pairing and device controls continue through the existing OHF controller. No new vendor cluster or controller restart is required.

这是同时适用于 **MatterDesk 和 MatterCamViewer** 的未发布接入。名称和房间通过涂鸦插件的独立 Metadata Sync v1 服务获取；Matter 配对、控制和摄像头播放继续使用现有 OHF 控制器，不增加厂商集群，也不需要重建 Fabric。

## 开始使用 / Connect

1. Build the feature versions of both repositories. The published Tuya npm **2.6.2**, MatterDesk **0.6.0** and MatterCamViewer **2.4.0** release artifacts do not include this preview. Enable the Homebridge metadata service using its [setup instructions](https://github.com/linanw/tuya-homebridge/blob/feature/metadata-sync-companion/docs/metadata-sync/SETUP.md).
2. Connect the application to the existing OHF controller. In MatterDesk open **Settings → 涂鸦同步**. In MatterCamViewer use the separate **涂** button; its settings remain available even when no camera is selected.
3. Enter the metadata service URL and its independent bearer token. HTTPS is required for another computer; only exact loopback addresses permit development HTTP. The normal certificate validation remains enabled and redirects are rejected. Tokens are held only in this application session, so reconnect after restarting the application.
4. Select the explicit source home, or the Custom-project device-name-only profile. Select a Matter endpoint and the corresponding Tuya source device, then confirm the binding. A unique `BridgedDeviceBasicInformation.uniqueId` match can suggest a source; duplicate or missing identities never cause name-based guessing.
5. Enable the independent Tuya display mirror and explicitly adopt the source value to establish a baseline. Choose following mode for automatic source updates, or bidirectional management for supported source writes. Editing the separate sync-name draft and confirming it sends one conditional command; it does not edit the ordinary local alias.

先构建两个仓库的功能分支；现有正式版尚未包含本预览。启动涂鸦服务后，在主应用设置的「涂鸦同步」或摄像头应用的独立「涂」入口中连接。选择家庭并逐一确认设备绑定，再启用独立镜像、采用来源建立基线。令牌只保存在本次会话，重启 App 后需重新输入。不要将 npm token、涂鸦 Access Secret 或 Apple 密码当作此服务令牌。

## 实际能力 / Capabilities

| Provider / 来源 | Read / 读取 | Supported writes / 写入 |
| --- | --- | --- |
| Smart Home PaaS | Device names, app homes and rooms, subject to API permissions / 设备名及 App 家庭、房间，取决于权限 | None until verified write APIs are implemented / 当前禁用 |
| Custom project | Device names; assets are not app rooms / 设备名，assets 不等于 App 房间 | Device rename only when the server administrator enables it / 管理员单独开启后的设备改名 |

Full bidirectional Tuya room writes remain unavailable. The UI disables unsupported actions; it never guesses a replacement request, creates/deletes rooms, or treats unknown room data as unassigned. The server polls Tuya once for all clients; the Qt application reads the server cache periodically rather than opening another Tuya cloud session.

完整涂鸦房间双向写入尚不支持。接口未核实或权限不足时明确禁用操作，不猜测整屋替换请求，不创建、删除房间。房间读取失败会保留旧资料并标为未知，不把所有设备移到默认房间。Qt 定期读取服务缓存，多客户端共用插件的一次云端轮询。

## 数据与冲突 / Data and conflicts

- The dedicated `tuya-metadata/` mirror is scoped to the selected controller/Fabric and source identity. Atomic files use owner-only permissions. Tokens are not written to this store.
- Existing HomeStore aliases and rooms, Xiaomi camera metadata, pairings and video sessions remain independent. Existing local aliases and Xiaomi source displays take precedence over a Tuya fallback. Saving an ordinary local alias never submits a cloud write.
- The full source name is preserved. A short Matter nodeLabel is not interpreted as a reverse rename; only explicit source bindings and the independent sync editor can request writes.
- No binding/baseline means no automatic change. Source changes can update the independent mirror when following is enabled. Divergent or stale changes preserve drafts for review.
- A command is persisted before POST, submitted once, and confirmed only after authoritative readback matches its requested value. Unknown outcomes stay locked for review after a fresh read; they are never replayed automatically on a timeout, reconnect or restart.
- The applications need to remain running and connected. Another Matter hub requires its own authorized metadata adapter to share its private home graph; ordinary Matter pairing does not grant that access.

同步镜像单独保存，不更改本地家庭数据格式、米家资料和配对。普通本机别名及房间编辑永不自动写回云端。用户需要明确绑定及基线；来源变化可跟随，冲突保留草稿，旧版本不能覆盖新值。命令先落盘、只发送一次；排队不代表成功，读取确认后才更新基线。未知结果需重新读取并人工核实，不能自动重发。

MatterCamViewer only displays actual camera endpoints already exposed by the controller. This metadata integration does **not** add a Tuya camera driver or video support to the Homebridge plugin, and cannot turn a light or switch into a camera.

MatterCamViewer 仍仅显示控制器真实提供的摄像头端点；本功能不会为涂鸦插件添加摄像头驱动、直播能力，也不会把灯或开关当作摄像头。

## 验证 / Validation

The shared Qt tests exercise a local HTTP metadata service and a synthetic OHF WebSocket endpoint, including guarded writes, identities, capabilities, stale data, independent local settings and camera-session preservation. CI builds **both** application targets and runs the full offline suite. This does not prove permissions for a real Tuya account, Apple Home authorization, physical camera behavior or interoperability with every Matter hub. The Mac HomeKit companion is built and tested in the Tuya repository; its signing and real home authorization require a Mac.

Qt 测试使用本地 HTTP 和合成 OHF 服务，CI 编译两个应用并运行离线套件。它们不能替代真实账号权限、家庭授权、设备或音视频链路验证。Mac HomeKit 伴侣位于涂鸦仓库，需要在 Mac 上正确签名并授权家庭访问。
