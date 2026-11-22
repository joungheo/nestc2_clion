#!/usr/bin/env python3
#### JO added (2026-11-22, docs/evta_todo.md §5 DPU comparison): ImageNet val (ILSVRC2012, 50,000 JPEGs) to 224 x 224 RGB PNGs
#### with the preprocessing of the model's own evaluation, so that the bundle's top-1 is comparable with the published float /
#### DPU numbers (the bundle mains only scale the bytes: multi_model.h MODEL_RANGE_*):
####   tv: torchvision (Resnet50TvTest): resize the short side to 256 (PIL bilinear, antialiased, as transforms.Resize), centre
####       crop 224 (transforms.CenterCrop rounding)
####   tf: TF-slim inception_preprocessing for eval (MobileNetV2TfTest / MobileNetV2ValTest): tf.image.central_crop(0.875) (offset
####       h // 16, w // 16), then tf.image.resize_bilinear to 224 x 224 (TF 1 legacy: src = dst * in / out, no antialiasing)
#### Class: the synset (n########) in the file name (HF ILSVRC/imagenet-1k val_images: ILSVRC2012_val_00000001_n01751748.JPEG)
#### or in the parent dir name (val/<synset>/*.JPEG); class index = position in the sorted 1000 synsets (the usual index, as
#### torchvision / TF-slim - 1 / ImageNetV2's folders).
#### Writes <out>/<class>/<name>.png and <out>/list.txt ("<path relative to out> <class>", classes interleaved as prep_imagenetv2.py
#### so that any prefix covers all classes).
#### usage: prep_imagenet_val.py <tv|tf> <dir with the val JPEGs, searched recursively> <out dir>
import os, re, sys
from multiprocessing import Pool
import numpy as np
from PIL import Image

MODE, SRC, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
assert MODE in ('tv', 'tf')
SYN = re.compile(r'(n\d{8})')


def tf_resize_bilinear(a, oh, ow):
    """tf.compat.v1.image.resize_bilinear(align_corners=False, half_pixel_centers=False) of an HxWxC float array."""
    h, w = a.shape[:2]
    ys = np.arange(oh) * (h / oh)
    xs = np.arange(ow) * (w / ow)
    y0 = np.floor(ys).astype(int); x0 = np.floor(xs).astype(int)
    y1 = np.minimum(y0 + 1, h - 1); x1 = np.minimum(x0 + 1, w - 1)
    dy = (ys - y0)[:, None, None]; dx = (xs - x0)[None, :, None]
    top = a[y0][:, x0] * (1 - dx) + a[y0][:, x1] * dx
    bot = a[y1][:, x0] * (1 - dx) + a[y1][:, x1] * dx
    return top * (1 - dy) + bot * dy


def one(job):
    src, cls = job
    dst = os.path.join(OUT, str(cls), os.path.splitext(os.path.basename(src))[0] + '.png')
    if not os.path.exists(dst):
        im = Image.open(src).convert('RGB')
        w, h = im.size
        if MODE == 'tv':
            if w <= h:
                nw, nh = 256, int(256 * h / w)
            else:
                nw, nh = int(256 * w / h), 256
            im = im.resize((nw, nh), Image.BILINEAR)
            t, l = int(round((nh - 224) / 2.0)), int(round((nw - 224) / 2.0))
            im = im.crop((l, t, l + 224, t + 224))
        else:
            a = np.asarray(im, dtype=np.float32)
            oy, ox = h // 16, w // 16
            a = a[oy:h - oy, ox:w - ox]
            a = tf_resize_bilinear(a, 224, 224)
            # the bundle reads bytes: round to the nearest byte (TF keeps the float; the loss is below one byte step)
            im = Image.fromarray(np.clip(np.rint(a), 0, 255).astype(np.uint8))
        im.save(dst)
    return '%d/%s %d' % (cls, os.path.basename(dst), cls)


if __name__ == '__main__':
    files = []
    for root, _, names in os.walk(SRC):
        for n in names:
            if n.lower().endswith(('.jpeg', '.jpg', '.png')):
                m = SYN.search(n) or SYN.search(os.path.basename(root))
                if m:
                    files.append((os.path.join(root, n), m.group(1)))
    syns = sorted({s for _, s in files})
    assert len(syns) == 1000, '%d synsets found' % len(syns)
    idx = {s: i for i, s in enumerate(syns)}
    for i in range(1000):
        os.makedirs(os.path.join(OUT, str(i)), exist_ok=True)
    jobs = sorted((f, idx[s]) for f, s in files)
    with Pool() as p:
        lines = p.map(one, jobs, chunksize=64)
    by = {}
    for ln in lines:
        by.setdefault(ln.split()[1], []).append(ln)
    order = []
    for k in range(max(len(v) for v in by.values())):
        order += [v[k] for v in by.values() if k < len(v)]
    open(os.path.join(OUT, 'list.txt'), 'w').write('\n'.join(order) + '\n')
    print(len(order), 'images', MODE)
