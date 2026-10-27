# EVTA 4개 동시 실행 609.7 GOPS 측정 보고서 (ResConv10, ZCU102)

작성: 2026-10-26. 측정: 보드 ZCU102(192.168.20.2), 커밋 `bc2abf36`(측정 프로그램) 기준.

## 1. 요약

- ZCU102 PL에 넣은 EVTA 4개(multi4 비트스트림 rs_rst, 333 MHz)에서 **3x3 conv(7x7x512 → 512)를 4개 EVTA가 동시에
  각자 400번 돌린 처리량이 609.7 GOPS**였다. 4개의 출력 모두 golden과 같았다. 같은 조건으로 3번 재서 609.6~609.7 GOPS였다.
- EVTA 1개일 때는 152.6 GOPS였다. 4개가 동시에 돌아도 EVTA 1개당 시간이 그대로여서 처리량이 정확히 4배가 됐다.
  EVTA 하드웨어 시간(VTA_SYNC)도 conv 한 번에 1.4473 ms(1개) → 1.4491 ms(4개 동시)로 0.12%만 늘었다.
  **HP 포트·DDR 경쟁이 없다.**
- 이론 최대(EVTA당 16x16 MAC x 2 ops x 333.3 MHz = 170.7 GOPS, 4개 682.7 GOPS)의 **89.3%**다.
  - 잃는 부분 중 약 4.4%는 conv 호출 사이 CPU 시간(명령어 생성과 시작/완료 처리, 66 µs)이다.
  - 약 6.2%는 EVTA 안에서 GEMM이 아닌 시간(첫 load, ALU 후처리, store)이다.
  - EVTA 하드웨어 시간만 보면 GEMM 가동률은 93.5%다.
- 이 값은 **같은 conv를 EVTA 4개가 독립적으로 돌린 처리량(throughput)**이다. conv 하나를 4개로 나눈 지연시간(latency)이 아니며,
  CPU 쪽 transpose와 버퍼 할당은 빠져 있다. 그것까지 넣으면(번들 그대로 반복) 4개 합계 296.6 GOPS다.
- 측정할 때 `OMP_NUM_THREADS=1`이 필요하다. 기본값(스레드마다 OpenMP 4개)에서는 4개 스레드가 4코어를 나눠 쓰면서
  CPU 쪽 명령어 생성이 늦어져 558~579 GOPS로 낮게, 들쭉날쭉 나왔다.

## 2. 실험 환경

| 항목 | 값 |
|---|---|
| 보드 | ZCU102(XCZU9EG), PYNQ 2.4 / Ubuntu 18.04, Cortex-A53 4코어 1.2 GHz, CMA 128 MB |
| 비트스트림 | `/lib/firmware/vtahw_multi4_y219.bin` = `bit/evta_multi4_y219_333MHz_pass_261026/vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta.bin` (rs_rst, md5 `86843c76...`, WNS +0.008 ns, 소스 커밋 `dfec80f7`) |
| EVTA 구성 | 4개, 각각 GEMM 1x16 · 16x16 int8(256 MAC/cycle), 버퍼 log2 바이트 uop/inp/wgt/acc = 16/15/18/17, 333.3 MHz, EVTA i는 `S_AXI_HP<i>_FPD`(128비트)로 DDR 접근 |
| 비교 | multi2 비트스트림 `vtahw_multi2_y219.bin`(EVTA 2개, 같은 버퍼 크기, 333 MHz) |
| 런타임 | `vtalib-zcu102-multi4`(=`vta/vtalib` f716ed3와 같은 내용), `NESTC_EVTA_MULTI=ON`, `NESTC_EVTA_MAX_XFER=4194304`, non-coherent(flush/invalidate) |
| 빌드 | `vtabundles_xcmpl/build_multievtax4`(일반), `build_multievtax4_prof`(`NESTC_EVTA_PROFILE=ON`, VTA_SYNC 기록), clang++-8 `-O3` |
| 측정 프로그램 | `vtabundles_xcmpl/hw_multievtax4/MultiEVTAConv4TestSrc/`(`make_multi4conv4.py`, `multi4conv4Main.cpp`) → `hw_multievtax4/MultiEVTAResConv10Test/multi4resconv10Bundle` |
| 실행 | 실행마다 비트스트림 재로드와 `xlnk_reset`, `/tmp/evta_run.lock`으로 다른 EVTA 실행과 겹치지 않게 함 |

## 3. ResConv10Test의 구조

### 3.1 정의

`lib/Backends/VTA/tests/VTASaverTest.cpp`의 `TEST(VTASaverTest, ResConv10Test)`가 만드는 conv 하나짜리 테스트다.
Resnet18 stage4의 3x3 conv(`resnetv10_stage4_conv1/3/4`)와 모양이 같다.

```
conv_test_template(N=1, H=7, W=7, C=512, input_scale=1, KN=512, KH=3, KW=3, KC=512, filter_scale=1,
                   bias_scale=1, stride=1, pad=1, group=1, input [0,10], filter [-10,10], bias [0,32768],
                   "vtaResConv10Test", out1, out2)            // 출력 scale 128
```

| 텐서 | 모양 | 형식 | 값 |
|---|---|---|---|
| 입력 `inputP` | 1 x 7 x 7 x 512 (NHWC) | int8, scale 1 | 난수 0..10 |
| 필터 `filterP` | 512 x 3 x 3 x 512 (KN, KH, KW, C) | int8, scale 1 | 난수 −10..10 |
| bias `biasP` | 512 | int32, scale 1 | 난수 0..32768 |
| 출력 `outP` | 1 x 7 x 7 x 512 | int8, scale 128 | |

- 양자화 규칙: 출력 = clip((Σ 입력·필터 + bias) >> 7, −128, 127). shift 7은 scale 비율 1·1/128에서 나온다.
  relu는 없다.
- golden(`vtaResConv10TestGolden.bin`)은 시뮬레이터 빌드에서 VTA 백엔드(시뮬레이터)로 계산한 출력이다.
  테스트는 같은 그래프를 VTAInterpreter로도 계산해 둘이 같은지 본다.
- 연산량: 2 x (N·out_h·out_w) x KN x (C·KH·KW) = 2 x 49 x 512 x 4608 = **231,211,008 ops(231.2 Mops)**.
  MAC 하나를 곱셈과 덧셈 2 ops로 센다.

### 3.2 생성된 번들 코드 (`vtaResConv10TestBundle.cpp`)

```cpp
// 상수 2,361,344 B = 필터 2,359,296 + bias 2,048, 가변 50,176 B = 입력 25,088 + 출력 25,088
void vtaResConv10TestMainEntry_load_module(uint8_t *constantWeight){
  filterP = VTABufferAlloc(2359296);  VTABufferCopy(constantWeight + 0, ..., filterP, ..., 2359296, 1);
  biasP   = VTABufferAlloc(2048);     VTABufferCopy(constantWeight + 2359296, ..., biasP, ..., 2048, 1);
}
int vtaResConv10TestMainEntry(constantWeight, mutableWeight, activations){
  conv_input_transpose = VTABufferAlloc(25088);
  transpose_nhwc2vtaio(inputP, conv_input_transpose, 1, 7, 7, 512);     // CPU: NHWC -> N, C/16, H, W, 16
  VTABufferFlush(conv_input_transpose, 0, 25088);                       // non-coherent: CPU 캐시 -> DDR
  conv_output_bef_transpose = VTABufferAlloc(25088);
  convolution_wo_tr(conv_input_transpose, filterP, biasP, conv_output_bef_transpose,
                    1, 7, 7, 512, 512, 3, 3, /*pad*/1, /*stride*/1, /*relu*/0, /*bias*/1, /*shift*/7,
                    /*out_h, out_w*/7, 7, vtaCmdH, /*nVirtualThread*/2, /*tile H, W*/7, 7, /*EVTA*/0);
  VTABufferInvalidate(conv_output_bef_transpose, 0, 25088);             // DDR -> CPU
  transpose_vtaio2nhwc(conv_output_bef_transpose, outP, 1, 7, 7, 512);  // CPU
  VTABufferFree(...) x 2;
}
```

- 필터는 `VTASave.cpp`가 번들을 만들 때 이미 EVTA 레이아웃(KN/16, C/16, KH, KW, 16, 16)으로 바꿔 weights.bin에 넣는다.
  실행할 때는 입력과 출력만 CPU가 바꾼다.
- 타일 인자(`nVirtualThread` 2, tile 7x7)는 `include/Backends/VTA/VTASchedules.h`의 스케줄에서 온다.
  multi4 Resnet18 번들의 stage4 conv도 같은 인자다.

### 3.3 EVTA에서의 동작 (`vta/vtalib/lib/Bundle/VTABundle.cpp`)

`convolution_wo_tr`(2547행)은 출력을 tile로 나눠 tile마다 `conv_kernel`(342행)을 부르고, 끝에서 `VTASynchronize`로
한 번 실행하고 기다린다(2703행). ResConv10은 출력이 7x7이고 tile도 7x7이라 **tile이 1개**(`conv_kernel` 1번 호출)다.

`conv_kernel`의 값(커널 상수 `ACC_MEM_ENTRY 2048`, `INP_MEM_ENTRY 2048`, `WGT_MEM_ENTRY 1024`는 multi 버퍼 깊이와 같다):

| 값 | 계산 | ResConv10 |
|---|---|---|
| 입력 tile | (7−1)·1 + 3 | 9 x 9 (위아래 좌우 pad 1 포함) |
| `nFilterinLoop` | min(KN/2/16, 2048/2/49) = min(16, 20), 32 % (16·2) == 0 | 16 (가상 스레드 하나가 출력 채널 16블록 = 256채널) |
| 필터 반복 `filterIdx` | KN/16/2/16 | 1번 |
| 입력 채널 반복 `channelIdx` | C/16 | 32번 |

명령 흐름(가상 스레드 t = 0, 1):

```
GEMM reset_reg: acc[t] = 0                                   (t마다 1개)
for c in 0..31:                                              # 입력 채널 16개씩
    t=0: LOAD INP  9x9 (DRAM 7x7 + pad), inp SRAM 두 영역(0 / 1024)을 번갈아 씀
    t=0,1: LOAD WGT  KH·KW x 16블록 = 144 원소 x 256 B = 36,864 B -> wgt SRAM [t·512, t·512+144)
    t=0,1: GEMM  uop 루프 49 출력 x 9 커널 위치 x 16 출력블록 = 7,056 GEMM -> acc[t]
for t: LOAD ACC bias(16 원소), ALU ADD bias, SHR 7, MAX -128, MIN 127
for t: STORE 16번(출력블록마다 7x7)
```

- 의존성 토큰(`VTADepPush/Pop`): load → compute(1→2)와 compute → load(2→1)로 오간다. 가상 스레드 2개가 서로 다른 wgt/acc
  영역을 쓰므로, **한 스레드가 GEMM을 하는 동안 load 모듈은 다른 스레드의 다음 weight를 읽는다.** 입력도 double buffer다.
- SRAM 사용량(EVTA 하나): acc 2 x (784 출력 + 16 bias) = 1,600 / 2,048, wgt 2 x 144 = 288 / 1,024, inp 81 x 2영역 / 2,048.
- 명령어 수는 직접 세면 약 200개다(reset 2, channel 반복 160, bias load 2, ALU 8, STORE 32, FINISH 1. 덤프로 확인하지는 않음).
  uop 커널은 `vGemmUOpHandle` 등에 캐시돼서 두 번째 호출부터는 다시 만들지 않는다.
- conv 한 번에 EVTA가 DDR에서 읽고 쓰는 양:
  - weight 2,359,296 B (전부 정확히 한 번)
  - 입력 32 x 49 x 16 = 25,088 B
  - bias 2,048 B
  - 출력 25,088 B
  - 합계 약 2.41 MB / 1.449 ms = **EVTA당 1.66 GB/s**, 4개 6.65 GB/s
  - HP 포트 하나는 128비트 x 333 MHz = 5.33 GB/s다.

### 3.4 GEMM 가동률

- GEMM 명령의 총 반복 수 = 2 스레드 x 32 x 7,056 = **451,584**(= 49 x 32 출력블록 x 32 입력블록 x 9). 사이클마다 1개라면
  451,584 / 333.33 MHz = **1.3548 ms**가 하한이다.
- 측정한 EVTA 시간(VTA_SYNC, run 시작부터 done까지) 1.4473 ms(1개) / 1.4491 ms(4개 동시) → **가동률 93.5%**.
  나머지 약 94 µs는 첫 load(GEMM이 시작하기 전 입력 1.3 KB + weight 2 x 36.9 KB), 끝의 bias load·ALU 4번·STORE 32번,
  명령 사이 파이프라인 빈틈이다(항목별로 따로 재지는 않았다).
- 비교: Conv4Test(56x56x64 → 64, 연산량 같음)는 tile 8x56이 7개이고 tile마다 입력 채널 반복이 4번뿐이다.
  - tile마다 reset → load/GEMM 4번 → ALU → STORE가 되풀이되어 채우고 비우는 시간이 7번 생긴다.
  - weight도 tile마다 다시 읽는다.
  - EVTA 시간은 1.864 ms(multi4) / 1.774 ms(single)로 가동률 73~76%다. 원인은 이 구조로 추정하며, 따로 재지는 않았다.
- 그래서 "채널이 많고 출력이 tile 하나에 들어가는" 3x3 conv가 가장 빠르다. §5.1의 layer 순위에서 상위가 모두
  7x7x512, 14x14x256 모양인 것과 맞다.

## 4. 측정 프로그램

### 4.1 4개 EVTA용 번들 만들기 (`make_multi4conv4.py`)

- VTASaverTest는 ResConv10을 EVTA 0용으로만 만든다. Glow를 다시 빌드하지 않으려고 생성된 번들을 이름만 바꿔 4벌을 만든다.
  - `vtaResConv10TestBundle<i>.cpp`
  - `vtaResConv10TestMainEntry<i>.h`
- EVTA i 사본에서 바꾸는 것: 함수/변수 이름, 명령 핸들 `vtaCmdH<i>`, `convolution_wo_tr`의 마지막 인자(EVTA 번호).
  나머지 코드는 똑같다.
- 각 사본에 `<entry><i>_convonly(mutableWeight, n)`를 덧붙인다. 원래 entry 본문에서 `convolution_wo_tr` 한 줄만 n번 반복한다.
  ```cpp
  conv_input_transpose = VTABufferAlloc(25088); transpose_nhwc2vtaio(...); VTABufferFlush(...);   // 한 번
  conv_output_bef_transpose = VTABufferAlloc(25088);                                               // 한 번
  for (int k = 0; k < n; k++) convolution_wo_tr(..., vtaCmdH<i>, 2, 7, 7, <i>);                    // n번
  VTABufferInvalidate(...); transpose_vtaio2nhwc(...); VTABufferFree(...) x 2;                      // 한 번
  ```
- `multiconv.h`에 이름, 파일 이름, 연산량(`MC_OPS 231211008.0`, `convolution_wo_tr` 인자에서 계산)을 쓴다.
- 같은 스크립트로 `hw/<Name>Test`에 있는 conv 하나짜리 번들이면 무엇이든 만들 수 있다(Conv4Test → MultiEVTAConv4Test).
- `collect_hw_bundles.py --set multievtax4|multievtax2`가 이 스크립트를 돌리고 `run_all.sh`에 실행 줄을 넣는다.

### 4.2 동시 실행과 시간 측정 (`multi4conv4Main.cpp`)

```
main: mask의 EVTA마다 스레드 생성; barrier start/end (참가자 = EVTA 스레드 수 + main)
스레드 i: initVTARuntime<i>()            # 명령 핸들, CMA
          weights.bin 읽기, load_module   # 필터/bias를 CMA 버퍼로
          입력 복사, entry 1번(warm-up)   # uop 캐시와 명령 버퍼가 여기서 만들어짐
          barrier(start)
          t0 = clock_gettime(MONOTONIC)
          _convonly(400)                  # conv 모드; 아니면 entry 400번
          t1 = ...                        # EVTA별 시간 = t1 - t0
          barrier(end)
          출력 vs golden 비교, destroy
main:     barrier(start); t_start = now; barrier(end); t_end = now
GOPS(전체) = MC_OPS x 400 x EVTA 수 / (t_end - t_start)
```

- start barrier는 모든 스레드가 warm-up까지 마쳐야 풀린다. end barrier는 가장 늦은 EVTA가 끝나야 풀린다.
  그래서 `t_end − t_start`는 "4개가 같이 출발해서 마지막 하나가 끝날 때까지"의 wall 시간이다.
  barrier가 풀리는 시각과 `clock_gettime` 사이에는 µs 정도의 차이만 있다.
- 전체 GOPS는 EVTA별 GOPS의 합이 아니다. wall 시간이 가장 느린 EVTA로 정해지므로 합보다 작거나 같다.
- `convolution_wo_tr` 한 번 = CPU가 명령어를 만듦 + EVTA 실행 + `VTASynchronize`로 완료 대기. 그래서 한 EVTA 안에서는
  conv가 차례로 끝나고, conv 사이에는 그 EVTA가 쉰다.
- `_prof` 빌드(`NESTC_EVTA_PROFILE=ON`)는 `VTASynchronize`마다 EVTA run 시작부터 done까지의 시간과 타임스탬프
  (VTA_SYNC)를 기록하고, 종료할 때 EVTA마다 표로 찍는다.

## 5. 결과

### 5.1 단일 EVTA에서 가장 빠른 layer (single `_timed`, 2026-10-26)

`hw*/*/timing*.csv`(번들 문장별 시간)의 conv를 모두 GOPS 순으로 정렬했다(마지막 실행 기준).

| GOPS | 시간 | layer | 번들 |
|---|---|---|---|
| 152.5 | 1.516 ms | conv (7x7x512 → 512) | ResConv10Test |
| 150.8 | 1.534 ms | res5_2_branch2b | Resnet50Test |
| 149.8 | 1.544 ms | stage4_conv1 | Resnet18PartitionTest |
| 149.4~149.7 | 1.544~1.548 ms | stage4_conv1/3/4 | Resnet18Test, MultiEVTAResnet18Test |
| 148.1 | 1.561 ms | conv (14x14x256 → 256) | ResConv7Test |
| 146~146.6 | 1.578~1.584 ms | res4_x_branch2b, stage3_conv1/3/4 | Resnet50Test, Resnet18Test |
| 122~124 | 1.87~1.89 ms | stage1 (56x56x64 → 64) | Conv4Test, Resnet18 stage1 |

- 상위는 모두 연산량이 231.2 Mops인 3x3 conv다.
- MultiEVTAResnet18Test에서 본 149 GOPS는 동시 실행 값이 아니다. 4개 copy의 같은 layer 시작 시각이 최대 120 ms
  어긋나 있었다(예: stage4_conv1 401.5 / 475.9 / 526.2 / 404.3 ms). 그래서 이 측정을 따로 만들었다.

### 5.2 conv만 반복 (`OMP_NUM_THREADS=1`, 400회, 모두 golden 일치)

| 비트스트림 | 번들 | EVTA 1개 | EVTA 2개 | EVTA 4개 |
|---|---|---|---|---|
| multi4 rs_rst | ResConv10 | 152.6 (3회 152.62~152.65) | — | **609.7** (3회 609.61~609.71) |
| multi4 rs_rst | Conv4 | 123.5 | — | 491.6~493.4 |
| multi2 | ResConv10 | 152.8 | 305.5 | — |
| multi2 | Conv4 | 123.6 | 247.1 | — |

(단위 GOPS, —는 이번에 재지 않은 조합. multi4 비트스트림의 mask 3(EVTA 2개)은 `OMP_NUM_THREADS=1` 없이만 쟀다.)

609.7 GOPS를 낸 실행의 EVTA별 줄:

```
EVTA0: 400 x ResConv10 conv only 606.370 ms (1.516 ms each, 152.52 GOPS) output = golden
EVTA1: 400 x ResConv10 conv only 606.558 ms (1.516 ms each, 152.47 GOPS) output = golden
EVTA2: 400 x ResConv10 conv only 606.736 ms (1.517 ms each, 152.43 GOPS) output = golden
EVTA3: 400 x ResConv10 conv only 606.521 ms (1.516 ms each, 152.48 GOPS) output = golden
RESULT mask 0xf: 4 EVTAs x 400 runs (conv only) in 606.748 ms wall, 609.71 GOPS, all = golden
```

4 x 400 x 231,211,008 = 369.94 Gops / 606.748 ms = 609.71 GOPS.

### 5.3 EVTA 하드웨어 시간 (`multi4resconv10Bundle_prof`, VTA_SYNC, 400회 + warm-up)

| | EVTA 1개 | EVTA 4개 동시 |
|---|---|---|
| EVTA 시간(run 시작 → done) 평균 / 최소 / 최대 | 1.4473 / 1.446 / 1.449 ms | 1.4491 / 1.447 / 1.453 ms |
| 같은 EVTA의 연속 실행 사이 간격(CPU) | 67.2 µs | 66.0~67.0 µs |
| conv 한 번(시간 + 간격) | 1.515 ms | 1.516 ms |
| 전체 GOPS | 152.47 | 609.17 |

- 4개 동시 실행에서 400 x 4개 구간을 시간축에 겹쳐 보면, 측정 구간(606.4 ms)의 **82.8%에서 4개 모두 실행 중**이고
  16.7%에서 3개, 0.4%에서 2개다(평균 3.82개). 3개뿐인 시간은 각 EVTA의 CPU 간격(66 µs)이 서로 어긋나서 생긴다.
- 첫 실행의 시작 시각 차이는 4개 사이에 160 µs였다.
- EVTA 하드웨어만 보면 conv 한 번에 159.6 GOPS, 4개에 638 GOPS(이론 최대의 93.5%)다.

### 5.4 OpenMP 스레드 수의 영향

| 조건 | ResConv10 4개 | Conv4 4개 |
|---|---|---|
| `OMP_NUM_THREADS=1` | 609.6~609.7 | 491.6~493.4 |
| 기본값(스레드마다 OpenMP 4개, 4코어에 16개) | 557.9, 579.3, 576.4, 563.7 | 412.5, 440.9, 476.5, 475.1 |

- 기본값에서는 EVTA별 시간이 1.535~1.658 ms로 들쭉날쭉하다. 가장 느린 EVTA가 wall을 정해서 전체가 떨어진다.
- conv-only 반복에서 CPU가 하는 일은 명령어 생성과 동기화뿐이고, 이 일에는 OpenMP가 필요 없다.
  예전 Conv4 측정(489.6 GOPS)도 `OMP_NUM_THREADS=1`이었다.

### 5.5 번들 그대로(end-to-end) 반복

매번 버퍼 할당, CPU transpose, flush/invalidate, 해제를 하면(`multi4resconv10Bundle 15 100`, OMP 기본값):

- ResConv10은 EVTA마다 3.07~3.12 ms에 한 번, 4개 합계 **296.6 GOPS**다.
- Conv4는 8.30 ms에 한 번, 합계 111.3 GOPS다.
- 입출력 tensor가 큰 Conv4(200 KB)는 CPU transpose가 대부분을 차지한다.

## 6. 해석

- **메모리 대역폭은 병목이 아니다.**
  - EVTA 4개가 conv마다 2.4 MB씩, 합계 6.65 GB/s를 읽고 써도 EVTA 시간은 0.12%만 늘었다.
  - EVTA마다 자기 HP 포트를 쓰고, 한 포트에 1.66 GB/s(포트 용량의 31%)만 흐른다.
- **4배가 되는 조건**: 같은 모양의 독립된 conv 4개가 동시에 있고, CPU가 4개 EVTA에 명령을 제때 공급할 수 있어야 한다.
  `OMP_NUM_THREADS=1`이면 EVTA마다 CPU 코어 1개로 충분하다(간격 66 µs, conv의 4.4%).
- **남은 여지**:
  - CPU 간격 4.4%: 다음 conv의 명령어를 EVTA가 도는 동안 미리 만들어 두면(명령 버퍼 두 개) 4개 합계 약 638 GOPS까지 갈 수 있다.
    지금은 `VTASynchronize`가 끝나야 다음 명령 생성을 시작한다.
  - EVTA 안의 6.5%: tile 하나짜리라 이미 작다. ALU/STORE를 다음 conv의 load와 겹치는 것은 명령 흐름 구조를 바꿔야 한다.
- **실제 네트워크와의 차이**:
  - Resnet18 한 번은 conv 크기가 제각각이다.
  - CPU 연산(conv0, transpose, elemadd, softmax)이 대부분의 시간을 차지한다(Resnet18 4 copy 동시 실행은 CPU-bound).
  - 이 측정은 "EVTA 4개가 HW로 낼 수 있는 처리량"의 상한을 확인한 것이다.

## 7. 재현

```bash
# 보드, vtabundles_xcmpl/
./collect_hw_bundles.py --set multievtax4 MultiEVTAResConv10Test   # hw/ResConv10Test 필요(single set 먼저)
(cd build_multievtax4 && make -j2 multi4resconv10Bundle)
(cd build_multievtax4_prof && make -j2 multi4resconv10Bundle)       # VTA_SYNC 표가 필요할 때
# 실행 전: 비트스트림 재로드 (run_all.sh가 하는 것과 같음), 다른 EVTA 실행 없음 확인
sudo -n /bin/sh -c 'echo 0 > /sys/class/fpga_manager/fpga0/flags; echo vtahw_multi4_y219.bin > /sys/class/fpga_manager/fpga0/firmware; python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()"'
cd hw_multievtax4/MultiEVTAResConv10Test
sudo OMP_NUM_THREADS=1 ./multi4resconv10Bundle 15 400 conv          # 4개 동시; 1 = EVTA0만
# golden 비교 포함 정식 실행: sudo hw_multievtax4/script/run_all.sh MultiEVTAResConv10Test
```

- 실행 하나가 끝날 때마다 비트스트림을 다시 로드한다(멈춘 실행이 EVTA를 deadlock 상태로 남김).
- 한 번에 하나의 EVTA 실행만 한다(`/tmp/evta_run.lock`. 두 실행이 서로 비트스트림을 바꿔 넣으면 SError/bus error).
- `multi4resconv10Bundle <mask> [iters] [conv]`: mask는 EVTA 비트(1, 3, 15). multi2 비트스트림에서는 mask 1/3만 쓴다.

## 8. 관련 파일과 커밋

| 파일 | 내용 |
|---|---|
| `vtabundles_xcmpl/hw_multievtax4/MultiEVTAConv4TestSrc/make_multi4conv4.py` | 4벌 생성, `_convonly`, `multiconv.h` |
| `vtabundles_xcmpl/hw_multievtax4/MultiEVTAConv4TestSrc/multi4conv4Main.cpp` | 스레드, barrier, 시간, golden 비교 |
| `vtabundles_xcmpl/collect_hw_bundles.py` (`EXTRA`), `vtabundles_xcmpl/CMakeLists.txt` | 생성기 실행, 빌드 타깃 `multi4resconv10Bundle` |
| `lib/Backends/VTA/tests/VTASaverTest.cpp` 770행 | ResConv10Test 정의 |
| `vta/vtalib/lib/Bundle/VTABundle.cpp` 342행 `conv_kernel`, 2547행 `convolution_wo_tr` | EVTA 명령 흐름 |
| `log/BOARD_TODO.md` 2026-10-26 | 측정 기록 |

- `bc2abf36` vtabundles_xcmpl: MultiEVTAResConv10Test, the fastest conv shape on 1-4 EVTAs
- `dfec80f7` multi4 333 MHz(EVTA별 리셋, rs_rst), `e119fac9` `NESTC_EVTA_MAX_XFER`, `68a51f05` 실행 lock
