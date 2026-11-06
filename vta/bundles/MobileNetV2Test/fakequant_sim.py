# fake-quant simulation of MobileNetV2 (float ONNX from tfl2onnx.py) with EVTA-like int8 symmetric quantization
#### JO added (2026-11-06): which tensors the EVTA quantization (int8 symmetric, power-of-two scales, per tensor) breaks:
#### weights / activations power-of-two or not, depthwise weights per channel; onnxruntime + Pillow. usage: fakequant_sim.py <model.onnx>
#### Result for mobilenetv2_qat_f32.onnx: only the per-tensor power-of-two depthwise weights break it (cat 293, dog 557);
#### per-channel power-of-two depthwise weights (or any-scale ones) give the fp32 classes for cat / dog / zebra.
import sys, numpy as np, onnx, onnxruntime as ort
from onnx import helper, numpy_helper, TensorProto
from PIL import Image
IMG = __import__('os').path.dirname(__import__('os').path.abspath(__file__)) + '/../../../glow/tests/images/imagenet/'
imgs = {f: np.asarray(Image.open(IMG + f).convert('RGB'), dtype=np.float32) for f in ('cat_285.png', 'dog_207.png', 'zebra_340.png')}
X = {f: (a / 255 * 2 - 1).transpose(2, 0, 1)[None].astype(np.float32) for f, a in imgs.items()}
base = onnx.load(sys.argv[1])
# activation ranges on the calibration image (as Glow's profile: one image)
m = onnx.ModelProto(); m.CopyFrom(base)
acts = [n.output[0] for n in m.graph.node]
for a in acts: m.graph.output.append(helper.make_tensor_value_info(a, TensorProto.FLOAT, None))
outs = ort.InferenceSession(m.SerializeToString()).run(acts, {'input': X['cat_285.png']})
rng = {a: float(np.abs(o).max()) for a, o in zip(acts, outs)}
rng['input'] = 1.0
ref = {f: ort.InferenceSession(base.SerializeToString()).run(None, {'input': x})[0][0] for f, x in X.items()}

def sc(r, p2):
    s = max(r, 1e-8) / 127
    return 2.0 ** np.ceil(np.log2(s)) if p2 else s

def build(wp2, ap2, wpc=False, dwp2=None, dwpc=None):
    m = onnx.ModelProto(); m.CopyFrom(base)
    W = {t.name: numpy_helper.to_array(t) for t in m.graph.initializer}
    newinit, nodes = [], []
    qa = lambda name: name  # renamed outputs
    def qdq(x, s, tag):
        sn = tag + '_s'; zn = tag + '_z'
        newinit.extend([numpy_helper.from_array(np.array(s, np.float32), sn), numpy_helper.from_array(np.array(0, np.int8), zn)])
        nodes.append(helper.make_node('QuantizeLinear', [x, sn, zn], [tag + '_q']))
        nodes.append(helper.make_node('DequantizeLinear', [tag + '_q', sn, zn], [tag + '_dq']))
        return tag + '_dq'
    ren = {'input': qdq('input', sc(1.0, ap2), 'input')}
    pre = list(nodes); nodes.clear()
    body = []
    clipin = {n.input[0] for n in m.graph.node if n.op_type == 'Clip'}  # conv + relu6 fused: only the clip output is int8
    for n in m.graph.node:
        for i, x in enumerate(n.input):
            if x in ren: n.input[i] = ren[x]
        if n.op_type == 'Conv':
            w = W[n.input[1]]; grp = [a.i for a in n.attribute if a.name == 'group'][0]
            dw = grp > 1
            p2 = (dwp2 if dw and dwp2 is not None else wp2); pc = (dwpc if dw and dwpc is not None else wpc)
            r = np.abs(w).reshape(w.shape[0], -1).max(1) if pc else np.full(w.shape[0], np.abs(w).max())
            s = np.array([sc(v, p2) for v in r])[:, None, None, None]
            wq = np.clip(np.round(w / s), -127, 127) * s
            for t in m.graph.initializer:
                if t.name == n.input[1]: t.CopyFrom(numpy_helper.from_array(wq.astype(np.float32), t.name))
        body.append(n)
        nodes.clear()
        if n.op_type in ('Clip', 'Add') or (n.op_type == 'Conv' and n.output[0] not in clipin):
            o = n.output[0]
            body.extend([]); q = qdq(o, sc(rng[o], ap2), o.replace('/', '_') + '_fq'); body.extend(nodes); ren[o] = q
    for o in m.graph.output:
        pass
    del m.graph.node[:]; m.graph.node.extend(pre + body); m.graph.initializer.extend(newinit)
    # output: Identity reads a renamed tensor already
    return m

def run(name, **kw):
    m = build(**kw); s = ort.InferenceSession(m.SerializeToString())
    res = []
    for f, x in X.items():
        o = s.run(['output'], {'input': x})[0][0]
        res.append('%s %d/%d' % (f[:3], np.argmax(o), np.argmax(ref[f])))
    top = [np.argmax(s.run(['output'], {'input': x})[0][0]) == np.argmax(ref[f]) for f, x in X.items()]
    print('%-46s %s  %s' % (name, ' '.join(res), 'OK' if all(top) else 'BAD'))

run('float scales (w, a)', wp2=False, ap2=False)
run('p2 w + p2 a (EVTA now)', wp2=True, ap2=True)
run('p2 a, float w', wp2=False, ap2=True)
run('float a, p2 w', wp2=True, ap2=False)
run('p2 a, p2 w, depthwise w float', wp2=True, ap2=True, dwp2=False)
run('p2 a, p2 w per-channel', wp2=True, ap2=True, wpc=True)
run('p2 a, p2 w, depthwise per-channel p2', wp2=True, ap2=True, dwpc=True)
run('float a, float w per-channel', wp2=False, ap2=False, wpc=True)
