#!/usr/bin/env python3
# #### JO added (2026-10-25): evaluate the Y219 wire test (WireTest.java, WireTestBit.tcl). For each source value v
# <out>_<v>.bit was loaded with fpga_manager and read back over JTAG (vtabundles_xcmpl/hwtest/brambist/readback.tcl,
# no capture) into <rb_v>.bin. The logic location file <out>_<v>.ll gives the LUT bits of each sink SRL
# (Block=<site> Ram=<LUT>:<bit>); how its bit offsets map onto the bitstream's FDRI words (offset, bit order) is
# found from the SRL INIT values in <out>_<v>.bit, and the readback is aligned to the FDRI data as in cfgdiff.py.
# The SRL bits are those that differ between the two bitstreams (INIT 1 vs 0). A tested node is bad when its SRL
# did not fill with the source value in one of the two runs.
# usage: python3 wiretest.py <wt.txt> <out> <rb0.bin> <rb1.bin> > bad_nodes.txt
import collections
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../../../vtabundles_xcmpl/hwtest/brambist'))
from cfgdiff import RB_PAD, fdri  # noqa: E402


def ll_ram(path):
    """(site, LUT letter) -> list of bit offsets"""
    ram = collections.defaultdict(list)
    for line in open(path):
        m = re.match(r'Bit\s+(\d+)\s.*Block=(\S+) Ram=([A-H]):\d+', line)
        if m:
            ram[(m.group(2), m.group(3))].append(int(m.group(1)))
    return ram


def bitval(words, offs, k, msb):
    w = offs // 32 + k
    sh = (31 - offs % 32) if msb else (offs % 32)
    return (words[w] >> sh.astype(np.uint32)) & 1


def main():
    txt, out, rbs = sys.argv[1], sys.argv[2], sys.argv[3:5]
    tests = [l.split() for l in open(txt)]
    #### nets whose SRL is not on the routed pin (WireTest.java <wt>.badpin.txt) are no test
    bp = txt[:-4] + '.badpin.txt'
    if os.path.exists(bp):
        skip = set(open(bp).read().split())
        sys.stderr.write('%d nets left out (sink pin differs from the route)\n' % len(skip & {t[0] for t in tests}))
        tests = [t for t in tests if t[0] not in skip]
    sink = {t[0]: tuple(re.match(r'\S+@(\S+)/([A-H])6LUT', t[3]).groups()) for t in tests}
    ram = ll_ram('%s_0.ll' % out)
    b = [fdri('%s_%d.bit' % (out, v)) for v in (0, 1)]
    offs = np.array(sorted(o for s in sink.values() for o in ram[s]), dtype=np.int64)
    # alignment: the bits where the two bitstreams differ are SRL INIT bits (1 in _0.bit)
    best = None
    sample = offs[::max(1, len(offs) // 2000)]   # a sample is enough to find the offset (-93 words, lsb first)
    for msb in (False, True):
        for k in range(-20000, 20000, 1):
            w = sample // 32 + k
            if w.min() < 0 or w.max() >= len(b[0]):
                continue
            d = bitval(b[0], sample, k, msb) & (1 - bitval(b[1], sample, k, msb))
            hit = int(d.sum())
            if best is None or hit > best[0]:
                best = (hit, k, msb)
    hit, k, msb = best
    sys.stderr.write('alignment: %d of %d sampled LUT bits are 1 in _0.bit and 0 in _1.bit (word offset %d, %s first)\n'
                     % (hit, len(sample), k, 'msb' if msb else 'lsb'))
    bad = {}
    for v in (0, 1):
        r = np.frombuffer(open(rbs[v], 'rb').read(), dtype='>u4').astype(np.uint32)[RB_PAD:]
        nbad = 0
        for t in tests:
            o = np.array(ram[sink[t[0]]], dtype=np.int64)
            srl = (bitval(b[0], o, k, msb) == 1) & (bitval(b[1], o, k, msb) == 0)
            got = bitval(r, o[srl], k, msb)
            if srl.sum() == 0 or np.any(got != v):
                bad.setdefault(t[1], []).append((v, int(np.count_nonzero(got == v)), int(srl.sum())))
                nbad += 1
        sys.stderr.write('source %d: %d of %d sinks wrong\n' % (v, nbad, len(tests)))
    for t in tests:
        if t[1] in bad:
            print(t[1], ' '.join('v%d:%d/%d' % x for x in bad[t[1]]), t[2], t[3])
    sys.stderr.write('bad nodes: %d of %d\n' % (len(bad), len(tests)))


if __name__ == '__main__':
    main()
