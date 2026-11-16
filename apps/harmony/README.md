# MatterDesk for HarmonyOS

这个 HarmonyOS 应用默认显示与浏览器相同的 MatterDesk Web 首页。顶部「连接设置」可改用自托管的 HTTPS 站点，默认网址为 `https://matter.linanw.me`。填写站点根地址，不附带子路径、用户名或密码。

服务器通过 HTTP Basic `MatterDesk` 认证域请求登录时，应用会显示原生表单。应用只保存站点地址，不保存账号密码；ArkWeb 处理该站点的会话认证。网页和 `/ws` 使用同一个 HTTPS 站点，证书按系统信任验证。Linux、浏览器和 HarmonyOS 使用同一控制器时，可访问它已配对的设备。房间和收藏等 Web 本地设置按 WebView 站点保存在设备上。

Web 模式用同一套界面浏览、搜索和控制配件，查看摄像头快照和直播。功能取决于控制器和设备能力；直播需要 ArkWeb 支持 H.264/Opus 及摄像头网络可达。此版本的 HarmonyOS 外壳未申请麦克风权限，不能在外壳内做双向语音。

## 保留的原生直连模式

「连接设置 → 使用 Matter Server 直连」打开原生 ArkUI 客户端。支持浏览与搜索、收藏、灯和插座开关、窗帘打开/停止/合上、输入配对码、WebRTC 直播和收听。断开控制器也能通过「模拟摄像头 → 播放测试画面」播放本地动态彩条；不需要真实设备、摄像头或麦克风。

可直接填可信的 `wss://服务器/ws`，也可通过无线 HDC 转发 Linux 上只监听回环的控制器：

```bash
hdc tconn <平板IP:无线调试端口>
hdc -t <平板IP:无线调试端口> rport tcp:15580 tcp:5580
```

直连页默认使用 `ws://127.0.0.1:15580/ws`；返回键回到 Web 模式。

## 构建

需要华为 DevEco Studio 或相匹配的命令行工具与 HarmonyOS SDK **API 26**。本地验证环境为 API 26 Release SDK `26.0.0.105`、DevEco 自带 Node.js 与 Java。构建脚本使用 Bash、Python 3、`ohpm` 和 `hvigorw`；OHPM 需要能访问锁文件中的依赖源或已有缓存。

从仓库根目录运行：

```bash
DEVECO_STUDIO_HOME=/opt/devecostudio apps/harmony/build-unsigned.sh
```

默认生成 `apps/harmony/dist/MatterDesk-unsigned.hap`。可传一个输出目录，也可用 `DEVECO_SDK_HOME`、`JAVA_HOME` 指定工具链路径。脚本在临时目录使用公共无签名模板，并打入唯一的 `src/camera/player.js`。它不读取本地签名配置，也不连接或安装到设备。

自动化发布可传 `MATTERDESK_HARMONY_BUILD_MODE=release`，并同时指定 `MATTERDESK_HARMONY_VERSION`（例如 `0.7.0-beta.2`）和 `MATTERDESK_HARMONY_VERSION_CODE`（需按发布顺序递增）。版本覆盖只应用到临时构建目录；输出仍是无签名 HAP。

如需在 DevEco Studio 中调试，先在仓库根目录准备 IDE 专用文件：

```bash
cp -n apps/harmony/build-profile.unsigned.json5 apps/harmony/build-profile.json5
cp src/camera/player.js apps/harmony/entry/src/main/resources/rawfile/camera/player.js
```

再打开 `apps/harmony`。真机安装或分发需要适用于 `com.matterdesk.harmony` 的有效签名；请用 IDE 本地签名或经授权的发布签名。IDE 的项目构建配置已忽略，签名文件和口令留在仓库外。
