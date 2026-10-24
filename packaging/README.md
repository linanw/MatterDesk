# Linux packaging

The root CMake project builds and installs both `matterdesk` and `mattercamviewer`. Desktop entries use the matching executable and icon names. Install with:

```bash
cmake --install build --prefix "$HOME/.local"
```

Installed desktop entries connect to an existing local controller. CMake does not install Node modules or the repository launch scripts; run `scripts/start-controller.sh` separately or use the launchers from a source / Release directory for automatic controller startup. Both applications retain the shared controller data directory `${XDG_DATA_HOME:-$HOME/.local/share}/matterdesk/controller`, unless `MATTERDESK_DATA_DIR` is set.

Source archives must include the CMake project, application sources, tests, scripts, desktop integration, documentation, controller manifests and vendor models. Exclude `bin/`, build trees, `controller/node_modules/`, runtime storage, keys, account data, local logs, internal QA receipts and private camera images.

## Release assets

Publish one product per AppImage: `MatterDesk-VERSION-x86_64.AppImage` and `MatterCamViewer-VERSION-x86_64.AppImage`. Each product has an independent version tag (`matterdesk-vVERSION` / `mattercamviewer-vVERSION`) in the shared source repository. Upload AppImages directly as Release assets with SHA-256 checksums; do not wrap them in another archive. A lightweight tar package and GitHub source archives remain optional alternatives.

AppImages contain the selected application, Qt / WebEngine, official Node.js 22 and the clean CI-installed, lockfile-pinned production controller dependencies. They require no system Qt / Node installation. They never include a developer's installed modules, real Fabric storage or account credentials. Lightweight tar packages keep the existing system-Qt model; their controller dependencies are installed by `scripts/setup-controller.sh`.

The build baseline is Ubuntu 24.04 x86_64. AppImage bundling does not remove host kernel, ABI, display / graphics-driver or audio requirements, and does not guarantee compatibility with Ubuntu 22.04 or older distributions. When a FUSE 2 runtime dependency is absent on Ubuntu 24.04, the package is `libfuse2t64`; `--appimage-extract-and-run` is an alternative to mounting. It does not bypass Chromium sandbox policy. See the [AppImage FUSE guide](https://docs.appimage.org/user-guide/troubleshooting/fuse.html).

## AppDir and controller layout

AppRun uses paths relative to the mounted AppDir and forwards arguments to the normal product launcher. Keep `usr/bin/{APPLICATION,node}`, `usr/scripts/` and `usr/controller/` aligned with the existing launchers' project-relative lookups. Prepend the bundled `usr/bin` to `PATH`; do not attempt `npm ci` at runtime inside the read-only image. Fabric storage and logs retain their existing external XDG paths. No AppRun changes global kernel / AppArmor settings or disables the WebEngine sandbox.

Use the verified linuxdeploy and linuxdeploy Qt plugin versions pinned by the build script, with checked hashes. Deploy QtWebEngineProcess, Qt plugins, the required `.pak` / ICU / V8 resources and locales, and retain generated `qt.conf`. A successful `--version` is insufficient: release checks must start a real bundled WebEngine renderer with synthetic local content and verify the packaged controller on an isolated non-production endpoint. Qt's [deployment instructions](https://doc.qt.io/QT-6/qtwebengine-deploying.html) describe the required process and resources.

If the hosted runner blocks unprivileged user namespaces, `scripts/prepare-ci-webengine-sandbox.sh` first checks `unshare -Ur true` and only then loads temporary AppArmor userns exceptions for the build test executables and explicitly selected WebEngine helpers. It is CI-only, omitted from product packages, and unloaded at the end of the job. It does not disable the sandbox or change a global sysctl.

## Bundled notices

Collect the notices for the actual payload into `usr/share/doc/licenses/`: distribution copyright files under `system/<package>/`, the official Node `LICENSE` under `node/`, npm license / copying / notice files under `npm/`, and font notices under `fonts/`. `inventory.json` records deployed versions and license metadata, including missing-file markers. Do not describe the general dependency table as a substitute for the bundled components' actual notices. Include the project's current license status without choosing a project license on the owner's behalf.

Record architecture, distribution / ABI, Qt build, Node build and tool versions in Release notes. The source build and lightweight tar executables dynamically link system Qt; the AppImage supplies those Qt libraries. Refer to [third-party notices](../THIRD_PARTY_NOTICES.md) and recommend a source build where the target platform differs.
