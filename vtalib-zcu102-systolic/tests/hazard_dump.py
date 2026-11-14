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
# #### JO added (2026-11-15, docs/evta_isa_ext.md 8.14): SLOTS=<wslots> (default 34) also checks the II 1 slot loop of an
# _alux2 bitstream as alu_ext_ref.h hazard_slots (one slot per read; a broadcast src is read once per inner loop). The
# dump has no lut_en, so imm MIN / MAX / ADD / SHR are checked with it too (conservative: they run in the imm loop then).
def reads(op, use_imm):
    rs = op in (8, 12, 13) or (not use_imm and op not in (7, 11))
    rd = op not in (6, 9, 10, 8)
    return rs, rd
def check_slots(ins, op, uopmap, W):
    bgn, end, io, ii, dfo, dfi, sfo, sfi, use_imm = ins
    rs, rd = reads(op, use_imm); two = rs and rd
    bc = two and end - bgn == 1 and sfi == 0
    wr = collections.deque(); bad = 0; slot = 0
    def stale(e, r, use):
        for (we, ws) in reversed(wr):
            if ws <= r - W: break
            if we == e and ws < use: return True
        return False
    for a in range(io):
        bslot = -1
        if bc: bslot = slot; slot += 1
        for b in range(ii):
            for u in range(bgn, end):
                d0, s0 = uopmap.get(u, (None, None))
                if d0 is None: return -1, 0
                d = d0 + a * dfo + b * dfi; s = s0 + a * sfo + b * sfi
                sr = dr = -1
                if bc: sr = bslot
                elif two: sr = slot; slot += 1
                elif rs: sr = slot
                if rd: dr = slot
                use = slot; slot += 1
                if (sr >= 0 and stale(s, sr, use)) or (dr >= 0 and stale(d, dr, use)): bad += 1
                if op != 8: wr.append((d, use))
                while wr and wr[0][1] <= use - W - 64: wr.popleft()
    return bad, slot
SLOTS = int(os.environ.get('SLOTS', '34'))
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
                h, ns = check_slots(ins, op, sram, SLOTS)
                if h >= 0:
                    tot['slots'] += ns
                    if h:
                        tot['slot_W%d_insn' % SLOTS] += 1; tot['slot_W%d_steps' % SLOTS] += h
                        first.setdefault('slot_W%d' % SLOTS, '%s: %s' % (os.path.basename(f), l.strip()))
                for W in WS:
                    if kind == 'imm' and W == 7: continue
                    h = check(ins, sram, W)
                    if h < 0: tot['unmapped'] += 1; break
                    if h:
                        tot['%s_W%d_insn' % (kind, W)] += 1; tot['%s_W%d_steps' % (kind, W)] += h
                        first.setdefault('%s_W%d' % (kind, W), '%s: %s' % (os.path.basename(f), l.strip()))
    print(model, dict(tot), 'steps checked', STEPS[0]); STEPS[0] = 0
    for k, v in first.items(): print('   first', k, v)
