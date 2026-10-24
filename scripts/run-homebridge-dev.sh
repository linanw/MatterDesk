#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
printf '%s\n' 'Homebridge 开发设备模式：本次启动的本地控制器接受测试/开发证书信任链，仍校验证书签名。' '仅配对你信任的开发桥接器；已有控制器需以 ENABLE_TEST_NET_DCL=true 重启才会生效。'
export ENABLE_TEST_NET_DCL=true
exec "$project_dir/scripts/run.sh" "$@"
