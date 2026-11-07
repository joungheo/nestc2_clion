# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

NEST-C is ETRI's fork/extension of [Glow](https://github.com/pytorch/glow), aimed at generating optimized
code for NPUs. Glow itself is a **git submodule** (`glow/`), as are TVM (`tvm/`), the EVTA runtime
(`vta/vtalib/`) and `onnx_yaml-parser/`. NEST-C does not fork Glow's history — it **overlays** its own
sources, headers and CMake files on top of the pristine submodule at configure time. Understanding that
overlay is the single most important thing about this repo.

## The overlay mechanism (read before editing anything)

The top-level [CMakeLists.txt](CMakeLists.txt) does three things before `add_subdirectory(glow)`:

1. **Copies `cmake/glow/` over `glow/`** (`copy_directory`). Every CMakeLists under `cmake/glow/` is a
   replacement for the Glow file at the same relative path.
   → **Never edit `glow/**/CMakeLists.txt` directly** — it is overwritten on the next cmake run. Edit
   `cmake/glow/...` instead. (`.gitmodules` marks the submodules `ignore = dirty` precisely because of this.)
2. **Deletes a handful of Glow headers** (`glow/tools/loader/Loader.h`, `glow/include/glow/Quantization/Base/Base.h`,
   `glow/include/glow/Base/Image.h`) so that NEST-C's versions under `include/` are picked up instead, and
   **copies** `include/glow/Optimizer/GraphOptimizer/FunctionPasses.def` into the submodule.
3. Puts `${NESTC_ROOT_DIR}/include` on the include path with `BEFORE`, so `include/glow/**` **shadows**
   the submodule's headers of the same name (`Graph.h`, `Nodes.h`, `Tensor.h`, `Type.h`, `Flags.h`,
   `Support.h`, the Partitioner and Runtime headers, …).

Consequently, NEST-C's `lib/` mirrors Glow's `lib/` only partially: the overlaid CMakeLists pull specific
NEST-C `.cpp` files into Glow's targets. E.g. [cmake/glow/lib/Graph/CMakeLists.txt](cmake/glow/lib/Graph/CMakeLists.txt)
builds the `Graph` library from Glow's `Hook.cpp`/`Node.cpp`/… plus `${NESTC_ROOT_DIR}/lib/Graph/Graph.cpp`
and `Nodes.cpp`. If you change a file under `lib/`, check the matching `cmake/glow/lib/.../CMakeLists.txt`
to see which target actually compiles it.

**Rule of thumb when a Glow file needs to change:** copy it into the NEST-C tree (`include/glow/...` or
`lib/...`), then wire it in via `cmake/glow/...`. Leave the submodule untouched.

## Backends

Backends live in `lib/Backends/` (sources) and `include/Backends/` (public headers), and are discovered by
the overlaid [FindBackends.cmake](cmake/glow/cmake/modules/FindBackends.cmake) /
[lib/Backends/CMakeLists.txt](cmake/glow/lib/Backends/CMakeLists.txt), which glob both Glow's and NEST-C's
backend directories. A directory `lib/Backends/Foo` is built unless `NESTC_WITH_FOO` is defined and OFF —
so a backend with **no** corresponding option (e.g. `Relay`) is always built.

| Backend | Purpose | Gate |
|---|---|---|
| `VTA` | EVTA NPU code generation (the flagship backend) | `NESTC_WITH_EVTA=ON` sets `NESTC_WITH_VTA` |
| `VTAInterpreter` | Reference/simulated EVTA execution, used for quantization profiling | implied by `NESTC_WITH_EVTA` |
| `Relay` | Lowers to TVM Relay (CPU/OpenCL/Mali/aarch64) | always on; needs `NESTC_WITH_TVM` (default ON) |

The partner backends (`Newton`, `Enlight`, `NMP`, `openedge`) are OFF by default and are not used in this
checkout — work here targets EVTA on ZCU102 only. Leave their sources in place (they cost nothing when OFF)
and do not enable them.

Each backend follows the Glow shape: `Foo.cpp` (`Backend` subclass: `isOpSupported`, `verify`, `compile`),
`FooFunction.cpp` (`CompiledFunction::execute`), `FooDeviceManager.cpp`, `FooSave.cpp` (bundle emission),
`FooNodes.cpp`, and `FooFactory.cpp` containing the `REGISTER_GLOW_BACKEND_FACTORY(FooFactory, Foo)` hook.
Backend-specific nodes/instructions go in `lib/Backends/Foo/ClassGen/Foo{SpecificNodes,SpecificInstrs}.h`;
[cmake/glow/tools/ClassGen/CMakeLists.txt](cmake/glow/tools/ClassGen/CMakeLists.txt) globs those into the
generated `NodeGenIncludes.h`/`InstrGenIncludes.h`, and `NodeGen`/`InstrGen` (built from
[tools/ClassGen/](tools/ClassGen/)) emit `AutoGenNodes.*` / `AutoGenInstr.*` into the build dir at build time.
`*SpecificNodesVerification.h` files are `configure_file`d into `${GLOW_BINARY_DIR}/glow/`.

## Compilation pipeline

ONNX/Caffe2 model → Glow importer → NEST-C graph optimizations → (optional) profile-guided quantization →
(optional) `NestPartitioner` splits the graph across backends → per-partition code generation → **bundle**
(`.cpp`/`.o` + `.weights.bin` + a generated `main.cpp` and `CMakeLists.txt`) built into a standalone executable.

Key tools, all built from `tools/loader/Loader.cpp` plus one driver
(see [cmake/glow/tools/loader/CMakeLists.txt](cmake/glow/tools/loader/CMakeLists.txt)):

- `image-classifier` — inference; also used with `-dump-profile=...` to produce quantization calibration YAML.
- `model-compiler` — emits a bundle for a single backend (`-backend=VTA -emit-bundle=<dir> -load-profile=...`).
- `model-partition-tuner` — the PartitionTuner: profiles per-node cost on each PU and emits partition plans
  and multi-backend bundles ([tools/loader/ModelPartitionTuner.cpp](tools/loader/ModelPartitionTuner.cpp)).
- `ccode-generator` — C/C++ code generator ([tools/loader/CCodeGenerator.cpp](tools/loader/CCodeGenerator.cpp)).
- `nestos-partitioner` — NestOS variant, links `HostManagerNestos`.

Partitioning logic lives in [lib/Partitioner/NestPartitioner.cpp](lib/Partitioner/NestPartitioner.cpp) and
is driven by a target-system description YAML (`tests/runtime_test/*DeviceConfigs.yaml`) listing each
processing unit, its backend name, id and supported/unsupported ops. See
[docs/nestc/NestPartitioner-kor.md](docs/nestc/NestPartitioner-kor.md) for the full `-exe-type` / `-profile-mode`
workflow (Korean).

## Build

Requires LLVM ≥ 7 (8 is what CI uses), protobuf, libpng, glog, gflags, and `fmt` built from source.
See [docs/nestc/install.md](docs/nestc/install.md). TVM needs environment variables set **before** cmake:

```bash
export TVM_HOME=<repo>/tvm
export PYTHONPATH=$TVM_HOME/python:$PYTHONPATH
export TVM_LIBRARY_PATH=<build>/tvm
```

### This checkout: building on the ZCU102 board itself

This tree lives **on a ZCU102** (aarch64, PYNQ 2.4 / Ubuntu 18.04, 4 cores, 3.9 GB RAM, **no swap**), synced
from CLion over remote.

- **Build in the existing `Debug2_remote/` with Make** (`Unix Makefiles`; it is the CLion remote profile's dir,
  Release despite the name). Do **not** create new build directories — a fresh configure+build runs out of
  memory. Use `make -j2 <target>` at most.
- Compiler: there is no `clang` symlink and `c++` points to g++ 7.3, so pass
  `-DCMAKE_C_COMPILER=clang-8 -DCMAKE_CXX_COMPILER=clang++-8` explicitly.
- LLVM is at **`/usr/lib/llvm-8`** — `docs/nestc/evta.md` and `.gitlab-ci.yml` say `/usr/lib/llvm-8.0`, which
  does not exist here. Use `-DLLVM_DIR=/usr/lib/llvm-8/lib/cmake/llvm`.
- Board run options: `-DNESTC_USE_VTASIM=OFF -DNESTC_EVTA_RUN_ON_ZCU102=ON` (vtalib then builds
  `lib/zcu102/pynq_driver.cc` against PYNQ's `/usr/lib/libcma.so`, plus `gomp`).
  `NESTC_EVTA_RUN_WITH_GENERIC_BUNDLE=OFF` uses the aarch64 ARM Compute Library path in `vta/vtalib/lib/acl`.
  Don't use `NESTC_USE_PRECOMPILED_EVTA_LIBRARY` here — its ZCU102 URL points at an `x86_sim` tarball.
- The ETRI S3 bucket `nestc-data-pub` **no longer exists** (`NoSuchBucket`), so every
  `NESTC_USE_PRECOMPILED_*` download fails; `aws` CLI is not installed either. Generate bundles locally.
- EVTA binaries mmap `/dev/mem` (`root:kmem`), so run them / `check_zcu102` with **sudo**, after loading
  the EVTA bitstream (`zcu102_1x16_i8w8a32_16_16_19_18.bit` from etri/nest-data).
- `make` re-runs cmake whenever a CMakeLists changes, and Glow puts `-DGLOW_BUILD_DATE=<today>` on every
  target — a cmake re-run on a new day recompiles all of Glow (~1 h at `-j2`).
  Even on the same day (or with `SOURCE_DATE_EPOCH` pinning the date), a re-run rewrites
  `glow/glow/{Node,Instr}GenIncludes.h` (`cmake/glow/tools/ClassGen/CMakeLists.txt`: `file(REMOVE)` + `file(APPEND)`),
  so NodeGen/InstrGen and everything including `AutoGenNodes.h` rebuild — most of Glow, ~1 h at `-j2`.

### EVTA on this board (verified 2026-09-29/30)

Overview of all EVTA work in date order with pointers to the detailed docs: [docs/evta_overview.md](docs/evta_overview.md);
how the whole stack works, for readers new to Glow / VTA (11 figures, entry: [docs/evta_stack_arch.md](docs/evta_stack_arch.md);
frontend / lowering / quantization [docs/evta_compiler_frontend.md](docs/evta_compiler_frontend.md), VTA backend
[docs/evta_compiler_backend.md](docs/evta_compiler_backend.md), runtime / driver [docs/evta_runtime_arch.md](docs/evta_runtime_arch.md));
commands, options and where to run them: [docs/evta_user_manual.md](docs/evta_user_manual.md).
Survey of other compilers, runtimes / drivers, NPUs and cheaper FPGAs, and the direction (a stack for several accelerators; kept up to date):
[docs/npu_stack_survey.md](docs/npu_stack_survey.md).

- **ISA of the bitstream:** 256-bit instructions, but `memory_type` and `alu_opcode` are **2 bits**. Both
  `vta/vtalib/include/zcu102/vta/hw_spec.h` and `.../simulator/hw_spec.h` are set to that (`//#### JO`
  markers); with 3 bits every LOAD/STORE address is off by one bit (results 0, deadlocks). ALU `MUL` (4) and
  `VTA_MEM_ID_MOVE` (5) cannot be encoded, so BNN/XP-GEMM paths do not run on this bitstream. If a different
  bitstream is used, re-check the widths first (probes in `vtabundles_xcmpl/hwtest/rawtest*.cpp`).
- **Not cache-coherent.** vtalib option `NESTC_EVTA_COHERENT_ACCESSES` (default **OFF** →
  `-DVTA_COHERENT_ACCESSES=false`) makes the runtime flush/invalidate; ON hangs on this board. Generated
  bundles call `VTABufferFlush` / `VTABufferInvalidate` (new vtalib API) around the CPU-side transposes —
  emitted by `lib/Backends/VTA/VTASave.cpp`, so bundles generated before that change must be regenerated.
- **Reload the bitstream before every EVTA run** (by hand: `sudo vtabundles_xcmpl/hw/script/load_bit.sh <bin>`; `fpga_manager` firmware write, then
  `Xlnk().xlnk_reset()`): a hung run leaves the auto-restart EVTA blocks deadlocked and all later runs hang.
  The runtime pre-allocates 64 MB of the 128 MB CMA pool per command handle. Other users share this board.
  The run scripts (`run_all.sh`, `run_prof.sh`, `load_bit.sh`, `hwtest/*.sh`) write the loaded bitstream's name to
  `/tmp/evta_bitstream`; vtalib (`pynq_driver.cc` `CheckLoadedBitstream`) exits before touching EVTA i's registers if that bitstream
  has fewer EVTAs (`<n>evta` / `multi<n>` in the name, else 1), if the load failed or nothing was recorded since boot (`EVTA_NO_BIT_CHECK=1`
  skips it). A multi-EVTA executable on a single-EVTA bitstream is an AXI SError: kernel Oops, CMA fragmented, bitstream loads fail
  with ENOMEM until a reboot (2026-10-31).
- With `NESTC_EVTA_RUN_ON_ZCU102=ON`, `cmake/glow/CMakeLists.txt` skips `lib`/`tools`, so VTASaverTest /
  model-compiler are not built there: generate bundle sources in the simulator build (`Debug2_remote`) and
  build them for the board with the standalone CMake project in `vtabundles_xcmpl/` (not part of the NEST-C
  CMake; its build dirs are git-ignored; see its `README.md`:
  `build_sim_bundles.sh` → `collect_hw_bundles.py` → `make` in `build/` → `hw/script/run_all.sh`; of `hw/` and `hw_<set>/` only `script/` is in git).

### EVTA bitstreams (host PC, Vivado 2020.1 in docker)

The bitstream is synthesized on the x86 host PC, not on the board. Vivado 2020.1 does not install on the
host's Ubuntu 20.04, so it lives in the docker container **`vivado2020-build`** (ubuntu:18.04, user
`ubuntu`, `/tools/Xilinx` and the nest-compiler tree mounted at the same paths). Vivado is installed at
**`/tools/Xilinx/v2020.1/`** (Y2K22 HLS patch applied); `/tools/Xilinx/Vivado/2018.3` and `2020.2` on the host
are other versions (2020.2 has no `vivado_hls`). `pynq24-build`/`pynq27-build` do not have 2020.1.

```bash
docker exec -it -u ubuntu vivado2020-build bash
source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh
cd <nest-compiler>/vta-hw-zcu102-single/hardware/xilinx
make ip && make bin                    # ~30 min; PROFILE=1 adds compute counters + AXI perf monitor
```

- Sources: [vta-hw-zcu102-single/](vta-hw-zcu102-single/) (vta-hw Xilinx flow with vtalib's 256-bit
  `hw_spec.h`, `zcu102` target in `config/`, `config/vta_config.json` = 16/16/19/18).
- Output: `build/hardware/xilinx/vivado/<CONF>[_prof]/export/vtahw_<CONF>[_prof].{bit,bin,hwh,xsa}` plus
  timing/utilization reports — check WNS >= 0 in `*.timing.rpt`. The `vtahw_` prefix keeps it apart from
  the etri bitstream of the same CONF in the board's `/lib/firmware`.
- Software: `vta/vtalib` for every set (`vtabundles_xcmpl -DBUNDLE_SET=single|multievtax2|multievtax4` sets
  buffers and queue size; `NESTC_EVTA_PROFILE_AXI_MONITOR=ON` for a `PROFILE=1` bitstream). The old copies
  `vtalib-zcu102-single/multi2/multi4` were removed 2026-10-29; `vtalib-zcu102-systolic` stays (systolic uop order).
- Multi-EVTA: [vta-hw-zcu102-multi2/](vta-hw-zcu102-multi2/) and [vta-hw-zcu102-multi4/](vta-hw-zcu102-multi4/)
  (`NUM_EVTA` 2 / 4, buffers 16/15/18/17 = vtalib `NESTC_EVTA_MULTI`): EVTA i registers at
  `0xA0000000 + i*0x10000`, data through `S_AXI_HP<i>_FPD`, perf monitor (`PROFILE=1`) at `0xA0050000 + i*0x10000`;
  Output `vtahw_<CONF>_<N>evta[_prof].bin`. On the board the
  HP1-3 port widths come from the boot image (AFIFM registers), not from the bitstream — check them if a
  multi-EVTA run returns zeros. `vtabundles_xcmpl/evta_hw/` is an older multi-EVTA flow (not built).
- Built 2026-10-20 at 333 MHz, all `_prof`: single WNS +0.054 ns, multi2 +0.004, multi4 +0.003 (BRAM 556/912).
- Default bitstreams of `vtabundles_xcmpl/hw*/script/run_all.sh` (2026-10-28, `docs/evta_systolic.md` §8 8)): 380 MHz HLS GEMM with the
  HP ports on a separate 333 MHz clock (single `..._16_16_19_18_380mhz`, multi2 `..._2evta_380mhz_hp333`, multi4 `..._4evta_380mhz`);
  `SYS_BIN` = systolic 400 MHz (single, multi4), used by `run_prof.sh -s _sys*` (executables built with `-DVTALIB=<vtalib-zcu102-systolic>`);
  single `SYS_BIN` is the requantize build `..._sa_400mhz_rq.bin` since 2026-11-04 (before: bias reset `..._sa_400mhz_bias.bin`, ALU imm II = 1 `..._alu1_epr.bin`), multi4 the requantize build `..._4evta_sa_400mhz_rq.bin` since 2026-11-04 (before: `..._4evta_sa_396mhz_rq.bin` at 395.8 MHz, bias reset `..._4evta_sa_400mhz_bias.bin`, ALU imm II = 1 `..._4evta_sa_400mhz_alu1.bin`, §7.15).
  The 333 MHz `vtahw_*_y219.bin` stay usable with `BIN=<file>`.
- Measuring (2026-10-29, `vtabundles_xcmpl/README.md`): `hw*/script/run_prof.sh -s _prof|_timed|_sysprof|_systimed` (build dirs
  `build[_<set>]<suffix>`), `-n <runs>` (default 25), `-l` lockstep of the multi-EVTA copies, `-r` `chrt -f 10`, `-t <threads>`
  `OMP_NUM_THREADS` (default **1 for every set**, single EVTA too, since 2026-10-31; `-t 4` for 4 cores; `run_all.sh` the same).
  `_prof` = EVTA time per conv (`prof_summary.py` adds GOPS), `_timed` = wall time per statement.
  예) `cd vtabundles_xcmpl && sudo /bin/sh -c "hw_multievtax4/script/run_prof.sh -s _sysprof -n 25 -l -r MultiEVTAResnet18Test"`
- Resnet18 conv0 runs on EVTA (`VTA_RESNET18_WITH_SKIPQUANT0=OFF`, zero-filled to 16 channels, schedule 2 virtual threads 8x16
  in `include/Backends/VTA/VTASchedules.h`); `Resnet18PartitionTest` keeps conv0 float (`docs/evta_runtime_aot.md` §5.3).
  예) regenerate it in the simulator build: `cd Debug2_remote && cmake -DVTA_RESNET18_WITH_SKIPQUANT0=OFF . && ./build_sim_bundles.sh vtaMxnetResnet18Bundle`
  The PS register port (`M_AXI_HPM0`) runs on `pl_clk0` 100 MHz; at 333 MHz it missed timing.
- Activations between EVTA convs stay in the EVTA layout N{C/16}HW16 (2026-10-31, `VTASave.cpp` `findPackedActivations`,
  model-compiler `-vta-keep-layout`, default on): no transpose between layers, elemadd/relu as is, pools with the
  16-channel blocks as the batch; Resnet18 single 73.6 -> 56.4 ms, multi4 100 inferences 3.9 -> 2.8 s (`docs/evta_runtime_compare.md` §7.1).
- EVTA conv + elementadd + relu in one call (2026-10-31, `VTASave.cpp` `findResidualFusions`, model-compiler `-vta-fuse-residual`,
  default on; vtalib `convolution_wo_tr_res`, also in `vtalib-zcu102-systolic`): residual LOADed into inp, identity-weight GEMM, ALU
  ADD/SHR/MAX/MIN before the store, power-of-two scales only; bit-exact with the CPU path (`EVTA_RES_CHECK=1` compares at run time).
  Resnet18 (all 8 fused) single OMP 1 90.9 -> 67.1 ms, OMP 4 56.6 -> 50.2 ms, multi4 100 inferences 2.47 -> 1.85 s (§7.2).
- Max pool on the EVTA (2026-10-31, `-vta-evta-maxpool`, default on, vtalib `maxpool_evta`, `findEvtaMaxPools`): input >= 0 only
  (written by an EVTA conv with relu or a relu; LOAD pads with 0), output tiles with the input halo re-read, identity GEMM, one ALU MAX
  instruction per window position. Resnet18 single OMP 1 67.1 -> 52.5 ms, multi4 100 inferences 1.85 -> 1.47 s (§7.3).
- NHWC <-> EVTA layout transposes as 16-byte copies (2026-11-01, vtalib `CPUBundle_generic.cpp`, the file the board builds with
  `NESTC_EVTA_RUN_WITH_GENERIC_BUNDLE=ON`; conv0 `_zerofill` 6.8 -> 0.2 ms): Resnet18 single OMP 1 52.5 -> 46.9 ms, multi4 100 inferences
  1.47 -> 1.31 s; Conv4Test 4 EVTAs end-to-end 757 -> 316 ms (§7.4).
- VTA buffer pool (2026-11-01, vtalib `lib/zcu102/api.cc`): `VTABufferFree` keeps the buffer in a free list by size, `VTABufferAlloc`
  of the same size reuses it (no `cma_alloc` ioctl after the first run; up to `EVTA_BUF_POOL_MB`, default 32, `0` = off): Resnet18
  single OMP 1 46.9 -> 44.3 ms, multi4 100 inferences 1.29 -> 1.19 s (§7.5). Reused buffers keep their old contents.
- CPU fc (2026-11-01, vtalib `cpuFullyConnected`): kernel rows read in order, NEON sums per 8 rows x 64 outputs, prefetch, bit-identical:
  Resnet18 dense OMP 1 4.4 -> 0.74 ms, single OMP 1 44.3 -> 40.6 ms, multi4 100 inferences 1.19 -> ~1.1 s (§7.6). multi4 runs
  hung now and then (about once per 400 inferences per copy, only with 4 EVTAs at once): libcma's `cma_flush_cache` is a `dc civac`
  loop with no `dsb`, so the MMIO start could overtake the flush of the instructions; vtalib `VTAFlushCache` / `VTAInvalidateCache` now
  add `dsb sy` (2026-11-02, `EVTA_NO_DSB=1` leaves it out; 4000 per copy hang-free with no wait). The 5 us busy wait before each run used
  during the hunt (`EVTA_PRE_RUN_US`) is off by default again (§7.7). A timed-out run (`EVTA_RUN_TIMEOUT_MS`, default 5 s) dumps every EVTA's state and exits 5; the run scripts `xlnk_reset` after a
  failed bitstream load (the dead run's CMA stays allocated until then).
- fc on the EVTA (2026-11-02, `VTASave.cpp` `-vta-evta-fc`, default on; vtalib `fullyconnected_evta_packed`): weights and bias written in the
  EVTA layout at compile time, GEMM over chunks of half the wgt buffer, rounding as `std::round` with ALU MIN/MAX/ADD/SHR (ALU shift is
  -16..15), bit-exact; batch 1, offsets 0, power-of-two scale ratio, >= 8 output blocks per chunk, else the CPU fc. Resnet18 fc 0.74 -> 0.15 ms,
  single OMP 1 40.6 -> 39.9 ms, multi4 per copy 41.2 -> 40.6 ms (§7.8).
- Input preprocessing in the conv0 input copy (2026-11-02, `VTASave.cpp` `-vta-fuse-preprocess`, default on, `findInputPreprocess`; vtalib
  `preprocess_nhwc2vtaio_zerofill`): the float quantize, per-channel constant elemsub / elemdiv (a [C][256] table made at compile time) and
  an NCHW -> NHWC transpose before the zero-filled first conv, NEON 16 pixels at a time, bit-exact: 2.4 -> 0.37 ms, Resnet18 single OMP 1
  39.9 -> 37.1 ms, multi4 per copy 40.6 -> 37.6 ms (§7.9).
- Record / replay of EVTA calls (AOT stage 0, 2026-11-02, vtalib `cmd_queue.h` `ReplayBegin`, `EVTA_REPLAY=0` turns it off): the conv / maxpool / fc
  entries record their instructions and uops on the first call with the same scalar arguments (with a relocation table for the buffers) and
  later calls only run the copies; bit-exact, Resnet18 single OMP 1 37.1 -> 34.5 ms, multi4 per copy 37.6 -> 35.0 ms (`docs/evta_runtime_aot.md` §5.4).
- DRAM traffic between layers is cheap (2026-11-02, `docs/evta_runtime_compare.md` §7.11): `EVTA_REPLAY_LOG=2` prints per replayed run the
  GEMM / ALU iterations, DRAM bytes and time; `EVTA_MEMEXP=<mask>` shrinks the replayed inp / wgt / acc LOADs / STOREs (bits 0-3, results
  garbage). Resnet18 EVTA 32.18 ms, GEMM + ALU floor 25.9 ms, all activation traffic gone 29.82 ms (conv -> conv at most 1.05 ms of it).
- conv0 over a packed input (2026-11-02, `VTASave.cpp` `-vta-conv0-pack`, default on, `getConv0Pack`; vtalib `preprocess_conv0pack` /
  `conv0pack_nhwc2vtaio`): KH x KW / stride s over 3 channels as ceil(KH/s) x 1 / stride 1 over s * KW * 3 channels (s rows and the KW taps
  in the channels; Resnet18 conv0 4x1 over 115x112x48, schedule 2 / 16x16) instead of 7x7 over 16 zero-filled channels; the zero-filled
  path stays for shapes that do not fit. Every EVTA conv output byte-identical (`EVTA_DUMP_CONV=<dir>` dumps them); conv0 9.13 -> 3.64 ms;
  the input copy packs two padded rows at a time per thread. Resnet18 single OMP 1 34.5 -> 29.2 ms, multi4 per copy 35.0 -> 29.8 ms (§7.12).
- conv0 + maxpool in one EVTA call (2026-11-02, `VTASave.cpp` `-vta-fuse-pool`, default on, `findPoolFusions`; vtalib `convolution_wo_tr_pool`,
  `ConvPool`): conv tiles with the pool halo row / column recomputed, ALU MAX in acc in two passes, only the pooled area stored; byte-identical
  (`EVTA_DUMP_CONV` also dumps maxpool outputs; `EVTA_RES_CHECK=1`). conv0 + pool 4.83 -> 4.48 ms, single OMP 1 29.2 -> 28.8 ms (§7.13).
- Where the EVTA time goes (2026-11-02, §7.14; `EVTA_REPLAY_DUMP=<dir>` writes every recorded run's instructions and uops as text):
  Resnet18 26.47 ms = GEMM 18.85 + ALU 4.63 (the HLS `alu` loop is `II = 2`; bias ADD, SHR, clip MIN/MAX, relu, pool) + 2.99
  (conv0 tiles 1.08, about 60 us per run, load-bound 1x1 downsample convs).
- ALU ops with an immediate at II = 1 (2026-11-03, §7.15; `vta-hw-zcu102-systolic` `vta.cc` `alu()`, systolic builds only): one acc read
  and one write per step; bitstream `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_400mhz_alu1_epr.bin` (`Performance_ExplorePostRoutePhysOpt`,
  WNS +0.001 ns; ExtraTimingOpt missed by 0.126 ns), same executables and vtalib. Golden 30 PASS, layer dumps byte-identical;
  Resnet18 EVTA 26.42 -> 24.89 ms, single OMP 1 28.98 -> 27.32 ms. Use it with `BIN=<that file>` on `run_prof.sh -s _sys*`.
- Bias reset (2026-11-04, §7.16; systolic `sa_rmw.v` / `sa_ctrl.v`, ISA `VTAGemInsn.bias_reset`, vtalib `conv_kernel`): the conv bias is
  LOADed into acc before the reset GEMM and the reset writes it to the tile instead of 0 (no ALU ADD); only on a bitstream with `bias` in
  the name (`/tmp/evta_bitstream`; `EVTA_BIAS_RESET=0|1` overrides). Single `..._sa_400mhz_bias.bin` (WNS +0.000 ns): golden 30 PASS, dumps
  byte-identical, Resnet18 EVTA 24.89 -> 24.15 ms, single OMP 1 27.30 -> 26.59 ms. multi4 `..._4evta_sa_400mhz_bias.bin` (WNS +0.037 ns):
  probes 40/40, golden 3 PASS, dumps byte-identical, EVTA per copy 25.23-25.37 -> 24.49-24.62 ms.
- Requantize on the write to out (2026-11-04, §7.18; **fields, bitstream names, switches and how to run other models: [docs/evta_isa_ext.md](docs/evta_isa_ext.md)**):
  GEMM / ALU `rq_*` (en, round, shift, lo, hi; `rq_en = 0` = the low 8 bits as before) in `sa_rmw.v` S4-S6 and `vta.cc` `requant()`;
  vtalib `VTARequant`, used instead of the ALU SHR / MAX / MIN on a bitstream with `_rq` in the name (`EVTA_REQUANT=0|1` overrides; `_rq` also
  turns the bias reset on). Single `..._sa_400mhz_rq.bin` (WNS +0.039 ns): golden PASS, dumps byte-identical, Resnet18 EVTA 24.15 -> 23.05 ms,
  single OMP 1 26.61 -> 25.49 ms. multi4 `..._4evta_sa_400mhz_rq.bin` (placer `ExtraNetDelay_high`, post-route phys_opt, the one net
  left in row Y219 rerouted with `Y219RWRoute.java -Dtd=0`; WNS +0.002 ns; first a 395.8 MHz `..._396mhz_rq.bin`, `mmcm_set.tcl ... 11.875`):
  probes 40/40, golden 3 PASS, dumps byte-identical, EVTA per copy 24.49-24.62 -> 23.44-23.56 ms, busy union 583.8 -> 610.4 GOPS.
  vtalib instructions are zeroed now (`NextInsn`; the pad bits were uninitialized).
- Other models (2026-11-05, `docs/evta_runtime_compare.md` §7.19): Resnet50 res2_0 residual fused (relu to a 2^q smaller scale), schedules of the
  Resnet50 / VGG16 shapes in `VTASchedules.h` (found with vtalib-systolic `EVTA_SCHED`), conv tiles below 7 x 7: the dummy-padded store
  used the wrong channel block with nVirtualThread 2 (fixed in both vtalibs) and the systolic vtalib's minimum tile is 1 on an `_sa_` bitstream
  (`EVTA_MIN_TILE`; the 7 x 7 minimum is for the HLS GEMM's acc hazard, `sa_rmw.v` forwards); EVTA fc for any chunk size (Resnet50
  2048 -> 1000; vtalib computes it on the CPU from the packed weights where the GEMM cannot, `fc_packed_cpu`). Single rq: Resnet50
  147.9 -> 83.4 ms, VGG16 15.5 -> 7.3 ms, Resnet18 25.4 ms; dumps and classifications unchanged. vtalib `conv_kernel` now leaves room
  for the bias entries in `nFilterinLoop` (thread 0's bias LOAD overwrote thread 1's first acc rows without the bias reset: VGG16 on the
  HLS bitstream and the simulator were wrong); simulator, HLS and systolic dumps identical.
- conv0 input copy overlapped with the conv (2026-11-06, §7.19 ⑥): VTASave emits `preprocess_conv0pack_stream`; the systolic vtalib
  (`InputStream`) runs the conv that reads it one EVTA run per tile row and its replay packs and flushes the next row's input while a run
  is running (pynq driver `RunStart` / `RunWait`, `cmd_queue.h` segment tag / hook; `EVTA_INPUT_STREAM=0` = all at once; vta/vtalib
  writes it at once). Dumps identical; Resnet50 83.4 -> 82.2 ms, Resnet18 25.4 -> 24.1 ms (single rq, OMP 1).
- multi4 Resnet50 / VGG16 (2026-11-06, §7.19 ⑦): `vta/multievta_bundles/MultiEVTAResnet50Test`, `MultiEVTAVgg16Cifar10Test` (common
  `multi_threads_main.cpp`, model in `multi_model.h`), copies generated with `vtabundles_xcmpl/gen_multi4_bundles.sh` (also Resnet18;
  these dirs are not in the NEST-C CMake on purpose: a cmake re-run in Debug2_remote rebuilds Glow). load_module's constant VTA buffers
  are shared by the copies (`VTAConstBufferAlloc`; Resnet50 x 4 does not fit the 128 MB CMA otherwise; `EVTA_CONST_SHARE=0`).
  vtalib-systolic `conv_kernel` leaves bias room when the last thread's bias would lie past the physical acc (multi buffers 2048 entries).
  copy 0 dumps = single EVTA; per copy with 4 at once, stream off -> on: Resnet18 25.9 -> 24.6 ms, Resnet50 87.1 -> 86.0 ms, VGG16 8.3 ms.
- MobileNetV2 (2026-11-07, `docs/evta_runtime_compare.md` §7.20; `vta/bundles/MobileNetV2Test`, TFLite QAT model as float ONNX,
  `vtabundles_xcmpl/gen_mobilenetv2_bundle.sh`): depthwise filters need per-channel scales (`-enable-channelwise-depthwise` in NEST-C
  `lib/Quantization/Quantization.cpp`; per tensor cat_285.png is 293 instead of 282), run as vtalib `cw_convolution` (NEON CPU, bit-exact
  with the Interpreter's rounding); `VTA.cpp` `-vta-pad-channels` (default on) widens 24-channel conv / add activations to 32 with zero
  weights so their convs run on the EVTA; `VTASave.cpp` `-vta-round-shift` (default **off**, on for MobileNetV2Test) rounds the EVTA conv
  shift (2^(shift-1) in the bias; truncating cost 0.906 -> 0.840 confidence); conv0 pack takes TF SAME asymmetric pads (vtalib `padEnd`).
  Single rq OMP 1 1048 -> 84.2 ms (`-t 4` 44.4 ms). 2026-11-08: depthwise reads / writes the EVTA layout (`findPackedActivations`,
  `cw_convolution` inPacked / outPacked; no transposes), a 1x1 conv over a 1x1 input as the EVTA fc (`saveEvtaConvAsFc`, logits),
  MobileNetV2 schedules in `VTASchedules.h` (vt 2 does not run with an odd number of output channel blocks): 43.2 ms (`-t 4` 22.5 ms);
  left: depthwise 22.8 ms on the CPU, EVTA 13.4 ms, elemadd 5.3 ms. The NEST-C `lib/Quantization/Base/Base.cpp`
  (int8 exponent floor -7) is only built with `NESTC_WITH_NMP`; this board uses Glow's.
- The lab board's PL has a defective configuration row (tile row SLICE Y219, cells stuck at 0; `log/BOARD_TODO.md`
  2026-10-23/24). `vivado.tcl` on zcu102 fences the design below it by default (`VTA_BRAM_FENCE=y219|upper|none`).
  multi2 fits below it and works (2026-10-24); multi4 needs more BRAM than the rows below, so it crosses the row with
  no PIP inside it (PIPs in the row fail even without a stuck cell; the wires passing over it work): RapidWright flow
  in `vta-hw-zcu102-multi4/hardware/xilinx/scripts/y219route/README.md`, works at 333 MHz with register slices before the HP ports and one reset block per EVTA (2026-10-26).
- Speed (2026-10-26, `docs/evta_multi4_609gops_report.md`): ResConv10 (3x3, 7x7x512 -> 512) repeated on each EVTA,
  152.6 GOPS on one, **609.7 GOPS on four at once** (89 % of 4 x 170.7; EVTA time unchanged, no DDR contention).
  Measure multi-EVTA runs with `OMP_NUM_THREADS=1` (the default OpenMP teams oversubscribe the 4 cores).
- Clock and systolic work (2026-10-27, `docs/evta_systolic.md`, design `docs/evta_systolic_design.md`): `FREQ=<MHz>` on the
  `make` of the vta-hw copies overrides the PLL (default unchanged). multi4 at 380 MHz with the HP ports on their own 333 MHz
  clock passes the board: ResConv10 x 4 EVTAs **685.1 GOPS** (+12.4 %). [vta-hw-zcu102-systolic/](vta-hw-zcu102-systolic/) and
  [vtalib-zcu102-systolic/](vtalib-zcu102-systolic/) are single-EVTA copies for the systolic (weight-stationary) GEMM
  (RTL `gemm_sa`, `SYSTOLIC=1`); work on it is done on branch `systolic` and merged into main.

Useful options (full list at the top of [CMakeLists.txt](CMakeLists.txt) and per-backend CMakeLists):

- `NESTC_USE_VTASIM=ON` (default) builds against the x86 EVTA simulator; `OFF` targets a real board.
- `NESTC_EVTA_RUN_ON_ZCU102=ON` + `NESTC_USE_VTASIM=OFF` for ZCU102 hardware runs (see [docs/nestc/evta.md](docs/nestc/evta.md)).
- `NESTC_USE_PRECOMPILED_EVTA_LIBRARY=ON` fetches prebuilt `libVTABundle.a`/`libvta_runtime.a` from S3
  instead of building `vta/vtalib`; `NESTC_USE_PRECOMPILED_BUNDLE[_FROM_AWS]=ON` downloads generated
  bundle sources/calibration YAMLs instead of regenerating them (much faster, but hides codegen changes —
  turn it **off** when you are changing the VTA backend).
- `NESTC_EVTA_MULTI=ON` enables the multi-EVTA path (`vta/multievta_bundles`, `check_multievta`).
- `GLOW_VTA_FUSION` / `GLOW_VTA_INTERPRETER_FUSION` / `NESTC_EVTA_GRAPH_OPT` toggle VTA fusion and the
  VTAConv graph optimization; `NESTC_BNN=ON` switches to the binary-neural-net bundles only.

## Tests

Tests are registered through `add_nestc_test()` in [cmake/modules/NESTCTestSupport.cmake](cmake/modules/NESTCTestSupport.cmake),
which attaches a CTest **label** (`NESTC`, `ZCU102`, `MULTIEVTA`, …) and accumulates the
build dependencies into a global property. The `check_*` targets are just `ctest -L <LABEL>` plus those deps:

```bash
make -j2 check_nestc       # ctest -L NESTC   — the default suite
sudo make check_zcu102     # board-only tests (needs /dev/mem)
make -j2 check_multievta   # multi-EVTA
```

Running a single test — build its target, then filter by name (gtest binaries land in `<build>/tests/`):

```bash
cmake --build . --target VTASaverTest && ctest -R VTASaverTest --output-on-failure
./tests/VTASaverTest --gtest_filter='*Conv*'          # single gtest case
ctest -L NESTC -N                                      # list what would run
```

Bundle targets are named per model, e.g. `cmake --build . --target vtaMxnetResnet18Bundle`, and the resulting executable takes
an image path: `./vta/bundles/Resnet18Test/vtaMxnetResnet18Bundle glow/tests/images/imagenet/cat_285.png`.
`nestc_tests/*.txt` list the targets CI builds; [.gitlab-ci.yml](.gitlab-ci.yml) is the authoritative
reference for which option combinations are expected to build.

Note that most bundle tests **generate** their inputs at build time: a `image-classifier -dump-profile` run
produces the calibration YAML, then `model-compiler`/`model-partition-tuner` emits the bundle sources. A
change to the importer, optimizer or a backend therefore shows up as a bundle build failure, not just a
unit-test failure.

## Conventions

- C++14, no compiler extensions, position-independent code. There is no `.clang-format` at the NEST-C root;
  follow the one in the submodule (`glow/.clang-format`, LLVM style) for code under `lib/`, `include/`, `tools/`.
- Documentation and design docs under `docs/nestc/` are partly in Korean; the code and comments are English.
- Commits are DCO-signed in this project's upstream CI (`check-dco`, currently commented out in
  [.gitlab-ci.yml](.gitlab-ci.yml)) — use `git commit -s`.
