#!/usr/bin/env python3
"""Runs and GOPS of a _prof / _sysprof log (NESTC_EVTA_PROFILE=ON), printed by hw*/script/run_prof.sh.

The log has one VTA_SYNC line per EVTA run (exe_time in ms; its GOPS column is 0 without a PROFILE=1
bitstream). Each EVTA call of the bundle .cpp (a statement with vtaCmdH, i.e. convolution_wo_tr) is one
VTA_SYNC, in order, so events / calls = inference runs; the ops of each call come from its shape
arguments (timed_bundle.call_ops, 2 per MAC; a zero-filled conv such as conv0 counts the filled channels).
Prints a per-call table (runs 1, 2, 3 and the last side by side) and, last, one summary line (run_prof.sh puts it in the summary).

usage: prof_summary.py <Dir> <log>
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
    return runs, n, ops, [(r, per[r]) for r in cols]


def main():
    d, log = sys.argv[1], sys.argv[2]
    cpps = [f for f in sorted(glob.glob(f'{d}/*.cpp'))
            if not os.path.basename(f).startswith('dump_') and any(RUN.match(l) for l in open(f))]
    #### JO changed (2026-10-29): VTA_SYNC lines per EVTA ("[profile] VTA<i> events" before each table;
    #### a log without that line is EVTA 0), matched to the .cpp whose calls go to that EVTA
    ev, dev = {}, 0
    for l in open(log):
        m = re.match(r'^\[profile\] VTA(\d+) events$', l.strip())
        if m:
            dev = int(m.group(1))
        elif l.startswith('VTA_SYNC '):
            ev.setdefault(dev, []).append(float(l.split()[13]))
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
        runs, n, ops, cols = r
        last = cols[-1][1]
        if len(ev) == 1:
            res.append(f'{runs} run(s) x {n} EVTA calls, {ops / 1e6:.1f} Mops per run; VTA ms per run: '
                       + ' / '.join(f'run{c + 1} {p:.3f}' for c, p in cols) + f'; last run {ops / (last * 1e6):.2f} GOPS')
        else:
            res.append(f'VTA{k} {runs} runs, last {last:.3f} ms {ops / (last * 1e6):.2f} GOPS')
    print('; '.join(res))


if __name__ == '__main__':
    main()
