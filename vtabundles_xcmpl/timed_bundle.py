#!/usr/bin/env python3
"""Make a per-statement timed copy of a generated EVTA bundle .cpp.

In every function that has generated "  //Run <kind> : <node>" blocks, each one-line statement gets
  __pt_t0 = __pt_now(); <stmt>  __pt_add(<id>, __pt_t0);
so the time of every call (CPU op, transpose, VTABuffer*, VTA conv, malloc/free) is attributed to
(<kind>, <node>, <called function>). Each run of the inference function overwrites timing.csv in the
working directory; every run is kept (first = warm-up in the stock mains) and printed at exit,
one line per statement in execution order with one time/GOPS column per run.
Files without //Run blocks (mains, helpers) are copied unchanged.

usage: timed_bundle.py <in.cpp> <out.cpp> [<csv name>]
       timed_bundle.py --repeat-only <in.cpp> <out.cpp>
#### JO added (2026-10-29): every inference function (one with //Run blocks) is renamed <name>__once and wrapped:
#### with BUNDLE_RUNS=<n> (n > 2) in the environment its first call runs it n - 1 times, later calls once, so the
#### stock mains (warm-up call + timed call) run n inferences; unset = unchanged. --repeat-only adds just the wrapper
#### (the _prof builds, NESTC_EVTA_PROFILE=ON); hw*/script/run_prof.sh -n <n> sets BUNDLE_RUNS (single set only).
#### JO added (2026-10-29): both modes also call evta_lockstep_hook() (weak) before every EVTA call (MULTI_LOCKSTEP).
#### JO added (2026-10-26): <csv name> (default timing.csv) for executables with several timed .cpp (multi-EVTA
#### copies, VTA partitions), which would otherwise overwrite one file; the table header names the .cpp.
"""
import re, sys

RUN = re.compile(r'^\s*//Run (\w+) : (\S+)')
CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
NOT_CALLS = {'sizeof', 'if', 'for', 'while', 'return'}

PRELUDE = r'''
// ---- per-statement timing (inserted by timed_bundle.py) ----
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <string>
#ifndef PT_CSV
#define PT_CSV "timing.csv"
#endif
#ifndef PT_NAME
#define PT_NAME ""
#endif
namespace {
//#### JO changed (2026-10-29): + evta (the statement passes vtaCmdH: an EVTA call) for the split summary
struct PtEntry { const char *kind, *node, *call; double ms, start, ops; int evta; };
extern PtEntry __pt_tab[];
extern const int __pt_n;
// per-run copies: runs 1..PT_MAXRUNS-1 keep their own column, later runs share the last one
#define PT_MAXRUNS 4
double *__pt_ms[PT_MAXRUNS], *__pt_st[PT_MAXRUNS], __pt_tot[PT_MAXRUNS];
int __pt_runno[PT_MAXRUNS];
double __pt_t0, __pt_begin_ms, __pt_last_total;
int __pt_runs = 0;
inline double __pt_now() {
  timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6;
}
inline void __pt_add(int i, double t0) {
  __pt_tab[i].ms += __pt_now() - t0; __pt_tab[i].start = t0 - __pt_begin_ms;
}
// every statement in execution order, one ms column per run (first = warm-up), GOPS of each run
void __pt_summary() {
  int nc = __pt_runs < PT_MAXRUNS ? __pt_runs : PT_MAXRUNS;
  //#### JO changed: name of the timed .cpp in the header (several tables in one executable)
  //printf("\n==== timing per statement, %d run(s) ====\n", __pt_runs);
  printf("\n==== timing per statement %s, %d run(s) ====\n", PT_NAME, __pt_runs);
  for (int r = 0; r < nc; r++) {
    double sum = 0;
    for (int i = 0; i < __pt_n; i++) sum += __pt_ms[r][i];
    printf("run #%d: total %.3f ms, statements %.3f ms\n", __pt_runno[r], __pt_tot[r], sum);
  }
  printf("%4s %10s", "idx", "start_ms");
  for (int r = 0; r < nc; r++) { char h[32]; snprintf(h, sizeof h, "run%d_ms", __pt_runno[r]); printf(" %10s", h); }
  for (int r = 0; r < nc; r++) { char h[32]; snprintf(h, sizeof h, "GOPS%d", __pt_runno[r]); printf(" %7s", h); }
  printf("  %-18s %-24s %s\n", "kind", "call", "node");
  for (int i = 0; i < __pt_n; i++) {
    PtEntry &e = __pt_tab[i];
    printf("%4d %10.3f", i, __pt_st[nc - 1][i]);
    for (int r = 0; r < nc; r++) printf(" %10.3f", __pt_ms[r][i]);
    for (int r = 0; r < nc; r++) {
      if (e.ops > 0 && __pt_ms[r][i] > 0) printf(" %7.2f", e.ops / (__pt_ms[r][i] * 1e6));
      else printf(" %7s", "");
    }
    printf("  %-18s %-24s %s\n", e.kind, e.call, e.node);
  }
  //#### JO changed (2026-10-29): EVTA calls and CPU conv/fc apart (fc on the CPU took 4.4 ms of Resnet18's "conv/fc")
  for (int r = 0; r < nc; r++) {
    double eOps = 0, eMs = 0, cOps = 0, cMs = 0;
    for (int i = 0; i < __pt_n; i++)
      if (__pt_tab[i].ops > 0) {
        if (__pt_tab[i].evta) { eOps += __pt_tab[i].ops; eMs += __pt_ms[r][i]; }
        else { cOps += __pt_tab[i].ops; cMs += __pt_ms[r][i]; }
      }
    if (eOps + cOps > 0) {
      printf("run #%d ops:", __pt_runno[r]);
      if (eOps > 0) printf(" EVTA calls %.1f Mops in %.3f ms = %.2f GOPS;", eOps * 1e-6, eMs, eOps / (eMs * 1e6));
      if (cOps > 0) printf(" CPU conv/fc %.1f Mops in %.3f ms = %.2f GOPS;", cOps * 1e-6, cMs, cOps / (cMs * 1e6));
      printf(" whole run %.3f ms = %.2f GOPS\n", __pt_tot[r], (eOps + cOps) / (__pt_tot[r] * 1e6));
    }
  }
  printf("per statement and run: %s (start_ms column above is from the last run)\n", PT_CSV);
}
void __pt_begin() {
  for (int i = 0; i < __pt_n; i++) __pt_tab[i].ms = __pt_tab[i].start = 0;
  __pt_begin_ms = __pt_now();
}
void __pt_end() {
  __pt_last_total = __pt_now() - __pt_begin_ms;
  int r = __pt_runs < PT_MAXRUNS ? __pt_runs : PT_MAXRUNS - 1;
  if (!__pt_ms[r]) { __pt_ms[r] = new double[__pt_n]; __pt_st[r] = new double[__pt_n]; }
  for (int i = 0; i < __pt_n; i++) { __pt_ms[r][i] = __pt_tab[i].ms; __pt_st[r][i] = __pt_tab[i].start; }
  __pt_tot[r] = __pt_last_total;
  __pt_runno[r] = ++__pt_runs;
  if (__pt_runs == 1) atexit(__pt_summary);
  // timing.csv: all runs, one row per (run, statement)
  FILE *f = fopen(PT_CSV, __pt_runs == 1 ? "w" : "a");
  if (!f) return;
  if (__pt_runs == 1) fprintf(f, "run,idx,start_ms,kind,node,call,ms,ops,gops\n");
  for (int i = 0; i < __pt_n; i++)
    fprintf(f, "%d,%d,%.4f,%s,%s,%s,%.4f,%.0f,%.3f\n", __pt_runs, i, __pt_tab[i].start, __pt_tab[i].kind, __pt_tab[i].node,
            __pt_tab[i].call, __pt_tab[i].ms, __pt_tab[i].ops,
            __pt_tab[i].ms > 0 ? __pt_tab[i].ops / (__pt_tab[i].ms * 1e6) : 0.0);
  fprintf(f, "%d,-1,,total,,,%.4f,,\n", __pt_runs, __pt_last_total);
  fclose(f);
}
} // namespace
// ---- end of timing prelude ----
'''


def call_args(stmt, name):
    """Top-level comma-separated arguments of the first call to name in stmt."""
    i = stmt.index(name + '(') + len(name) + 1
    args, depth, cur = [], 0, ''
    for ch in stmt[i:]:
        if ch == '(':
            depth += 1
        elif ch == ')':
            if depth == 0:
                break
            depth -= 1
        if ch == ',' and depth == 0:
            args.append(cur.strip())
            cur = ''
        else:
            cur += ch
    args.append(cur.strip())
    return args


def call_ops(stmt, name):
    """Multiply-add ops (2 per MAC) of a conv / fc call, from its literal shape arguments; 0 if unknown."""
    try:
        a = call_args(stmt, name)
        n = lambda k: int(a[k])
        if name in ('convolution_wo_tr', 'convolution_wo_tr_scale'):
            # (in, ker, bias, out, N, H, W, C, KN, KH, KW, pad, stride, relu, bias, shift, out_h, out_w, ...)
            return 2 * n(4) * n(16) * n(17) * n(8) * n(9) * n(10) * n(7)
        if name == 'convolutionFloat':
            # (in, ker, bias, out, N, H, W, C, KN, KH, KW, pad, stride, group, dilation, relu, bias, out_h, out_w)
            return 2 * n(4) * n(17) * n(18) * n(8) * n(9) * n(10) * n(7) // n(13)
        #### JO added: int8 CPU conv (e.g. Resnet50 / VGG16 first layer)
        if name == 'nonvtaconvolution':
            # (in,s,o, ker,s,o, bias,s,o, out,s,o, N, H, W, C, KN, KH, KW, pad, stride, group, dilation, relu, bias, out_h, out_w)
            return 2 * n(12) * n(25) * n(26) * n(16) * n(17) * n(18) * n(15) // n(21)
        if name == 'fullyconnected':
            # (in,s,o, ker,s,o, bias,s,o, out,s,o, inDim0, inDim1, kerDim0, kerDim1, ...)
            return 2 * n(12) * n(13) * n(15)
    except (ValueError, IndexError):
        pass
    return 0


def call_name(stmt):
    for m in CALL.finditer(stmt):
        if m.group(1) not in NOT_CALLS:
            return m.group(1)
    return 'assign'


DEF = re.compile(r'^(\w[\w\s\*]*?)\b(\w+)\((.*)\)\s*\{\s*$')


#### JO added (2026-10-29): BUNDLE_RUNS repeat wrapper (see the usage above)
def add_repeat(lines):
    """Rename each inference function to <name>__once; return (lines, wrapper lines to append)."""
    out, wrap = [], []
    for i, l in enumerate(lines):
        m = DEF.match(l)
        if m:
            body = []
            for k in range(i + 1, len(lines)):
                if lines[k].startswith('}'):
                    break
                body.append(lines[k])
            if any(RUN.match(b) for b in body):
                ret, name, params = m.group(1).strip(), m.group(2), m.group(3)
                args = ', '.join(re.findall(r'\w+', a)[-1] for a in params.split(',') if a.strip())
                once = f'{name}__once({args})'
                wrap += ['', '// ---- BUNDLE_RUNS repeat wrapper (inserted by timed_bundle.py) ----',
                         f'{ret} {name}({params}) {{',
                         '  static int __rp_calls = 0;',
                         '  int __rp_n = 1;',
                         '  if (__rp_calls++ == 0) {',
                         '    const char *__rp_e = getenv("BUNDLE_RUNS");',
                         '    if (__rp_e && atoi(__rp_e) > 2) __rp_n = atoi(__rp_e) - 1;',
                         '  }',
                         f'  for (int __rp_i = 1; __rp_i < __rp_n; __rp_i++) {once};',
                         f'  return {once};' if ret != 'void' else f'  {once};',
                         '}']
                l = l.replace(f'{name}(', f'{name}__once(', 1)
        out.append(l)
    return out, wrap


#### JO added (2026-10-29): lockstep hook before every EVTA call (a statement passing vtaCmdH). The multi-EVTA main
#### defines evta_lockstep_hook() (a barrier of the copies when MULTI_LOCKSTEP=1); elsewhere the weak symbol is 0.
LOCKSTEP_DECL = 'extern "C" void evta_lockstep_hook() __attribute__((weak));  // lockstep hook (timed_bundle.py)'
LOCKSTEP_CALL = '  if (evta_lockstep_hook) evta_lockstep_hook();'


def add_lockstep(lines):
    out, in_fn = [], False
    for i, l in enumerate(lines):
        if not in_fn and DEF.match(l):
            body = []
            for k in range(i + 1, len(lines)):
                if lines[k].startswith('}'):
                    break
                body.append(lines[k])
            in_fn = any(RUN.match(b) for b in body)
        elif in_fn and l.startswith('}'):
            in_fn = False
        s = l.strip()
        if in_fn and 'vtaCmdH' in s and s.endswith(';') and not s.startswith('//'):
            out.append(LOCKSTEP_CALL)
        out.append(l)
    return out


def main():
    #### JO added (2026-10-29): --repeat-only
    repeat_only = sys.argv[1] == '--repeat-only'
    if repeat_only:
        del sys.argv[1]
    src, dst = sys.argv[1], sys.argv[2]
    csv = sys.argv[3] if len(sys.argv) > 3 else 'timing.csv'
    lines = open(src).read().split('\n')
    if not any(RUN.match(l) for l in lines):
        open(dst, 'w').write('\n'.join(lines))
        return
    lines, wrap = add_repeat(lines)
    lines = add_lockstep(lines)
    if repeat_only:
        last_inc = max(i for i, l in enumerate(lines[:50]) if l.startswith('#include'))
        lines[last_inc + 1:last_inc + 1] = ['#include <stdlib.h>  // getenv, atoi (BUNDLE_RUNS wrapper)', LOCKSTEP_DECL]
        open(dst, 'w').write('\n'.join(lines + wrap) + '\n')
        return
    out, tab = [], []
    kind = node = None
    in_fn = False
    for i, l in enumerate(lines):
        # a function definition at column 0 whose body contains //Run blocks
        if not in_fn and re.match(r'^\w[\w\s\*]*\(.*\)\s*\{\s*$', l):
            body = []
            for k in range(i + 1, len(lines)):
                if lines[k].startswith('}'):
                    break
                body.append(lines[k])
            if any(RUN.match(b) for b in body):
                in_fn, kind, node = True, 'prologue', '-'
                out += [l, '  __pt_begin();']
                continue
        if in_fn:
            if l.startswith('}'):
                in_fn = False
                out.append(l)
                continue
            m = RUN.match(l)
            if m:
                kind, node = m.group(1), m.group(2)
            s = l.strip()
            if s.startswith('return'):
                out += ['  __pt_end();', l]
                continue
            if s and not s.startswith('//') and s.endswith(';'):
                c = call_name(s)
                tab.append((kind, node, c, call_ops(s, c), int('vtaCmdH' in s)))
                out += ['  __pt_t0 = __pt_now();', l, f'  __pt_add({len(tab) - 1}, __pt_t0);']
                continue
        out.append(l)
    # prelude after the last leading #include, table at the end of the file
    last_inc = max(i for i, l in enumerate(out[:50]) if l.startswith('#include'))
    #out[last_inc + 1:last_inc + 1] = PRELUDE.split('\n')
    out[last_inc + 1:last_inc + 1] = [f'#define PT_CSV "{csv}"', f'#define PT_NAME "{src.split("/")[-1]}"', LOCKSTEP_DECL] + PRELUDE.split('\n')
    out.append('namespace {')
    out.append('PtEntry __pt_tab[] = {')
    out += [f'  {{"{k}", "{n}", "{c}", 0, 0, {o}, {e}}},' for k, n, c, o, e in tab]
    out.append('};')
    out.append(f'const int __pt_n = {len(tab)};')
    out.append('} // namespace')
    open(dst, 'w').write('\n'.join(out + wrap) + '\n')


if __name__ == '__main__':
    main()
