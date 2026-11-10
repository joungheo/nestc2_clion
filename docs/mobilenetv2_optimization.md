# MobileNetV2 실행 시간을 줄여 온 과정 (2026-11-07 ~ 11-10)

ZCU102 위 EVTA와 NEST-C로 MobileNetV2 1장을 줄인 과정이다. 처음에는 1,048 ms에 분류도 틀렸다(293). 지금은 **18.7 ms**(1코어) / **18.3 ms**(4코어)이고 정답(282)이다. 단계마다 왜 바꿨는지, 어떻게 바꿨는지, 얼마나 줄었는지, 어떻게 검증했는지를 적었다.

- **자세한 기록:** `docs/evta_runtime_compare.md` §7.20, §7.21, §7.29
- **Resnet18 과정:** `docs/resnet18_optimization.md`. MobileNetV2는 Resnet18에서 만든 기능에 더해, depthwise conv와 per-channel 양자화를 처음 다뤘다.

## 0. 한눈에

| 날짜 | 단계 | 1장 OMP 1 | `-t 4` | 분류 (확률) | 바꾼 곳 | § |
|---|---|---|---|---|---|---|
| 11-07 | 처음 | 1,048.0 ms | | **293** (0.25) 오답 | – | §7.20 |
| 11-07 | ① depthwise per-channel 양자화 + NEON `cw_convolution` | 488.4 ms | | 282 (0.906) | 양자화 + VTASave + vtalib | §7.20 ① |
| 11-07 | ② 24채널 → 32채널 (`-vta-pad-channels`) | 234.2 ms | | 282 (0.840) | 컴파일러(VTA.cpp) | §7.20 ② |
| 11-07 | ③ EVTA shift 반올림 (`-vta-round-shift`) | 234.0 ms | | 282 (0.939) | 컴파일러 | §7.20 ③ |
| 11-07 | ④ conv0 비대칭 pad를 pack으로 | 84.2 ms | 44.4 ms | 282 (0.944) | 컴파일러 + 두 vtalib | §7.20 ④ |
| 11-08 | ⑤ depthwise가 EVTA layout을 직접 읽고 씀 + kernel 개선 | 55.5 ms | | 282 (0.944) | 컴파일러 + vtalib | §7.20 ⑤ |
| 11-08 | ⑥ logits(1×1 conv)를 EVTA fc로 | 49.8 ms | | 같음 | 컴파일러 | §7.20 ⑥ |
| 11-08 | ⑦ MobileNetV2 모양의 schedule | 43.2 ms | 22.5 ms | 같음 | schedule | §7.20 ⑦ |
| 11-08 | ⑧ depthwise를 EVTA로 (대각 weight GEMM) | 28.6 ms | 24.7 ms | 282 (0.944157) | 컴파일러 + vtalib-systolic | §7.21 |
| 11-09 | (DeiT 작업의 vtalib NEON elemadd 등이 함께 적용) | 약 24.3 ms | | 같음 | vtalib CPU | §7.23 |
| 11-10 | ⑨ conv DMA 묶기 (`EVTA_CONV_BURST`) | 24.30 → **18.74 ms** | **18.27 ms** | 같음 | vtalib | §7.29 |

- 1코어 기준 1,048 → 18.7 ms로 **56배** 빨라졌다.
- 정확도: quantized Interpreter(같은 profile, depthwise per-channel)는 282(0.935)다. 지금 보드는 0.944157이다.
- 11-09 줄의 근거: 11-08(28.6 ms)과 11-10 burst 끔(24.3 ms) 사이에 번들은 같다. 바뀐 것은 vtalib뿐이고, CPU elemadd가 5.3 → 1.0 ms로 준 것(11-10 측정)이 차이의 대부분이다. 11-09 DeiT 작업에서 vtalib elemadd / quantize에 넣은 offset 0 NEON 경로의 효과다. 그 시점에 따로 재지는 않았다.

## 1. 측정 조건

| 항목 | 내용 |
|---|---|
| 모델 | TFLite QAT `mobilenet_v2_1.0_224_quant`를 float ONNX로 바꾼 것(`tfl2onnx.py`, 1001 class, cat_285.png 정답 282) |
| 양자화 | int8 power-of-2, depthwise만 per-channel(`-enable-channelwise-depthwise`) |
| 번들 | `vtabundles_xcmpl/gen_mobilenetv2_bundle.sh` → `vta/bundles/MobileNetV2Test` → `hw/MobileNetV2Test`. 옵션 `-enable-channelwise-depthwise -vta-round-shift -vta-evta-depthwise` |
| 하드웨어 | ZCU102, single EVTA, systolic 400 MHz `..._sa_400mhz_rq.bin` |
| 측정 | `run_prof.sh -s _systimed -r`, OMP 1 또는 `-t 4`, 25장 중앙값 |

ONNX zoo의 MXNet `mobilenetv2-7`도 시험했다. power-of-two per-tensor로는 617이 나왔고, channelwise로 해야 281이 맞았다.

## 2. 출발점 (11-07, 1,048 ms, 분류 293)

| 항목 | 시간 |
|---|---|
| CPU conv 합 | 1,018.9 ms |
| ↳ depthwise 17개 (CPU float / int8) | 603.6 ms |
| ↳ 24채널 1×1 conv 4개 (EVTA 조건 밖) | 260.5 ms |
| ↳ conv0 (TF SAME 비대칭 pad) | 149.2 ms |
| ↳ logits | 5.5 ms |
| EVTA conv 30개 | 13.4 ms |

- **EVTA가 거의 일하지 않았다.** depthwise conv(group = C)와 16의 배수가 아닌 채널(24), 비대칭 pad conv0이 모두 EVTA 조건 밖이었다.
- **분류도 틀렸다.**

## 3. 단계별로 뭘 바꿨나

### 3.1 ① 정확도: depthwise weight의 per-tensor scale (`lib/Quantization/Quantization.cpp` `-enable-channelwise-depthwise`)

**원인을 가른 방법:**
- 보드와 quantized Interpreter가 똑같이 293이었다. 번들은 quantizer가 만든 그대로 돌고 있었다는 뜻이다.
- onnxruntime에서 Glow 규칙(symmetric, scale은 2의 거듭제곱으로 올림)으로 fake-quantize해서 원인을 갈랐다.
  - depthwise weight만 양자화해도 293이 나왔다. pointwise만, 또는 activation만 양자화하면 282였다.
  - depthwise weight는 BN을 접은 뒤 한 filter 안에서 채널 최대값이 0.3–56으로 퍼진다. scale이 0.5 하나이면 작은 채널은 0–2단계밖에 남지 않는다.
  - depthwise만 채널별 scale(각 채널 scale도 2의 거듭제곱)로 두면 282(0.925)가 된다.

**바꾼 것:**
- 이 옵션은 group > 1 conv만 `ChannelwiseQuantizedConvolution`으로 바꾼다. EVTA로 가는 1×1 conv는 per-tensor 그대로다.
- VTASave `saveChannelwiseConvolutionInst`와 vtalib `cw_convolution`(CPU)이 이것을 처리한다.
  - VTASave가 compile time에 채널별 bias(acc scale)와 출력 배율 `matMulScale / outScale`을 계산한다.
  - 반올림은 Interpreter와 같이 `std::round(float(sum) * m)`이다.
  - depthwise 경로는 NEON으로 한 번에 16채널씩 처리한다.

**A53의 한계:**
- `cw_convolution`은 C와 상관없이 16채널당 약 290 ns(약 350 cycle)다. benchmark로 재현했다(g++로는 237 ns).
- A53 NEON은 128-bit 연산을 반 속도로 하고, int8 dot product가 없다(ARMv8.0). 탭 9개 × 8개에 마무리까지 연산이 약 100개다.

**한때 의심한 것:** "int8 지수 하한 −7"(`lib/Quantization/Base/Base.cpp`)은 원인이 아니었다. 그 파일은 `NESTC_WITH_NMP`일 때만 빌드되고, 이 보드는 Glow의 `Base.cpp`(하한 없음)를 쓴다.

**결과:** depthwise 603.6 → 43.2 ms, 1장 1,048 → 488.4 ms, 분류 293 → **282**(0.906)

### 3.2 ② 24채널을 32채널로 (`VTA.cpp` `padChannelsTo16`, `-vta-pad-channels`, 기본 켜짐)

- **왜:** 24채널 bottleneck의 1×1 conv 4개가 EVTA 조건(채널 16의 배수) 밖이라 CPU에서 260.5 ms였다.
- **어떻게:**
  - 채널 수가 16의 배수가 아닌 int8 activation을 16의 배수로 넓힌다. 조건은 만드는 쪽과 쓰는 쪽이 모두 group 1 conv(상수 filter, bias)이거나, 그런 activation끼리의 Add인 것이다.
  - 만드는 conv의 filter 행과 bias는 0으로, 쓰는 conv의 filter 열도 0으로 채운다. 그래서 늘어난 채널은 항상 0이고 Pad / Slice 노드가 없다.
- **검증:** 옮긴 conv(expanded_conv_2 expand, 32 → 144)의 출력을 numpy로 다시 계산했다. 잘라내는 shift로 계산하면 451,584개 중 0개가 다르고, 늘어난 lane은 모두 0이다.
- **확률이 0.906 → 0.840으로 내려간 이유:** 반올림이다. EVTA conv는 `acc >> shift`로 잘라내고, CPU conv와 Interpreter는 `std::round`로 반올림한다. project conv 출력은 scale이 1/4, 1/2로 거칠어서 −0.5 LSB 편향이 뒤 층으로 퍼진다(→ ③).
- **결과:** 260.5 ms가 EVTA +3.8 ms가 됐고, 1장 488.4 → 234.2 ms

### 3.3 ③ EVTA shift 반올림 (`VTASave.cpp` `convRoundHalf`, `-vta-round-shift`, 기본 꺼짐)

- **어떻게:** compile time에 bias에 2^(shift−1)을 더해 shift가 반올림하게 한다. 실행 비용은 없다.
- fuse된 conv(residual, pool)는 shift가 add나 pool 뒤에 있어서 제외한다.
- 켜면 다른 모델의 EVTA conv 출력도 모두 바뀐다. 그래서 기본은 꺼 두고 MobileNetV2Test(와 DeiTTest)에서만 켠다.
- **결과:** 시간은 같고(234.0 ms), 확률이 0.840 → **0.939**가 됐다.

### 3.4 ④ conv0 비대칭 pad를 pack으로 (`getConv0Pack`, 두 vtalib `padEnd`)

- **왜:** TF SAME stride 2의 pad는 위·왼쪽 0, 아래·오른쪽 1이다. conv0 pack(Resnet18 §3.12)은 대칭 pad만 받아서 conv0이 CPU 149.9 ms였다.
- **어떻게:** conv0 pack은 pad된 행을 직접 만든다. 그래서 위·왼쪽 pad만큼 offset을 두고, 아래·오른쪽 pad를 `padEnd`로 따로 받게 했다. 그 결과 113×112×32 → 32의 2×1 packed conv로 EVTA에서 돈다.
- **결과:** conv0 149.9 ms → EVTA 약 2.8 ms, 1장 234.0 → **84.2 ms**(`-t 4`는 44.4 ms, depthwise 43.0 → 11.7 ms)

### 3.5 ⑤ depthwise 앞뒤 transpose 없애기 + kernel 개선 (11-08)

- **왜:** depthwise는 CPU에서 NHWC로 돌았다. 그래서 EVTA conv 사이에 transpose 34개(8.6 ms)가 생겼다.
- **어떻게:**
  - `findPackedActivations`가 depthwise conv(C % 16 = 0)의 입력과 출력을 EVTA layout N{C/16}HW16으로 둘 수 있게 했다.
  - 입력과 출력의 layout은 따로 정해지고, `cw_convolution`이 두 layout을 모두 받는다(`inPacked`, `outPacked`). CPU가 쓴 packed 출력은 EVTA용으로 flush한다.
- **처음에는 오히려 느려졌다(43.0 → 50.3 ms).** pixel마다 16채널 block의 평면을 건너다녔기 때문이다. 이렇게 고쳤다.
  - 16채널 block을 바깥 루프로 옮겨 한 평면을 순서대로 읽게 했다.
  - 3×3이고 행이 모두 안쪽일 때는 weight 9개를 register에 두고, 안쪽 열을 두 pixel씩 처리한다(누산기 8개가 독립).
  - 보드 benchmark에서 스칼라 식과 bit-exact였다(NHWC와 packed, stride 1 / 2, pad 0 / 1, 7×7 – 112×112). packed 56×56×144가 280 → 136 ns / 16채널이 됐다.
- **검증:** EVTA conv dump 35개가 바꾸기 전과 같다.
- **결과:** transpose 34개 없어짐, depthwise 43.0 → 22.8 ms, 1장 84.2 → **55.5 ms**

### 3.6 ⑥ logits를 EVTA fc로 (`VTASave.cpp` `saveEvtaConvAsFc`)

- **왜:** 마지막 logits는 1×1 입력 위의 1×1 conv(1280 → 1001)인데, conv 경로로 가면 최소 tile 때문에 비효율이라 CPU 5.9 ms였다.
- **어떻게:** 1×1 입력 위의 1×1 conv(활성 없음, pad 0)를 EVTA fc(`fullyconnected_evta_packed`)로 보낸다. fc 저장 함수 본체를 `saveEvtaFcPacked`로 나눠, conv filter 순서 `[OUT][1][1][IN]`도 받게 했다.
- **검증:** `EVTA_RES_CHECK=1`로 1001개 중 0개가 다르다.
- **결과:** 5.9 → 0.29 ms, 1장 55.5 → **49.8 ms**

### 3.7 ⑦ MobileNetV2 모양의 schedule (`VTASchedules.h`)

- **어떻게:** 모양 20개(packed conv0 포함)의 schedule을 Resnet50과 같은 방법으로 찾았다(`EVTA_SCHED` 후보, conv별 `_systimed`).
- **발견:** 출력 채널 block 수가 홀수인 모양(16, 144채널)은 virtual thread 2가 **실행되지 않았다**(시간 0.01 ms). 그 둘은 vt 1의 큰 tile을 쓴다.
  - 이것은 11-10 DeiT 작업에서 원인을 찾았다. 블록 묶음 수가 0이 되어 계산 없이 끝나던 결함이다. 이제 두 vtalib이 vt 1로 낮춘다(`deit_optimization.md` §3.7).
- **검증:** 기본 schedule과 비교한 dump 35개가 같다.
- **가장 크게 준 것:** conv0 2.76 → 1.55 ms, 56×56 1×1 project 0.75–1.06 → 0.29–0.42 ms
- **결과:** EVTA conv 20.0 → 13.4 ms(45.8 GOPS), 1장 49.8 → **43.2 ms**, `-t 4` **22.5 ms**

### 3.8 ⑧ depthwise를 EVTA로 (§7.21, `-vta-evta-depthwise`, 기본 켜짐; vtalib-systolic `dw_convolution_evta`)

**방법 (ISA·비트스트림 변경 없음):**
- EVTA layout N{C/16}HW16에서 16채널 block 하나는 H×W×16 평면이다. 그래서 depthwise는 block마다 16 → 16 conv다.
- weight는 tap마다 대각 행렬 diag(w[c][ky][kx])다. wgt entry 1개, block당 KH×KW개이고, VTASave가 컴파일 때 쓴다.
- vtalib-systolic `conv_kernel`에 depthwise 모드(`ConvDw`)를 넣었다.
  - 입력 block 루프 대신, 출력 block마다 자기 입력 block의 tile을 thread의 inp 절반에 LOAD한다(`gemmop_mxn` `in_kernel_factor_`).
  - tap 9개를 GEMM이 acc에 누적한다.
  - tiling, replay, dump는 일반 conv와 같다.
- **채널별 scale(모두 2의 거듭제곱, shift 1–10)은 채널별 shift로 한다.** shift(int32, block당 acc entry 1개)를 bias entry 뒤에 LOAD하고, ALU SHR을 vector operand로 한다(dst = tile, src = block의 shift entry, II = 2). relu6 clip은 그 SHR의 requantize(rq shift 0)나 ALU MAX / MIN으로 한다.
- **반올림:** round half up(bias에 2^(shift−1))이다. `cw_convolution`(std::round)과는 음수 .5에서만 다르다.
- **조건:** 입력·출력이 EVTA layout, N = 1, pad 위 = 왼쪽(TF SAME stride 2 됨), dilation 1, 모든 배율이 2^−k. 아니면 `cw_convolution`으로 간다.
- **HLS vtalib:** `vta/vtalib`(HLS GEMM)에는 depthwise 모드가 없다. 그래서 같은 이름의 함수가 같은 산술을 CPU로 한다(link 호환).
- **schedule:** `VTASchedules.h`에 filterN 0으로 넣고, `EVTA_SCHED`도 KN 0으로 받는다. 블록 수가 홀수면 vt 1이다.

**확인:** `EVTA_RES_CHECK=1`이 같은 산술의 CPU 루프와 비교한다. depthwise 17개 모두 0개 다름이고, 분류는 282(0.944157)로 그대로다. schedule을 바꾼 뒤에도 dump가 기본 schedule과 같다.

| | OMP 1 | OMP 4 |
|---|---|---|
| CPU depthwise (⑦) | 43.2 ms (depthwise 22.8) | 22.5 ms |
| EVTA depthwise, 기본 schedule 1 / 8×8 | 39.8 ms (depthwise 19.6) | |
| EVTA depthwise, schedule 탐색 후 | **28.6 ms** (depthwise 8.4) | 24.7 ms |

- OMP 1에서 34 % 줄었다.
- **OMP 4에서는 손해였다.** 4코어 NEON depthwise(약 6 ms)보다 느렸다.
- **기본값을 켬으로 둔 이유(11-08 결정):** 채널별 scale depthwise에만 걸리고(Resnet18 / 50, VGG16 번들은 byte 단위로 같음), OMP 4의 손해(22.5 → 24.7 ms)가 OMP 1의 이득(43.2 → 28.6 ms)보다 작다.
- **EVTA 효율은 약 2.4 GOPS였다.** GEMM만 보면(1/16 활용) 14.4만 pixel-block × 9 cycle ≈ 3.2 ms인데, block마다 행 하나가 112–896 B인 작은 DMA(입력 행, 출력 행, bias, shift)가 많았다. → ⑨

### 3.9 ⑨ conv DMA 묶기 (§7.29, vtalib-systolic `EVTA_CONV_BURST`)

- **왜:** ⑧의 결론대로 작은 DMA가 많았다. 일반 1×1 conv에서도 weight가 256 byte 행으로 나뉘어 있었다.
- **어떻게:**
  - 연속인 LOAD / STORE 행을 한 행으로 합친다. depthwise의 평면 tile도 해당된다.
  - 1×1 conv는 스레드의 weight를 한 번에 LOAD한다.
  - 폭 전체 tile은 출력 블록 전부를 STORE 하나로 보낸다.
  - 자세한 내용은 `resnet50_optimization.md` §3.6에 있다.
- **검증:** `EVTA_DUMP_CONV` 5,200개(depthwise 포함, 여러 run)가 burst 0 / 1에서 byte 단위로 같다. 분류는 282(0.944157)다.

| (11-10, OMP 1) | burst 0 | burst 1 |
|---|---|---|
| `convolution_wo_tr` (EVTA conv) | 13.29 ms | **8.83 ms** |
| `dw_convolution_evta` + 기타 | 8.71 ms | 7.62 ms |
| 추론 | 24.30 ms | **18.74 ms** |
| 추론 `-t 4` | | **18.27 ms** |

**4코어의 손해가 사라졌다.** depthwise가 EVTA에서 7.3 ms라, 4코어 NEON(약 6 ms)과 거의 같아졌다.

## 4. 지금 시간 분해 (11-10, single)

| 항목 | OMP 1 (18.74 ms) | `-t 4` (18.27 ms) | 어디서 |
|---|---|---|---|
| `convolution_wo_tr` (1×1 / 3×3 / conv0) | 8.82 ms | 8.37 ms | EVTA |
| `dw_convolution_evta` (depthwise 17개) | 7.34 ms | 7.33 ms | EVTA |
| elemadd (residual 10개) | 1.03 ms | 1.04 ms | CPU |
| avgpool | 0.59 ms | 0.58 ms | CPU |
| VTABufferInvalidate | 0.48 ms | 0.48 ms | CPU |
| logits fc | 0.29 ms | 0.29 ms | EVTA |

연산량은 약 0.6 GOP이고, 1장 18.7 ms는 약 32 GOPS(피크의 16 %)다. 같은 ZCU102의 DPU는 스레드 1개에 약 3.6 ms다(피크 대비 약 14 %, `kv260_product_compare.md` §3.2).

## 5. 다음 후보

| 후보 | 내용 | 예상 | 비고 |
|---|---|---|---|
| depthwise 여러 block을 한 LOAD로 | 평면이 연속이라 pad만 해결하면 됨. 출력 STORE도 같이. 7×7 층은 tile = 이미지 | depthwise 7.3 ms 중 일부 | vtalib |
| depthwise 하드웨어 | 대각 GEMM은 GEMM 효율 1/16. 채널별 MAC(depthwise 모드)이나 ALU MAC | depthwise 수 ms → 1 ms 안팎 | ISA 개정(`deit_optimization.md` §6.2 H1, `unified_runtime_plan.md` §4.3 2단계) |
| residual add를 EVTA로 | MobileNetV2의 residual은 project conv(relu 없음) + add라 지금 fusion 조건(relu 붙은 add) 밖. 반올림 차이를 확인하고 relu 없는 경우도 합치기 | elemadd −1 ms | 컴파일러 + vtalib |
| avgpool을 EVTA로 | 7×7 평균은 ÷49라 MUL이나 표 필요 | −0.6 ms | ISA 개정 |
| burst 뒤 schedule 다시 찾기 | DMA 모양이 바뀜 | 미측정 | `EVTA_SCHED` |
| `-vta-round-shift` 기본값 | 다른 모델의 정확도와 시간을 재고 정하기로 함(11-07) | – | 결정 필요 |

## 6. 재현

끄는 방법은 세 종류다. `-vta-*`는 번들 생성(`model-compiler`) 옵션이라 번들을 다시 만들어야 하고, `EVTA_*`는 실행할 때 주는 환경변수(`sudo /bin/sh -c "EVTA_X=0 hw/script/run_prof.sh ..."`), 하드웨어 단계는 `BIN=<비트스트림>`이다. 자세한 절차와 vtalib마다 읽는 변수의 차이는 `resnet18_optimization.md` §9.2–§9.5에 있다.

```bash
vtabundles_xcmpl/gen_mobilenetv2_bundle.sh          # 번들 생성 (보드, Debug2_remote model-compiler)
python3 vtabundles_xcmpl/collect_hw_bundles.py MobileNetV2Test
cd vtabundles_xcmpl
sudo hw/script/run_prof.sh -s _systimed -r MobileNetV2Test
sudo hw/script/run_prof.sh -s _systimed -r -t 4 MobileNetV2Test
```

| 단계 | 끄는 방법 |
|---|---|
| depthwise per-channel | `-enable-channelwise-depthwise` 빼기 (분류가 293으로 틀림) |
| 채널 넓히기 | `-vta-pad-channels=false` |
| 반올림 | `-vta-round-shift` 빼기 |
| depthwise EVTA | `-vta-evta-depthwise=0` |
| logits EVTA fc | `-vta-evta-fc=false` |
| DMA 묶기 | `EVTA_CONV_BURST=0` (실행 시) |
