# EVTA 4개 bitstream에 ALU 확장 3단계 넣기 (2026-11-22)

EVTA 1개 bitstream에는 ALU 확장 3단계(`_alux3`)가 있다. 그런데 EVTA 4개(multi4) bitstream은 아직 11-04의 `_rq` 판이다.
- ALU 확장이 없으면 MobileNetV2의 residual add / avgpool, DeiT의 layer norm / softmax / elemadd가 CPU에서 돈다.
- 그래서 multi4는 장당 시간이 EVTA 1개보다 길다. 예: MobileNetV2 17.2 대 14.4 ms.

이 문서는 다음을 적는다.
- alux3를 EVTA 4개에 넣으려다 칩이 모자랐던 것.
- 무엇을 견주어 어떤 방법을 골랐는지.
- 지금 상태와 명령.

[evta_todo.md](evta_todo.md) 순서 7이 이 작업이다. 회로 설명은 [evta_alux_hw.md](evta_alux_hw.md)에 있고, ISA와 bitstream 목록은 [evta_isa_ext.md](evta_isa_ext.md) §8에 있다.

용어:
- **LUT**: FPGA의 기본 논리 칸. 조합 논리와 작은 RAM(LUTRAM)이 여기에 들어간다.
- **FF**: flip-flop, 1비트 레지스터.
- **BRAM**: 36 Kb 블록 RAM.
- **WNS**: 가장 나쁜 경로의 setup 여유(ns). 0 이상이어야 그 클럭으로 동작한다.
- **II**: HLS 루프가 몇 클럭마다 새 반복을 시작하는지.
- **opt_design / place / route**: Vivado 구현의 논리 최적화 / 배치 / 배선 단계.

## 1. 문제: LUT가 모자란다

첫 판의 build 명령: `m4_build.sh alux3 SYSTOLIC=1 ALUX=1 FREQ=400 IMPL_STRATEGY=Performance_ExtraTimingOpt`.
- opt_design 뒤 LUT 287,909개가 필요했다.
- 칩(xczu9eg)의 LUT는 274,080개다. 연구실 보드의 결함 행(SLICE Y219)을 피해 배치할 수 있는 자리는 271,752개다.
- 그래서 place_design이 `DRC UTLZ-1 Slice LUTs over-utilized`로 멈췄다(`build/y219/place_malux3.log`).

**어디에 쓰였나 (opt_design 뒤, EVTA 하나당, `build/util_alux3_hier.rpt`)**

| 블록 | LUT | FF | BRAM36 | DSP | 하는 일 |
|---|---|---|---|---|---|
| `compute` (HLS) | **49,518** | 53,883 | 11 | 47 | GEMM 명령 해석, ALU(확장 포함), requantize, LUT 표 |
| `smc` (SmartConnect) | 10,219 | 15,396 | 0 | 0 | fetch / load / compute / store의 AXI 5개 → HP 포트 1개 |
| `gemm_sa` (RTL systolic) | 6,435 | 9,526 | 0 | 256 | 16 × 16 곱셈 배열 |
| `fetch` | 1,808 | 4,009 | 4 | 0 | 명령 읽기 |
| `load` | 1,336 | 2,418 | 2 | 0 | inp / wgt DMA |
| `store` | 805 | 1,524 | 2 | 0 | out DMA |
| `gemm_queue_rs` | 267 | 518 | 0 | 0 | 명령 queue 뒤 AXIS register slice (400 MHz용) |
| `hpslice` | 216 | 788 | 0 | 0 | HP 포트 앞 register slice |
| 합 (4개 + 공통) | 287,909 | 359,345 | 572 + 16 (RAMB18) | 1,212 | |

- compute가 EVTA 하나의 70 %다.
- compute 안은 HLS가 거의 한 덩어리로 펼친다(`(inst)` 34k). 그래서 어느 함수가 얼마인지는 HLS 보고서(`csynth.rpt`)로 본다.

## 2. 고려한 방법

17k LUT 이상(EVTA 하나당 4k 이상)을 줄여야 한다. 배치와 배선에 여유도 있어야 하므로 실제로는 더 줄여야 한다.

| 방법 | 기대 효과 (EVTA 하나당) | 판단 |
|---|---|---|
| ① 마지막에 넣은 register 빼기 (§8.13 `alu_reg`, queue register slice) | LUT 약 1k. 대부분 FF라 LUT는 적다 | 모자란다. 400 MHz 타이밍도 잃는다 |
| ② 클럭을 낮춰 HLS를 다시 합성 | 파이프라인 단이 줄어 FF가 준다. LUT는 거의 그대로 | 모자란다 |
| ③ DSP를 2배 클럭으로 돌려 곱셈을 2번씩 (외부 클럭을 낮춤) | 타이밍 여유가 생긴다. LUT는 줄지 않는다. 모자란 것은 LUT이고, ALU는 LUT 논리다 | 이번 문제에는 맞지 않는다. 타이밍이 막힐 때 다시 본다 |
| ④ **ALU의 겹친 경로 하나 빼기 (`ALUX_NOIMM`)** | compute 49.5k → 38.5k (−11k) | **채택** |
| ⑤ SmartConnect 대신 AXI Interconnect (`VTA_DATA_IC=1`) | SmartConnect 10.2k를 줄일 것으로 봤다 | 해 봤더니 더 나쁘다: LUT 245.1k(④만 243.9k), BRAM 580 → 742 |

### 2.1 ④ ALU의 겹친 경로 (`VTA_ALUX_NO_IMM_LOOP`)

`vta.cc` `alu()`에는 ALU 명령을 처리하는 루프가 둘 있었다.
- **imm 루프:** immediate(상수) 연산만 한다. op < 4(MIN / MAX / ADD / SHR)이고 상수 하나를 쓴다. II 1이고, 16 lane ALU와 requantize가 따로 있다. 11-03에 넣었다(§7.15).
- **slot 루프:** 확장 연산을 모두 한다(MUL, SUB, CPY, LUT, MADD, SQACC 등). II 1이고, 11-14의 `_alux2`(§8.14)에서 넣었다.
  - immediate 연산도 slot 하나로 한다. src 대신 상수를 broadcast한다.

따라서 slot 루프만 있어도 결과와 처리 속도(한 클럭에 하나)가 같다. 달라지는 것은 명령마다 파이프라인을 채우는 깊이뿐이다. slot 루프가 더 깊다.

- **스위치:** `make ... ALUX_NOIMM=1`이 HLS에 `-DVTA_ALUX_NO_IMM_LOOP`를 넘긴다(`scripts/hls.tcl`). IP 이름에 `_ni`가 붙는다. multi4와 systolic 소스 모두 같다.
- **HLS 추정:** compute LUT 87.4k → 70.1k. 구현 뒤에는 49.5k → 38.5k다.
- **C 시뮬레이션:** `vta-hw-zcu102-systolic/hardware/xilinx/sim/run_csim.sh` (`NOIMM=1`). GEMM / ALU / ALU 확장 무작위 프로그램을 참조 모델(`alu_ext_ref.h`)과 견줬고, 모두 통과했다(0 fail).
  - `PROG=1`(layer norm / softmax 프로그램)은 컴파일이 안 된다(`VTABufferFlush` 선언 없음). 이 변경 전부터 그렇다.
- **EVTA 1개 `_ni` bitstream** (성능 영향을 보려고 같이 만듦): `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_alux_ni_400mhz_rq_alux3.bin`.

  | | `_alux3` (11-15) | `_alux3` + `_ni` (11-22) |
  |---|---|---|
  | LUT | 69,764 (25.5 %) | **58,510 (21.3 %)** |
  | FF | 91,997 | 84,948 |
  | BRAM | 258 | 258 |
  | DSP | 303 | 298 |
  | WNS @ 400 MHz | +0.022 ns (Explore) | **+0.001 ns** (ExplorePostRoutePhysOpt) |

  - 보드 시험(ALU 시험, CNN dump 비교, 시간)은 아직 하지 않았다. 이름에 `_alux3`, `_rq`가 들어 있으므로 vtalib는 ALU 확장 3단계와 requantize를 켠다.

### 2.2 ⑤ AXI Interconnect (`VTA_DATA_IC=1`)

- `vivado.tcl`: `VTA_DATA_IC=1`이면 EVTA마다 `smc_<i>`를 `axi_interconnect:2.1`(NUM_SI 5, NUM_MI 1, STRATEGY 2)로 만든다.
  - S 쪽 클럭은 `evta_clk`, M 쪽은 HP 클럭이다.
- 결과(`m4_build.sh nic ...`): LUT 245,105, BRAM 742. SmartConnect 판보다 LUT가 많고 BRAM이 162개 늘었다. 클럭 변환 FIFO가 BRAM으로 간다.
- 쓰지 않는다. 기본은 SmartConnect 그대로이고, 스위치와 결과는 `vivado.tcl` 주석에 남겼다.

## 3. multi4 `_ni` 지금 상태

- **build 명령:** `m4_build.sh ni SYSTOLIC=1 ALUX=1 ALUX_NOIMM=1 FREQ=400 IMPL_STRATEGY=Performance_ExtraTimingOpt`.
- **opt_design 뒤:**
  - LUT **243,945 (89.3 %**, 배치 가능한 자리 271,752 기준), FF 331,133, BRAM 580 / 899.
  - EVTA 하나당 compute는 38.5k다.
- **배치·배선:** Y219 흐름 `run_flow.sh mni`를 쓴다(`build/y219`, 결함 행에 PIP를 쓰지 않는 RapidWright 재배선; [y219route/README.md](../vta-hw-zcu102-multi4/hardware/xilinx/scripts/y219route/README.md)).
  - placement는 끝났다(`placed_mni.dcp`, 11-22 21:58).
  - 배치 뒤 phys_opt의 추정 WNS는 −2.373 → −0.560 ns이고, hold 수정 중이다(11-23 01:00).
  - 배선(route), RapidWright 재배선, post-route phys_opt가 남았다.
- **결과 (11-23 01:14, 멈춤):** Vivado route_design(2시간 6분)이 **congestion level 6**으로 끝났다. `Route 35-447 Congestion is preventing the router from routing all nets`가 나왔다.
  - 배선 직후 WNS −5.03 ns, TNS −105,214 ns였다. post-route phys_opt 중에 −4.45 ns였다.
  - 혼잡한 곳은 위쪽 반(EVTA 2, 3)과 PS 옆이다.
  - 이 음수는 결함 행과 상관없는 칩 전체의 혼잡이다. RapidWright(행 Y219를 지나는 net만 다시 배선)나 MMCM(수십 ps)으로는 메울 수 없다. 그래서 RapidWright 단계 전에 멈췄다.
  - 11-04 `_rq` 판은 LUT 164k(60 %)로 같은 흐름이 400 MHz를 맞췄다.
- **다음 판 (11-23):**
  1. **같은 opt_design 결과로 placer만 `AltSpreadLogic_high`** (로직을 넓게 편다). `VTA_PLACE_DIRECTIVE=AltSpreadLogic_high run_flow.sh mnis`. 혼잡이 얼마나 줄어드는지 보는 기준이다.
  2. **SmartConnect 대신 crossbar (`VTA_DATA_XBAR=1`, tag `nix`):**
     - `smc_<i>`라는 같은 이름의 BD hierarchy 안에 다음을 둔다.
       - `axi_crossbar`: SAMD, 128비트, M00 issuing 8, S acceptance 4.
       - 읽기 전용 `axi_dwidth_converter` 2개: fetch 256 → 128, compute uop 64 → 128.
       - HP 클럭 쪽으로 `axi_clock_converter` 1개.
     - 이름이 같으므로 pblock, reset multicycle, 성능 모니터 연결이 그대로다.
     - AXI Interconnect 판(§2.2)은 입력마다 FIFO가 붙어 BRAM이 늘었다. 이번에는 clock converter가 출력 1개에만 있다.
     - 예상은 EVTA 하나당 10.2k → 3–4k LUT, 전체 약 215k(79 %)다.
  - register를 빼는 것은 하지 않는다. 대부분 FF이고(FF는 60 %만 씀) LUT는 EVTA 하나당 1–2k뿐인데, 400 MHz 타이밍을 잃는다.
  - ALU 기능을 빼는 것도 하지 않는다. 남은 연산은 DeiT / CNN이 쓴다.
- **결과 (11-23):**

  | 판 | LUT (opt_design 뒤) | Vivado 배선 뒤 WNS | Y219 행을 지나는 net | 결과 |
  |---|---|---|---|---|
  | `mni` (SmartConnect, Explore) | 243,945 (89.3 %) | −5.03 ns (congestion 6) | — | 멈춤 |
  | `mnis` (SmartConnect, `AltSpreadLogic_high`) | 같음 | — | — | **place_design 실패** ("Placer could not place all instances", 3시간 14분) |
  | `mnix` (crossbar, Explore) | **212,241 (77.7 %)**; EVTA 하나당 `smc` 2,273 (전 10.2k) | **−0.288 ns** (TNS −2,080, 배선 오류 0) | **1,922** (`_rq` 판은 1) | RapidWright가 다 못 풂 (아래) |

  - crossbar로 LUT 31.7k가 줄고 칩 전체 혼잡은 풀렸다(−5.03 → −0.288 ns).
  - 하지만 결함 행 근처(EVTA 1의 compute, 행 Y185–Y217)가 여전히 붐벼서, Vivado가 행 Y219의 PIP를 쓰는 net이 1,922개였다.
  - **RapidWright 1차** (`rws_mnix`, `crit=0.95`, 100회):
    - 반복 99회째에도 겹친 node가 4,671개 남았고, 연결 못 한 sink가 227개였다.
    - 1회차부터 "Unroutable connection"이 나왔다(대부분 행 Y196–Y217 slice의 CKEN / LUT 입력). 행을 막으면 우회로가 bounding box 밖으로 나간다.
    - `route_s.tcl`의 Vivado `route_design -nets`도 "20 node overlaps"로 실패해 bitstream이 없다.
  - ubuntu2에는 RapidWright timing 파일이 없어 처음에 예외로 멈췄다. jar 안의 `timing/`을 `~/tools/rapidwright/d2025.2/`에 풀어 해결했다.
  - `Y219RWRoute.java`에 `-Drwargs="<RWRoute 옵션>"`을 더했다. **2차** (`rws_mnix2`)는 `crit=0 --maxIterations 250 --enlargeBoundingBox`로 돌고 있다.
- **음수로 끝나면 할 순서 (몇 ps–수십 ps일 때):**
  1. post-route phys_opt(`route_s.tcl ... opt`)를 한다.
  2. 그래도 남으면 MMCM으로 클럭을 조금 낮춘다(`mmcm_set.tcl <dcp> <out> 11.875` = 395.8 MHz; 11-04 `_rq`가 이렇게 했다).
  3. 그래도 모자라면 placer directive를 바꾸거나(`VTA_PLACE_DIRECTIVE=ExtraNetDelay_high`), §2의 ①, ③을 본다.
- **나온 뒤 할 일:**
  - bitstream 이름에 `_4evta_sa_alux3_400mhz_rq`가 들어가게 한다(+ `_ni`). vtalib는 이름으로 기능을 켠다.
  - 보드에서 확인한다: hwtest probe 40/40, golden, CNN dump가 EVTA 1개와 byte 단위로 같은지.
  - multi4 MobileNetV2 / ResNet50 / DeiT를 다시 잰다([evta_dpu_compare.md](evta_dpu_compare.md) §6.1).
- **`m4_build.sh`의 문제:** 이번에는 make가 끝난 뒤 impl dir 변수(`I`)가 비어 `run_flow.sh`를 부르지 않았다(`build/m4_ni_driver.log`: `ls: cannot access '/vta.runs/impl_1/...'`). 그래서 아래처럼 직접 불렀다. 원인은 11-23에 고쳤다(§4.2).

## 4. 명령

```bash
# host (ubuntu2), vta-hw-zcu102-multi4/: multi4 alux3 + ni, opt_design까지 + Y219 흐름
hardware/xilinx/scripts/y219route/flow/m4_build.sh ni SYSTOLIC=1 ALUX=1 ALUX_NOIMM=1 FREQ=400 \
    IMPL_STRATEGY=Performance_ExtraTimingOpt > build/m4_ni_driver.log 2>&1 &
# m4_build.sh가 run_flow.sh를 부르지 않았을 때 (위 §3):
cd build/y219 && VTA_IMPL_DIR=../hardware/xilinx/vivado/zcu102_1x16_i8w8a32_16_15_18_17_4evta_sa_alux_ni_400mhz_ni/vta.runs/impl_1 \
    ../../hardware/xilinx/scripts/y219route/flow/run_flow.sh mni > ../m4_ni_flow.log 2>&1 &
# 같은 opt_design 결과로 placer만 바꿔 (build/y219에서)
VTA_IMPL_DIR=<위 impl_1> VTA_PLACE_DIRECTIVE=AltSpreadLogic_high ../../hardware/xilinx/scripts/y219route/flow/run_flow.sh mnis
# crossbar 판 (SmartConnect 대신)
hardware/xilinx/scripts/y219route/flow/m4_build.sh nix SYSTOLIC=1 ALUX=1 ALUX_NOIMM=1 VTA_DATA_XBAR=1 FREQ=400 IMPL_STRATEGY=Performance_ExtraTimingOpt
# docker 권한: host에서 docker가 sudo를 요구하면 PATH=$HOME/bin_sudo_docker:$PATH (sudo -n docker wrapper)
# AXI Interconnect 판 (비교용, 쓰지 않음)
hardware/xilinx/scripts/y219route/flow/m4_build.sh nic SYSTOLIC=1 ALUX=1 ALUX_NOIMM=1 VTA_DATA_IC=1 FREQ=400 IMPL_STRATEGY=Performance_ExtraTimingOpt

# EVTA 1개 _ni bitstream (vivado2020-build 컨테이너, vta-hw-zcu102-systolic/hardware/xilinx)
source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh
make bin JOBS=8 SYSTOLIC=1 ALUX=1 ALUX_NOIMM=1 FREQ=400 IMPL_STRATEGY=Performance_ExplorePostRoutePhysOpt BUILD_TAG=rq_alux3

# RapidWright 재배선만 다시 (build/y219에서, 배치 결과 routed_<tag>.dcp 재사용): run_flow.sh의 java 줄에
#   -Dcrit=0 -Drwargs="--maxIterations 250 --enlargeBoundingBox" 를 주고, 그 뒤 route_s.tcl
# ubuntu2에 RapidWright timing 파일이 없으면: cd ~/tools/rapidwright/d2025.2 && unzip -o ../rapidwright-2025.2.2-standalone-lin64.jar 'timing/*'

# 모듈별 사용량 (opt_design dcp에서; 컨테이너는 /tmp를 못 보니 출력은 /home 아래로)
vivado -mode batch -source build/util.tcl    # open_checkpoint .../impl_1/vta_wrapper_opt.dcp; report_utilization -hierarchical

# C 시뮬레이션 (vivado_hls 없이, PC)
vta-hw-zcu102-systolic/hardware/xilinx/sim/run_csim.sh            # 지금 소스
NOIMM=1 vta-hw-zcu102-systolic/hardware/xilinx/sim/run_csim.sh    # imm 루프를 뺀 판
```

- 결과 파일:
  - `build/hardware/xilinx/vivado/<CONF>/export/vtahw_<CONF>.{bit,bin,hwh}`
  - `*.timing.rpt`: WNS가 0 이상인지 본다.
  - `*.utilization.rpt`
- multi4의 bitstream은 `build/y219/rs_m<tag>.bit`이고, `bit2bin.py`로 `.bin`을 만든다.

## 4.1 ALU 확장 1단계 + crossbar multi4 (`ALUX_STEP1=1`, 11-23)

**왜**
- 11-04 `_rq` multi4는 LUT 164k(60 %)에서 빌드 한 번이 1–1.5시간이었고 Y219 행을 지나는 net이 1개였다.
- alux3 crossbar 판은 212k(77 %)에서 한 번이 약 8시간이었고, 행을 지나는 net이 1,922개라 RapidWright가 수렴하지 않았다.
- LUT가 는 것은 400 MHz용 register가 아니라 ALU 확장 회로다(EVTA 1개 bitstream, 배치·배선 뒤):

  | 판 | LUT | 앞 판보다 |
  |---|---|---|
  | `_alux` 380 MHz (1단계, 11-14) | 53.6k | |
  | `_alux` 400 MHz + register (§8.13 `alu_reg`, queue slice) | 55.5–55.7k | +2k (register) |
  | `_alux2` (slot 루프) | 67.8k | +12k |
  | `_alux3` | 69.8k | +2k |
  | `_alux3` + `_ni` | 58.5k | −11k |

- 폭 변환기(crossbar 판의 `dw0` 256 → 128 424 LUT, `dw2` 64 → 128 303 LUT)는 EVTA 하나당 727, 4개 2.9k로 작다. SmartConnect(10.2k)가 컸다.
- 예상: 1단계 + crossbar multi4 약 190k(69 %).

**무엇을 했나**
- `src/vta_alux1.cc`(multi4, systolic 두 트리): `7a128c5`(11-14)의 `vta.cc` 그대로. 그 뒤 RTL과 `vta.h`는 그대로이고 `hw_spec.h`는 필드만 늘었다.
  - 오늘 `VTA_ALU_FULL_OP`는 `alu_op_ext2`를 bit 4로 더한다. 이 파일은 `(ap_uint<4>)`로 잘라 연산 16–31을 해석하지 않는다. vtalib는 `_alux1`에 그런 명령을 보내지 않는다.
- `Makefile` `ALUX_STEP1=1`(`ALUX=1`과 함께): `hls.tcl`이 `vta_alux1.cc`를 쓰고 IP 이름은 `_alux1`이다. vtalib는 `_alux<k>`의 k로 단계를 읽는다(`VTAHasAluExt2`는 k ≥ 2).
- C 시뮬레이션: `sim/run_csim.sh STEP1=1`(연산 0–11만, `VTA_ALUX_OPS=12`; `alux_test.cc`에 `VTA_ALUX_OPS` 추가). 39 / 39 통과, 기본 소스도 39 / 39.

**같은 소스의 EVTA 1개 bitstream으로 보드 확인** (`..._sa_alux_400mhz_rq.bin`, 11-14; 지금 실행 파일 그대로)
- `run_prof.sh -s _systimed all`: 시험 bundle 모두 PASS. `_alux3`와 같다.
- `EVTA_EPI_CHECK` / `LN` / `SM` / `EA` / `RES` / `MATMUL_CHECK=1`: MobileNetV2 87, DeiT 180, Resnet18 33, Resnet50 57, VGG16 18개 모두 0 differ. 분류도 같다: MobileNetV2 282 (0.920282), DeiT 281 (0.719773).
- 시간 (OMP 1, `-n 10`, 번갈아 두 번):

  | 모델 | `_alux` (1단계) | `_alux3` | 차이 |
  |---|---|---|---|
  | MobileNetV2 (QAT) | 14.68 / 14.68 ms | 14.39 / 14.39 ms | +0.29 ms (+2 %) |
  | Resnet50 | 64.54 / 64.57 | 64.48 / 64.45 | 같음 |
  | Resnet18 | 22.78 / 22.73 | 22.71 / 22.72 | 같음 |
  | VGG16 CIFAR-10 | 6.76 / 6.76 | 6.75 / 6.75 | 같음 |
  | DeiT-tiny | 46.30 / 46.18 | 37.06 / 37.00 | +9.2 ms (+25 %: fused layer norm / softmax / elemadd가 없다) |

  - CNN에는 1단계로 거의 충분하다. DeiT만 2·3단계의 효과가 크다.

**빌드**
```bash
# host, vta-hw-zcu102-multi4/
PATH=$HOME/bin_sudo_docker:$PATH hardware/xilinx/scripts/y219route/flow/m4_build.sh x1 SYSTOLIC=1 ALUX=1 ALUX_STEP1=1 VTA_DATA_XBAR=1 FREQ=400 IMPL_STRATEGY=Performance_ExtraTimingOpt
# C 시뮬레이션 (vta-hw-zcu102-systolic/hardware/xilinx)
sim/run_csim.sh STEP1=1
```
- 11-23 12시에 시작했다(`build/m4_x1.log`, `build/y219/*_mx1.*`). 결과는 §4.2.

## 4.2 11-23 ~ 11-24에 해 본 것과 안 된 이유 (멈춤)

결론: alux3도 alux1도 multi4 400 MHz를 결함 행 Y219와 함께 맞추지 못했다. 11-24에 멈췄다. multi4 기본은 `_rq`에 ALU MUL만 더한 `_amul` 판(ubun PC에서 합성, §6.2)으로 하고, 이 판들은 쓰지 않는다.

**A. 결함 행 둘레를 비우기 (띠, `VTA_FENCE_ROWS="213 225"`, alux3 + `_ni` + crossbar의 opt_design 결과)**
- `place_d.tcl`이 SLICE 행 Y213–Y225를 PROHIBIT(1,261 site). `VTA_MMCM_MULT`로 PLL 배수도 바꾼다(`run_flow.sh`가 둘 다 컨테이너에 넘긴다).

  | 판 | 클럭 | Y219를 지나는 net | Vivado 배선 뒤 WNS | RapidWright (`crit=0`, 250회, `--enlargeBoundingBox`) |
  |---|---|---|---|---|
  | `mnix` (띠 없음, §3) | 400 MHz | 1,922 | −0.288 ns | 수렴 안 함 |
  | `mnixb` | 400 MHz | 199 | −0.452 ns | 돌리지 않음 (멈춤) |
  | `mnixc` | 379.2 MHz (배수 11.375) | 199 | −0.873 ns | 22회째 겹친 node 77k → 342k로 늚, 멈춤 |

- 띠로 행을 지나는 net은 1/10이 됐지만 빼앗긴 slice만큼 다른 곳이 붐벼 타이밍이 크게 나빠졌다. 클럭을 5 % 내려도 −0.87 ns다.

**B. ALU 확장 1단계 + crossbar (`x1`, §4.1), 400 MHz, 띠 없음**
- 배치·배선 뒤 LUT 190,609 (70.1 %). Vivado 배선 뒤 WNS −0.195 ns, Y219 행을 지나는 net **781**.

  | 시도 | RapidWright | Vivado 마무리 (`route_s.tcl`) | 결과 |
  |---|---|---|---|
  | `mx1` | `crit=0.95`, 100회: 행 net 0, sink 42개 연결 못 함 | `route_design -nets` 202 net: node 3개 겹침으로 실패 | bitstream 없음 |
  | `mx1b` | 300회: 같음 (42개) | 107 net, 실패 | bitstream 없음 |
  | `mx1c` | 300회 + `--enlargeBoundingBox`: **모두 연결** | 성공, 행 net 0 | WNS **−9.468 ns** (22,240 endpoint). 최악 경로 `gemm_sa_0/rmw/s4_reg` → `acc_mem_0` BRAM이 배선 11.5 ns: 넓은 bounding box로 먼 길을 돌았다 |
  | `mx1d` | `mx1b` 결과를 Vivado 전체 `route_design`(남의 net도 뜯음) → 행 net **306**, WNS −0.859 → 다시 RapidWright 300회: sink 35개 못 함 | 223 net 마무리 | 행 net **1**, WNS **−4.323 ns** |

- Vivado에 배선을 맡기면 행으로 돌아가고, RapidWright로 빼면 연결이 남거나 먼 길을 돈다. 반복해도 줄지 않았다.
- RapidWright(`Y219RWRoute.java`)는 설계의 클럭을 모른다(`crit`는 모든 연결에 같은 값). 행을 지나는 net이 수백 개면 우회가 타이밍을 깬다. `_rq` 판은 1개뿐이어서 됐다.

**C. 해 보지 않은 것 (다시 할 때)**
1. EVTA 0·1은 행 아래, 2·3은 위로 pblock을 나눠 행을 지나는 net을 HP / AXI 몇 개로 줄이고, 그 net에 register 한 단을 넣는다(지금은 crossbar, 폭 변환기, GEMM 내부 net도 행을 지난다).
2. Vivado timing 보고서의 slack으로 연결마다 criticality를 주는 RapidWright.
3. 클럭을 낮추는 것은 1과 함께일 때만 의미가 있다(`mx1b` / `mx1d`는 타이밍보다 배선 실패).

**도구 쪽에서 고친 것**
- `m4_build.sh`: host의 `docker`는 `sudo -n docker` wrapper라 make 프로세스가 root다. `kill -0 $MK`가 EPERM으로 실패해 감시 루프가 곧바로 끝났고, 그래서 `I`가 비었다(§3의 문제). `[ -d /proc/$MK ]`로 바꾸고 make 뒤에 `I`를 다시 찾는다.
- Vivado 컨테이너는 `/tmp`를 못 본다. tcl은 `/home` 아래(`build/y219`)에 둔다. `mx1d` 첫 실행이 이것으로 3초 만에 끝났다.
- 결과 파일: `build/y219/{routed,rws,rs,route_s}_m{nixb,nixc,x1,x1b,x1c,x1d}.*`(`rs_mx1c.bit`, `rs_mx1d.bit`는 쓰지 말 것), `vfix.tcl`(`mx1d`의 Vivado 단계).

## 5. 남은 일

1. ~~multi4 `_ni` 배선과 타이밍(§3)~~: 멈춤(§4.2). multi4 기본은 `_rq` + ALU MUL(`_amul`, §6.2), EVTA 1개 기본은 `_alux3`.
2. EVTA 1개 `_ni`의 보드 시험.
   - ALU 시험과 dump 비교를 한다.
   - CNN / DeiT 시간을 `_alux3`와 견준다. 명령마다 파이프라인을 채우는 시간이 늘어나는 만큼 차이가 날 수 있다.
   - 같으면 EVTA 1개도 `_ni`를 기본으로 할지 정한다.
3. ~~`m4_build.sh`의 `I` 변수 문제~~: 고침(§4.2).
4. ~~multi4 `_amul_bc`~~: 만들어 보드를 통과했지만 이득이 없다(§6.4). multi4는 `_amul`을 쓴다.
5. multi4 `run_all.sh` `SYS_BIN`을 `_amul`로 바꿀지 정하기(§6.2).

## 6. 다른 길: CNN에 필요한 MUL만 넣은 `_amul` (2026-11-23, PC ubuntu)

alux3 전체 대신 CNN이 실제로 쓰는 확장 연산만 넣었다.
- `EVTA_REPLAY_DUMP`로 센 결과, CNN이 `_alux3`에서 쓰는 확장 연산은 avgpool의 MUL immediate(op 4) 하나뿐이다. 나머지는 DeiT용이다.
- HLS `vta.cc` `VTA_ALU_MUL`(Makefile `ALUMUL=1`, 이름 `_amul`): imm 루프(II 1)에 MUL immediate만 더한다. 표 RAM, slot 루프, 다른 확장 연산은 없다.
- vtalib-systolic `VTAHasAluMul()`: 이름에 `_amul` 또는 `_alux`가 있으면 `avgpool_evta`를 EVTA에서 돌린다(`EVTA_ALUMUL=0|1`). bundle은 그대로다.
- 바꾸기 전 코드: tag `amul-before-261123`, `~/npuprg/archive/amul_before_261123/`.

### 6.1 EVTA 1개 `_amul`
`vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_amul_400mhz_rq.bin` (Explore, WNS +0.024 ns, LUT 43.4k: `_rq` 42.1k, `_alux3` 69.8k).
golden 30 PASS, avgpool `EVTA_RES_CHECK` 5 모델 0 differ. 보드에서 빌드한 `_systimed`, OMP 1, 25회 중앙값(ms):

| bundle | `_amul` | `_alux3` | `_amul` + `EVTA_ALUMUL=0` |
|---|---|---|---|
| MobileNetV2Test | 14.65 | 14.39 | 15.21 |
| MobileNetV2TfTest | 14.64 | 14.39 | 15.19 |
| Resnet50Test | 64.46 | 64.47 | 65.27 |
| Resnet50TvTest | 68.37 | 68.38 | 69.21 |
| Resnet18Test | 22.70 | 22.71 | 22.90 |

MobileNetV2에 남은 0.26 ms는 depthwise의 채널별 vector SHR이다. `_alux2`부터는 slot 루프에서 broadcast src(uop 1개, `src_factor_in` 0)로 II 1에 돈다(§6.3).

### 6.2 EVTA 4개 `_amul`
`vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta_sa_amul_400mhz_hp322_rq.bin`
(보드 `bit/evta_multi4_amul_400MHz_pass_261123/`, PC `build/y219/rs_mamulh`).
- `m4_build.sh amul VTA_PLACE_DIRECTIVE=ExtraNetDelay_high SYSTOLIC=1 ALUMUL=1 FREQ=400 IMPL_STRATEGY=Performance_ExtraTimingOpt`.
- LUT 167.9k (62 %), FF 215.3k, BRAM 580. `_ni` 243.9k, crossbar 212k보다 훨씬 작다.
- 배선 직후 WNS 0.000. 행 Y219의 net 382개를 RapidWright로 옮긴 뒤 −0.259 ns.
- `route_s.tcl ... opt`: 0.000 ns지만 net 11개가 행 안으로 돌아갔다. RapidWright 재배선 뒤 −0.176, 다시 opt에서 행 안 2개.
- 그 뒤 반복은 수렴하지 않았다. phys_opt는 HP 읽기 데이터 net(`saxigp4_rdata[123]` → `hpslice2`, Y221)을 행 안으로 넣어 맞추고, RapidWright가 돌려 배선하면 −0.092 / −0.192 ns가 된다.
- 그래서 HP 포트 클럭만 내렸다. `flow/mmcm_hp_set.tcl rs_mamulo3or.dcp rs_mamulh 46.25`로 `hp_pll` 333.0 → 321.7 MHz. EVTA는 400 MHz 그대로다.
- 결과: EVTA 클럭 WNS 0.000, HP +0.009, WHS +0.004, WPWS +0.054 ns. 행 Y219 PIP 0개, `stuckchk.py -b rs_mrqp2o3.bit` 0 extra.

보드: probes 40/40, `EVTA_RES_CHECK` 5 모델 391건 0 differ(avgpool 29건 포함), 분류는 `_rq`와 같다.
실행 파일은 보드 `build_multievtax4_systimed`에서 `cmake .` 후 다시 빌드했다(그 dir의 vtalib가 11-10 것이었다).
4 copy lockstep, OMP 1, 25회 중앙값(ms, copy마다):

| bundle | `_rq` (11-04) | `_amul` |
|---|---|---|
| MultiEVTAMobileNetV2Test | 19.23 | **18.69** |
| MultiEVTAMobileNetV2TfTest | 19.15 | **18.72** |
| MultiEVTAResnet50Test | 78.47 | **77.99** |
| MultiEVTAResnet50TvTest | 78.47 | **77.89** |
| MultiEVTAResnet18Test | 23.94 | 24.04 |

- Resnet18 multi4 bundle에는 EVTA avgpool이 없어서(`EVTA_RES_CHECK avgpool` 0건) 이득이 없다. +0.1 ms는 HP 클럭 3 % 차이로 보인다.
- 이미 있던 문제: `MultiEVTAResnet50Test`가 281 (0.832588)을 낸다. 이 값은 single `Resnet50TvTest`의 결과다. single `Resnet50Test`는 0.478383이다. `_rq`에서도 같으니, 11-22에 만든 multi4 copy가 Tv 모델로 만들어진 것으로 보인다. 같은 dir의 `singlevtaCaffe2Resnet50Bundle_systimed`(11-08)는 옛 실행 파일이라 exit 1이다.

### 6.3 broadcast vector를 imm 루프에 (`_bc`, Makefile `ALUBC=1`)
- `vta.cc` `VTA_ALU_BC`(ALUMUL=1과 함께): uop 1개이고 `src_factor_in` 0인 vector MIN / MAX / ADD / SHR이 imm 루프에서 돈다.
- 안쪽 루프마다 src를 한 slot 읽어 `s_reg`에 두고, 나머지 단계는 dst만 읽는다(II 1). imm 루프는 slot 하나짜리 루프 `ALU_IMM_SLOT`이 됐다.
- 명령 형식이 같아서 vtalib와 bundle은 그대로다. 이름 `_amul_bc`도 vtalib의 `_amul` 검사에 걸린다.
- HLS 추정(compute, `_amul` → `_amul_bc`): LUT 44,870 → 45,948, FF 28,978 → 31,040, DSP 같음, 2.635 ns 같음, 깊이 15.
- csim(`sim/run_csim.sh AMUL=1 BC=1`, broadcast 명령이 무작위 명령의 1/3): 20/20. `s_reg`에 +1을 넣은 고장판은 0/20. `AMUL=1`, `ALUX=1`도 20/20.
- MobileNetV2 replay dump: broadcast vector 218개(모두 depthwise SHR, 784 단계 = II 2에서 1,568 사이클 → 785), 그 밖의 vector 4개. Resnet18 / 50 / VGG16은 0개.
  `SLOTS=34 hazard_dump.py` 0건.

첫 합성(`..._sa_amul_bc_400mhz_rq.bin`, WNS +0.014 ns)은 **틀렸다**:
- MobileNetV2 14.38 ms였지만 confidence 0.920 → 0.862, depthwise 7개 층 `EVTA_RES_CHECK` differ(golden 30과 다른 CNN은 같음).
- 틀린 층은 바깥 반복이 여러 번인 명령(`iter 3–10 × 49 / 196`)이고, 처음 다른 곳이 각 줄의 마지막 2단계(entry 47–48, 194–195)였다.
- 원인: `s_reg`에 `DEPENDENCE inter false`를 두어, HLS가 `s_reg` 읽기를 MUL 단계 뒤로(쓰기보다 2단계 늦게) 놓았다. 줄 끝 2단계가 다음 줄의 src를 썼다. csim은 순서대로 돌아서 못 본다.
- cosim도 못 봤다: `run_cosim.sh`가 `VTA_ALUX_DIST`를 늘 빈 문자열로 넘겨 `alux_test.cc`가 직접 지정 모드로 들어가 무작위 명령을 0개 만들었다(2026-11-14부터 모든 cosim).

고침:
- `vta.cc`: pragma를 빼고 operand를 `alu_reg`로 잡았다(II 1, 깊이 15 → 17).
- `sim/alux_test.cc`: 빈 `VTA_ALUX_DIST`는 직접 지정이 아니다. `VTA_ALU_BC`면 depthwise 모양 broadcast 명령 6개(2–4줄, 줄마다 다른 src, rq 일부)를 넣는다.
- `sim/run_cosim.sh` `DEFS="-DVTA_ALU_MUL -DVTA_ALU_BC"`(`sim/cosim_compute.tcl` `VTA_COSIM_DEFS`, testbench도 `VTA_ALU_MUL`에서).
- cosim: 고치기 전 3/3 FAIL(323–447 byte, 보드 증상 재현), 고친 뒤 3/3 PASS. `DEFS=-DVTA_ALU_MUL`, ALUX(seed마다 명령 54개, 모든 확장 연산)도 3/3 PASS.

`..._sa_amul_bc_400mhz_rq2.bin` (Explore, WNS +0.036, WHS +0.010 ns, LUT 44,954; 보드 `bit/evta_single_amul_bc_400MHz_261124/`):
golden 30 PASS, `EVTA_RES_CHECK` 6 모델 3,830건 0 differ, 분류 / confidence는 `_amul`과 같다. OMP 1, 25회 중앙값(ms):

| bundle | `_amul` | `_amul_bc` (rq2) | `_alux3` |
|---|---|---|---|
| MobileNetV2Test | 14.65 | **14.40** | 14.43 |
| MobileNetV2TfTest | 14.63 | **14.39** | 14.38 |
| Resnet50Test | 64.45 | 64.48 | 64.45 |
| Resnet50TvTest | 68.37 | 68.41 | 68.37 |
| Resnet18Test | 22.69 | 22.69 | 22.72 |
| Vgg16Cifar10Test | 6.75 | 6.75 | 6.75 |

CNN 시간은 `_alux3`와 같고 LUT는 `_alux3`(69.8k)보다 25k 적다.

### 6.4 EVTA 4개 `_amul_bc` (2026-11-24)
`vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta_sa_amul_bc_400mhz_hp282_rq.bin`
(보드 `bit/evta_multi4_amul_bc_400MHz_pass_261124/`, PC `build/y219/rs_bch`). vtalib는 `vtalib-zcu102-systolic` `398f358`(바꾼 것 없음), 실행 파일은 §6.2와 같다.
- `m4_build.sh amulbc VTA_PLACE_DIRECTIVE=ExtraNetDelay_high SYSTOLIC=1 ALUMUL=1 ALUBC=1 FREQ=400 IMPL_STRATEGY=Performance_ExtraTimingOpt`.
- LUT 175.0k (64 %, `_amul` +7.1k), FF 233.8k. 배선 직후 EVTA +0.006, HP +0.084 ns.
- RapidWright(행 Y219 net 502개) 뒤 `route_s.tcl`이 "Interactive router failed ... node overlaps"로 멈췄다. `-Dtd=0`으로 다시 해도 같았다.
  - `report_route_status -show_all`: RapidWright가 두 쌍의 net을 같은 노드에 배선했다(`m_atarget_enc[3]` / `fetch_1 ... data_buf[147]`: `INT_X29Y218/INODE_W_9_FT1`; `ap_ce_reg_repN_22` / `icmp_ln723...repN_8`: `INT_X50Y213/INT_NODE_GLOBAL_4_INT_OUT1`).
  - 겹친 net을 모두 다시 배선하면 Vivado가 행 안으로 넣고, RapidWright가 다시 겹치게 옮겨 반복이 돌았다.
  - 풀이: 쌍마다 한 net(`icmp...repN_8`, `data_buf[147]`)만 Vivado로 다시 배선하고, 행 안으로 들어간 그 2개만 RapidWright로 옮겼다(겹침 0, 행 PIP 0).
    그 뒤 `route_s.tcl ... opt`와 RapidWright를 되풀이해 EVTA setup +0.003. hold −0.049는 `holdnets.tcl`로 오히려 −0.113이 되어 `holdpin.tcl ... 200`(최소 지연 200 ps)로 +0.010.
  - HP 읽기 경로 `SAXIGP4RCLK` → `hpslice2`가 −0.432 ns(25개)로 남아 `flow/mmcm_hp_set.tcl rs_bcp.dcp rs_bch 40.5`: `hp_pll` 281.7 MHz.
- 결과: EVTA 400 MHz WNS +0.003, WHS +0.010, HP 281.7 MHz +0.093, WPWS +0.275 ns. 행 Y219 PIP 0개.
- `stuckchk.py -b rs_mrqp2o3.bit`: 1 extra(frame 26209, word 62 bit 1). 행 Y219 타일 401개에 쓰인 PIP가 없고 그 행 site(SLICE / RAMB / DSP)에 cell도 없다. RapidWright 뒤의 모든 판에 있었다. 무엇인지는 찾지 못했다.
- 이번 흐름에서 쓴 임시 스크립트(`route_s_ur.tcl`: 겹친 net을 최상위 net으로 지우고 다시 배선, `fix2.tcl`)는 `build/y219`에만 있다.

보드: probes 40/40, `EVTA_RES_CHECK` 5 모델 432건 0 differ(MobileNetV2 152건 중 14x14 / 7x7 depthwise 80건). 4 copy lockstep, OMP 1, 25회 중앙값(ms, copy마다, 같은 날 잇달아):

| bundle | `_amul` (hp322) | `_amul_bc` (hp282) |
|---|---|---|
| MultiEVTAMobileNetV2Test | 18.66 | 18.73 |
| MultiEVTAMobileNetV2TfTest | 18.71 | 18.65 |
| MultiEVTAResnet50Test | 78.06 | 79.33 |
| MultiEVTAResnet50TvTest | 78.02 | 79.36 |
| MultiEVTAResnet18Test | 23.99 | 24.14 |

- MobileNetV2는 같다. single에서 줄인 0.25 ms(depthwise SHR)가 4 copy에서는 CPU 연산과 DDR 경쟁 시간에 묻힌다.
- Resnet50은 1.3 ms 느리다. 다른 차이가 HP 클럭(321.7 → 281.7 MHz)이라 그 탓으로 본다(따로 재지 않았다).
- **multi4는 `_amul`(hp322)을 쓴다.** `_amul_bc`는 single(LUT 여유, HP 333 MHz)에서만 의미가 있다.
