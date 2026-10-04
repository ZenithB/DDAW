#!/usr/bin/env python3
"""Float64 numpy implementation of ddsp.synths.Harmonic (the spec), used to attribute the
null-test floor: is the difference between C++ and TF the C++ synth or TF's float32 arithmetic?
Usage: harmonic_f64.py <instrument>   (needs models/ref and models/out from `ddaw_b1 verify`)"""
import sys, os
import numpy as np
from common import *

def exp_sigmoid(x): return 2.0 * (1.0 / (1.0 + np.exp(-x))) ** np.log(10.0) + 1e-7

def synth(raw, f0, hop=64, sr=16000):
    n = raw.shape[0]
    amp = exp_sigmoid(raw[:, 0].astype(np.float64))
    hd = exp_sigmoid(raw[:, 1:61].astype(np.float64))
    k = np.arange(1, 61)
    hd = np.where(f0[:, None] * k[None, :] >= sr / 2, 0.0, hd)
    s = hd.sum(axis=1, keepdims=True); hd = hd / np.where(s == 0, 1e-7, s)
    A = amp[:, None] * hd                                   # [n, 60]
    f0 = f0.astype(np.float64)
    f0n = np.concatenate([f0, f0[-1:]]); An = np.vstack([A, A[-1:]])
    t = np.arange(n * hop); f = t // hop; p = (t % hop) / hop
    f0s = f0n[f] + (f0n[f + 1] - f0n[f]) * p                # bilinear, last held
    w = (0.5 - 0.5 * np.cos(2 * np.pi * (t % hop) / (2 * hop)))[:, None]
    As = An[f] * (1 - w) + An[f + 1] * w                    # Hann cross-fade
    phase = np.cumsum(2 * np.pi * f0s / sr)                 # exact, inclusive
    out = np.zeros(n * hop)
    for h in range(60):
        a = np.where(f0s * (h + 1) >= sr / 2, 0.0, As[:, h])
        out += a * np.sin((h + 1) * phase)
    return out

def angular_cumsum_f32(om, chunk=1000):
    """ddsp.core.angular_cumsum in float32: chunked cumsum, mod 2pi, stitched offsets."""
    n, k = om.shape; two_pi = np.float32(2 * np.pi)
    pad = (-n) % chunk
    x = np.vstack([om, np.zeros((pad, k), np.float32)]) if pad else om
    c = x.reshape(-1, chunk, k)
    ph = np.cumsum(c, axis=1, dtype=np.float32)
    off = np.mod(ph[:, -1:, :], two_pi)
    off = np.concatenate([np.zeros_like(off[:1]), off[:-1]], axis=0)
    off = np.mod(np.cumsum(off, axis=0, dtype=np.float32), two_pi)
    ph = np.mod(ph + off, two_pi)
    return ph.reshape(-1, k)[:n]

def synth_f32(raw, f0, hop=64, sr=16000):
    """The same spec evaluated with TF's float32 arithmetic and its angular_cumsum."""
    f32 = np.float32
    n = raw.shape[0]
    sg = lambda x: f32(1) / (f32(1) + np.exp(-x.astype(f32)))
    es = lambda x: f32(2) * sg(x) ** f32(np.log(10.0)) + f32(1e-7)
    amp = es(raw[:, 0:1]); hd = es(raw[:, 1:61])
    k = np.arange(1, 61, dtype=f32)
    hd = np.where(f0.astype(f32)[:, None] * k[None, :] >= f32(sr / 2), f32(0), hd)
    s = hd.sum(axis=1, keepdims=True, dtype=f32); hd = hd / np.where(s == 0, f32(1e-7), s)
    A = (amp * hd).astype(f32)
    f0n = np.concatenate([f0, f0[-1:]]).astype(f32); An = np.vstack([A, A[-1:]])
    t = np.arange(n * hop); fi = t // hop; p = ((t % hop) / hop).astype(f32)
    f0s = (f0n[fi] + (f0n[fi + 1] - f0n[fi]) * p).astype(f32)
    w = (f32(0.5) - f32(0.5) * np.cos(f32(2 * np.pi) * (t % hop).astype(f32) / f32(2 * hop)))[:, None]
    As = (An[fi] * (f32(1) - w) + An[fi + 1] * w).astype(f32)
    freq = f0s[:, None] * k[None, :]
    As = np.where(freq >= f32(sr / 2), f32(0), As)
    om = (freq * f32(2 * np.pi) / f32(sr)).astype(f32)
    ph = angular_cumsum_f32(om)
    return (As * np.sin(ph)).sum(axis=1, dtype=f32).astype(np.float64)

def null_db(a, b): 
    d = np.sqrt(np.mean((a - b) ** 2)); r = np.sqrt(np.mean(b ** 2)); return 20 * np.log10(max(d / r, 1e-8))

if __name__ == "__main__":
    inst = sys.argv[1] if len(sys.argv) > 1 else "violin"
    ref = os.path.join(MODELS, "ref", inst); out = os.path.join(MODELS, "out", inst)
    rd = lambda d, n: np.fromfile(os.path.join(d, n + ".bin"), dtype=np.float32)
    raw = rd(ref, "decoder_raw").reshape(N_FRAMES, 126); f0 = rd(ref, "f0_hz")
    tf_h = rd(ref, "harmonic_signal").astype(np.float64)
    cpp_h = rd(out, "harmonic_cpp").astype(np.float64)
    f64 = synth(raw, f0)
    f32s = synth_f32(raw, f0)
    print(f"{inst}: f64-spec vs TF  null {null_db(f64, tf_h):7.1f} dB   <- TF float32 arithmetic floor")
    print(f"{inst}: f32-emulation vs TF null {null_db(f32s, tf_h):7.1f} dB   <- should be tiny if the spec reading is right")
    print(f"{inst}: C++     vs f64  null {null_db(cpp_h, f64):7.1f} dB   <- C++ synth error vs the spec")
    print(f"{inst}: C++     vs TF   null {null_db(cpp_h, tf_h):7.1f} dB")
    ok = null_db(cpp_h, f64) <= -100.0
    print("PASS" if ok else "FAIL", "(gate: C++ synth vs float64 spec <= -100 dB)")
    sys.exit(0 if ok else 1)
