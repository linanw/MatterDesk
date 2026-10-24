#!/usr/bin/env python3
"""Build and smoke-test a self-contained Qt/WebEngine/Node application image."""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile


TOOLS = {
    "linuxdeploy-x86_64.AppImage": (
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage",
        "c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"),
    "linuxdeploy-plugin-qt-x86_64.AppImage": (
        "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage",
        "15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724"),
    "appimagetool-x86_64.AppImage": (
        "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage",
        "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"),
    "runtime-x86_64": (
        "https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64",
        "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"),
    "node-v22.22.1-linux-x64.tar.xz": (
        "https://nodejs.org/dist/v22.22.1/node-v22.22.1-linux-x64.tar.xz",
        "9a6bc82f9b491279147219f6a18add1e18424dce90d41d2a5fcd69d4924ba3aa"),
}
RUNTIME_NOTICES = {
    "runtime-LICENSE": ("https://raw.githubusercontent.com/AppImage/type2-runtime/20251108/LICENSE", "aa154fc9070614bbe7921f89db11efd1dba7a1f3a41685958110e2230f9c0ca1"),
    "musl-COPYRIGHT": ("https://git.musl-libc.org/cgit/musl/plain/COPYRIGHT?h=v1.2.5", "f9bc4423732350eb0b3f7ed7e91d530298476f8fec0c6c427a1c04ade22655af"),
    "libfuse-LICENSE": ("https://raw.githubusercontent.com/libfuse/libfuse/fuse-3.15.0/LICENSE", "b8832d9caaa075bbbd2aef24efa09f8b7ab66a832812d88c602da0c7b4397fad"),
    "libfuse-LGPL2.txt": ("https://raw.githubusercontent.com/libfuse/libfuse/fuse-3.15.0/LGPL2.txt", "dc626520dcd53a22f727af3ee42c770e56c97a64fe3adb063799d8ab032fe551"),
    "libfuse-AUTHORS": ("https://raw.githubusercontent.com/libfuse/libfuse/fuse-3.15.0/AUTHORS", "fafe891ac0d23452355fe41d5e5538904fe5324b44cc03f519063e83deb9b0f8"),
    "squashfuse-LICENSE": ("https://raw.githubusercontent.com/vasi/squashfuse/0.5.2/LICENSE", "9e909cc8a8ba27b1a649c964cf9eda37de911f3138c2b64e6d2f01703904ac13"),
    "zstd-LICENSE": ("https://raw.githubusercontent.com/facebook/zstd/v1.5.6/LICENSE", "7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8"),
    "zlib-LICENSE": ("https://raw.githubusercontent.com/madler/zlib/v1.3.1/LICENSE", "845efc77857d485d91fb3e0b884aaa929368c717ae8186b66fe1ed2495753243"),
    "mimalloc-LICENSE": ("https://raw.githubusercontent.com/microsoft/mimalloc/v2.1.7/LICENSE", "19c99805e7a44a34b297a75d1edea9985e300066dfc024d5c99d4236d4573b5d"),
    "type2-runtime-20251108.tar.gz": ("https://github.com/AppImage/type2-runtime/archive/refs/tags/20251108.tar.gz", "4c4f6df4647c9f01f871d7edd3716d8aeeffda9d22ffbebe3fccd95a6ab52c95"),
    "libfuse-fuse-3.15.0.tar.gz": ("https://github.com/libfuse/libfuse/archive/refs/tags/fuse-3.15.0.tar.gz", "1e79f1dd60eaf79645c21411c4a7028c97b5e745ea677ac5bda747c90497a785"),
}


def run(command, **kwargs):
    kwargs.setdefault("stdout", sys.stderr)
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


def download(cache: Path, name: str) -> Path:
    url, expected = {**TOOLS, **RUNTIME_NOTICES}[name]
    destination = cache / name
    if not destination.is_file() or hashlib.sha256(destination.read_bytes()).hexdigest() != expected:
        partial = cache / (name + ".partial")
        run(["curl", "--fail", "--location", "--retry", "3", "--output", partial, url])
        if hashlib.sha256(partial.read_bytes()).hexdigest() != expected:
            partial.unlink()
            raise ValueError(f"Official tool checksum mismatch: {name}")
        os.replace(partial, destination)
    destination.chmod(0o755)
    return destination


def write(path: Path, content: str, executable=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    path.chmod(0o755 if executable else 0o644)


def is_elf(path: Path) -> bool:
    if not path.is_file() or path.is_symlink():
        return False
    with path.open("rb") as stream:
        return stream.read(4) == b"\x7fELF"


def node_modules(source: Path, destination: Path):
    if not (source / "matter-server/dist/esm/bin.js").is_file():
        raise ValueError("Run npm ci --prefix controller before packaging")
    shutil.copytree(source, destination, symlinks=True)
    # node-gyp-build selects the platform/libc at runtime; other prebuilds are
    # unrelated architectures and must not be scanned as x86_64 dependencies.
    for directory in list(destination.rglob("prebuilds")):
        for platform in directory.iterdir():
            if platform.name != "linux-x64":
                shutil.rmtree(platform) if platform.is_dir() else platform.unlink()
    for native in list(destination.rglob("*.node")):
        if "musl" in native.name:
            native.unlink()
    root = destination.resolve()
    for link in destination.rglob("*"):
        if link.is_symlink():
            target = os.readlink(link)
            if os.path.isabs(target) or not link.resolve().is_relative_to(root):
                raise ValueError("A Node dependency symlink escapes the bundled module tree")


def collect_licenses(appdir: Path, node_license: Path, qt_plugins: Path, qt_process: Path):
    licenses = appdir / "usr/share/doc/licenses"
    licenses.mkdir(parents=True, exist_ok=True)
    (licenses / "node").mkdir(exist_ok=True)
    shutil.copyfile(node_license, licenses / "node/LICENSE")
    inventory = {"node": "22.22.1", "systemPackages": {}, "npmPackages": []}
    lookup = {}
    cache = subprocess.check_output(["ldconfig", "-p"], text=True)
    for line in cache.splitlines():
        match = re.match(r"\s*(\S+) .* => (.+)", line)
        if match:
            lookup.setdefault(match.group(1), Path(match.group(2)))
    for plugin in qt_plugins.rglob("*.so"):
        lookup[plugin.name] = plugin
    # Some deployed backends (for example libproxy's private backend) have no
    # ldconfig entry, but still belong to a distribution package.
    for library in Path("/usr/lib").rglob("*.so*"):
        if library.is_file():
            lookup.setdefault(library.name, library)
    lookup["QtWebEngineProcess"] = qt_process
    system_packages = set()
    for binary in appdir.rglob("*"):
        if not is_elf(binary) or "node_modules" in binary.parts or binary.name in {"node", "matterdesk", "mattercamviewer"}:
            continue
        original = lookup.get(binary.name)
        if original is None:
            raise ValueError(f"Cannot identify bundled library license: {binary.name}")
        ownership = None
        for candidate in {str(original), str(original.resolve()), str(original).replace("/usr/lib/", "/lib/", 1)}:
            found = subprocess.run(["dpkg-query", "-S", candidate], text=True, capture_output=True)
            if found.returncode == 0:
                ownership = found.stdout.split(": /", 1)[0]
                break
        if not ownership:
            raise ValueError(f"Cannot find distribution copyright: {binary.name}")
        system_packages.update(ownership.split(", "))
    system_packages.add("fonts-noto-cjk")
    for package in sorted(system_packages):
        short = package.split(":", 1)[0]
        copyright_file = Path("/usr/share/doc") / short / "copyright"
        if not copyright_file.is_file():
            raise ValueError(f"Missing distribution copyright for {short}")
        target = licenses / "system" / short
        target.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(copyright_file, target / "copyright")
        version = subprocess.check_output(["dpkg-query", "-W", "-f=${Version}", package], text=True)
        inventory["systemPackages"][package] = version
    modules = appdir / "usr/controller/node_modules"
    for metadata in sorted(modules.rglob("package.json")):
        parent = metadata.parent
        if not (parent.parent.name == "node_modules" or
                (parent.parent.name.startswith("@") and parent.parent.parent.name == "node_modules")):
            continue
        try:
            package = json.loads(metadata.read_text())
        except (ValueError, UnicodeDecodeError):
            continue
        name = package.get("name")
        if not name or not package.get("version"):
            continue
        found = []
        for child in metadata.parent.iterdir():
            if child.is_file() and re.match(r"^(?:licen[cs]e|notice|copying|copyright)(?:[._-].*)?$", child.name, re.I):
                destination = licenses / "npm" / str(name).replace("/", "_") / child.name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(child, destination)
                found.append(child.name)
        inventory["npmPackages"].append({"name": name, "version": package["version"],
                                        "license": package.get("license"), "noticeFiles": found})
    write(licenses / "inventory.json", json.dumps(inventory, ensure_ascii=False, indent=2) + "\n")
    for license_file in Path("/usr/share/common-licenses").glob("*"):
        if license_file.is_file():
            destination = licenses / "common-licenses" / license_file.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(license_file, destination)


def check_runtime(appdir: Path, product: str):
    env = dict(os.environ, LD_LIBRARY_PATH=str(appdir / "usr/lib"))
    for executable in [appdir / "usr/bin" / product, appdir / "usr/bin/node",
                       appdir / "usr/libexec/QtWebEngineProcess",
                       *appdir.glob("usr/plugins/platforms/*.so"),
                       *appdir.glob("usr/controller/node_modules/**/*.node")]:
        result = subprocess.run(["ldd", str(executable)], env=env, text=True, capture_output=True)
        if result.returncode or "not found" in result.stdout:
            raise ValueError(f"Unresolved packaged runtime dependency: {executable.name}\n{result.stdout}{result.stderr}")
    for relative in ["usr/libexec/QtWebEngineProcess", "usr/resources/qtwebengine_resources.pak",
                     "usr/resources/qtwebengine_resources_100p.pak", "usr/resources/qtwebengine_resources_200p.pak",
                     "usr/translations/qtwebengine_locales/en-US.pak", "usr/plugins/platforms/libqxcb.so",
                     "usr/bin/qt.conf", "usr/libexec/qt.conf"]:
        if not (appdir / relative).is_file():
            raise ValueError(f"Missing required WebEngine/Qt deployment resource: {relative}")


def smoke(image: Path, build_dir: Path, work: Path, source: Path, product: str, expected_version: str):
    extracted = work / "smoke"
    extracted.mkdir()
    run([image, "--appimage-extract"], cwd=extracted, stdout=subprocess.DEVNULL)
    appdir = extracted / "squashfs-root"
    if os.environ.get("GITHUB_ACTIONS") == "true":
        run([source / "scripts/prepare-ci-webengine-sandbox.sh", build_dir,
             appdir / "usr/libexec/QtWebEngineProcess"])
    isolated = work / "isolated-home"
    for relative in ["config", "cache", "data", "state", "runtime"]:
        (isolated / relative).mkdir(parents=True, mode=0o700)
    env = dict(os.environ, HOME=str(isolated), XDG_CONFIG_HOME=str(isolated / "config"),
               XDG_CACHE_HOME=str(isolated / "cache"), XDG_DATA_HOME=str(isolated / "data"),
               XDG_STATE_HOME=str(isolated / "state"), XDG_RUNTIME_DIR=str(isolated / "runtime"),
               QT_QPA_PLATFORM="offscreen", QT_SCALE_FACTOR="1", QTWEBENGINE_CHROMIUM_FLAGS="--disable-gpu")
    env.pop("QTWEBENGINE_DISABLE_SANDBOX", None)
    version_output = subprocess.check_output([appdir / "AppRun", "--version"], env=env, text=True, timeout=30)
    expected_name = "MatterDesk" if product == "matterdesk" else "MatterCamViewer"
    if version_output.strip() != f"{expected_name} {expected_version}":
        raise ValueError("The packaged executable version does not match the release tag")
    print(version_output.strip(), file=sys.stderr)
    with socket.socket() as reserved:
        reserved.bind(("127.0.0.1", 0))  # Reserved, deliberately not listening.
        port = reserved.getsockname()[1]
        assert port != 5580
        run([appdir / "AppRun", "--server", f"ws://127.0.0.1:{port}/ws", "--quit-after", "1200"], env=env, timeout=30)
    shell = 'export APPDIR="$1"; source "$APPDIR/usr/scripts/appimage-env.sh"; shift; exec "$@"'
    matched_font = subprocess.check_output(
        ["bash", "-c", shell, "smoke", appdir, "fc-match", "-f", "%{file}", "Noto Sans CJK SC"],
        env=env, text=True)
    if not Path(matched_font).resolve().is_relative_to(appdir / "usr/share/fonts"):
        raise ValueError("The packaged font configuration did not select the bundled CJK font")
    run(["bash", "-c", shell, "smoke", appdir, appdir / "usr/bin/node",
         appdir / "usr/controller/bootstrap.mjs", "--help"], env=env, timeout=30)
    if (isolated / "data/matterdesk/controller").exists():
        raise ValueError("An offline smoke check unexpectedly created controller Fabric storage")
    # Same packaged environment as AppRun; the existing test decodes synthetic
    # landscape and portrait frames in an actual WebEngine renderer process.
    run(["bash", "-c", shell, "smoke", appdir,
         build_dir / "tst_mainpanelcamera", "videoFitsEntirePanelForPortraitAndLandscapeSources"], env=env, timeout=90)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--product", required=True, choices=["matterdesk", "mattercamviewer"])
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--tools-dir", type=Path)
    parser.add_argument("--skip-smoke", action="store_true", help="Local diagnosis only; never used by release CI")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent.parent
    spec = importlib.util.spec_from_file_location("release_package", source / "scripts/package-release.py")
    package = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(package)
    info = package.PRODUCTS[args.product]
    version = re.search(info["version_pattern"], (source / "CMakeLists.txt").read_text()).group(1)
    tag = f"{args.product}-v{version}"
    if os.environ.get("GITHUB_REF_TYPE") == "tag" and os.environ.get("GITHUB_REF_NAME") != tag:
        raise ValueError("The tag does not match this product's CMake version")
    build_dir, output = args.build_dir.resolve(), args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cache = args.tools_dir.resolve() if args.tools_dir else output / ".tools"
    cache.mkdir(parents=True, exist_ok=True)
    tools = {name: download(cache, name) for name in TOOLS}
    runtime_notices = {name: download(cache, name) for name in RUNTIME_NOTICES}
    qmake = shutil.which("qmake6")
    if not qmake:
        raise ValueError("qmake6 is required to deploy the exact build's Qt runtime")
    query = lambda key: Path(subprocess.check_output([qmake, "-query", key], text=True).strip())
    work = output / f".appimage-{args.product}"
    if work.exists():
        shutil.rmtree(work)
    appdir = work / "AppDir"
    appdir.mkdir(parents=True)
    usr = appdir / "usr"
    files = set(package.COMMON_FILES)
    files.update(f"scripts/{name}" for name in [info["launcher"], info["dev_launcher"], *info["extra_launchers"]])
    files.add(f"packaging/{args.product}.svg")
    if (source / "LICENSE").is_file():
        files.add("LICENSE")
    for relative in sorted(files):
        package.copy_file(source / relative, usr / relative, relative.startswith("scripts/"))
    app_binary = usr / "bin" / args.product
    package.copy_file(build_dir / args.product, app_binary, True)
    package.strip_binary(app_binary)
    archive = tools["node-v22.22.1-linux-x64.tar.xz"]
    node_tree = work / "node"
    node_tree.mkdir()
    with tarfile.open(archive) as tar:
        tar.extractall(node_tree, filter="data")
    node_root = node_tree / "node-v22.22.1-linux-x64"
    package.copy_file(node_root / "bin/node", usr / "bin/node", True)
    node_modules(source / "controller/node_modules", usr / "controller/node_modules")
    font = Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc")
    package.copy_file(font, usr / "share/fonts/NotoSansCJK-Regular.ttc")
    write(usr / "etc/fonts/fonts.conf", '<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "fonts.dtd"><fontconfig><dir prefix="relative">../../share/fonts</dir><cachedir prefix="xdg">fontconfig</cachedir></fontconfig>\n')
    write(usr / "scripts/appimage-env.sh", '''export PATH="$APPDIR/usr/bin:$PATH"
export LD_LIBRARY_PATH="$APPDIR/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$APPDIR/usr/plugins"
export QTWEBENGINEPROCESS_PATH="$APPDIR/usr/libexec/QtWebEngineProcess"
export QTWEBENGINE_RESOURCES_PATH="$APPDIR/usr/resources"
export QTWEBENGINE_LOCALES_PATH="$APPDIR/usr/translations/qtwebengine_locales"
export FONTCONFIG_FILE="$APPDIR/usr/etc/fonts/fonts.conf"
''')
    apprun = work / "AppRun"
    write(apprun, f'''#!/usr/bin/env bash
set -euo pipefail
export APPDIR="$(cd -- "$(dirname -- "${{BASH_SOURCE[0]}}")" && pwd)"
source "$APPDIR/usr/scripts/appimage-env.sh"
exec "$APPDIR/usr/scripts/{info['launcher']}" "$@"
''', True)
    env = dict(os.environ, APPIMAGE_EXTRACT_AND_RUN="1", QMAKE=qmake,
               EXTRA_QT_MODULES="svg", EXTRA_PLATFORM_PLUGINS="libqoffscreen.so",
               PATH=str(cache) + os.pathsep + os.environ["PATH"], ARCH="x86_64")
    command = [tools["linuxdeploy-x86_64.AppImage"], "--appdir", appdir,
               "--executable", app_binary, "--executable", usr / "bin/node",
               "--desktop-file", source / f"packaging/{args.product}.desktop",
               "--icon-file", source / f"packaging/{args.product}.svg",
               "--custom-apprun", apprun, "--plugin", "qt"]
    # NSS loads these providers with dlopen; they do not all appear in ldd's
    # direct dependency graph for WebEngineCore.
    for name in ["libfreebl3.so", "libfreeblpriv3.so", "libnssckbi.so", "libnssdbm3.so", "libsoftokn3.so"]:
        provider = query("QT_INSTALL_LIBS") / name
        if provider.is_file():
            command += ["--library", provider]
    for native in usr.glob("controller/node_modules/**/*.node"):
        command += ["--deploy-deps-only", native]
    run(command, env=env)
    process = query("QT_INSTALL_LIBEXECS") / "QtWebEngineProcess"
    for name in ["icudtl.dat", "v8_context_snapshot.bin"]:
        if (query("QT_INSTALL_DATA") / "resources" / name).is_file() and not (usr / "resources" / name).is_file():
            raise ValueError(f"Qt plugin omitted the installed WebEngine resource {name}")
    check_runtime(appdir, args.product)
    collect_licenses(appdir, node_root / "LICENSE", query("QT_INSTALL_PLUGINS"), process)
    runtime_docs = usr / "share/doc/licenses/appimage-runtime"
    for name, path in runtime_notices.items():
        destination = runtime_docs / ("source" if name.endswith(".tar.gz") else "notices") / name
        package.copy_file(path, destination)
    write(runtime_docs / "README.md", '''# AppImage runtime

The image embeds the unmodified official type2-runtime 20251108. Its source and
the LGPL libfuse 3.15.0 source are provided in `source/`; the runtime source includes
the exact libfuse patch and its build Makefile. `notices/` retains all identified
runtime dependency notices. Other build tools are not shipped inside the image.

To replace/relink the runtime, extract these source archives and follow the
runtime Makefile/build instructions. Then extract this AppImage's SquashFS with
`--appimage-extract` and rebuild it using appimagetool's `--runtime-file` option
with your replacement runtime. The application payload is separate from this
runtime and its dynamically linked Qt libraries can also be replaced in AppDir.
''')
    write(usr / "share/doc/README.md", f'''# {info['name']} {version}

This AppImage bundles Qt/WebEngine, Node.js 22.22.1 and the locked controller dependencies.
Run the AppImage directly; first launch does not run npm or install packages.
Ubuntu 24.04 x86_64 is the build baseline. Graphics/audio drivers and the Linux
kernel remain supplied by the host. A usable FUSE device is required to mount
the image, or use
`APPIMAGE_EXTRACT_AND_RUN=1 ./<application>.AppImage` without FUSE.

Existing controller services are reused. Fabric/pairing data remain in
`${{XDG_DATA_HOME:-$HOME/.local/share}}/matterdesk/controller`, outside the read-only
image. Keep this directory when upgrading. `MATTERDESK_DATA_DIR` still overrides it.
Explicit `--server ws://host:port/ws` uses an existing controller.

WebEngine's sandbox stays enabled. Ubuntu AppArmor/user-namespace policy must
allow the application renderer; the package never disables sandboxing or changes host policy.
Runtime licenses and actual dependency versions are in `usr/share/doc/licenses/`.
Qt is dynamically linked and can be replaced in an extracted AppDir.
Source: {package.REPOSITORY}/tree/{tag}
''')
    image = output / f"{info['name']}-{version}-x86_64.AppImage"
    run([tools["appimagetool-x86_64.AppImage"], "--runtime-file", tools["runtime-x86_64"],
         "--no-appstream", appdir, image], env=env)
    image.chmod(0o755)
    if not args.skip_smoke:
        smoke(image, build_dir, work, source, args.product, version)
    checksum = output / f"{image.name}.sha256"
    digest = hashlib.sha256(image.read_bytes()).hexdigest()
    write(checksum, f"{digest}  {image.name}\n")
    notes = output / f"{info['name']}-{version}-release-notes.md"
    write(notes, f"{info['description']}\n\nUbuntu 24.04 x86_64 AppImage with Qt/WebEngine, Node.js 22.22.1 and pinned controller dependencies. No first-launch npm installation. Existing Fabric and pairing data are preserved.\n\nSource: {package.REPOSITORY}/tree/{tag}\n\nVerify with `sha256sum -c {checksum.name}`.\n")
    print(json.dumps({"product": args.product, "name": info["name"], "version": version, "tag": tag,
                      "appimage": image.name, "checksum": checksum.name, "sha256": digest,
                      "notes": notes.name, "smoke": "skipped" if args.skip_smoke else "passed"}))


if __name__ == "__main__":
    main()
