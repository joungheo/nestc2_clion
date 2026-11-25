#!/usr/bin/env python3
# #### JO added (2026-11-24, docs/evta_dw_unit.md 6): vectors for tb_gemm_sa.v with depthwise window steps (gemm_sa DWU = 1).
# usage: gen_dw_vectors.py <dir> <seed>
# Instructions in issue order (window GEMMs: ky, rows oy, the input pixels of the row): a plain GEMM, a window GEMM with stride 1, one with stride 2, a plain GEMM again (the row mux
# switching both ways; new weight blocks each time). Each window GEMM: rows oy, ky, the input pixels of the row (emit from the
# third, every s-th). Same file format as sim/systolic_model.cc dump; uops.hex has bits 65 = dw, 66 = emit.
import os, random, sys

N, NI, NW, NA = 16, 256, 64, 256
d, seed = sys.argv[1], int(sys.argv[2])
R = random.Random(seed)
os.makedirs(d, exist_ok=True)
s8 = lambda v: ((v + 128) & 255) - 128
s32 = lambda v: ((v + (1 << 31)) & 0xffffffff) - (1 << 31)

inp = [[R.randint(-128, 127) for _ in range(N)] for _ in range(NI)]
wgt = [[[R.randint(-128, 127) for _ in range(N)] for _ in range(N)] for _ in range(NW)]   # wgt[b][row j][col i]
acc = [[R.randint(-100000, 100000) for _ in range(N)] for _ in range(NA)]
acc0 = [a[:] for a in acc]
out = [[0] * N for _ in range(NA)]


def rq_out(v, rq):
    en, rnd, sh, lo, hi = rq
    if not en:
        return s8(v)
    h = (1 << (sh - 1)) if rnd and sh else 0
    if rnd == 2 and sh and v < 0:
        h -= 1
    q = s32(v + h) >> sh
    return lo if q < lo else hi if q > hi else q


def rnd_rq():
    return (R.randint(0, 1), R.randint(0, 2), R.randint(0, 15), R.randint(-128, 0), R.randint(0, 127))


uops = []   # (dst, src, wgt, reset, last, first, dw, emit, rq)


def plain(dsts, srcs, wb, rq):
    for k, (dd, ss) in enumerate(zip(dsts, srcs)):
        uops.append((dd, ss, wb, 0, k == len(dsts) - 1, k == 0, 0, 0, rq))
        for j in range(N):
            acc[dd][j] = s32(acc[dd][j] + sum(inp[ss][i] * wgt[wb][j][i] for i in range(N)))
            out[dd][j] = rq_out(acc[dd][j], rq)


def window(ib, db, wb, ho, wo, s, rq):
    wi = (wo - 1) * s + 3
    first = True
    steps = []
    for ky in range(3):          # ky outside: one weight block per ky over all rows (a run longer than the 17-cycle fill)
        for oy in range(ho):
            for x in range(wi):
                e = x >= 2 and (x - 2) % s == 0
                steps.append((db + oy * wo + ((x - 2) // s if e else 0), ib + (oy * s + ky) * wi + x, wb + ky, e))
    win = [[0] * N for _ in range(3)]
    for k, (dd, ss, ww, e) in enumerate(steps):
        uops.append((dd, ss, ww, 0, k == len(steps) - 1, k == 0, 1, int(e), rq))
        win = [win[1], win[2], inp[ss]]
        if e:
            for j in range(N):
                acc[dd][j] = s32(acc[dd][j] + sum(wgt[ww][j][kk] * win[kk][j] for kk in range(3)))
                out[dd][j] = rq_out(acc[dd][j], rq)
    assert steps[-1][3]


plain(list(range(0, 8)), [R.randrange(NI) for _ in range(8)], 0, rnd_rq())
window(16, 32, 1, 3, 10, 1, rnd_rq())            # input rows 5 x 12 at 16..75, out 32..61
window(100, 80, 4, 2, 5, 2, rnd_rq())            # input rows 5 x 11 at 100..154, out 80..89
window(160, 100, 7, 4, 3, 1, rnd_rq())           # input 6 x 5 at 160..189, out 100..111
plain(list(range(120, 136)), [R.randrange(NI) for _ in range(16)], 10, rnd_rq())
plain(list(range(32, 40)), [R.randrange(NI) for _ in range(8)], 11, rnd_rq())   # onto window outputs


def hexw(f, vals, bits):
    w = 0
    for i, v in enumerate(vals):
        w |= (v & ((1 << bits) - 1)) << (bits * i)
    f.write('%0*x\n' % ((bits * len(vals) + 3) // 4, w))


with open(f'{d}/uops.hex', 'w') as f:
    for (dd, ss, ww, rs, la, fi, dw, em, rq) in uops:
        en, rnd, sh, lo, hi = rq
        rqb = en | (rnd << 1) | (sh << 3) | ((lo & 255) << 8) | ((hi & 255) << 16)
        w = dd | (ss << 12) | (ww << 24) | (rs << 35) | (int(la) << 36) | (int(fi) << 37) | (rqb << 41) | (dw << 65) | (em << 66)
        f.write('%017x\n' % w)
with open(f'{d}/inp.hex', 'w') as f:
    for v in inp: hexw(f, v, 8)
with open(f'{d}/wgt.hex', 'w') as f:
    for b in range(NW):
        for j in range(N): hexw(f, wgt[b][j], 8)
for name, m in (('acc0', acc0), ('acc', acc)):
    with open(f'{d}/{name}.hex', 'w') as f:
        for v in m: hexw(f, v, 32)
with open(f'{d}/out.hex', 'w') as f:
    for v in out: hexw(f, v, 8)
nl = sum(1 for u in uops if u[4])
with open(f'{d}/meta.txt', 'w') as f:
    f.write(f'{len(uops)} {NI} {NW} {NA} {nl} 0\n')
print(f'dw vectors {d} seed {seed}: {len(uops)} uops, {nl} instructions')
