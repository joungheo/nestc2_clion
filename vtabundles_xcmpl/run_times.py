#!/usr/bin/env python3
"""Time of every inference of a _timed / _systimed log, then medians and GOPS, printed by hw*/script/run_prof.sh.

Reads the timing csv(s) the log names ("per statement and run: <csv>", one per timed .cpp, in <Dir>): the
"total" row of each run (whole run, CPU included) and the EVTA conv calls (convolution_wo_tr*, the statements
with vtaCmdH). Run 1 is the warm-up of the stock mains, so medians are over runs 2..n (all runs if there is
only one); GOPS = ops / median ms (whole run: ops of all statements, conv: ops of the EVTA calls).
Several csv with the same runs = the copies of a multi-EVTA executable, run at once (each run starts at a
barrier of the copies, multi_threads_main.cpp): per run
  whole run = the slowest copy, GOPS = ops of all copies / that;
  conv      = the busy union of the EVTA calls of all copies (start_ms of each call is from the start of its
              run), GOPS = EVTA ops of all copies / union.
Lockstep (run_prof.sh -l) makes every conv start on all EVTAs together: the conv GOPS is then that of the EVTAs
at once, while the whole run gets slower by the waits; measure the throughput (whole run) without -l.
The last line (summary of run_prof.sh) has the medians only.

usage: run_times.py <Dir> <log>
#### JO added (2026-10-31)
"""
import csv, os, re, sys


def median(x):
    s = sorted(x)
    k = len(s) // 2
    return s[k] if len(s) % 2 else (s[k - 1] + s[k]) / 2


def is_evta(call):
    return 'convolution_wo_tr' in call


def runs_of(path):
    """{run: total ms}, {run: [(start, ms)] of the EVTA calls}, ops of one run, EVTA ops of one run."""
    tot, conv, ops, eops = {}, {}, 0.0, 0.0
    for row in csv.DictReader(open(path)):
        r = int(row['run'])
        if row['idx'] == '-1':
            tot[r] = float(row['ms'])
            continue
        if is_evta(row['call']):
            conv.setdefault(r, []).append((float(row['start_ms']), float(row['ms'])))
        if r == 1 and row['ops']:
            ops += float(row['ops'])
            if is_evta(row['call']):
                eops += float(row['ops'])
    return tot, conv, ops, eops


def union(iv):
    """ms during which at least one interval (start, ms) runs."""
    iv = sorted((b, b + m) for b, m in iv)
    if not iv:
        return 0.0
    u, cb, ce = 0.0, iv[0][0], iv[0][1]
    for b, e in iv[1:]:
        if b > ce:
            u += ce - cb
            cb, ce = b, e
        else:
            ce = max(ce, e)
    return u + ce - cb


def gops(o, ms):
    return o / (ms * 1e6) if ms > 0 and o > 0 else 0.0


def stat(name, per, o):
    """(median, long text, short text) over runs 2..n."""
    rr = per[1:] if len(per) > 1 else per
    m = median(rr)
    g = f', {gops(o, m):.2f} GOPS' if o > 0 else ''
    return m, f'{name} median {m:.3f} ms (min {min(rr):.3f}, max {max(rr):.3f}){g}', f'{name} median {m:.3f} ms{g}'


def main():
    d, log = sys.argv[1], sys.argv[2]
    names = []
    for l in open(log, errors='replace'):
        m = re.search(r'per statement and run: (\S+)', l)
        if m and m.group(1) not in names:
            names.append(m.group(1))
    cps = []
    for n in names:
        p = os.path.join(d, n)
        if os.path.isfile(p):
            tot, conv, ops, eops = runs_of(p)
            if tot:
                cps.append((n, tot, conv, ops, eops))
    if not cps:
        return
    res = []
    multi = len(cps) > 1
    for n, tot, conv, ops, eops in cps:
        rs = sorted(tot)
        rng = f'runs 2-{len(rs)}' if len(rs) > 1 else 'run 1'
        tag = f'{n}: ' if multi else ''
        whole = [tot[r] for r in rs]
        cv = [sum(m for _, m in conv.get(r, [])) for r in rs]
        print(f'{tag}whole run ms per run: ' + ' '.join(f'{x:.3f}' for x in whole))
        print(f'{tag}conv (EVTA calls) ms per run: ' + ' '.join(f'{x:.3f}' for x in cv))
        _, lw, sw = stat('whole run', whole, ops)
        _, lc, sc = stat('conv', cv, eops)
        print(f'{tag}{rng}: {lw}; {lc}')
        if not multi:
            res.append(f'{rng}: {sw}; {sc}')
    #### copies at once: the same runs in every csv
    if multi and len(set(tuple(sorted(c[1])) for c in cps)) == 1:
        rs = sorted(cps[0][1])
        rng = f'runs 2-{len(rs)}' if len(rs) > 1 else 'run 1'
        k = len(cps)
        ops = sum(c[3] for c in cps)
        eops = sum(c[4] for c in cps)
        whole = [max(c[1][r] for c in cps) for r in rs]
        cu = [union([iv for c in cps for iv in c[2].get(r, [])]) for r in rs]
        cs = [sum(m for c in cps for _, m in c[2].get(r, [])) for r in rs]
        print(f'---- {k} copies at once ----')
        print(f'whole run (slowest copy) ms per run: ' + ' '.join(f'{x:.3f}' for x in whole))
        print(f'conv busy union of the {k} EVTAs ms per run: ' + ' '.join(f'{x:.3f}' for x in cu))
        _, lw, sw = stat(f'whole run x{k}', whole, ops)
        _, lc, sc = stat(f'conv x{k} (busy union)', cu, eops)
        mu = median(cu[1:] if len(cu) > 1 else cu)
        mo = median(cs[1:] if len(cs) > 1 else cs) / mu if mu > 0 else 0.0
        print(f'{rng}: {lw}; {lc}; {mo:.2f} EVTAs at once on average')
        print(f'  ({ops / 1e6:.1f} Mops per run of all copies, of which EVTA {eops / 1e6:.1f}; lockstep (-l): '
              f'{"on" if os.environ.get("MULTI_LOCKSTEP") == "1" else "off"})')
        res.append(f'{k} copies, {rng}: {sw}; {sc}')
    elif multi:
        res.append('copies with different runs: no combined numbers')
    print('; '.join(res))


if __name__ == '__main__':
    main()
