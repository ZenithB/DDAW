#!/bin/sh
# Fails if model weights or checkpoints are tracked by git (their terms are unconfirmed; see NOTICE).
set -eu
cd "$(dirname "$0")/.."
bad=$(git ls-files | grep -E '(^|/)models/|\.ddspw$|\.ckpt|ckpt-[0-9]+\.(index|data)|dataset_statistics\.pkl$|\.safetensors$|\.onnx$' || true)
if [ -n "$bad" ]; then
  echo "model weights must not be committed (see NOTICE):" >&2
  echo "$bad" >&2
  exit 1
fi
echo "no model weights tracked"
