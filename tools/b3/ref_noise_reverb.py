#!/usr/bin/env python3
"""B3 reference: the filtered-noise and reverb stages of the Magenta solo-instrument models, computed with
DDSP itself on the B1 reference clips, with the white noise fixed so the C++ port can be null-tested.

For each instrument, next to the B1 reference (models/ref/<inst>/) this writes
  noise_in.bin      [64000]  the white noise (uniform -1..1, seeded)
  noise_out.bin     [64000]  ddsp.core.frequency_filter(noise, exp_sigmoid(raw_noise_mags - 5), window_size=0)
  wet_out.bin       [64000]  harmonic + noise_out, then Reverb (trainable IR, add_dry=True)
Run with the B1 environment:  .venv-b1/bin/python tools/b3/ref_noise_reverb.py [instrument ...]
"""
import os, sys
import numpy as np
import tensorflow as tf
import ddsp
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "b1"))
from common import read_tensors

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
MODELS = os.path.join(ROOT, "models")
INSTRUMENTS = ["violin", "flute", "tenor_saxophone", "trumpet"]

def run(inst):
    ref = os.path.join(MODELS, "ref", inst)
    raw = np.fromfile(os.path.join(ref, "decoder_raw.bin"), dtype=np.float32).reshape(-1, 126)
    harm = np.fromfile(os.path.join(ref, "harmonic_signal.bin"), dtype=np.float32)
    T = read_tensors(os.path.join(MODELS, "export", inst + ".ddspw"))
    ir = np.asarray(T["reverb.ir"], dtype=np.float32).reshape(-1)
    n = len(harm)
    rng = np.random.RandomState(11)
    noise = rng.uniform(-1.0, 1.0, size=n).astype(np.float32)

    mags = ddsp.core.exp_sigmoid(tf.constant(raw[None, :, 61:126] + -5.0))
    filt = ddsp.core.frequency_filter(tf.constant(noise[None]), mags, window_size=0).numpy()[0]

    add = harm + filt
    rv = ddsp.effects.Reverb(trainable=False, reverb_length=len(ir), add_dry=True)
    masked = np.concatenate([[0.0], ir[1:]]).astype(np.float32)
    wet = ddsp.core.fft_convolve(tf.constant(add[None]), tf.constant(masked[None]), delay_compensation=0).numpy()[0]
    out = add + wet

    for name, a in [("noise_in", noise), ("noise_out", filt), ("wet_out", out)]:
        np.asarray(a, dtype=np.float32).tofile(os.path.join(ref, name + ".bin"))
    print(f"{inst}: noise rms {np.sqrt((filt**2).mean()):.4f}, wet rms {np.sqrt((out**2).mean()):.4f}, ir len {len(ir)}")

if __name__ == "__main__":
    for i in (sys.argv[1:] or INSTRUMENTS):
        run(i)
