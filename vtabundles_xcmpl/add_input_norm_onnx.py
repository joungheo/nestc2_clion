#!/usr/bin/env python3
#### JO added (2026-11-22): put the per-channel input normalization a model was trained with into the ONNX graph:
#### x -> (x - mean) / std on the NCHW input, so the bundle main keeps feeding what it fed before (0..1 RGB for VGG16
#### CIFAR-10) and VTASave -vta-fuse-preprocess folds the Sub / Div into the conv0 input copy (no extra pass).
#### usage: add_input_norm_onnx.py <in.onnx> <out.onnx> <m0,m1,m2> <s0,s1,s2>
import sys
import numpy as np
import onnx
from onnx import helper, numpy_helper

src, dst = sys.argv[1], sys.argv[2]
mean = np.array([float(v) for v in sys.argv[3].split(',')], np.float32).reshape(1, -1, 1, 1)
std = np.array([float(v) for v in sys.argv[4].split(',')], np.float32).reshape(1, -1, 1, 1)
m = onnx.load(src)
g = m.graph
inits = {i.name for i in g.initializer}
inp = [i for i in g.input if i.name not in inits][0].name
sub_out, div_out = inp + '_sub_mean', inp + '_norm'
for n in g.node:
    for k, v in enumerate(n.input):
        if v == inp:
            n.input[k] = div_out
g.initializer.extend([numpy_helper.from_array(mean, inp + '_mean'), numpy_helper.from_array(std, inp + '_std')])
nodes = [helper.make_node('Sub', [inp, inp + '_mean'], [sub_out], name=inp + '_sub'),
         helper.make_node('Div', [sub_out, inp + '_std'], [div_out], name=inp + '_div')]
for k, n in enumerate(nodes):
    g.node.insert(k, n)
onnx.checker.check_model(m)
onnx.save(m, dst)
print('input', inp, 'mean', mean.ravel(), 'std', std.ravel(), '->', dst)
