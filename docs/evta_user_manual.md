# EVTA on ZCU102 사용 설명서 (명령과 옵션)

bitstream 만들기부터 bundle 생성, 보드 빌드, 실행, 측정까지의 명령을 모았다. 어느 기계의 어느 dir에서 실행하는지, 옵션의 뜻을
적는다. 지금까지 한 일의 요약은 [evta_overview.md](evta_overview.md)에 있다.

## 0. 기계와 dir

| 기계 | 위치 | 하는 일 |
|---|---|---|
| 호스트 PC (x86, Ubuntu 20.04) | `/home/ubuntu/npuprg/nestc2_clion/nest-compiler` (git `main`) | 코드 수정, commit / push, bitstream 합성(docker) |
| PC의 docker `vivado2020-build` | 같은 경로가 mount됨, user `ubuntu` | Vivado / Vivado HLS 2020.1 (`/tools/Xilinx/v2020.1/`) |
| ZCU102 보드 (`xilinx@192.168.20.2`) | `~/JO/npuprg/nestc2_clion/nest-compiler` | bundle 생성(simulator 빌드), 보드용 빌드, 실행 |

- **PC와 보드의 소스는 git으로만 맞춘다.** PC에서 push하고 보드에서 `git pull`한다. submodule `vta/vtalib`는 그 안에서 따로
  `git pull origin main`한다. scp로 소스를 덮지 않는다.
- bitstream 파일만 scp로 보낸다(§1.3).

```bash
# 보드에서 (최신으로)
cd ~/JO/npuprg/nestc2_clion/nest-compiler && git pull && (cd vta/vtalib && git pull origin main)
```

| 소스 dir | 내용 |
|---|---|
| `lib/Backends/VTA/` | backend, `VTASave.cpp`가 bundle 코드를 만든다(옵션은 §2.2) |
| `include/Backends/VTA/VTASchedules.h` | conv tile schedule |
| `vta/vtalib/` (submodule) | EVTA runtime과 bundle 함수(HLS GEMM bitstream용) |
| `vtalib-zcu102-systolic/` | 같은 runtime의 systolic bitstream용 사본(uop 순서만 다름). vtalib을 고치면 둘 다 고친다 |
| `vta-hw-zcu102-single/`, `-multi2/`, `-multi4/`, `-systolic/` | bitstream 소스(HLS, RTL, Vivado tcl) |
| `vtabundles_xcmpl/` | 보드에서 bundle을 빌드하고 실행하는 독립 CMake 프로젝트와 스크립트 |

## 1. bitstream 만들기 (PC, docker)

### 1.1 명령

```bash
docker start vivado2020-build                 # PC 재부팅 뒤에는 꺼져 있다
docker exec -it -u ubuntu vivado2020-build bash
source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh
cd /home/ubuntu/npuprg/nestc2_clion/nest-compiler/vta-hw-zcu102-systolic/hardware/xilinx
make ip CSIM=1 SYSTOLIC=1                      # HLS: C simulation + 4개 IP 합성 (약 10분)
make bin SYSTOLIC=1 FREQ=400 JOBS=4 IMPL_STRATEGY=Performance_ExtraTimingOpt BUILD_TAG=<tag>   # Vivado (약 40분)
```

- 다른 copy도 같은 방법이다(`vta-hw-zcu102-single`, `-multi2`, `-multi4`의 `hardware/xilinx`).
- **`JOBS=4`로 돌린다.** PC가 Vivado 전체 부하 중에 세 번 꺼졌다(10-25, 10-28, 11-02). 돌릴 때 온도를 남기려면
  `/home/ubuntu/npuprg/templog/templog.sh`를 백그라운드로 띄운다(5초마다 CPU 온도, load를 디스크에 기록).
- 오래 걸리므로 PC 셸에서 `nohup docker exec -u ubuntu vivado2020-build bash -c "source ... && cd ... && make ..." > <log> 2>&1 &`로 돌려도 된다.

### 1.2 make 옵션

| 옵션 | 기본 | 뜻 |
|---|---|---|
| `SYSTOLIC=1` | 0 | GEMM을 RTL systolic 어레이 `gemm_sa`로(`-DVTA_SYSTOLIC`, dir 이름에 `_sa`). `vta-hw-zcu102-systolic`, `-multi4`에서 |
| `FREQ=<MHz>` | copy 기본값 | EVTA PLL 클럭(dir 이름에 `_<FREQ>mhz`) |
| 환경 `VTA_HP_FREQ` | 333 | 333 MHz보다 높게 빌드할 때 HP 포트 쪽 클럭(`hp_pll`). PS8 HP 포트는 333 MHz가 한계다 |
| `PROFILE=1` | 0 | compute 카운터와 AXI perf monitor(`_prof`). 보드에서는 vtalib `-DNESTC_EVTA_PROFILE_AXI_MONITOR=ON`과 같이 쓴다 |
| `MEM_OREG=1` | 0 | compute의 inp/wgt BRAM 읽기에 출력 register(`_oreg`) |
| `CSIM=1` | 0 | `make ip` 전에 HLS C simulation(`sim/vta_test.cc`) |
| `JOBS=<n>` | 8 | Vivado 동시 run 수(`launch_runs -jobs`). **4 권장** |
| `IMPL_STRATEGY=<이름>` | (기본 전략) | Vivado implementation 전략. systolic 400 MHz는 `Performance_ExtraTimingOpt` |
| `BUILD_TAG=<tag>` | 없음 | vivado dir과 export 이름 끝에 `_<tag>`. 같은 설정의 다른 판을 구분할 때 |
| `EXPORT_NAME=<이름>` | `vtahw_<dir 이름>` | 출력 파일 이름 |
| 환경 `VTA_BRAM_FENCE` | `y219` | 이 보드의 결함 행(SLICE Y219)을 피하는 방법: `y219`, `upper`, `none`. 이 보드에서는 바꾸지 않는다 |

### 1.3 결과 확인과 보드로 옮기기

- 출력: `<copy>/build/hardware/xilinx/vivado/<dir>/export/vtahw_<dir>.{bin,bit,hwh,xsa,timing.rpt,utilization.rpt}`
- **timing:** `*.timing.rpt`의 Design Timing Summary에서 WNS, WHS, WPWS가 모두 0 이상인지 본다.
  - 실패 path를 묶어 보려면 routed checkpoint(`vta.runs/impl_1/vta_wrapper_routed.dcp`)를 Vivado로 열고
    `get_timing_paths -max_paths 3000 -nworst 1 -slack_lesser_than 0`의 시작점과 끝점을 센다.
- **보드로 옮기기:** dir 이름 규칙은 `bit/<목적>_<YYMMDD>`이고, 보드에서 통과한 판은 `<clock>_pass_`를 붙인다.

```bash
# PC에서
ssh xilinx@192.168.20.2 mkdir -p /home/xilinx/JO/npuprg/bit/<목적>_<YYMMDD>
scp <export>/vtahw_<dir>.{bin,bit,hwh,timing.rpt,utilization.rpt} xilinx@192.168.20.2:/home/xilinx/JO/npuprg/bit/<목적>_<YYMMDD>/
# 보드에서
sudo cp /home/xilinx/JO/npuprg/bit/<목적>_<YYMMDD>/vtahw_<dir>.bin /lib/firmware/
```

- `.bit`만 있으면 `vtabundles_xcmpl/hwtest/bit2bin.py <x>.bit`로 `.bin`을 만든다.
- JTAG(ILA, hw_server)를 쓰기 전에는 보드가 부팅될 때마다 `sudo vtabundles_xcmpl/hwtest/nocpuidle.sh`를 먼저 실행한다. 안 하면 Linux가 멈춘다.

## 2. bundle 소스 만들기 (보드, simulator 빌드 `Debug2_remote`)

### 2.1 명령

```bash
cd ~/JO/npuprg/nestc2_clion/nest-compiler/Debug2_remote
./build_sim_bundles.sh [target ...]           # 예: ./build_sim_bundles.sh vtaMxnetResnet18Bundle
```

- 출력은 `Debug2_remote/vta/bundles/<Dir>/`, 로그는 `bundle_logs/<target>.log`(요약 `summary.txt`)에 남는다.
- **새 build dir을 만들지 않는다.** 메모리가 3.9 GB라 처음부터 빌드하면 메모리가 모자란다. `make -j2`까지만 쓴다.
- cmake를 다시 돌리면(CMakeLists 변경, 날짜가 바뀐 뒤 첫 빌드) Glow 거의 전체가 다시 빌드된다(약 1시간).
- Resnet18 conv0을 EVTA에서 돌리려면 `cmake -DVTA_RESNET18_WITH_SKIPQUANT0=OFF .`로 한 번 설정한다(지금 기본).
- **multi4 Resnet18 copy 4개**는 VTASaverTest를 거치지 않고 `model-compiler`로 따로 만든다. 그래서 backend를 바꾸면 이것도 다시
  만들어야 한다. 플래그는 `vta/multievta_bundles/MultiEVTAResnet18Test/CMakeLists.txt`에 있고, copy i는
  `-index-multi-evta=$((1<<i))`, `-network-name=mxnet_exported_resnet18_<i>`다. 결과를
  `Debug2_remote/vta/multievta_bundles/MultiEVTAResnet18Test/`에 둔다(`vtabundles_xcmpl/README.md` "Multi-EVTA bundle sets").

### 2.2 VTA backend 옵션 (`model-compiler`, `VTASave.cpp`)

기본으로 꺼진 것만 표에 표시했고, 나머지는 기본으로 켜져 있다. `=false`를 주면 그 단계 이전의 경로로 bundle을 만든다(비교나 문제 가르기용).
모델마다 다른 옵션은 그 모델의 생성 규칙에 적어 둔다(MobileNetV2: `vta/bundles/MobileNetV2Test/CMakeLists.txt`,
`vtabundles_xcmpl/gen_mobilenetv2_bundle.sh`). 실행 때 줄 것은 없다(생성된 코드에 들어간다).

생성된 bundle `.cpp` 맨 앞에는 아래 표의 옵션(과 `-enable-channelwise`) 값이 기본값까지 주석으로 적힌다(2026-11-08).
bundle을 어떤 옵션으로 만들었는지는 그것으로 확인한다. 예: `head -14 hw/MobileNetV2Test/*.cpp`
```
// Generated by the NEST-C VTA backend with:
//   -vta-keep-layout=1
//   ...
//   -vta-evta-depthwise=1
//   -vta-round-shift=1
```

| 옵션 | 켜면 하는 일 | 자세히 |
|---|---|---|
| `-vta-keep-layout` | 층 사이 activation을 EVTA layout N{C/16}HW16으로 둔다(층마다 transpose 없음) | compare §7.1 |
| `-vta-fuse-residual` | conv 뒤의 elementadd + relu를 같은 EVTA 호출에서(`convolution_wo_tr_res`) | §7.2 |
| `-vta-evta-maxpool` | EVTA layout max pool을 EVTA에서(`maxpool_evta`) | §7.3 |
| `-vta-evta-fc` | fc를 EVTA에서, weight와 bias를 compile time에 EVTA layout으로(`fullyconnected_evta_packed`) | §7.8 |
| `-vta-fuse-preprocess` | 입력 quantize, 채널별 상수 elemsub/elemdiv, NCHW→NHWC를 conv0 입력 복사 하나로 | §7.9 |
| `-vta-conv0-pack` | 채널이 적은 첫 conv를 packed 입력의 ceil(KH/s)×1 conv로(Resnet18: 7×7/2 → 4×1, 48채널) | §7.12 |
| `-vta-fuse-pool` | packed conv0 뒤의 EVTA max pool 3×3/2/1을 conv0 호출 안에서(`convolution_wo_tr_pool`) | §7.13 |
| `-vta-pad-channels` | 채널 수가 16의 배수가 아닌 conv / add activation을 0 채널로 넓혀 그 conv들을 EVTA로(MobileNetV2 24채널) | §7.20 |
| `-vta-round-shift` (기본 **꺼짐**) | EVTA conv의 출력 shift를 반올림(bias에 2^(shift−1)); 켜면 모든 EVTA conv 출력이 바뀐다. MobileNetV2Test만 켬 | §7.20 |
| `-vta-evta-depthwise` | depthwise channelwise conv(EVTA layout, 2의 거듭제곱 scale)를 EVTA에서(`dw_convolution_evta`, 대각 weight, 채널별 shift, round half up). 채널별 scale depthwise(`-enable-channelwise-depthwise`)에만 걸리므로 2026-11-08부터 기본 켜짐; 4코어 CPU 쪽이 조금 빠를 때는 `=0`. HLS용 vta/vtalib에서는 같은 산술을 CPU로 | §7.21 |
| `-enable-channelwise-depthwise` (기본 꺼짐, quantizer) | depthwise(group > 1) conv만 채널별 scale로 양자화, vtalib `cw_convolution`(CPU)으로 | §7.20 |
| `-index-multi-evta=<mask>` | bundle을 EVTA 하나에 고정(1, 2, 4, 8 → EVTA 0..3) | README |

조건이 맞지 않는 층(채널 수, offset, scale 비, 모양)은 옵션이 켜져 있어도 이전 경로(CPU 함수나 따로 부르는 EVTA 호출)로 간다.
"§"는 [evta_runtime_compare.md](evta_runtime_compare.md)의 절이다.

## 3. 보드용으로 빌드 (보드, `vtabundles_xcmpl/`)

```bash
cd ~/JO/npuprg/nestc2_clion/nest-compiler/vtabundles_xcmpl
./collect_hw_bundles.py [--set multievtax2|multievtax4] [Dir ...]   # Debug2_remote의 소스를 hw[_<set>]/<Dir>/로 복사, run_all.sh 다시 생성
cd build_sysprof && make -j2 vtaMxnetResnet18Bundle                   # 또는 make -j2 hw_bundles (전부)
```

- `collect_hw_bundles.py`는 `hw/script/run_all.sh`를 다시 만든다. git의 파일과 달라지면 보드에서 `git stash` 후 pull한다
  (보드에서는 `git checkout`으로 되돌리지 않는다).

build dir마다 cmake 옵션이 정해져 있다. 처음 한 번만 `mkdir <dir> && cd <dir> && cmake <옵션> ..`으로 만든다(git에 없음).

| build dir | cmake 옵션 | 실행 파일 끝 | 무엇을 재나 |
|---|---|---|---|
| `build/` | (없음) | — | 기본 HLS bitstream, golden 확인 |
| `build_prof/` | `-DNESTC_EVTA_PROFILE=ON -DBUNDLE_EXE_SUFFIX=_prof` | `_prof` | EVTA run마다 시간과 GOPS |
| `build_timed/` | `-DBUNDLE_TIMED=ON -DBUNDLE_EXE_SUFFIX=_timed` | `_timed` | bundle 문장마다 wall time |
| `build_sysprof/` | 위 `_prof` + `-DVTALIB=<root>/vtalib-zcu102-systolic`, 끝 `_sysprof` | `_sysprof` | systolic bitstream에서 run마다 |
| `build_systimed/` | 위 `_timed` + systolic vtalib, 끝 `_systimed` | `_systimed` | systolic bitstream에서 문장마다 |
| `build_multievtax2[_prof\|_timed]/` | `-DBUNDLE_SET=multievtax2` (+ 위 옵션) | | EVTA 2개 |
| `build_multievtax4[_prof\|_timed\|_sys\|_sysprof\|_systimed]/` | `-DBUNDLE_SET=multievtax4 -DNESTC_EVTA_MULTI=ON -DNESTC_EVTA_MAX_XFER=4194304` (+ 위 옵션) | | EVTA 4개 |

- `BUNDLE_SET`은 buffer 크기와 queue 크기를 정한다(single 16/16/19/18, multi 16/15/18/17).
- `VTALIB`는 vtalib 소스 dir이다. 기본은 `vta/vtalib`이고, systolic bitstream에는 `vtalib-zcu102-systolic`을 쓴다.
- vtalib의 헤더(`cmd_queue.h` 등)만 바꾸면 의존성이 잡히지 않을 수 있다. 결과가 이상하면 그 build dir의 vtalib을 다시 빌드한다.

## 4. 실행과 측정 (보드, sudo)

### 4.1 실행 전에

- **다른 사용자의 EVTA 실행을 확인한다:** `ps aux | grep -E "[r]un_prof|[r]un_all|Bundle"`. run 스크립트는
  `/tmp/evta_run.lock`으로 한 번에 하나만 돈다. 다른 사람의 시간 측정 중에는 빌드나 실행으로 끼어들지 않는다.
- **sudo는 `sudo -n /bin/sh -c "..."` 형태로** 쓴다(`sudo VAR=... cmd`는 비밀번호를 묻는다).
- **실행마다 bitstream을 다시 올린다.** 멈춘 run이 있으면 EVTA가 deadlock 상태로 남는다. run 스크립트는 자동으로 올리고, 손으로는
  `sudo vtabundles_xcmpl/hw/script/load_bit.sh <bin>`이다. 올린 이름은 `/tmp/evta_bitstream`에 남는다.
- **CMA:** 비정상 종료한 프로세스의 CMA(128 MB 중)는 `xlnk_reset` 전까지 돌아오지 않는다.
  손으로 할 때는 `sudo -n /bin/sh -c "cd /tmp && python3 -c 'from pynq import Xlnk; Xlnk().xlnk_reset()'"`이다.
  **다른 사람이 EVTA를 쓰는 중에는 하지 않는다**(그 사람의 CMA도 지운다). 남은 양은 `grep CmaFree /proc/meminfo`로 본다.
- ssh로 오래 걸리는 일을 걸 때는 `nohup <cmd> > <log> 2>&1 < /dev/null &`를 쓴다(`< /dev/null`이 없으면 ssh가 끝나지 않는다).

### 4.2 golden 확인: `run_all.sh`

```bash
cd ~/JO/npuprg/nestc2_clion/nest-compiler/vtabundles_xcmpl
sudo hw/script/run_all.sh [Dir ...]                     # single, build/의 실행 파일, 결과 hw/result/results.txt
sudo hw_multievtax4/script/run_all.sh                    # multi4 (multi2도 같은 형태)
sudo /bin/sh -c "BIN=<file in /lib/firmware> hw/script/run_all.sh Resnet18Test"   # 다른 bitstream으로
```

- 기본 bitstream은 `run_all.sh` 맨 위의 `BIN`(HLS 380 MHz)이다. `SYS_BIN`(systolic 400 MHz)은 `run_prof.sh -s _sys*`가 쓴다.
- `OMP_NUM_THREADS` 기본값은 1이다.

### 4.3 측정: `run_prof.sh`

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
| `-t <n>` | `OMP_NUM_THREADS`(기본 1). single은 `-t 4`로 4코어 |
| 환경 `BIN=<file>` | bitstream 바꾸기 |
| `-h`, `--help` | 옵션 설명을 출력하고 끝난다(sudo 없이 된다). `run_all.sh --help`, `load_bit.sh --help`도 있다 |

- **실행 시작 때 출력(2026-11-04):** 어느 bitstream으로 도는지와 그 설정을 먼저 출력하고, summary 파일 맨 앞에도 같은 내용을 넣는다.
  `run_all.sh`와 `load_bit.sh`도 bitstream 부분을 출력한다(`vtabundles_xcmpl/bininfo.sh`). 나오는 항목은 다음과 같다.
  - bitstream: 파일 이름, 어디서 왔는지(`run_all.sh`의 `BIN` / `SYS_BIN`, 또는 환경 변수 `BIN=`), 날짜, md5 앞 8자리
  - 이름에서 읽은 것: EVTA 수, clock, GEMM 종류(HLS / systolic), 기능(ALU imm II = 1, bias reset, requantize, AXI perf monitor). vtalib이 읽는 규칙과 같다
  - 실행 설정: `EVTA_*` / `MULTI_*` / `BUNDLE_*` 환경 변수, `-s`의 build dir과 무엇을 재는지, 횟수, lockstep, `chrt`, `OMP_NUM_THREADS`, Dir 목록
  - 경고: `-s`와 bitstream이 맞지 않으면 `WARNING`을 출력한다(`_sys*` 실행 파일을 HLS bitstream에서 돌리는 경우, 또는 그 반대).
- **결과:**
  - 로그는 `hw/<Dir>/<suffix>.log`이고, `_prof` 로그에는 `prof_summary.py` 요약(`<log>.sum`)이 붙는다.
  - `_timed`는 `timing.csv`를 남긴다.
  - Dir마다 한 줄씩 `hw/result/<suffix>_summary.txt`에 남는다(exit, golden, 전체 시간).
- multi-EVTA 실행 파일은 copy마다 표를 따로 출력한다. `multi4conv4Bundle <mask> [iters] [conv]` 같은 GOPS 측정은
  [vtabundles_xcmpl/README.md](../vtabundles_xcmpl/README.md)를 본다.

## 5. runtime 환경 변수 (vtalib, 실행할 때)

| 변수 | 기본 | 뜻 |
|---|---|---|
| `EVTA_REPLAY` | 1 | EVTA 명령 record / replay. `0`이면 매번 명령을 새로 만든다 |
| `EVTA_REPLAY_MB` | 16 | EVTA마다 replay 사본에 쓰는 메모리 상한 |
| `EVTA_REPLAY_LOG` | 0 | `1`: 기록 목록을 출력. `2`: replay된 run마다 `EVTA_RUN` 줄(GEMM/ALU 반복, DRAM 바이트, µs) |
| `EVTA_REPLAY_DUMP=<dir>` | 없음 | 기록된 run마다 명령과 uop을 텍스트로(`rec_<id>_<seg>.txt`) |
| `EVTA_MEMEXP=<mask>` | 0 | 실험용: replay의 inp/wgt/acc LOAD와 STORE(bit 0~3)를 1 element로 줄인다. **결과는 틀린다** |
| `EVTA_DUMP_CONV=<dir>` | 없음 | EVTA conv와 maxpool 출력을 모두 `conv_<n>.bin`으로. 변경 전후 byte 비교용 |
| `EVTA_RES_CHECK=1` | 0 | fused 경로(residual, conv+pool)를 따로 돈 결과와 실행 중에 비교해 불일치 수를 출력 |
| `EVTA_BUF_POOL_MB` | 32 | buffer pool 상한. `0`이면 pool을 끈다 |
| `EVTA_RUN_TIMEOUT_MS` | 5000 | run 하나가 이 시간을 넘으면 모든 EVTA 상태와 명령을 `/tmp/evta_hang_*`에 남기고 exit 5. `0`은 무제한 |
| `EVTA_NO_BIT_CHECK=1` | 0 | 올라간 bitstream의 EVTA 수 검사를 건너뛴다(위험: EVTA 수가 모자라면 SError) |
| `EVTA_NO_DSB=1` | 0 | cache flush 뒤 `dsb sy`를 뺀다(multi4 멈춤 재현용) |
| `EVTA_PRE_RUN_US`, `EVTA_PRE_RUN_MASK`, `EVTA_PRE_RUN_AT` | 0 | 멈춤 추적용: run 앞 대기 시간(µs), 대기할 EVTA mask, 대기 위치 |
| `EVTA_STORE_SENTINEL`, `EVTA_DOUBLE_QUEUE` | 0 | 멈춤 추적용: STORE 영역 표식, 명령 buffer 번갈아 쓰기 |
| `EVTA_FC_PROBE=1` | 0 | fc EVTA 경로 bring-up 시험(결과는 틀린다) |
| `EVTA_BIAS_RESET=0\|1` | bitstream 이름 | bias reset 강제(이름에 `bias`나 `_rq`가 있으면 켜짐). [evta_isa_ext.md](evta_isa_ext.md) |
| `EVTA_REQUANT=0\|1` | bitstream 이름 | out 쓰기 requantize 강제(이름에 `_rq`가 있으면 켜짐). 기능이 없는 bitstream에서 1이면 결과가 틀린다 |
| `MULTI_MASK=<bits>` | 모두 | multi4 Resnet50 / VGG16 실행 파일(`multi_threads_main.cpp`): 이 copy만 돌린다(copy i = EVTA i, 예 `0x6` = EVTA 1, 2; `evta_runtime_compare.md` §7.19 ⑧) |
| `EVTA_CONST_SHARE=0` | 1 | load_module의 상수 VTA buffer(weight, bias, 표)를 copy마다 따로. 기본은 같은 내용이면 process 안에서 하나로 공유(multi-EVTA copy, `VTAConstBufferAlloc`, `evta_runtime_compare.md` §7.19 ⑦) |
| `EVTA_INPUT_STREAM=0` | 1 | systolic vtalib: conv0 입력(`preprocess_conv0pack_stream`)을 conv 전에 한 번에 채운다. 기본은 타일 행마다 run을 나눠 EVTA가 앞 행을 계산하는 동안 다음 행을 채움(`evta_runtime_compare.md` §7.19 ⑥) |
| `EVTA_MIN_TILE=<n>` | bitstream 이름 | systolic vtalib: 가장 작은 conv 출력 tile(작으면 더미로 채움). 이름에 `_sa_`면 1, 아니면 7. HLS GEMM bitstream에서 7 미만이면 3×3 conv가 틀릴 수 있다 |
| `EVTA_SCHED=<H>,<W>,<C>,<KN>,<K>,<stride>:<vt>,<tileH>,<tileW>[;...]`, `EVTA_SCHED_LOG=1` | 없음 | systolic vtalib: conv 모양의 schedule을 실행 중에 바꾼다(bundle 없이 schedule 찾기), conv마다 모양과 schedule 출력. [evta_runtime_compare.md](evta_runtime_compare.md) §7.19 |
| `MULTI_RUNS`, `MULTI_LOCKSTEP` | 25, 0 | multi-EVTA main의 copy당 추론 수, lockstep(`run_prof.sh -n`, `-l`이 설정) |
| `BUNDLE_RUNS` | | `_prof` / `_timed` 실행 파일의 추론 수(`run_prof.sh -n`이 설정) |

## 6. 하드웨어 시험 (`vtabundles_xcmpl/hwtest/`)

```bash
cd ~/JO/npuprg/nestc2_clion/nest-compiler/vtabundles_xcmpl/hwtest
SUFFIX=_rb ./build.sh runbench                 # vtalib ZCU102 -D 세트로 빌드 (BUFF, UOP, ISA 등은 build.sh 머리말)
sudo ./runbench_rb base 0 1 aluimm 16 2048 load 16 4096 ...   # 여러 case를 한 프로세스에서 (REPS=200)
sudo ./gemmprobe ident                         # 새 bitstream의 GEMM 기본 확인 (reset, zero, ident, perm, ones)
```

- **`runbench`:** run 하나의 고정 비용(약 4.7 µs)과 명령 하나, 반복 하나의 비용을 잰다.
  - case: `base`, `gemm N K`, `gemmu N K`, `aluimm N K`, `alureg N K`, `load N K`, `store N K`
  - 프로세스 하나에서 여러 case를 돌린다. 프로세스를 여러 번 띄우면 CMA가 새어 할당이 실패한다.
- 다른 bitstream을 쓰기 전에는 `rawtest*` 프로브로 ISA 비트폭부터 확인한다([vta_synth_notes.md](vta_synth_notes.md) §2.4).

## 7. 그 밖의 도구

| 일 | 어디서 | 명령 |
|---|---|---|
| 날짜별 일지 생성 | PC | `docs/dailyreport/tools/daily_report.sh [YYMMDD ...]` (cron 매일 07:00, 07:00 경계, 로그 `~/.cache/daily_report.log`) |
| bitstream만 HLS C 모델로 시험 | PC docker | `make ip CSIM=1 ...` (§1) |
| systolic RTL 단위 시험 | PC docker | `vta-hw-zcu102-systolic/hardware/xilinx/rtl/tb/run_xsim.sh <vector dir>` ([evta_systolic_design.md](evta_systolic_design.md)) |
| PC 온도 기록 | PC | `nohup /home/ubuntu/npuprg/templog/templog.sh &` → `templog/temp_<날짜_시각>.log` |

## 8. 자주 하는 일의 순서

**vtalib(runtime)만 바꿨을 때**
1. PC에서 `vta/vtalib`과 `vtalib-zcu102-systolic`을 같이 고친다.
2. `vta/vtalib`(submodule)을 commit하고 push한다. main에서 submodule 포인터와 systolic 사본을 commit하고 push한다.
3. 보드에서 pull한다(main, 그다음 `vta/vtalib` 안).
4. `vtabundles_xcmpl/build_sysprof`에서 `make -j2 <target>`으로 빌드한다.
5. `run_prof.sh -s _sysprof`로 측정한다. golden 확인은 `run_all.sh`로 한다.
6. 결과가 같은지는 `EVTA_DUMP_CONV`로 변경 전후 출력을 비교한다.

**backend(`VTASave.cpp`)를 바꿨을 때**
1. 보드 `Debug2_remote`에서 `make model-compiler` 등으로 다시 빌드한다(`-j2`).
2. `./build_sim_bundles.sh <target>`로 bundle을 만든다. multi4 copy는 `model-compiler`로 다시 만든다(§2.1).
3. `collect_hw_bundles.py`로 복사한 뒤, 위 4~6단계를 한다.

**bitstream을 바꿨을 때**
1. §1로 빌드하고 timing을 확인한 뒤 보드로 옮긴다.
2. `gemmprobe`, `runbench`로 기본 동작을 확인한다.
3. `BIN=<새 파일>` 또는 `SYS_BIN=<새 파일>`로 golden 확인과 측정을 한다.
4. 통과하면 bit dir 이름에 `<clock>_pass_`를 붙이고 README를 쓴다(맞춰야 하는 software: vtalib commit, cmake 옵션, buffer 크기).
