#!/usr/bin/env python3
"""Fix the input shape of an ONNX model and constant-fold every node whose inputs are known (initializers, Constant
nodes, Shape of statically shaped tensors), so the Glow importer gets static shapes (DeiT-tiny: the HF export computes
its reshapes from Shape / Gather / Where, 'could not find constant ...'). usage: fold_onnx.py in.onnx out.onnx N,C,H,W

#### JO added (2026-11-09): DeiT-tiny (gen_deit_bundle.sh); 1261 -> 608 nodes, outputs identical to the original
"""
import collections
import sys

import numpy as np
import onnx
from onnx import helper, numpy_helper, shape_inference
from onnx.reference import ReferenceEvaluator

src, dst, shp = sys.argv[1], sys.argv[2], [int(x) for x in sys.argv[3].split(',')]
m = onnx.load(src)
g = m.graph
for d, v in zip(g.input[0].type.tensor_type.shape.dim, shp):
    d.Clear()
    d.dim_value = v


def static_shapes(model):
    mi = shape_inference.infer_shapes(model)
    out = {}
    for vi in list(mi.graph.value_info) + list(mi.graph.input) + list(mi.graph.output):
        dims = vi.type.tensor_type.shape.dim
        if dims and all(d.HasField('dim_value') for d in dims):
            out[vi.name] = [d.dim_value for d in dims]
    return out


const = {i.name: numpy_helper.to_array(i) for i in g.initializer}
HEAVY = {'MatMul', 'Gemm', 'Conv'}
changed = True
while changed:
    changed = False
    shapes = static_shapes(m)
    keep = []
    for n in g.node:
        vals = None
        if n.op_type == 'Constant':
            vals = [numpy_helper.to_array(n.attribute[0].t)]
        elif n.op_type == 'Shape' and n.input[0] in shapes:
            vals = [np.array(shapes[n.input[0]], dtype=np.int64)]
        elif n.op_type not in HEAVY and all(i in const for i in n.input if i):
            ins = [i for i in dict.fromkeys(n.input) if i]
            sub = helper.make_model(
                helper.make_graph([n], 'f', [helper.make_tensor_value_info(
                    i, helper.np_dtype_to_tensor_dtype(const[i].dtype), const[i].shape) for i in ins],
                    [helper.make_tensor_value_info(o, 0, None) for o in n.output]),
                opset_imports=m.opset_import)
            vals = ReferenceEvaluator(sub).run(None, {i: const[i] for i in ins})
            # do not materialize big tensors computed from weights (keep e.g. weight transposes as nodes)
            if any(np.asarray(v).size > 4096 for v in vals) and n.op_type not in ('Expand', 'ConstantOfShape'):
                vals = None
        if vals is None:
            keep.append(n)
            continue
        for o, v in zip(n.output, vals):
            const[o] = np.asarray(v)
            g.initializer.append(numpy_helper.from_array(np.asarray(v), o))
        changed = True
    del g.node[:]
    g.node.extend(keep)
# drop unused initializers
used = {i for n in g.node for i in n.input} | {o.name for o in g.output}
inits = [i for i in g.initializer if i.name in used]
del g.initializer[:]
g.initializer.extend(inits)
del g.value_info[:]
m = shape_inference.infer_shapes(m)
onnx.checker.check_model(m)
onnx.save(m, dst)
print(len(m.graph.node), 'nodes',
      sorted(collections.Counter(n.op_type for n in m.graph.node).items(), key=lambda x: -x[1]))
