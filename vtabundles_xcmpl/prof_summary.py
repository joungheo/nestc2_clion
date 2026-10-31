#!/usr/bin/env python3
"""Runs and GOPS of a _prof / _sysprof log (NESTC_EVTA_PROFILE=ON), printed by hw*/script/run_prof.sh.

The log has one VTA_SYNC line per EVTA run (exe_time in ms; its GOPS column is 0 without a PROFILE=1
bitstream). Each EVTA call of the bundle .cpp (a statement with vtaCmdH, i.e. convolution_wo_tr) is one
VTA_SYNC, in order, so events / calls = inference runs; the ops of each call come from its shape
arguments (timed_bundle.call_ops, 2 per MAC; a zero-filled conv such as conv0 counts the filled channels).
Prints a per-call table (runs 1, 2, 3 and the last side by side) and, last, one summary line (run_prof.sh puts it in the summary).

usage: prof_summary.py <Dir> <log>
e.g.:  cd hw_multievtax4 && python3 ../prof_summary.py MultiEVTAResnet18Test MultiEVTAResnet18Test/sysprof_multivtaMxnetResnet18Bundle.log
#### JO added (2026-10-29)
"""
import glob, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from timed_bundle import RUN, call_args, call_name, call_ops


def evta_calls(cpp):
    """(node, call, ops, EVTA index) of every EVTA call in the bundle .cpp, in execution order."""
    calls, node = [], '-'
    for l in open(cpp):
        m = RUN.match(l)
        if m:
            node = m.group(2)
        s = l.strip()
        if 'vtaCmdH' in s and s.endswith(';') and not s.startswith('//'):
            c = call_name(s)
            #### JO added (2026-10-29): the last argument of convolution_wo_tr is the EVTA index
            try:
                dev = int(call_args(s, c)[-1])
            except (ValueError, IndexError):
                dev = 0
            calls.append((node, c, call_ops(s, c), dev))
    return calls


def table(name, calls, ev):
    """Per-call table of runs 1, 2, 3 and the last; returns (runs, last-run ms, GOPS) or None."""
    n = len(calls)
    if n == 0 or len(ev) % n:
        print(f'{name}: {len(ev)} VTA runs, {n} EVTA calls: not a multiple')
        return None
    runs = len(ev) // n
    ops = sum(c[2] for c in calls)
    per = [sum(ev[r * n:(r + 1) * n]) for r in range(runs)]
    #### JO changed (2026-10-29): runs 1, 2, 3 and the last side by side (as the _timed table), ms then GOPS
    cols = sorted(set(r for r in (0, 1, 2, runs - 1) if r < runs))
    print(f'---- EVTA calls per run ({name}, {runs} run(s)) ----')
    print(f'{"#":>3} {"Mops":>8}' + ''.join(f' {"run" + str(r + 1) + "_ms":>10}' for r in cols)
          + ''.join(f' {"GOPS" + str(r + 1):>7}' for r in cols) + '  node')
    for i, (node, c, o, _) in enumerate(calls):
        t = [ev[r * n + i] for r in cols]
        print(f'{i:3d} {o / 1e6:8.1f}' + ''.join(f' {x:10.3f}' for x in t)
              + ''.join(f' {o / (x * 1e6) if x > 0 else 0:7.2f}' for x in t) + f'  {node}')
    print(f'{"sum":>3} {ops / 1e6:8.1f}' + ''.join(f' {per[r]:10.3f}' for r in cols)
          + ''.join(f' {ops / (per[r] * 1e6):7.2f}' for r in cols))
    #### JO added (2026-10-31): VTA ms of every run, then the average of runs 2..n (run 1 = warm-up) and its GOPS
    av = per[1:] if runs > 1 else per
    avg = sum(av) / len(av)
    print(f'VTA ms per run: ' + ' '.join(f'{x:.3f}' for x in per))
    print(f'average runs {"2-" + str(runs) if runs > 1 else "1"}: {avg:.3f} ms (min {min(av):.3f}, max {max(av):.3f}), '
          f'{ops / (avg * 1e6):.2f} GOPS')
    # return runs, n, ops, [(r, per[r]) for r in cols]
    return runs, n, ops, [(r, per[r]) for r in cols], avg


def main():
    d, log = sys.argv[1], sys.argv[2]
    cpps = [f for f in sorted(glob.glob(f'{d}/*.cpp'))
            if not os.path.basename(f).startswith('dump_') and any(RUN.match(l) for l in open(f))]
    #### JO changed (2026-10-29): VTA_SYNC lines per EVTA ("[profile] VTA<i> events" before each table;
    #### a log without that line is EVTA 0), matched to the .cpp whose calls go to that EVTA
    #### JO changed (2026-10-29): VTA_SYNC / header anywhere in the line: the threads of a multi-EVTA executable print
    #### while the others still run, so a line can start with another thread's output ("100VTA_SYNC 62 ...")
    ev, ts, dev = {}, {}, 0
    for l in open(log):
        m = re.search(r'\[profile\] VTA(\d+) events', l)
        if m:
            dev = int(m.group(1))
            continue
        i = l.find('VTA_SYNC ')
        if i >= 0:
            f = l[i:].split()
            k = int(f[16]) if len(f) > 16 and f[16].isdigit() else dev  # evta column (vtalib 2026-10-29)
            ev.setdefault(k, []).append(float(f[13]))
            ts.setdefault(k, []).append((int(f[14]), int(f[15])))
    if not ev:
        return
    bydev = {}
    for f in cpps:
        calls = [c for c in evta_calls(f) if c[1] != 'assign']
        if calls:
            bydev.setdefault(calls[0][3], []).append((os.path.basename(f), calls))
    res = []
    for k in sorted(ev):
        src = bydev.get(k, [])
        if len(src) != 1:
            print(f'VTA{k}: {len(ev[k])} VTA runs ({len(src)} bundle .cpp on this EVTA: runs and GOPS not split)')
            res.append(f'VTA{k} {len(ev[k])} VTA runs')
            continue
        name, calls = src[0]
        r = table(f'VTA{k}, {name}', calls, ev[k])
        if not r:
            res.append(f'VTA{k} {len(ev[k])} VTA runs')
            continue
        runs, n, ops, cols, avg = r
        last = cols[-1][1]
        rng = f'runs 2-{runs}' if runs > 1 else 'run 1'
        if len(ev) == 1:
            #### JO changed (2026-10-31): + the average of runs 2..n
            res.append(f'{runs} run(s) x {n} EVTA calls, {ops / 1e6:.1f} Mops per run; VTA ms per run: '
                       + ' / '.join(f'run{c + 1} {p:.3f}' for c, p in cols) + f'; last run {ops / (last * 1e6):.2f} GOPS'
                       + f'; avg {rng} {avg:.3f} ms {ops / (avg * 1e6):.2f} GOPS')
        else:
            res.append(f'VTA{k} {runs} runs, last {last:.3f} ms {ops / (last * 1e6):.2f} GOPS, avg {rng} {avg:.3f} ms')
    #### JO added (2026-10-29): EVTAs that ran the same bundle together (multi-EVTA, same runs and calls): per conv of a
    #### run, the window from the first start to the last end over the EVTAs; GOPS = ops of all EVTAs / window
    conc = [k for k in sorted(ev) if len(bydev.get(k, [])) == 1 and len(ev[k]) % len(bydev[k][0][1]) == 0]
    if len(conc) > 1 and len(set(len(ev[k]) for k in conc)) == 1:
        calls = bydev[conc[0]][0][1]
        n, runs = len(calls), len(ev[conc[0]]) // len(calls)
        for r in sorted(set(x for x in (1, runs - 1) if 0 <= x < runs)):
            print(f'---- {len(conc)} EVTAs at once, run {r + 1} (window = first start to last end of the conv) ----')
            print(f'{"#":>3} {"Mops":>8} {"skew_ms":>8} {"window_ms":>10} {"max_exe":>8} {"GOPS":>8}  node')
            tw = tops = 0
            for i, (node, c, o, _) in enumerate(calls):
                be = [ts[k][r * n + i] for k in conc]
                b0, b1, e1 = min(b for b, _ in be), max(b for b, _ in be), max(e for _, e in be)
                w = (e1 - b0) / 1000.0
                ops = o * len(conc)
                tw += w; tops += ops
                print(f'{i:3d} {ops / 1e6:8.1f} {(b1 - b0) / 1000.0:8.3f} {w:10.3f} {max(ev[k][r * n + i] for k in conc):8.3f}'
                      f' {ops / (w * 1e6) if w > 0 else 0:8.2f}  {node}')
            print(f'{"sum":>3} {tops / 1e6:8.1f} {"":>8} {tw:10.3f} {"":>8} {tops / (tw * 1e6):8.2f}')
        #### busy union: time at least one EVTA runs; sum of EVTA times / union = how many ran at once on average
        un = []
        for r in sorted(set(x for x in (1, runs - 1) if 0 <= x < runs)):
            iv = sorted(ts[k][r * n + i] for k in conc for i in range(n))
            u, cb, ce = 0, iv[0][0], iv[0][1]
            for b, e in iv[1:]:
                if b > ce:
                    u += ce - cb; cb, ce = b, e
                else:
                    ce = max(ce, e)
            u = (u + ce - cb) / 1000.0
            busy = sum(ev[k][r * n + i] for k in conc for i in range(n))
            ops = sum(c[2] for c in calls) * len(conc)
            print(f'run {r + 1}: EVTA busy union {u:.3f} ms (sum of EVTA times {busy:.3f} ms, {busy / u:.2f} EVTAs at once on average)'
                  f' -> {ops / 1e6:.1f} Mops / union = {ops / (u * 1e6):.2f} GOPS')
            un.append((r, u, busy, ops))
        #### JO changed (2026-10-29): every run, then the median in the summary: one late completion (the polling thread off
        #### the CPU for a 4 ms tick, CONFIG_HZ=250) costs a whole run in lockstep, so a single run is noisy
        # r, u, busy, ops = un[-1]
        # res.append(f'{len(conc)} EVTAs, run {r + 1}: {ops / (u * 1e6):.2f} GOPS over the busy union {u:.3f} ms ({busy / u:.2f} at once)')
        allg = []
        for r in range(runs):
            iv = sorted(ts[k][r * n + i] for k in conc for i in range(n))
            u, cb, ce = 0, iv[0][0], iv[0][1]
            for b, e in iv[1:]:
                if b > ce:
                    u += ce - cb; cb, ce = b, e
                else:
                    ce = max(ce, e)
            allg.append(ops / ((u + ce - cb) / 1000.0 * 1e6))
        print('GOPS over the busy union, every run: ' + ' '.join(f'{g:.0f}' for g in allg))
        sg = sorted(allg)
        res.append(f'{len(conc)} EVTAs at once over the busy union: median {sg[len(sg) // 2]:.2f} GOPS'
                   f' (min {sg[0]:.1f}, max {sg[-1]:.1f}, {runs} runs)')
    print('; '.join(res))


if __name__ == '__main__':
    main()
