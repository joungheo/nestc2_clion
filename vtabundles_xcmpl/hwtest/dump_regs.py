#!/usr/bin/env python3
# Dump the AXI-lite register maps (offset 0x00-0x7C) of the four EVTA HLS blocks.
# Run right after a timed-out test (e.g. `sudo timeout 30 ./add_test_nc`) so the
# post-run state is preserved:   sudo python3 dump_regs.py
#
# The software driver assumes (vta/vtalib/CMakeLists.txt, ZCU102 branch):
#   fetch  : 0x10 insn_count, 0x18 insn_addr
#   load   : 0x10 inp_addr,   0x18 wgt_addr
#   compute: 0x10 done_wr,    0x18 done_rd, 0x20 uop_addr, 0x28 bias_addr
#   store  : 0x10 out_addr
# A non-zero value at an unexpected offset (e.g. a 1 near the compute "done" slot)
# points to a register-map mismatch between this bitstream and the driver.
from pynq import MMIO

#### JO changed: EVTA index from the command line (EVTA i at 0xA0000000 + i * 0x10000, as in vtalib's
#### VTA_FETCH_ADDR_<i>); e.g. sudo python3 dump_regs.py 0 1   (default: 0). Only ask for EVTAs the bitstream has.
import sys
BLOCKS = tuple((f'evta{e} {n}', 0xA0000000 + e * 0x10000 + off)
               for e in (map(int, sys.argv[1:]) if sys.argv[1:] else [0])
               for n, off in (('fetch', 0x0), ('load', 0x1000), ('compute', 0x2000), ('store', 0x3000)))
CTRL_BITS = ((0, 'start'), (1, 'done'), (2, 'idle'), (3, 'ready'), (7, 'auto_restart'))

for name, base in BLOCKS:
    m = MMIO(base, 0x1000)
    ctrl = m.read(0)
    flags = ','.join(n for b, n in CTRL_BITS if ctrl >> b & 1)
    print(f'--- {name} @ {base:#x}  ctrl={ctrl:#x} [{flags}]')
    row = []
    for off in range(0x04, 0x80, 4):
        v = m.read(off)
        row.append(f'{off:#04x}={v:#010x}')
        if len(row) == 4:
            print('   ' + '  '.join(row))
            row = []
    if row:
        print('   ' + '  '.join(row))
