# 涂鸦名称与房间 beta / Tuya metadata sync beta

This integration targets **MatterDesk 0.7.0-beta.1** and **MatterCamViewer 2.5.0-beta.1**, using the optional Metadata Sync v1 service in **@linanw/homebridge-tuya-platform 2.7.0-beta.1**. Beta test packages are being prepared; download only files actually listed on the matching GitHub pre-release. The separate stable desktop channel (0.6.1 / 2.4.1) and plugin npm `latest` (2.6.2) do not include this integration.

本轮对应 **MatterDesk 0.7.0-beta.1**、**MatterCamViewer 2.5.0-beta.1** 和插件 **2.7.0-beta.1**。测试包正在准备，具体可用性以对应预发布页的实际附件为准；稳定桌面通道 **0.6.1 / 2.4.1** 及插件正式版 `latest` **2.6.2** 不包含本功能。

Names and rooms use a separately authenticated HTTP service. Native Matter pairing, device control and camera playback continue through the existing OHF controller. No vendor cluster, controller restart or replacement Fabric is required.

名称与房间通过独立授权服务传输。Matter 配对、设备控制和摄像头播放继续走原有 OHF 控制器，不增加厂商集群，也不重建 Fabric。

## 1. Install matching beta versions / 安装对应 beta

- [MatterDesk pre-release](https://github.com/linanw/MatterDesk/releases/tag/matterdesk-v0.7.0-beta.1): expected package `MatterDesk-0.7.0-beta.1-x86_64.AppImage`.
- [MatterCamViewer pre-release](https://github.com/linanw/MatterDesk/releases/tag/mattercamviewer-v2.5.0-beta.1): expected package `MatterCamViewer-2.5.0-beta.1-x86_64.AppImage`.

Once the files and SHA-256 checksums appear, verify the download, make the selected AppImage executable and run it. If the files are absent, that test package is not ready; the older stable package cannot substitute for it. Source builds must use the matching beta source tag and the [normal build requirements](../README.md#从源码快速开始). No signed Mac application is bundled with these Linux packages.

附件和 SHA-256 校验文件出现后，核验下载、添加执行权限并运行所需 AppImage。附件不存在时请等待，旧稳定包不能代替。本地源码构建使用相应 beta 标签及正常依赖；Linux 包不附带已签名 Mac App。

In Homebridge UI, open the Tuya plugin card's **⋮ → Manage Versions / 管理版本**, then select **beta / 2.7.0-beta.1**. CLI users run the following in the same Node/npm environment or container as Homebridge:

```sh
npm install -g @linanw/homebridge-tuya-platform@beta
# Exact beta / 固定版本：
npm install -g @linanw/homebridge-tuya-platform@2.7.0-beta.1
```

Follow the plugin's [beta service setup](https://github.com/linanw/tuya-homebridge/blob/v2.7.0-beta.1/docs/metadata-sync/SETUP.md). Its npm package contains the server and protocol documentation, but not the separately fetched Mac companion or optional Node adapter. npm publication uses `--tag beta`; stable `latest` remains 2.6.2. Keep existing Homebridge storage, controller data, pairing identities and configuration during upgrades or rollback.

插件 npm 包包含服务和协议文档，不包含需另外获取的 Mac 伴侣或 Node 适配器。升级和退回时保留 Homebridge 存储及控制器配对。返回插件正式版使用 `npm install -g @linanw/homebridge-tuya-platform@latest`；桌面端可重新运行稳定 AppImage，沿用原 Fabric 目录。

## 2. Connect read-only, then bind one device / 先只读绑定一个设备

1. Enable the plugin's metadata service with **`allowWrites: false`** first. Set a separate token using **`tokenEnv: "TUYA_METADATA_TOKEN"`** and the actual Homebridge service-process environment, then restart that bridge. Keep the value out of JSON, URLs, logs and Git; it is not a Tuya Access Secret, npm token or Apple password.
2. Keep the existing OHF controller. In MatterDesk open **Settings → 涂鸦同步**. In MatterCamViewer use the independent **涂** button, available even with no camera selected.
3. Enter the metadata base URL and service token. Another computer requires HTTPS with a certificate trusted by that client. Only exact `localhost`, `127.0.0.1` or `::1` permits development HTTP. Normal certificate checks remain enabled; redirects are rejected. Tokens stay in this application session and must be entered again after restarting it.
4. Check the authoritative observation and source availability. Select one explicit Tuya home, or the Custom device-name-only profile. Select an existing Matter endpoint and its source device, then confirm the binding. A unique `BridgedDeviceBasicInformation.uniqueId` match can suggest a source. Missing/duplicate IDs require explicit selection; labels and endpoint numbers are not automatic identity proof.
5. Enable the separate display mirror, choose following mode and explicitly adopt the source value to establish a baseline. Confirm one device's source name appears before adding more bindings. Select bidirectional management only for supported, deliberately enabled source writes.

先在服务进程环境配置独立令牌，按服务指南开启只读服务；终端里的临时 export 不会更新已经运行的 Homebridge。然后在「涂鸦同步」或独立「涂」入口中连接、选择家庭和一个设备，确认绑定并采用来源建立基线。跨机器使用 HTTPS；Qt 令牌不写入设置，重启后重新输入。不要通过修改普通本机别名来测试云写入。

## 3. Supported writes / 实际写入能力

| Provider / 来源 | Read / 读取 | Writes with server `allowWrites: true` / 开启后写入 |
| --- | --- | --- |
| Smart Home PaaS | Device names and App homes/rooms, subject to permissions / 名称及 App 家庭、房间，取决于权限 | Device names; room names when room metadata is authoritative / 设备名；房间资料完整时的房间名 |
| Custom project | Device names; assets are not App rooms / 设备名，assets 不是 App 房间 | Device names only / 仅设备名 |

The server implements official name-only PaaS requests: `PUT /v1.0/devices/{device_id}` and `PUT /v1.0/homes/{home_id}/rooms/{room_id}`, each with `{"name":"..."}`. Custom uses its separate IoT-03 device API. [Official contracts and remaining verification](https://github.com/linanw/tuya-homebridge/blob/v2.7.0-beta.1/docs/metadata-sync/TUYA_API_CONTRACTS.md) distinguish request support from real project/account permissions, which remain unverified on the user's account. A permission failure is not a confirmed update.

管理员将服务端 `allowWrites` 改为 **true** 并重启桥后，客户端仍会检查当前能力、源观测、明确绑定及草稿版本。PaaS 的设备名和房间名接口已实现且有官方依据；Custom 仅设备名。真实账号、API 订阅、角色和数据中心权限还需实际核对，不会因接口文档存在就宣称写入成功。

Device-name and room-name drafts are separate. To test a name write, edit the independent sync-name field and confirm once. To rename a room, select the bound device whose current source room is the intended room, edit **房间名称草稿**, then choose **确认写入房间名**. This changes that source room's name; it does not rename a local HomeStore room or move devices. Source identity, home, room and revision changes preserve the draft and require review/rebasing before another submission.

先在一个非关键目标上确认改名，并从服务权威读回及涂鸦 App 核对。房间名只修改所选设备当前关联的来源房间；源身份、家庭、房间或版本变化会阻止旧草稿。设备名和房间名分别确认，普通本机别名及房间始终独立。

**Device movement remains disabled (`moveDevice: false`) for both providers.** The documented POST adds devices but does not prove atomic movement out of the old room. PUT membership requests replace the whole list and must not be used as a move. The client never creates/deletes rooms, submits replacement lists, or treats unknown membership as unassigned. Full bidirectional room-membership synchronization is not available.

**两个 provider 都不支持设备移房写回。** POST 添加未证实原子移房，PUT 成员接口是全量替换，禁止用于单设备移动。不先删除再添加，不创建、删除房间。房间读取失败保留旧资料并标为未知，不把设备批量移到默认房间。

The server performs one coordinated cloud poll for all clients. Qt periodically reads its cache and opens no additional Tuya account session.

## Data, conflicts and recovery / 数据、冲突与恢复

- The dedicated `tuya-metadata/` mirror is scoped to controller/Fabric and source identity. Atomic files use owner-only permissions; session tokens are not stored.
- HomeStore aliases/rooms, Xiaomi camera metadata, pairings and video sessions remain independent. Existing local aliases and Xiaomi source displays take precedence over the Tuya fallback. Saving an ordinary alias never sends a cloud write.
- Full Unicode source names are retained. A truncated Matter nodeLabel is never a reverse rename. Missing binding or baseline means no automatic write; divergent changes preserve drafts for explicit resolution.
- Commands are recorded before POST and submitted once. HTTP 202/queued is not success: authoritative readback must match the requested value. Only confirmed matching values advance the baseline.
- Timeout/lost/uncertain outcomes stay locked across reconnects and restarts. Read current state, inspect the existing receipt, then explicitly acknowledge an unknown outcome when appropriate. No automatic POST retry or rollback occurs.
- A source epoch change requires a fresh read and review. The app must remain running and connected. Other controllers need their own authorized metadata adapter; Matter pairing does not expose their private room graph.

镜像按控制器和来源隔离，完整名称与普通别名、米家资料及配对互不覆盖。无基线不自动写，冲突保留草稿，排队不代表成功。命令先落盘、只发送一次，读回一致才更新基线；未知结果在重启后仍锁定，需要新读取和人工核实，不自动重发。

MatterCamViewer still displays only actual camera endpoints already exposed by its controller. This metadata integration adds no Tuya camera driver or video support to the Homebridge plugin and cannot turn a light or switch into a camera. Apple Home authorization is handled separately by the [Mac companion](https://github.com/linanw/tuya-homebridge/blob/v2.7.0-beta.1/companions/mac/README.md), built from source on a signed Mac Catalyst target.

## Validation / 验证边界

Local HTTP/OHF fixtures and offline UI tests check parsing, guards, receipts, capability limits, identity changes, independent local settings and camera-session preservation. Both desktop application targets must build for the beta packages. These checks do not establish live Tuya permissions, Apple Home access, physical camera behavior or compatibility with every Matter hub. Use one intended account/device/client for live testing; no credential value needs to be shared in chat or an issue.
