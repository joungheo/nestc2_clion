#!/usr/bin/env python3
# #### JO added (2026-10-23): do clock and reset work in the upper PL region? Write/read the HLS AXI-lite
# data registers of the four EVTA blocks bit by bit, then pulse pl_resetn0 and check they read 0 again.
# On the 333 MHz wgt16 vtahw bitstream (md5 3911d84c...) these flip-flops sit at SLICE Y177..190, i.e. on
# both sides of the clock-region row 2/3 boundary (Y180 = BRAM row Y36, where the wgt banks start to fail).
# Never starts EVTA (no ap_start), so it cannot overwrite memory.
# usage: sudo python3 regtest.py            (load the bitstream first; see the SLICE Y map below)
import mmap
import os
import struct
import subprocess
import sys

BASE = 0xA0000000
# (block offset, register offset, name, SLICE Y of bit 0..31) from the routed design (clkchk/bits.tcl)
Y_FETCH = [177, 177, 178, 179, 177, 180, 183, 179, 180, 180, 180, 180, 182, 182, 182, 182,
           184, 182, 182, 183, 182, 182, 182, 182, 183, 182, 183, 183, 184, 184, 183, 184]
Y_LOAD = [179, 178, 181, 181, 184, 186, 181, 180, 186, 186, 184, 186, 189, 185, 189, 188,
          188, 183, 189, 188, 186, 186, 189, 188, 189, 183, 188, 185, 188, 186, 188, 188]
Y_UOPS = [183, 183, 182, 185, 187, 187, 184, 183, 187, 187, 187, 190, 185, 187, 189, 190,
          188, 183, 189, 184, 187, 184, 188, 187, 185, 183, 188, 188, 188, 188, 188, 186]
Y_BIAS = [183, 183, 182, 182, 187, 181, 184, 183, 187, 187, 187, 187, 185, 185, 189, 188,
          188, 182, 189, 187, 187, 184, 188, 187, 185, 183, 188, 188, 188, 187, 188, 188]
Y_STORE = [181, 179, 179, 180, 183, 183, 181, 180, 181, 181, 183, 183, 183, 183, 186, 186,
           184, 184, 185, 185, 184, 184, 185, 185, 184, 184, 186, 186, 186, 186, 185, 185]
REGS = [(0x0000, 0x10, 'fetch.insn_count', Y_FETCH), (0x0000, 0x18, 'fetch.insns', Y_FETCH),
        (0x1000, 0x10, 'load.inputs', Y_LOAD), (0x1000, 0x18, 'load.weights', Y_LOAD),
        (0x2000, 0x20, 'compute.uops', Y_UOPS), (0x2000, 0x28, 'compute.biases', Y_BIAS),
        (0x3000, 0x10, 'store.outputs', Y_STORE)]
PATTERNS = [0xFFFFFFFF, 0xA5A5A5A5, 0x5A5A5A5A, 0x00000001, 0x80000000, 0x00000000]

fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 0x4000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=BASE)


def rd(a):
    return struct.unpack('<I', m[a:a + 4])[0]


def wr(a, v):
    m[a:a + 4] = struct.pack('<I', v)


def bits(x, ys):
    return ', '.join('b%d(Y%d)' % (i, ys[i]) for i in range(32) if (x >> i) & 1)


bad = 0
for blk, off, name, ys in REGS:
    for p in PATTERNS:
        wr(blk + off, p)
        got = rd(blk + off)
        if got != p:
            bad += 1
            print('WRITE %-16s wrote 0x%08x read 0x%08x  wrong: %s' % (name, p, got, bits(got ^ p, ys)))
    wr(blk + off, 0xFFFFFFFF)
print('write/read: %s' % ('OK' if bad == 0 else '%d mismatches' % bad))

print('ctrl before reset:', ' '.join('0x%x' % rd(b) for b in (0, 0x1000, 0x2000, 0x3000)))
subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'plreset.py')], check=True)
nz = 0
for blk, off, name, ys in REGS:
    got = rd(blk + off)
    if got:
        nz += 1
        print('RESET %-16s still 0x%08x after pl_resetn0  not reset: %s' % (name, got, bits(got, ys)))
print('ctrl after reset: ', ' '.join('0x%x' % rd(b) for b in (0, 0x1000, 0x2000, 0x3000)))
print('reset: %s' % ('OK (all data registers 0)' if nz == 0 else '%d registers not cleared' % nz))
