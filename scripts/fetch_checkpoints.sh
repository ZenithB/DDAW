#!/bin/sh
# Fetch Magenta DDSP solo-instrument checkpoints into models/ (git-ignored).
# Terms of the pretrained checkpoints are unconfirmed (see NOTICE and docs/PLAN.md): never commit or
# redistribute these files. scripts/check_no_weights.sh enforces the first half in CI.
set -eu
BASE="https://storage.googleapis.com/ddsp/models/timbre_transfer_colab/2021-07-08"
DST="$(cd "$(dirname "$0")/.." && pwd)/models"
get() {  # instrument ckpt-step
  d="$DST/solo_$1_ckpt"; mkdir -p "$d"
  for f in "ckpt-$2.data-00000-of-00001" "ckpt-$2.index" dataset_statistics.pkl operative_config-0.gin; do
    [ -s "$d/$f" ] || curl -fsSL "$BASE/solo_$1_ckpt/$f" -o "$d/$f"
  done
  echo "ok solo_$1_ckpt"
}
get violin 40000
get flute 20000
get tenor_saxophone 20000
get trumpet 20000
echo "Note: the terms of these checkpoints are unconfirmed (see NOTICE). Do not commit or redistribute them."
