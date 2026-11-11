#!/usr/bin/env python3
#### JO added (2026-11-10): int8 fake quantization of the DeiT ONNX on the PC (onnxruntime, QuantizeLinear / DequantizeLinear after
#### every float node output and on the MatMul / Gemm / Conv weights), to try integer formulas and scale choices without the
#### board. Calibration: max |x| per tensor over the 100 calibration images of the board bundles (every 10th of the last 1000
#### list entries), symmetric scale max / 127.5 as Glow's symmetric schema, rounded up to a power of two with --pow2.
#### Options: --ln-float / --gelu-float / --softmax-float leave the inside of those blocks unquantized (their outputs stay
#### int8), --skip <substring> leaves matching tensors unquantized, --exact <substring> gives them a free scale with --pow2.
#### usage: fakequant.py <model.onnx> <png dir> [--n 9000] [--pow2] [--ln-float] [--gelu-float] [--softmax-float] [--skip s]...
import argparse, os, sys, numpy as np, onnx, onnxruntime as ort
from onnx import helper, numpy_helper, TensorProto
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument('model'); ap.add_argument('png')
ap.add_argument('--n', type=int, default=9000)
ap.add_argument('--pow2', action='store_true')
ap.add_argument('--ln-float', action='store_true')
ap.add_argument('--gelu-float', action='store_true')
ap.add_argument('--softmax-float', action='store_true')
ap.add_argument('--no-weights', action='store_true')
ap.add_argument('--skip', action='append', default=[])
ap.add_argument('--exact', action='append', default=[], help='with --pow2: exact (not power-of-two) scale for tensors containing this')
ap.add_argument('--out', default='')
ap.add_argument('--threads', type=int, default=0, help='onnxruntime intra-op threads (0: all cores)')
ap.add_argument('--per-channel', action='store_true', help='weights per output channel (MatMul column, Gemm / Conv row); with --ratio out the output keeps the per-tensor ratio rounding of the largest channel, with --ratio weight each channel is rounded up to a power-of-two ratio (a per-channel shift)')
ap.add_argument('--ratio', choices=['', 'out', 'weight', 'best'], default='',
                help='free scales, but every MatMul / Gemm / Conv (+ bias) has a power-of-two in * w / out ratio (EVTA shift): '
                     'out = round the output scale up; weight = round the weight scale up (activation output exact); best = the smaller of the two per op; '
                     'activation x activation matmuls always round the output')
a = ap.parse_args()

lst = [l.split() for l in open(a.png + '/list.txt')]
calib = [p for i, (p, c) in enumerate(lst[-1000:]) if i % 10 == 0]
mean = np.array([0.485, 0.456, 0.406], np.float32)[:, None, None]
std = np.array([0.229, 0.224, 0.225], np.float32)[:, None, None]
def load(p):
    x = np.asarray(Image.open(a.png + '/' + p), np.float32).transpose(2, 0, 1) / 255.0
    return ((x - mean) / std)[None]

m = onnx.load(a.model)
g = m.graph
inits = {t.name for t in g.initializer}
ftypes = {}
mi = onnx.shape_inference.infer_shapes(m)
for vi in list(mi.graph.value_info) + list(mi.graph.input) + list(mi.graph.output):
    ftypes[vi.name] = vi.type.tensor_type.elem_type
def region(name):
    if 'layernorm' in name and not name.endswith('/Add_1_output_0'): return 'ln'
    if 'intermediate_act_fn' in name and not name.endswith('/Mul_1_output_0'): return 'gelu'
    if '/attention/attention/Div_output_0' in name: return 'other'
    return 'other'
cands = ['pixel_values'] + [o for n in g.node for o in n.output if ftypes.get(o) == TensorProto.FLOAT]
def keep(t):
    r = region(t)
    if a.ln_float and r == 'ln': return False
    if a.gelu_float and r == 'gelu': return False
    if a.softmax_float and t.endswith('/attention/attention/Div_output_0'): return False
    return not any(s in t for s in a.skip)
qt = [t for t in cands if keep(t)]

# calibration
cm = onnx.ModelProto(); cm.CopyFrom(m)
have = {o.name for o in cm.graph.output}
for t in cands:
    if t not in have and t != 'pixel_values':
        cm.graph.output.append(helper.make_tensor_value_info(t, TensorProto.FLOAT, None))
so = ort.SessionOptions(); so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
if a.threads: so.intra_op_num_threads = a.threads; so.inter_op_num_threads = 1
s = ort.InferenceSession(cm.SerializeToString(), so, providers=['CPUExecutionProvider'])
names = [o.name for o in s.get_outputs()]
amax = {}
for p in calib:
    x = load(p)
    amax['pixel_values'] = max(amax.get('pixel_values', 0), float(np.abs(x).max()))
    for n, v in zip(names, s.run(None, {'pixel_values': x})):
        amax[n] = max(amax.get(n, 0), float(np.abs(v).max()))
def scale(mx, t=''):
    if t in fixed: return fixed[t]
    sc = max(mx, 1e-8) / 127.5
    return float(2.0 ** np.ceil(np.log2(sc))) if a.pow2 and not any(e in t for e in a.exact) else sc
fixed = {}

# --ratio: the EVTA ops' requantize is a shift: in * w / out = 2^-k
cons = {}
for n in g.node:
    for i in n.input: cons.setdefault(i, []).append(n)
skipq = set()
chscale = {}
if a.ratio:
    for n in g.node:
        if n.op_type not in ('MatMul', 'Gemm', 'Conv'): continue
        out = n.output[0]
        c = cons.get(out, [])
        if n.op_type == 'MatMul' and len(c) == 1 and c[0].op_type == 'Add' and any(i in inits for i in c[0].input):
            skipq.add(out); out = c[0].output[0]      # bias folded: the MatMul output is not quantized on its own
        xin = n.input[0]
        if xin not in amax or out not in amax: continue
        si = scale(amax[xin]); so_ = scale(amax[out])
        if n.input[1] in inits:
            w = numpy_helper.to_array(next(t for t in g.initializer if t.name == n.input[1]))
            sw = max(float(np.abs(w).max()), 1e-8) / 127.5
            if a.per_channel and a.ratio == 'weight':
                chscale[n.output[0]] = (lambda si, so_: lambda sc: so_ / (si * 2.0 ** np.floor(np.log2(so_ / (si * sc)))))(si, so_)
                fixed[out] = so_
                continue
            r = so_ / (si * sw); fo = 2.0 ** np.ceil(np.log2(r)) / r   # output up by fo, or weight up by 2 / fo
            if a.ratio == 'best' and fo <= 2.0 / fo:
                fixed[n.input[1]] = sw; fixed[out] = float(si * sw * 2.0 ** np.ceil(np.log2(r)))
            elif a.ratio in ('weight', 'best'):
                # sw' = so / (si * 2^k) >= sw, the smallest such: k = floor(log2(so / (si * sw)))
                kk = np.floor(np.log2(so_ / (si * sw)))
                fixed[n.input[1]] = float(so_ / (si * 2.0 ** kk)); fixed[out] = so_
            else:
                fixed[n.input[1]] = sw
                fixed[out] = float(si * sw * 2.0 ** np.ceil(np.log2(so_ / (si * sw))))
        elif n.input[1] in amax:
            sw = scale(amax[n.input[1]])
            fixed[out] = float(si * sw * 2.0 ** np.ceil(np.log2(so_ / (si * sw))))
    print('ratio-adjusted tensors:', len(fixed), flush=True)

# fake-quant model
nodes, k = [], 0
def qdq(t):
    global k
    k += 1
    sn, zn, q, d = 'fq_s%d' % k, 'fq_z%d' % k, 'fq_q%d' % k, t + '__fq'
    g.initializer.extend([numpy_helper.from_array(np.array(scale(amax[t], t), np.float32), sn),
                          numpy_helper.from_array(np.array(0, np.int8), zn)])
    return [helper.make_node('QuantizeLinear', [t, sn, zn], [q]), helper.make_node('DequantizeLinear', [q, sn, zn], [d])], d
ren = {}
pre, rn = qdq('pixel_values'); nodes += pre; ren['pixel_values'] = rn
for n in list(g.node):
    for i, inp in enumerate(n.input):
        if inp in ren: n.input[i] = ren[inp]
    if not a.no_weights and n.op_type in ('MatMul', 'Gemm', 'Conv') and n.input[1] in inits:
        w = numpy_helper.to_array(next(t for t in g.initializer if t.name == n.input[1]))
        amax[n.input[1]] = float(np.abs(w).max())
        if a.per_channel and w.ndim >= 2:
            ax = 1 if n.op_type == 'MatMul' else 0
            if n.op_type == 'Gemm' and not any(at.name == 'transB' and at.i for at in n.attribute): ax = 1
            red = tuple(i for i in range(w.ndim) if i != ax)
            sc = np.maximum(np.abs(w).max(axis=red), 1e-8) / 127.5
            if a.ratio == 'weight' and n.output[0] in chscale:
                sc = chscale[n.output[0]](sc)
            # opset 11 has no per-axis QuantizeLinear: the weight is replaced by its quantized-dequantized values
            shp = [1] * w.ndim; shp[ax] = -1; scb = sc.reshape(shp)
            wq = np.clip(np.round(w / scb), -128, 127) * scb
            t = next(t for t in g.initializer if t.name == n.input[1])
            t.CopyFrom(numpy_helper.from_array(wq.astype(np.float32), t.name))
        else:
            wq, wd = qdq(n.input[1]); nodes += wq; n.input[1] = wd
    nodes.append(n)
    for o in n.output:
        if o in qt and o in amax and o not in skipq:
            q, d = qdq(o); nodes += q; ren[o] = d
outs = [o.name for o in g.output]
for o in g.output:
    if o.name in ren:   # graph output: keep the name on the dequantized value
        nodes.append(helper.make_node('Identity', [ren[o.name]], [o.name + '__out']))
        o.name = o.name + '__out'
del g.node[:]; g.node.extend(nodes)
s = ort.InferenceSession(m.SerializeToString(), so, providers=['CPUExecutionProvider'])
ok, rows = 0, []
for i, (p, c) in enumerate(lst[:a.n]):
    y = s.run(None, {'pixel_values': load(p)})[0].reshape(-1)
    pred = int(y.argmax()); ok += pred == int(c); rows.append('%d %d %s' % (i + 1, pred, c))
if a.out: open(a.out, 'w').write('\n'.join(rows) + '\n')
print('%s: top1 %d / %d = %.2f %%' % (' '.join(sys.argv[3:]), ok, a.n, 100.0 * ok / a.n), flush=True)
