#!/usr/bin/env python3
"""Independent reference for the M0 stub devices (StubTone + StubGain).

Re-implements the spec in pure Python (no shared code with the C++ engine) and
writes the golden for tests/fixtures/ddaw/projects/stub-tone-gain.json, with the
same 0.02 s lead-in synthyy's goldens have. Regenerate with:
    python3 tests/fixtures/make_stub_golden.py
"""
import json, math, os, struct

HERE = os.path.dirname(os.path.abspath(__file__))
SR, PPQ, TAIL, LEAD, REL = 44100, 96, 1.0, 0.02, 64

def rnd(x):  # round half away from zero, like std::llround
    return int(math.floor(x + 0.5)) if x >= 0 else -int(math.floor(-x + 0.5))

def render_track(track, clips, scene_ticks, fpt, total):
    inst = track["inst"]["params"]; level = inst.get("level", 0.25)
    events = []
    clip = clips.get(track["id"] + "|s1")
    if clip:
        base = 0
        while base < scene_ticks:
            for k, n in enumerate(sorted(clip["notes"])):
                on, off = base + clip["notes"][n]["s"], base + clip["notes"][n]["s"] + clip["notes"][n]["d"]
                if on >= scene_ticks: continue
                nid = (base, n)
                events.append((rnd(on * fpt), 1, nid, clip["notes"][n]))
                events.append((rnd(off * fpt), 0, nid, None))
            base += clip["len"]
    events.sort(key=lambda e: (e[0], e[1]))  # offs (0) before ons (1) at the same frame
    out = [0.0] * total
    voices, ei = {}, 0
    for i in range(total):
        while ei < len(events) and events[ei][0] <= i:
            _, on, nid, n = events[ei]; ei += 1
            if on:
                hz = 440.0 * 2 ** ((n["p"] - 69) / 12.0)
                voices[nid] = {"phase": 0.0, "inc": 2 * math.pi * hz / SR, "amp": n["v"] * level, "rel": None}
            elif nid in voices and voices[nid]["rel"] is None:
                voices[nid]["rel"] = 0
        s = 0.0
        for nid in list(voices):
            v = voices[nid]; env = 1.0
            if v["rel"] is not None:
                env = 1.0 - v["rel"] / REL
                v["rel"] += 1
                if v["rel"] > REL:
                    del voices[nid]; continue
            s += v["amp"] * env * math.sin(v["phase"]); v["phase"] += v["inc"]
        out[i] = s
    return out

def pan(l, r, p):
    p = max(-1.0, min(1.0, p))
    if p == 0: return l, r
    x = p + 1.0 if p <= 0 else p
    gl, gr = math.cos(x * math.pi / 2), math.sin(x * math.pi / 2)
    return (l + r * gl, r * gr) if p <= 0 else (l * gl, r + l * gr)

def main():
    fx = json.load(open(os.path.join(HERE, "ddaw/projects/stub-tone-gain.json")))
    p = fx["project"]; bpm = p["meta"]["bpm"]
    fpt = SR * 60.0 / bpm / PPQ
    longest = max([4 * PPQ] + [c["len"] for c in p["clips"].values()])
    scene_ticks = longest * 2
    horizon = scene_ticks + TAIL * bpm / 60.0 * PPQ   # clips keep looping through the tail
    total = int(math.ceil(scene_ticks * fpt + TAIL * SR))
    L, R = [0.0] * total, [0.0] * total
    for t in p["tracks"]:
        sig = render_track(t, p["clips"], horizon, fpt, total)
        for f in t["fx"]:
            if f["type"] == "stubgain" and f["on"]:
                g = f["params"].get("gain", 1.0); sig = [x * g for x in sig]
        g = 10 ** (t["gain"] / 20.0); m = 10 ** (p["meta"]["masterGain"] / 20.0)
        for i, x in enumerate(sig):
            l, r = pan(x, x, t["pan"]); L[i] += l * g * m; R[i] += r * g * m
    lead = rnd(LEAD * SR)
    data = bytearray()
    q = lambda x: struct.pack("<h", max(-32768, min(32767, rnd(max(-1.0, min(1.0, x)) * 32767))))
    for _ in range(lead): data += q(0) + q(0)
    for l, r in zip(L, R): data += q(l) + q(r)
    out = os.path.join(HERE, "ddaw/golden/stub-tone-gain.wav")
    with open(out, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, SR, SR * 4, 4, 16))
        f.write(b"data" + struct.pack("<I", len(data))); f.write(data)
    print("wrote", out, len(data) // 4, "frames")

main()
