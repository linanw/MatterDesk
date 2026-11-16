#!/usr/bin/env python3
"""Build a versioned, credential-free MatterServer deployment bundle."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
from time import gmtime
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parents[1]
DEPLOY_FILES = (
    ".env.example", ".gitignore", "Caddyfile", "Dockerfile", "README.md",
    "compose.yaml", "compose.direct-tls.yaml", "controller-entrypoint.sh",
    "create-user.sh", "secrets/users.caddy.example",
)
PUBLIC_WEB_TYPES = {".css", ".html", ".js", ".mjs", ".json", ".png", ".svg", ".woff2"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("release_package", ROOT / "scripts/package-release.py")
    package = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(package)
    version = package.release_identity(ROOT, "matterdesk")["version"]
    subprocess.run(["npm", "--prefix", "web", "run", "build"], cwd=ROOT, check=True)
    web_root = ROOT / "web/dist"
    if not (web_root / "index.html").is_file():
        raise SystemExit("Web bundle is missing its index")
    files = [".dockerignore", *[f"deploy/{name}" for name in DEPLOY_FILES],
             *[name for name in package.COMMON_FILES if name.startswith("controller/")]]
    files += sorted(str(path.relative_to(ROOT)) for path in web_root.rglob("*")
                    if path.is_file() and path.suffix in PUBLIC_WEB_TYPES
                    and not any(part.startswith(".") for part in path.relative_to(web_root).parts))
    timestamp = gmtime(max(315532800, int(os.getenv("SOURCE_DATE_EPOCH", "315532800"))))[:6]
    args.output_dir.mkdir(parents=True, exist_ok=True)
    bundle = args.output_dir / f"MatterServer-{version}-nas.zip"
    with ZipFile(bundle, "w", compression=ZIP_DEFLATED) as archive:
        for name in files:
            path = ROOT / name
            if not path.is_file() or path.is_symlink() or not path.resolve().is_relative_to(ROOT):
                raise SystemExit(f"Missing or unsafe deployment input: {name}")
            member = ZipInfo(name, timestamp)
            member.create_system = 3
            member.compress_type = ZIP_DEFLATED
            member.external_attr = ((0o100755 if name.endswith(".sh") else 0o100644) << 16)
            archive.writestr(member, path.read_bytes())
    bundle.with_suffix(".zip.sha256").write_text(f"{hashlib.sha256(bundle.read_bytes()).hexdigest()}  {bundle.name}\n")
    print(bundle)


if __name__ == "__main__":
    main()
