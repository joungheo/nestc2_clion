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
- **`m4_build.sh`의 문제:** 이번에는 make가 끝난 뒤 impl dir 변수(`I`)가 비어 `run_flow.sh`를 부르지 않았다(`build/m4_ni_driver.log`: `ls: cannot access '/vta.runs/impl_1/...'`). 그래서 아래처럼 직접 불렀다. 원인은 아직 고치지 않았다.

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

## 5. 남은 일

1. multi4 `_ni` 배선과 타이밍(§3).
2. EVTA 1개 `_ni`의 보드 시험.
   - ALU 시험과 dump 비교를 한다.
   - CNN / DeiT 시간을 `_alux3`와 견준다. 명령마다 파이프라인을 채우는 시간이 늘어나는 만큼 차이가 날 수 있다.
   - 같으면 EVTA 1개도 `_ni`를 기본으로 할지 정한다.
3. `m4_build.sh`의 `I` 변수 문제.
