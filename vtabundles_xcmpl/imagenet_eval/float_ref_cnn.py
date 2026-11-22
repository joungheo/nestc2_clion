#!/usr/bin/env python3
#### JO added (2026-11-22): top-1 of a float ONNX model (onnxruntime, CPU) on a labeled PNG set with the same input tensor as
#### the bundle's main makes it (multi_model.h of vta/bundles/<Dir>: channel order, value range, layout), so that the bundle's
#### top-1 (eval_board.sh) minus this is the loss of the int8 EVTA path alone. float_ref.py is the DeiT one (mean / std).
#### usage: float_ref_cnn.py <model> <model.onnx> <png dir with list.txt "<png> <class>"> [n images] [out prefix]
####   model: resnet18 | resnet50 | vgg16 | mobilenetv2 | deit
#### needs onnxruntime, numpy, pillow (e.g. a venv: python3 -m venv ~/.venvs/onnxeval; pip install onnxruntime numpy pillow)
import sys, numpy as np, onnxruntime as ort
from PIL import Image

# channel order of the bundle input, value range, layout, model class - list class
MODELS = {
    'resnet18': ('RGB', (0.0, 255.0), 'NHWC', 0),      # mxnet_exported_resnet18BundleMain.cpp (NHWC 1)
    'resnet50': ('BGR', (0.0, 1.0), 'NCHW', 0),        # caffe2_resnet50Main.cpp
    'vgg16': ('RGB', (0.0, 1.0), 'NCHW', 0),           # vgg16BundleMain.cpp
    'mobilenetv2': ('RGB', (-1.0, 1.0), 'NCHW', 1),    # TFLite: 1001 classes, 0 = background
    'deit': ('RGB', None, 'NCHW', 0),                  # / 255, ImageNet mean / std (float_ref.py)
}

kind, model, d = sys.argv[1], sys.argv[2], sys.argv[3]
order, rng, layout, off = MODELS[kind]
lst = [l.split() for l in open(d + '/list.txt')]
if len(sys.argv) > 4:
    lst = lst[:int(sys.argv[4])]
out = sys.argv[5] if len(sys.argv) > 5 else 'float_ref_' + kind
s = ort.InferenceSession(model, providers=['CPUExecutionProvider'])
iname = s.get_inputs()[0].name
mean = np.array([0.485, 0.456, 0.406], np.float32)[:, None, None]
std = np.array([0.229, 0.224, 0.225], np.float32)[:, None, None]
ok, rows = 0, []
for i, (p, c) in enumerate(lst):
    x = np.asarray(Image.open(d + '/' + p).convert('RGB'), np.float32).transpose(2, 0, 1)  # C H W, RGB, 0..255
    if order == 'BGR':
        x = x[::-1]
    if rng is None:
        x = (x / 255.0 - mean) / std
    else:
        x = x / 255.0 * (rng[1] - rng[0]) + rng[0]
    if layout == 'NHWC':
        x = x.transpose(1, 2, 0)
    y = s.run(None, {iname: np.ascontiguousarray(x[None], np.float32)})[0].reshape(-1)
    pred = int(y.argmax())
    ok += pred == int(c) + off
    rows.append('%d %d %s' % (i + 1, pred, c))
    if (i + 1) % 1000 == 0:
        print(i + 1, ok, flush=True)
open(out + '.txt', 'w').write('\n'.join(rows) + '\n')
print('float top1 %d / %d = %.2f %%' % (ok, len(lst), 100.0 * ok / len(lst)))
