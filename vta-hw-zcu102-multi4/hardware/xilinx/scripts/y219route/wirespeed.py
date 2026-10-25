#!/usr/bin/env python3
# #### JO added (2026-10-25): evaluate the Y219 at-speed wire test (WireTest.java -Dspeed=1, WireTestBit.tcl,
# wirespeed_run.sh). Each source SRL circulates 32'h1F35AC93 and the sink SRL across the row records what arrives.
# After a run at some pl_clk0 frequency the clock was stopped and the PL read back over JTAG. If the path is faster
# than a clock period, the sink holds exactly the source's 32 bits (rotation 0); one cycle late gives rotation 1;
# anything else (no rotation of the source, or the source itself not a rotation of the pattern) is a failure.
# An SRLC32E keeps each stage k in two LUT bits, .ll Ram=<LUT>:2k+1 = the value and Ram=<LUT>:2k = its complement
# (checked on the INIT in the bitstream); the stage order (ascending or descending, maybe inverted) is confirmed on
# the sources' INIT in <out>_p.bit. <out>_n.bit (every INIT inverted) only serves to find the bit alignment.
# usage: python3 wirespeed.py <ws.txt> <out> <label>=<rb.bin> ...   > per-node results
import collections
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../../../vtabundles_xcmpl/hwtest/brambist'))
from cfgdiff import RB_PAD, fdri  # noqa: E402

P = 0x1F35AC93
M32 = 0xFFFFFFFF
rot = lambda x, r: ((x >> r) | (x << (32 - r))) & M32 if r else x


def ll_ram(path):
    """(site, LUT letter) -> [(ram index, bit offset)]"""
    ram = collections.defaultdict(list)
    for line in open(path):
        m = re.match(r'Bit\s+(\d+)\s.*Block=(\S+) Ram=([A-H]):(\d+)', line)
        if m:
            ram[(m.group(2), m.group(3))].append((int(m.group(4)), int(m.group(1))))
    return ram


def bitval(words, offs, k, msb):
    offs = np.asarray(offs, dtype=np.int64)
    sh = (31 - offs % 32) if msb else (offs % 32)
    return (words[offs // 32 + k] >> sh.astype(np.uint32)) & 1


def main():
    txt, out = sys.argv[1], sys.argv[2]
    runs = [a.split('=', 1) for a in sys.argv[3:]]
    tests = [l.split() for l in open(txt)]
    loc = lambda s: tuple(re.match(r'\S+@(\S+)/([A-H])6LUT', s).groups())
    ram = ll_ram('%s_p.ll' % out)
    bp, bn = fdri('%s_p.bit' % out), fdri('%s_n.bit' % out)
    allo = np.array([o for t in tests for c in (t[2], t[3]) for _, o in ram[loc(c)]], dtype=np.int64)
    sample = allo[::max(1, len(allo) // 3000)]
    best = None
    for msb in (False, True):
        for k in range(-20000, 20000):
            w = sample // 32 + k
            if w.min() < 0 or w.max() >= len(bp):
                continue
            hit = int(np.count_nonzero(bitval(bp, sample, k, msb) != bitval(bn, sample, k, msb)))
            if best is None or hit > best[0]:
                best = (hit, k, msb)
    _, K, MSB = best
    sys.stderr.write('alignment: %d of %d sampled LUT bits differ between _p and _n (word offset %d)\n' % (best[0], len(sample), K))

    def srl_bits(cell):
        """bit offsets of the 32 SRL stages of a LUT (the odd .ll indices), in stage order"""
        e = sorted(ram[loc(cell)])
        return np.array([x[1] for x in e if x[0] % 2 == 1], dtype=np.int64)

    def word(words, offs, order, inv):
        b = bitval(words, offs, K, MSB)
        b = b[::-1] if order else b
        v = int(sum(int(x) << i for i, x in enumerate(b)))
        return v ^ M32 if inv else v

    # stage order: which reading of the sources' bits in _p.bit gives P
    votes = collections.Counter()
    for t in tests[:200]:
        o = srl_bits(t[2])
        if len(o) != 32:
            continue
        for order in (0, 1):
            for inv in (0, 1):
                if word(bp, o, order, inv) == P:
                    votes[(order, inv)] += 1
    (ORDER, INV), nv = votes.most_common(1)[0]
    sys.stderr.write('stage order: %s, %s (%d of %d sources)\n' % ('descending' if ORDER else 'ascending', 'inverted' if INV else 'as is', nv, min(200, len(tests))))
    rots = {rot(P, r): r for r in range(32)}
    res = {t[0]: [] for t in tests}
    for label, rb in runs:
        r = np.frombuffer(open(rb, 'rb').read(), dtype='>u4').astype(np.uint32)[RB_PAD:]
        hist = collections.Counter()
        for t in tests:
            so, do = srl_bits(t[2]), srl_bits(t[3])
            if len(so) != 32 or len(do) != 32:
                res[t[0]].append('nobits'); hist['nobits'] += 1
                continue
            s, d = word(r, so, ORDER, INV), word(r, do, ORDER, INV)
            if s not in rots:
                c = 'src?'
            else:
                c = next((str(k) for k in range(32) if rot(s, k) == d), None) or next(
                    ('L' + str(k) for k in range(32) if rot(s, 32 - k) == d), 'bad')
            res[t[0]].append(c)
            hist[c] += 1
        sys.stderr.write('%s: %s\n' % (label, dict(hist.most_common(8))))
    for t in tests:
        print(t[0], t[1], ' '.join('%s:%s' % (runs[i][0], c) for i, c in enumerate(res[t[0]])))


if __name__ == '__main__':
    main()
