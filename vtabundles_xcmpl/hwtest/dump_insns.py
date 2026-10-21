#!/usr/bin/env python3
# #### JO added (2026-10-21): dump the instruction buffer EVTA's fetch reads, straight from DRAM.
# fetch registers: 0x10 insn_count, 0x18 insn address (physical). Run while a test hangs (second terminal) or
# right after it, to see whether DRAM holds the instructions the runtime wrote (compare with the
# VTA_DEBUG_DUMP_INSN output) -- the ILA showed fetch forwarding garbage to the store queue.
# usage: sudo python3 dump_insns.py [evta index] [count override]
import mmap
import os
import struct
import sys

evta = int(sys.argv[1]) if len(sys.argv) > 1 else 0
fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
regs = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=0xA0000000 + evta * 0x10000)
count = struct.unpack('<I', regs[0x10:0x14])[0]
addr = struct.unpack('<I', regs[0x18:0x1c])[0]
if len(sys.argv) > 2:
    count = int(sys.argv[2])
print('fetch: insn_count=%d insn_addr=0x%08x ctrl=0x%x' % (count, addr, struct.unpack('<I', regs[0:4])[0]))

page = addr & ~0xfff
size = ((addr - page) + count * 32 + 0xfff) & ~0xfff
mem = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ, offset=page)
OPS = {0: 'LOAD', 1: 'STORE', 2: 'GEMM', 3: 'FINISH', 4: 'ALU', 5: 'XP_GEMM'}
MEM = {0: 'UOP', 1: 'WGT', 2: 'INP', 3: 'ACC'}


def f(v, lo, w):
    return (v >> lo) & ((1 << w) - 1)


for i in range(count):
    b = mem[addr - page + 32 * i: addr - page + 32 * i + 32]
    v = int.from_bytes(b, 'little')
    op = f(v, 0, 3)
    dep = 'dep(pp=%d pn=%d hp=%d hn=%d)' % (f(v, 3, 1), f(v, 4, 1), f(v, 5, 1), f(v, 6, 1))
    if op in (0, 1):
        d = '%s %s sram=0x%x dram=0x%08x y=%d x=%d stride=%d' % (
            OPS[op], MEM[f(v, 7, 2)] if op == 0 else '', f(v, 9, 16), f(v, 25, 32), f(v, 64, 16),
            f(v, 80, 16), f(v, 96, 16))
    elif op in (2, 4):
        d = '%s reset=%d uop=[%d,%d) iter=%dx%d' % (OPS[op], f(v, 7, 1), f(v, 8, 13), f(v, 21, 14),
                                                    f(v, 35, 14), f(v, 49, 14))
    else:
        d = OPS.get(op, '?')
    print('%2d %s  %s %s' % (i, b[::-1].hex(), d, dep))
