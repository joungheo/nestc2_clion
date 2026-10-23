#!/usr/bin/env python3
# #### JO added (2026-10-23): compare a JTAG configuration readback (readback.tcl -> <rb>.bin) with the bitstream.
# The .msk comes from `write_bitstream -mask_file` of the same routed checkpoint (mask bit 1 = dynamic bit: BRAM /
# LUTRAM content, FF state; not compared). Prints the differing bits per frame word / bit position.
# On the lab ZCU102 every design shows the same stuck-at-0 cells: clock-region row 3, frame words 61/62
# (word 61 bit 18, word 62 bits 0-3, 12-14) = the tile row at SLICE Y219 / RAMB36 Y43.
# usage: python3 cfgdiff.py <design.bit> <design.msk> <readback.bin>
import collections
import struct
import sys

import numpy as np

FRAME_WORDS = 93     # UltraScale+
RB_PAD = 118         # readback starts with a dummy frame + pad words (found by alignment)


def fdri(path):
    """the configuration data of the (single) large FDRI write of a .bit / .msk"""
    d = open(path, 'rb').read()
    i = d.index(b'\xaa\x99\x55\x66') + 4
    w = struct.unpack('>%dI' % ((len(d) - i) // 4), d[i:i + (len(d) - i) // 4 * 4])
    best, k, reg = (), 0, None
    while k < len(w):
        h = w[k]
        if h >> 29 == 1:
            reg, n = (h >> 13) & 0x3FFF, h & 0x7FF
        elif h >> 29 == 2:
            n = h & 0x7FFFFFF
        else:
            k += 1
            continue
        if (h >> 27) & 3 == 2 and reg == 2 and n > len(best):
            best = w[k + 1:k + 1 + n]
        k += 1 + n
    return np.array(best, dtype=np.uint32)


b, m = fdri(sys.argv[1]), fdri(sys.argv[2])
r = np.frombuffer(open(sys.argv[3], 'rb').read(), dtype='>u4').astype(np.uint32)[RB_PAD:RB_PAD + len(b)]
x = (r ^ b) & ~m
idx = np.nonzero(x)[0]
pc = lambda a: int(np.unpackbits(a.view(np.uint8)).sum())
print('compared bits differing: %d (1->0 %d, 0->1 %d) in %d frames' % (pc(x), pc(x & b), pc(x & r), len(set(idx // FRAME_WORDS))))
if len(idx):
    print('frame index range %d..%d' % (idx.min() // FRAME_WORDS, idx.max() // FRAME_WORDS))
    pos = collections.Counter()
    for i in idx:
        v = int(x[i])
        for k in range(32):
            if v >> k & 1:
                pos[(int(i) % FRAME_WORDS, k)] += 1
    print('frame word / bit : count')
    for (w, k), c in sorted(pos.items()):
        print('  %2d / %2d : %d' % (w, k, c))
