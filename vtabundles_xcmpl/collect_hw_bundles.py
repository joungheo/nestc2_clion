#!/usr/bin/env python3
"""Collect every vta/bundles bundle into hw/<Dir> for the ZCU102 EVTA build (copy only, no compile).

Bundle sources/data are taken from the simulator build (Debug2_remote/vta/bundles/<Dir>, produced by
Debug2_remote/build_sim_bundles.sh) and from the source tree (vta/bundles/<Dir>); the build dir wins
when both have a file, except sources add_executable names from the source dir (as NEST-C compiles them). Files already in hw/<Dir> (executables, *_prof, logs, output.bin) are kept;
copied files overwrite older copies.

Compiling is done by CMake in this directory (CMakeLists.txt; vtalib is built once per build dir):
  cd build && make -j2 hw_bundles          (profiling: build_prof, -DNESTC_EVTA_PROFILE=ON)

Output: hw/<Dir>/ data + sources, and hw/run_all.sh (sudo) which reloads the bitstream,
runs each bundle and compares with the golden where the CMake test does (USE_DIFF).

usage: ./collect_hw_bundles.py [--set multievtax2|multievtax4] [Dir ...]

#### JO added: --set picks a bundle set (source dir, hw dir, bitstream). The default "single" is vta/bundles ->
#### hw/ for zcu102_1x16_*_16_16_19_18 (one executable per Dir). "multievtax2" is vta/multievtax2_bundles ->
#### hw_multievtax2/ for the 2-EVTA bitstream; every executable a test runs gets a run line, and hw/run_prof.sh
#### is copied there. Build those with a build dir configured with -DBUNDLE_SET=multievtax2.
#### JO added (2026-10-26): EXTRA dirs are not in the NEST-C bundle sets; their generator script makes them here
#### (MultiEVTAConv4Test from hw/Conv4Test, so collect the single set first) and they get a run line too.
"""
import filecmp, os, re, shlex, shutil, subprocess, sys

#### JO changed: this script now lives in nest-compiler/vtabundles_xcmpl -> NEST-C root is the parent dir
# R = '/home/xilinx/JO/npuprg/nestc2_clion/nest-compiler'
HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.dirname(HERE)
#### JO changed: per-set source/hw dirs and bitstream (see --set above)
# SRC = f'{R}/vta/bundles'
# BLD = f'{R}/Debug2_remote/vta/bundles'
# HW = f'{HERE}/hw'
SETS = {  # name: (bundle dir under the NEST-C root, hw dir here, bitstream in /lib/firmware, all test executables)
    #### JO changed (2026-10-26): the Y219 build of vta-hw-zcu102-single (same results as the etri bitstream,
    #### EVTA time 1-21 % shorter); the etri one stays usable with BIN=zcu102_1x16_i8w8a32_16_16_19_18.bin
    # 'single': ('vta/bundles', 'hw', 'zcu102_1x16_i8w8a32_16_16_19_18.bin', False),
    'single': ('vta/bundles', 'hw', 'vtahw_single_y219.bin', False),
    #### JO changed (2026-10-26): the lab board's Y219 build of vta-hw-zcu102-multi2 (etri bitstream: stuck cells)
    # 'multievtax2': ('vta/multievtax2_bundles', 'hw_multievtax2', 'zcu102_2_1x16_i8w8a32_16_15_18_17.bin', True),
    'multievtax2': ('vta/multievtax2_bundles', 'hw_multievtax2', 'vtahw_multi2_y219.bin', True),
    #### JO added: 4 EVTAs (vta-hw-zcu102-multi4 on the lab board: the Y219 build, /lib/firmware/vtahw_multi4_y219.bin)
    'multievtax4': ('vta/multievta_bundles', 'hw_multievtax4', 'vtahw_multi4_y219.bin', True),
}
#### JO added: per set, dirs made by a script here: (dir, executable, arguments, golden, generator command)
CONV4_GEN = 'hw_multievtax4/MultiEVTAConv4TestSrc/make_multi4conv4.py'
EXTRA = {
    'multievtax2': [('MultiEVTAConv4Test', 'multi4conv4Bundle', '3 100', 'vtaConv4TestGolden.bin', CONV4_GEN)],
    'multievtax4': [('MultiEVTAConv4Test', 'multi4conv4Bundle', '15 100', 'vtaConv4TestGolden.bin', CONV4_GEN)],
}
SET = 'single'
if sys.argv[1:2] == ['--set']:
    SET = sys.argv[2]
    del sys.argv[1:3]
SRC = f'{R}/{SETS[SET][0]}'
BLD = f'{R}/Debug2_remote/{SETS[SET][0]}'
HW = f'{HERE}/{SETS[SET][1]}'
BIN = SETS[SET][2]
ALL_EXES = SETS[SET][3]
SKIP = {'BinaryNet', 'BiRealNet', 'Resnet18RelayEVTAPartitionTest'}   # BNN / Relay(TVM)
DATA_EXT = ('.cpp', '.h', '.bin', '.o')


def copy(src, out):
    """Copy into dir out only if the content differs, with a fresh mtime so make rebuilds."""
    dst = f'{out}/{os.path.basename(src)}'
    if os.path.exists(dst) and filecmp.cmp(src, dst, shallow=False):
        return 0
    shutil.copy(src, dst)
    return 1


def parse(d):
    s = re.sub(r'#.*', '', open(f'{SRC}/{d}/CMakeLists.txt').read())
    #### JO changed: keep the first add_executable of a name (the generation branch); later ones are the
    #### precompiled-download (AWS) branch, e.g. Resnet18PartitionTest still lists p0.o there
    # exes = {n: srcs.split() for n, srcs in re.findall(r'add_executable\(\s*(\S+)\s+([^)]*)\)', s)}
    exes = {}
    for n, srcs in re.findall(r'add_executable\(\s*(\S+)\s+([^)]*)\)', s):
        exes.setdefault(n, srcs.split())
    tests = {}
    for t in re.findall(r'add_nestc_test\(([^)]*)\)', s):
        a = t.split()
        name = a[a.index('NAME') + 1]
        params = a[a.index('PARAMS') + 1] if 'PARAMS' in a else ''
        golden = a[a.index('DIFF_TARGET') + 1] if 'DIFF_TARGET' in a else ''
        tests[name] = (params, golden)
    return exes, tests


def resolve(p, d):
    """Map a CMake path to (kind, value): 'local' file name in hw/<d> or an absolute path."""
    p = p.replace('${GLOW_SOURCE_DIR}', f'{R}/glow').replace('${NESTC_ROOT_DIR}', R)
    for pre in ('${CMAKE_CURRENT_BINARY_DIR}/', '${CMAKE_CURRENT_SOURCE_DIR}/'):
        if p.startswith(pre):
            return os.path.basename(p)
    return p


def main():
    #### JO changed: EXTRA dirs are handled below, not parsed from SRC
    # dirs = sys.argv[1:] or sorted(x for x in os.listdir(SRC)
    #                               if os.path.isfile(f'{SRC}/{x}/CMakeLists.txt') and x not in SKIP)
    extra = [x for x in EXTRA.get(SET, []) if not sys.argv[1:] or x[0] in sys.argv[1:]]
    dirs = [x for x in sys.argv[1:] if x not in {e[0] for e in EXTRA.get(SET, [])}] if sys.argv[1:] else \
        sorted(x for x in os.listdir(SRC) if os.path.isfile(f'{SRC}/{x}/CMakeLists.txt') and x not in SKIP)
    dirs_all = dirs + [e[0] for e in extra]
    runs, report = [], []
    for d in dirs:
        exes, tests = parse(d)
        # the bundle executable is the one a test runs (skips the *Profiling variants)
        cand = [n for n in exes if n in tests] or list(exes)
        if not cand:
            continue
        #### JO changed: multi-EVTA sets run every test executable of a Dir (e.g. threaded + single-thread)
        # exe = cand[0]
        sel = cand if ALL_EXES else cand[:1]
        exe = sel[0]
        out = f'{HW}/{d}'
        os.makedirs(out, exist_ok=True)
        # Sources add_executable takes from the source dir come from there, as in the NEST-C build: e.g.
        # Resnet18PartitionTest/main.cpp (1 command handle) vs the generated one in the build dir
        # (4 handles x 64 MB CMA > the board's 128 MB). Everything else: the build dir wins.
        # src_owned = {s for s in exes[exe] if not s.startswith('${') and os.path.isfile(f'{SRC}/{d}/{s}')}
        src_owned = {s for e in sel for s in exes[e] if not s.startswith('${') and os.path.isfile(f'{SRC}/{d}/{s}')}
        pick = {}   # file name -> source path; decided first so each file is copied once
        for base in (f'{SRC}/{d}', f'{BLD}/{d}'):
            if os.path.isdir(base):
                for f in os.listdir(base):
                    if base.startswith(BLD) and f in src_owned:
                        continue
                    if f.endswith(DATA_EXT) and os.path.isfile(f'{base}/{f}'):
                        pick[f] = f'{base}/{f}'
        n = sum(copy(p, out) for p in pick.values())
        #### JO changed: check/report every selected executable
        for exe in sel:
            missing = [os.path.basename(s) for s in exes[exe] if not os.path.exists(f'{out}/{os.path.basename(s)}')]
            if missing:
                report.append(f'{d:30s} MISSING {", ".join(missing)}  (run Debug2_remote/build_sim_bundles.sh {exe})')
            else:
                report.append(f'{d:30s} OK   {n:3d} updated  {exe}')
                params, golden = tests.get(exe, ('', ''))
                runs.append((d, exe, resolve(params, d) if params else '', resolve(golden, d) if golden else ''))
            print(report[-1], flush=True)
    #### JO added: EXTRA dirs: run the generator, then a run line like the others
    for d, exe, args, golden, gen in extra:
        r = subprocess.run([sys.executable, f'{HERE}/{gen}', f'{HERE}/hw/Conv4Test', f'{HW}/{d}'],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        if r.returncode:
            report.append(f'{d:30s} MISSING (generator {gen} failed: {r.stdout.strip().splitlines()[-1:]})')
        else:
            report.append(f'{d:30s} OK   generated   {exe}')
            runs.append((d, exe, args, golden))
        print(report[-1], flush=True)

    #### JO added: with Dir arguments, keep the run lines of the other dirs already in run_all.sh
    #### (it used to be rewritten with only the given dirs)
    #### JO changed: keyed by "Dir exe" (a multi-EVTA Dir can have several executables)
    # lines = {d: f'run {d} {exe} {shlex.quote(inp)} {shlex.quote(gold)}' for d, exe, inp, gold in runs}
    lines = {f'{d} {exe}': f'run {d} {exe} {shlex.quote(inp)} {shlex.quote(gold)}' for d, exe, inp, gold in runs}
    if sys.argv[1:] and os.path.isfile(f'{HW}/run_all.sh'):
        for l in open(f'{HW}/run_all.sh').read().split('\n'):
            m = re.match(r'run (\S+) (\S+) ', l)
            if m and m.group(1) not in dirs_all:
                lines[f'{m.group(1)} {m.group(2)}'] = l
    with open(f'{HW}/run_all.sh', 'w') as f:
        f.write('#!/bin/bash\n# Run all ZCU102 bundles (generated by collect_hw_bundles.py). usage: sudo ./run_all.sh [Dir ...]\n'
                #### JO changed: BIN can be overridden, e.g. sudo BIN=evta_zcu102_1evta.bin ./run_all.sh
                f'set -u\ncd "$(dirname "$0")"\nBIN=${{BIN:-{BIN}}}\n'
                '[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }\n'
                #### JO added (2026-10-26): one EVTA run at a time on the board (each run reloads the PL; two runs
                #### swap bitstreams under each other -> SError / bus errors); run_prof.sh takes the same lock
                'exec 9>/tmp/evta_run.lock; flock -n 9 || { echo "another EVTA run is active (/tmp/evta_run.lock)"; exit 1; }\n'
                'ONLY="$*"\n: > results.txt\n'
                #### JO changed: multi-EVTA sets have several executables per Dir -> one log/rc per executable
                f'LOG() {{ echo {"$1.log" if ALL_EXES else "run.log"}; }}  # log name for executable $1\n'
                'run() {  # dir exe input golden\n'
                '  [ -n "$ONLY" ] && [[ " $ONLY " != *" $1 "* ]] && return\n'
                '  echo "================ $1 ($2)"\n'
                '  echo 0 > /sys/class/fpga_manager/fpga0/flags; echo $BIN > /sys/class/fpga_manager/fpga0/firmware\n'
                '  python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()" 2>/dev/null\n'
                '  ( cd $1 && rm -f output.bin && timeout 300 ./$2 $3 > $(LOG $2) 2>&1; echo $? > $(LOG $2).rc )\n'
                '  rc=$(cat $1/$(LOG $2).rc); chown -R --reference=. $1\n'
                '  if [ $rc = 124 ]; then res=TIMEOUT\n'
                '  elif [ $rc != 0 ]; then res="FAIL(rc=$rc)"\n'
                '  elif [ -n "$4" ]; then cmp -s $1/output.bin $1/$4 && res=PASS || res=MISMATCH\n'
                '  else res="RAN (no golden; see $1/$(LOG $2))"; fi\n'
                '  tail -3 $1/$(LOG $2); printf "%-30s %-34s %s\\n" $1 $2 "$res" | tee -a results.txt\n}\n')
        #### JO changed: write the merged lines, sorted by dir
        # for d, exe, inp, gold in runs:
        #     f.write(f'run {d} {exe} {shlex.quote(inp)} {shlex.quote(gold)}\n')
        for d in sorted(lines):
            f.write(lines[d] + '\n')
        f.write('echo; echo "== summary"; cat results.txt\n')
    os.chmod(f'{HW}/run_all.sh', 0o755)
    #### JO added: the profiling runner works from any hw dir (it reads BIN and the run lines from run_all.sh)
    if HW != f'{HERE}/hw':
        copy(f'{HERE}/hw/run_prof.sh', HW)
    open(f'{HW}/collect_report.txt', 'w').write('\n'.join(report) + '\n')
    print('\n== collect report'); print('\n'.join(report))


if __name__ == '__main__':
    main()
