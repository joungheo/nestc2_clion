#!/usr/bin/env python3
# #### JO added (2026-10-23): run the BRAM self test (brambist.bin, v2, loaded with fpga_manager).
#   all            : every BRAM at once, <runs> times, map of the errors
#   columns        : one RAMB36 column (X) at a time (few BRAMs switching), map of the errors
#   load <seconds> : all BRAMs in loop mode while sampling VCCINT / VCCBRAM (PS SYSMON, iio "ams")
# Map: '.' no error, 'o' <16, 'O' <1024, 'X' >=1024 errors, ' ' no RAMB36 at that X/Y.
# usage: sudo python3 brambist.py <sites.txt> all [runs] | columns | load [seconds]
import glob
import mmap
import os
import struct
import sys
import time

sites_file = sys.argv[1]
mode = sys.argv[2] if len(sys.argv) > 2 else 'all'
arg = sys.argv[3] if len(sys.argv) > 3 else None
sites = {}
for line in open(sites_file):
    i, s = line.split()
    x, y = s.replace('RAMB36_X', '').split('Y')
    sites[int(i)] = (int(x), int(y))

fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 0x10000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=0xA0000000)


def rd(a):
    return struct.unpack('<I', m[a:a + 4])[0]


def wr(a, v):
    m[a:a + 4] = struct.pack('<I', v)


ident, n = rd(8), rd(4)
if ident != 0xB1A57002:
    sys.exit('ID 0x%08x: brambist v2 bitstream not loaded' % ident)


def run(lo, hi, loop_s=0.0):
    """test BRAMs lo..hi-1 (one pass, or loop for loop_s seconds); return {i: (count, bits)}"""
    wr(0x0C, lo)
    wr(0x10, hi)
    wr(0, 3 if loop_s else 1)
    if loop_s:
        time.sleep(loop_s)
        wr(0, 0)
    t0 = time.time()
    while not rd(0) & 2:
        if time.time() - t0 > 2:
            sys.exit('BIST did not finish (ctrl 0x%08x)' % rd(0))
    res = {}
    for i in range(lo, hi):
        w0 = rd(0x4000 + 8 * i)
        if w0 & 0xFFFF:
            res[i] = (w0 & 0xFFFF, rd(0x4004 + 8 * i) | ((w0 >> 28) << 32))
    return res, rd(0) >> 16


def show(total, title):
    xs = sorted({x for x, _ in sites.values()})
    ys = sorted({y for _, y in sites.values()})
    where = {v: k for k, v in sites.items()}
    print('\n%s: . ok  o <16  O <1024  X >=1024 errors' % title)
    print('      ' + ''.join('%-3d' % x for x in xs))
    for y in reversed(ys):
        row = ''
        for x in xs:
            i = where.get((x, y))
            c = total.get(i, (0, 0))[0] if i is not None else None
            row += '   ' if c is None else (' . ' if c == 0 else (' o ' if c < 16 else (' O ' if c < 1024 else ' X ')))
        print('Y%-3d %s' % (y, row))
    print('RESULT brambist %s: %d of %d BRAMs with errors' % (mode, len(total), n))


def ams():
    d = [p for p in glob.glob('/sys/bus/iio/devices/iio:device*') if open(p + '/name').read().strip() == 'ams'][0]
    def v(name):
        f = glob.glob('%s/in_voltage*_%s_raw' % (d, name))[0]
        return int(open(f).read()) * float(open(f.replace('_raw', '_scale')).read())
    return v('vccint'), v('vccbram')


if mode == 'all':
    total = {}
    for r in range(int(arg or 3)):
        res, _ = run(0, n)
        for i, (c, b) in res.items():
            oc, ob = total.get(i, (0, 0))
            total[i] = (oc + c, ob | b)
        print('run %d: %d of %d BRAMs with errors' % (r, len(res), n))
    show(total, 'all BRAMs at once')
elif mode == 'columns':
    cols = {}
    for i, (x, y) in sites.items():
        cols.setdefault(x, []).append(i)
    total = {}
    for x in sorted(cols):
        lo, hi = min(cols[x]), max(cols[x]) + 1
        assert hi - lo == len(cols[x]), 'column %d not contiguous in sites.txt' % x
        res, _ = run(lo, hi)
        total.update(res)
        print('column X%-2d (BRAM %3d..%3d, %2d BRAMs): %d with errors' % (x, lo, hi - 1, hi - lo, len(res)))
    show(total, 'one column at a time')
elif mode == 'load':
    secs = float(arg or 5)
    idle = ams()
    wr(0x0C, 0)
    wr(0x10, n)
    wr(0, 3)
    vi, vb = [], []
    t0 = time.time()
    while time.time() - t0 < secs:
        a, b = ams()
        vi.append(a)
        vb.append(b)
    wr(0, 0)
    while not rd(0) & 2:
        pass
    passes = rd(0) >> 16
    bad = sum(1 for i in range(n) if rd(0x4000 + 8 * i) & 0xFFFF)
    print('idle   : VCCINT %.1f mV  VCCBRAM %.1f mV' % idle)
    print('loaded : VCCINT min %.1f max %.1f mV, VCCBRAM min %.1f max %.1f mV (%d samples, %d passes)'
          % (min(vi), max(vi), min(vb), max(vb), len(vi), passes))
    print('RESULT brambist load: %d of %d BRAMs with errors' % (bad, n))
else:
    sys.exit('usage: brambist.py <sites.txt> all [runs] | columns | load [seconds]')
