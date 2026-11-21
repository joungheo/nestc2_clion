# EVTA on ZCU102 사용 설명서 (명령, 스크립트, 옵션)

이 문서는 명령과 스크립트를 **한곳에** 모은 참고서다. 범위는 환경 만들기, bitstream, bundle 생성, 보드 빌드, 실행과 측정, 정확도, 시험이다.
각 명령마다 어느 기계의 어느 dir에서 실행하는지와 옵션의 뜻을 적는다. 장은 하는 일의 종류별로 나눴다.

- 처음부터 그대로 따라 하는 순서: [evta_walkthrough.md](evta_walkthrough.md). 컴파일러 cmake / make, VTASaverTest, profile, model-compiler, 보드 빌드, 실행, 로그 보기 순서다.
- 지금까지 한 일의 요약: [evta_overview.md](evta_overview.md).
- 스크립트를 새로 만들거나 옵션을 더하면 **이 문서의 해당 장에 한 줄을 더한다.**

| 장 | 내용 | 주로 어디서 |
|---|---|---|
| 0 | 기계와 dir | |
| 1 | 환경 만들기 (docker, sysroot) | 호스트 PC |
| 2 | bitstream 만들기와 HLS 시험 | PC docker `vivado2020-build` |
| 3 | bundle 소스 만들기, VTA backend 옵션, `VTA_FUSION_LOG` | PC docker `nestc-build` 또는 보드 |
| 4 | 보드용 실행 파일 빌드 | 보드, 또는 PC cross build |
| 5 | 실행과 측정 | 보드 (sudo) |
| 6 | 정확도 (ImageNetV2) | 보드 + PC |
| 7 | runtime 환경 변수 | 실행할 때 |
| 8 | vtalib 시험 | PC 또는 보드 |
| 9 | 하드웨어 시험 (`hwtest/`) | 보드 |
| 10 | 그 밖의 도구 | |
| 11 | 자주 하는 일의 순서 | |

## 0. 기계와 dir

| 기계 | 위치 | 하는 일 |
|---|---|---|
| 호스트 PC `ubuntu2` (Ryzen 9 5950X, user `jerry`) | `/home/jerry/npuprg/nestc2_clion/nest-compiler` (git `main`) | 코드 수정, commit / push, bundle 생성, cross build, bitstream 합성 |
| 첫 호스트 PC (user `ubuntu`) | `/home/ubuntu/npuprg/nestc2_clion/nest-compiler` | 위와 같은 일. 과열로 꺼진 적이 있어 전체 코어를 쓰는 일은 하지 않는다([host_pc_shutdown.md](host_pc_shutdown.md)) |
| PC docker `vivado2020-build` | `$HOME`와 `/tools/Xilinx`가 같은 경로로 mount, user `ubuntu` | Vivado / Vivado HLS 2020.1 (`/tools/Xilinx/v2020.1/`) |
| PC docker `nestc-build` | `$HOME`가 같은 경로로 mount, user `ubuntu` | NEST-C 시뮬레이터 빌드 `Debug2_remote`(model-compiler 등), 보드용 cross build. 보드와 같은 bundle을 바이트까지 같게 만든다 |
| ZCU102 보드 (`xilinx@192.168.20.2`) | `~/JO/npuprg/nestc2_clion/nest-compiler` | 실행과 측정. bundle 생성과 보드 빌드도 되지만 느리다 |

- **PC와 보드의 소스는 git으로만 맞춘다.**
  - PC에서 `git push origin main`, 보드에서 `git pull`한다.
  - `origin`은 GitHub 원격 저장소, `main`은 브랜치 이름이다.
  - submodule `vta/vtalib`는 그 안에서 따로 `git pull origin main`한다.
  - scp로 소스를 덮지 않는다.
- **scp로 보내는 것:** bitstream 파일(§2.3), PC에서 cross build한 실행 파일(§4.2).
- **`Debug2_remote/`와 `vtabundles_xcmpl/build*/`는 git 밖이다**(빌드 결과). 거기에 둔 스크립트는 pull로 오지 않는다.

```bash
# 보드에서 (최신으로)
cd ~/JO/npuprg/nestc2_clion/nest-compiler && git pull && (cd vta/vtalib && git pull origin main)
```

| 소스 dir | 내용 |
|---|---|
| `lib/Backends/VTA/` | backend. `VTASave.cpp`가 bundle 코드를 만든다(옵션은 §3.3) |
| `include/Backends/VTA/VTASchedules.h` | conv tile schedule |
| `vta/vtalib/` (submodule) | EVTA runtime과 bundle 함수(HLS GEMM bitstream용) |
| `vtalib-zcu102-systolic/` | 같은 runtime의 systolic bitstream용 사본. ALU 확장, epilogue 등 새 기능은 여기에만 있다 |
| `vta-hw-zcu102-single/`, `-multi2/`, `-multi4/`, `-systolic/` | bitstream 소스(HLS, RTL, Vivado tcl) |
| `vtabundles_xcmpl/` | 보드 실행 파일을 빌드하는 독립 CMake 프로젝트, bundle 생성 / 실행 / 측정 스크립트 |
| `utils/docker_vivado2020/`, `utils/docker_nestc/` | PC docker 환경(§1) |

## 1. 환경 만들기 (호스트 PC)

자세한 배경은 [host_pc_build.md](host_pc_build.md)에 있다.

| 일 | 명령 | 비고 |
|---|---|---|
| Vivado 컨테이너 만들기 | `utils/docker_vivado2020/setup.sh` | ubuntu:18.04 이미지와 `vivado2020-build` 컨테이너. `--init`이 없으면 합성이 `synth_design` 뒤에 멈춘다 |
| Vivado 설치 | 컨테이너 안에서 `utils/docker_vivado2020/install_vivado.sh <Xilinx_Unified_2020.1_0602_1208.tar.gz> [y2k22_patch-1.2.zip]` | MD5 확인, Zynq UltraScale+만 설치, Y2K22 HLS 패치. license `~/.Xilinx/xilinx.lic` |
| NEST-C 컨테이너 만들기 | `utils/docker_nestc/setup.sh` | `nestc-build`. 보드와 같은 clang-8, cmake 3.10, protobuf 3.0 |
| 시뮬레이터 빌드 configure + make | `docker exec -u ubuntu nestc-build <tree>/utils/docker_nestc/configure.sh [targets]` | `Debug2_remote`를 보드와 같은 옵션으로. 기본 target은 model-compiler, image-classifier(-j16, 약 10분). 모델 파일은 보드의 `Debug2_remote`에서 복사 |
| 보드 sysroot 복사 | `utils/docker_nestc/sync_sysroot.sh [xilinx@192.168.20.2] [~/zcu102_sysroot]` | cross build용(약 2.1 GB). 보드에 라이브러리를 새로 깔면 다시 돌린다 |

```bash
docker start vivado2020-build nestc-build        # PC 재부팅 뒤에는 꺼져 있을 수 있다
docker exec -it -u ubuntu nestc-build bash       # 안에서 <tree>/Debug2_remote 로
```

## 2. bitstream 만들기 (PC, docker `vivado2020-build`)

### 2.1 명령

```bash
docker exec -it -u ubuntu vivado2020-build bash
source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh
cd <tree>/vta-hw-zcu102-systolic/hardware/xilinx
make ip CSIM=1 SYSTOLIC=1                      # HLS: C simulation + 4개 IP 합성 (약 10분)
make bin SYSTOLIC=1 FREQ=400 JOBS=4 IMPL_STRATEGY=Performance_ExtraTimingOpt BUILD_TAG=<tag>   # Vivado (약 40분)
```

- 다른 copy도 같은 방법이다(`vta-hw-zcu102-single`, `-multi2`, `-multi4`의 `hardware/xilinx`).
- **첫 PC에서는 `JOBS=4`로 돌린다.** Vivado 전체 부하 중에 PC가 세 번 꺼졌다(10-25, 10-28, 11-02).
  - 돌릴 때 온도를 남기려면 `templog.sh`를 백그라운드로 띄운다(§10).
- 오래 걸리므로 PC 셸에서 `nohup docker exec -u ubuntu vivado2020-build bash -c "source ... && cd ... && make ..." > <log> 2>&1 &`로 돌려도 된다.

### 2.2 make 옵션

| 옵션 | 기본 | 뜻 |
|---|---|---|
| `SYSTOLIC=1` | 0 | GEMM을 RTL systolic 어레이 `gemm_sa`로(`-DVTA_SYSTOLIC`, dir 이름에 `_sa`). `vta-hw-zcu102-systolic`, `-multi4`에서 |
| `ALUX=1` / `ALUX=2` / `ALUX=3` | 0 | ALU 확장(`_alux`, `_alux2`, `_alux3`; [evta_isa_ext.md](evta_isa_ext.md) §8). systolic copy에서 |
| `FREQ=<MHz>` | copy 기본값 | EVTA PLL 클럭(dir 이름에 `_<FREQ>mhz`) |
| 환경 `VTA_HP_FREQ` | 333 | 333 MHz보다 높게 빌드할 때 HP 포트 쪽 클럭(`hp_pll`). PS8 HP 포트는 333 MHz가 한계다 |
| `PROFILE=1` | 0 | compute 카운터와 AXI perf monitor(`_prof`). 보드에서는 vtalib `-DNESTC_EVTA_PROFILE_AXI_MONITOR=ON`과 같이 쓴다 |
| `MEM_OREG=1` | 0 | compute의 inp/wgt BRAM 읽기에 출력 register(`_oreg`) |
| `CSIM=1` | 0 | `make ip` 전에 HLS C simulation(`sim/vta_test.cc`) |
| `JOBS=<n>` | 8 | Vivado 동시 run 수(`launch_runs -jobs`). 첫 PC는 **4** |
| `IMPL_STRATEGY=<이름>` | (기본 전략) | Vivado implementation 전략. systolic 400 MHz는 `Performance_ExtraTimingOpt` 또는 `Performance_ExplorePostRoutePhysOpt` |
| `BUILD_TAG=<tag>` | 없음 | vivado dir과 export 이름 끝에 `_<tag>`. 같은 설정의 다른 판을 구분할 때 |
| `EXPORT_NAME=<이름>` | `vtahw_<dir 이름>` | 출력 파일 이름 |
| 환경 `VTA_BRAM_FENCE` | `y219` | 이 보드의 결함 행(SLICE Y219)을 피하는 방법: `y219`, `upper`, `none`. 이 보드에서는 바꾸지 않는다 |
| 환경 `VTA_RESET_MC=0` | 1 | reset 경로의 multicycle 2(`reset_multicycle.xdc`)를 끈다 |
| 환경 `VTA_QUEUE_RS` | 380 MHz 초과에서 켜짐 | command queue 뒤의 AXIS register slice |

### 2.3 결과 확인과 보드로 옮기기

- **출력:** `<copy>/build/hardware/xilinx/vivado/<dir>/export/vtahw_<dir>.{bin,bit,hwh,xsa,timing.rpt,utilization.rpt}`
- **timing:** `*.timing.rpt`의 Design Timing Summary에서 WNS, WHS, WPWS가 모두 0 이상인지 본다.
  - 실패 path를 묶어 보려면 routed checkpoint(`vta.runs/impl_1/vta_wrapper_routed.dcp`)를 Vivado로 연다.
  - 그다음 `get_timing_paths -max_paths 3000 -nworst 1 -slack_lesser_than 0`의 시작점과 끝점을 센다.
- **보드로 옮기기:** dir 이름 규칙은 `bit/<목적>_<YYMMDD>`이다. 보드에서 통과한 판에는 `<clock>_pass_`를 붙인다.

```bash
# PC에서
ssh xilinx@192.168.20.2 mkdir -p /home/xilinx/JO/npuprg/bit/<목적>_<YYMMDD>
scp <export>/vtahw_<dir>.{bin,bit,hwh,timing.rpt,utilization.rpt} xilinx@192.168.20.2:/home/xilinx/JO/npuprg/bit/<목적>_<YYMMDD>/
# 보드에서
sudo cp /home/xilinx/JO/npuprg/bit/<목적>_<YYMMDD>/vtahw_<dir>.bin /lib/firmware/
```

- `.bit`만 있으면 `vtabundles_xcmpl/hwtest/bit2bin.py <x>.bit`로 `.bin`을 만든다.
- JTAG(ILA, hw_server)를 쓰기 전에는 보드가 부팅될 때마다 `sudo vtabundles_xcmpl/hwtest/nocpuidle.sh`를 먼저 실행한다. 안 하면 Linux가 멈춘다.

### 2.4 HLS 시험 (bitstream 없이)

| 명령 (PC, `vta-hw-zcu102-systolic/hardware/xilinx`) | 하는 일 | 시간 |
|---|---|---|
| `sim/run_csim.sh [ALUX=1\|0] [SYSTOLIC=1\|0]` | HLS C simulation을 g++로(vivado_hls 없이). 임의 ALU 프로그램(`VTA_ALUX_SEEDS`, 기본 20)을 참조 모델과 비교 | 수 초 ~ 수 분 |
| `sim/run_csim.sh PROG=1 [rows] [seed]` | layer norm / softmax ALU 프로그램을 HLS ALU 위에서, bundle CPU 함수와 비교 | |
| `sim/run_cosim.sh [seeds] [insns]` | `compute()`의 C/RTL co-simulation(vivado_hls, 컨테이너). 결과 `build/hardware/xilinx/cosim_alux/cosim.log` | 수십 분 |
| `rtl/tb/run_xsim.sh <vector dir>` | systolic RTL 단위 시험([evta_systolic_design.md](evta_systolic_design.md)) | |

`run_cosim.sh`의 변형은 이렇다. 자세한 것은 스크립트 머리말에 있다.
- `VTA_ALUX_WINDOW=16`: 16 step 안에서는 같은 자리를 다시 읽지 않는 프로그램만 쓴다.
- `VTA_ALUX_DIST=14,15,16,17,18`: 재사용 거리 d를 하나씩 시험한다.
- `VTA_ALUX_HAZARD_OK=1`: 17-step 규칙을 어기는 프로그램을 일부러 넣는다. 이때는 FAIL이 나와야 맞다.

## 3. bundle 소스 만들기 (시뮬레이터 빌드 `Debug2_remote`)

PC 컨테이너 `nestc-build`에서 만드는 것을 권한다. 결과는 보드와 바이트까지 같고 훨씬 빠르다(bundle target 하나에 PC 4~7초, 보드 약 2분).
보드에서도 같은 명령이 돈다.

### 3.1 명령

| 무엇 | 명령 (어디서 실행해도 됨) | 출력 |
|---|---|---|
| NEST-C CMake에 있는 bundle (시험 번들, Resnet18 / 50, VGG16) | `vtabundles_xcmpl/build_sim_bundles.sh [-f] [target ...]` | `Debug2_remote/vta/bundles/<Dir>/`, 로그 `Debug2_remote/bundle_logs/<target>.log`, `summary.txt` |
| MobileNetV2 | `vtabundles_xcmpl/gen_mobilenetv2_bundle.sh [model-compiler 옵션]` | `Debug2_remote/vta/bundles/MobileNetV2Test/` (`gen.log`, `prof.log`) |
| DeiT-tiny | `vtabundles_xcmpl/gen_deit_bundle.sh [model-compiler 옵션]` | `Debug2_remote/vta/bundles/DeiTTest/` (`gen.log`) |
| multi4 copy 4개 | `vtabundles_xcmpl/gen_multi4_bundles.sh <Dir> ...` (`MultiEVTAResnet18Test`, `MultiEVTAResnet50Test`, `MultiEVTAVgg16Cifar10Test`, `MultiEVTADeiTTest`) | `Debug2_remote/vta/multievta_bundles/<Dir>/` (`gen_<i>.log`). single bundle이 먼저 있어야 한다(모델과 profile을 거기서 읽음) |
| model-compiler 다시 빌드 | `Debug2_remote`에서 `make -j16 model-compiler`(PC) / `make -j2 model-compiler`(보드) | VTASave.cpp 등을 바꾼 뒤 |

**`build_sim_bundles.sh`:**
- target마다 시작할 때 로그 경로를 찍는다. make가 끝날 때까지는 화면이 조용하다. 진행은 `tail -f <로그>`로 본다.
- 끝나면 `OK / FAIL`, 걸린 시간, 융합 요약(§3.4)을 찍는다.
- **`-f`:** 그 target이 만든 번들 `.cpp`를 지우고 다시 만든다.
  - make는 번들이 최신이면 model-compiler를 다시 돌리지 않는다.
  - 그래서 옵션이나 `VTA_FUSION_LOG`만 바꾸고 다시 만들려면 `-f`가 필요하다.
- target을 주지 않으면 시험 번들과 Resnet18 / 50 / VGG16 전부를 만든다(보드에서 수십 분).
- `NESTC_SIM_BUILD=<dir>`로 `Debug2_remote` 대신 다른 빌드 dir을 쓸 수 있다.

**`gen_*_bundle.sh`:**
- MobileNetV2와 DeiT는 NEST-C CMake에 target이 없다. 그래서 make 없이 도구를 직접 부르고, 매번 model-compiler를 돌린다.
  - target을 더하면 Debug2_remote의 cmake가 다시 돌아 Glow 전체를 다시 빌드한다(-j2로 약 1시간).
- DeiT에 필요한 것: `Debug2_remote/vta/bundles/DeiTTest/`에 `deit_fold.onnx`가 있어야 한다.
  - PC에서 `vtabundles_xcmpl/fold_onnx.py model.onnx deit_fold.onnx 1,3,224,224`로 만든다. 보드의 onnx 1.11에는 `onnx.reference`가 없다.
- DeiT의 Glow graph hash는 실행마다 달라진다. 그래서 스크립트가 profile 사본의 hash를 고쳐 가며 model-compiler를 4번까지 돌린다.
- DeiT의 환경 변수:
  - `PRUNE_ROWS=1`(기본): `prune_rows_onnx.py`로 마지막 블록을 출력이 읽는 2행만 계산하게 바꾼다([deit_optimization.md](deit_optimization.md) §6.7).
  - `QSCHEMA=symmetric`(기본).

**주의:**
- **보드에서는 새 build dir을 만들지 않는다.** 메모리가 3.9 GB라 처음부터 빌드하면 메모리가 모자란다. `make -j2`까지만 쓴다.
- cmake가 다시 돌면 Glow 거의 전체가 다시 빌드된다(보드 -j2로 약 1시간). CMakeLists를 바꿨거나 날짜가 바뀐 뒤 첫 빌드일 때 그렇다.
  - 로그에 `Re-running CMake`가 보이면 멈추고 PC에서 하는 것이 낫다.
- Resnet18 conv0을 EVTA에서 돌리려면 `cmake -DVTA_RESNET18_WITH_SKIPQUANT0=OFF .`로 한 번 설정한다(지금 기본).

### 3.2 ONNX 도구 (PC)

| 명령 | 하는 일 |
|---|---|
| `vtabundles_xcmpl/fold_onnx.py in.onnx out.onnx N,C,H,W` | 입력 모양을 고정하고, 알 수 있는 노드를 모두 상수로 접는다(DeiT 1261 → 608 노드). onnx ≥ 1.13 필요 |
| `vtabundles_xcmpl/prune_rows_onnx.py in.onnx out.onnx [in.yaml out.yaml [extra.yaml]]` | 출력이 읽는 행만 계산하게 Slice를 넣는다. yaml을 주면 profile도 맞춰 복사한다. `gen_deit_bundle.sh`가 부른다 |

### 3.3 VTA backend 옵션 (`model-compiler`, `VTA.cpp` / `VTASave.cpp`)

- 표에 "꺼짐"이라고 적은 것만 기본으로 꺼져 있고, 나머지는 켜져 있다.
- `=0`을 주면 그 단계 이전의 경로로 bundle을 만든다. 비교하거나 문제를 가를 때 쓴다.
- 실행 때 줄 것은 없다. 옵션의 결과는 생성된 코드에 들어간다.
- 모델마다 다른 옵션은 그 모델의 생성 규칙에 적혀 있다.
  - MobileNetV2: `vta/bundles/MobileNetV2Test/CMakeLists.txt`, `gen_mobilenetv2_bundle.sh`.
  - DeiT: `gen_deit_bundle.sh`(`-vta-round-shift`).
- 조건이 맞지 않는 층은 옵션이 켜져 있어도 이전 경로(CPU 함수나 따로 부르는 EVTA 호출)로 간다. 채널 수, offset, scale 비, 모양이 그런 조건이다.
- 무엇이 왜 그렇게 됐는지는 §3.4의 요약에 나온다.

생성된 bundle `.cpp` 맨 앞에는 아래 옵션 값이 기본값까지 주석으로 적힌다. 그 뒤에 융합 요약이 이어진다(§3.4).
그래서 bundle을 어떤 옵션으로 만들었는지 `head -40 hw/MobileNetV2Test/*.cpp`로 확인할 수 있다.

| 옵션 | 켜면 하는 일 | 자세히 |
|---|---|---|
| **층 사이 layout** | | |
| `-vta-keep-layout` | 층 사이 activation을 EVTA layout N{C/16}HW16으로 둔다(층마다 transpose 없음) | compare §7.1 |
| `-vta-pack-rows` | DeiT의 행 × C int8 텐서도 EVTA layout으로 | [evta_isa_ext.md](evta_isa_ext.md) §8.16 |
| `-vta-attn-keep-layout` | Q K^T와 A × V를 query / key / value conv 출력에서 바로 읽기(`matmul_evta_qk`, `matmul_evta_av`) | compare §7.27, deit §6.8 |
| `-vta-pad-channels` | 채널 수가 16의 배수가 아닌 conv / add activation을 0 채널로 넓혀 그 conv를 EVTA로(MobileNetV2 24채널) | §7.20 |
| **conv와 꼬리 (융합)** | | [evta_fusion_rules.md](evta_fusion_rules.md) |
| `-vta-fuse-residual` | conv 뒤의 elementadd + relu를 같은 EVTA 호출에서 | §7.2 |
| `-vta-fuse-residual-norelu` | relu 없는 residual add도(MobileNetV2) | isa_ext §8.12 |
| `-vta-conv-epilogue` | conv 뒤 elementwise chain을 epilogue 프로그램으로(`VTASetConvEpilogue`) | [evta_alux_epilogue.md](evta_alux_epilogue.md) |
| `-vta-fuse-pool` | EVTA conv(relu) 뒤의 max pool 2x2/2/0, 3x3/2/1을 conv 호출 안에서(`convolution_wo_tr_pool`) | §7.13, [evta_conv_pool_guide.md](evta_conv_pool_guide.md) |
| `-vta-alu-output-lut` | conv 뒤의 표 구간(GELU), attention `Div` 표를 conv / Q K^T의 출력 LUT로 | isa_ext §8.10, §8.22 |
| `-vta-fuse-embed` | patch embedding의 position-embedding add를 patch conv의 epilogue로 | deit §6.9 |
| `-vta-fuse-elemadd-mul` (**꺼짐**) | 2의 거듭제곱이 아닌 scale 비의 add를 conv epilogue로(MUL / ADD / RQ). 결과는 같지만 `elemadd_evta`가 더 빠름 | isa_ext §8.18 |
| **첫 conv** | | |
| `-vta-fuse-preprocess` | 입력 quantize, 채널별 상수 elemsub/elemdiv, NCHW→NHWC를 conv0 입력 복사 하나로 | §7.9 |
| `-vta-conv0-pack` | 채널이 적은 첫 conv를 packed 입력의 ceil(KH/s)×1 conv로(Resnet18: 7×7/2 → 4×1, 48채널) | §7.12 |
| **EVTA에서 돌릴 연산** | | |
| `-vta-evta-maxpool` | EVTA layout max pool을 EVTA에서(`maxpool_evta`) | §7.3 |
| `-vta-evta-fc` | fc를 EVTA에서. weight와 bias는 compile time에 EVTA layout으로(`fullyconnected_evta_packed`) | §7.8 |
| `-vta-evta-depthwise` | depthwise channelwise conv를 EVTA에서(`dw_convolution_evta`). `-enable-channelwise-depthwise`에만 걸림 | §7.21 |
| `-vta-evta-avgpool` | global avgpool을 EVTA ALU에서(`avgpool_evta`, `_alux`만) | isa_ext §8.12 |
| `-vta-evta-elemadd` | packed int8 add를 정수 a A + b B >> N으로 EVTA ALU에서(`elemadd_evta`) | isa_ext §8.17 |
| `-vta-elemadd-precise` (**꺼짐**) | 위를 22비트 곱수로(약 1.8배 ALU 시간) | isa_ext §8.17 |
| `-vta-matmul-conv` | MatMul을 int8로, 상수 오른쪽 MatMul(Linear)을 EVTA 1x1 conv로 | §7.23 |
| `-vta-fold-matmul-bias` | Linear 뒤의 상수 bias Add를 conv bias로 | §7.26 |
| `-vta-evta-matmul` | 오른쪽도 activation인 int8 MatMul을 EVTA에서(`matmul_evta`) | §7.24 |
| `-vta-matmul-heads` | 같은 모양의 연속 matmul(attention head)을 EVTA 호출 하나로 | §7.27 |
| `-vta-alu-layernorm` | 융합한 layer norm을 EVTA ALU에서(`layernorm_evta`) | isa_ext §8.10 |
| `-vta-alu-softmax` | softmax + quantize를 정수 softmax로, ALU 확장이 있으면 EVTA에서. `=0`은 float softmax | deit §6.10 |
| **CPU 쪽** | | |
| `-vta-lut-elementwise` | int8 → int8 elementwise 연산 구간을 256칸 표 하나로 | §7.25 |
| `-vta-fuse-layernorm` | int8 layer norm chain을 함수 하나로 | §7.26 |
| `-vta-cpu-approx` | softmax / layer norm의 나눗셈을 역수 곱으로(값이 조금 바뀔 수 있음) | §7.27 |
| **양자화 / 반올림** | | |
| `-vta-pow2-ratio` | int8 conv / fc / matmul의 출력 scale을 올려 in × w / out을 2의 거듭제곱으로(`-quantization-schema=symmetric`에서) | deit §6.6 |
| `-vta-round-shift` (**꺼짐**) | EVTA conv 출력 shift를 반올림(bias에 2^(shift−1)). 켜면 모든 EVTA conv 출력이 바뀐다. MobileNetV2, DeiT만 켬 | §7.20 |
| `-enable-channelwise-depthwise` (꺼짐, quantizer) | depthwise conv만 채널별 scale로 양자화 | §7.20 |
| **multi-EVTA** | | |
| `-index-multi-evta=<mask>` | bundle을 EVTA 하나에 고정(1, 2, 4, 8 → EVTA 0..3) | README |

"§"만 적은 것은 [evta_runtime_compare.md](evta_runtime_compare.md)의 절이다.

**옵션 하나만 바꿔 번들을 만들 때:**
- 모델의 CMakeLists에 옵션을 붙이면 Debug2_remote의 cmake가 다시 돌아 Glow가 다시 빌드된다.
- 그래서 `gen_*_bundle.sh <옵션>`처럼 model-compiler를 직접 부르거나, 빈 작업 dir에서 부른다.
- 결과를 `hw/<Dir>/`에 복사해 빌드하고, 끝나면 `collect_hw_bundles.py <Dir>`로 되돌린다([resnet18_optimization.md](resnet18_optimization.md) §9.3).

### 3.4 무엇이 EVTA / CPU에서 도나: `VTA_FUSION_LOG`

model-compiler가 bundle을 만들 때 읽는 환경 변수다. 번들 코드는 이 값과 상관없이 같다.

| 값 | 출력 (stderr, 스크립트가 로그로 보냄) |
|---|---|
| 없음 또는 빈 값 (기본) | 요약만 |
| `0` | 아무것도 안 찍음 |
| 그 밖의 값 (`1` 등) | 요약 + 명령마다 자세히(생산자와 꼬리, CPU로 간 conv / fc / matmul과 그 이유) |

요약은 bundle `.cpp` 맨 위 주석(`// EVTA fusion summary`)에도 **늘** 들어간다. 스크립트는 끝에 그것을 화면에 찍는다.

```
  73 EVTA producers, 13 with a tail, 24 read by a tail op not fused
    not fused: 24 x an add: -vta-fuse-elemadd-mul is off (elemadd_evta is faster)
  6 GEMMs (conv / fc / matmul) on the CPU
    on the CPU: 3 x matmul 2 x 198 x 64: 2 rows: no H x W with W >= 7 (largest W 1)
```

```bash
VTA_FUSION_LOG=1 vtabundles_xcmpl/build_sim_bundles.sh -f vtaVGG16Cifar10Bundle   # -f: 최신이어도 다시 만듦
VTA_FUSION_LOG=1 vtabundles_xcmpl/gen_deit_bundle.sh
```

규칙과 읽는 법: [evta_fusion_rules.md](evta_fusion_rules.md), 코드와 그림: [evta_fusion_rules_guide.md](evta_fusion_rules_guide.md).

## 4. 보드용 실행 파일 빌드 (`vtabundles_xcmpl/`)

### 4.1 보드에서

```bash
cd ~/JO/npuprg/nestc2_clion/nest-compiler/vtabundles_xcmpl
./collect_hw_bundles.py [--set multievtax2|multievtax4] [Dir ...]   # Debug2_remote의 소스를 hw[_<set>]/<Dir>/로 복사, run_all.sh 다시 생성
cd build_sysprof && make -j2 vtaMxnetResnet18Bundle                   # 또는 make -j2 hw_bundles (전부)
```

- `collect_hw_bundles.py`는 `hw/script/run_all.sh`를 다시 만든다.
  - git의 파일과 달라지면 보드에서 `git stash` 후 pull한다(보드에서는 `git checkout`으로 되돌리지 않는다).
- MobileNetV2 / DeiT 등 처음 더한 target은 그 build dir에서 `cmake .`를 한 번 한다.

build dir마다 cmake 옵션이 정해져 있다. 처음 한 번만 `mkdir <dir> && cd <dir> && cmake <옵션> ..`으로 만든다(git에 없음).

| build dir | cmake 옵션 | 실행 파일 끝 | 무엇을 재나 |
|---|---|---|---|
| `build/` | (없음) | — | 기본 HLS bitstream, golden 확인 |
| `build_prof/` | `-DNESTC_EVTA_PROFILE=ON -DBUNDLE_EXE_SUFFIX=_prof` | `_prof` | EVTA run마다 시간과 GOPS |
| `build_timed/` | `-DBUNDLE_TIMED=ON -DBUNDLE_EXE_SUFFIX=_timed` | `_timed` | bundle 문장마다 wall time(`timed_bundle.py`가 .cpp를 감쌈) |
| `build_sys/` | `-DVTALIB=<root>/vtalib-zcu102-systolic -DBUNDLE_EXE_SUFFIX=_sys` | `_sys` | systolic bitstream, golden / 정확도 |
| `build_sysprof/` | 위 `_prof` + systolic vtalib, 끝 `_sysprof` | `_sysprof` | systolic bitstream에서 run마다 |
| `build_systimed/` | 위 `_timed` + systolic vtalib, 끝 `_systimed` | `_systimed` | systolic bitstream에서 문장마다 |
| `build_multievtax2[_prof\|_timed]/` | `-DBUNDLE_SET=multievtax2` (+ 위 옵션) | | EVTA 2개 |
| `build_multievtax4[_prof\|_timed\|_sys\|_sysprof\|_systimed]/` | `-DBUNDLE_SET=multievtax4 -DNESTC_EVTA_MULTI=ON -DNESTC_EVTA_MAX_XFER=4194304` (+ 위 옵션) | | EVTA 4개 |

- `BUNDLE_SET`은 buffer 크기와 queue 크기를 정한다(single 16/16/19/18, multi 16/15/18/17).
- `VTALIB`는 vtalib 소스 dir이다. 기본은 `vta/vtalib`이고, systolic bitstream에는 `vtalib-zcu102-systolic`을 쓴다.
- vtalib의 헤더(`cmd_queue.h` 등)만 바꾸면 의존성이 잡히지 않을 수 있다. 결과가 이상하면 그 build dir의 vtalib을 다시 빌드한다.

### 4.2 PC에서 cross build

결과는 보드에서 빌드한 것과 기계어까지 같다. 먼저 §1의 `sync_sysroot.sh`를 해 둔다.

```bash
# PC, nestc-build 컨테이너 안
cd <tree>/vtabundles_xcmpl && mkdir build_pc_systimed && cd build_pc_systimed
cmake -DCMAKE_TOOLCHAIN_FILE=<tree>/utils/docker_nestc/zcu102_toolchain.cmake \
      -DBUNDLE_TIMED=ON -DBUNDLE_EXE_SUFFIX=_systimed -DVTALIB=$PWD/../../vtalib-zcu102-systolic ..
make -j16 vtaMobileNetV2Bundle
scp ../hw/MobileNetV2Test/vtaMobileNetV2Bundle_systimed xilinx@192.168.20.2:<tree>/vtabundles_xcmpl/hw/MobileNetV2Test/
```

- toolchain file을 바꾸면 **새 build dir**에서 configure한다. `cmake .`는 처음의 `*_INIT` 플래그를 그대로 쓴다.
- 맞춘 내용(libgomp, hash style 등)은 [host_pc_build.md](host_pc_build.md) §4에 있다.

## 5. 실행과 측정 (보드, sudo)

### 5.1 실행 전에

- **다른 사용자의 EVTA 실행을 확인한다:** `ps aux | grep -E "[r]un_prof|[r]un_all|Bundle"`.
  - run 스크립트는 `/tmp/evta_run.lock`으로 한 번에 하나만 돈다.
  - 다른 사람의 시간 측정 중에는 빌드나 실행으로 끼어들지 않는다.
- **sudo는 `sudo -n /bin/sh -c "..."` 형태로** 쓴다. `sudo VAR=... cmd`는 비밀번호를 묻는다.
- **실행마다 bitstream을 다시 올린다.** 멈춘 run이 있으면 EVTA가 deadlock 상태로 남기 때문이다.
  - run 스크립트는 자동으로 올린다. 손으로는 `sudo vtabundles_xcmpl/hw/script/load_bit.sh <bin>`이다.
  - 올린 이름은 `/tmp/evta_bitstream`에 남고, vtalib이 그 이름으로 기능을 고른다.
- **CMA:** 비정상 종료한 프로세스의 CMA(128 MB 중)는 `xlnk_reset` 전까지 돌아오지 않는다.
  - 손으로 할 때: `sudo -n /bin/sh -c "cd /tmp && python3 -c 'from pynq import Xlnk; Xlnk().xlnk_reset()'"`.
  - **다른 사람이 EVTA를 쓰는 중에는 하지 않는다.** 그 사람의 CMA도 지운다.
  - 남은 양은 `grep CmaFree /proc/meminfo`로 본다.
- **root 디스크가 100 %가 되면 run이 조용히 끝난다.** 로그가 잘리고 rc 3이 된다. dump는 쓴 뒤에 지운다.
- ssh로 오래 걸리는 일을 걸 때는 `nohup <cmd> > <log> 2>&1 < /dev/null &`를 쓴다. `< /dev/null`이 없으면 ssh가 끝나지 않는다.

### 5.2 golden 확인: `run_all.sh`

```bash
cd ~/JO/npuprg/nestc2_clion/nest-compiler/vtabundles_xcmpl
sudo hw/script/run_all.sh [Dir ...]                     # single, build/의 실행 파일, 결과 hw/result/results.txt
sudo hw_multievtax4/script/run_all.sh                    # multi4 (multi2도 같은 형태)
sudo /bin/sh -c "BIN=<file in /lib/firmware> hw/script/run_all.sh Resnet18Test"   # 다른 bitstream으로
```

- 기본 bitstream은 `run_all.sh` 맨 위의 `BIN`(HLS 380 MHz)이다. `SYS_BIN`(systolic 400 MHz)은 `run_prof.sh -s _sys*`가 쓴다.
- `OMP_NUM_THREADS` 기본값은 1이다.

### 5.3 측정: `run_prof.sh`

```bash
sudo hw/script/run_prof.sh [-s <suffix>] [-n <runs>] [-l] [-r] [-t <threads>] Dir [Dir ...]
# 예
sudo /bin/sh -c "hw/script/run_prof.sh -s _sysprof -n 25 -r Resnet18Test"
sudo /bin/sh -c "hw/script/run_prof.sh -s _systimed -r -t 4 Resnet18Test"
sudo /bin/sh -c "hw_multievtax4/script/run_prof.sh -s _sysprof -n 25 -l -r MultiEVTAResnet18Test"
```

| 옵션 | 뜻 |
|---|---|
| `-s _prof` (기본) / `_timed` / `_sysprof` / `_systimed` / `_sys` | 어느 build dir의 실행 파일인지. `_sys*`는 `SYS_BIN`(systolic)을 올린다 |
| `-n <runs>` | 추론 횟수(기본 25). multi는 copy당 |
| `-l` | multi-EVTA lockstep: 모든 EVTA 호출 앞에서 copy들이 barrier로 만나 같은 conv를 동시에 돈다 |
| `-r` | `chrt -f 10`(SCHED_FIFO). 4 ms scheduler tick 때문에 흔들리는 run이 없어진다. 시간 측정에는 붙인다 |
| `-t <n>` | `OMP_NUM_THREADS`(기본 1). `-t 4`면 4코어 |
| 환경 `BIN=<file>` | bitstream 바꾸기 |
| `-h`, `--help` | 옵션 설명을 출력하고 끝난다(sudo 없이 된다). `run_all.sh --help`, `load_bit.sh --help`도 있다 |

- **실행 시작 때 출력:** 어느 bitstream으로 도는지와 그 설정을 먼저 출력하고, summary 파일 맨 앞에도 넣는다(`vtabundles_xcmpl/bininfo.sh`). 나오는 항목은 다음과 같다.
  - bitstream: 파일 이름, 어디서 왔는지(`BIN` / `SYS_BIN` / 환경 `BIN=`), 날짜, md5 앞 8자리.
  - 이름에서 읽은 것: EVTA 수, clock, GEMM 종류(HLS / systolic), 기능(ALU imm II = 1, bias reset, requantize, ALU 확장, AXI perf monitor).
  - 실행 설정: `EVTA_*` / `MULTI_*` / `BUNDLE_*` 환경 변수, `-s`의 build dir과 무엇을 재는지, 횟수, lockstep, `chrt`, `OMP_NUM_THREADS`, Dir 목록.
  - 경고: `-s`와 bitstream이 맞지 않으면 `WARNING`을 출력한다.
- **결과:**
  - 로그는 `hw/<Dir>/<suffix>.log`다.
  - `_prof` 로그에는 `prof_summary.py` 요약(`<log>.sum`, conv마다 시간과 GOPS)이 붙는다.
  - `_timed` 로그에는 `run_times.py` 요약(추론마다 시간, 중앙값, GOPS)이 붙고, 문장별 시간은 `timing.csv`에 남는다.
  - Dir마다 한 줄씩 `hw/result/<suffix>_summary.txt`에 남는다(exit, golden, 전체 시간).
- multi-EVTA 실행 파일은 copy마다 표를 따로 출력한다.
  - `multi4conv4Bundle <mask> [iters] [conv]` 같은 GOPS 측정은 [vtabundles_xcmpl/README.md](../vtabundles_xcmpl/README.md)를 본다.

## 6. 정확도 (ImageNetV2, `vtabundles_xcmpl/imagenet_eval/`)

| 일 | 어디서 | 명령 |
|---|---|---|
| 이미지 준비 (10,000장 → 224 × 224 PNG, `list.txt`) | PC | `prep_imagenetv2.py <imagenetv2-matched-frequency-format-val> <out>`. 보드 `~xilinx/JO/imagenetv2_png224`로 복사 |
| bundle top-1 | 보드 | `sudo /bin/sh -c "imagenet_eval/eval_board.sh <exe> <name> [n]"`. 예: `build_sys/DeiTTest/vtaDeiTBundle_sys base 10000`. 결과 `imagenet_eval/result/<name>.log` |
| float 기준 | PC | `float_ref.py <model.onnx> <png dir> [n] [out]` (onnxruntime) |
| int8 흉내 (보드 없이 양자화 시도) | PC | `fakequant.py <model.onnx> <png dir> [--n 9000] [--pow2] [--ln-float] [--gelu-float] [--softmax-float] [--skip s]` |

- `eval_board.sh`는 bitstream을 올리고 lock을 잡는다(`BIN=`, 기본 `SYS_BIN`).
  - 환경 `PNG=<dir>`, `OMP_NUM_THREADS`(기본 4), 그리고 `EVTA_*` 같은 실행 스위치를 그대로 넘긴다.
- DeiT main의 환경 변수:
  - `EVAL_LIST=<file>`("<png> <class>" 줄들): 정확도 모드.
  - `EVAL_HASH=1`: 이미지마다 출력 바이트의 hash를 찍는다. 두 bundle이 같은 출력을 내는지 비교할 때 쓴다.

## 7. runtime 환경 변수 (vtalib, 실행할 때)

"systolic"이라고 적은 것은 `vtalib-zcu102-systolic`에만 있다.

**동작 바꾸기**

| 변수 | 기본 | 뜻 |
|---|---|---|
| `EVTA_REPLAY` | 1 | EVTA 명령 record / replay. `0`이면 매번 명령을 새로 만든다 |
| `EVTA_REPLAY_MB` | 16 | EVTA마다 replay 사본에 쓰는 메모리 상한 |
| `EVTA_BUF_POOL_MB` | 32 | buffer pool 상한. `0`이면 pool을 끈다 |
| `EVTA_RUN_TIMEOUT_MS` | 5000 | run 하나가 이 시간을 넘으면 모든 EVTA 상태와 명령을 `/tmp/evta_hang_*`에 남기고 exit 5. `0`은 무제한 |
| `EVTA_NO_BIT_CHECK=1` | 0 | 올라간 bitstream의 EVTA 수 검사를 건너뛴다(위험: EVTA 수가 모자라면 SError) |
| `EVTA_BIAS_RESET=0\|1` | bitstream 이름 | bias reset 강제(이름에 `bias`나 `_rq`가 있으면 켜짐). [evta_isa_ext.md](evta_isa_ext.md) |
| `EVTA_REQUANT=0\|1` | bitstream 이름 | out 쓰기 requantize 강제(이름에 `_rq`). 기능이 없는 bitstream에서 1이면 결과가 틀린다 |
| `EVTA_ALUX=0\|1`, `EVTA_ALUX2`, `EVTA_ALUX3` | bitstream 이름 | systolic: ALU 확장 사용(`_alux<k>`). `0`이면 같은 실행 파일이 CPU 대체 경로로 |
| `EVTA_ALU_INP=0` | 1 | systolic: layer norm / softmax의 int8 입력을 inp + identity GEMM 대신 int32로 acc에 |
| `EVTA_AVGPOOL=0` | 1 | systolic: `avgpool_evta`를 끄고 CPU avgpool |
| `EVTA_LOAD_AHEAD` | 2 | systolic: 다음 filter group / tile의 LOAD를 앞당김. `0` 끔, `2`는 1x1 conv 상주 weight 재LOAD도 생략([evta_load_ahead_guide.md](evta_load_ahead_guide.md)) |
| `EVTA_CONV_BURST=0` | 1 | systolic: conv DMA를 길게 묶는 것을 끔 |
| `EVTA_INPUT_STREAM=0` | 1 | systolic: conv0 입력을 conv 전에 한 번에 채움. 기본은 tile 행마다 run을 나눠 겹침 |
| `EVTA_MIN_TILE=<n>` | bitstream 이름 | systolic: 가장 작은 conv 출력 tile. 이름에 `_sa_`면 1, 아니면 7 |
| `EVTA_SCHED=<H>,<W>,<C>,<KN>,<K>,<stride>:<vt>,<tileH>,<tileW>[;...]`, `EVTA_SCHED_LOG=1` | 없음 | systolic: conv 모양의 schedule을 실행 중에 바꾸기, conv마다 모양과 schedule 출력(compare §7.19) |
| `EVTA_RES_PREFETCH=1` | 0 | systolic: epilogue residual을 첫 입력과 같이 LOAD(이득 없음, isa_ext §8.19) |
| `EVTA_QUANT_EXACT=1` | 0 | quantize를 나눗셈으로(기본은 곱 + 확인) |
| `EVTA_CONST_SHARE=0` | 1 | load_module의 상수 VTA buffer를 copy마다 따로. 기본은 같은 내용이면 process 안에서 공유 |
| `NESTC_LN_INT=0` | 1 | DeiT layer norm을 int8 chain으로(기본 정수 layer norm) |
| `NESTC_AVGPOOL_NEON=1` | 0 | 비교용 NEON CPU avgpool |
| `MULTI_MASK=<bits>` | 모두 | multi4 Resnet50 / VGG16: 이 copy만 돌린다(copy i = EVTA i) |
| `MULTI_RUNS`, `MULTI_LOCKSTEP` | 25, 0 | multi-EVTA main의 copy당 추론 수, lockstep(`run_prof.sh -n`, `-l`이 설정) |
| `BUNDLE_RUNS` | | `_prof` / `_timed` 실행 파일의 추론 수(`run_prof.sh -n`이 설정) |

**확인 (EVTA 결과를 CPU와 실행 중에 비교, 불일치 수 출력)**

| 변수 | 무엇을 |
|---|---|
| `EVTA_RES_CHECK=1` | fused residual, conv + pool, avgpool, depthwise |
| `EVTA_EPI_CHECK=1` | conv epilogue(어떻게 내렸는지도 출력) |
| `EVTA_EA_CHECK=1` | `elemadd_evta` |
| `EVTA_LN_CHECK=1`, `EVTA_SM_CHECK=1` | layer norm, softmax |
| `EVTA_MATMUL_CHECK=1` | `matmul_evta*`(Q K^T는 출력 LUT까지) |

**기록과 시간**

| 변수 | 뜻 |
|---|---|
| `EVTA_DUMP_CONV=<dir>` | EVTA conv와 maxpool 출력을 모두 `conv_<n>.bin`으로. 변경 전후 byte 비교용(`_CHECK` 변수 없이 비교한다) |
| `EVTA_REPLAY_LOG=1\|2` | `1`: 기록 목록. `2`: replay된 run마다 `EVTA_RUN` 줄(GEMM/ALU 반복, DRAM 바이트, µs) |
| `EVTA_REPLAY_DUMP=<dir>` | 기록된 run마다 명령과 uop을 텍스트로(`rec_<id>_<seg>.txt`). `hazard_dump.py`의 입력(§8) |
| `EVTA_LN_TIME=1`, `EVTA_SM_TIME=1`, `EVTA_MATMUL_TIME=1` | layer norm / softmax / matmul 시간 분해 |

**실험과 멈춤 추적 (결과가 틀릴 수 있음)**

| 변수 | 뜻 |
|---|---|
| `EVTA_MEMEXP=<mask>` | replay의 inp/wgt/acc LOAD와 STORE(bit 0~3)를 1 element로. **결과는 틀린다** |
| `EVTA_TIME_NO_CONV_CLIP=1` | conv clip의 MAX / MIN을 뺀다(시간 측정용, 결과 틀림) |
| `EVTA_FC_PROBE=1` | fc EVTA 경로 bring-up 시험(결과 틀림) |
| `EVTA_NO_DSB=1` | cache flush 뒤 `dsb sy`를 뺀다(multi4 멈춤 재현용) |
| `EVTA_PRE_RUN_US`, `EVTA_PRE_RUN_MASK`, `EVTA_PRE_RUN_AT` | run 앞 대기 시간(µs), 대기할 EVTA mask, 대기 위치 |
| `EVTA_STORE_SENTINEL`, `EVTA_DOUBLE_QUEUE` | STORE 영역 표식, 명령 buffer 번갈아 쓰기 |
| `EVTA_SIM_TOKENS=1`, `EVTA_ALU_HAZARD=17`, `EVTA_ALU_SLOTS=34` | 시뮬레이터 전용: run이 남긴 token 출력, ALU hazard 흉내(step / slot 수) |

## 8. vtalib 시험 (`vtalib-zcu102-systolic/tests/`)

| 명령 | 어디서 | 하는 일 |
|---|---|---|
| `run_alu_ext_test.sh [rows] [seed]` | g++ 있는 아무 기계 | 정수 layer norm ALU 프로그램 vs bundle CPU 함수(수 초) |
| `run_lut_conv_test.sh` | PC (llvm-8 헤더) | 시뮬레이터 vtalib을 빌드해 conv 출력 LUT, `layernorm_evta`, `softmax_evta`를 CPU와 비교(약 1분) |
| `run_epi_test.sh` | PC | conv epilogue를 세 방식(rq 끔, `EVTA_REQUANT=1`, `EVTA_ALUX=0`)으로 CPU와 비교 |
| `sudo -n /bin/sh -c "run_alux_board_tests.sh [build\|run\|all]"` | 보드 (`_alux` bitstream 올린 뒤) | 위 시험들을 보드 EVTA에서. `EVTA_ALUX=0`이면 CPU 대체 경로. `CROSS=1`이면 PC에서 빌드 |
| `WS=7,17 hazard_dump.py <EVTA_REPLAY_DUMP dir>` | 아무 데 | ALU hazard 검사: vector ALU 명령이 최근 W step 안에 쓴 acc 자리를 읽는지 |

## 9. 하드웨어 시험 (보드, `vtabundles_xcmpl/hwtest/`)

```bash
cd ~/JO/npuprg/nestc2_clion/nest-compiler/vtabundles_xcmpl/hwtest
SUFFIX=_rb ./build.sh runbench                 # vtalib ZCU102 -D 세트로 빌드 (BUFF, UOP, ISA 등은 build.sh 머리말)
sudo ./runbench_rb base 0 1 aluimm 16 2048 load 16 4096 ...   # 여러 case를 한 프로세스에서 (REPS=200)
sudo ./gemmprobe ident                         # 새 bitstream의 GEMM 기본 확인 (reset, zero, ident, perm, ones)
```

| 명령 | 하는 일 |
|---|---|
| `runbench` | run 하나의 고정 비용(약 4.7 µs), 명령 하나와 반복 하나의 비용. case `base`, `gemm N K`, `gemmu N K`, `aluimm N K`, `alureg N K`, `load N K`, `store N K`. 한 프로세스에서 여러 case를 돌린다(프로세스를 여러 번 띄우면 CMA가 샌다) |
| `sudo -n /bin/sh -c 'NUM_EVTA=4 /bin/sh probes.sh <bin> [...]'` | bitstream 프로브 묶음: gemmprobe, sramscan, donerace를 case마다 bitstream을 다시 올려 가며. 통과 기준은 머리말 |
| `sudo ./run_rawtests.sh`, `run_rawtests2.sh` | ISA 비트폭 프로브 `rawtest*`. 다른 bitstream을 쓰기 전에 먼저([vta_synth_notes.md](vta_synth_notes.md) §2.4). `BIN=`으로 bitstream |
| `sudo ./run_convvar.sh` | vtalib `convolution()` EVTA vs CPU, conv 모양 여러 개 |
| `sudo python3 dump_regs.py` | 시간 초과 직후 EVTA 네 HLS 블록의 AXI-lite 레지스터 |
| `sudo python3 dump_insns.py [evta] [count]` | 멈춘 run의 fetch 레지스터와 DRAM의 명령(runtime이 쓴 것과 비교) |
| `sudo python3 plreset.py [n]` | PS→PL reset `pl_resetn<n>` 펄스(`xlnk_reset`은 PL을 건드리지 않음) |
| `sudo python3 plclk.py` / `plclkset.py <MHz>\|stop\|start\|show` | `pl_clk0` 보기 / 바꾸기(기본 100 MHz로 되돌린다) |
| `sudo ./nocpuidle.sh` | JTAG 전에 cpuidle 끄기(부팅마다) |
| `bit2bin.py <x>.bit` | `.bit` → fpga_manager용 `.bin` |
| `brambist/brambist.py all\|columns\|load <s>` | BRAM self test(결함 행 찾기). `stuckchk.py`, `cfgdiff.py`, `sramscan_analyse.py`, `regtest.py`, `regpat.py`는 같은 조사용 |

결함 행(Y219) 조사와 multi4 route는 `log/BOARD_TODO.md`, `vta-hw-zcu102-multi4/hardware/xilinx/scripts/y219route/README.md`에 있다.

## 10. 그 밖의 도구

| 일 | 어디서 | 명령 |
|---|---|---|
| 날짜별 일지 생성 | PC | `docs/dailyreport/tools/daily_report.sh [YYMMDD ...]` (cron 매일 07:00, 07:00 경계, 로그 `~/.cache/daily_report.log`) |
| 문서 그림 다시 만들기 | PC | `python3 docs/figs/gen_evta_<이름>_figs.py docs/figs` |
| bundle 문장마다 시간 재는 사본 | (CMake가 부름) | `vtabundles_xcmpl/timed_bundle.py <in.cpp> <out.cpp> [csv]` |
| `_timed` 로그 요약 | (run_prof.sh가 부름) | `vtabundles_xcmpl/run_times.py <Dir> <log>` |
| PC 온도 기록 | 첫 PC | `nohup /home/ubuntu/npuprg/templog/templog.sh &` → `templog/temp_<날짜_시각>.log` |

## 11. 자주 하는 일의 순서

**backend(`VTASave.cpp`, `VTA.cpp`)를 바꿨을 때**
1. PC 컨테이너 `nestc-build`의 `Debug2_remote`에서 `make -j16 model-compiler`.
2. bundle을 만든다(§3.1). `build_sim_bundles.sh -f <target>`, `gen_*_bundle.sh`, multi4는 `gen_multi4_bundles.sh`.
3. 바꾸기 전의 `.cpp`와 비교한다. 바이트가 같으면 보드 실행은 바뀌지 않는다.
4. 바뀌었으면:
   - commit, push.
   - 보드에서 pull, `Debug2_remote`에서 `make -j2 model-compiler`, 다시 생성.
   - 또는 PC에서 cross build해서 scp(§4.2).
   - 그다음 `collect_hw_bundles.py`, 빌드, 측정.
5. 결과가 같은지 `EVTA_DUMP_CONV`나 `_CHECK` 변수로 확인한다.

**vtalib(runtime)만 바꿨을 때**
1. PC에서 고친다. 새 기능은 `vtalib-zcu102-systolic`, HLS용 공통 수정은 `vta/vtalib`에도 한다.
2. PC에서 시험을 돌린다(§8).
3. `vta/vtalib`(submodule)을 commit하고 push한다. main에서 submodule 포인터와 systolic 사본을 commit하고 push한다.
4. 보드에서 pull한다(main, 그다음 `vta/vtalib` 안).
5. `vtabundles_xcmpl/build_sysprof` 등에서 `make -j2 <target>`으로 빌드한다.
6. `run_prof.sh -s _sysprof`로 측정한다. golden 확인은 `run_all.sh`로 한다.
7. 결과가 같은지는 `EVTA_DUMP_CONV`로 변경 전후 출력을 비교한다.

**bitstream을 바꿨을 때**
1. §2로 빌드하고 timing을 확인한 뒤 보드로 옮긴다.
2. `probes.sh`(또는 `gemmprobe`, `runbench`)로 기본 동작을 확인한다.
3. `BIN=<새 파일>` 또는 `SYS_BIN=<새 파일>`로 golden 확인과 측정을 한다.
4. 통과하면 bit dir 이름에 `<clock>_pass_`를 붙이고 README를 쓴다. README에는 맞춰야 하는 software(vtalib commit, cmake 옵션, buffer 크기)를 적는다.

**새 모델을 더할 때**
1. ONNX를 준비한다(`fold_onnx.py`로 모양 고정).
2. `gen_mobilenetv2_bundle.sh`나 `gen_deit_bundle.sh`를 본떠 스크립트를 만든다. profile과 model-compiler를 직접 부르는 방식이다.
3. 요약(§3.4)에서 CPU로 간 GEMM과 합쳐지지 않은 꼬리를 본다. `VTA_FUSION_LOG=1`로 자세히 보고 하나씩 줄인다.
4. `vta/bundles/<Dir>`의 main, `vtabundles_xcmpl/CMakeLists.txt`에 target을 더하고, 이 문서 §3.1에 한 줄을 더한다.
