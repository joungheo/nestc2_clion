# TFLite (uint8 QAT) MobileNet -> float ONNX (NCHW): weights dequantized, fused RELU6 as Clip, SAME padding as explicit pads.
#### JO added (2026-11-06): TFLite (uint8 QAT) -> float ONNX for vta/bundles/MobileNetV2Test (see its CMakeLists.txt).
#### Needs the tflite, flatbuffers, onnx and numpy Python packages. usage: tfl2onnx.py <model.tflite> <out.onnx> [--relu]
import sys, numpy as np, tflite, onnx
from onnx import helper, numpy_helper, TensorProto
BO = tflite.BuiltinOperator
buf = open(sys.argv[1], 'rb').read()
m = tflite.Model.GetRootAsModel(buf, 0)
g = m.Subgraphs(0)
relu6 = '--relu' not in sys.argv   # --relu: RELU6 as Relu (for an experiment)

def tdata(i):
    t = g.Tensors(i)
    b = m.Buffers(t.Buffer()).DataAsNumpy()
    dt = {tflite.TensorType.UINT8: np.uint8, tflite.TensorType.INT32: np.int32,
          tflite.TensorType.FLOAT32: np.float32}[t.Type()]
    a = np.frombuffer(b.tobytes(), dtype=dt).reshape(t.ShapeAsNumpy())
    q = t.Quantization()
    if q is not None and q.ScaleLength():
        a = (a.astype(np.float64) - q.ZeroPoint(0)) * q.Scale(0)
    return a.astype(np.float32)

def tname(i): return g.Tensors(i).Name().decode()
def tshape(i): return g.Tensors(i).ShapeAsNumpy().tolist()

nodes, inits = [], []
def const(name, a):
    inits.append(numpy_helper.from_array(a, name)); return name

def same_pads(ih, iw, kh, kw, s):
    oh, ow = -(-ih // s), -(-iw // s)
    ph = max((oh - 1) * s + kh - ih, 0); pw = max((ow - 1) * s + kw - iw, 0)
    return [ph // 2, pw // 2, ph - ph // 2, pw - pw // 2]

def act(out, fused, ti=None):
    # old QAT models give no fused activation: RELU6 is the uint8 saturation of an output range [0, 6]
    if fused == tflite.ActivationFunctionType.NONE and ti is not None:
        q = g.Tensors(ti).Quantization()
        lo = (0 - q.ZeroPoint(0)) * q.Scale(0)
        if abs(lo) < 1e-6: fused = tflite.ActivationFunctionType.RELU6
    if fused == tflite.ActivationFunctionType.NONE: return out
    y = out + '_act'
    if fused == tflite.ActivationFunctionType.RELU6 and relu6:
        nodes.append(helper.make_node('Clip', [out], [y], min=0.0, max=6.0))
    else:
        nodes.append(helper.make_node('Relu', [out], [y]))
    return y

alias = {}
def nm(i): n = tname(i); return alias.get(n, n)
for k in range(g.OperatorsLength()):
    op = g.Operators(k)
    c = m.OperatorCodes(op.OpcodeIndex()); code = max(c.BuiltinCode(), c.DeprecatedBuiltinCode())
    ins = [op.Inputs(j) for j in range(op.InputsLength())]; out = op.Outputs(0)
    oname = tname(out) + '_f'
    if code in (BO.CONV_2D, BO.DEPTHWISE_CONV_2D):
        o = (tflite.Conv2DOptions if code == BO.CONV_2D else tflite.DepthwiseConv2DOptions)()
        t = op.BuiltinOptions(); o.Init(t.Bytes, t.Pos)
        w = tdata(ins[1]); b = tdata(ins[2])
        if code == BO.CONV_2D: w = w.transpose(0, 3, 1, 2); grp = 1          # OHWI -> OIHW
        else: w = w.transpose(3, 0, 1, 2); grp = w.shape[0]                  # 1HWC -> C1HW
        ih, iw = tshape(ins[0])[1:3]; s = o.StrideH(); assert s == o.StrideW()
        pads = same_pads(ih, iw, w.shape[2], w.shape[3], s) if o.Padding() == tflite.Padding.SAME else [0] * 4
        nodes.append(helper.make_node('Conv', [nm(ins[0]), const(tname(ins[1]), np.ascontiguousarray(w)), const(tname(ins[2]), b)],
                                      [oname], kernel_shape=list(w.shape[2:]), strides=[s, s], pads=pads, group=grp))
        alias[tname(out)] = act(oname, o.FusedActivationFunction(), out)
    elif code == BO.ADD:
        o = tflite.AddOptions(); t = op.BuiltinOptions(); o.Init(t.Bytes, t.Pos)
        nodes.append(helper.make_node('Add', [nm(ins[0]), nm(ins[1])], [oname]))
        alias[tname(out)] = act(oname, o.FusedActivationFunction(), out)
    elif code == BO.AVERAGE_POOL_2D:
        nodes.append(helper.make_node('GlobalAveragePool', [nm(ins[0])], [oname])); alias[tname(out)] = oname
    elif code == BO.RESHAPE:
        nodes.append(helper.make_node('Flatten', [nm(ins[0])], [oname], axis=1)); alias[tname(out)] = oname
    else:
        raise SystemExit('op %d not handled' % code)

iname, oname = tname(g.Inputs(0)), nm(g.Outputs(0))
nodes.append(helper.make_node('Identity', [oname], ['output']))
ish = tshape(g.Inputs(0)); osh = tshape(g.Outputs(0))
graph = helper.make_graph(nodes, 'mobilenet_v2_quant_float',
    [helper.make_tensor_value_info('input', TensorProto.FLOAT, [1, ish[3], ish[1], ish[2]])],
    [helper.make_tensor_value_info('output', TensorProto.FLOAT, osh)], inits)
# the graph input is named 'input' in NCHW; the first conv reads tflite's 'input' tensor, the same name
model = helper.make_model(graph, opset_imports=[helper.make_opsetid('', 10)], producer_name='tfl2onnx')
model.ir_version = 5
onnx.checker.check_model(model)
onnx.save(model, sys.argv[2])
print('nodes', len(nodes), 'saved', sys.argv[2])
