#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${MATTERDESK_EXECUTABLE:-}" ]]; then
  app_bin="$MATTERDESK_EXECUTABLE"
  if [[ ! -f "$app_bin" || ! -x "$app_bin" ]]; then
    echo 'MATTERDESK_EXECUTABLE 必须指向存在且可执行的程序文件。' >&2
    exit 1
  fi
elif [[ -x "$project_dir/build/matterdesk" ]]; then
  app_bin="$project_dir/build/matterdesk"
elif [[ -x "$project_dir/bin/matterdesk" ]]; then
  app_bin="$project_dir/bin/matterdesk"
else
  echo '没有找到可执行文件，请先运行 ./scripts/build.sh。' >&2
  exit 1
fi

# These modes need no locally managed controller. An explicit server may be remote.
for argument in "$@"; do
  case "$argument" in
    --demo|--help|-h|--version|-v|--server|--server=*)
      exec "$app_bin" "$@"
      ;;
  esac
done

port_open() { (exec 3<>/dev/tcp/127.0.0.1/5580) 2>/dev/null; }

# Reuse an existing listener. The Qt client checks its Matter handshake and schema.
if port_open; then
  printf '%s\n' '复用 127.0.0.1:5580 的现有服务；Qt 客户端将验证 Matter 协议。'
  exec "$app_bin" --server ws://127.0.0.1:5580/ws "$@"
fi

if [[ ! -f "$project_dir/controller/node_modules/matter-server/dist/esm/bin.js" ]]; then
  echo '本地控制器尚未安装，请先运行 ./scripts/setup-controller.sh。' >&2
  exit 1
fi
if ! command -v node >/dev/null; then
  echo '请先安装 Node.js 22.13+，或使用 --server 指定已有控制器。' >&2
  exit 1
fi

umask 077
state_dir="${XDG_STATE_HOME:-$HOME/.local/state}/matterdesk"
mkdir -p -- "$state_dir"
controller_log="$state_dir/controller.log"
controller_pid=''
ui_pid=''
cleanup() {
  trap - EXIT INT TERM
  if [[ -n "$ui_pid" ]] && kill -0 "$ui_pid" 2>/dev/null; then
    kill -TERM "$ui_pid" 2>/dev/null || true
    wait "$ui_pid" 2>/dev/null || true
  fi
  if [[ -n "$controller_pid" ]] && kill -0 "$controller_pid" 2>/dev/null; then
    kill -TERM "$controller_pid" 2>/dev/null || true
    for ((attempt=0; attempt<50; attempt++)); do
      kill -0 "$controller_pid" 2>/dev/null || break
      sleep 0.1
    done
    if kill -0 "$controller_pid" 2>/dev/null; then
      kill -KILL "$controller_pid" 2>/dev/null || true
    fi
  fi
  if [[ -n "$controller_pid" ]]; then
    wait "$controller_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

printf '%s\n' "启动本地 Matter Server，日志：$controller_log"
"$project_dir/scripts/start-controller.sh" >"$controller_log" 2>&1 &
controller_pid=$!
ready=false
for ((attempt=0; attempt<60; attempt++)); do
  if ! kill -0 "$controller_pid" 2>/dev/null; then
    echo "控制器启动失败，请查看：$controller_log" >&2
    tail -n 12 -- "$controller_log" >&2
    exit 1
  fi
  if port_open; then
    ready=true
    break
  fi
  sleep 0.25
done
if [[ "$ready" != true ]]; then
  echo "等待控制器超时（15 秒），请查看：$controller_log" >&2
  tail -n 12 -- "$controller_log" >&2
  exit 1
fi

"$app_bin" --server ws://127.0.0.1:5580/ws "$@" &
ui_pid=$!
set +e
wait -n "$ui_pid" "$controller_pid"
first_status=$?
set -e
if kill -0 "$ui_pid" 2>/dev/null; then
  echo "Matter Server 已意外退出，Qt 界面仍可使用；日志：$controller_log" >&2
  tail -n 12 -- "$controller_log" >&2
  set +e
  wait "$ui_pid"
  ui_status=$?
  set -e
  exit "$ui_status"
fi
ui_pid=''
exit "$first_status"
