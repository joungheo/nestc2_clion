# EVTA bundles on the ZCU102

Location: `nest-compiler/vtabundles_xcmpl/` (the NEST-C root is the parent directory; no absolute paths).
Build directories are not kept in git (`.gitignore`): create them with the `cmake` lines below.
`build_timed/`: `cmake -DBUNDLE_TIMED=ON -DBUNDLE_EXE_SUFFIX=_timed ..` (per-statement timing, `hw/run_prof.sh -s _timed`).

Bundle sources are generated in the simulator build, then compiled for the board here.

| Step | Where | Command |
|---|---|---|
| 1. Generate bundle sources (simulator, `NESTC_USE_VTASIM=ON`) | `nest-compiler/Debug2_remote/` | `./build_sim_bundles.sh [target ...]` |
| 2. Copy them into `hw/<Dir>/` (no compile) | `vtabundles_xcmpl/` | `./collect_hw_bundles.py [Dir ...]` |
| 3a. Compile for the board | `vtabundles_xcmpl/build/` | `make -j2 hw_bundles` or `make -j2 <target>` |
| 3b. Compile with HW profiling | `vtabundles_xcmpl/build_prof/` | `make -j2 <target>` → `hw/<Dir>/<target>_prof` |
| 4a. Run all + compare with golden | `vtabundles_xcmpl/hw/` | `sudo ./run_all.sh [Dir ...]` → `hw/results.txt` |
| 4b. Run profiling bundles | `vtabundles_xcmpl/hw/` | `sudo ./run_prof.sh [-s _timed] [Dir ...]` (no Dir, `all` or `hw_bundles` = every bundle) → `hw/<Dir>/prof.log` (`timed.log`, `timing.csv`), one line per Dir in `hw/prof_summary.txt` (`timed_summary.txt`) |

- Step 1 runs `make <target>` in the existing Debug2_remote build (no new cmake); outputs go to
  `Debug2_remote/vta/bundles/<Dir>/`, logs to `Debug2_remote/bundle_logs/`.
- Step 2 keeps what is already in `hw/<Dir>` (executables, `*_prof`, logs) and regenerates `hw/run_all.sh`.
- Step 3 uses `CMakeLists.txt` here: vtalib (`libvta_runtime.a`, `libVTABundle.a`) is built once per build
  directory with its own ZCU102 settings, and every bundle is compiled with the same `-D` macros.
  First configure: `mkdir build && cd build && cmake ..` (clang-8 is the default compiler);
  profiling: `mkdir build_prof && cd build_prof && cmake -DNESTC_EVTA_PROFILE=ON -DBUNDLE_EXE_SUFFIX=_prof ..`.
  With a `vta-hw-zcu102-single` `PROFILE=1` bitstream (compute counters + AXI perf monitor), use its vtalib copy:
  `cmake -DVTALIB=$PWD/../../vtalib-zcu102-single -DNESTC_EVTA_PROFILE=ON -DNESTC_EVTA_PROFILE_AXI_MONITOR=ON -DBUNDLE_EXE_SUFFIX=_prof ..`.
- Step 4 reloads the bitstream before each bundle (a hung run leaves the EVTA deadlocked).
- `hwtest/` holds raw ISA probes and vtalib HW tests (`build.sh` has the vtalib ZCU102 `-D` set inline);
  it is not part of the flow above. Re-run its `rawtest*` probes first if a different bitstream is used.

## Multi-EVTA bundle sets (#### JO, 2026-10-17)

| Set | Bitstream (`/lib/firmware`) | Sources | hw dir | Build dir |
|---|---|---|---|---|
| `single` (default) | `zcu102_1x16_i8w8a32_16_16_19_18.bin` | `vta/bundles` | `hw/` | `build*/` |
| `multievtax2` | `zcu102_2_1x16_i8w8a32_16_15_18_17.bin` | `vta/multievtax2_bundles` | `hw_multievtax2/` | `build_multievtax2/` |

- Generate (simulator build, configured once with `-DNESTC_EVTA_MULTI_BUNDLES=ON`):
  `Debug2_remote/build_sim_bundles.sh multievtax2ConvTestBundle multievtax2SingleResnet18Bundle multievtax2Resnet18Bundle`
- Copy: `./collect_hw_bundles.py --set multievtax2` (writes `hw_multievtax2/run_all.sh` with the 2-EVTA bitstream
  and copies `hw/run_prof.sh` there).
- Build: `mkdir build_multievtax2 && cd build_multievtax2 && cmake -DBUNDLE_SET=multievtax2 .. && make -j2 hw_bundles`
  (vtalib with `NESTC_EVTA_MULTI=ON` -> buffers 16/15/18/17, and `VTA_MAX_XFER` 8 MB so two command handles fit in
  the 128 MB CMA).
- Bitstream: `hwtest/bit2bin.py <x>.bit` then `sudo cp <x>.bin /lib/firmware/`.
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
