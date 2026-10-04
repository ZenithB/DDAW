#!/usr/bin/env python3
"""Python reference render for the B1 null test.

Runs the real Magenta DDSP model (TF 2.11 + ddsp 3.7) on a deterministic synthetic
(f0, loudness) performance and dumps, as raw little-endian float32 under
models/ref/<instrument>/:
  f0_hz [1000]  loudness_db [1000]           the inputs
  f0_scaled [1000]  ld_scaled [1000]         after the model's preprocessor
  decoder_raw [1000,126]                     amps(1) | harmonic_distribution(60) | noise_magnitudes(65), pre-scale
  harmonic_signal [64000]                    deterministic harmonic synth output (angular cumsum), 16 kHz
  audio_full [64000]                         full model output incl. noise + reverb, for listening only
"""
import os, sys, json
import numpy as np
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "3")
import sys
from unittest.mock import MagicMock
# ddsp.training imports tensorflow_datasets, note_seq, apache_beam, tensorflowjs for training/eval only. The
# reference render never touches them, and the real package drags in a dependency tree that
# conflicts with TF 2.11's protobuf pin, so stub it.
for _m in ("tensorflow_datasets", "note_seq", "note_seq.protobuf", "apache_beam", "tensorflowjs"):
    sys.modules.setdefault(_m, MagicMock())
import tensorflow as tf, gin, ddsp, ddsp.training
from common import *

def run(instrument):
    d, prefix = ckpt_prefix(instrument)
    gin.clear_config()
    gin.parse_config_file(os.path.join(d, "operative_config-0.gin"), skip_unknown=True)
    gin.parse_config("F0LoudnessPreprocessor.compute_loudness = False")
    model = ddsp.training.models.get_model()
    f0, ld = test_features()
    feats = {"f0_hz": tf.constant(f0[None, :, None]), "loudness_db": tf.constant(ld[None, :, None])}
    model(feats, training=False)               # build variables
    model.restore(prefix)

    pre = model.preprocessor(dict(feats))
    f0s, lds = pre["f0_scaled"].numpy()[0, :, 0], pre["ld_scaled"].numpy()[0, :, 0]
    dec = model.decoder(pre)
    raw = np.concatenate([dec["amps"].numpy()[0], dec["harmonic_distribution"].numpy()[0],
                          dec["noise_magnitudes"].numpy()[0]], axis=-1)
    assert raw.shape == (N_FRAMES, 126), raw.shape

    harm = ddsp.synths.Harmonic(n_samples=N_SAMPLES, sample_rate=SAMPLE_RATE, use_angular_cumsum=True,
                                scale_fn=ddsp.core.exp_sigmoid, normalize_below_nyquist=True)
    hs = harm(dec["amps"], dec["harmonic_distribution"], pre["f0_hz"]).numpy()[0]
    full = model(dict(feats), training=False)
    audio = (full["audio_synth"] if "audio_synth" in full else full["out"]).numpy()[0]

    out = os.path.join(MODELS, "ref", instrument); os.makedirs(out, exist_ok=True)
    for name, a in [("f0_hz", f0), ("loudness_db", ld), ("f0_scaled", f0s), ("ld_scaled", lds),
                    ("decoder_raw", raw), ("harmonic_signal", hs), ("audio_full", audio)]:
        np.asarray(a, dtype=np.float32).tofile(os.path.join(out, name + ".bin"))
    json.dump({"frames": N_FRAMES, "samples": N_SAMPLES, "sample_rate": SAMPLE_RATE, "frame_rate": FRAME_RATE},
              open(os.path.join(out, "meta.json"), "w"))
    print(f"{instrument}: raw[{raw.min():.2f},{raw.max():.2f}] harmonic rms {np.sqrt((hs**2).mean()):.4f} "
          f"full rms {np.sqrt((audio**2).mean()):.4f} -> {out}")

if __name__ == "__main__":
    for inst in (sys.argv[1:] or INSTRUMENTS):
        run(inst)
