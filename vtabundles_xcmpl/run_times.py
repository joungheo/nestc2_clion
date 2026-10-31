#!/usr/bin/env python3
"""Whole-run time of every inference of a _timed / _systimed log, then the average and GOPS, printed by hw*/script/run_prof.sh.

Reads the timing csv(s) the log names ("per statement and run: <csv>", one per timed .cpp, in <Dir>): the
"total" row of each run and the ops of the statements. Run 1 is the warm-up of the stock mains, so the average
is over runs 2..n (all runs if there is only one); GOPS = ops of one run / average ms.
Prints one line per csv; the last line (summary of run_prof.sh) has the averages only.

usage: run_times.py <Dir> <log>
#### JO added (2026-10-31)
"""
import csv, os, re, sys


def runs_of(path):
    """(total ms of every run in order, ops of one run)."""
    tot, ops = {}, {}
    for row in csv.DictReader(open(path)):
        r = int(row['run'])
        if row['idx'] == '-1':
            tot[r] = float(row['ms'])
        elif row['ops']:
            ops[r] = ops.get(r, 0.0) + float(row['ops'])
    return [tot[r] for r in sorted(tot)], ops.get(1, 0.0)


def main():
    d, log = sys.argv[1], sys.argv[2]
    names = []
    for l in open(log, errors='replace'):
        m = re.search(r'per statement and run: (\S+)', l)
        if m and m.group(1) not in names:
            names.append(m.group(1))
    res = []
    for n in names:
        p = os.path.join(d, n)
        if not os.path.isfile(p):
            continue
        t, ops = runs_of(p)
        if not t:
            continue
        avg_runs = t[1:] if len(t) > 1 else t
        avg = sum(avg_runs) / len(avg_runs)
        rng = f'runs 2-{len(t)}' if len(t) > 1 else 'run 1'
        g = f', {ops / (avg * 1e6):.2f} GOPS' if ops > 0 else ''
        tag = '' if len(names) == 1 else f'{n}: '
        print(f'{tag}whole run ms per run: ' + ' '.join(f'{x:.3f}' for x in t))
        med = sorted(avg_runs)[len(avg_runs) // 2]
        print(f'{tag}average {rng}: {avg:.3f} ms (median {med:.3f}, min {min(avg_runs):.3f}, max {max(avg_runs):.3f}){g}')
        res.append(f'{tag}avg {rng} {avg:.3f} ms{g} (median {med:.3f} ms)')
    if res:
        print('; '.join(res))


if __name__ == '__main__':
    main()
