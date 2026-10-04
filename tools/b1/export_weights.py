#!/usr/bin/env python3
"""Export a Magenta DDSP solo-instrument checkpoint to DDAW's tensor container.

Reads the TF checkpoint with tf.train.load_checkpoint (no model code needed) and
writes models/export/<instrument>.ddspw. Layout conversions match RTNeural:
  Dense  kernel [in,out] -> weights [out][in] (transposed), bias [out]
  GRU    kernel [in,3N], recurrent_kernel [N,3N], bias [2,3N]: unchanged (TF reset_after)
  LayerNorm gamma/beta, Reverb IR: unchanged
"""
import os, sys
import numpy as np
import tensorflow as tf
from common import *

P = "model/decoder/"
V = "/.ATTRIBUTES/VARIABLE_VALUE"

def export(instrument):
    d, prefix = ckpt_prefix(instrument)
    r = tf.train.load_checkpoint(prefix)
    get = lambda k: r.get_tensor(k + V)
    T = []
    for s, name in ((0, "ld"), (1, "f0")):
        for i in range(3):
            b = f"{P}input_stacks/{s}/layer_with_weights-{i}/"
            T += [(f"{name}.{i}.W", get(b + "layer_with_weights-0/kernel").T),
                  (f"{name}.{i}.b", get(b + "layer_with_weights-0/bias")),
                  (f"{name}.{i}.gamma", get(b + "layer_with_weights-1/gamma")),
                  (f"{name}.{i}.beta", get(b + "layer_with_weights-1/beta"))]
    T += [("gru.W", get(P + "rnn/rnn/cell/kernel")), ("gru.U", get(P + "rnn/rnn/cell/recurrent_kernel")),
          ("gru.b", get(P + "rnn/rnn/cell/bias"))]
    for i in range(3):
        b = f"{P}out_stack/layer_with_weights-{i}/"
        T += [(f"out.{i}.W", get(b + "layer_with_weights-0/kernel").T),
              (f"out.{i}.b", get(b + "layer_with_weights-0/bias")),
              (f"out.{i}.gamma", get(b + "layer_with_weights-1/gamma")),
              (f"out.{i}.beta", get(b + "layer_with_weights-1/beta"))]
    T += [("dense_out.W", get(P + "dense_out/kernel").T), ("dense_out.b", get(P + "dense_out/bias")),
          ("reverb.ir", r.get_tensor("model/processor_group/reverb/ir" + V))]
    shapes = {n: a.shape for n, a in T}
    assert shapes["gru.W"] == (1024, 1536) and shapes["gru.U"] == (512, 1536) and shapes["gru.b"] == (2, 1536)
    assert shapes["out.0.W"] == (512, 1536) and shapes["dense_out.W"] == (126, 512)
    out = os.path.join(MODELS, "export"); os.makedirs(out, exist_ok=True)
    path = os.path.join(out, instrument + ".ddspw")
    write_tensors(path, T)
    n = sum(a.size for _, a in T)
    print(f"{instrument}: {len(T)} tensors, {n/1e6:.2f}M floats -> {path}")

if __name__ == "__main__":
    for inst in (sys.argv[1:] or INSTRUMENTS):
        export(inst)
