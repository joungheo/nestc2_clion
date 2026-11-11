#!/usr/bin/env python3
# #### JO added (2026-11-11, docs/evta_isa_ext.md 8.8). usage: WS=7,17 tests/hazard_dump.py <EVTA_REPLAY_DUMP dir> ...
# Hazard check of EVTA_REPLAY_DUMP segments (docs/evta_isa_ext.md 8.8): for each vector ALU instruction (use_imm 0), steps
# reading an acc entry (dst or src) written within the last W steps of the same instruction. imm ALU instructions keep the
# II 1 loop (unchanged, up to 12 in flight) and are reported separately.
import sys, glob, os, collections
def check(ins, uopmap, W):
    bgn, end, io, ii, dfo, dfi, sfo, sfi, use_imm = ins
    wr = collections.deque(maxlen=W); bad = 0
    for a in range(io):
        for b in range(ii):
            for u in range(bgn, end):
                d0, s0 = uopmap.get(u, (None, None))
                if d0 is None: return -1
                d = d0 + a * dfo + b * dfi; s = s0 + a * sfo + b * sfi
                STEPS[0] += 1
                if d in wr or (not use_imm and s in wr): bad += 1
                wr.append(d)
    return bad
STEPS = [0]
WS = [int(w) for w in os.environ.get('WS', '7,12,17').split(',')]
for model in sys.argv[1:]:
    tot = collections.Counter(); first = {}
    import re
    files = sorted(glob.glob(model + '/rec_*.txt'), key=lambda f: tuple(int(x) for x in re.findall(r'\d+', os.path.basename(f))))
    sram = {}; prev = None
    for f in files:
        rec = re.findall(r'\d+', os.path.basename(f))[0]
        if rec != prev: sram = {}; prev = rec   # sram uops persist across the segments of one record
        lines = open(f).read().split('\n')
        uops = {}
        for l in lines:
            t = l.split()
            if t and t[0] == 'uop': uops[int(t[1])] = (int(t[2]), int(t[3]))
        base = None
        for l in lines:
            t = l.split()
            if len(t) < 2 or t[0] == 'uop': continue
            if t[1] == 'LOAD' and t[3] == 'mem' and t[4] == '0' and int(t[12]) > 0:
                s, dr, x = int(t[6]), int(t[8]), int(t[12])
                if base is None: base = dr
                for k in range(x): sram[s + k] = uops.get(dr - base + k, (None, None))
            elif t[1] == 'ALU':
                v = dict(zip(t[3::1], t[4::1]))
                i = t.index('uop'); bgn, end = int(t[i+1]), int(t[i+2])
                i = t.index('iter'); io, ii = int(t[i+1]), int(t[i+2])
                i = t.index('dst'); dfo, dfi = int(t[i+1]), int(t[i+2])
                i = t.index('src'); sfo, sfi = int(t[i+1]), int(t[i+2])
                i = t.index('alu'); op = int(t[i+1]); i = t.index('imm'); use_imm = int(t[i+1])
                ins = (bgn, end, io, ii, dfo, dfi, sfo, sfi, use_imm)
                kind = 'imm' if use_imm else 'vec'
                tot[kind + '_insn'] += 1
                for W in WS:
                    if kind == 'imm' and W == 7: continue
                    h = check(ins, sram, W)
                    if h < 0: tot['unmapped'] += 1; break
                    if h:
                        tot['%s_W%d_insn' % (kind, W)] += 1; tot['%s_W%d_steps' % (kind, W)] += h
                        first.setdefault('%s_W%d' % (kind, W), '%s: %s' % (os.path.basename(f), l.strip()))
    print(model, dict(tot), 'steps checked', STEPS[0]); STEPS[0] = 0
    for k, v in first.items(): print('   first', k, v)
