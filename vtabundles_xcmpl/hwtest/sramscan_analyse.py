#!/usr/bin/env python3
# #### JO added (2026-10-23): where do the wrong weights come from? Reads sramscan_wgt[_invert].bin
# (./sramscan wgt [invert] dump) and tests, for every wrong byte got[w][oc][ic], which pattern value it is:
#   addr  : the value of entry w+d (d = -8..8, d != 0)       -> write or read one or more cycles off
#   byte  : byte ic+d of the same entry/bank                  -> shifted inside the 128-bit word
#   bank  : bank oc' of the same entry/byte                   -> banks mixed up
#   prev  : the complemented pattern (the other sramscan run) -> the write did not happen
#   const : the same value for every entry of that bank/byte  -> stuck output
# usage: python3 sramscan_analyse.py sramscan_wgt.bin [invert]
import collections
import sys

CHUNK = 256


def pat(a, b, c, inv):
    v = ((a * 37 + b * 11 + c * 5 + 3) & 0xFF) ^ (((a >> 3) * 0x55) & 0xFF)
    return (~v & 0xFF) if inv else v


fn = sys.argv[1]
inv = len(sys.argv) > 2 and sys.argv[2] == 'invert'
raw = open(fn, 'rb').read()
# #### JO changed (2026-10-24): depth from the dump size (single 2048, multi-EVTA 1024 weight entries)
WGT_DEPTH = len(raw) // 256
assert WGT_DEPTH % CHUNK == 0 and len(raw) == WGT_DEPTH * 256, 'unexpected size %d' % len(raw)
got = [[[0] * 16 for _ in range(16)] for _ in range(WGT_DEPTH)]
for base in range(0, WGT_DEPTH, CHUNK):
    blk = raw[base * 256:(base + CHUNK) * 256]
    for c in range(CHUNK):
        for k in range(16):
            for oc in range(16):
                got[base + c][oc][k] = blk[(c * 16 + k) * 16 + oc]

for oc in range(16):
    wrong = [(w, ic) for w in range(WGT_DEPTH) for ic in range(16) if got[w][oc][ic] != pat(w, oc, ic, inv)]
    if not wrong:
        print('bank %2d: all %d bytes OK' % (oc, WGT_DEPTH * 16))
        continue
    hits = collections.Counter()
    for w, ic in wrong:
        g = got[w][oc][ic]
        for d in range(-8, 9):
            if d and 0 <= w + d < WGT_DEPTH and g == pat(w + d, oc, ic, inv):
                hits['addr%+d' % d] += 1
            if d and 0 <= ic + d < 16 and g == pat(w, oc, ic + d, inv):
                hits['byte%+d' % d] += 1
        for o2 in range(16):
            if o2 != oc and g == pat(w, o2, ic, inv):
                hits['bank%d' % o2] += 1
        if g == pat(w, oc, ic, not inv):
            hits['prev'] += 1
    per_byte_const = sum(1 for ic in range(16) if len({got[w][oc][ic] for w in range(WGT_DEPTH)}) == 1)
    xors = collections.Counter(got[w][oc][ic] ^ pat(w, oc, ic, inv) for w, ic in wrong)
    print('bank %2d: %5d of %d bytes wrong; constant bytes %d/16; best matches %s; top xor %s'
          % (oc, len(wrong), WGT_DEPTH * 16, per_byte_const,
             ', '.join('%s=%d' % kv for kv in hits.most_common(4)) or 'none',
             ', '.join('0x%02x:%d' % kv for kv in xors.most_common(3))))
    # a few raw rows to look at by eye
    for w in (0, 1, 2, 3, 8, 9):
        print('    w=%4d got %s' % (w, ' '.join('%02x' % got[w][oc][ic] for ic in range(16))))
        print('           exp %s' % ' '.join('%02x' % pat(w, oc, ic, inv) for ic in range(16)))
