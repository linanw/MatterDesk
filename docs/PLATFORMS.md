# 同一家庭，三个入口

在 NAS 运行一个 MatterServer。Linux MatterDesk 和 HarmonyOS MatterDesk 使用同一个 Fabric，MatterServer 随附的 Web App 可直接在浏览器打开。HarmonyOS 默认打开和浏览器相同的 Web 界面：同一套设备、控件、筛选和直播逻辑，不维护两份界面。Linux 保留本地 Qt 界面，并沿用相同的设备列表、配对、控制和直播流程。MatterCamViewer 仍是独立编号的 Linux 极简摄像头应用。

```text
Linux Qt  ──── WSS /ws ─────────────┐
Harmony ─┐                           ├── NAS HTTPS 网关 ── MatterServer ── Homebridge Matter
浏览器   ├─ HTTPS / + 同源 WSS /ws ───┘
         ┘
```

## 共用用例

| 用例 | Linux Qt | HarmonyOS 默认界面和浏览器 |
| --- | --- | --- |
| 连接 NAS | 控制器与日志 → 连接 NAS；用 HTTPS 地址和账号登录 | 打开 HTTPS 地址；通过同一账号登录 |
| 浏览设备 | 按房间、类型、常用、在线状态筛选；搜索名称 | 对应的房间、类型、常用、在线筛选和搜索 |
| 控制设备 | 根据配件报告的服务端集群显示控制 | 使用同样的集群与命令，仅显示设备支持的操作 |
| 添加设备 | 使用 Matter 配对码，向当前控制器请求配对 | 同样向当前控制器配对；结果由服务端确认 |
| 摄像头 | 快照 → 主面板直播 → 返回；支持按设备能力收听 | 同样从快照进入直播，使用与 Qt 相同的 WebRTC 播放器；退出会停止会话 |
| 名称、房间、常用 | 本机 Qt 配置 | Web 存储按服务端和 Fabric 区分；另一浏览器中的配置独立 |

设备配对与状态共享；**本地别名、收藏、房间配置和自动化不会因为连接同一个服务就自动同步**。Linux 的 Tuya 云端授权和双向资料同步、自动化、活动记录、原生小窗和厂商媒体功能保持原有入口。Web 与 HarmonyOS 不把尚未实现、设备未报告或未验证的功能显示为可用。HarmonyOS 兼容入口仍能直接连接既有 OHF 控制器，也能使用合成测试画面。

## 使用同一个服务

- **Linux**：在「控制器与日志」填入 `https://matter.linanw.me` 后点击连接，或使用 `./MatterDesk-<版本>-x86_64.AppImage --server https://matter.linanw.me`。应用连接 `wss://matter.linanw.me/ws`，在登录窗口输入 NAS 账号。凭据不写入 Qt 设置。无需 NAS 时继续使用 `ws://127.0.0.1:5580/ws`。
- **HarmonyOS**：打开 MatterDesk，默认面板为 `https://matter.linanw.me/`。需要更换站点时在应用连接设置输入自部署的 HTTPS 地址；凭据只用于当前选中的受信站点。真机需要与设备匹配的 HarmonyOS 签名、API 26 运行环境及 ArkWeb。
- **Web**：在浏览器访问同一个 HTTPS 入口。网关认证后，Web 用同源 `/ws` 接收设备状态。局域网媒体连接情况取决于设备和网络，公开 WSS 不代表实时视频一定能跨网访问。
- **NAS**：使用 [Compose 部署说明](../deploy/README.md)。给域名配置匹配的受信任证书；现有 IPv6/mDNS 路由要能到达 Matter 设备。

## 验证边界

Linux 测试包含 MatterServer 协议夹具、TLS 校验、登录头与切换控制器时的凭据隔离，以及右面板直播与窗口位置回归。Web 使用同协议夹具和共享播放器测试。Harmony 在 DevEco API 26 构建 ArkTS 并复用同一个静态面板；设备安装、ArkWeb 原生登录和真实摄像头仍需签名后的真机验收。独立 NAS 烟测使用新建空 Fabric、合成数据和隔离网络。
