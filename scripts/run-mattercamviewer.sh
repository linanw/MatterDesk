#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -f "$project_dir/build/mattercamviewer" && -x "$project_dir/build/mattercamviewer" ]]; then
  viewer_bin="$project_dir/build/mattercamviewer"
elif [[ -f "$project_dir/bin/mattercamviewer" && -x "$project_dir/bin/mattercamviewer" ]]; then
  viewer_bin="$project_dir/bin/mattercamviewer"
else
  echo '没有找到 mattercamviewer，请先运行 ./scripts/build.sh。' >&2
  exit 1
fi

# Select the camera app while retaining the full launcher's controller lifecycle.
# An inherited full-app override must not silently start the other interface.
export MATTERDESK_EXECUTABLE="$viewer_bin"
exec "$project_dir/scripts/run.sh" "$@"
