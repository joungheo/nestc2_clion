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
  `SYS_BIN` = systolic 400 MHz (single, multi4), used by `run_prof.sh -s _sys*` (executables built with `-DVTALIB=<vtalib-zcu102-systolic>`).
  The 333 MHz `vtahw_*_y219.bin` stay usable with `BIN=<file>`.
- Measuring (2026-10-29, `vtabundles_xcmpl/README.md`): `hw*/script/run_prof.sh -s _prof|_timed|_sysprof|_systimed` (build dirs
  `build[_<set>]<suffix>`), `-n <runs>` (default 25), `-l` lockstep of the multi-EVTA copies, `-r` `chrt -f 10`; multi-EVTA with
  `OMP_NUM_THREADS=1`. `_prof` = EVTA time per conv (`prof_summary.py` adds GOPS), `_timed` = wall time per statement.
  예) `cd vtabundles_xcmpl && sudo /bin/sh -c "OMP_NUM_THREADS=1 hw_multievtax4/script/run_prof.sh -s _sysprof -n 25 -l -r MultiEVTAResnet18Test"`
- Resnet18 conv0 runs on EVTA (`VTA_RESNET18_WITH_SKIPQUANT0=OFF`, zero-filled to 16 channels, schedule 2 virtual threads 8x16
  in `include/Backends/VTA/VTASchedules.h`); `Resnet18PartitionTest` keeps conv0 float (`docs/evta_runtime_aot.md` §5.3).
  예) regenerate it in the simulator build: `cd Debug2_remote && cmake -DVTA_RESNET18_WITH_SKIPQUANT0=OFF . && ./build_sim_bundles.sh vtaMxnetResnet18Bundle`
  The PS register port (`M_AXI_HPM0`) runs on `pl_clk0` 100 MHz; at 333 MHz it missed timing.
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
