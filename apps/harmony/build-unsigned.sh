#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$project_dir/../.." && pwd)
studio_dir=${DEVECO_STUDIO_HOME:-/opt/devecostudio}
export DEVECO_SDK_HOME=${DEVECO_SDK_HOME:-$studio_dir/sdk}
export JAVA_HOME=${JAVA_HOME:-$studio_dir/jbr}
export PATH="$studio_dir/tools/node/bin:$studio_dir/tools/bin:$JAVA_HOME/bin:$PATH"
if [ "$#" -gt 1 ]; then
    printf 'Usage: %s [output-directory]\n' "$0" >&2
    exit 2
fi
output_dir=${1:-$project_dir/dist}
[[ "$output_dir" == /* ]] || output_dir="$PWD/$output_dir"
build_mode=${MATTERDESK_HARMONY_BUILD_MODE:-debug}
case "$build_mode" in
    debug|release) ;;
    *) printf 'MATTERDESK_HARMONY_BUILD_MODE must be debug or release.\n' >&2; exit 2 ;;
esac

for cmd in ohpm hvigorw java python3; do
    if ! command -v "$cmd" >/dev/null; then
        printf 'Missing %s; configure DEVECO_STUDIO_HOME/PATH for DevEco with HarmonyOS SDK API 26.\n' "$cmd" >&2
        exit 1
    fi
done

# Explicit staging keeps ignored local build/signing settings and previous HAPs
# out of both the build and its output. Never read a developer's signed profile.
stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT
cd -- "$project_dir"
mkdir -p -- "$stage/entry/src"
cp -R -- AppScope hvigor hvigorfile.ts oh-package.json5 oh-package-lock.json5 "$stage/"
cp -- entry/{build-profile.json5,hvigorfile.ts,oh-package.json5,obfuscation-rules.txt} "$stage/entry/"
cp -R -- entry/src/main "$stage/entry/src/"
cp --remove-destination -- "$repo_dir/src/camera/player.js" "$stage/entry/src/main/resources/rawfile/camera/player.js"
cp -- build-profile.unsigned.json5 "$stage/build-profile.json5"
python3 - "$stage/AppScope/app.json5" <<'PY'
import json
import os
from pathlib import Path
import re
import sys

name = os.environ.get("MATTERDESK_HARMONY_VERSION")
code = os.environ.get("MATTERDESK_HARMONY_VERSION_CODE")
if (name is None) != (code is None):
    sys.exit("Set MATTERDESK_HARMONY_VERSION and MATTERDESK_HARMONY_VERSION_CODE together")
if name is not None:
    version_component = r"(?:0|[1-9][0-9]*)"
    prerelease_id = r"(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
    pattern = (rf"{version_component}\.{version_component}\.{version_component}"
               rf"(?:-{prerelease_id}(?:\.{prerelease_id})*)?")
    if len(name) > 127 or not re.fullmatch(pattern, name):
        sys.exit("MATTERDESK_HARMONY_VERSION must be a release version such as 0.7.0-beta.1")
    if not re.fullmatch(r"[1-9][0-9]{0,9}", code) or int(code) > 2147483647:
        sys.exit("MATTERDESK_HARMONY_VERSION_CODE must be an integer from 1 to 2147483647")
    path = Path(sys.argv[1])
    package = json.loads(path.read_text(encoding="utf-8"))
    package["app"]["versionName"] = name
    package["app"]["versionCode"] = int(code)
    path.write_text(json.dumps(package, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
PY
mkdir -p -- "$output_dir"
output_dir=$(cd -- "$output_dir" && pwd)
cd -- "$stage"
ohpm install
hvigorw assembleHap --mode module -p module=entry@default -p product=default -p "buildMode=$build_mode" --no-daemon
hap=entry/build/default/outputs/default/entry-default-unsigned.hap
test -s "$hap"
cp -- "$hap" "$output_dir/MatterDesk-unsigned.hap"
printf '%s\n' "$output_dir/MatterDesk-unsigned.hap"
