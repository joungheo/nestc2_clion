#!/usr/bin/env python3
# #### JO added (2026-10-20): print the PS-side settings a bitstream depends on but does not set itself:
# the PL clocks pl_clk0..3 and the PL-side data width of the S_AXI_HPC0/1 and HP0-3 ports (AFIFM).
# Writing a bitstream through fpga_manager changes neither (they come from the boot image, or from the
# last PYNQ Overlay() load). The vtahw_* designs assume pl_clk0 = 100 MHz (clk_wiz x 3.33 -> 333 MHz;
# the MMCM still locks for ~84-167 MHz, so a different pl_clk0 silently over/under-clocks EVTA) and
# 128-bit HP0 (single) / HP0-3 (multi4).
# usage: sudo python3 plclk.py
import mmap
import os
import struct

PS_REF_MHZ = 33.333
CRL_APB = 0xFF5E0000
PLL_CTRL = {0: ('IOPLL', 0x20), 2: ('RPLL', 0x30)}
PL_REF_CTRL = [0xC0, 0xC4, 0xC8, 0xCC]  # PL0..PL3_REF_CTRL

fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=CRL_APB)


def reg(off):
    return struct.unpack('<I', m[off:off + 4])[0]


def pll_mhz(off):
    v = reg(off)
    fbdiv = (v >> 8) & 0x7f
    div2 = (v >> 16) & 1
    return PS_REF_MHZ * fbdiv / (2 if div2 else 1), v


for i, off in enumerate(PL_REF_CTRL):
    v = reg(off)
    src = v & 7
    div0 = (v >> 8) & 0x3f
    div1 = (v >> 16) & 0x3f
    act = (v >> 24) & 1
    if src in PLL_CTRL:
        name, poff = PLL_CTRL[src]
        pll, pv = pll_mhz(poff)
        mhz = pll / max(div0, 1) / max(div1, 1)
        print('pl_clk%d: %.3f MHz  (%s %.3f MHz / %d / %d, active=%d, PL%d_REF_CTRL=0x%08x, %s_CTRL=0x%08x)'
              % (i, mhz, name, pll, div0, div1, act, i, v, name, pv))
    else:
        print('pl_clk%d: source %d (DPLL to LPD), div %d / %d, active=%d, PL%d_REF_CTRL=0x%08x'
              % (i, src, div0, div1, act, i, v))

# AFIFM i serves S_AXI_GP i: RDCTRL (+0x00) / WRCTRL (+0x14) bits[1:0] = fabric width 0:128 1:64 2:32 bit
AFIFM = (('HPC0', 0xFD360000), ('HPC1', 0xFD370000), ('HP0', 0xFD380000),
         ('HP1', 0xFD390000), ('HP2', 0xFD3A0000), ('HP3', 0xFD3B0000))
WIDTH = {0: 128, 1: 64, 2: 32, 3: '?'}
for name, base in AFIFM:
    a = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=base)
    rd = struct.unpack('<I', a[0x00:0x04])[0]
    wr = struct.unpack('<I', a[0x14:0x18])[0]
    print('S_AXI_%-4s: read %s bit, write %s bit  (RDCTRL=0x%08x WRCTRL=0x%08x)'
          % (name, WIDTH[rd & 3], WIDTH[wr & 3], rd, wr))
