# depthwise 창 장치 하드웨어 수정 보고 (2026-11-24 ~ 26)

MobileNetV2Tf(AMD DPU와 비교하는 모델)를 빠르게 하려고 EVTA 하드웨어에 **depthwise 창 장치**(`_dw`)를 넣은 일을 처음부터 끝까지 정리한다.
bitstream을 두 개 만들었다.

- multi4: `_amul` 판에 창 장치를 더한 것.
- single: `_alux3` 판에 창 장치를 더한 것.

이 문서에 담은 것:

- 무엇을 어떻게 바꿨나
- 빌드가 어떤 과정을 거쳐 WNS가 어떻게 나왔나
- 어떤 소프트웨어 판으로 성능을 쟀고, 무엇이 나왔나
- 다른 모델에는 어떤 영향이 있었나

설계와 날짜순 기록은 [evta_dw_unit.md](evta_dw_unit.md)(§1–8, §9)에 있다. 모델 7개의 최신 시간 전체는 [evta_perf_latest.md](evta_perf_latest.md)에 있다.

용어:

- **WNS**(worst negative slack): 가장 나쁜 경로가 클럭 주기 안에 남긴 시간. 0 이상이면 타이밍 통과, 음수면 그만큼 늦다.
- **WHS**: hold 시간의 같은 값.
- **TNS**: 음수 slack의 합.
- **실패 끝점**: 타이밍을 못 맞춘 register 수.
- **depthwise conv**: 채널마다 따로 하는 conv. 채널끼리 더하지 않는다.
- **PLL / MMCM**: 클럭을 만드는 회로. 배선은 그대로 두고 배수만 바꿔 주파수를 낮출 수 있다.
- **phys_opt**: 배선 뒤 cell을 복제하거나 옮겨 늦은 경로를 줄이는 Vivado 단계.
- **HP 클럭**: PS의 DDR 포트(HP)와 EVTA DMA 사이의 클럭.
- **행 Y219**: 이 보드 PL의 고장 난 행. 그 행의 배선 스위치(PIP)를 쓰면 값이 0에 붙는다.
- **RapidWright**: 행 Y219를 피해 다시 배선하는 도구.
- **copy**: multi4에서 EVTA 하나를 쓰는 실행 사본.
- **fps**: 초당 처리한 그림 수.

## 0. 요약

| | multi4 `_amul` → `_amul_dw` | single `_alux3` → `_alux3_dw` |
|---|---|---|
| bitstream | `..._16_15_18_17_4evta_sa_amul_dw_400mhz_hp304_rq.bin` | `..._16_16_19_18_sa_alux3_dw_387mhz_rq.bin` |
| 클럭 | EVTA 400 MHz(그대로), HP 321.7 → **304.3 MHz** | EVTA 400 → **387.5 MHz** |
| 최종 WNS / WHS | +0.002 / 0.000 ns | +0.010 / 0.000 ns |
| 어려웠던 곳 | 행 Y219를 지나는 net 549개 → `VTA_CONTAIN=1`로 29개 | `_alux3`의 ALU ↔ acc_mem 경로(원래 여유가 없었다) → PLL만 낮춤 |
| LUT | 167.9k → 173.2k (EVTA당 +1.3k) | 69.8k → 71.1k |
| MobileNetV2Tf | 4개 동시 256.7 → **281.5 fps**(+9.7 %, DPU S 270), 혼자 13.55 → **12.14 ms** | 13.06 → **11.65 ms**(−10.8 %) |
| 다른 모델 | 4개 동시: VGG16 −2 %, 나머지는 잡음 안. 혼자: 0–2.5 % 느림 | 1–3 % 느림(클럭 3 % 낮음) |
| 결정 | **multi4 기본**(11-26부터) | single 기본은 `_alux3` 400 MHz 그대로. MobileNetV2용으로만 `BIN=` |

창 장치는 depthwise conv만 빠르게 한다. 다른 모델에는 depthwise가 없어서 이득이 없다. 대신 타이밍을 맞추려고 낮춘 클럭만큼 손해를 본다.
multi4는 EVTA 클럭 400 MHz를 지켰기 때문에, 손해가 HP 클럭에서 온 VGG16 2 % 정도에 그쳤다.

## 1. 왜 바꿨나

- DPU 비교([evta_dpu_compare.md](evta_dpu_compare.md) §6.6)에서 multi4 MobileNetV2Tf는 260 fps로 DPU S(B4096 1개) 270 fps보다 느렸다.
  시간의 큰 몫이 depthwise 3 × 3이었다.
- 창 장치를 넣기 전의 depthwise(`dw_convolution_evta`)는 탭마다 16 × 16 **대각 행렬** weight로 GEMM을 했다.
  - 대각 행렬이라 GEMM 한 step의 MAC 256개 가운데 16개만 쓴다.
  - 출력 픽셀 하나에 9 step이 걸린다.
- **창 장치:** 가로로 이웃한 출력은 입력 픽셀 2개를 같이 쓴다. 그래서 입력을 한 픽셀씩 읽어 최근 3픽셀을 창 register에 들고 있으면,
  새 픽셀 하나가 들어올 때마다 가로 3탭(16채널 × 3 MAC)을 한 step에 끝낸다.
  - 세로 3탭은 acc에 세 번 더한다.
  - 출력 하나에 약 3 step(stride 1)이다. MobileNetV2Tf의 depthwise GEMM 하한은 3.24 → 1.39 ms다.
- 크기가 작아서(EVTA당 LUT 약 1.3k, BRAM 0) 지금 multi4에 바로 넣을 수 있다. 그래서 픽셀 병렬(PP8)처럼 구성 전체를 바꾸는 일보다 먼저 했다(설계 §7).

## 2. 무엇을 바꿨나 (두 판 공통)

| 층 | 바꾼 것 |
|---|---|
| ISA | GEMM 명령 `dw_win` 1 bit(pad bit), uop bit 두 개 `dw` / `emit`. 창 모드 step: `win0 = win1; win1 = win2; win2 = inp[src]`, emit이면 `acc[dst] += Σk w[k]·win_k`(16채널 각각) |
| HLS compute (`vta.cc`) | uop의 bit 두 개를 GEMM 쪽으로 실어 나른다. C 모델 `gemm_sa_csim`에 창 모드 |
| RTL | 새 `rtl/sa_dw.v`(16 lane × 3탭, int8 곱 48개를 LUT로), `sa_ctrl.v`(FIFO와 tag에 dw / emit), `gemm_sa.v` / `gemm_sa_bd.v`(row 출력 mux). 파라미터 `DWU=0`이면 예전 회로 그대로 |
| 빌드 | Makefile `DWU=1`(이름에 `_dw`), `vivado.tcl` `CONFIG.DWU`. uop 폭은 그대로(single 72 bit, multi 64 bit 안에 들어간다) |
| vtalib-systolic | `VTAHasDwUnit()`(bitstream 이름의 `_dw`, `EVTA_DWU=0\|1`): 대각 weight를 블록 × ky entry로 바꾼다(처음 한 번, cache). uop `gemmop_dw`. ky loop를 행보다 바깥에 둔다(weight 채우기 17 cycle 대기를 피한다) |

- bundle과 컴파일러는 바꾸지 않았다. 같은 실행 파일이 `_dw`가 없는 bitstream에서는 예전 대각 GEMM으로 돈다.
- 시험은 이 순서로 했다:
  1. C 모델(`sim/run_csim.sh DWU=1`, 8경우)
  2. RTL xsim(창 벡터 6개와 기존 5종, 역압 `GAP=40 BP=50`)
  3. vtalib 시뮬레이터(`tests/run_dw_test.sh`, MobileNetV2Tf의 depthwise 10모양과 경계 모양 5개, 창 / 대각 모두 0 differ)
  4. 보드
- 처음 RTL에서 고친 것이 하나 있다. flag / dv 한 bit가 sa_array PE와 sa_dw의 768 bit register를 한꺼번에 구동해 −0.112 ns였다.
  sa_dw 입력을 lane마다 한 번 더 등록했다(`keep`). 지연선을 30 → 29로 줄였으므로 출력 시각 a+33은 그대로다.

## 3. multi4: `_amul` + 창 장치

**설정:** `_amul`과 같다(systolic, ALU MUL immediate, EVTA 400 MHz, HP는 따로 333 MHz로 빌드한 뒤 낮춤). 여기에 `DWU=1`을 더했다.

```
vta-hw-zcu102-multi4: y219route/flow/m4_build.sh amuldw VTA_PLACE_DIRECTIVE=ExtraNetDelay_high \
    SYSTOLIC=1 ALUMUL=1 DWU=1 FREQ=400 IMPL_STRATEGY=Performance_ExtraTimingOpt
```

multi4는 BRAM이 모자라 고장 난 행 Y219 위아래에 걸쳐 놓인다. 흐름은 세 단계다.

1. Vivado 배치 / 배선
2. 행에 PIP가 있는 net을 RapidWright로 다시 배선
3. Vivado `route_s.tcl`로 나머지 배선

| 단계 | 결과 | 판단 |
|---|---|---|
| ① 첫 빌드 `mamuldw`: Vivado 배선 직후 | WNS 0.000 ns, 그런데 **행 Y219에 PIP가 있는 net 549개**(`_amul`은 382) | |
| ② RapidWright | 99회 뒤 멈춤, PIP 겹침 22, 남은 sink 119 | |
| ③ `route_s.tcl` | **WNS −5.363 ns**, 행 안 net 7개(그중 3개는 고장 셀 위) | 쓸 수 없다 |
| ④ RapidWright 반복 250회, bounding box 넓힘 | 겹침이 175k → 385k로 늘어 수렴하지 않음 | 멈춤 |
| ⑤ 깨진 경로의 net만 Vivado로 다시 배선 | −5.858 ns, 깨진 끝점 15,028 | 더 나빠짐 |

**원인 찾기.** 행 net 549개를 출발 / 도착 블록으로 나눴다(`build/y219/rowcls.tcl`). 대부분 행을 건널 필요가 없는 net이었다.

| 행 net | 개수 |
|---|---|
| EVTA 1 내부(compute_1 156, gemm_sa_1 47, acc_mem_1 30, load_1 …) | 약 330 |
| crossbar, 폭 변환기(`reg_dwc`) 내부 | 약 70 |
| 위쪽 EVTA 2 / 3의 HP 경로(꼭 건너야 하는 것) | 약 100 |

- `place_d.tcl`이 모든 pblock의 `CONTAIN_ROUTING`을 끄고 있었다. 이 속성을 켜면 net을 pblock(배치 영역) 안 배선 자원으로만 잇는다.
  - 2026-10-24에 끈 이유: 두 사각형 `pb_fence`에서는 사이 행이 막히지 않았고, 자식 pblock에 켜면 배선 실패였다.
- 그래서 행 바로 아래 좁은 영역에 빽빽이 놓인 EVTA 1이 행의 PIP를 지름길로 빌려 썼다.
- ⑤가 더 나빴던 까닭도 같다. RapidWright가 행 net을 옆으로 돌리면서 그 아래 compute_1의 배선 자원을 차지했다.

**해결 `VTA_CONTAIN=1`.** 한 사각형인 pblock `pb_evta_0..3`과 `pb_hpslice`, 새로 만든 `pb_lo`(crossbar, `reg_dwc`, PS reset; 행 아래)에만
`CONTAIN_ROUTING`을 켰다. 배치는 같은 `opt_design` 결과에서 다시 했다(`mamuldwc`).

| 단계 (`mamuldwc`) | 걸린 시간 | EVTA 400 MHz WNS | HP WNS | 행 net |
|---|---|---|---|---|
| 배치 + Vivado 배선 (`run_flow.sh`) | 2 h 17 m | +0.004 ns | | 549 → **29** |
| RapidWright | 약 2 m | | | 남은 sink 76, 겹침 없음 |
| `route_s.tcl` | 9 m | −0.016 ns | −0.476 ns (333 MHz) | 0 |
| `route_s.tcl ... opt` (post-route phys_opt) | 27 m | **+0.002 ns** | −0.261 ns (PS `SAXIGP5` rdata → `hpslice3`) | 0 |
| `mmcm_hp_set.tcl ... 43.75`: HP 클럭만 304.3 MHz로 | 7 m | **+0.002 ns** | 통과, **WHS 0.000 ns** | 0 |
| `stuckchk.py` (고장 셀 위 bit) | | | | 0개 |

- EVTA 클럭 400 MHz는 지켰고, HP 클럭만 321.7(`_amul`) → 304.3 MHz로 낮췄다.
- 자원: LUT 167.9k → 173.2k(64 %), FF 215.3k → 220.3k, BRAM36 580 → 584, DSP 1,412.
- 보드 확인: probes 40/40. `EVTA_RES_CHECK=1`(EVTA 호출마다 CPU와 비교)이 4개 동시 / 혼자 모두 0 differ, 네 copy 모두 282.
- 앞으로 multi4 빌드는 `VTA_CONTAIN=1`을 쓴다(`y219route/README.md`). RapidWright가 수렴하지 않을 때 반복 횟수를 늘리는 것은 효과가 없었다.
  먼저 행 net이 왜 많은지 나눠 본다.

## 4. single: `_alux3` + 창 장치

**설정:** `_alux3`(ALU 확장 3단계, single 기본)과 같다. 여기에 `DWU=1`을 더했다.

```
vta-hw-zcu102-systolic/hardware/xilinx: make bin SYSTOLIC=1 ALUX=1 DWU=1 FREQ=400 IMPL_STRATEGY=<전략> BUILD_TAG=rq_alux3<x>
```

`_alux3`은 원래 400 MHz에서 Explore로 겨우 +0.022 ns였다. 실패 경로는 대부분 **ALU(compute) ↔ acc_mem BRAM** 경로다.
창 장치 자체의 경로(gemm_sa 안)는 lane 등록(§2) 뒤에는 거의 문제가 되지 않았다. 창 장치가 들어가면서 배치가 바뀌어 이 기존 경로가 밀린 것이다.

| 날짜 | 단계 | WNS | 판단 |
|---|---|---|---|
| 11-25 | ① 첫 빌드(lane 등록 전, ExplorePostRoutePhysOpt) | −0.145 ns, 실패 끝점 1,008(ALU ↔ acc_mem 608, gemm_sa 안 16) | sa_dw 입력 등록 추가 |
| 11-25 | ② ①의 PLL만 375 MHz(`mmcm_set.tcl ... 11.25`) | +0.020 ns | 시험용 판 `_alux3_dw_375mhz`. 보드 시험과 첫 성능 측정에 썼다 |
| 11-25 | ③ 고친 RTL로 다시(`rq_alux3b`) | 멈춤 | multi4가 먼저 |
| 11-26 | ④ 두 전략을 동시에 빌드: c = ExplorePostRoutePhysOpt | −0.158 ns, TNS −158.3, 실패 2,906 | 버림 |
| 11-26 | ④ d = **ExtraTimingOpt** | **−0.092 ns**, TNS −16.8, 실패 498 | 가장 나쁜 경로: acc_mem BRAM → `compute_0` `dst_tensor` |
| 11-26 | ⑤ d에 phys_opt AggressiveExplore / AlternateFlowWithRetiming / AggressiveFanoutOpt, 그다음 `route_design -directive Explore` (`popt.tcl`) | phys_opt 뒤 −0.070, 다시 배선 뒤 **−0.091** | 다시 배선하면 이득이 사라진다 |
| 11-26 | ⑥ d에 phys_opt AggressiveExplore만 (`popt2.tcl`) | **−0.070 ns**, TNS −13.9, WHS 0.000 | 이것을 쓴다 |
| 11-26 | ⑦ ⑥의 PLL만 387.5 MHz(`mmcm_set.tcl popt2_d.dcp dw387 11.625`, VCO 1162.5 MHz ÷ 3) | **WNS +0.010, WHS 0.000 ns**, 실패 0 | **최종** |
| 11-26 | (같이) ⑤의 PLL만 383.3 MHz(`... 11.5`) | +0.016 ns | 보드에 있지만 387.5가 약 0.5 % 빠르다 |

- ④에서 c와 d 두 빌드는 HLS 디렉터리 하나를 같이 써서 서로 부딪쳤다(c가 실패). d의 HLS가 끝난 뒤 c를 다시 시작했다.
  두 빌드를 함께 돌릴 때는 HLS를 먼저 하나만 끝낸다.
- 0.070 ns를 줄이는 데 필요한 주기 비율만큼만 클럭을 낮췄다. 2.5 ns → 2.58 ns, 400 → 387.5 MHz(PLL 배수 단위 0.125). 배치와 배선은 그대로다.
- 자원(⑦): LUT 71.1k(`_alux3` 69.8k), FF 93.1k(92.0k), BRAM 259(258), DSP 351.
- 보드 확인:
  - `run_alux_board_tests.sh`(ALU 확장, epilogue, layer norm, softmax, elemadd, `dw_evta_test` 창 / 대각 모드)가 모두 0 differ였다.
  - 모델 7개 모두 `EVTA_RES_CHECK` 0 differ였다.
  - (375 MHz 판에서는 MobileNetV2Tf conv dump 5,200개가 `_alux3`과 바이트까지 같았다.)
- 파일:
  - 보드 `/lib/firmware/vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_alux3_dw_387mhz_rq.bin`, 사본 `bit/evta_single_alux3_dw_387MHz_261126/`
  - PC `build/single_dw400/`(`popt.tcl`, `popt2.tcl`, `dw387.*`, `dw383.*`)

## 5. 성능

### 5.1 잰 조건 (소프트웨어 판)

| | 판 |
|---|---|
| 컴파일러 | `0b1439b` 이후(invalidate 빼기 `-vta-skip-evta-invalidate` 포함). 그 뒤 컴파일러 변경 없음 |
| bundle | 11-26에 보드에서 지금 컴파일러로 다시 만듦. single과 multi4 copy 0의 weights가 같다 |
| vtalib | `vtalib-zcu102-systolic` `818a73c`: 창 장치(`VTAHasDwUnit`, `gemmop_dw`, `dwWindowWeights`와 free hook) + ubun의 11-25 CPU 시간 줄이기(`1cdf422`까지) |
| 실행 파일 | 보드 `build_sys`, `build_multievtax4_sys`(시간 재는 코드 없음), 범용 main(`multi_threads_main.cpp`) |
| 입력 | int8(`<Dir>/i8/`, bundle 자신의 quantize로 만든 것) |
| 재는 법 | OMP 1. 혼자 = 4 × 25장, 4개 동시 = copy마다 25장. bitstream을 번갈아 3번씩, 표는 3번의 평균. `vtabundles_xcmpl/perf_record.sh` |
| 결과 | 모든 실행 파일 `EVTA_RES_CHECK=1` 0 differ, 분류가 전과 같다 |

같은 실행 파일을 쓰고 bitstream만 바꿨다. 그래서 표의 차이는 하드웨어(창 장치와 클럭)에서만 온다.

### 5.2 MobileNetV2Tf (이 일의 목표)

| | 전 | `_dw` | 변화 |
|---|---|---|---|
| multi4 4개 동시 | 256.7 fps (`_amul`) | **281.5 fps** | +9.7 %, **DPU S 270 fps의 104 %** |
| multi4 copy당 (4개 동시) | 15.60 ms | 14.22 ms | −8.8 % |
| multi4 혼자 (copy 0) | 13.55 ms | **12.14 ms** | −10.4 % |
| single | 13.06 ms (`_alux3` 400) | **11.65 ms** (387.5 MHz) | −10.8 % |
| single, 같은 `_dw` 판에서 `EVTA_DWU=0`(대각 GEMM) | | 13.30 ms | 창 모드 덕이 −12.4 % |

- 4개 동시는 3번 값이 ±3 % 흔들린다. 11-25에 따로 잰 값은 `_amul` 260.1 → `_dw` 287.4 fps(+10.5 %)였다(같은 실행 파일, ubun vtalib + dw).
- 375 MHz 시험 판에서 depthwise 17층 합은 6.05 → 4.21 ms였다. 큰 층(112 × 112 1.05 → 0.53, 56 × 56 0.97 → 0.53 ms)이 많이 줄었다.
  작은 층(7 × 7 × 960 0.20 → 0.17 ms)은 행이 짧아, 창을 채우는 2 step과 weight 채우기의 몫이 커서 덜 줄었다.
- 하한(1.39 ms)과 아직 차이가 있다. depthwise 층은 GEMM 말고도 LOAD / STORE와 ALU(requantize) 시간이 있기 때문이다.
- MobileNetV2(QAT, ETRI 모델)도 같다. multi4 263.9 → 282.6 fps, 혼자 13.55 → 12.10 ms, single 13.04 → 11.70 ms.

### 5.3 다른 모델에 준 영향

**multi4** (`_amul` hp322 → `_amul_dw` hp304. EVTA 클럭은 둘 다 400 MHz):

| 모델 | 4개 동시 fps | 변화 | 혼자 ms | 변화 |
|---|---|---|---|---|
| Resnet18 | 171.3 → 170.3 | −0.6 % | 22.38 → 22.42 | +0.2 % |
| Resnet50 | 55.4 → 55.7 | +0.5 % | 66.06 → 66.58 | +0.8 % |
| Resnet50Tv | 55.5 → 55.9 | +0.7 % | 65.43 → 65.96 | +0.8 % |
| VGG16 CIFAR-10 | 508.3 → 497.7 | **−2.1 %** | 6.90 → 7.07 | **+2.5 %** |
| DeiT-tiny | 43.7 → 44.2 | +1.1 % | 80.72 → 80.81 | +0.1 % |

**single** (`_alux3` 400 MHz → `_alux3_dw` 387.5 MHz):

| 모델 | ms | 변화 | 같은 판 `EVTA_DWU=0` |
|---|---|---|---|
| Resnet18 | 22.15 → 22.81 | +2.9 % | 22.86 |
| Resnet50 | 62.33 → 63.96 | +2.6 % | 63.88 |
| Resnet50Tv | 61.78 → 63.33 | +2.5 % | 63.34 |
| VGG16 CIFAR-10 | 6.68 → 6.75 | +1.1 % | 6.75 |
| DeiT-tiny | 34.44 → 35.10 | +1.9 % | 35.10 |

읽는 법:

- **이득이 없는 까닭:** 다섯 모델에는 depthwise conv가 없다. 창 모드를 한 번도 쓰지 않는다. `EVTA_DWU=0`으로 꺼도 시간이 같은 것이 그 증거다.
- **손해의 원인은 클럭뿐이다.** 창 장치는 GEMM 옆에 붙은 별도 경로다. `DWU`가 켜져도 일반 GEMM의 step 수, 지연, 명령은 같다.
  - single: EVTA 클럭이 3.1 % 낮아졌다. EVTA 시간이 대부분인 Resnet은 +2.5–2.9 %다.
    CPU 몫이 큰 DeiT(+1.9 %)와 VGG16(+1.1 %)은 덜 느려졌다.
  - multi4: EVTA 클럭은 그대로다. HP 클럭만 5.4 % 낮아졌다. DMA가 많은 모델만 영향을 받는다.
    - VGG16: 혼자 +2.5 %, 4개 동시 −2 %다. 그림이 작고(32 × 32) conv가 짧아, 시간에 비해 데이터 이동이 가장 많은 모델이라서로 보인다.
    - Resnet50: 혼자 +0.8 %다.
    - 나머지: 4개 동시 값은 측정 잡음(±3 %) 안이다.
- **그래서 판단은 이렇다.**
  - multi4는 MobileNetV2 두 모델이 +7–10 %이고 손해는 VGG16 2 %뿐이다. 그래서 기본을 바꿨다(11-26, `hw_multievtax4/script/run_all.sh` `SYS_BIN`).
  - single은 다섯 모델이 1–3 % 느려진다. 그래서 기본은 `_alux3` 400 MHz로 두었다. MobileNetV2는 `BIN=…_sa_alux3_dw_387mhz_rq.bin`으로 잰다.
- 참고로 DeiT multi4(80.8 ms)가 single(34.4 ms)보다 훨씬 느린 것은 창 장치와 관계없다.
  multi4 bitstream에 ALU 확장 2 / 3단계가 없어서 layer norm과 softmax가 CPU로 가기 때문이다.

## 6. 남은 것

- **single `_dw`를 400 MHz로 맞추기:** 남은 차이는 0.070 ns다. 이것이 맞으면 다섯 모델의 1–3 % 손해가 없어진다.
  그러면 single 기본을 `_dw`로 바꿀 수 있다. 실패 경로는 `_alux3`의 ALU ↔ acc_mem이다.
  - 할 수 있는 것: compute / acc_mem에 pblock 걸기, ALU 쪽 HLS register 추가, 배치 directive 바꾸기.
- **multi4 HP 클럭:** 이제 `VTA_CONTAIN=1`이 있으니, 다시 빌드하면 HP를 322 MHz 이상으로 맞출 수 있을지 모른다(해 보지 않았다). VGG16의 2 %가 걸린 일이다.
- **MobileNetV2Tf를 더 빠르게:** depthwise는 하한 1.39 ms에 대해 아직 약 2배다.
  - LOAD / STORE를 겹치기, 작은 층의 schedule 바꾸기.
  - 더 크게는 PP8(설계 §7).
- 다시 만드는 명령은 [evta_dw_unit.md](evta_dw_unit.md) §9.4(multi4)와 이 문서 §4(single)에 있다. 다시 재는 법은 [evta_perf_latest.md](evta_perf_latest.md) §5에 있다.
