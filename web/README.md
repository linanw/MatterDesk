# MatterDesk Web

中文桌面与手机网页。兼容 MatterDesk 的同一份 Matter Server Fabric；Harmony 可通过同源 HTTPS WebView 打开相同网页。

```sh
npm --prefix web run build
npm --prefix web test
npm --prefix web run test:browser
```

要求 Node.js 22.13+；无需安装前端依赖。浏览器验收需要 Chromium/Chrome，默认命令是 `google-chrome`，可通过 `MATTERDESK_CHROME` 指定。浏览器测试启动随机端口的本机模拟控制器，不接触真实家庭。设置 `MATTERDESK_WEB_SCREENSHOT_DIR` 可保存该模拟环境的桌面版与手机版截图。

构建将 `web/src` 复制到 `web/dist`，并从仓库 `src/camera/player.js` 复制 Linux 原生版使用的同一份 WebRTC 播放器。默认服务器路径是同源 `/ws`；页面不接受自定义地址、密码或 URL 令牌。使用部署目录中的 HTTPS 与认证配置：**页面和 `/ws` 都必须经认证**。WebSocket 的 `Origin` 需和网页相符。生产请勿开放未认证 Matter Server 端口。仅本机测试使用 HTTP。

- 家庭总览、摄像头与收藏，按名称/桥接器、类别、在线状态和房间筛选。
- 支持手动码和 `MT:` 二维码文本网络配对。配对码不会保存。
- 只在控制器回报能力时提供电源、亮度、色温、色相和饱和度、门锁、窗帘、恒温器和风扇控制。操作完成后，当前值仍以配件回报为准。
- 已知 Fabric 的房间、显示名称与收藏保存在当前浏览器，不会和原生版、其他浏览器或用户账号同步。
- 摄像头快照和按需 WebRTC 直播使用同一套 Matter API。需要支持 schema 14 的 Matter Server、兼容的浏览器及摄像头。收音默认关闭；受浏览器权限限制，对讲仅在有能力时显式开启。关闭浏览器页面时，会尝试停止本页的会话；浏览器断开后由控制器负责清理。
- 原生版的场景与自动化、PTZ、摄像头名称写入设备、Mini 桌面窗口特性及浏览器扫码目前未移植。浏览器测试验证握手、配对流程、状态/控制、真实 JPEG 解码、WebRTC 协议请求和忙状态处理，不宣称已覆盖每台物理设备的媒体解码或对讲。
