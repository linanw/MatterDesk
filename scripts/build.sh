#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cmake -S "$project_dir" -B "$project_dir/build" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$project_dir/build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
printf '%s\n' "MatterDesk：$project_dir/build/matterdesk" "MatterCamViewer：$project_dir/build/mattercamviewer" "演示模式：$project_dir/build/matterdesk --demo"
