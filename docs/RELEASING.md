# 发布 MatterDesk

**一个 MatterDesk 标签，两个客户端，同一个版本。** `matterdesk-vX.Y.Z` 拉起独立的 Linux、HarmonyOS 与 NAS/Web 任务。三个任务成功且各自的校验文件通过检查后，才创建一个 GitHub Release。

| 附件 | 内容 |
| --- | --- |
| `MatterDesk-X.Y.Z-x86_64.AppImage` | Linux Qt 客户端；包含本地控制器和运行依赖 |
| `MatterDesk-X.Y.Z-harmony.hap` | HarmonyOS 6 / API 26 客户端，使用保护环境的正式签名 |
| `MatterServer-X.Y.Z-nas.zip` | Docker Compose、固定版本控制器和预构建 Web App；没有凭据或 Fabric 数据 |

每个附件都有独立的 `.sha256`。HarmonyOS 的签名和发布渠道决定哪些真机允许安装；GitHub Release 不会自动提交华为 AppGallery 审核。MatterCamViewer 是另一款 Linux 极简摄像头工具，保留自己的 `mattercamviewer-vX.Y.Z` 标签与 AppImage，发布它不会改变 MatterDesk 两端的版本。

## 版本与预发布

- 在 `CMakeLists.txt` 更新 `project(MatterDesk VERSION …)` / `MATTERDESK_PRERELEASE`，并将 `MATTERDESK_HARMONY_VERSION_CODE` 递增。Harmony 构建使用这个整数（用于系统升级判断）及与 Linux 一致的完整版本；原来独立 Harmony 项目的基线为 `1000002`。
- 将预发布字段设为空字符串表示正式版，或设为 `alpha.N`、`beta.N`、`rc.N`。例如本次整合候选是 `0.7.0-beta.2`，标签为 `matterdesk-v0.7.0-beta.2`。预发布不替换正式版的 latest。
- 合并到 `main` 并验证 CI 后，给已经通过的提交创建新标签；不要移动已发布标签。正式 MatterDesk 必须来自 `main` 历史。

## 配置 HarmonyOS 构建与签名

1. 为仓库准备一台受控 Linux x86_64 runner，标签为 `harmony-api26`。安装 [DevEco Studio CLI / HarmonyOS SDK API 26](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/ide-command-line-building-app/)、Node/OHPM、Hvigor、JDK、Python 3 和 OpenSSL。默认 DevEco 路径 `/opt/devecostudio`；不同路径通过 runner 的 `DEVECO_STUDIO_HOME`、`DEVECO_SDK_HOME`、`JAVA_HOME` 和 `PATH` 配置。此 runner 只用于审核过的发布标签；公开 PR 在 GitHub 托管 runner 执行，无签名权限。
2. 建立 GitHub Environment `harmony-release`，限制允许发布的 `matterdesk-v*` 标签和发布人员。在环境的 Secrets 添加 `HARMONY_KEYSTORE_B64`（`.p12`）、`HARMONY_CERT_B64`（证书链）、`HARMONY_PROFILE_B64`（`.p7b`）、`HARMONY_KEY_ALIAS`、`HARMONY_KEY_PASSWORD`、`HARMONY_KEYSTORE_PASSWORD`。前三项填各文件的 base64。使用对应 `com.matterdesk.harmony` 的 release profile 和合法发行签名，不使用个人调试证书；通过 GitHub Secret 设置，避免复制到命令行、聊天或源码。
3. 检查签名和 runner 都已配置，再设置仓库变量 `HARMONY_RELEASE_READY=true`。缺少前置配置时，发布流程直接报错。签名步骤不上传密钥、证书资料、profile 或未签名中间件，只上传经验证的 HAP 与校验文件。

## 发布前验证

向 `main` 提交 PR，运行 Linux 构建、离线回归、真实 X11 窗口位置、真实 Fcitx 拼音、Web 用例与隔离 Compose 烟测。可在 GitHub Actions 手动执行 **Release MatterDesk platforms** 选择产品并生成托管 Linux 候选。选择 MatterDesk 时也会生成 NAS 部署包；手动候选不使用签名凭据，不创建 GitHub Release。Harmony 的无签名候选可本地构建：

```bash
MATTERDESK_HARMONY_VERSION=0.7.0-beta.2 \
MATTERDESK_HARMONY_VERSION_CODE=1000003 \
MATTERDESK_HARMONY_BUILD_MODE=release \
  bash apps/harmony/build-unsigned.sh dist/harmony-check
python3 scripts/sign-harmony-release.py --check-only \
  --hap dist/harmony-check/MatterDesk-unsigned.hap \
  --version 0.7.0-beta.2 --version-code 1000003 --output-dir dist
python3 scripts/package-nas.py --output-dir dist
```

AppImage 发布使用 Ubuntu 24.04 x86_64 环境，并解包检查中文/符号字体、Fcitx、摄像头播放器和控制器。不绕过沙箱或证书验证。各套测试用临时目录和合成数据，不接触真实家庭设备；Harmony 真机和真实摄像头仍需安装后验收。

确认前置条件后，从已验证的 `main` 提交创建版本标签，例如：

```bash
git tag -a matterdesk-v0.7.0-beta.2 -m 'MatterDesk 0.7.0-beta.2'
git push origin matterdesk-v0.7.0-beta.2
```

确认 GitHub Release 同时列出三类附件和校验文件。在安静时段独立更新 NAS 服务；发布工作流不连接 NAS，也不替换生产控制器。原 Fabric 始终留在宿主卷或旧控制器数据目录。更新与备份见 [NAS 部署说明](../deploy/README.md)。
