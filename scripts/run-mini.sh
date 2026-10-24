#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# Compatibility entry point for the renamed camera application.
exec "$project_dir/scripts/run-mattercamviewer.sh" "$@"
