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

### EVTA on this board (verified 2026-09-29/30)

- **ISA of the bitstream:** 256-bit instructions, but `memory_type` and `alu_opcode` are **2 bits**. Both
  `vta/vtalib/include/zcu102/vta/hw_spec.h` and `.../simulator/hw_spec.h` are set to that (`//#### JO`
  markers); with 3 bits every LOAD/STORE address is off by one bit (results 0, deadlocks). ALU `MUL` (4) and
  `VTA_MEM_ID_MOVE` (5) cannot be encoded, so BNN/XP-GEMM paths do not run on this bitstream. If a different
  bitstream is used, re-check the widths first (probes in `../vtabundles_xcmpl/hwtest/rawtest*.cpp`).
- **Not cache-coherent.** vtalib option `NESTC_EVTA_COHERENT_ACCESSES` (default **OFF** →
  `-DVTA_COHERENT_ACCESSES=false`) makes the runtime flush/invalidate; ON hangs on this board. Generated
  bundles call `VTABufferFlush` / `VTABufferInvalidate` (new vtalib API) around the CPU-side transposes —
  emitted by `lib/Backends/VTA/VTASave.cpp`, so bundles generated before that change must be regenerated.
- **Reload the bitstream before every EVTA run** (`fpga_manager` firmware write, then
  `Xlnk().xlnk_reset()`): a hung run leaves the auto-restart EVTA blocks deadlocked and all later runs hang.
  The runtime pre-allocates 64 MB of the 128 MB CMA pool per command handle. Other users share this board.
- With `NESTC_EVTA_RUN_ON_ZCU102=ON`, `cmake/glow/CMakeLists.txt` skips `lib`/`tools`, so VTASaverTest /
  model-compiler are not built there: generate bundle sources in the simulator build (`Debug2_remote`) and
  build them for the board outside CMake, as `../vtabundles_xcmpl/Conv3Test/{build.sh,run.sh}` does
  (Conv3 bundle matches the simulator golden on the board).

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
