# 发布 MatterDesk 与 MatterCamViewer

同一个仓库维护公共协议、播放器和控制器模型，两个应用独立编号、打包和发布。

| 应用 | CMake 基础版本 / 预发布字段 | 本次测试版 Git 标签 | AppImage 文件 |
| --- | --- | --- | --- |
| MatterDesk | `project(MatterDesk VERSION 0.7.0)` / `MATTERDESK_PRERELEASE "beta.1"` | `matterdesk-v0.7.0-beta.1` | `MatterDesk-0.7.0-beta.1-x86_64.AppImage` |
| MatterCamViewer | `MATTERCAMVIEWER_VERSION 2.5.0` / `MATTERCAMVIEWER_PRERELEASE "beta.1"` | `mattercamviewer-v2.5.0-beta.1` | `MatterCamViewer-2.5.0-beta.1-x86_64.AppImage` |

CMake 的数值版本保持 `major.minor.patch`；对应 `*_PRERELEASE` 只接受空字符串或 `alpha.N`、`beta.N`、`rc.N`（N 从 1 开始）。CMake 将完整版本传给应用的 `--version`，打包脚本读取相同字段，因此应用、镜像文件名与标签中的版本必须完全一致。正式发布时将对应的预发布字段改为 `""`，保留 `matterdesk-vX.Y.Z` / `mattercamviewer-vX.Y.Z` 标签规范。不要将预发布仅标在文件名或 Release 标题中。

## 验证改动

推送 `main` 或向 `main` 提交 PR 会运行 `Build and test`：安装锁定的控制器依赖，构建两个应用，执行全部离线测试，并检查启动入口、安装文件和图标。合成摄像头、随机回环端口与隔离的用户数据目录用于测试；不会访问真实家庭控制器或云账户。

AppImage 基线为 Ubuntu 24.04 x86_64。发布流程另外捆绑对应 Qt / WebEngine、Node.js 22、控制器生产依赖及中文字体。工具的下载地址与 SHA-256 固定在 `scripts/build-appimage.py`，实际依赖和许可证清单随镜像保存。

## 先验证 AppImage

在 GitHub **Actions → Release Linux applications → Run workflow** 中选择需要验证的分支（可以是功能分支）和产品。手动运行会构建两个应用并执行完整离线回归，再打包、烟测所选产品；不会创建标签或 GitHub Release。

也可以指定已推送的分支与产品运行：

```bash
gh workflow run release.yml --ref <branch> --field product=matterdesk
gh workflow run release.yml --ref <branch> --field product=mattercamviewer
```

成功后在该运行的 **Summary → Artifacts** 下载候选包；运行摘要也提供直接链接。产物保留 14 天，包含 `.AppImage`、对应 `.sha256`、中英文发布说明和 `release-info.json`（完整版本、实际源码提交、SHA-256 与烟测状态）。两个产品需要分别运行。手动候选包中的源码链接指向实际构建提交，之后即使分支继续更新也可追溯。Actions 下载通常需要登录 GitHub；需要长期公开下载时使用下面的标签预发布流程。

下载并解压 Actions 的产物压缩包后，在镜像与校验文件所在目录执行：

```bash
sha256sum -c MatterDesk-0.7.0-beta.1-x86_64.AppImage.sha256
chmod +x MatterDesk-0.7.0-beta.1-x86_64.AppImage
./MatterDesk-0.7.0-beta.1-x86_64.AppImage
```

MatterCamViewer 使用它自己的文件名。没有可用 FUSE 时，可用 `APPIMAGE_EXTRACT_AND_RUN=1 ./<文件名>.AppImage`。这仍需主机允许 WebEngine renderer 的用户命名空间。

构建脚本会检查包内共享库与 WebEngine 资源，解包最终 AppImage 并验证：

- 应用名称与版本输出。
- 使用随机、未监听的回环端口启动界面并自动退出。
- 使用包内字体、Node 与控制器入口。
- 使用包内 WebEngine 启动真实 renderer，并解码合成横屏、竖屏视频。

应用沙箱默认保持开启。GitHub 临时 runner 的 AppArmor 例外仅用于测试路径，并在任务结束时移除；它们不进入 AppImage。流程不会用跳过测试或关闭沙箱来生成候选包。离线回归与烟测不代表真实家庭设备、云写入或视频流已完成物理验证。

本地也可以在安装构建依赖后运行：

```bash
./scripts/setup-controller.sh
./scripts/build.sh
ctest --test-dir build --output-on-failure
python3 scripts/build-appimage.py --product mattercamviewer \
  --build-dir build --output-dir dist
```

本地生成的镜像以该机器的 Qt 和系统库为基础。正式 Release 使用 GitHub 的 Ubuntu 24.04 构建环境。

## 发布预发布或正式版本

先更新对应版本字段与说明，推送并确认 CI 成功。再为已验证的提交建立对应标签，例如：

```bash
git tag -a matterdesk-v0.7.0-beta.1 -m 'MatterDesk 0.7.0-beta.1'
git tag -a mattercamviewer-v2.5.0-beta.1 -m 'MatterCamViewer 2.5.0-beta.1'
git push origin matterdesk-v0.7.0-beta.1 mattercamviewer-v2.5.0-beta.1
```

每个标签独立触发 `Release Linux applications`。流程先核对标签与产品完整版本，构建、测试并生成 AppImage，全部检查成功且保存候选产物后才发布镜像与 `.sha256` 校验文件。GitHub 同时生成该标签的源码归档。发布权限只授予该任务的临时仓库 token，不需要额外保存个人访问令牌。

带预发布字段的标签使用 `gh release create --prerelease --latest=false`，页面明确显示 **Pre-release**，不会替换现有正式版的 latest。正式 MatterDesk 发布仍设为仓库 latest；正式 MatterCamViewer 发布通过自己的标签链接访问，不占用仓库 latest。

已发布标签应保留原提交。修复后递增对应应用版本并建立新标签，旧版本可继续下载。

## 用户数据

镜像与源码归档不包含 Fabric、配对密钥、云账户、真实摄像头图片或用户配置。应用升级继续使用镜像外的现有控制器数据目录：

```text
${XDG_DATA_HOME:-$HOME/.local/share}/matterdesk/controller
```

`MATTERDESK_DATA_DIR` 可选择既有目录。不要在打包、升级或版本切换时删除它。
