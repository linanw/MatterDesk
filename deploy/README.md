# MatterDesk 独立 NAS 部署

这一套部署在 NAS 上同时提供 Matter Controller 和统一 Web 面板。Linux Qt 和 Harmony 客户端可以连接同一套控制器并看到同一个 Fabric。

| 入口 | 用途 |
| --- | --- |
| `https://matter.linanw.me/` | Web 面板、Harmony Web 面板 |
| `wss://matter.linanw.me/ws` | Linux Qt 与 Web 面板使用的 Matter Server WebSocket |
| `127.0.0.1:5580` | NAS 内部 Matter Server，不能作为远程客户端入口 |
| `127.0.0.1:18081` | NAS 内部 Caddy，默认由 DSM/nginx 终止 HTTPS 后转发 |

两条公开路径均要求同一 HTTP Basic 账号，密码只通过 HTTPS/WSS 发送。浏览器完成 Basic 登录后使用本页同源 WebSocket；原生客户端在 Upgrade 请求中发送 `Authorization: Basic <Base64(UTF-8 用户名:密码)>`。不要把用户名、密码或配对码写进 URL。带 `Origin` 的 WebSocket 请求只接受部署的准确 HTTPS 来源，原生客户端可不发送 Origin。Caddy 转发时移除 Authorization/Cookie。默认仅转发 `/ws`，不公开上游 OHF dashboard、健康检查或 BLE 入口。[Caddy 身份验证](https://caddyserver.com/docs/caddyfile/directives/basic_auth) · [WebSocket 反代](https://caddyserver.com/docs/caddyfile/directives/reverse_proxy)

## 准备文件

需要 Linux 主机（Synology DSM / Container Manager 亦可）、Docker 和 Compose。部署包需一并包含项目根的 `.dockerignore`、`controller/` 固定依赖和 vendor 模块、`deploy/`、以及已经构建的 `web/dist/`。如果从源码部署，在能运行 Node.js 22.13+ 的电脑上先构建 Web，再将整包同步到 NAS：

```sh
# 从项目根执行。已有 web/dist/index.html 的部署包可跳过。
npm --prefix web run build
```

控制器镜像使用 `npm ci` 和仓库中的锁文件，以项目原来的 `controller/bootstrap.mjs` 注册 vendor 相机元数据与 PTZ；没有安装 Home Assistant 的要求。Compose 固定 Node 和 Caddy 镜像的多平台摘要。首次构建需要访问 Docker 镜像仓和 npm 注册表；使用锁文件而非 NAS 上已有的 `node_modules`。

Node 24 官方支持的 GNU/Linux x64 内核为 4.18 或更新版本，文档也注明更早的内核可能可用。Docker 容器仍使用宿主内核。目标 DSM 7.1.1 的 4.4.180+ 主机已通过固定 Node 镜像的隔离启动、socket 和 `worker_threads` 检查；这是运行时预检，不能代替整套容器、Matter 局域网或外部 TLS 验收。其他旧内核 NAS 可先执行 `./smoke-isolated.sh`，验证完整容器，而无须连入家庭设备或改动反代。若失败，优先选用受支持的宿主内核，不建议通过关闭 Docker 默认 seccomp 来绕过。[Node 24 平台说明](https://github.com/nodejs/node/blob/v24.22.3/BUILDING.md#platform-list)

```sh
cd deploy
cp .env.example .env
# 按需修改 .env 的域名、端口或 Matter 网卡；这里不填写明文密码。
./create-user.sh matterdesk
test -s ../web/dist/index.html && test -s secrets/users.caddy
```

凭据脚本使用 Caddy 交互生成 bcrypt，默认保存为只有所有者可读写的 `secrets/users.caddy`。建议使用密码管理器生成长而唯一的密码。轮换凭据时先生成一个新文件（例如 `./create-user.sh matterdesk secrets/users.next`），将 `.env` 的 `MATTERDESK_USERS_FILE` 改为新路径，然后用 `up -d --force-recreate gateway` 重新挂载，旧会话会断开。不要将 `.env`、凭据文件和 Fabric 数据放入发布包。

## Synology DSM 或已有 HTTPS 反代

以下命令从 `deploy/` 执行。若 NAS 提供独立版 Compose，用 `docker-compose` 替换 `docker compose`。没有 Compose 的较旧 DSM，需要先安装适配其 Docker Engine 的 Compose，或在支持 Compose 项目的 Container Manager 上导入。若宿主端口 5580 或 18081 已被占用，先修改 `.env`；不要接管来源不明的服务。

```sh
docker compose --env-file .env -f compose.yaml -p matterdesk config -q
docker compose --env-file .env -f compose.yaml -p matterdesk up -d --build
docker compose --env-file .env -f compose.yaml -p matterdesk ps
```

在 DSM 的反向代理中添加以下规则（也可在已有 nginx 实现等效路由）：

- 来源为 `HTTPS`、主机名 `matter.linanw.me`、端口 `443`；给该主机名分配匹配的受信任证书。DNS 指向使用该规则的入口。
- 目的地为 `HTTP`、主机名 `127.0.0.1`、端口 `18081`（与 `.env` 一致）。外部只访问 HTTPS，不转发公网的 18081 或 5580。
- 在自订标头使用 DSM 的 WebSocket 预设，保留 `Upgrade` / `Connection`。若手写 nginx，使用 `proxy_http_version 1.1` 并转发 Upgrade/Connection。保留来自浏览器的 `Origin` 与客户端的 `Authorization`；不要在 DSM 重写它们。为长连接选合适的 `proxy_read_timeout`（例如 3600 秒）。不要设为跳过证书验证。

当域名由其他反代或隧道提供时，其 HTTPS/转发终点要能访问 **同一台 NAS** 的 `127.0.0.1:18081`。若前置入口在别的主机上，需要自行配置经认证的内部链路；不建议改 Caddy 的内部监听地址来绕开这一限制。浏览器访问时必须使用配置的域名，访问 NAS IP 会使 Origin 不匹配。

## 由 Caddy 直接提供 HTTPS（可选）

这适用于 TCP 80/443 空闲、DNS 和防火墙已经准备好、允许 Caddy 向 ACME 申请证书的独立 Linux 主机。DSM/nginx 已占用 80/443 时使用上一节，无需该覆盖文件。

```sh
docker compose --env-file .env -f compose.yaml -f compose.direct-tls.yaml -p matterdesk up -d --build
```

该覆盖仅将 Caddy 改为公网监听并自动申请证书；并未修改控制器的 `127.0.0.1` 绑定。[Caddy 自动 HTTPS](https://caddyserver.com/docs/automatic-https)

## Matter 网络与配对

- 使用 Docker `network_mode: host`，让控制器复用 NAS 网卡的 IPv6、路由和 mDNS。不要添加 Compose 的 `ports:`、将 `5580` 绑定到 `0.0.0.0` 或转发到公网。主机模式仍允许 Matter 自身的局域网传输，不能代替 NAS 防火墙。[Docker host network](https://docs.docker.com/engine/network/drivers/host/)
- Homebridge 需启用对应主桥/子桥的 Matter 功能。Homebridge 与 NAS 控制器应在可通 IPv6 和 mDNS 的局域网；注意交换机 VLAN、AP 隔离、UDP 5353 和 Matter 设备的实际网络端口。多网卡的 NAS 可在 `.env` 设 `MATTER_INTERFACE`，使用真实的 `ip link` 接口名。
- 使用 Web 或客户端连接 **NAS 控制器** 后输入 Homebridge Matter 配对码。如已经配对到原控制器，从原控制器开启多管理员配对窗口并生成新的码。NAS 新建 Fabric 不会继承别处的绑定。HomeKit 配对码不适用。
- Fabric 数据保存在 Compose 命名卷 `matterdesk_fabric`（取决于 `-p` 项目名）；升级或重启正常保留。不要执行 `down -v`，并将该卷当成包含私钥的数据备份。同一 Fabric 数据不要同时启动两个控制器。不要复制正在运行的数据库文件作为一致备份；先停控制器。
- 在浏览器和 Harmony 访问 `https://matter.linanw.me/`；Linux 地址填 `wss://matter.linanw.me/ws`，使用相同账号。摄像头媒体连接另受设备协议与网络路径约束；WSS 只负责控制和信令，不保证公共网络可直接连接内网媒体。

## 验证与排查

```sh
# 无账号应收到 401（公开服务需配置正确的 TLS，无需跳过证书检查）。
curl -sSI https://matter.linanw.me/
# curl 安全提示输入密码，预期 200。
curl -fsS -u matterdesk -o /dev/null https://matter.linanw.me/
```

正确账户能打开面板且能连接 `/ws`；匿名不能读取静态页面或设备。若认证成功但连接不上：`401` 检查反代是否转发 Authorization；`403` 核对实际网页 Origin；`502` 检查 NAS 上 `curl -fsS http://127.0.0.1:5580/health`、Compose 日志和上游端口。排障时不要公开发送日志中的设备身份或配对材料。

开发与打包时可运行 `./deploy/smoke-isolated.sh`。该脚本构建镜像、验证两个 Compose 配置并使用临时空 Fabric 与测试凭据；容器共享一个无外部网络的命名空间，测试匿名/错误账号、正确 HTTP/WS 认证、Origin 拦截和 OHF `start_listening`，并验证重新创建容器后 Fabric 身份不变。它不访问家庭 Matter 设备，也不会操作 NAS 或域名。
