#!/bin/sh
# A6 exit check: the 30-minute soak at a 64-frame buffer. Builds Release if needed.
#   scripts/soak.sh            accelerated (30 minutes of audio in about a minute)
#   scripts/soak.sh realtime   paced against the clock (30 minutes of wall time)
set -e
cd "$(dirname "$0")/.."
[ -d build-rel ] || cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel --target ddaw_soak
if [ "$1" = realtime ]; then exec build-rel/ddaw_soak --minutes 30 --realtime; fi
exec build-rel/ddaw_soak --minutes 30
