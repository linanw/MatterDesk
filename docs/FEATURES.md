# 功能范围 / Feature scope

本文区分稳定通道 **MatterDesk 0.6.1 / MatterCamViewer 2.4.1** 与新增涂鸦元数据的 **0.7.0-beta.1 / 2.5.0-beta.1** 测试通道。beta 测试包正在准备，以对应 GitHub 预发布页实际附件为准。安装、配对与启动方式见 [README](../README.md)。两个应用使用同一套 Matter 协议、摄像头播放器和控制器扩展；设备或桥接器没有公布的能力，客户端不会根据名称或外观推断。

MatterDesk provides a full home dashboard; MatterCamViewer focuses on camera viewing. Both use an independent OHF Matter Server. Capability support below describes implemented protocol handling, not universal hardware compatibility or Matter certification.

## 两个应用

| 功能 | MatterDesk | MatterCamViewer |
| --- | --- | --- |
| 普通配件与传感器 | 家庭总览、分类、详情和支持的控制 | 仅显示摄像头 |
| 房间、区域、收藏、本机别名 | 提供本机组织与编辑 | 摄像头选择与来源信息 |
| 场景、应用内自动化、活动记录 | 提供 | 无此界面 |
| JPEG 快照与直播 | 卡片快照，右侧主面板等比适配直播 | 快照与直播铺满同一窗口，比例不同时居中裁切 |
| 声音、对讲 | 设备支持时可用，默认关闭 | 设备支持时可用，默认关闭 |
| 窗口控制 | 常规桌面主窗口 | 无边框、10 秒自动隐藏控件、置顶、全屏、X11 贴边抽屉 |
| 摄像头四向步进 | 提供扩展能力时可用 | 提供扩展能力时可用 |
| 米家名称 / 房间 | 配件详情中显示及明确编辑 | 「米」入口显示及明确编辑 |
| 涂鸦名称 / 房间（beta） | 设置中的独立同步服务、绑定与显示镜像 | 独立「涂」入口；只匹配真实设备，不新增摄像头驱动 |
| 配对与控制器 | Matter 配对码，本地或显式远程控制器 | 同一本地控制器，也可显式指定远程控制器 |

MatterCamViewer 2.4.0 采用 Mini 2.3 的窗口行为并接入当前元数据功能。窗口默认 800×450；单击切换控件显示，双击先恢复默认尺寸，再进入全屏。F11 直接切换全屏，Esc 退出全屏；普通窗口内 Esc 从直播返回快照。X11 支持拖到屏幕边缘后收起为细边条，悬停展开；Wayland 使用系统拖动并停用屏幕外抽屉。

## 家庭与配件组织

以下工作流属于 MatterDesk；组织信息保存在本机，不同步到 Apple 家庭。

| 功能 | 实现 | 边界 |
| --- | --- | --- |
| 家庭总览 | 设备状态、收藏与设备分类 | 使用当前控制器实际发现的配件；区分未知与离线 |
| 房间、区域 | 创建、编辑、删除，将房间归入区域 | 本机数据，不自动写回桥接器或米家 |
| 名称与收藏 | 本机别名、房间归属、收藏 | 以原 Matter 标识匹配；按控制器隔离保存 |
| 原生详情 | 按设备报告能力显示控制与读数 | 未报告的操作不会出现 |
| 场景 | 顺序执行多个配件的目标动作 | 开始前校验全部动作，每步再次检查设备；失败停止，无事务回滚 |
| 自动化 | 本机星期 / 时间，或设备状态变为指定值时运行场景 | 应用需保持运行且控制器连接；无后台服务、地理围栏或离线补跑 |
| 活动记录 | 最近 200 条操作、场景与自动化结果 | 请求记录，不等于设备物理状态历史或安全审计 |
| 持久保存 | 家庭组织、场景、规则、运行去重及活动 | Qt 应用数据目录内的 `homes/`；演示配置独立保存 |

自动化使用本机时间。刚连接、未知状态恢复与首次同步时先建立基线；场景执行期间与短暂抑制期不连续引发设备联动。同日时间规则的执行标记会保存，失败或部分执行不自动重试。取消、断线或失败无法撤回已送达设备的动作。场景是顺序请求，不是同时下发的 Matter 组操作。

## 控制与读数矩阵

| 设备 / 能力 | 可用操作或显示 | 识别与限制 |
| --- | --- | --- |
| 开关、插座、灯 | 明确开启 / 关闭 | On/Off 集群；不发送隐式 Toggle |
| 调光灯 | 0–100% 亮度 | Level Control；换算 Matter 0–254 |
| 色温灯 | 支持范围内的色温 | Color Control；mired 换算并检查物理上下限 |
| 彩光灯 | 色相 0–360°、饱和度 0–100% | Hue/Saturation；优先增强色相，依据接受命令列表回退；不转换 XY-only 灯具 |
| 门锁 | 主动上锁 / 开锁、锁状态、电池 | Timed Interaction；不携带或保存 PIN，设备要求 PIN 时显示错误 |
| 场景中的门锁 | 上锁 | 场景与自动化不提供开锁动作；开锁只在详情中主动操作 |
| 窗帘、升降遮阳 | 0–100% 开合位置 | Lift 与 Position Aware Lift；界面 0=全关、100=全开；无倾角、校准或非位置感知控制 |
| 恒温器 | 支持的关闭 / 制热 / 制冷 / 自动，室温、占用设定点 | 检查特征、运行序列、范围和制热制冷最小间隔；无日程、预设、干燥或应急制热控制 |
| 风扇 | 百分比与设备支持的模式 | 依赖模式序列和 Auto 特征；无摇头、风向或自然风配置 |
| 温湿度、人体、接触、照度、电池 | 只读状态与数值 | 按集群解析；接触 true=关闭；未知与 null 不用零替代 |
| 空气质量 | 设备报告的优 / 良 / 中等 / 差等级 | 不推算为统一 AQI |
| CO₂、PM2.5 | 数值及设备报告单位 | 规则字段仅在已知 ppm / µg/m³ 单位时提供 |
| 甲醛、TVOC、PM1、PM10 | 数值及设备报告单位 | 只读，不估计缺失单位 |
| 电功率、累计输入电能 | W、kWh | 按 mW / mWh 换算；无账单、费率与历史耗能图 |
| 摄像头 | JPEG 快照、H.264 直播、Opus 收听、可选对讲 | 实际 Camera 设备及相关服务集群，OHF schema 14 的 `camera_*` 接口；并发受设备和桥接器限制 |
| 摄像头云台 | 上 / 下 / 左 / 右单次步进 | 厂商集群 `0xFFF1FC01`；方向需公布，至少 1.8 秒冷却，无排队或自动重试；无角度、位置、变焦或完成回报 |
| 米家名称与房间 | 来源显示、刷新、同步开关、能力允许时明确改名 / 移动 | 厂商集群 `0xFFF1FC02`；设备身份、来源 epoch / revision 守卫及新读取确认；不提供全局房间新建、改名或删除 |
| 未覆盖设备 | 标识、属性与可识别的只读信息 | 无阀门、媒体、报警器布防、烟雾 / CO 报警联动或扫地机器人专用控制 |

控制依据 Descriptor 服务集群和接受命令列表。请求前重新检查当前设备、端点、在线状态、范围与上下文；过期卡片不能控制已移除的设备。未知锁枚举、半锁、未闩上与 null 不当作已开锁。成功回执不会把显示强行改成目标状态，界面仍等待设备报告；属性写入逐路径检查设备返回状态。

## 摄像头与来源数据

快照仅放在内存。隐私模式、控制器断开或切换时清空；离线或抓取失败时保留旧图并明确标记。不可见、最小化、抽屉收起和直播时暂停相应快照更新。麦克风默认关闭，仅在用户启用对讲且允许权限后采集，关闭对讲或播放器时释放。

两个应用可同时播放的数量由桥接器和实际摄像头决定。支持多路的配套桥接器可共享上游取流与编码，各窗口保持独立 WebRTC / SRTP 会话；关闭一个窗口不应结束另一窗口自己的会话。客户端只释放自己分配的流资源。

米家元数据需要配套桥接器 0.4.0+ 和控制器启动前的厂商模型注册。插件负责后台约每 60 秒读取，Qt 不额外轮询云端。改名 / 移动需要已验证的写入能力；名称最多 160 个 UTF-8 字节，只能选择已有非空房间 ID。回执与完成确认分开，来源数据更新后需新权威读取与目标值一致才确认；超时不自动重发。本机别名和房间不触发云端写入。缺失来源信息保持暂不可用，未知房间不当作未分房。

元数据镜像单独保存在 Qt 应用数据目录的 `camera-metadata/`，按控制器 / Fabric 隔离并原子写入，不保存米家账号凭据，不改变本机家庭数据格式。普通元数据更新只更新文字，不重新打开直播会话。

## 与 Apple 家庭的差异

[涂鸦元数据 beta](TUYA_METADATA_SYNC.md)为两个 App 增加独立的 HTTP 同步通道，对应插件 2.7.0-beta.1。它保留本地家庭存储与米家协议，普通本机别名编辑不写回涂鸦。管理员开启 `allowWrites` 后，PaaS 支持设备名称和房间名称写入；Custom 仅设备名，不提供 App 房间。设备移动仍禁用，完整房间归属双向同步尚不可用。房间名草稿只针对所选设备明确关联的来源房间；账号权限及设备行为仍需实际验证。Apple 家庭数据需要涂鸦仓库中的 Mac HomeKit 伴侣另外授权，Qt 应用不直接读取 Apple ID 或家庭数据库。

| Apple 家庭能力 | 本项目状态 |
| --- | --- |
| 家庭、房间、收藏、配件面板、场景 | 提供本机类似工作流，与 Apple 数据互不相通 |
| 配件与按时间自动化 | 应用内规则，依赖当前进程与控制器 |
| Apple ID、成员共享、访问权限 | 未提供 |
| Siri、Home Key、Apple Watch、Apple 小组件 | 未提供 |
| 家庭中枢、iCloud 远程访问、推送 | 未提供 |
| HomeKit Secure Video、iCloud 录像时间线 | 未提供 |
| 人物 / 动物 / 车辆或面孔识别、活动区域、门铃通知 | 未提供 |
| 历史录像、云备份、长期家庭安全历史 | 未提供 |
| HomeKit-only 配件 | 需要桥接端实际提供 Matter，不能直接加入 |

## 验证边界

GitHub Release 为两个应用分别提供 AppImage，包含 Qt / WebEngine、Node.js 22 和控制器生产依赖；源码与依赖系统 Qt 的轻量 tar 包也可使用。AppImage 的构建基线是 Ubuntu 24.04 x86_64，仍需要宿主兼容的内核、图形 / 音频和系统 ABI。FUSE 不可用时的解包运行仅替代挂载，不关闭 WebEngine 沙箱。安装和运行见 [README](../README.md#下载与运行-appimage)。

默认 CTest 使用本地假 WebSocket 服务、合成设备状态和离线播放器测试，覆盖协议请求、能力判断、参数与状态校验、配对界面、快照 / 直播生命周期、家庭组织、自动化、窗口交互、PTZ 和元数据。Node schema 测试检查锁定 OHF / Matter 模型的数字字段和转换格式。独立隔离设备 / 控制器联调验证过元数据读取、刷新、同步开关、中文改名、房间移动及来源变化。

CI 不连接真实生产控制器、摄像头或云端账号。主分支 / PR 测试与版本标签发布分别运行；AppImage 发布另检查封装后的入口、WebEngine renderer 与依赖部署。CI 沙箱例外仅对临时测试程序开放用户命名空间，不写入产品或关闭沙箱。默认不构建的 live / PTZ probe 需要显式指定隔离回环服务并拒绝生产端口 5580。测试成功不能替代具体配件、音视频链路或云端账号权限验证；当前客户端不声明 Matter 认证。

```bash
./scripts/setup-controller.sh
./scripts/build.sh
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1 \
  ctest --test-dir build --output-on-failure
```

## English summary

MatterDesk supports capability-driven accessory controls, local home organization, sequential scenes and in-process automations. MatterCamViewer exposes camera viewing, window controls, optional PTZ and metadata. Shared camera support includes in-memory snapshots, H.264 / Opus playback and explicitly enabled talkback. Device capabilities and bridge limits determine availability and concurrent streams.

Xiaomi metadata editing requires verified vendor capabilities, guarded identity / source / revision context and fresh authoritative readback. Local aliases remain local. PTZ offers discrete directions with rate limiting, not position feedback or continuous movement.

The applications do not integrate with Apple Home accounts, iCloud, HomeKit Secure Video, Apple home hubs or HomeKit-only devices. Default tests use synthetic local fixtures; hardware and account compatibility must be checked separately. No Matter certification is claimed.
