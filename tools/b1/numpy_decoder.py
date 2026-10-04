#!/usr/bin/env python3
"""Pure-numpy implementation of the Magenta RnnFcDecoder from the exported .ddspw tensors.
An executable spec: if this matches TF, the export and the architecture reading are right,
and any C++ mismatch is an implementation bug. Usage: numpy_decoder.py <instrument>"""
import sys, os
import numpy as np
from common import *

EPS, ALPHA = 1e-3, 0.2

def sigmoid(x): return 1.0 / (1.0 + np.exp(-x))

def fc(x, W, b, g, be):                       # Dense -> LayerNorm -> LeakyReLU ; W is [out,in]
    y = W @ x + b
    y = (y - y.mean()) / np.sqrt(y.var() + EPS) * g + be
    return np.where(y >= 0, y, ALPHA * y)

def stack(x, T, p):
    for i in range(3):
        x = fc(x, T[f"{p}.{i}.W"], T[f"{p}.{i}.b"], T[f"{p}.{i}.gamma"], T[f"{p}.{i}.beta"])
    return x

def gru_step(x, h, T):                        # TF GRU, reset_after=True; gate order z, r, h
    W, U, b = T["gru.W"], T["gru.U"], T["gru.b"]
    n = h.shape[0]
    xw = x @ W + b[0]
    hu = h @ U + b[1]
    z = sigmoid(xw[:n] + hu[:n])
    r = sigmoid(xw[n:2*n] + hu[n:2*n])
    c = np.tanh(xw[2*n:] + r * hu[2*n:])
    return z * h + (1 - z) * c

def decode(T, ld_scaled, f0_scaled):
    h = np.zeros(512, np.float32)
    out = []
    for ld, f0 in zip(ld_scaled, f0_scaled):
        a = stack(np.array([ld], np.float32), T, "ld")
        b = stack(np.array([f0], np.float32), T, "f0")
        h = gru_step(np.concatenate([a, b]), h, T)
        x = stack(np.concatenate([a, b, h]), T, "out")
        out.append(T["dense_out.W"] @ x + T["dense_out.b"])
    return np.array(out, np.float32)

if __name__ == "__main__":
    inst = sys.argv[1] if len(sys.argv) > 1 else "violin"
    T = read_tensors(os.path.join(MODELS, "export", inst + ".ddspw"))
    ref = os.path.join(MODELS, "ref", inst)
    rd = lambda n: np.fromfile(os.path.join(ref, n + ".bin"), dtype=np.float32)
    ld, f0 = rd("ld_scaled"), rd("f0_scaled")
    tf_raw = rd("decoder_raw").reshape(N_FRAMES, 126)
    n = int(sys.argv[2]) if len(sys.argv) > 2 else N_FRAMES
    mine = decode(T, ld[:n], f0[:n])
    err = np.abs(mine - tf_raw[:n])
    print(f"{inst}: numpy vs TF over {n} frames: max|err| {err.max():.3e}  rms {np.sqrt((err**2).mean()):.3e}")
    print("per-frame max err (first 5):", err.max(axis=1)[:5])
