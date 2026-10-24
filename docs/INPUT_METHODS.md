# 中文输入与 Linux 输入法

从 MatterDesk 0.6.2 / MatterCamViewer 2.4.2 起，AppImage 显式包含与包内 Qt 匹配的 Fcitx 5、IBus 和 Compose 输入上下文插件及所需动态库。旧稳定版只含 IBus / Compose；桌面选择 `QT_IM_MODULE=fcitx` 时会回退到 Compose，无法正常使用拼音。

输入法由两部分组成：AppImage 内的 Qt 客户端插件，以及桌面会话中运行的输入法服务和中文引擎。使用已配置好的系统输入法，在搜索、名称或房间等普通文本框中切换输入即可。配对码等密码框使用受限的输入提示，不用于检查中文输入能力。

启动器继承桌面的 `QT_IM_MODULE`、`XMODIFIERS` 和 `DBUS_SESSION_BUS_ADDRESS`，不强制改成另一种输入法。Fcitx 桌面通常选择 `fcitx`，IBus 桌面通常选择 `ibus`。无需为了这个修复修改系统配置文件或重新配对设备。

正式 AppImage 的 Qt 基线为 Ubuntu 24.04 的 Qt 6.4。输入法插件必须和包内 Qt 匹配；不要把较新系统 Qt 的插件手工复制进旧 AppImage。当前真实输入法回归覆盖 X11 / xcb。Wayland 桌面使用 XWayland 时也依赖桌面的输入法配置；原生 Wayland 输入协议需另行验证。

## 发布验证

构建环境安装 `fcitx5-frontend-qt6`，打包器要求 Fcitx 5、IBus 和 Compose 三个插件存在，并检查全部 Qt 插件的动态依赖。

测试用独立的 D-Bus 会话、Xvfb 显示、临时 HOME / XDG 目录和本地拼音引擎运行真实 Fcitx 5。它验证实际 Qt 插件加载、拼音预编辑、中文候选提交、焦点切换与组合态回车；既有桌面会话不参与测试。最终 AppImage 解包后再次执行该测试，要求实际加载包内插件。

本地已安装测试依赖时，可以运行：

```bash
python3 tests/run-inputmethod-smoke.py --test-executable build/tst_inputmethod
```

使用 `--appdir /absolute/path/to/extracted/squashfs-root` 可检查解包后的 AppImage。普通离线事件测试与真实输入法测试分别运行，前者通过不能代替后者。

参考：[Fcitx 配置](https://fcitx-im.org/wiki/Setup_Fcitx_5)、[Fcitx 与 Wayland](https://fcitx-im.org/wiki/Using_Fcitx_5_on_Wayland)、[Qt 插件部署兼容性](https://doc.qt.io/qt-6/deployment-plugins.html)。
