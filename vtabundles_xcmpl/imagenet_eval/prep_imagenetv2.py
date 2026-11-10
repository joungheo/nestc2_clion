#!/usr/bin/env python3
#### JO added (2026-11-10): ImageNetV2 matched-frequency (10,000 images, 10 per class, folder name = ImageNet class index)
#### to 224 x 224 RGB PNGs as the DeiT preprocessing does before the normalization (resize the short side to 256 bicubic,
#### center crop 224; the bundle main does / 255, - mean, / std). Writes <out>/<class>/<name>.png and <out>/list.txt with
#### "<path relative to out> <class>" lines; eval_list.sh makes the absolute list for the board.
#### usage: prep_imagenetv2.py <imagenetv2-matched-frequency-format-val dir> <out dir>
import os, sys
from multiprocessing import Pool
from PIL import Image

SRC, OUT = sys.argv[1], sys.argv[2]


def one(job):
    cls, name = job
    dst = os.path.join(OUT, cls, os.path.splitext(name)[0] + '.png')
    if not os.path.exists(dst):
        im = Image.open(os.path.join(SRC, cls, name)).convert('RGB')
        w, h = im.size
        s = 256.0 / min(w, h)
        im = im.resize((max(256, round(w * s)), max(256, round(h * s))), Image.BICUBIC)
        w, h = im.size
        l, t = (w - 224) // 2, (h - 224) // 2
        im.crop((l, t, l + 224, t + 224)).save(dst)
    return '%s/%s %s' % (cls, os.path.basename(dst), cls)


if __name__ == '__main__':
    jobs = []
    for cls in sorted(os.listdir(SRC), key=int):
        os.makedirs(os.path.join(OUT, cls), exist_ok=True)
        jobs += [(cls, n) for n in sorted(os.listdir(os.path.join(SRC, cls)))]
    with Pool() as p:
        lines = p.map(one, jobs, chunksize=64)
    # interleave the classes (image k of every class, then k + 1) so that any prefix of the list covers all classes
    by = {}
    for ln in lines:
        by.setdefault(ln.split()[1], []).append(ln)
    order = []
    for k in range(max(len(v) for v in by.values())):
        order += [v[k] for v in by.values() if k < len(v)]
    open(os.path.join(OUT, 'list.txt'), 'w').write('\n'.join(order) + '\n')
    print(len(order), 'images')
