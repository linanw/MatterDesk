#!/bin/sh
set -eu
umask 077

set -- \
  --storage-path=/var/lib/matterdesk \
  --listen-address=127.0.0.1 \
  "--port=${MATTERDESK_CONTROLLER_PORT:-5580}" \
  --disable-dashboard \
  --disable-thread-diagnostics \
  --disable-ota

if [ -n "${MATTER_INTERFACE:-}" ]; then
  set -- "$@" "--primary-interface=$MATTER_INTERFACE"
fi

exec node /opt/matterdesk/controller/bootstrap.mjs "$@"
