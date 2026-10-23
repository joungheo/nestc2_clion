#!/usr/bin/env python3
# #### JO added (2026-10-24): does a bitstream need any of the stuck-at-0 configuration cells of the lab ZCU102?
# The cells (found with readback.tcl + cfgdiff.py) are, in every type-0 frame of clock-region row 3 (frame index
# 20586..27350 of the FDRI data), frame word 61 bit 18 and word 62 bits 0-3, 12-14 = bit offsets 2, 16-19, 28-30
# of the 48-bit slot of tile row 39 of the region (SLICE Y219 / RAMB36 Y43). The board always reads these as 0, so
# every 1 a bitstream has there is lost on the board.
# Every design seen so far sets the same 194 of them (word 62 bit 0, the configuration of unused tiles), and the
# designs that work on the board set exactly those and no more. With -b <base.bit> (e.g. the single _lowbram or
# y219 build) only the cells set beyond the base are reported: 0 extra = the design is configured as written.
# usage: python3 stuckchk.py <design.bit> [-m <design.msk>] [-b <base.bit>]   (exit 1 if any (extra) cell is set)
import argparse
import collections
import sys

import numpy as np

from cfgdiff import FRAME_WORDS, fdri

ROW3_FRAMES = (20586, 27350)
STUCK = {61: (18,), 62: (0, 1, 2, 3, 12, 13, 14)}


def hits(bit, msk=None):
    """set of (frame index, word, bit) of the stuck cells this bitstream sets to 1 (dynamic bits excluded)"""
    b = fdri(bit)
    m = fdri(msk) if msk else np.zeros_like(b)
    f = np.arange(ROW3_FRAMES[0], ROW3_FRAMES[1] + 1)
    s = set()
    for w, bits in STUCK.items():
        v = b[f * FRAME_WORDS + w] & ~m[f * FRAME_WORDS + w]
        for k in bits:
            s.update((int(i), w, k) for i in f[np.nonzero(v >> k & 1)[0]])
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bit')
    ap.add_argument('-m', '--mask')
    ap.add_argument('-b', '--base')
    a = ap.parse_args()
    h = hits(a.bit, a.mask)
    msg = '%s: %d bits set in stuck cells' % (a.bit, len(h))
    if a.base:
        base = hits(a.base)
        msg += ', %d beyond the base, %d of the base not set' % (len(h - base), len(base - h))
        h -= base
    print(msg)
    pos = collections.Counter((w, k) for _, w, k in h)
    for (w, k), c in sorted(pos.items()):
        print('  word %d bit %2d : %d' % (w, k, c))
    if h:
        fr = [i for i, _, _ in h]
        print('  frame index range %d..%d' % (min(fr), max(fr)))
    sys.exit(1 if h else 0)


if __name__ == '__main__':
    main()
