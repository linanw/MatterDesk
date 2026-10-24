# 发布 MatterDesk 与 MatterCamViewer

同一个仓库维护公共协议、播放器和控制器模型，两个应用独立编号、打包和发布。

| 应用 | CMake 版本字段 | Git 标签 | AppImage 文件 |
| --- | --- | --- | --- |
| MatterDesk | `project(MatterDesk VERSION …)` | `matterdesk-v0.6.2` | `MatterDesk-0.6.2-x86_64.AppImage` |
| MatterCamViewer | `MATTERCAMVIEWER_VERSION` | `mattercamviewer-v2.4.2` | `MatterCamViewer-2.4.2-x86_64.AppImage` |

## 验证改动

推送 `main` 或向 `main` 提交 PR 会运行 `Build and test`：安装锁定的控制器依赖，构建两个应用，执行全部离线测试，并检查启动入口、安装文件和图标。合成摄像头、随机回环端口与隔离的用户数据目录用于测试；不会访问真实家庭控制器或云账户。

AppImage 基线为 Ubuntu 24.04 x86_64。发布流程另外捆绑对应 Qt / WebEngine、Node.js 22、控制器生产依赖及中文、拉丁与符号字体。工具的下载地址与 SHA-256 固定在 `scripts/build-appimage.py`，实际依赖和许可证清单随镜像保存。

## 先验证 AppImage

在 GitHub **Actions → Release Linux applications → Run workflow** 中选择 `main` 和产品。手动运行只构建与测试，不创建 Release。

构建脚本会检查包内共享库与 WebEngine 资源，解包最终 AppImage 并验证：

- 应用名称与版本输出。
- 使用随机、未监听的回环端口启动界面并自动退出。
- 使用包内中文、拉丁与符号字体、Node 与控制器入口。
- 使用实际 Qt 字形排版检查界面符号、中文与单位；缺字或使用包外字形时发布失败。
- 使用包内 WebEngine 启动真实 renderer，并解码合成横屏、竖屏视频。

应用沙箱默认保持开启。GitHub 临时 runner 的 AppArmor 例外仅用于测试路径，并在任务结束时移除；它们不进入 AppImage。

本地也可以在安装构建依赖后运行：

```bash
./scripts/setup-controller.sh
./scripts/build.sh
ctest --test-dir build --output-on-failure
python3 scripts/build-appimage.py --product mattercamviewer \
  --build-dir build --output-dir dist
```

本地生成的镜像以该机器的 Qt 和系统库为基础。正式 Release 使用 GitHub 的 Ubuntu 24.04 构建环境。

## 发布版本

先更新对应版本字段与说明，推送并确认 CI 成功。再为已验证的提交建立对应标签，例如：

```bash
git tag -a matterdesk-v0.6.2 -m 'MatterDesk 0.6.2'
git tag -a mattercamviewer-v2.4.2 -m 'MatterCamViewer 2.4.2'
git push origin matterdesk-v0.6.2 mattercamviewer-v2.4.2
```

每个标签独立触发 `Release Linux applications`。流程核对标签与产品版本，构建、测试并生成 AppImage，全部检查成功后才发布镜像与 `.sha256` 校验文件。GitHub 同时生成该标签的源码归档。发布权限只授予该任务的临时仓库 token，不需要额外保存个人访问令牌。

MatterDesk 的发布设为仓库 latest；MatterCamViewer 通过自己的标签链接访问，因此两个产品的版本号不会混淆。

已发布标签应保留原提交。修复后递增对应应用版本并建立新标签，旧版本可继续下载。

## 用户数据

镜像与源码归档不包含 Fabric、配对密钥、云账户、真实摄像头图片或用户配置。应用升级继续使用镜像外的现有控制器数据目录：

```text
${XDG_DATA_HOME:-$HOME/.local/share}/matterdesk/controller
```

`MATTERDESK_DATA_DIR` 可选择既有目录。不要在打包、升级或版本切换时删除它。
