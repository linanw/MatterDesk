#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if ! command -v node >/dev/null || ! command -v npm >/dev/null; then
  echo '请先安装 Node.js 22.13 或更新版本（含 npm）。' >&2
  exit 1
fi
node -e 'const [major, minor] = process.versions.node.split(".").map(Number); if (major < 22 || (major === 22 && minor < 13)) { console.error("需要 Node.js >= 22.13.0"); process.exit(1); }'
npm ci --prefix "$project_dir/controller" --omit=dev --no-audit --no-fund
printf '%s\n' '控制器依赖已安装。运行 ./scripts/start-controller.sh 启动本地 Matter Server。'
