# EVTA bundles on the ZCU102

Location: `nest-compiler/vtabundles_xcmpl/` (the NEST-C root is the parent directory; no absolute paths).
Build directories are not kept in git (`.gitignore`): create them with the `cmake` lines below.
`build_timed/`: `cmake -DBUNDLE_TIMED=ON -DBUNDLE_EXE_SUFFIX=_timed ..` (per-statement timing, `hw/script/run_prof.sh -s _timed`).
#### JO added (2026-10-29): systolic bitstreams (`SYS_BIN` of run_all.sh, picked by `run_prof.sh -s _sys*`), vtalib `vtalib-zcu102-systolic`:
`build_sysprof/` (`-DNESTC_EVTA_PROFILE=ON -DBUNDLE_EXE_SUFFIX=_sysprof`), `build_systimed/` (`-DBUNDLE_TIMED=ON -DBUNDLE_EXE_SUFFIX=_systimed`),
`build_multievtax4_sys/` (`-DBUNDLE_EXE_SUFFIX=_sys`), `build_multievtax4_systimed/` (`-DBUNDLE_TIMED=ON -DBUNDLE_EXE_SUFFIX=_systimed`);
each with `-DVTALIB=<NEST-C root>/vtalib-zcu102-systolic`, the multi4 ones also `-DBUNDLE_SET=multievtax4 -DNESTC_EVTA_MULTI=ON -DNESTC_EVTA_MAX_XFER=4194304`.
Run e.g. `sudo hw/script/run_prof.sh -s _systimed Resnet18Test`, `sudo OMP_NUM_THREADS=1 hw_multievtax4/script/run_prof.sh -s _systimed MultiEVTAResnet18Test`.

Bundle sources are generated in the simulator build, then compiled for the board here.

| Step | Where | Command |
|---|---|---|
| 1. Generate bundle sources (simulator, `NESTC_USE_VTASIM=ON`) | `nest-compiler/Debug2_remote/` | `./build_sim_bundles.sh [target ...]` |
| 2. Copy them into `hw/<Dir>/` (no compile) | `vtabundles_xcmpl/` | `./collect_hw_bundles.py [Dir ...]` |
| 3a. Compile for the board | `vtabundles_xcmpl/build/` | `make -j2 hw_bundles` or `make -j2 <target>` |
| 3b. Compile with HW profiling | `vtabundles_xcmpl/build_prof/` | `make -j2 <target>` → `hw/<Dir>/<target>_prof` |
| 4a. Run all + compare with golden | `vtabundles_xcmpl/` | `sudo hw/script/run_all.sh [Dir ...]` → `hw/result/results.txt` |
| 4b. Run profiling bundles | `vtabundles_xcmpl/` | `sudo hw/script/run_prof.sh [-s _timed] [Dir ...]` (no Dir, `all` or `hw_bundles` = every bundle) → `hw/<Dir>/prof.log` (`timed.log`, `timing.csv`; `prof_<exe>.log` when a Dir has several executables); `_prof` logs also get `prof_summary.py`: runs, VTA ms per run, per-conv GOPS of the last run (`<log>.sum`), one line per Dir in `hw/result/prof_summary.txt` (`timed_summary.txt`) |

- Step 1 runs `make <target>` in the existing Debug2_remote build (no new cmake); outputs go to
  `Debug2_remote/vta/bundles/<Dir>/`, logs to `Debug2_remote/bundle_logs/`.
- Step 2 keeps what is already in `hw/<Dir>` (executables, `*_prof`, logs) and regenerates `hw/script/run_all.sh`.
- git keeps only `hw/script/` (`run_all.sh`, kept so the run lines are versioned, and `run_prof.sh`) of `hw/`; the
  bundle dirs (with their logs) stay in `hw/`, the summaries (`results.txt`, `*_summary.txt`, `collect_report.txt`)
  are in `hw/result/`, the `sds_trace_data.dat` that PYNQ `libcma.so` writes to the working directory at exit
  (bitstream reload `Xlnk().xlnk_reset()`; by hand: `sudo hw/script/load_bit.sh <bin>`) in `hw/trace/` (the executables leave one in their bundle dir); all ignored (each run overwrites them; the numbers worth keeping go into the docs). The scripts `cd`
  to `hw/` themselves (call them from anywhere). Same for `hw_<set>/script/` (2026-10-27).
- Step 3 uses `CMakeLists.txt` here: vtalib (`libvta_runtime.a`, `libVTABundle.a`) is built once per build
  directory with its own ZCU102 settings, and every bundle is compiled with the same `-D` macros.
  First configure: `mkdir build && cd build && cmake ..` (clang-8 is the default compiler);
  profiling: `mkdir build_prof && cd build_prof && cmake -DNESTC_EVTA_PROFILE=ON -DBUNDLE_EXE_SUFFIX=_prof ..`.
  With a `vta-hw-zcu102-single/multi2/multi4` `PROFILE=1` bitstream (compute counters + AXI perf monitor) add
  `-DNESTC_EVTA_PROFILE_AXI_MONITOR=ON` (one monitor per EVTA at 0xA0050000 + i * 0x10000).
  Every set builds with `vta/vtalib` (the default `VTALIB`); `BUNDLE_SET` alone picks the buffer and queue sizes.
  The only other vtalib is `vtalib-zcu102-systolic` (`-DVTALIB=`, for the systolic bitstreams). The copies
  `vtalib-zcu102-single/multi2/multi4` were removed on 2026-10-29 (identical to `vta/vtalib` since 2026-10-26).
- Step 4 reloads the bitstream before each bundle (a hung run leaves the EVTA deadlocked).
- `hwtest/` holds raw ISA probes and vtalib HW tests (`build.sh` has the vtalib ZCU102 `-D` set inline);
  it is not part of the flow above. Re-run its `rawtest*` probes first if a different bitstream is used.

## Multi-EVTA bundle sets (#### JO, 2026-10-17)

| Set | Bitstream (`/lib/firmware`) | Sources | hw dir | Build dir |
|---|---|---|---|---|
| `single` (default) | `vtahw_single_y219.bin` (vta-hw-zcu102-single, Y219 fence, 333 MHz; the etri `zcu102_1x16_i8w8a32_16_16_19_18.bin` gives the same results, EVTA time 1-21 % longer) | `vta/bundles` | `hw/` | `build*/` |
| `multievtax2` | `vtahw_multi2_y219.bin` (vta-hw-zcu102-multi2, Y219 fence, 333 MHz; was the etri `zcu102_2_1x16_i8w8a32_16_15_18_17.bin`) | `vta/multievtax2_bundles` | `hw_multievtax2/` | `build_multievtax2/` |
| `multievtax4` | `vtahw_multi4_y219.bin` (vta-hw-zcu102-multi4, Y219 build `rs_rst`, 333 MHz) | `vta/multievta_bundles` | `hw_multievtax4/` | `build_multievtax4/` |

- Generate (simulator build, configured once with `-DNESTC_EVTA_MULTI_BUNDLES=ON`):
  `Debug2_remote/build_sim_bundles.sh multievtax2ConvTestBundle multievtax2SingleResnet18Bundle multievtax2Resnet18Bundle`
- Copy: `./collect_hw_bundles.py --set multievtax2` (writes `hw_multievtax2/script/run_all.sh` with the 2-EVTA bitstream
  and copies `hw/script/run_prof.sh` there).
- Build: `mkdir build_multievtax2 && cd build_multievtax2 && cmake -DBUNDLE_SET=multievtax2 .. && make -j2 hw_bundles`
  (vtalib with `NESTC_EVTA_MULTI=ON` -> buffers 16/15/18/17, and `VTA_MAX_XFER` 8 MB so two command handles fit in
  the 128 MB CMA).
- Bitstream: `hwtest/bit2bin.py <x>.bit` then `sudo cp <x>.bin /lib/firmware/`.
- `multievtax4` (2026-10-26) uses the upstream 4-EVTA sources. They were generated without a cmake re-run of
  Debug2_remote (that rebuilds all of Glow): the ConvTest bundles 0-3 are the ones VTASaverTest wrote into
  `vta/multievtax2_bundles/MultiEVTAx2ConvTest`, Resnet18 copies 0/1 those of `MultiEVTAx2Resnet18Test`, and copies
  2/3 come from `Debug2_remote/glow/bin/model-compiler` with the flags of `MultiEVTAResnet18Test/CMakeLists.txt`
  (`-index-multi-evta=4/8 -network-name=mxnet_exported_resnet18_2/_3`), all copied into
  `Debug2_remote/vta/multievta_bundles/<Dir>`. Build: `cmake -DBUNDLE_SET=multievtax4 ..`
  (`VTA_MAX_XFER` 4 MB for four handles). `run_all.sh` compares only
  ConvTest output 0; outputs 1-3 are `multievtaConvTestOutput<i>.bin` vs `multievtaConvTestGolden<i>.bin`.
  Resnet18 is CPU-bound: `multivtaMxnetResnet18Bundle` (4 threads share 100 inferences) is fastest with
  `OMP_NUM_THREADS=1` (11.0 s vs 14.1 s with 4; single EVTA 13.5 s with 4).
- `MultiEVTAConv4Test` (both multi sets): `hw_multievtax4/MultiEVTAConv4TestSrc/make_multi4conv4.py` makes Conv4Test
  copies for EVTA 0-3 from `hw/Conv4Test` (renamed from the EVTA0 bundle; collect the single set first) plus a
  `_convonly` entry; `collect_hw_bundles.py --set multievtax4|multievtax2` runs it into `hw_<set>/MultiEVTAConv4Test`
  and adds the run line (`multi4conv4Bundle 15 100` / `3 100`: one thread per EVTA, the same bundle on every EVTA at
  once). `multi4conv4Bundle <mask> [iters] [conv]` prints per-EVTA and wall time and GOPS, writes the lowest EVTA's
  output to `output.bin` (golden check of run_all.sh) and exits 1 if any EVTA's output is wrong. Results in BOARD_TODO.
- `MultiEVTAResConv10Test` (both multi sets, 2026-10-26): the same generator and main for `hw/ResConv10Test`
  (3x3, 7x7x512 -> 512, the conv shape with the highest GOPS on one EVTA): `make_multi4conv4.py hw/<Name>Test` takes
  any single-conv bundle of `hw/` and writes `MultiEVTA<Name>Test` (names and op count in `multiconv.h`); run line
  `multi4resconv10Bundle 15 100` / `3 100`. Measure GOPS with `OMP_NUM_THREADS=1 ./<exe> <mask> 400 conv` (with the
  default OpenMP teams, the 4 threads oversubscribe the 4 cores and the slowest EVTA sets the wall time).
- git keeps `hw_<set>/script/` (`run_all.sh`, `run_prof.sh`) and `*Src/` dirs; the bundle dirs and run outputs are ignored.
- Results (2026-10-26, 333 MHz, `OMP_NUM_THREADS=1 ./<exe> <mask> 400 conv`, all = golden): ResConv10 152.6 GOPS on 1
  EVTA, **609.7 on 4** (multi4), 305.5 on 2 (multi2); Conv4 123.5 / 491.6-493.4 / 247.1. With `build_multievtax4_prof`
  the VTA_SYNC table shows EVTA time per conv 1.447 ms alone and 1.449 ms with 4 at once (no HP/DDR contention) and a
  66 us CPU gap between runs. Report with the ResConv10 structure and the command stream:
  `../docs/evta_multi4_609gops_report.md`.
- Per VTA run / per statement for the multi sets, as for `hw/`: build dirs `build_<set>_prof`
  (`-DBUNDLE_SET=<set> -DNESTC_EVTA_PROFILE=ON -DBUNDLE_EXE_SUFFIX=_prof`) and `build_<set>_timed`
  (`-DBUNDLE_SET=<set> -DBUNDLE_TIMED=ON -DBUNDLE_EXE_SUFFIX=_timed`);
  then `sudo hw_<set>/script/run_prof.sh [-s _timed] [Dir ...]`. Each timed .cpp (one per EVTA copy) prints its own table
  and writes `timing_<cpp>.csv` (a single timed .cpp still writes `timing.csv`).
  A Dir with several run lines (threaded `multi*` + single-thread `single*` executable) logs each executable to
  `<suffix>_<exe>.log` (2026-10-29; before, both wrote `<suffix>.log` and the second overwrote the first).
  The threaded executable runs one bundle copy per EVTA in its own pthread; the copies take inferences from one shared
  counter (100 in all, ~25 each), so each timed table says how many runs that copy did. Tables print at exit (atexit).
  `single*` runs copy 0 (EVTA 0) alone 100 times, the 1-EVTA reference.
- `OMP_NUM_THREADS`: the CPU ops of vtalib (`CPUBundle_generic.cpp`: transposes, elemadd, relu, maxpool, fc, quantize) are
  OpenMP loops; each pthread opens its own team. Resnet18 x 100 on multi4, systolic 400 (2026-10-29):
  threaded 4014 / 4955 / 5813 / 6025 ms and single 15276 / 10285 / 8638 / 7989 ms for 1 / 2 / 3 / 4 threads.
  So the threaded executable with `OMP_NUM_THREADS=1` (4 pthreads already fill the 4 cores), the single one with the default 4.
- Each bundle is pinned to one EVTA at code generation (`-index-multi-evta` mask 1/2/4/8 -> EVTA 0..3,
  `VTASave.cpp` ~line 4958); a multi-EVTA executable runs one bundle per thread.
- The conv tile schedules (`include/Backends/VTA/VTASchedules.h`) were tuned for the single-EVTA buffers;
  the 2-EVTA buffers are half as large, so check results of large layers.

## EVTA bitstreams (#### JO, 2026-10-17)

`evta_hw/` builds 1-4 EVTA cores into a ZCU102 bitstream with Vivado/Vivado HLS 2020.1 from
`tvm/3rdparty/vta-hw`'s HLS, compiled against vtalib's 256-bit `hw_spec.h` (see `evta_hw/README.md`).
`run_all.sh` / `run_prof.sh` / `hwtest/run_rawtests*.sh` take `BIN=<file in /lib/firmware>` to test one.

## To do

### Resnet50 conv1 on the CPU is 78.5 % of the run (measured 2026-10-16, `run_prof.sh -s _timed Resnet50Test`)

- Node `Conv_gpu_0_conv1_1__2` (7x7, 3 -> 64 channels, stride 2, pad 3, 224x224 -> 112x112, 236 Mops) is emitted
  as `nonvtaconvolution(...)` (`hw/Resnet50Test/resnet50.cpp` line 475) because C = 3 cannot go to the VTA.
- `nonvtaconvolution` (`nest-compiler/vta/vtalib/lib/Bundle/CPUBundle_generic.cpp`, ~line 296) calls
  `cpuvtaconvolution` only when C and KN are multiples of 16 with power-of-2 scales and zero offsets; otherwise
  `cpunonvtaconvolution` (~line 227), which is still the original single-threaded loop
  (one output channel at a time, kernel row re-read per pixel).
- Board time: **1164.5 ms of 1482.6 ms** (0.2 GOPS). The rest of Resnet50: VTA convs 152.9 ms, transposes 73.9 ms,
  elemadd 34.5 ms, relu 17.4 ms, VTABufferAlloc 14.3 ms, maxpool 7.6 ms, FC 5.0 ms.
  VGG16's first conv (`Conv_0__2`, 3.5 Mops) uses the same function: 23.0 ms.
- Fix: same approach as `convolutionFloat` (Resnet18 conv0, 1547 -> 76 ms): repack the kernel to
  [fx][fy][fd][KN] so the output channel is the inner loop, accumulate all KN int32 sums of a pixel at once,
  `#pragma omp parallel for` over output rows. Integer sums do not depend on the order, and the per-channel
  bias / rounding / clamp stay the same expressions, so the output must stay bit-identical: check off-board
  against the original function first, then `run_all.sh Resnet50Test` (top-1 281 / 0.702210 expected).
- Expected: Resnet50 ~1.48 s -> ~0.35 s.

### First conv layer (3 input channels) stays on the CPU (measured 2026-10-16)

Resnet18 conv0 (7x7, 3 -> 64, stride 2, 224x224 -> 112x112, 236 Mops) cannot use the VTA: `VTASave.cpp`
(~line 3098) sends a conv to `convolution_wo_tr` only if input and output channels are multiples of 16
(plus group 1, zero offsets, power-of-2 scales); `VTA.cpp` (~line 509) has the same rule for the graph rewrite.
So even inside a VTA partition it becomes a CPU call:
- float (node listed in `nest-compiler/vta/bundles/Resnet18Test/VTASkipQuantizeNodes.txt`) -> `convolutionFloat`,
  76 ms on the board (3.1 GOPS; Resnet18Test, and Resnet18PartitionTest p0 since NESTOptimalFusedPlan_p0VTA.yaml);
- int8 -> `nonvtaconvolution` -> `cpunonvtaconvolution`, ~1160 ms (same as Resnet50 conv1 above).

Options, not done yet:
1. Faster `convolutionFloat` (`vta/vtalib/lib/Bundle/CPUBundle_generic.cpp`): Cortex-A53 1.2 GHz x 4 has ~19 GFLOPS
   fp32 NEON peak, 3.1 GOPS is ~16 %. Register-blocked NEON over several outputs could reach ~25-30 ms.
   Float summation order changes, so check top-1/confidence instead of bit equality.
2. conv0 on the VTA in int8: zero-pad input channels 3 -> 16 (input and weights) in the VTA backend so the
   `convolution_wo_tr` conditions hold. 5.3x the ops but at >120 GOPS ~10 ms. Changes accuracy (conv0 quantized;
   that is why the skip list exists) and needs backend changes (`VTA.cpp` / `VTASave.cpp`).
3. Optimize `cpunonvtaconvolution` (the Resnet50 conv1 item above); the same kernel layout work as option 1,
   so 1 and 3 can be done together.
