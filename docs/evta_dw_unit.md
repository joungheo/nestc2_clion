# depthwise 창 장치 (작은 판, 2026-11-24 설계, 11-25 multi4 보드 동작)

MobileNetV2의 depthwise 3 × 3 conv를 지금보다 약 3배 빠르게 하는 GEMM 옆 장치다. 왜 이것을 먼저 하는지는
[evta_dpu_compare.md](evta_dpu_compare.md) §6.6과 이 문서 §1, 픽셀 병렬(PP8)과 비교한 것은 §7에 있다.
**결과만 보려면 §9**(바뀐 것, 빌드가 어떻게 성공했는지, 다시 만드는 명령, 성능). §8은 날짜순 기록이다.

## 1. 지금 depthwise가 느린 이유

- depthwise 3 × 3: 채널 c의 출력 = 입력 채널 c만 3 × 3 필터(9개 값)로 훑은 것. 채널끼리 더하지 않는다. 필터는 채널 수만큼 있다.
- 지금(`dw_convolution_evta`, 11-08): 16채널 블록 하나를 16 → 16 conv로 보고, 탭마다 **대각 행렬** weight(diag(w[c][ky][kx]))를 쓴다.
  - GEMM 한 step = 입력 vector 하나(한 픽셀의 16채널) × 16 × 16 weight. 대각선이라 256 MAC 가운데 16개만 쓴다.
  - 출력 vector 하나(한 픽셀의 16채널)에 탭 9개 = **9 step**.
- MobileNetV2Tf 1장: depthwise GEMM 하한 3.24 ms(+ bias reset 0.36 ms), 실제 depthwise 약 6.1 ms(1개) / 6.4 ms(4개 동시).
  한 층(56 × 56 × 144)을 보면 977 us 가운데 GEMM 706 us(72 %).

## 2. 생각: 가로 3탭을 한 step에

출력 픽셀 x는 같은 행의 입력 픽셀 x, x+1, x+2(padding 포함 tile 좌표)를 쓴다. 이웃한 출력은 입력 2개를 같이 쓴다.
그래서 입력을 한 픽셀씩 읽어 **최근 3픽셀을 창(window) register에** 들고 있으면, 새 픽셀 하나가 들어올 때마다 가로 3탭이 다 모인다.

```
step:   입력 픽셀 읽기   창 = {x-2, x-1, x}     내보내기(emit)
 0      in[0]           {?, ?, 0}             없음 (창 채우기)
 1      in[1]           {?, 0, 1}             없음
 2      in[2]           {0, 1, 2}             acc[out 0] += w0*in0 + w1*in1 + w2*in2   (16채널 각각)
 3      in[3]           {1, 2, 3}             acc[out 1] += ...
```

- 세로 3탭(ky)은 지금처럼 acc에 세 번 더한다(입력 행 y, y+1, y+2를 차례로).
- step 수: 출력 tile 폭 tw, stride s이면 행 하나에 (tw − 1) s + 3 step이 ky마다 → 출력 하나에 약 3 step(s = 1), 약 6 step(s = 2).
- MobileNetV2Tf의 지금 schedule로 셈: depthwise GEMM 3.24 → **1.39 ms**(s = 1 층은 약 1/3, s = 2 층은 2/3).

| 모양 (H, W, C, stride) | tw | 지금 GEMM (ms) | 창 장치 (ms) |
|---|---|---|---|
| 112, 112, 32, 1 | 56 | 0.564 | 0.195 |
| 112, 112, 96, 2 | 56 | 0.423 | 0.285 |
| 56, 56, 144, 1 | 56 | 0.635 | 0.219 |
| 56, 56, 144, 2 | 28 | 0.159 | 0.108 |
| 28, 28, 192, 1 (2번) | 28 | 0.423 | 0.151 |
| 28, 28, 192, 2 | 14 | 0.053 | 0.037 |
| 14, 14, 384, 1 (4번) | 14 | 0.423 | 0.161 |
| 14, 14, 576, 1 (2번) | 14 | 0.318 | 0.121 |
| 14, 14, 576, 2 | 7 | 0.040 | 0.028 |
| 7, 7, 960, 1 (3번) | 7 | 0.198 | 0.085 |
| 합 | | 3.24 | 1.39 |

## 3. ISA (지금 것에 bit 두 개)

- `VTAGemInsn.dw_win` (pad_0에서 1 bit): 이 GEMM 명령의 step은 창 모드로 돈다.
- uop bit 63 `dw_emit` (64-bit uop의 맨 위; 지금 쓰는 것은 single 35 bit, multi 32 bit).
- 창 모드 step의 뜻(모든 기준 모델이 이대로 한다):

```
win0 = win1; win1 = win2; win2 = inp[src]                       // 16 x int8
if (dw_emit)
  for j in 0..15:  acc[dst][j] += w[wgt][j][0] * win0[j] + w[wgt][j][1] * win1[j] + w[wgt][j][2] * win2[j]
                   out[dst][j] = requant(acc[dst][j])            // 명령의 rq_*, 지금 GEMM과 같다
```

- weight entry 하나(16 × 16 byte)의 행 j = 채널 j, 열 k = 가로 탭 kx = k(열 3–15는 0). 블록 하나의 ky마다 entry 1개 → 블록당 3개(지금 9개).
- 창은 명령이 시작할 때 정해져 있지 않다. 소프트웨어가 행마다 처음 2 step을 emit 없이 채운다. 그래서 명령의 마지막 step은 emit이어야 한다(행 끝은 늘 emit이다).
- `reset_reg`, `bias_reset`과 같이 쓰지 않는다(bias reset은 지금처럼 따로 하는 GEMM 명령).

## 4. 하드웨어 (vta-hw-zcu102-systolic, `make ... DWU=1`, 이름 `_dw`)

```
 HLS compute ──uop {.., dw, emit}──→ sa_ctrl (FIFO, weight 채우기, issue: 바뀌지 않음, bit 두 개를 실어 나름)
                                        │ inp 읽기 (cycle마다 1)      │ wgt bank j 읽기 (지금처럼 g+j)
                                        ▼                             ▼
                                  sa_array (16x16)              sa_dw (새): B1[j] <= bank j의 0..2 byte, flag에 B2 <= B1
                                        │                             │ 창 3 x 16 byte, lane마다 곱 3개 + 합 (20 bit)
                                        │                             │ SRL로 sa_array와 같은 지연(a+33)에 맞춤
                                        └────── row_out mux (tag의 dw bit) ─────┘
                                                        ▼
                                  sa_rmw (acc 읽기-더하기-쓰기, rq, out 쓰기, token: 바뀌지 않음)
```

- `sa_dw.v`: x_in(inp 한 행), flag, go, wd(16 bank) = sa_array와 같은 입력, 그리고 dw step의 valid. 곱은 int8 × int8 48개(LUT, 약 3k), 창 / weight register와
  지연 SRL. 지연은 sa_array와 같게 해서 tag(목적지, rq, token)가 그대로 맞는다.
- `sa_ctrl.v`: uop bit 두 개(dw, emit)를 FIFO와 tag에 싣는다. emit이 아닌 dw step은 tag valid = 0(acc를 쓰지 않는다; 마지막 step은 emit).
- weight는 지금 채우기 엔진이 그대로 bank j를 g+j에 읽는다. sa_dw는 그 cycle에 B1[j]을 받고 첫 step의 flag에 B2로 옮긴다.
  sa_array의 PE처럼 다음 채우기가 latch 전 B1을 덮지 않는다(같은 규칙).
- `gemm_sa` / `gemm_sa_bd` 파라미터 `DWU`(기본 0): 0이면 sa_dw가 없고 bit 두 개는 0이다. 지금 bitstream 그대로(multi4 Makefile도 같은 RTL을 쓴다).
- uop TDATA: single 12 + 12 + 11 + 30 = 65 → 72 bit, multi 62 → 64 bit라 bit 두 개(B+30, B+31)가 지금 폭 안에 들어간다.

## 5. 소프트웨어

- vtalib-zcu102-systolic: `VTAHasDwUnit()` = bitstream 이름에 `_dw`(시뮬레이터는 늘, `EVTA_DWU=0|1`).
  `dw_convolution_evta`: 창 모드면 weight를 블록 × ky entry로 바꿔 VTA buffer에 둔다(bundle의 대각 weight에서 처음 한 번; bundle은 바뀌지 않는다),
  uop(`gemmop_dw`): tile의 출력 행 × 행의 입력 픽셀((tw − 1) s + 3 step, emit 표시), loop 바깥 = 블록(nF), 안 = ky.
  - **ky가 행보다 바깥이어야 한다:** gemm_sa는 weight 블록이 바뀔 때마다 다음 블록을 채우는 데 17 cycle이 걸린다(sa_ctrl의 fill 규칙).
    행마다 ky를 바꾸면 run이 행 길이(7폭 tile이면 9 step)뿐이라 기다린다. RTL 시험에서 266 uop이 563 → 338 cycle.
  - tile의 uop 수(행 × 입력 폭)가 uop buffer의 1/4(2,048)을 넘으면 대각 판으로 돈다.
  - weight 변환 버퍼는 kernel buffer마다 한 번 만들고, 원래 탭이 바뀌면(해제된 buffer 주소가 다시 쓰일 때) 다시 만든다. replay의 buffer 목록에는 kernel 대신 이 버퍼가 들어간다.
- vtalib 시뮬레이터(`vtalib-zcu102-systolic` sim)와 HLS C 모델(`vta.cc` `gemm_sa_csim`)에 §3의 뜻을 넣는다.
- 확인: `EVTA_RES_CHECK=1`(CPU와 비교), conv dump가 대각 판과 byte 단위로 같은지, `tests/`의 depthwise 경우.

## 6. 할 순서

1. 기준 모델: `hw_spec.h` bit, `vta.cc` `gemm_sa_issue` / `gemm_sa_csim`, `run_csim.sh`에 창 모드 무작위 경우(대각 GEMM 9 step 결과와 같은지).
2. RTL: `sa_dw.v`, `sa_ctrl.v`, `gemm_sa.v`, `gen_gemm_sa_bd.py`, `tb_gemm_sa.v`에 창 모드(xsim, container).
3. vtalib: 시뮬레이터, `dw_convolution_evta` 창 모드, PC에서 MobileNetV2 bundle을 시뮬레이터로 돌려 0 differ.
4. single `_alux3` + `DWU=1` bitstream(400 MHz), 보드: dump 같음, 시간.
5. 되면 multi4 `_amul` + `DWU=1`.

## 7. 픽셀 병렬(PP8)과의 관계

- PP8(같은 weight를 픽셀 8개에)은 모든 conv를 빠르게 하지만 inp / acc / out 폭 8배, DSP가 칩에 EVTA 2개분뿐이라 구성 자체가 바뀐다.
  3 × 3과 depthwise까지 하려면 8 bank + 회전 읽기가 있어야 한다. 그때는 이 장치의 일이 겹친다.
- 이 장치는 작고(LUT 약 3–5k, BRAM 0) 지금 multi4에 넣을 수 있어 MobileNetV2로 DPU S(270장/s)를 먼저 겨룬다. compute에 새 모드를 넣는 일(기준 모델,
  RTL, vtalib, 시뮬레이터)은 PP8에도 그대로 쓰인다.

## 8. 진행 기록

**2026-11-24 ~ 25 (ubuntu2)**
- 기준 모델:
  - `sim/run_csim.sh DWU=1` → `sim/dwu_test.cc`의 8경우(1 × 1, 2 × 3, 4 × 56, 14 × 14, 5 × 9는 stride 1; 7 × 7, 3 × 28, 1 × 5는 stride 2; rq 무작위)가 C로 직접 계산한 depthwise conv와 같다.
  - 기존 ALU / GEMM 시험도 통과한다.
- RTL (`rtl/tb/run_xsim.sh`, xsim):
  - `rtl/tb/gen_dw_vectors.py`의 창 모드 벡터 6개: 일반 GEMM → 창(s 1) → 창(s 2) → 창(s 1) → 일반 GEMM 두 번.
  - 기존 벡터 5종(random, hazard, conv, biashazard, reload)도 `DWU=1` 판에서 모두 PASS다. `GAP=40 BP=50`(uop 공백, done 역압)에서도 PASS다.
- vtalib 시뮬레이터 (`tests/run_dw_test.sh`, nestc-build container):
  - `tests/dw_evta_test.cc`의 15모양: MobileNetV2Tf의 depthwise 10개(지금 schedule)와 홀수 크기, tile 경계, stride 2 + padding.
  - 창 모드와 대각 판 모두 CPU loop와 0 differ다. `run_epi_test.sh`도 그대로 통과한다.
  - 처음에는 창 모드만 순서에 따라 틀렸다. 시험이 buffer를 해제하고 다시 잡을 때 pool이 같은 주소를 돌려주어, 주소로만 찾던 weight 변환 cache가 옛 weight를 냈다(위 §5).
- bitstream: `make bin JOBS=12 SYSTOLIC=1 ALUX=1 DWU=1 FREQ=400 IMPL_STRATEGY=Performance_ExplorePostRoutePhysOpt BUILD_TAG=rq_alux3`
  (`vta-hw-zcu102-systolic/hardware/xilinx`, container `vivado2020-build`) → `..._sa_alux_dw_400mhz_rq_alux3`. 보드 이름 `..._sa_alux3_dw_400mhz_rq.bin`.
  - 첫 빌드(11-25 01:04): WNS −0.145 ns, 실패 끝점 1008개. LUT 69.8k → 73.6k(+3.8k), FF 92.0k → 94.6k, BRAM 258 → 259, DSP 303.
    실패 경로는 대부분 기존 ALU(compute) ↔ acc_mem(608개)이고 gemm_sa 안은 16개뿐이었다. 그중 sa_dw 것은
    `ctrl/flag_p[2]` → 16 lane의 B2 / window register CE(−0.112 ns): flag / dv 한 비트가 sa_array PE와 sa_dw 768 bit를 한꺼번에 구동했다.
  - 고침: sa_dw 입력(x, flag, dv, go, weight 24 bit)을 lane마다 한 번 등록한다(`keep`으로 lane별 복사 유지). 모든 입력이 함께 한 cycle 밀리므로
    순서는 같고, 지연선을 30 → 29로 줄여 출력은 그대로 a+33이다. xsim dw1–6, r1–r5, `GAP=40 BP=50` 모두 PASS.
    다시 빌드: `BUILD_TAG=rq_alux3b`.
  - 11-25: single 재빌드(`rq_alux3b`)는 멈췄다. `_alux3`은 LUT가 많아 400 MHz에서 여유가 없다. 대신 multi4 `_amul`(LUT 167.9k)에 넣는다.
    multi4로 만든 bitstream도 EVTA 하나만 써서 single처럼 돌릴 수 있다.
    - multi4 트리: `src/vta.cc` / `vta.h`는 systolic HEAD와 같았으므로 DWU 판을 그대로 복사했다. Makefile `DWU`, `hls.tcl -DVTA_DWU`,
      `vivado.tcl` gemm_sa_<i> `CONFIG.DWU` + UB(11 + 11 + 10 + 30 + 2 = 64 bit, 폭 그대로). RTL은 systolic `rtl/`을 같이 쓴다.
    - 빌드: `vta-hw-zcu102-multi4`에서 `hardware/xilinx/scripts/y219route/flow/m4_build.sh amuldw VTA_PLACE_DIRECTIVE=ExtraNetDelay_high
      SYSTOLIC=1 ALUMUL=1 DWU=1 FREQ=400 IMPL_STRATEGY=Performance_ExtraTimingOpt` (`_amul`과 같은 설정 + `DWU=1`).
  - 기다리는 동안 single 첫 빌드(고치기 전 sa_dw, 400 MHz −0.145 ns)의 PLL만 바꿨다: `flow/mmcm_set.tcl <postroute_physopt.dcp> dw375 11.25`
    (VCO 1200 → 1125 MHz, /3 = 375 MHz), WNS +0.020 ns. 보드 `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_alux3_dw_375mhz_rq.bin`
    (`bit/evta_single_alux3_dw_375MHz_261125/`, PC `build/single_dw375/`).
  - 보드(이 375 MHz 판, OMP 1):
    - `dw_evta_test` 창 모드 15모양 0 differ.
    - MobileNetV2Tf `EVTA_DUMP_CONV` 5200개가 `_alux3` 기준 dump(`~/JO/dwu/dump_base`)와 바이트까지 같다, 282.
    - 처음 잰 값: 1장 15.67(`EVTA_DWU=0`) → 14.66 ms. 576 / 960채널 층이 0.19 → 0.24 ms로 느려졌다. `dwWindowWeights`가 호출마다 kernel 전체를
      invalidate하고 다시 읽어 tap을 비교했기 때문이다. `VTABufferFree`에 hook(`VTABufferFreeHook`, api.cc와 simulator runtime.cc)을 두어
      해제될 때만 cache 항목을 지우게 했다(simulator `run_dw_test.sh` 두 모드 통과, 보드 dump 다시 0 differ).
    - 고친 뒤 (같은 bitstream, `EVTA_DWU=1` / `0`):

| | 대각 GEMM (`EVTA_DWU=0`) | 창 모드 |
|---|---|---|
| 1장 (`_sys`, 100회 평균) | 15.72 ms | **13.90 ms** |
| `_systimed` EVTA 호출 합 | 14.25 ms | 12.43 ms |
| depthwise 17개 합 | 6.05 ms | **4.21 ms** |

    - 112 × 112 층 1.05 → 0.53, 56 × 56 0.97 → 0.53 ms. 7 × 7 × 960 등 작은 층은 0.20 → 0.17 ms.
      400 MHz `_alux3`(dw 없음) 14.40 ms보다 375 MHz에서 더 빠르다.
  - multi4 `amuldw` (11-25 15:38): Vivado 배선 직후 WNS 0.000 ns(400 MHz). RapidWright는 행 Y219의 net 549개(`_amul` 382개)에서 99회 뒤 멈췄다
    (PIP 22개 겹침, sink 119개 남음). 그 뒤 `route_s.tcl`: WNS −5.363 ns, 행 안 net 7개(고장 셀 위 3개). 쓸 수 없다.
    RapidWright를 `-Dcrit=0 -Drwargs="--maxIterations 250 --enlargeBoundingBox"`로 다시(`build/y219/rw2_mamuldw.sh`, tag `mamuldw2`).
    - 그 재시도는 수렴하지 않았다: 13회 동안 겹침 175k → 385k로 늘고 회당 3–4분(넓힌 bounding box로 다시 잇는 연결이 1.5k → 16k). 멈췄다.
    - `rs_mamuldw`에서 깨진 경로의 net만 Vivado로 다시 배선(`build/y219/fixrt.tcl`): −5.858 ns, 깨진 끝점 15,028, 행 net 43. 더 나쁘다.
      깨진 2000경로 중 1678개가 행 아래 compute_1 안(fanout 152 net이 Y207 → Y169에 배선 7.4 ns): RapidWright가 행 net을 행 옆으로 돌리며
      그 아래 compute_1의 배선 자원을 차지했다.
  - **원인과 해결 (`VTA_CONTAIN=1`, 11-25 저녁).** 행 net 549개를 driver → load 계층으로 나눴다(`build/y219/rowcls.tcl`):
    compute_1 내부 156, gemm_sa_1 47, acc_mem_1 30, load_1 등 ≈ 90 / crossbar·`reg_dwc` 내부 ≈ 70 / smc_2·3 ↔ hpslice2·3(건너야 하는 HP) ≈ 100.
    EVTA 1(행 바로 아래, 좁은 아래쪽에 빽빽이 놓임)의 안쪽 net이 행의 PIP를 빌려 썼다. `place_d.tcl`이 모든 pblock의 `CONTAIN_ROUTING`을
    끄고 있었다(2026-10-24: 두 사각형 `pb_fence`는 사이 행을 못 막고, 자식 pblock에 켜면 RTSTAT-6 배선 실패였다; `docs/vta_synth_notes.md`).
    - `place_d.tcl` `VTA_CONTAIN=1`(`run_flow.sh`가 넘김): 한 사각형 pblock `pb_evta_0..3`, `pb_hpslice`와 새 `pb_lo`(crossbar, `reg_dwc`,
      `proc_sys_reset_ps`, 행 아래)에 `CONTAIN_ROUTING`을 켠다. 이번에는 배선 실패가 없었다.
    - `VTA_CONTAIN=1 VTA_PLACE_DIRECTIVE=ExtraNetDelay_high VTA_IMPL_DIR=<amuldw impl_1> run_flow.sh mamuldwc` (같은 opt_design 결과):

| | `mamuldw` | `mamuldwc` (`VTA_CONTAIN=1`) |
|---|---|---|
| Vivado 배선 직후 WNS | 0.000 ns | +0.004 ns |
| 행 Y219의 net (RapidWright 전) | 549 | **29** |
| RapidWright 뒤 남은 sink | 119 + PIP 겹침 22 | 76, 겹침 없음 |
| `route_s.tcl` 뒤 행 net / 고장 셀 net | 7 / 3 | 0 / 0 |
| EVTA 400 MHz / HP 333 MHz WNS | −5.363 / −1.455 ns | −0.016 / −0.476 ns |

    - `route_s.tcl rs_mamuldwc.dcp rs_mamuldwc_o opt`: EVTA +0.002 ns, HP −0.261 ns(PS `SAXIGP5` rdata → `hpslice3`), 행 net 0.
    - `mmcm_hp_set.tcl rs_mamuldwc_o.dcp rs_mamuldwc_h 43.75`: HP 304.3 MHz. **WNS +0.002, WHS 0.000 ns**, 행 net 0,
      `stuckchk.py -b build/single_dw375/dw375.bit` 0 beyond the base.
    - 보드 `vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta_sa_amul_dw_400mhz_hp304_rq.bin` (md5 8ebdfbb4, `bit/evta_multi4_amul_dw_400MHz_261125/`,
      PC `build/multi4_amuldw/`). probes 40/40.
  - 보드 multi4 `amuldw` (11-25 22시, PC cross build `build_pc_m4_sysdw[t]` `_pcsysdw[t]`, ubun `1cdf422` vtalib에 dw를 합친 것, 보드에서
    19시에 다시 만든 MultiEVTAMobileNetV2TfTest bundle, int8 입력, OMP 1, 25회):
    - `EVTA_RES_CHECK=1`: 4개 동시 144건, 혼자 152건 0 differ, 네 copy 모두 282 (0.322088).
    - 같은 실행 파일 `_pcsysdwt`, 세 판을 번갈아 3번:

| | `_amul` hp322 | `amuldw` hp304, `EVTA_DWU=0` | `amuldw` hp304, 창 모드 |
|---|---|---|---|
| 4개 동시 (fps) | 262.6 / 261.5 / 256.1 (260.1) | 260.0 / 260.1 / 261.6 (260.6) | 285.7 / 291.3 / 285.3 (**287.4**) |
| 혼자 1장 (multi4 copy 0, 100회) | 13.54 ms | 13.71 ms | **12.09 ms** |
| copy당 cpu conv/fc (4개 동시, 가장 느린 copy) | 6.71–6.76 ms | 6.73–6.76 ms | 5.26–5.37 ms |

    - 창 모드로 4개 동시 +10.5 %(260 → 287 fps): **DPU S 270 fps보다 빠르다**. HP 304 MHz(322 → 304)는 혼자 1장 +0.17 ms, 4개 동시는 잡음 안.

## 9. 정리 (2026-11-25): 무엇을 바꿨고, 빌드가 어떻게 성공했나

용어: **WNS**(worst negative slack) = 가장 나쁜 경로가 클럭 주기보다 남긴 시간(0 이상이면 타이밍 통과), **WHS** = hold의 같은 값,
**pblock** = Vivado에서 cell을 놓을 수 있는 칩 영역, **PIP** = 배선 스위치 하나, **행 Y219** = 이 보드 PL의 고장 난 configuration 행
(그 행의 PIP를 쓰면 값이 0에 붙는다), **RapidWright** = 행 Y219를 피해 다시 배선하는 Java 도구(`Y219RWRoute.java`).

### 9.1 바뀐 것

| 층 | 파일 | 내용 |
|---|---|---|
| ISA | `include/vta/hw_spec.h`(systolic, multi4), vtalib 두 `hw_spec.h` | `VTAGemInsn.dw_win`(pad bit 1개), uop `dw` / `emit` 두 bit(§3) |
| HLS compute | `src/vta.cc` / `vta.h`(systolic, multi4 같은 내용), `hls.tcl -DVTA_DWU` | uop에 bit 두 개를 싣고, C 모델 `gemm_sa_csim`에 창 모드 |
| RTL | 새 `rtl/sa_dw.v`(78줄), `sa_ctrl.v`, `gemm_sa.v`, `gemm_sa_bd.v`, `gen_gemm_sa_bd.py` | 16 lane × 3탭 창 곱셈기, uop FIFO / tag에 dw / emit, row 출력 mux. 파라미터 `DWU`(기본 0이면 예전 회로 그대로) |
| 빌드 | Makefile `DWU=1`(이름에 `_dw`), `vivado.tcl` gemm_sa `CONFIG.DWU` | uop TDATA 폭은 그대로(single 72, multi 64 bit 안에 들어감) |
| multi4 배치 | `y219route/flow/place_d.tcl`, `run_flow.sh` | `VTA_CONTAIN=1`: pblock `CONTAIN_ROUTING`(§9.3) |
| 시험 | `sim/dwu_test.cc`, `run_csim.sh DWU=1`, `rtl/tb/gen_dw_vectors.py`, `tb_gemm_sa.v`, `run_xsim.sh` | C 모델과 RTL의 창 모드 시험 |
| vtalib-systolic | `VTABundle.cpp`(`dw_convolution_evta` 창 판, `dwWindowWeights`), `uop_kernel.h` `gemmop_dw`, `api.cc` `VTABufferFreeHook`, 시뮬레이터 `sim_driver.cc` / `runtime.cc` | `VTAHasDwUnit()`(`_dw` 이름, `EVTA_DWU=0\|1`), weight 대각 → 블록 × ky entry 변환(처음 한 번, cache), 시험 `tests/dw_evta_test.cc`, `run_dw_test.sh` |

bundle과 컴파일러는 바뀌지 않았다. 같은 실행 파일이 `_dw`가 없는 bitstream에서는 예전 대각 GEMM으로 돈다.

자원(multi4, 배선 뒤):

| | `_amul` (11-23) | `_amul_dw` (11-25) |
|---|---|---|
| LUT | 167.9k (62 %) | 173.2k (64 %), EVTA 하나당 +1.3k |
| FF | 215.3k | 220.3k |
| BRAM36 | 580 | 584 |
| DSP | | 1,412 (곱셈기는 LUT로 만들었다) |

### 9.2 확인한 순서

1. C 기준 모델: `sim/run_csim.sh DWU=1` 8경우가 직접 계산한 depthwise conv와 같다.
2. RTL xsim: 창 벡터 6개 + 기존 5종, 역압(`GAP=40 BP=50`)에서도 PASS.
3. vtalib 시뮬레이터: `tests/run_dw_test.sh`, MobileNetV2Tf의 depthwise 10모양 + 홀수 모양 5개, 창 / 대각 둘 다 0 differ.
4. single 보드(375 MHz 판): `dw_evta_test` 0 differ, MobileNetV2Tf conv dump 5,200개가 `_alux3`과 바이트까지 같다.
5. multi4 보드: probes 40/40, `EVTA_RES_CHECK=1`(EVTA 결과를 CPU와 비교) 4개 동시 / 혼자 모두 0 differ, 282.

### 9.3 빌드: 실패한 것과 성공한 방법

**single (`_alux3` + DWU, 400 MHz).** 첫 빌드 WNS −0.145 ns. sa_dw 쪽 실패는 flag / dv 한 bit가 768 bit의 register를 한꺼번에 구동한 것이었다.
lane마다 입력을 한 번 더 등록해 고쳤다(지연선 30 → 29, 출력 시각 a+33 그대로). 다만 대부분의 실패는 `_alux3`의 ALU ↔ acc_mem 경로라
400 MHz는 어렵다고 보고 다시 빌드는 멈췄다. 첫 빌드의 PLL만 375 MHz로 낮춰(`mmcm_set.tcl ... 11.25`) WNS +0.020 ns로 보드 시험에 썼다.

**multi4 (`_amul` + DWU, EVTA 400 / HP 333 MHz).** multi4는 BRAM이 모자라 고장 행 Y219 위아래에 걸친다. 흐름은
Vivado 배치 / 배선 → 행 Y219에 PIP가 있는 net을 RapidWright로 다시 배선 → Vivado `route_s.tcl`로 나머지 배선이다(README 참고).

1. 첫 시도(`mamuldw`): Vivado 배선 직후 WNS 0.000 ns였지만 **행에 PIP가 있는 net이 549개**(`_amul`은 382개)였다. RapidWright가 99회 뒤
   멈췄고(겹침 22, 남은 sink 119), `route_s.tcl` 뒤 WNS −5.363 ns, 행 안 net 7개. 쓸 수 없다.
2. RapidWright 반복을 늘리고 bounding box를 넓혀 다시: 겹침 175k → 385k로 늘어 수렴하지 않았다.
3. 깨진 경로의 net만 Vivado로 다시 배선: −5.858 ns로 더 나빠졌다. 깨진 경로 대부분이 행 바로 아래 EVTA 1(compute_1) 안이었다.
   RapidWright가 행 net을 행 옆으로 돌리면서 그 아래 compute_1의 배선 자원을 차지한 것이다.
4. **원인.** 행 net 549개를 출발 / 도착 블록으로 나눠 보니(`build/y219/rowcls.tcl`) 대부분 행을 건널 필요가 없는 net이었다:
   EVTA 1 내부(compute_1 156, gemm_sa_1 47, acc_mem_1 30 …), crossbar / 폭 변환기 내부 약 70. 꼭 건너야 하는 것은 위쪽 EVTA 2 / 3의
   HP 경로 약 100개뿐이었다. EVTA 0 / 1은 행 아래, 2 / 3은 위 pblock에 놓여 cell은 잘 나뉘어 있었지만, `place_d.tcl`이 모든 pblock의
   `CONTAIN_ROUTING`(net을 pblock 안 배선 자원으로만 잇게 하는 속성)을 끄고 있어 배선기가 행 Y219의 PIP를 지름길로 빌려 썼다.
   - 끈 이유(2026-10-24, `docs/vta_synth_notes.md`): 위아래 두 사각형인 바깥 `pb_fence`에 켜도 두 사각형 사이 행은 막히지 않았고,
     그때 자식 pblock에 켜면 배선 실패(RTSTAT-6)였다. 그 뒤 줄곧 꺼진 채였다.
   - EVTA 1이 특히 많은 까닭: 행 바로 아래 좁은 영역에(왼쪽 아래는 PS가 차지) 빽빽이 놓여, 행의 자원이 가장 가까운 여유였다.
     EVTA 0 / 2 / 3의 net은 대부분 제 영역 안에서 이어졌다.
5. **해결 (`VTA_CONTAIN=1`).** 한 사각형인 pblock(`pb_evta_0..3`, `pb_hpslice`)과 새 `pb_lo`(crossbar, `reg_dwc`, PS 쪽 reset; 행 아래)에만
   `CONTAIN_ROUTING`을 켰다. 두 사각형 `pb_fence`는 그대로 둔다. crossbar 등을 `pb_lo`에 넣은 것은 그것들이 `pb_fence`(두 사각형)에만 속해
   있으면 막을 수 없기 때문이다. 이번에는 배선 실패가 없었다. 같은 `opt_design` 결과에서 배치부터 다시 했다:

| 단계 (`mamuldwc`) | 걸린 시간 | 결과 |
|---|---|---|
| `run_flow.sh`: 배치 + Vivado 배선 | 2 h 17 m | WNS +0.004 ns, **행 net 549 → 29** |
| RapidWright | 약 2 m | 남은 sink 76, 겹침 없음 |
| `route_s.tcl` | 9 m | 행 net 0, EVTA −0.016 / HP −0.476 ns |
| `route_s.tcl ... opt` (post-route phys_opt) | 27 m | EVTA +0.002, HP −0.261 ns (PS `SAXIGP5` rdata → `hpslice3`), 행 net 0 |
| `mmcm_hp_set.tcl ... 43.75` (HP 클럭만 304.3 MHz) | 7 m | **WNS +0.002, WHS 0.000 ns**, 행 net 0 |
| `stuckchk.py` | | 고장 셀 위 bit 0개(기준 bit 대비) |

HP 클럭은 PS의 HP 포트와 EVTA의 DMA 사이 클럭이다. 322 → 304 MHz로 낮춘 영향은 혼자 1장 +0.17 ms, 4개 동시에는 측정 잡음 안이었다.

배운 것:
- multi4 빌드에는 앞으로 `VTA_CONTAIN=1`을 쓴다(`y219route/README.md`). 행 net이 적을수록 RapidWright가 옮길 것이 적고, 옮긴 net이
  다른 EVTA의 자원을 빼앗지 않는다. `_amul` 등 예전 multi4 판도 이것으로 다시 빌드하면 HP를 더 높이 맞출 수 있을지 모른다(해 보지 않았다).
- RapidWright가 수렴하지 않을 때 반복이나 bounding box를 늘리는 것은 효과가 없었다. 먼저 행 net이 왜 많은지 나눠 본다(`rowcls.tcl`).
- Vivado tcl `get_pblocks -regexp {a|b}`는 `|`가 듣지 않아 하나만 잡혔다. 이름을 목록으로 준다.

### 9.4 다시 만드는 명령 (ubuntu2, host)

```bash
cd vta-hw-zcu102-multi4
# 1) HLS + 합성 + opt_design (impl_1), 그다음 run_flow.sh까지
VTA_CONTAIN=1 VTA_PLACE_DIRECTIVE=ExtraNetDelay_high hardware/xilinx/scripts/y219route/flow/m4_build.sh amuldw \
    SYSTOLIC=1 ALUMUL=1 DWU=1 FREQ=400 IMPL_STRATEGY=Performance_ExtraTimingOpt      # 환경 변수는 run_flow.sh가 읽는다
#    (이번에는 opt_design 결과를 두고 배치부터: VTA_CONTAIN=1 VTA_PLACE_DIRECTIVE=ExtraNetDelay_high VTA_IMPL_DIR=<amuldw impl_1> run_flow.sh mamuldwc)
# 2) build/y219에서 (container vivado2020-build, scripts/y219route/flow/)
vivado -mode batch -source route_s.tcl     -tclargs rs_mamuldwc.dcp rs_mamuldwc_o opt
vivado -mode batch -source mmcm_hp_set.tcl -tclargs rs_mamuldwc_o.dcp rs_mamuldwc_h 43.75     # HP 6.956 x 43.75 = 304.3 MHz
# 3) 확인과 .bin (stuckchk는 numpy가 있는 nestc-build container; 경로는 저장소 기준)
python3 vtabundles_xcmpl/hwtest/brambist/stuckchk.py rs_mamuldwc_h.bit -b <행 아래만 쓴 single bit>
python3 vtabundles_xcmpl/hwtest/bit2bin.py rs_mamuldwc_h.bit vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta_sa_amul_dw_400mhz_hp304_rq.bin
```

보드: `/lib/firmware/vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta_sa_amul_dw_400mhz_hp304_rq.bin` (md5 8ebdfbb4), 사본과 bit / 보고서는
`bit/evta_multi4_amul_dw_400MHz_261125/`. single 375 MHz 판은 `..._16_16_19_18_sa_alux3_dw_375mhz_rq.bin`(`bit/evta_single_alux3_dw_375MHz_261125/`).
실행: `BIN=<그 파일> hw_multievtax4/script/run_prof.sh -s _pcsys... -n 25 MultiEVTAMobileNetV2TfTest`(systolic vtalib로 만든 실행 파일).

### 9.5 성능 (MobileNetV2Tf, int8 입력, OMP 1)

| | 전 | 창 모드 | |
|---|---|---|---|
| single 375 MHz 판, 1장 | 15.72 ms (`EVTA_DWU=0`) | **13.90 ms** | depthwise 17층 6.05 → 4.21 ms. 400 MHz `_alux3`(14.40 ms)보다 빠르다 |
| multi4, 혼자 1장 (copy 0) | 13.54 ms (`_amul` hp322) | **12.09 ms** | |
| multi4, 4개 동시 | 260.1 fps (`_amul` hp322) | **287.4 fps** (+10.5 %) | AMD DPU S(B4096 1개) 270 fps보다 빠르다 |

- 같은 bitstream에서 `EVTA_DWU=0`(대각 GEMM)은 260.6 fps로 `_amul`과 같다: 이득은 모두 창 모드에서 왔다.
- 4개 동시에서 copy당 EVTA conv 시간이 6.7 → 5.3 ms로 줄었다.
- 2026-11-26부터 multi4 기본 `SYS_BIN`이 이 판이다(`hw_multievtax4/script/run_all.sh`, `collect_hw_bundles.py`).
- 남은 것: DPU 비교 문서([evta_dpu_compare.md](evta_dpu_compare.md)) 갱신, PP8(§7).
