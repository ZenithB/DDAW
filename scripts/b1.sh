#!/bin/sh
# Reproduce the B1 decoder spike end to end: fetch checkpoints, build the Python reference
# environment, export weights, render the TF reference, build, verify, benchmark.
# Needs: uv, cmake, ninja. Network access for the checkpoints, PyPI and GitHub.
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)

./scripts/fetch_checkpoints.sh

if [ ! -x .venv-b1/bin/python ]; then
  uv venv --python 3.10 .venv-b1
  PY=.venv-b1/bin/python
  uv pip install --python $PY "tensorflow-macos==2.11.0" "numpy<1.24" "protobuf==3.20.3" \
    gin-config absl-py "scipy<1.11" "librosa==0.10.0" "tensorflow-probability==0.19.0" pydub future six \
    resampy mir_eval dill "setuptools<70" wheel "matplotlib<3.8" google-cloud-storage \
    "googleapis-common-protos==1.56.4" hmmlearn
  uv pip install --python $PY --no-deps "cloudml-hypertune==0.1.0.dev6"
  uv pip install --python $PY --no-build-isolation --no-deps crepe==0.0.12
  # ddsp 3.7.0 at a pinned commit; its own dependency pins are unsatisfiable on Apple Silicon.
  [ -d /tmp/ddsp_src ] || git clone -q https://github.com/magenta/ddsp.git /tmp/ddsp_src
  git -C /tmp/ddsp_src checkout -q 8bbf7c9fdbf9be9892b1982a32751fd38067888d
  uv pip install --python $PY --no-deps /tmp/ddsp_src
  uv pip install --python $PY "protobuf==3.20.3"   # google-cloud-storage may have bumped it
fi

(cd tools/b1 && ../../.venv-b1/bin/python export_weights.py && ../../.venv-b1/bin/python ddsp_ref.py)

cmake -S . -B build-b1 -G Ninja -DCMAKE_BUILD_TYPE=Release -DDDAW_BUILD_APP=OFF
cmake --build build-b1
ctest --test-dir build-b1 -R 'b1_|ddsp|allocate' --output-on-failure
for i in violin flute tenor_saxophone trumpet; do ./build-b1/src/ddsp/ddaw_b1 verify "$i" | grep -E '^\[|decoder \+|PASS|FAIL'; done
echo; echo "blocking wait:"; ./build-b1/src/ddsp/ddaw_b1 bench violin
echo; echo "spinning wait:"; DDAW_SPIN=1 ./build-b1/src/ddsp/ddaw_b1 bench violin
