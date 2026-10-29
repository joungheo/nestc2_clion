#!/usr/bin/env python3
"""Runs and GOPS of a _prof / _sysprof log (NESTC_EVTA_PROFILE=ON), printed by hw*/script/run_prof.sh.

The log has one VTA_SYNC line per EVTA run (exe_time in ms; its GOPS column is 0 without a PROFILE=1
bitstream). Each EVTA call of the bundle .cpp (a statement with vtaCmdH, i.e. convolution_wo_tr) is one
VTA_SYNC, in order, so events / calls = inference runs; the ops of each call come from its shape
arguments (timed_bundle.call_ops, 2 per MAC; a zero-filled conv such as conv0 counts the filled channels).
Prints a per-call table of the last run and, last, one summary line (run_prof.sh puts it in the summary).

usage: prof_summary.py <Dir> <log>
#### JO added (2026-10-29)
"""
import glob, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from timed_bundle import RUN, call_name, call_ops


def evta_calls(cpp):
    """(node, call, ops) of every EVTA call in the bundle .cpp, in execution order."""
    calls, node = [], '-'
    for l in open(cpp):
        m = RUN.match(l)
        if m:
            node = m.group(2)
        s = l.strip()
        if 'vtaCmdH' in s and s.endswith(';') and not s.startswith('//'):
            c = call_name(s)
            calls.append((node, c, call_ops(s, c)))
    return calls


def main():
    d, log = sys.argv[1], sys.argv[2]
    cpps = [f for f in sorted(glob.glob(f'{d}/*.cpp'))
            if not os.path.basename(f).startswith('dump_') and any(RUN.match(l) for l in open(f))]
    ev = [float(l.split()[13]) for l in open(log) if l.startswith('VTA_SYNC ')]
    if not ev:
        return
    if len(cpps) != 1:
        print(f'{len(ev)} VTA runs ({len(cpps)} bundle .cpp: runs and GOPS not split)')
        return
    calls = [c for c in evta_calls(cpps[0]) if c[1] != 'assign']
    n = len(calls)
    if n == 0 or len(ev) % n:
        print(f'{len(ev)} VTA runs ({n} EVTA calls in {os.path.basename(cpps[0])}: not a multiple)')
        return
    runs = len(ev) // n
    ops = sum(c[2] for c in calls)
    last = ev[-n:]
    print(f'---- EVTA calls of the last run ({os.path.basename(cpps[0])}) ----')
    print(f'{"#":>3} {"ms":>8} {"Mops":>9} {"GOPS":>7}  node')
    for i, ((node, c, o), t) in enumerate(zip(calls, last)):
        print(f'{i:3d} {t:8.3f} {o / 1e6:9.1f} {o / (t * 1e6) if t > 0 else 0:7.2f}  {node}')
    per = [sum(ev[r * n:(r + 1) * n]) for r in range(runs)]
    print(f'{runs} run(s) x {n} EVTA calls, {ops / 1e6:.1f} Mops per run; VTA ms per run: '
          + ' / '.join(f'{p:.3f}' for p in per) + f'; last run {ops / (per[-1] * 1e6):.2f} GOPS')


if __name__ == '__main__':
    main()
