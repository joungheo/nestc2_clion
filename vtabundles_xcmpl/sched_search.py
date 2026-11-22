#!/usr/bin/env python3
"""EVTA conv schedule search on the board (2026-11-22, docs/evta_sched_search.md).

A conv schedule is (nVirtualThread, tileH, tileW) per conv shape (include/Backends/VTA/VTASchedules.h); vtalib-systolic
EVTA_SCHED=<H>,<W>,<C>,<KN>,<KH>,<stride>:<vt>,<tileH>,<tileW>[;...] replaces it at run time. One run gives every shape
one candidate at once (EVTA time does not depend on the values), timing.csv of a _systimed executable gives each
conv statement's time, so a model needs as many runs as its longest candidate list.

  sched_search.py cands <bundle.cpp> > cands.json        candidates of every EVTA conv shape of a bundle (on the PC)
  sched_search.py env cands.json <i>                     the EVTA_SCHED value of run i (empty: the bundle's schedules)
  sched_search.py runs cands.json                        number of runs
  sched_search.py run cands.json <out dir> <Dir> [runs]  on the board as root in vtabundles_xcmpl/: every candidate set
                                                         (<out dir>/<name>.env / .csv / .out / .ok, failing pairs in bad.txt)
  sched_search.py pick cands.json <out dir>              per shape the fastest candidate, the table
  sched_search.py final cands.json <dir>                 EVTA_SCHED with the fastest candidate of every shape

Candidates: tileH a divisor of the output height, tileW a divisor of the output width >= width / 4 (tiles that do not
divide the output can change the result), vt 1 or 2 (vtalib uses 1 when 2 does not divide the output channel blocks),
the tile + one entry in a thread's acc half (2048 / vt entries) and one channel block's input tile in a thread's inp
half (2048 / vt). Results are checked afterwards (EVTA_DUMP_CONV dumps against the bundle's schedules).
"""
import csv, glob, json, os, re, statistics, sys

CALL = re.compile(r'^\s*(convolution_wo_tr(?:_pool)?)\((.*)\);')


def divisors(n):
    return [d for d in range(1, n + 1) if n % d == 0]


def cands(path):
    shapes = {}
    for line in open(path):
        m = CALL.match(line)
        if not m:
            continue
        a = [x.strip() for x in m.group(2).split(',')]
        # (in, kernel, bias, out, N, H, W, C, KN, KH, KW, pad, stride, relu, bias, shift, out_h, out_w, cmd, vt, th, tw, id, ...)
        H, W, C, KN, KH, KW, pad, s = (int(x) for x in a[5:13])
        oh, ow = int(a[16]), int(a[17])
        vt, th, tw = int(a[19]), int(a[20]), int(a[21])
        key = f'{H},{W},{C},{KN},{KH},{s}'
        out = a[3].split(')')[-1].strip()
        e = shapes.setdefault(key, dict(key=key, KH=KH, KW=KW, stride=s, oh=oh, ow=ow, cur=[vt, th, tw], outs=[], cands=[], pool=0))
        if m.group(1) == 'convolution_wo_tr_pool':
            e['pool'] = 1   # the fused 2x2 / 2 or 3x3 / 2 / 1 max pool: the pooled tile is tile / 2
        e['outs'].append(out)
    for e in shapes.values():
        KH, KW, s, oh, ow = e['KH'], e['KW'], e['stride'], e['oh'], e['ow']
        KN = int(e['key'].split(',')[3])
        c = []
        for vt in (2, 1):
            if vt == 2 and (KN // 16) % 2:
                continue
            for th in divisors(oh):
                for tw in divisors(ow):
                    if tw * 4 < ow:
                        continue
                    if e['pool'] and (th % 2 or tw % 2):
                        continue
                    if th * tw < min(4, oh * ow):   # uop_kernel.h VerifyDep: the same acc entry twice in a row
                        continue
                    if th * tw + 1 > 2048 // vt:
                        continue
                    if ((th - 1) * s + KH) * ((tw - 1) * s + KW) > 2048 // vt:
                        continue
                    c.append([vt, th, tw])
        if e['cur'] not in c:
            c.insert(0, e['cur'])
        e['cands'] = c
    return list(shapes.values())


def env(sh, i):
    if i < 0:
        return ''
    return ';'.join(f"{e['key']}:{','.join(map(str, e['cands'][i % len(e['cands'])]))}" for e in sh)


def read_csv(path, outs_by_key):
    """median over runs 2.. of each shape's summed conv statement time (ms)"""
    per_run = {}
    for r in csv.DictReader(open(path)):
        run = int(r['run'])
        if run < 2:
            continue
        if r['call'] not in ('convolution_wo_tr', 'convolution_wo_tr_pool', 'preprocess_conv0pack_stream'):
            continue
        node = r['node']
        for key, outs in outs_by_key.items():
            if any(o.startswith(node + '_') or o == node for o in outs):
                per_run.setdefault(key, {}).setdefault(run, 0.0)
                per_run[key][run] += float(r['ms'])
                break
    return {k: statistics.median(v.values()) for k, v in per_run.items()}


def applied(sh, path):
    """the schedule of every shape in the run of <path>.csv: <path>.env (EVTA_SCHED), the bundle's for the others"""
    e = {x['key']: tuple(x['cur']) for x in sh}
    envf = path[:-4] + '.env'
    if os.path.exists(envf):
        for one in open(envf).read().strip().split(';'):
            if ':' in one:
                k, v = one.split(':')
                e[k] = tuple(int(x) for x in v.split(','))
    return e


def pick(sh, d):
    outs = {e['key']: e['outs'] for e in sh}
    t = {e['key']: {} for e in sh}
    for p in glob.glob(f'{d}/*.csv'):
        if not os.path.exists(p[:-4] + '.ok'):
            continue
        x = read_csv(p, outs)
        for k, c in applied(sh, p).items():
            if k in x:
                t[k].setdefault(c, []).append(x[k])
    res = []
    for e in sh:
        k = e['key']
        tk = {c: statistics.median(v) for c, v in t[k].items()}
        best = min(tk, key=tk.get) if tk else None
        res.append(dict(key=k, cur=e['cur'], base=None, best=list(best) if best else None, best_ms=tk.get(best),
                        cur_ms=tk.get(tuple(e['cur'])), n=len(tk), times={','.join(map(str, c)): v for c, v in tk.items()}))
    return res


def run_all(sh, d, D, R):
    """board, as root, in vtabundles_xcmpl/: every candidate run; a failing run (exit != 0) is split in halves of its shapes
    until the failing (shape, candidate) pairs are found (bad.txt), the rest kept"""
    import subprocess
    os.makedirs(d, exist_ok=True)
    bad = open(f'{d}/bad.txt', 'a')

    def one(name, pairs):
        envs = ';'.join(f'{k}:{",".join(map(str, c))}' for k, c in pairs)
        open(f'{d}/{name}.env', 'w').write(envs)
        o = subprocess.run(['hw/script/run_prof.sh', '-s', '_systimed', '-n', str(R), D], env=dict(os.environ, EVTA_SCHED=envs),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.decode(errors='replace')
        open(f'{d}/{name}.out', 'w').write(o)
        ok = re.search(r'exit=0 ', o) is not None
        m = re.findall(r'whole run median ([0-9.]+) ms', o)
        if ok:
            import shutil
            shutil.copy(f'hw/{D}/timing.csv', f'{d}/{name}.csv')
            open(f'{d}/{name}.ok', 'w').close()
        print(name, len(pairs), 'ok' if ok else 'FAIL', m[-1] if m else '', flush=True)
        return ok

    def tryset(name, pairs):
        if one(name, pairs) or not pairs:
            return
        if len(pairs) == 1:
            bad.write(f'{pairs[0][0]}:{",".join(map(str, pairs[0][1]))}\n'); bad.flush()
            return
        h = len(pairs) // 2
        tryset(name + 'a', pairs[:h])
        tryset(name + 'b', pairs[h:])

    one('base0', [])
    n = max(len(e['cands']) for e in sh)
    for i in range(n):
        pairs = [(e['key'], e['cands'][i % len(e['cands'])]) for e in sh if e['cands'][i % len(e['cands'])] != e['cur']]
        tryset(str(i), pairs)
    one('base1', [])


def main():
    cmd = sys.argv[1]
    if cmd == 'cands':
        json.dump(cands(sys.argv[2]), sys.stdout, indent=1)
        return
    sh = json.load(open(sys.argv[2]))
    if cmd == 'env':
        print(env(sh, int(sys.argv[3])))
    elif cmd == 'run':
        run_all(sh, sys.argv[3], sys.argv[4], int(sys.argv[5]) if len(sys.argv) > 5 else 4)
    elif cmd == 'runs':
        print(max(len(e['cands']) for e in sh))
    elif cmd == 'pick':
        tot_b = tot_n = 0.0
        print(f"{'shape (H,W,C,KN,K,s)':28} {'now':>10} {'ms':>7} {'best':>10} {'ms':>7} {'gain':>6}  candidates")
        for r in pick(sh, sys.argv[3]):
            b = r['cur_ms']
            gain = (b - r['best_ms']) if b is not None and r['best_ms'] is not None else 0
            tot_b += b or 0
            tot_n += (r['best_ms'] if gain > 0 else b) or 0
            print(f"{r['key']:28} {','.join(map(str, r['cur'])):>10} {b or 0:7.3f} {','.join(map(str, r['best'] or [])):>10} "
                  f"{r['best_ms'] or 0:7.3f} {gain:6.3f}  {r['n']}")
        print(f"sum of the conv statements: {tot_b:.3f} -> {tot_n:.3f} ms")
    elif cmd == 'final':
        out = []
        for r in pick(sh, sys.argv[3]):
            if r['best'] and r['cur_ms'] is not None and r['best_ms'] < r['cur_ms'] * 0.99 and r['best'] != r['cur']:
                out.append(f"{r['key']}:{','.join(map(str, r['best']))}")
        print(';'.join(out))


if __name__ == '__main__':
    main()
