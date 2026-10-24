#!/usr/bin/env bash
# Hosted-CI-only permission for Chromium user namespaces. This helper is never
# installed in an application package and never disables the WebEngine sandbox.
set -euo pipefail

if [[ "${GITHUB_ACTIONS:-}" != true || -z "${RUNNER_TEMP:-}" ]]; then
  echo 'This helper is restricted to GitHub Actions with RUNNER_TEMP set.' >&2
  exit 2
fi
profile_dir="$RUNNER_TEMP/matterdesk-ci-userns"
if [[ "${1:-}" == --cleanup ]]; then
  if [[ -d "$profile_dir" ]]; then
    shopt -s nullglob
    for profile in "$profile_dir"/*.profile; do
      sudo apparmor_parser -R "$profile"
      rm -- "$profile"
    done
    rmdir -- "$profile_dir"
  fi
  exit 0
fi
if [[ $# -lt 1 ]]; then
  echo 'Usage: prepare-ci-webengine-sandbox.sh BUILD_DIR [EXACT_EXECUTABLE ...]' >&2
  exit 2
fi

# Most runners need no exception. Test the actual kernel policy first.
if unshare -Ur true 2>/dev/null; then
  echo 'Unprivileged user namespaces work; no CI AppArmor exception is needed.'
  exit 0
fi
if [[ ! -r /sys/module/apparmor/parameters/enabled ]] ||
   [[ "$(cat /sys/module/apparmor/parameters/enabled)" != Y ]] ||
   ! command -v apparmor_parser >/dev/null; then
  echo 'User namespaces are blocked without an available AppArmor policy loader.' >&2
  echo 'WebEngine tests must run on a runner permitting its sandbox; no sandbox override was applied.' >&2
  exit 1
fi

build_dir="$(realpath -- "$1")"
shift
if [[ ! -d "$build_dir" ]]; then
  echo 'BUILD_DIR must be an existing build directory.' >&2
  exit 2
fi
attachments=("$build_dir/tst_*")
if command -v qmake6 >/dev/null; then
  qt_helper="$(qmake6 -query QT_INSTALL_LIBEXECS)/QtWebEngineProcess"
  if [[ -x "$qt_helper" ]]; then
    attachments+=("$(realpath -- "$qt_helper")")
  fi
fi
for executable in "$@"; do
  if [[ ! -f "$executable" || ! -x "$executable" ]]; then
    echo 'Extra AppArmor attachments must be existing executable files.' >&2
    exit 2
  fi
  attachments+=("$(realpath -- "$executable")")
done

mkdir -p -- "$profile_dir"
for attachment in "${attachments[@]}"; do
  # A profile attachment is quoted AppArmor syntax, not a shell command.
  # Reject syntax metacharacters; only our generated tst_* suffix uses a glob.
  checked_path="${attachment%/tst_\*}"
  if [[ "$checked_path" == *[$'\n\r\t"\\{}[]?*']* ]]; then
    echo 'Unsupported metacharacter in CI executable path.' >&2
    exit 2
  fi
  profile_id="$(printf '%s' "$attachment" | sha256sum | cut -c 1-20)"
  profile="$profile_dir/$profile_id.profile"
  cat > "$profile" <<EOF
abi <abi/4.0>,
include <tunables/global>
profile matterdesk-ci-$profile_id "$attachment" flags=(unconfined) {
  userns,
}
EOF
  sudo apparmor_parser -r "$profile"
done
echo 'Loaded temporary CI userns profiles for test binaries and explicit WebEngine helpers.'
