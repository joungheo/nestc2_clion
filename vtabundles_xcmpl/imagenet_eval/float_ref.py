#!/usr/bin/env python3
#### JO added (2026-11-10): top-1 of the float ONNX model (onnxruntime, CPU) on the PNGs of prep_imagenetv2.py, the same
#### input as the bundle main gets (/ 255, ImageNet mean / std, NCHW). Writes <out>.txt "<n> <pred> <class>" per image.
#### usage: float_ref.py <model.onnx> <png dir> [n images] [out prefix]
import sys, numpy as np, onnxruntime as ort
from PIL import Image

model, d = sys.argv[1], sys.argv[2]
lst = [l.split() for l in open(d + '/list.txt')]
if len(sys.argv) > 3:
    lst = lst[:int(sys.argv[3])]
out = sys.argv[4] if len(sys.argv) > 4 else 'float_ref'
s = ort.InferenceSession(model, providers=['CPUExecutionProvider'])
iname = s.get_inputs()[0].name
mean = np.array([0.485, 0.456, 0.406], np.float32)[:, None, None]
std = np.array([0.229, 0.224, 0.225], np.float32)[:, None, None]
ok, rows = 0, []
for i, (p, c) in enumerate(lst):
    x = np.asarray(Image.open(d + '/' + p), np.float32).transpose(2, 0, 1) / 255.0
    x = ((x - mean) / std)[None]
    y = s.run(None, {iname: x})[0].reshape(-1)
    pred = int(y.argmax())
    ok += pred == int(c)
    rows.append('%d %d %s' % (i + 1, pred, c))
    if (i + 1) % 1000 == 0:
        print(i + 1, ok, flush=True)
open(out + '.txt', 'w').write('\n'.join(rows) + '\n')
print('float top1 %d / %d = %.2f %%' % (ok, len(lst), 100.0 * ok / len(lst)))
