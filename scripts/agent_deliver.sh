#!/bin/sh
# Copy a finished device (and only that) from an agent workspace into the main tree.
#   scripts/agent_deliver.sh <workspace-name> <instruments|effects> <type> [<type>...]
# Copies src/devices/<kind>/<type>.cpp and tests/devices/<type>_test.cpp. Refuses anything else and
# refuses to overwrite a device that is already in the main tree unless DDAW_DELIVER_UPDATE=1 is set
# (a deliberate revision of an already-delivered device).
set -eu
NAME=${1:?usage}; KIND=${2:?usage}; shift 2
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WS="${AGENT_WS_DIR:-/private/tmp/ddaw-agents}/$NAME"
case "$KIND" in instruments|effects) ;; *) echo "kind must be instruments or effects" >&2; exit 1;; esac
[ -d "$WS" ] || { echo "no workspace $WS" >&2; exit 1; }
for T in "$@"; do
  case "$T" in *[!a-z0-9_]*|"") echo "bad device name: $T" >&2; exit 1;; esac
  SRC="src/devices/$KIND/$T.cpp"; TST="tests/devices/${T}_test.cpp"
  [ -f "$WS/$SRC" ] && [ -f "$WS/$TST" ] || { echo "$T: missing $SRC or $TST in the workspace" >&2; exit 1; }
  if [ "${DDAW_DELIVER_UPDATE:-0}" != 1 ] && { [ -e "$ROOT/$SRC" ] || [ -e "$ROOT/$TST" ]; }; then
    echo "$T: already exists in the main tree, not overwriting (set DDAW_DELIVER_UPDATE=1 to revise it)" >&2; exit 1
  fi
  cp "$WS/$SRC" "$ROOT/$SRC"; cp "$WS/$TST" "$ROOT/$TST"
  echo "delivered $T -> $SRC, $TST"
done
