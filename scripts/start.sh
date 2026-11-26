#!/bin/sh
set -eu

app_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$(id -u)" -ne 0 ]; then
    echo 'Gizlet Browser needs the Gizlet launcher to mount its runtime.' >&2
    exit 1
fi

session_dir=$(mktemp -d /run/gizlet-browser.XXXXXX)
trap 'rmdir "$session_dir/root" "$session_dir"' EXIT
mkdir "$session_dir/root"
unshare --mount --propagation private --pid --fork --kill-child \
    /bin/sh "$app_dir/bin/session" "$app_dir/runtime.sqfs" "$session_dir/root" "$@" &
session_pid=$!
trap 'kill "$session_pid" 2>/dev/null || true; wait "$session_pid" || true; exit 143' TERM INT
wait "$session_pid"
