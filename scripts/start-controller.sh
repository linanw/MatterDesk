#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
controller_bin="$project_dir/controller/node_modules/matter-server/dist/esm/bin.js"
controller_bootstrap="$project_dir/controller/bootstrap.mjs"
if [[ ! -f "$controller_bin" ]]; then
  echo '尚未安装控制器依赖，请先运行 ./scripts/setup-controller.sh。' >&2
  exit 1
fi
if [[ ! -f "$controller_bootstrap" || ! -f "$project_dir/controller/vendor-camera-ptz.mjs" || ! -f "$project_dir/controller/vendor-camera-metadata.mjs" ]]; then
  echo '控制器预注册入口缺失，请恢复 controller/bootstrap.mjs、vendor-camera-ptz.mjs 和 vendor-camera-metadata.mjs。' >&2
  exit 1
fi
data_dir="${MATTERDESK_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/matterdesk/controller}"
umask 077
mkdir -p -- "$data_dir"
controller_args=("--storage-path=$data_dir" --listen-address=127.0.0.1 --port=5580 --disable-thread-diagnostics)
if [[ -n "${MATTER_INTERFACE:-}" ]]; then
  controller_args+=("--primary-interface=$MATTER_INTERFACE")
fi
printf '%s\n' '默认 Matter Server 地址：ws://127.0.0.1:5580/ws（自定义参数以服务器日志为准）' "Fabric 数据：$data_dir" '按 Ctrl+C 停止控制器。'
# The vendor model imports Matter before the server CLI parser. Matter's early
# environment parser needs --key=value; preserve separated user options by
# normalizing them before Node starts (including paths containing spaces).
extra_args=()
while (( $# )); do
  if [[ "$1" == --* && "$1" != *=* && $# -ge 2 && "$2" != --* ]]; then
    extra_args+=("$1=$2")
    shift 2
  else
    extra_args+=("$1")
    shift
  fi
done
exec node "$controller_bootstrap" "${controller_args[@]}" "${extra_args[@]}"
