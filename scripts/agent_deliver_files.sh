#!/bin/sh
# Deliver explicitly named files from an agent workspace into the main tree, restricted to a
# per-task allowlist. For tasks that are not a single device (see agent_deliver.sh for devices).
#   DDAW_DELIVER_UPDATE=1 scripts/agent_deliver_files.sh <workspace> <allow-regex> <file>...
# Every <file> is relative to the repo root and must match <allow-regex> (anchored). Overwriting an
# existing file requires DDAW_DELIVER_UPDATE=1.
set -eu
NAME=${1:?usage}; ALLOW=${2:?usage}; shift 2
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WS="${AGENT_WS_DIR:-/private/tmp/ddaw-agents}/$NAME"
[ -d "$WS" ] || { echo "no workspace $WS" >&2; exit 1; }
for F in "$@"; do
  case "$F" in /*|*..*) echo "bad path: $F" >&2; exit 1;; esac
  printf '%s\n' "$F" | grep -Eq "^($ALLOW)$" || { echo "$F: not allowed for this task (allow: $ALLOW)" >&2; exit 1; }
  [ -f "$WS/$F" ] || { echo "$F: missing in the workspace" >&2; exit 1; }
  if [ "${DDAW_DELIVER_UPDATE:-0}" != 1 ] && [ -e "$ROOT/$F" ]; then echo "$F: exists, not overwriting (DDAW_DELIVER_UPDATE=1)" >&2; exit 1; fi
  mkdir -p "$(dirname "$ROOT/$F")"; cp "$WS/$F" "$ROOT/$F"; echo "delivered $F"
done
