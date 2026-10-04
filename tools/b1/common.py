"""Shared helpers for the B1 decoder spike (Python side)."""
import os, struct, sys
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MODELS = os.path.join(ROOT, "models")
INSTRUMENTS = {"violin": "solo_violin_ckpt", "flute": "solo_flute_ckpt",
               "tenor_saxophone": "solo_tenor_saxophone_ckpt", "trumpet": "solo_trumpet_ckpt"}
FRAME_RATE, SAMPLE_RATE = 250, 16000
N_FRAMES = 1000                      # 4 s, the length the checkpoints were configured for
N_SAMPLES = N_FRAMES * (SAMPLE_RATE // FRAME_RATE)

def ckpt_prefix(instrument):
    d = os.path.join(MODELS, INSTRUMENTS[instrument])
    idx = [f[:-6] for f in os.listdir(d) if f.endswith(".index")]
    assert len(idx) == 1, f"expected one checkpoint in {d}"
    return d, os.path.join(d, idx[0])

def test_features():
    """Deterministic synthetic performance: a violin-range line with vibrato and a
    loudness envelope with a crescendo, a dip and a release (no audio needed)."""
    t = np.arange(N_FRAMES) / FRAME_RATE
    base = 220.0 * 2 ** (np.interp(t, [0, 1, 2, 3, 4], [0, 4, 7, 12, 9]) / 12.0)   # glide through a few notes
    vib = 1.0 + 0.012 * np.sin(2 * np.pi * 5.5 * t) * np.clip(t - 0.3, 0, 1)
    f0 = (base * vib).astype(np.float32)
    ld = np.interp(t, [0, 0.1, 1.2, 1.6, 3.2, 3.6, 4.0], [-70, -35, -22, -40, -18, -30, -75]).astype(np.float32)
    return f0, ld

def write_tensors(path, tensors):
    """Binary container read by src/ddsp/Weights.cpp. 'DDSPW1', u32 count, then per
    tensor: u32 nameLen, name, u32 ndim, u32 dims..., float32 data (C order)."""
    with open(path, "wb") as f:
        f.write(b"DDSPW1"); f.write(struct.pack("<I", len(tensors)))
        for name, arr in tensors:
            a = np.ascontiguousarray(arr, dtype=np.float32)
            nb = name.encode()
            f.write(struct.pack("<I", len(nb))); f.write(nb)
            f.write(struct.pack("<I", a.ndim)); f.write(struct.pack(f"<{a.ndim}I", *a.shape))
            f.write(a.tobytes())

def read_tensors(path):
    out = {}
    with open(path, "rb") as f:
        assert f.read(6) == b"DDSPW1"
        n, = struct.unpack("<I", f.read(4))
        for _ in range(n):
            ln, = struct.unpack("<I", f.read(4)); name = f.read(ln).decode()
            nd, = struct.unpack("<I", f.read(4)); dims = struct.unpack(f"<{nd}I", f.read(4 * nd))
            out[name] = np.frombuffer(f.read(4 * int(np.prod(dims))), dtype=np.float32).reshape(dims)
    return out
