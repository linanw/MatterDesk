#!/usr/bin/env python3
"""Package one Linux application from an explicit, runtime-only file list."""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile


REPOSITORY = "https://github.com/linanw/MatterDesk"
PRODUCTS = {
    "matterdesk": {
        "name": "MatterDesk",
        "version_pattern": r"project\(MatterDesk\s+VERSION\s+(\d+\.\d+\.\d+)(?=\s|\))",
        "prerelease_variable": "MATTERDESK_PRERELEASE",
        "launcher": "run.sh",
        "dev_launcher": "run-homebridge-dev.sh",
        "extra_launchers": [],
        "description": "Linux / Qt 6 Matter 家庭控制客户端。",
    },
    "mattercamviewer": {
        "name": "MatterCamViewer",
        "version_pattern": r"set\(MATTERCAMVIEWER_VERSION\s+(\d+\.\d+\.\d+)\)",
        "prerelease_variable": "MATTERCAMVIEWER_PRERELEASE",
        "launcher": "run-mattercamviewer.sh",
        "dev_launcher": "run-mattercamviewer-homebridge-dev.sh",
        "extra_launchers": ["run-mini.sh", "run-mini-homebridge-dev.sh"],
        "description": "Linux / Qt 6 极简 Matter 摄像头查看器，快照与直播共用无边框窗口。",
    },
}
COMMON_FILES = [
    "THIRD_PARTY_NOTICES.md",
    "scripts/run.sh",
    "scripts/setup-controller.sh",
    "scripts/start-controller.sh",
    "controller/bootstrap.mjs",
    "controller/vendor-camera-ptz.mjs",
    "controller/vendor-camera-metadata.mjs",
    "controller/package.json",
    "controller/package-lock.json",
]


def release_identity(source_root: Path, product: str) -> dict:
    """Read one product's complete version and reject mismatched release tags."""
    info = PRODUCTS[product]
    cmake = (source_root / "CMakeLists.txt").read_text(encoding="utf-8")
    # Ignore commented-out versions rather than silently packaging the wrong one.
    cmake = "\n".join(line for line in cmake.splitlines() if not line.lstrip().startswith("#"))
    match = re.search(info["version_pattern"], cmake)
    if not match:
        raise ValueError("The product version is missing from CMakeLists.txt")
    base = match.group(1)
    if any(part != str(int(part)) for part in base.split(".")):
        raise ValueError("The product version must not contain leading zeros")
    suffix_match = re.search(r"set\(" + info["prerelease_variable"] + r'\s+"([^"\n]*)"\s*\)', cmake)
    if not suffix_match and re.search(r"set\(" + info["prerelease_variable"] + r"(?:\s|\))", cmake):
        raise ValueError("The prerelease declaration must use one quoted literal")
    suffix = suffix_match.group(1) if suffix_match else ""
    if suffix and not re.fullmatch(r"(?:alpha|beta|rc)\.[1-9][0-9]*", suffix):
        raise ValueError("The prerelease must be empty or alpha.N, beta.N, rc.N (N >= 1)")
    version = base + (f"-{suffix}" if suffix else "")
    tag = f"{product}-v{version}"
    tagged = os.environ.get("GITHUB_REF_TYPE") == "tag"
    if tagged and os.environ.get("GITHUB_REF_NAME") != tag:
        raise ValueError("The release tag does not match this product's complete CMake version")
    source_ref = tag if tagged else os.environ.get("GITHUB_SHA", "")
    if not source_ref:
        source_ref = subprocess.check_output(["git", "-C", source_root, "rev-parse", "HEAD"], text=True).strip()
    return {"product": product, "name": info["name"], "version": version,
            "tag": tag, "prerelease": bool(suffix), "sourceRef": source_ref}


def copy_file(source: Path, destination: Path, executable: bool = False) -> None:
    if not source.is_file() or source.is_symlink():
        raise ValueError(f"Missing regular release input: {source.name}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    destination.chmod(0o755 if executable else 0o644)


def strip_binary(binary: Path) -> None:
    header = binary.read_bytes()[:20]
    if (len(header) != 20 or header[:6] != b"\x7fELF\x02\x01"
            or struct.unpack_from("<H", header, 18)[0] != 62):
        raise ValueError("The release binary must be a Linux x86_64 ELF executable")
    subprocess.run(["strip", "--strip-unneeded", str(binary)], check=True)
    sections = subprocess.check_output(["readelf", "--sections", "--wide", str(binary)], text=True)
    if re.search(r"\.(?:z?debug_|gnu_debuglink|gnu_debugaltlink)", sections):
        raise ValueError("Debug sections remain in the release binary")
    if re.search(rb"/(?:home|Users)/[A-Za-z0-9_.-]+/", binary.read_bytes()):
        raise ValueError("A developer home directory remains in the release binary")


def readme(product: str, info: dict, version: str, platform: str, source_ref: str) -> str:
    name, launcher, dev_launcher = info["name"], info["launcher"], info["dev_launcher"]
    return f"""# {name} {version}

{info['description']}

本包面向 **{platform}**，动态使用系统 Qt 6 / WebEngine，未打包 Qt 或 Node.js。
Ubuntu 24.04 可用以下方式安装 Qt 依赖（包含开发包及 WebEngine 资源）：

```bash
sudo apt install qt6-base-dev qt6-websockets-dev qt6-webengine-dev qt6-webchannel-dev
```

本地控制器需要 **Node.js 22.13 或更新版本及 npm**。安装控制器依赖后启动：

```bash
./scripts/setup-controller.sh
./scripts/{launcher}
```

启动器自动复用 `127.0.0.1:5580` 上的服务；没有服务时启动附带的 OHF Matter
控制器。关闭应用时仅停止本次启动器创建的控制器。Homebridge 与客户端可以位于
不同机器；默认仍连接运行 Qt 应用这台电脑上的本地控制器。

需要信任 Homebridge 开发证书进行配对时，使用 `./scripts/{dev_launcher}`。
此入口只影响新启动的控制器，已有控制器的设置保持不变。

也可以直接连接已部署的控制器，此时无需安装本包的 Node 依赖：

```bash
./bin/{product} --server ws://localhost:5580/ws
```

MatterDesk 和 MatterCamViewer 共用既有控制器的 Fabric 目录：
`${{XDG_DATA_HOME:-$HOME/.local/share}}/matterdesk/controller`。
升级时保留此目录；删除它会使本控制器失去原有配对身份。
`MATTERDESK_DATA_DIR` 可指定原有数据目录。

摄像头功能需要支持 Matter Camera 的设备与相应控制器 API；普通 HomeKit 摄像头
不能仅凭 HomeKit 配对码加入。米家名称/房间和 PTZ 扩展还需要兼容的桥接插件。
本项目不接入 Apple 家庭账户，不提供 HomeKit Secure Video。

源码与构建说明：[linanw/MatterDesk]({REPOSITORY}/tree/{source_ref})。
标签发布同时提供源码归档；Actions 候选包以该提交为准。其他 Linux 发行版请从源码构建。
项目许可状态及第三方依赖说明见 `THIRD_PARTY_NOTICES.md`；若包内有 `LICENSE`，请同时阅读。
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--product", required=True, choices=PRODUCTS)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--platform", required=True, choices=["ubuntu-24.04-x86_64"])
    args = parser.parse_args()
    source_root = Path(__file__).resolve().parent.parent
    build_dir, output_dir = args.build_dir.resolve(), args.output_dir.resolve()
    info = PRODUCTS[args.product]
    release = release_identity(source_root, args.product)
    version, tag = release["version"], release["tag"]

    output_dir.mkdir(parents=True, exist_ok=True)
    stem = f"{info['name']}-{version}-{args.platform}"
    archive = output_dir / f"{stem}.tar.gz"
    checksum = output_dir / f"{stem}.tar.gz.sha256"
    notes = output_dir / f"{info['name']}-{version}-release-notes.md"
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
    if epoch < 0:
        raise ValueError("SOURCE_DATE_EPOCH must not be negative")

    with tempfile.TemporaryDirectory(prefix=f".{args.product}-", dir=output_dir) as temporary:
        staging = Path(temporary) / stem
        files = set(COMMON_FILES)
        if (source_root / "LICENSE").is_file():
            files.add("LICENSE")
        files.update(f"scripts/{name}" for name in [info["launcher"], info["dev_launcher"], *info["extra_launchers"]])
        files.update([f"packaging/{args.product}.desktop", f"packaging/{args.product}.svg"])
        for relative in sorted(files):
            copy_file(source_root / relative, staging / relative, relative.startswith("scripts/"))
        binary = staging / "bin" / args.product
        copy_file(build_dir / args.product, binary, True)
        strip_binary(binary)
        (staging / "README.md").write_text(readme(args.product, info, version, args.platform, release["sourceRef"]), encoding="utf-8")
        (staging / "README.md").chmod(0o644)

        temporary_archive = Path(temporary) / archive.name
        with temporary_archive.open("wb") as raw:
            with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=epoch) as compressed:
                with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as tar:
                    for path in [staging, *sorted(staging.rglob("*"))]:
                        if path.is_symlink() or not (path.is_file() or path.is_dir()):
                            raise ValueError("Only regular runtime files and directories may be packaged")
                        entry = tar.gettarinfo(str(path), arcname=str(path.relative_to(staging.parent)))
                        entry.uid = entry.gid = 0
                        entry.uname = entry.gname = ""
                        entry.mtime = epoch
                        entry.mode = 0o755 if path.is_dir() or path.stat().st_mode & 0o111 else 0o644
                        if path.is_file():
                            with path.open("rb") as content:
                                tar.addfile(entry, content)
                        else:
                            tar.addfile(entry)
        os.replace(temporary_archive, archive)

    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    checksum.write_text(f"{digest}  {archive.name}\n", encoding="utf-8")
    notes.write_text(
        f"{info['description']}\n\n"
        f"- Linux x86_64 binary built on Ubuntu 24.04; system Qt 6 / WebEngine required.\n"
        f"- Local controller requires Node.js 22.13+ and npm; see the included README.\n"
        f"- Existing MatterDesk controller Fabric and pairing data are preserved.\n"
        f"- Source: {REPOSITORY}/tree/{release['sourceRef']}\n"
        f"- Verify the archive with `sha256sum -c {checksum.name}`.\n",
        encoding="utf-8",
    )
    print(json.dumps({**release, "archive": archive.name, "checksum": checksum.name,
                      "sha256": digest, "notes": notes.name}, ensure_ascii=False))


if __name__ == "__main__":
    main()
