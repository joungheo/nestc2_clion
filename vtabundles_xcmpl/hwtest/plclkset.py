#!/usr/bin/env python3
# #### JO added (2026-10-25): set or stop the PS clock pl_clk0 (CRL_APB PL0_REF_CTRL: SRCSEL [2:0], DIVISOR0 [13:8],
# DIVISOR1 [21:16], CLKACT [24]). For the Y219 at-speed wire test (y219route/wirespeed_run.sh): run the PL at a given
# frequency, stop the clock so that a JTAG readback sees a frozen state, and restore 100 MHz afterwards (the vtahw
# designs assume pl_clk0 = 100 MHz; plclk.py prints the current setting).
# usage: sudo python3 plclkset.py <MHz> | stop | start | show      (MHz: the PLL output / DIVISOR0, DIVISOR1 = 1)
import mmap
import os
import struct
import sys

PS_REF_MHZ = 33.333
CRL_APB = 0xFF5E0000
PLL_CTRL = {0: 0x20, 2: 0x30}   # IOPLL_CTRL, RPLL_CTRL
PL0_REF_CTRL = 0xC0

fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=CRL_APB)
rd = lambda off: struct.unpack('<I', m[off:off + 4])[0]


def wr(off, v):
    m[off:off + 4] = struct.pack('<I', v)


v = rd(PL0_REF_CTRL)
pll = rd(PLL_CTRL[v & 7])
pll_mhz = PS_REF_MHZ * ((pll >> 8) & 0x7f) / (2 if (pll >> 16) & 1 else 1)
arg = sys.argv[1]
if arg == 'stop':
    wr(PL0_REF_CTRL, v & ~(1 << 24))
elif arg == 'start':
    wr(PL0_REF_CTRL, v | (1 << 24))
elif arg != 'show':
    div = max(1, round(pll_mhz / float(arg)))
    wr(PL0_REF_CTRL, v & ~(1 << 24))                                        # stop, change, start
    nv = (v & ~((0x3f << 8) | (0x3f << 16) | (1 << 24))) | (div << 8) | (1 << 16)
    wr(PL0_REF_CTRL, nv)
    wr(PL0_REF_CTRL, nv | (1 << 24))
v = rd(PL0_REF_CTRL)
d0, d1 = (v >> 8) & 0x3f, (v >> 16) & 0x3f
print('pl_clk0: %.3f MHz (PLL %.3f / %d / %d), %s, PL0_REF_CTRL=0x%08x'
      % (pll_mhz / max(d0, 1) / max(d1, 1), pll_mhz, d0, d1, 'running' if v >> 24 & 1 else 'stopped', v))
