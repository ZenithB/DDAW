#!/bin/sh
# Copy synthyy's golden fixtures into tests/fixtures/synthyy (committed: 75 small files, 34 MB).
# Read-only on the synthyy side: this script never writes there.
set -eu
SRC="${SYNTHYY_DIR:-$HOME/Documents/synthyy}/fixtures"
DST="$(cd "$(dirname "$0")/.." && pwd)/tests/fixtures/synthyy"
[ -d "$SRC/projects" ] && [ -d "$SRC/golden" ] || { echo "no fixtures under $SRC" >&2; exit 1; }
mkdir -p "$DST/projects" "$DST/golden"
cp "$SRC"/projects/*.json "$DST/projects/"
cp "$SRC"/golden/*.wav "$DST/golden/"
# Scores the Rust port reached against the goldens (synthyy's parity ledger): the baseline a C++
# port is gated against. Read-only; needs synthyy's target/parity-report.json (from `cargo test`).
REPORT="${SYNTHYY_DIR:-$HOME/Documents/synthyy}/target/parity-report.json"
if [ -f "$REPORT" ]; then
  python3 - "$REPORT" "$DST/baseline.txt" <<'PY'
import json, sys
rows = json.load(open(sys.argv[1]))
with open(sys.argv[2], "w") as f:
    for r in rows:
        f.write("%s %.4f %.3f\n" % (r["name"], r["spectral_similarity"], r["rendered_rms_dbfs"] - r["golden_rms_dbfs"]))
print("wrote baseline.txt:", len(rows), "fixtures")
PY
else
  echo "no $REPORT; baseline.txt not written" >&2
fi
echo "synced $(ls "$DST/projects" | wc -l | tr -d ' ') projects, $(ls "$DST/golden" | wc -l | tr -d ' ') goldens -> $DST"
