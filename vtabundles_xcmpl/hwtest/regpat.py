#!/usr/bin/env python3
# #### JO added (2026-10-25): write/read back patterns (0, ~0, 0xAAAAAAAA, 0x55555555) to the address registers of
# the EVTA HLS blocks, without ap_start, and print per block the bits that read 1 when written 0 / 0 when written 1.
# A wrong bit is a broken register read/write data path (on the lab ZCU102: nets crossing tile row Y219).
# usage: sudo python3 regpat.py 0 1 3   (EVTA indices; only those the bitstream has)
import sys
from pynq import MMIO
REGS = {'fetch': (0x0, (0x10, 0x18)), 'load': (0x1000, (0x10, 0x18)),
        'compute': (0x2000, (0x20, 0x28)), 'store': (0x3000, (0x10,))}
PATS = (0x00000000, 0xFFFFFFFF, 0xAAAAAAAA, 0x55555555)
for e in map(int, sys.argv[1:]):
    for name, (off, regs) in REGS.items():
        base = 0xA0000000 + e * 0x10000 + off
        m = MMIO(base, 0x1000)
        ctrl = m.read(0)
        s1, s0 = 0, 0      # bits that read 1 when written 0 / read 0 when written 1
        for r in regs:
            for p in PATS:
                for _ in range(3):
                    m.write(r, p)
                    v = m.read(r)
                    s1 |= v & ~p & 0xFFFFFFFF
                    s0 |= p & ~v & 0xFFFFFFFF
        f = lambda x: [b for b in range(32) if x >> b & 1]
        print(f'evta{e} {name:7s} ctrl={ctrl:#010x}  read1-when-0: {f(s1)}  read0-when-1: {f(s0)}', flush=True)
