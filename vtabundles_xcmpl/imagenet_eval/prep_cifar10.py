#!/usr/bin/env python3
#### JO added (2026-11-22): the CIFAR-10 test set (cifar-10-python.tar.gz of https://www.cs.toronto.edu/~kriz/cifar.html)
#### as 10,000 32 x 32 PNGs and list.txt "<png> <class>" for eval_board.sh (VGG16 CIFAR-10 eval executable) and float_ref_cnn.py.
#### The tar holds Python pickles: read with an unpickler that only builds numpy arrays (no other class can be loaded).
#### usage: prep_cifar10.py <cifar-10-python.tar.gz> <out dir> [train <n>]
#### 2026-11-22: "train <n>": the first n images of data_batch_1 (training set) instead, e.g. for the calibration profile
#### (not the test images the accuracy is measured on).
import io, os, pickle, sys, tarfile
import numpy as np
from PIL import Image

ALLOWED = {('numpy.core.multiarray', '_reconstruct'), ('numpy', 'ndarray'), ('numpy', 'dtype'),
           ('numpy.core.multiarray', 'scalar'), ('_codecs', 'encode')}


class SafeUnpickler(pickle.Unpickler):
    def find_class(self, module, name):
        if (module, name) in ALLOWED:
            return super().find_class(module, name)
        raise pickle.UnpicklingError(f'not allowed: {module}.{name}')


tgz, out = sys.argv[1], sys.argv[2]
train = len(sys.argv) > 4 and sys.argv[3] == 'train'
nmax = int(sys.argv[4]) if train else None
os.makedirs(out, exist_ok=True)
with tarfile.open(tgz) as t:
    m = t.getmember('cifar-10-batches-py/' + ('data_batch_1' if train else 'test_batch'))
    d = SafeUnpickler(io.BytesIO(t.extractfile(m).read()), encoding='bytes').load()
x = np.asarray(d[b'data'], np.uint8).reshape(-1, 3, 32, 32).transpose(0, 2, 3, 1)   # N H W C, RGB
y = list(d[b'labels'])[:nmax]
rows = []
for i in range(len(y)):
    p = f'{y[i]}/{i:05d}.png'
    os.makedirs(f'{out}/{y[i]}', exist_ok=True)
    Image.fromarray(x[i], 'RGB').save(f'{out}/{p}')
    rows.append(f'{p} {y[i]}')
open(f'{out}/list.txt', 'w').write('\n'.join(rows) + '\n')
print(len(rows), 'images')
