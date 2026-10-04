#!/bin/sh
# Create a private build workspace for a device-port agent (the repo is not under git, and sources
# are globbed, so one agent's half-written file must not break another agent's build).
#   scripts/agent_workspace.sh <name>       -> $AGENT_WS_DIR/<name> (default: $TMPDIR-style scratch)
# The workspace is a copy of the repo without build output, models or fixtures; the synthyy fixtures
# are symlinked (read-only use). Dependencies are reused from the main build tree, so nothing is
# downloaded. The JUCE app and the DDSP decoder are not built.
set -eu
NAME=${1:?usage: agent_workspace.sh <name>}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BASE=${AGENT_WS_DIR:-/private/tmp/ddaw-agents}
WS="$BASE/$NAME"
DEPS="$ROOT/build/_deps"
[ -d "$DEPS/catch2-src" ] && [ -d "$DEPS/nlohmann_json-src" ] || { echo "configure the main build first (build/_deps missing)" >&2; exit 1; }

rm -rf "$WS"; mkdir -p "$WS"
rsync -a --exclude 'build*' --exclude 'models' --exclude '.venv-b1' --exclude '.git' \
      --exclude 'tests/fixtures/synthyy' "$ROOT/" "$WS/"
ln -s "$ROOT/tests/fixtures/synthyy" "$WS/tests/fixtures/synthyy"

# The Command Line Tools on this machine ship a stub libc++ (README); use the SDK's.
SDK=$(xcrun --show-sdk-path)
cat > "$WS/env.sh" <<ENV
export CXXFLAGS="-nostdinc++ -isystem $SDK/usr/include/c++/v1"
ENV
. "$WS/env.sh"

cmake -S "$WS" -B "$WS/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DDDAW_BUILD_APP=OFF -DDDAW_BUILD_DDSP=OFF \
      -DFETCHCONTENT_SOURCE_DIR_CATCH2="$DEPS/catch2-src" \
      -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$DEPS/nlohmann_json-src" > "$WS/configure.log" 2>&1 \
  || { cat "$WS/configure.log"; exit 1; }

cat <<MSG
workspace: $WS
  . $WS/env.sh                                         # compiler flags (run once per shell)
  cmake -S $WS -B $WS/build >/dev/null                 # re-run after adding a device file
  cmake --build $WS/build
  ctest --test-dir $WS/build -R <type> --output-on-failure
  $WS/build/ddaw_parity $WS/tests/fixtures/synthyy --limiter tone --no-trim | grep -E 'fixture|<fixture-name>'
MSG
