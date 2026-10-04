"""Reference features for the B2 tracking path, computed with Magenta's own DDSP code.

Writes tests/fixtures/ddaw/b2/:
  signal_16k.wav     a deterministic test signal (chirp, tone with vibrato, noise burst, silence, quiet tone)
  loudness_ref.csv   ddsp.spectral_ops.compute_loudness(signal) - one value per 250 fps frame
Run with the B1 environment:  .venv-b1/bin/python tools/b2/ref_features.py
"""
import os, struct
import numpy as np
import ddsp.spectral_ops as so

SR = 16000
OUT = os.path.join(os.path.dirname(__file__), "..", "..", "tests", "fixtures", "ddaw", "b2")
os.makedirs(OUT, exist_ok=True)

rng = np.random.RandomState(7)
t = lambda s: np.arange(int(s * SR)) / SR
parts = []
# 0.5 s exponential chirp 110 Hz -> 1760 Hz, fading in
f = 110.0 * (1760.0 / 110.0) ** (t(0.5) / 0.5)
parts.append(0.5 * np.sin(2 * np.pi * np.cumsum(f) / SR) * np.linspace(0.1, 1.0, len(f)))
# 0.5 s 440 Hz with 5 Hz vibrato
tt = t(0.5)
parts.append(0.4 * np.sin(2 * np.pi * np.cumsum(440.0 * (1 + 0.02 * np.sin(2 * np.pi * 5 * tt))) / SR))
# 0.25 s of white noise
parts.append(0.3 * rng.randn(int(0.25 * SR)))
# 0.25 s of digital silence
parts.append(np.zeros(int(0.25 * SR)))
# 0.5 s quiet 220 Hz (-50 dBFS) then 0.25 s of 3 kHz tone
parts.append(0.003 * np.sin(2 * np.pi * 220.0 * t(0.5)))
parts.append(0.25 * np.sin(2 * np.pi * 3000.0 * t(0.25)))
x = np.concatenate(parts).astype(np.float32)

ld = np.asarray(so.compute_loudness(x, sample_rate=SR, frame_rate=250, n_fft=512, use_tf=False), dtype=np.float64)

def write_wav(path, data):
    with open(path, "wb") as f:
        n = len(data)
        f.write(b"RIFF" + struct.pack("<I", 36 + 4 * n) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 3, 1, SR, SR * 4, 4, 32))
        f.write(b"data" + struct.pack("<I", 4 * n) + data.astype("<f4").tobytes())

write_wav(os.path.join(OUT, "signal_16k.wav"), x)
np.savetxt(os.path.join(OUT, "loudness_ref.csv"), ld, fmt="%.6f")
print(f"signal {len(x)} samples, {len(ld)} loudness frames, range {ld.min():.2f}..{ld.max():.2f} dB")
