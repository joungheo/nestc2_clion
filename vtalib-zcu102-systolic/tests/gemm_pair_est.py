#!/usr/bin/env python3
# #### JO added (2026-11-29, pixel pair experiment, gemm_sa_px): upper bound of the gain of two pixels per GEMM step.
# usage: tests/gemm_pair_est.py <dump dir>...   (each <dir> = EVTA_REPLAY_DUMP of one model, <dir>.runs = its EVTA_RUN lines
#        of EVTA_REPLAY_LOG=2: grep ^EVTA_RUN <Dir>/sys.log; MHZ=400; V=1 prints every run)
# For every GEMM instruction of every recorded run, the steps in the HLS order (iter_out, iter_in, uop) are cut into weight
# runs as sa_ctrl.v does (new run: first step of an instruction or another wgt). A run of M steps takes max(M, 17) cycles
# (a fill every >= 17 cycles); with pairs, two consecutive steps of a run with src+1 / dst+1 (reset steps: dst+1) issue as one
# step (greedy), the run takes max(M', 17).  Also counted: pairs with src+2 (stride 2: possible only with the input split by
# column parity).  The saving (cycles / MHZ) is taken off the measured run time: an upper bound (GEMM on the critical path).
# Also: the same runs with their steps reordered by dst (a vtalib loop order with the pixel x innermost), with and without
# the stride-2 split.
# Output channel pairs (sa_array_pk): two neighbouring runs of an instruction with the same src order and dst at a fixed
# offset (another filter block) run as one run of 32 channels; both: that run with its pixels paired (reordered).
# Per inference: a record used k times per inference shows up NINF k - 1 times in the runs (NINF = inferences of the run,
# default 4: the generic main of run_prof.sh -n 1; the stock mains of Resnet18 / 50 / VGG16 run 2).
import sys, os, re, glob, collections, statistics
MHZ = float(os.environ.get("MHZ", "400")); V = int(os.environ.get("V", "0")); NINF = int(os.environ.get("NINF", "4"))

def runs_of(steps):
    # steps: list of (insn index, wgt, src, dst, reset); a run = consecutive steps of one instruction with one wgt
    out = []; cur = []; last = None
    for st in steps:
        key = (st[0], st[1])
        if cur and key != last: out.append(cur); cur = []
        cur.append(st); last = key
    if cur: out.append(cur)
    return out

def pair_count(run, ds):
    k = 0; n = 0; i = 0
    while i < len(run):
        if i + 1 < len(run):
            a, b = run[i], run[i + 1]
            ok = b[3] == a[3] + 1 and (a[4] or b[2] == a[2] + ds)
            if ok: k += 1; i += 2; n += 1; continue
        i += 1; n += 1
    return n, k            # issue slots, pairs

def pair_sorted(run, ds):
    # the same run with its steps reordered by dst (vtalib loop order): pairs dst / dst+1 with src / src+ds (reset: any src)
    r = sorted(run, key=lambda x: x[3]); return pair_count(r, ds)

def seg_stats(lines, sram):
    uops = {}
    for l in lines:
        t = l.split()
        if t and t[0] == 'uop': uops[int(t[1])] = (int(t[2]), int(t[3]), int(t[4]))
    base = None
    st = collections.Counter()
    segruns = collections.defaultdict(list)    # (length, reset, (src, inp version) order) -> runs of this EVTA run
    iver = {}; wver = {}; nload = 0              # version of each inp / wgt entry (the LOAD that wrote it)
    for l in lines:
        t = l.split()
        if len(t) < 2 or t[0] == 'uop': continue
        if t[1] == 'LOAD' and t[4] == '0' and int(t[12]) > 0:
            s, dr, x = int(t[6]), int(t[8]), int(t[12])
            if base is None: base = dr
            for k in range(x): sram[s + k] = uops.get(dr - base + k)
        elif t[1] == 'LOAD' and t[4] in ('1', '2'):
            nload += 1; s0 = int(t[6]); y, x = int(t[10]), int(t[12])
            n = y * x if t[4] == '1' else (int(t[16]) + y + int(t[17])) * (int(t[18]) + x + int(t[19]))
            vv = wver if t[4] == '1' else iver
            for k in range(n): vv[s0 + k] = nload
        elif t[1] == 'GEMM':
            g = lambda name, j=1: int(t[t.index(name) + j])
            reset = g('reset'); bgn, end = g('uop'), g('uop', 2); io, ii = g('iter'), g('iter', 2)
            dfo, dfi = g('dst'), g('dst', 2); sfo, sfi = g('src'), g('src', 2); wfo, wfi = g('wgt'), g('wgt', 2)
            steps = []
            for a in range(io):
                for b in range(ii):
                    for u in range(bgn, end):
                        e = sram.get(u)
                        if e is None: st['unmapped'] += 1; continue
                        d0, s0, w0 = e
                        steps.append((int(t[0]), w0 + a * wfo + b * wfi, s0 + a * sfo + b * sfi, d0 + a * dfo + b * dfi, reset))
            st['insn'] += 1; st['steps'] += len(steps)
            if reset: st['reset_steps'] += len(steps)
            rl = runs_of(steps)
            # output channel pairs: neighbouring runs of the instruction with the same src order and dst at a fixed offset
            i = 0
            while i < len(rl):
                a = rl[i]; m = len(a); ok = False
                if i + 1 < len(rl):
                    b = rl[i + 1]
                    if len(b) == m and m:
                        off = b[0][3] - a[0][3]
                        ok = off != 0 and all(y[3] - x[3] == off and (x[4] or y[2] == x[2]) for x, y in zip(a, b))
                if ok:
                    _, q = pair_sorted(a, 1)
                    st['cyc_oc'] += max(m, 17); st['cyc_both'] += max(m - q, 17); st['oc_pairs'] += 1; i += 2
                else:
                    _, q = pair_sorted(a, 1)
                    st['cyc_oc'] += max(m, 17); st['cyc_both'] += max(m - q, 17); i += 1
            for r in rl:
                key = (len(r), r[0][4], tuple((x[2], iver.get(x[2], 0)) for x in r) if not r[0][4] else ())
                w = r[0][1]; mine = (w, wver.get(w, 0))
                lst = segruns[key]
                # pair with the latest unpaired run of the key whose weight is another block and still in the buffer
                for c in reversed(lst):
                    if c['p'] is None and c['w'] != mine and wver.get(c['w'][0], 0) == c['w'][1]:
                        c['p'] = True; mine = None; break
                if mine is not None: lst.append({'w': mine, 'p': None, 'r': r})
            for r in rl:
                m = len(r); n1, k1 = pair_count(r, 1); n2, k2 = pair_count(r, 2)
                st['runs'] += 1; st['cyc'] += max(m, 17); st['cyc_px'] += max(n1, 17)
                st['cyc_px2'] += max(m - max(k1, k2), 17)   # best of src+1 / src+2 per run
                st['pairs'] += k1; st['pairs_s2'] += k2
                o1, q1 = pair_sorted(r, 1); o2, q2 = pair_sorted(r, 2)
                st['cyc_ro'] += max(m - q1, 17); st['cyc_ro2'] += max(m - max(q1, q2), 17); st['pairs_ro'] += q1
                st['short_runs'] += m < 34
    # output channel pairs over the whole EVTA run (vtalib putting two filter blocks in one GEMM instruction): runs with the
    # same src order pair up whatever instruction they are in (another filter block of the same input)
    for key, rs in segruns.items():
        m = key[0]
        for c in rs:                               # an entry = one run alone or (p True) a merged pair of two runs
            _, q = pair_sorted(c['r'], 1)
            st['cyc_ocs'] += max(m, 17); st['cyc_boths'] += max(m - q, 17)
            if c['p']: st['ocs_pairs'] += 1
    return st

def model(d):
    files = sorted(glob.glob(d + '/rec_*.txt'), key=lambda f: tuple(int(x) for x in re.findall(r'\d+', os.path.basename(f))))
    segs = {}; sram = {}; prev = None
    for f in files:
        rec, seg = [int(x) for x in re.findall(r'\d+', os.path.basename(f))[:2]]
        if rec != prev: sram = {}; prev = rec
        segs[(rec, seg)] = seg_stats(open(f).read().split('\n'), sram)
    times = collections.defaultdict(list)
    if os.path.exists(d + '.runs'):
        for l in open(d + '.runs'):
            t = l.split()
            times[(int(t[2]), int(t[3]))].append(float(t[5]))
    tot = collections.Counter(); us = us_new = us_new2 = 0.0; us_ro = us_ro2 = 0.0; us_oc = us_both = 0.0; us_ocs = us_boths = 0.0
    for key, st in segs.items():
        c = len(times.get(key, []))
        k = (c + 1) // NINF if c else 1
        tm = statistics.median(times[key]) if c else 0.0
        sv = (st['cyc'] - st['cyc_px']) / MHZ; sv2 = (st['cyc'] - st['cyc_px2']) / MHZ
        for x, v in st.items(): tot[x] += k * v
        us += k * tm; us_new += k * max(tm - sv, 0.0); us_new2 += k * max(tm - sv2, 0.0)
        us_oc += k * max(tm - (st['cyc'] - st['cyc_oc']) / MHZ, 0.0); us_both += k * max(tm - (st['cyc'] - st['cyc_both']) / MHZ, 0.0)
        us_ocs += k * max(tm - (st['cyc'] - st['cyc_ocs']) / MHZ, 0.0); us_boths += k * max(tm - (st['cyc'] - st['cyc_boths']) / MHZ, 0.0)
        us_ro += k * max(tm - (st['cyc'] - st['cyc_ro']) / MHZ, 0.0); us_ro2 += k * max(tm - (st['cyc'] - st['cyc_ro2']) / MHZ, 0.0)
        if V: print('  rec %d seg %d x%d: %.1f us, GEMM model %.1f -> %.1f us, steps %d pairs %d (s2 %d)' % (
            key[0], key[1], k, tm, st['cyc'] / MHZ, st['cyc_px'] / MHZ, st['steps'], st['pairs'], st['pairs_s2']))
    name = os.path.basename(d.rstrip('/'))
    print('%-18s EVTA runs %.2f ms | GEMM model %.2f -> %.2f ms (pairs: %.0f %% of steps paired, s+2 %.0f %%) | runs < 34 steps %.0f %% | '
          'EVTA est. %.2f ms (-%.2f), with stride-2 split %.2f ms (-%.2f)%s' % (
        name, us / 1e3, tot['cyc'] / MHZ / 1e3, tot['cyc_px'] / MHZ / 1e3,
        200.0 * tot['pairs'] / max(tot['steps'], 1), 200.0 * tot['pairs_s2'] / max(tot['steps'], 1),
        100.0 * tot['short_runs'] / max(tot['runs'], 1), us_new / 1e3, (us - us_new) / 1e3, us_new2 / 1e3, (us - us_new2) / 1e3,
        '  unmapped %d' % tot['unmapped'] if tot['unmapped'] else ''))
    print('%-18s   reordered by dst: GEMM %.2f ms (%.0f %% paired), EVTA est. %.2f ms (-%.2f); + stride-2 split: GEMM %.2f ms, EVTA est. %.2f ms (-%.2f)' % (
        '', tot['cyc_ro'] / MHZ / 1e3, 200.0 * tot['pairs_ro'] / max(tot['steps'], 1), us_ro / 1e3, (us - us_ro) / 1e3,
        tot['cyc_ro2'] / MHZ / 1e3, us_ro2 / 1e3, (us - us_ro2) / 1e3))
    print('%-18s   output channel pairs: GEMM %.2f ms (%.0f %% of runs merged), EVTA est. %.2f ms (-%.2f); both (32 ch x 2 px): GEMM %.2f ms, EVTA est. %.2f ms (-%.2f)' % (
        '', tot['cyc_oc'] / MHZ / 1e3, 200.0 * tot['oc_pairs'] / max(tot['runs'], 1), us_oc / 1e3, (us - us_oc) / 1e3,
        tot['cyc_both'] / MHZ / 1e3, us_both / 1e3, (us - us_both) / 1e3))
    print('%-18s   output channel pairs over the EVTA run: GEMM %.2f ms (%.0f %% of runs merged), EVTA est. %.2f ms (-%.2f); both: GEMM %.2f ms, EVTA est. %.2f ms (-%.2f)' % (
        '', tot['cyc_ocs'] / MHZ / 1e3, 200.0 * tot['ocs_pairs'] / max(tot['runs'], 1), us_ocs / 1e3, (us - us_ocs) / 1e3,
        tot['cyc_boths'] / MHZ / 1e3, us_boths / 1e3, (us - us_boths) / 1e3))

for d in sys.argv[1:]: model(d)
