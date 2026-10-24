# MatterDesk & MatterCamViewer

Linux / Qt 6 的两个 Matter 客户端，共享设备协议、摄像头播放器与本地控制器集成。

Two Linux / Qt 6 Matter clients sharing the protocol, camera player and local-controller integration.

| 应用 / Application | 当前版本 / Version | 用途 / Purpose |
| --- | --- | --- |
| **MatterDesk** | **0.6.0** | 家庭总览、房间、收藏、配件控制、场景和应用内自动化；摄像头在主窗口右侧播放。 / A home dashboard with rooms, favorites, accessory controls, scenes and in-app automations; camera playback uses the main panel. |
| **MatterCamViewer** | **2.4.0** | 专注摄像头的无边框窗口；快照与直播铺满同一画面，支持自动隐藏控件、置顶和 X11 贴边抽屉。 / A compact frameless camera viewer with an integrated snapshot/live view, auto-hiding controls, pinning and an X11 edge drawer. |

两者连接 **OHF Matter Server**，再由控制器连接 Matter 设备或桥接器。Homebridge 的 IP 不是 Matter Server 的 WebSocket 地址。仅支持 HomeKit 的设备需要桥接端另外提供 Matter 能力。界面文字目前主要为简体中文。

```text
MatterDesk / MatterCamViewer → OHF Matter Server → Matter device or bridge
```

本项目不连接 Apple ID、Apple 家庭数据库或 iCloud；功能范围见 [功能矩阵](docs/FEATURES.md)。

## 下载与运行 AppImage

在 [GitHub Releases](https://github.com/linanw/MatterDesk/releases) 下载对应应用，无需另外安装 Qt、Node.js 或 npm：

- `MatterDesk-0.6.0-x86_64.AppImage`：完整家庭控制界面。
- `MatterCamViewer-2.4.0-x86_64.AppImage`：摄像头专用窗口。

```bash
chmod +x MatterDesk-0.6.0-x86_64.AppImage
./MatterDesk-0.6.0-x86_64.AppImage

chmod +x MatterCamViewer-2.4.0-x86_64.AppImage
./MatterCamViewer-2.4.0-x86_64.AppImage
```

AppImage 包含对应应用、Qt / WebEngine、Node.js 22 和锁定的控制器生产依赖。它复用已有本地控制器，必要时启动附带控制器；运行时不在镜像内安装 npm 包。配对数据与日志仍写入下文的 XDG 目录，不写到只读镜像内。使用远程控制器也可传入 `--server ws://CONTROLLER_HOST:5580/ws`。

当前 CI 构建基线为 **Ubuntu 24.04 x86_64**，仍依赖宿主 Linux 内核、显示 / 图形驱动、音频服务与兼容的系统 ABI；不承诺 Ubuntu 22.04 或任意旧发行版兼容。源码构建和依赖系统 Qt 的轻量 tar 包仍是可选方式。

若启动提示缺少 `libfuse.so.2`，Ubuntu 24.04 对应包为 `libfuse2t64`。无法使用 FUSE 时可改为解包运行：

```bash
sudo apt install libfuse2t64
# 或在无 FUSE 的环境中：
./MatterCamViewer-2.4.0-x86_64.AppImage --appimage-extract-and-run
```

解包运行只替代 FUSE 挂载，不绕过 WebEngine 沙箱。应用默认保留沙箱；如果 Ubuntu 的 AppArmor / 用户命名空间策略阻止 renderer，应由系统管理员按应用配置权限，或使用发行版 Qt 的源码构建。[AppImage FUSE 说明](https://docs.appimage.org/user-guide/troubleshooting/fuse.html)、[Ubuntu 24.04 命名空间说明](https://documentation.ubuntu.com/release-notes/24.04/)

## 从源码快速开始

源码要求 **Qt 6.4+、CMake 3.21+、C++17**。摄像头界面需要 Qt WebEngine 和 WebChannel。下列依赖命令适用于 Ubuntu 24.04：

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build \
  qt6-base-dev qt6-websockets-dev qt6-webengine-dev qt6-webchannel-dev
git clone https://github.com/linanw/MatterDesk.git
cd MatterDesk
./scripts/build.sh
./scripts/run.sh --demo
```

演示模式只模拟标准版的普通配件，不连接真实设备。构建会生成 `build/matterdesk` 和 `build/mattercamviewer`。

源码或轻量 tar 包连接自己的设备时，先安装附带控制器的依赖。需要 **Node.js 22.13+ 和 npm**；锁定版本为 OHF `matter-server` **1.5.3**，首次安装需要联网。AppImage 已包含这些运行依赖，无需执行此安装步骤：

```bash
./scripts/setup-controller.sh
./scripts/run.sh                    # MatterDesk
./scripts/run-mattercamviewer.sh    # MatterCamViewer，另一个终端中运行
```

启动器复用 `127.0.0.1:5580` 上已有的控制器；没有服务时启动本项目的控制器。退出时只停止该启动器自己启动的控制器。两个应用共享这个本地服务；需要独立于任一窗口持续运行时，可先在单独终端执行 `./scripts/start-controller.sh`。旧入口 `./scripts/run-mini.sh` 继续转发到 MatterCamViewer。

使用已部署的控制器时，不需要安装本地 Node 依赖：

```bash
./scripts/run.sh --server ws://CONTROLLER_HOST:5580/ws
./scripts/run-mattercamviewer.sh --server ws://CONTROLLER_HOST:5580/ws
```

控制器应处于可信网络内；本项目的本地启动脚本默认只监听回环地址。

## 配对与控制器数据

1. 从实际提供 Matter 服务的设备或桥接插件获取 **Matter 配对码**，或 `MT:…` 二维码原文。HomeKit 配对码不能用于此处。
2. 在应用中点击「添加设备」或「添加摄像头」。已加入同一个控制器的摄像头会自动出现在两个应用中。部分插件为摄像头单独提供配对码，配对主桥不一定包含摄像头。
3. 已加入其他控制器的设备，需要先在原控制器中打开新配对窗口，并使用新的多管理员配对码。局域网配对应保证设备与控制器间的 IPv6、mDNS 和网络连通。

默认 Fabric / 配对目录保持为：

```text
${XDG_DATA_HOME:-$HOME/.local/share}/matterdesk/controller
```

可用 `MATTERDESK_DATA_DIR` 指定已有目录；重启与升级时沿用同一目录。删除或更换它会丢失该控制器的配对身份。日志位于 `${XDG_STATE_HOME:-$HOME/.local/state}/matterdesk/controller.log`。窗口偏好与本机家庭组织保存在各自 Qt 应用数据目录，摄像头快照只留在内存。

多网卡环境可指定实际网卡；额外选项传给 OHF Matter Server：

```bash
MATTER_INTERFACE=YOUR_INTERFACE ./scripts/start-controller.sh
./scripts/start-controller.sh --port 5581
```

如果配对错误明确指出设备使用测试 / 开发证书，对自己信任的开发桥接器可使用：

```bash
./scripts/run-homebridge-dev.sh
./scripts/run-mattercamviewer-homebridge-dev.sh
```

这为新启动的控制器设置 `ENABLE_TEST_NET_DCL=true`，不改变已运行的服务。已有服务需要由其管理者在保留原 Fabric 目录的前提下以相应配置重启。正常启动不会默认启用开发证书。

## 摄像头与 MatterCamViewer

摄像头直播需要实际的 Matter Camera 端点及 OHF 1.5.3 的 `camera_*` 接口；快照还需 JPEG Snapshot 能力。播放器支持 H.264 视频与 Opus 音频。声音默认关闭；对讲仅在设备提供能力且用户主动授权麦克风时启用。

MatterDesk 在右侧主面板等比适配完整视频，保留导航。MatterCamViewer 将预览与直播铺满同一窗口，比例不同时居中裁切：

- 默认显示快照，可见时约每 30 秒更新；点击「直播」进入同一窗口的直播，点击「快照」返回预览。
- 信息和按钮在 10 秒无操作后隐藏，单击画面切换显示；拖动画面移动窗口。
- F11 或全屏按钮切换全屏。双击先恢复默认 800×450，再次双击进入全屏；Esc 退出全屏，普通窗口中可从直播返回快照。
- 「钉」按钮控制普通窗口置顶。X11 下可拖到屏幕边缘收为细边条，悬停展开；Wayland 下使用系统拖动，停用屏幕外抽屉。
- 多摄像头时通过选择器切换。窗口隐藏、最小化、抽屉收起或正在直播时暂停后台快照抓取。

并发播放取决于桥接器和设备可用流数量。配套米家桥接器支持共享上游编码的多路观看，各窗口使用独立 WebRTC 会话；客户端不能扩大设备本身的并发限制。

云台四向步进需要插件提供 `0xFFF1FC01` 厂商扩展；至少间隔 1.8 秒，每次只发一个请求，不自动重试。接口没有角度、位置或移动完成回报。

## 米家名称与房间

配套插件 **0.4.0+** 提供 `0xFFF1FC02` 元数据扩展时，配件详情和 MatterCamViewer 的「米」入口可显示来源名称、家庭、房间、读取时间，执行刷新及自动同步设置。两个应用使用同一份控制器模型注册；后台读取由插件负责，Qt 不额外轮询云端。

云端改名或移动到已有房间，仅在插件声明并验证对应写入能力后启用。名称上限为 160 个 UTF-8 字节；未分房只读。编辑检查设备身份、来源 epoch 和 revision，并在新权威读取与目标一致后确认成功；超时不会自动重发。缺失的来源信息显示为暂不可用。本机别名和本机房间仍只保存在当前电脑，不自动写回米家。

Apple 家庭的名称 / 房间同步、HomeKit Secure Video、历史录像、云端推送和家庭中枢功能不在当前实现范围。

## 测试、安装与发行

主分支和面向主分支的 PR 运行 CI：Ubuntu 24.04 的系统 Qt、Node.js 22 和锁定控制器依赖，构建两个应用并运行离线 Qt / JavaScript 单元测试和本地 schema 转换测试。它不访问真实家庭设备、账号或生产控制器。CI 先检查用户命名空间，仅在宿主策略需要时为测试程序加载临时 AppArmor 例外，不关闭 WebEngine 沙箱。可在本机运行构建和测试：

```bash
./scripts/setup-controller.sh
./scripts/build.sh
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1 \
  ctest --test-dir build --output-on-failure
```

`camera_live_probe`、`snapshot_live_probe` 和 PTZ probe 是显式选择的独立集成工具，不属于默认 CTest。测试覆盖不等于所有真实配件兼容，详见 [验证边界](docs/FEATURES.md#验证边界)。

安装两个可执行文件、桌面入口和图标：

```bash
cmake --install build --prefix "$HOME/.local"
```

CMake 安装不复制 Node 依赖或启动脚本。桌面入口连接已运行的本地控制器；首次使用可先从源码 / Release 目录运行 `scripts/start-controller.sh`，或使用完整启动脚本。源码树中的 `scripts/run*.sh` 优先选择 `build/`，其次为 Release 的 `bin/`。

两个应用分别使用 `matterdesk-v0.6.0`、`mattercamviewer-v2.4.0` 形式的版本标签。发布流水线核对产品版本、构建与测试，再为对应产品生成独立 AppImage、可选轻量 tar 包及 SHA-256 校验文件，发布到 GitHub Release。源码属于同一个仓库。

AppImage 带 Qt / WebEngine 和控制器运行依赖；轻量 tar 包动态依赖系统 Qt，需要另装 Node 控制器依赖。请查具体 Release 的构建版本与系统基线。源码包不包含可执行文件、Node 依赖、Fabric 数据或内部 QA 文件。AppImage 的实际依赖清单及许可文件放在镜像内 `usr/share/doc/licenses/`，可用 `--appimage-extract` 查看；该目录不会改变项目尚未指定源码许可的状态。

项目尚未指定源码许可；公开可见不代表已授予开源使用、修改或再分发许可。依赖各自的许可说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## English guide

**MatterDesk 0.6.0** is the full home dashboard. **MatterCamViewer 2.4.0** is the camera-only application, combining the Mini 2.3 window behavior with the current metadata integration. Both connect to an **OHF Matter Server**; a Homebridge host is not automatically a Matter Server. The interface is primarily in Simplified Chinese.

For a ready-to-run application, download `MatterDesk-0.6.0-x86_64.AppImage` or `MatterCamViewer-2.4.0-x86_64.AppImage` from [Releases](https://github.com/linanw/MatterDesk/releases), make it executable and run it. Each includes Qt / WebEngine, Node.js 22 and production controller dependencies, so system Qt / Node installation is unnecessary. The build baseline is **Ubuntu 24.04 x86_64**; host kernel, graphics / audio support and compatible system ABI remain required. Older distributions are not guaranteed.

If FUSE 2 is missing on Ubuntu 24.04, install `libfuse2t64`, or use `./APPLICATION.AppImage --appimage-extract-and-run`. Extraction avoids the FUSE mount only; it does not bypass WebEngine sandbox requirements. Application launchers keep the sandbox enabled and do not change system security settings.

Source build requirements are **Qt 6.4+** (Core, Gui, Widgets, Network, WebSockets, WebEngineWidgets and WebChannel), **CMake 3.21+** and **C++17**. Use the Ubuntu 24.04 dependency and build commands above. `./scripts/run.sh --demo` opens the full-app demo without connecting to devices. The build produces `matterdesk` and `mattercamviewer`.

For a source / lightweight tar installation using the local controller, install **Node.js 22.13+ / npm**, run `./scripts/setup-controller.sh`, then launch `./scripts/run.sh` or `./scripts/run-mattercamviewer.sh`. AppImage users skip dependency installation. The controller is pinned to **matter-server 1.5.3**. Launchers reuse `ws://127.0.0.1:5580/ws`, start a local controller when needed and stop only the controller they started. For independent controller lifetime, run `./scripts/start-controller.sh` in its own terminal first. An existing remote controller can be selected with `--server ws://CONTROLLER_HOST:5580/ws`; Node is then optional on the client computer.

Pair with a **Matter** setup code or `MT:…` payload, not a HomeKit code. Some camera plugins expose a separate Matter camera device. Keep the original `${XDG_DATA_HOME:-$HOME/.local/share}/matterdesk/controller` directory or the directory selected by `MATTERDESK_DATA_DIR`: it contains the controller identity and pairings. The new application name does not change that shared Fabric path. Development-certificate launchers opt in only for controllers they start; existing controllers retain their own configuration.

Camera playback needs a real Matter Camera endpoint and the controller's `camera_*` API. H.264 / Opus playback, optional talkback, JPEG snapshots and concurrency depend on the exposed device capabilities. MatterDesk fits live video into its main panel. MatterCamViewer fills one frameless window, offers auto-hiding controls and pinning, and supports an edge drawer on X11. Wayland uses system window movement without the off-screen drawer.

PTZ and Xiaomi metadata use explicit vendor clusters. Metadata writes require verified plugin capabilities, identity / source / revision checks and authoritative readback. Local aliases and rooms remain local. Neither application integrates with Apple accounts, Apple Home data, iCloud recording or an Apple home hub.

CI runs on `main` changes and pull requests targeting `main`, building both applications and running offline tests without real hardware or cloud accounts. Product tags trigger version-checked releases of independent AppImages, optional lightweight tar packages and SHA-256 checksums. Bundled dependency licenses and inventory are under `usr/share/doc/licenses/` inside each AppImage. `cmake --install build --prefix "$HOME/.local"` installs source-build binaries and desktop entries, while controller scripts remain in the source / Release directory.

**No project source license has been selected yet.** Public availability does not grant an open-source license. Third-party components retain their own terms, documented in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
