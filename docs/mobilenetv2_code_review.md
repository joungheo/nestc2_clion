# MobileNetV2 시간 줄이기 코드 리뷰: 컴파일러, vtalib, 하드웨어에 넣은 코드

2026-11-27. [mobilenetv2_evta_paper.md](mobilenetv2_evta_paper.md)의 487 → 11.6 ms(EVTA 1개), 291 fps(EVTA 4개)를 만든 코드를 기능마다 설명한다.
각 기능에 대해 다음을 적는다.
- 무엇을 왜 넣었나
- 어느 파일, 어느 함수인가
- 핵심 코드
- 어떻게 맞는지 확인했나
- 리뷰에서 본 위험과 한계

**범위.** MobileNetV2 일(2026-11-06 ~ 27)에서 새로 넣은 코드다.
- Resnet18 / DeiT 일에서 이미 있던 기능도 MobileNetV2 시간에 크게 기여했다. buffer pool, record / replay, DMA 묶기, LOAD 앞당기기, bias reset, requantize, ALU 확장이다.
- 그런 기능은 §5의 표에 문서 위치만 둔다.

**읽는 법.**
- 코드 위치는 `파일:줄`이다(2026-11-27 `main`).
- 우리가 고친 곳에는 `//#### JO added (<날짜>)` 표식이 있다. 그 주석이 설계 설명이므로 함께 읽는다.
- 효과 숫자는 논문 표 4다. 다 켠 상태(`_alux3_dw`, 11.70 ms)에서 그 기능만 끄면 늘어나는 시간이다.

## 0. 한눈에

| § | 기능 | 층 | 주 파일 | 스위치 | 끄면 | 첫 commit |
|---|---|---|---|---|---|---|
| 1.1 | depthwise만 채널별 양자화 | 컴파일러 | `lib/Quantization/Quantization.cpp` | `-enable-channelwise-depthwise` | 분류 틀림(293) | `69183921` 11-07 |
| 1.2 | ReLU6(Clip)를 conv에 합침 | 컴파일러 | `lib/Backends/VTA/VTA.cpp`, vtalib | (항상) | | `2944ce3b` 11-06 |
| 1.3 | 24채널을 32로 넓힘 | 컴파일러 | `VTA.cpp` `padChannelsTo16` | `-vta-pad-channels` | +267.8 ms | `14c77182` 11-07 |
| 1.4 | 비대칭 pad의 첫 conv 묶기 | 컴파일러 + vtalib | `VTASave.cpp` `getConv0Pack` | `-vta-conv0-pack` | +153.3 ms | `8bde38b4` 11-07 |
| 1.5 | depthwise가 EVTA layout을 읽고 씀 | 컴파일러 + vtalib | `VTASave.cpp`, `cw_convolution` | `-vta-keep-layout` 안 | +35.4 ms(layout 전체) | `59d5a908` 11-07 |
| 1.6 | EVTA conv shift 반올림 | 컴파일러 | `VTASave.cpp` `convRoundHalf` | `-vta-round-shift` | 정확도 | `32c11097` 11-07 |
| 1.7 | logits(1 × 1 over 1 × 1)를 EVTA fc로 | 컴파일러 | `VTASave.cpp` `saveEvtaConvAsFc` | `-vta-evta-fc` | +5.3 ms | `b66c755a` 11-07 |
| 1.8 / 2.1 | depthwise를 EVTA로(대각 GEMM) | 컴파일러 + vtalib | `VTASave.cpp`, `dw_convolution_evta` | `-vta-evta-depthwise` | +19.5 ms | `5e0685ea` 11-08 |
| 1.9 | relu 없는 residual add 합치기 | 컴파일러 + vtalib | `VTASave.cpp`, epilogue | `-vta-fuse-residual-norelu` | +0.7 ms | `bf20900a` 11-14 |
| 1.10 / 2.3 | global avgpool을 EVTA ALU로 | 컴파일러 + vtalib + HW | `VTASave.cpp`, `avgpool_evta` | `-vta-evta-avgpool`, `EVTA_AVGPOOL` | +0.5 ms | `bf20900a` 11-14 |
| 1.11 | EVTA만 읽는 출력의 invalidate 빼기 | 컴파일러 | `VTASave.cpp` `dropEvtaOnlyInvalidates` | `-vta-skip-evta-invalidate` | +0.5 ms | `0b1439b7` 11-25 |
| 2.2 | depthwise 창 모드 명령 | vtalib | `VTABundle.cpp` `gemmop_dw`, `dwWindowWeights` | `EVTA_DWU` | +1.7 ms | `818a73c3` 11-25 |
| 2.4 | 첫 conv 입력 묶기 NEON | vtalib | `CPUBundle_generic.cpp` | (항상) | | `b5c4e7db` 11-25 |
| 2.5 | replay 명령 사본 | vtalib | `cmd_queue.h` | `EVTA_REPLAY_VARIANTS` | +0.2 ms(4개 동시에 더 큼) | `1cdf422d` 11-25 |
| 2.6 | thread별 buffer pool | vtalib | `api.cc` | `EVTA_BUF_POOL_SHARED` | (4개 동시) | `1cdf422d` 11-25 |
| 2.7 | 1 × 1 weight 한 LOAD | vtalib | `VTABundle.cpp` `conv_kernel` | `EVTA_WGT_FIT` | +0.2 ms | `38e3da66` 11-26 |
| 2.8 | int8 입력(`.i8`) | vtalib + main | `VTABundle.cpp`, `multi_threads_main.cpp` | `INPUT_INT8` | | `d4d8ddf2` 11-25 |
| 2.9 | 다음 그림 quantize 겹치기 | vtalib + main + 드라이버 | `pynq_driver.cc`, main | `INPUT_PIPELINE` | | `24ac3ef1` 11-27 |
| 3.1–3.5 | depthwise 창 장치 | HW | `sa_dw.v`, `sa_ctrl.v`, `gemm_sa.v`, `vta.cc`, `hw_spec.h` | Makefile `DWU=1` | (위 2.2) | `818a73c3` 11-25 |
| 3.6 | ALU MUL immediate만(multi4) | HW | `vta.cc` | Makefile `ALUMUL=1` | avgpool CPU | `398f358c` 11-23 |

## 1. 컴파일러 (NEST-C VTA backend)

### 1.1 depthwise만 채널별 양자화 (`Quantization.cpp:30`, `enableChannelwise(groupedOnly)` `:834`)

**왜.** MobileNetV2의 depthwise filter는 BN을 접어 넣어서, 한 filter 안의 채널 최댓값이 0.3 ~ 56으로 벌어진다.
scale을 하나만 쓰면 작은 채널에 0 ~ 2단계만 남는다. 그래서 cat_285가 293으로 틀렸다.

**어떻게.** Glow에는 `-enable-channelwise`가 있었지만 모든 conv를 채널별로 만든다. EVTA conv는 scale 하나를 shift로 쓰므로 그러면 안 된다.
그래서 `groupedOnly`로 group > 1인 conv만 `ChannelwiseQuantizedConvolution`으로 바꾼다.

```cpp
//#### JO changed (2026-11-07): \p groupedOnly: only the convs with group > 1 (-enable-channelwise-depthwise)
void enableChannelwise(bool groupedOnly = false) {
```

**리뷰.**
- NEST-C의 `lib/Quantization/Base/Base.cpp`(int8 지수 하한 −7)는 `NESTC_WITH_NMP`일 때만 빌드된다. 이 보드는 Glow 것을 쓴다(CLAUDE.md).
  채널별 scale도 power-of-two여야 1.8의 EVTA depthwise가 shift로 받을 수 있다. `VTASave`가 `m[d] == 2^-k`가 아닌 채널을 만나면 CPU로 둔다.

### 1.2 ReLU6를 conv의 relu + 위 clip으로 (`2944ce3b`)

- TF 모델의 `Clip(0, 6)`을 int8로 남기고, 앞 conv의 fused relu와 위 clip으로 합친다.
- EVTA conv는 이미 clip(MIN / MAX 또는 requantize의 `rq_lo` / `rq_hi`)을 하므로 비용이 없다.
- 1.8의 depthwise도 `lo`, `hi`를 받는다(`VTASave.cpp:990` 근처, `lround(args / outScale)`).

### 1.3 24채널을 32로 넓히기 (`VTA.cpp:40` 옵션, `:685` `isWidenableConv`, `padChannelsTo16`)

**왜.** EVTA conv는 입력과 출력 채널이 16의 배수여야 한다.
MobileNetV2의 24채널 bottleneck(1 × 1 conv 4개와 그 add)이 CPU로 갔다. 그 이웃 depthwise도 EVTA layout을 쓰지 못했다.

**어떻게.** 그래프 단계(lowering 뒤, 양자화된 그래프)에서 한다.
1. C % 16 ≠ 0인 int8 4차원 결과를 내는 노드 가운데 group 1 conv(상수 filter / bias, offset 0)와 Add를 후보 S에 넣는다.
2. 고정점 반복으로 S를 줄인다. 모든 사용자가 같은 조건의 conv 입력이거나 S 안의 Add여야 남는다. Add는 두 입력이 모두 S여야 한다.
3. 남은 활성값을 만드는 conv는 filter 행과 bias를 0으로 늘린다(KN → pad16). 그것을 쓰는 conv는 filter 열을 0으로 늘린다(C → pad16).
4. offset이 0이므로 늘어난 채널은 끝까지 0이다.

```cpp
const dim_t KN = out ? pad16(fd[0]) : fd[0], C = in ? pad16(fd[3]) : fd[3];
auto *nf = M->createConstant(M->uniqueTypeWithNewShape(filter->getType(), {KN, fd[1], fd[2], C}), ...);
nf->getPayloadMutable().zero();   // then copy the original KN x KH x KW x C block
```

**확인.** 분류 282, EVTA conv dump.
- 이 옵션을 끈 bundle만 confidence가 0.3017이다(나머지 0.3221).
- CPU int8 conv와 EVTA conv의 반올림이 다르기 때문이다. 분류는 같다.

**리뷰.**
- 그 conv들의 MAC이 1/3 늘어난다(24 → 32). CPU로 가는 것보다 비교할 수 없이 빠르지만 공짜는 아니다.
- 조건이 보수적이다(offset 0, 상수 weight, 사용자 전부 conv / Add). 다른 모양(concat, pool이 끼는 경우)은 넓히지 않는다. 그런 모델이 오면 조건을 늘려야 한다.

### 1.4 비대칭 pad의 첫 conv 묶기 (`VTASave.cpp:1675` `getConv0Pack`, vtalib `preprocess_conv0pack*`)

**왜.** TF SAME pad의 3 × 3 / 2 conv는 pad가 위 / 왼쪽 0, 아래 / 오른쪽 1이다. 기존 conv0 묶기와 zero-fill 경로는 네 pad가 같아야 했다. 그래서 첫 conv(149 ms)가 CPU였다.

**어떻게.** `pads[0] == pads[1]`(위 = 왼쪽)과 `pads[2] == pads[3]`(아래 = 오른쪽)만 요구한다. 아래 / 오른쪽 pad는 vtalib에 `padEnd`로 넘긴다.
- 출력 크기를 다시 계산해 맞는지 본다.
- 묶은 모양은 `KH2 = ceil(KH / s)`, `CO = pad16(s × KW × C)`다. MobileNetV2는 2 × 1 / 1 over 18 → 32채널이다.

```cpp
int s = strides[0], pad = pads[0], padEnd = pads[2];
if (strides[1] != s || pads[1] != pad || pads[3] != padEnd || s < 1 || KN % 16) return false;
if ((H + pad + padEnd - KH) / s + 1 != outH || (W + pad + padEnd - KW) / s + 1 != outW) return false;
```

**리뷰.**
- 위 = 왼쪽, 아래 = 오른쪽이 아닌 pad(예: 위 1, 왼쪽 0)는 받지 않는다. TF / PyTorch의 보통 모델은 모두 이 형태다.
- 18채널을 32로 채우므로 GEMM의 14/32가 0이다. 3채널 zero-fill(3/16)보다는 훨씬 낫다.

### 1.5 depthwise가 EVTA layout을 읽고 씀 (`59d5a908`, vtalib `cw_convolution` inPacked / outPacked)

- 처음 depthwise(CPU, NEON `cw_convolution`)는 NHWC를 읽고 썼다. 앞뒤 EVTA conv와의 사이에 transpose가 두 번씩 들어갔다.
- `findPackedActivations`가 depthwise의 입력과 출력도 EVTA layout(N{C/16}HW16)으로 두게 했다. `cw_convolution`은 두 layout을 모두 읽고 쓴다.
- 1.8의 EVTA depthwise는 입력과 출력이 모두 EVTA layout일 때만 탄다. 그래서 `-vta-keep-layout`을 끄면 depthwise 17개가 모두 CPU로 간다. 논문 표 4의 +35.4 ms에는 이것이 들어 있다.

### 1.6 EVTA conv shift 반올림 (`VTASave.cpp:1319` 옵션, `:1327` `convRoundHalf`)

- EVTA conv의 출력 shift는 원래 버림이었다. CPU conv와 Glow Interpreter는 `std::round`다.
- 그래서 컴파일 때 bias에 2^(shift−1)을 더한다. 첫 conv의 bias에는 11-22에야 넣었다(`:1868`, Resnet50 44.66 → 52.92 %).
- MobileNetV2 bundle은 늘 켠다(`gen_calib_bundle.sh`).
- **리뷰:** bias에 2^(shift−1)을 더하고 산술 shift하므로 음수의 .5는 위로 간다(half up). `std::round`(half away from zero)와 음수 .5에서만 다르다. 정확도 기록은 [evta_accuracy.md](evta_accuracy.md) §6에 있다.

### 1.7 logits를 EVTA fc로 (`VTASave.cpp:4975` `isConvAsFc`, `:4983` `saveEvtaConvAsFc`)

- MobileNetV2의 마지막 층은 1 × 1 입력 위 1 × 1 conv(1280 → 1001)다.
- 출력 1001이 16의 배수가 아니라 conv 경로가 거절했다. 그래서 같은 계산인 EVTA fc(`saveEvtaFcPacked`)로 보낸다.
- EVTA fc는 출력 묶음 단위로 chunk를 돌고, 남는 묶음은 packed weight로 CPU에서 한다(`fc_packed_cpu`).
- 조건: batch 1, 1 × 1 입력 / 출력 / 커널, group 1, 활성 함수 없음, pad 0.

### 1.8 depthwise를 EVTA로: bundle 쪽 (`VTASave.cpp:997`)

**조건.**
- `-vta-evta-depthwise`, C % 16 = 0, N = 1, 입력과 출력이 EVTA layout
- pad 위 = 왼쪽, dilation 1, stride 가로 = 세로
- 모든 채널 scale이 2^-k

조건 밖이면 `cw_convolution`(CPU)이다. 그 이유는 `VTA_FUSION_LOG`가 찍는다(`:1063`).

**weight 파일에 쓰는 것.**
1. 16채널 묶음마다 탭 수(KH × KW)만큼 16 × 16 entry를 쓴다. 대각선 `(d % 16) * 17`에 `w[d][tap]`이 들어간다.
2. bias `B[d] + 2^(sh[d] − 1)`: 반올림 half up이다.
3. 채널별 shift `sh[d]`: int32다.

```cpp
for (int d = 0; d < C; d++)
  for (int t = 0; t < KK; t++)
    dwW[((size_t)(d / 16) * KK + t) * 256 + (d % 16) * 17] = hF.raw((size_t)d * KK + t);
...
bundle->append("  dw_convolution_evta(");   // src_vbuf, filter, bias, shift, dest_vbuf, N H W C KH KW pad stride OH OW lo hi, cmdH, vt th tw idx
```

schedule은 `VTASchedules.h`의 KN 0 칸이다(`getConvSchedule(..., 0, ...)`).

**리뷰.**
- 대각 weight는 GEMM MAC의 1/16만 쓴다. 그래도 A53 NEON(int8 dot product 없음)보다 빠르다. 하드웨어 창 장치(§3)가 이 낭비를 줄인다.
- 코드 주석(`:139`)대로 EVTA depthwise는 half up, `cw_convolution`은 `std::round`라 음수 .5가 다르다.
  MobileNetV2Tf에서는 켜고 끈 두 bundle의 분류와 confidence가 같았다(논문 표 4). 다른 모델에서는 바이트가 다를 수 있다.

### 1.9 relu 없는 residual add 합치기 (`VTASave.cpp:65` 옵션, `:4424`, `:4503`)

**왜.** inverted residual의 add 10개에는 relu가 없다. 기존 `findResidualFusions`는 add + relu만 conv에 합쳤다.

**어떻게.** `vtaFuseResidualNoRelu`면 relu가 없는 체인(conv → add)도 epilogue로 만든다.
- vtalib는 이것을 ADD → RQ(round 2) 프로그램으로 내린다.
- round 2(half away from zero)가 CPU add의 반올림과 같아서 바이트가 같다.
- `_rq`가 없는 bitstream에서는 conv + CPU add로 대신 돈다.

**리뷰.**
- 하드웨어(`_rq`)를 전제로 한 컴파일러 기능이다. 기준 bitstream에서는 오히려 4.3 ms 느렸다(논문 표 3 단계 7).
  옵션 기본값이 bitstream과 무관하게 켜져 있는 것은, 지금 기본 bitstream이 모두 `_rq`라서다.
- `EVTA_EPI_CHECK=1`로 매 호출을 CPU와 비교할 수 있다.

### 1.10 global avgpool을 EVTA ALU로 (`VTASave.cpp:5162` 옵션, `:5272` emit)

- bundle은 `avgpool_evta(...)`를 부르고, 0을 돌려받으면 CPU avgpool로 간다.
- 그래서 같은 bundle이 ALU MUL이 없는 bitstream에서도 돈다. vtalib 쪽은 §2.3에서 다룬다.

### 1.11 EVTA만 읽는 출력의 invalidate 빼기 (`VTASave.cpp:9869` `dropEvtaOnlyInvalidates`)

**왜.** bundle은 EVTA 호출 뒤마다 출력에 `VTABufferInvalidate`(`dc civac` 루프)를 불렀다. 다음 EVTA 호출만 읽는 출력에는 필요 없다. EVTA는 DRAM을 읽기 때문이다. 1장에 0.5 ms였다.

**어떻게.** 만들어진 bundle 본문 **텍스트**에서 한다. 한 줄에 문장 하나라는 규칙에 기댄다.
1. invalidate 뒤로, 그 buffer의 이름과 그 이름에서 선언한 포인터(별칭, 가상 주소)를 따라간다.
2. buffer가 해제되거나 다음 invalidate가 나올 때까지, 이름이 나오는 줄을 하나씩 본다. 허용되는 줄은 다음 셋뿐이다.
   - `kEvtaReaders`(`convolution_wo_tr*`, `dw_convolution_evta`, `maxpool_evta`, `VTASetConvEpilogue`)의 호출
   - epilogue residual 저장(`<x>_epi.t[k] = <name>;`)
   - 그 이름에서 포인터를 선언하는 줄
3. 모든 줄이 이 셋이면 invalidate를 지운다. 다른 줄(CPU 함수, `if`, 함수 끝)이 하나라도 있으면 남긴다.

**리뷰.**
- **텍스트 분석이라 깨지기 쉽다.** 새 EVTA 함수를 bundle에 쓰게 되면 `kEvtaReaders`에 넣어야 효과가 난다. 넣지 않으면 invalidate가 남을 뿐이라 안전 쪽이다.
  반대로 `kEvtaReaders`의 함수가 CPU 대체 경로에서 입력을 invalidate하지 않게 바뀌면 틀린 결과가 난다. vtalib 쪽 대체 경로(`EVTA_*_CHECK`, `EVTA_DUMP_CONV` 포함)가 스스로 invalidate한다는 약속이 이 기능의 전제다.
- `timed_bundle.py`, `VTA_FUSION_LOG`와 함께 "한 줄에 문장 하나" 규칙을 지키는 또 하나의 이유다.

## 2. vtalib (`vtalib-zcu102-systolic`)

### 2.1 `dw_convolution_evta`: 대각 GEMM (`VTABundle.cpp:956` `ConvDw`, `:5354`)

**구조.** 기존 `conv_kernel`(`convolution_wo_tr_impl`)에 `ConvDw *dw`를 넘겨 depthwise로 쓴다.
- 16채널 묶음 하나가 H × W × 16 평면이다. 그래서 depthwise는 묶음마다 16 → 16 conv다.
- 출력 묶음마다 입력 tile 하나를 LOAD한다(`in_kernel_factor_`). 입력 묶음 loop가 없다.
- 채널별 shift는 int32 벡터로 acc의 bias entry 뒤에 LOAD한다. `alu_dw_shr`가 ALU SHR을 벡터 operand로 한다(`dst = tile`, `src = 묶음의 shift entry`).
- relu6 clip은 그 SHR의 requantize(rq shift 0)가 하거나, ALU MAX / MIN이 한다.

```cpp
// acc[base + f * T + i] >>= acc[src + f], i < T, f < nF
int alu_dw_shr(void *param, VTACommandHandle cmdh) {
  VTAUopLoopBegin(cmdh, p->nF_, p->T_, 1, 0);
  VTAUopLoopBegin(cmdh, p->T_, 1, 0, 0);
  VTAUopPush(cmdh, 1, 0, p->base_, p->src_, 0, 3 /* SHR */, 0, 0);
```

**schedule 줄이기.** acc tile + 2 entry와 입력 tile이 thread 몫(2048 / vt)에 들어갈 때까지 tile을 반씩 줄인다. 그래도 안 되면 vt를 1로 한다.
vt가 출력 묶음 수를 나누지 못하면 vt = 1이다. 이것이 없으면 13 묶음에서 vt 2가 아무것도 계산하지 않는 문제가 DeiT에서 있었다.

**확인.** `EVTA_RES_CHECK=1`이면 매 호출 뒤 CPU 루프로 다시 계산해 비교한다(`:5390` 이후). 시험 `tests/run_dw_test.sh`, `tests/dw_evta_test.cc`가 있다.

### 2.2 창 모드 명령 (`VTABundle.cpp:286` `gemmop_dw`, `:5334` `dwWindowWeights`, `:779` `VTAHasDwUnit`)

**언제.** bitstream 이름에 `_dw`가 있으면 창 모드다(`EVTA_DWU=0|1`로 강제할 수 있다). 3 × 3이고, tile의 uop 수(행 × 한 행의 입력 픽셀)가 uop buffer의 1/4에 들어가야 한다.

**weight 다시 배치(`dwWindowWeights`).** 대각 weight(탭마다 entry 하나)를 묶음 × ky마다 entry 하나로 바꾼다. entry ky의 행 j = 채널 j, 열 kx = 탭이다.
처음 한 번 만들어 kernel 주소별로 cache한다. buffer가 해제되면 `VTABufferFreeHook`이 cache 항목을 지운다. pool이 같은 주소를 다른 내용으로 돌려줄 수 있기 때문이다.

```cpp
w[((size_t)blk * KH + ky) * 256 + j * 16 + kx] = k[((size_t)blk * KH * KW + ky * KW + kx) * 256 + j * 17];
```

**uop 순서(`gemmop_dw`).**
- 바깥 loop: 출력 묶음(dst + T, src + 입력 tile, wgt + KH)
- 안 loop: ky(src + 입력 한 행, wgt + 1)
- uop: 출력 행마다 입력 픽셀 `(tile_w − 1) × s + KW`개를 넣는다.
  - 처음 두 픽셀은 창만 채운다.
  - 그 뒤 s번째마다 `emit`(acc에 쓰기)이다.

```cpp
const bool e = x + 1 >= ctx->kernel_w_ && (x + 1 - ctx->kernel_w_) % s == 0;
VTAUopPushDw(cmdh, out + oy * tile_w + (e ? (x + 1 - KW) / s : 0), in + oy * stride_h * in_tile_w + x, wgt, e);
```

`conv_kernel`의 filter 묶음 계산도 창 모드면 wgt를 KH entry로 센다(`:1393`).

**리뷰.**
- **`VTABufferFreeHook`은 전역 포인터 하나다.** `dwWindowWeights`가 덮어쓴다. 다른 기능이 hook을 쓰게 되면 사슬로 바꿔야 한다.
- 창 모드는 "dw 명령의 마지막 step은 emit"이라는 약속에 기댄다(sa_ctrl 주석). 하드웨어는 검사하지 않는다. 그 약속을 지키는 곳은 `gemmop_dw` 하나뿐이다.
- 작은 층(7 × 7)은 창을 채우는 2 step과 weight 채우기의 몫이 커서 덜 줄었다(0.20 → 0.17 ms).

### 2.3 `avgpool_evta` (`VTABundle.cpp:7127` `avgMulPlan`, `:7187`, `:7254`)

**계산.**
1. 입력을 identity GEMM으로 acc에 올린다.
2. ALU ADD로 HW개를 더한다.
3. `sum × M`을 MUL immediate로 곱한다. M이 16 bit를 넘으면 두 번 곱한다.
4. requantize(round 2, shift k, clip)로 int8에 쓴다.

**`avgMulPlan`.** 가능한 모든 합(−128 × HW .. 127 × HW)에서 CPU의 `round(float(sum) × r)`과 같아지는 (M, k)를 찾는다. 찾으면 (scale, area)별로 cache한다. 못 찾으면 0을 돌려 CPU로 간다. 그래서 bit-exact다.

**조건.** `VTAHasAluMul()`(`_amul` 또는 `_alux*`), `evtaRequant()`(`_rq`), global pool(KH = H, KW = W), pad 0이어야 한다.

**리뷰.**
- 출력 scratch가 `static void *scratch[4]`(EVTA마다 하나)이다. 해제하지 않고, 같은 idEVTA를 두 thread가 동시에 쓰면 안 된다. 지금 main은 copy마다 EVTA가 다르므로 괜찮다.
- 계획 찾기는 (M, k) 후보마다 모든 합을 검사한다. 처음 한 번만 하지만, 큰 area에서는 시간이 든다.

### 2.4 첫 conv 입력 묶기 NEON (`CPUBundle_generic.cpp`, `b5c4e7db`)

- 3 × 3 / 2 over 3채널(run 9, 32채널 중 18) 전용 경로를 넣었다. 한 픽셀에 `vqtbl4q` 두 번 하던 것을 `vext` + `vbsl` 하나와 `vext` + `vand` 하나로 바꿨다.
- 다음 행 prefetch를 넣고, offset 0이면 `vaddq`를 뺐다(`preprocessQuant16Mul`). 1.24 → 0.86 ms였다.
- 11-27에는 NCHW quantize의 평면 나누기를 `vld3q_s8`로 바꿨다(`preprocess_quantize_input_rows`, 1.47 → 0.64 ms, §2.9).

### 2.5 replay 명령 사본 (`cmd_queue.h:775` `ReplayVariants`, `MakeVariant`)

**왜.** 모양이 같은 호출(replay 하나)이 다른 buffer로 불리면, 매 호출 명령의 주소를 고치고 flush했다(5–10 us). 4개 동시에서는 더 컸다.

**어떻게.** buffer 주소 조합마다 명령 사본을 만든다. replay당 최대 `EVTA_REPLAY_VARIANTS`(기본 8)개이고, 그 조합의 첫 호출에서 만든다. 다음부터는 주소가 맞는 사본을 그대로 보낸다.
상한을 넘으면 예전처럼 원본을 고친다. 메모리는 `EVTA_REPLAY_MB` 안이다. 다음에 올 replay도 짐작해 찾기 시간을 줄였다(같은 commit).

### 2.6 thread별 buffer pool (`api.cc:28`)

**왜.** pool이 크기별 목록 하나였다. 4개 동시에서는 copy들이 매 추론 다른 순서로 서로의 buffer를 가져갔다.
그러면 replay된 호출마다 주소가 새로워 2.5의 사본도 소용이 없었다.

**어떻게.** 열쇠를 (thread id, 크기)로 바꿨다. 나중에 해제한 것을 먼저 꺼낸다. 자기 목록이 비면 다른 thread의 같은 크기를 가져온다(첫 추론). `EVTA_BUF_POOL_SHARED=1`이면 예전 방식이다.

**리뷰.** 상한(`EVTA_BUF_POOL_MB`)은 전체 하나다. 한 thread가 많이 쥐고 있으면 다른 thread는 새로 할당한다. 지금 쓰임(같은 bundle 4 copy)에서는 대칭이라 문제가 없다.

### 2.7 1 × 1 weight 한 LOAD (`VTABundle.cpp:1397`)

- `nFilterinLoop × cBlocks`가 thread의 wgt 몫을 넘는 1 × 1 conv에서는 filter 묶음 수를 줄여 몫에 맞춘다.
- 그러면 DMA 묶기(`wgtAll`)가 thread와 group마다 LOAD 한 번으로 한다. 묶음을 줄이지 않으면 입력 묶음마다 256 byte LOAD를 nFilterinLoop번 해야 한다.
- MobileNetV2 7 × 7 960 → 320은 10 × 60 = 600 > 512 entry였다. 5 묶음 × 2 group이 되고, 입력은 두 번 LOAD한다.
- 조건: `EVTA_WGT_FIT`(기본 1), `evtaConvBurst()`, depthwise와 pool이 아님, 1 × 1, `cBlocks > 1`

### 2.8 int8 입력 `.i8` (`VTABundle.cpp:4024` `VTASetInputInt8`, `VTAInputInt8.h`)

**흐름.**
1. main이 첫 그림을 float로 한 번 돌린다.
2. bundle의 quantize(scale, offset, `[C][256]` 표, 표의 FNV hash)를 `VTAInputInt8`로 얻는다.
3. 그것으로 만든 int8을 `<그림>.i8`로 저장한다(`INPUT_INT8_SAVE`).
4. 다음부터는 `VTASetInputInt8`로 넘기면 conv0 묶기가 quantize 없이 바이트만 옮긴다.

**맞는지.** quantize 기술자(N, H, W, C, nchw, scale, offset, hash)가 다르면 stream이 멈추고 메시지를 낸다(`:4107`). 그래서 다른 bundle의 `.i8`을 잘못 읽지 않는다.

### 2.9 다음 그림 quantize 겹치기 (`INPUT_PIPELINE`, main `multi_threads_main.cpp:382` `QuantPipe`, `:440` `WaitQuant`, `pynq_driver.cc:459`, `VTABundle.cpp:4012`)

**두 방식.**
- `=1`(`QuantPipe`): copy마다 둘째 thread가 이중 buffer의 다른 쪽에 다음 그림을 quantize한다(`request(k)`, `wait(k)`).
- `=2`(`WaitQuant`): thread를 따로 두지 않는다. copy 자신이 EVTA run을 기다리는 polling마다 한 행씩 quantize한다.
  - 드라이버에 thread-local hook을 넣었다.
  - run 뒤에 남은 행은 `finish()`가 마저 한다.

```cpp
// pynq_driver.cc RunWait loop
if (tWaitWork && !tWaitWork(tWaitArg)) tWaitWork = nullptr;
else std::this_thread::yield();
// main WaitQuant
static int step(void *a){ ... VTAInputQuantizeRows(w->src, w->buf[w->k & 1].data(), w->next, w->next + 1); return ++w->next < w->rows; }
```

**리뷰.**
- **찾은 버그(`392d1a80`):** `=2`의 첫 판은 `if (!pipe)`로 `WaitQuant`를 run마다 새로 만들었다. 그래서 buffer 0이 비어 짝수 장이 틀렸다(819).
  `if (!pipe && !wq)`로 고쳤다. 0 wrong 검사(매 추론의 출력이 첫 추론과 같은지)가 잡아냈다. 이 검사는 계속 켜 둔다.
- polling 한 번에 한 행(약 3 us)을 하므로, EVTA가 끝난 것을 그만큼 늦게 알 수 있다. 측정에서는 손해가 보이지 않았다.
- 시뮬레이터는 동기 실행이라 hook을 무시한다(`sim_driver.cc:672`). PC에서는 이 경로를 시험할 수 없다.
- **유지보수 위험:** `multi_threads_main.cpp`는 git에 15벌 사본이 있다. 보드의 `vtabundles_xcmpl/hw*/<Dir>/`에는 git 밖 사본도 있다.
  이번에 git의 공통본만 고치고 보드 사본은 옛것인 채 빌드한 일이 있었다. 하나의 원본에서 복사하는 단계(또는 include)로 바꾸는 것을 권한다.

## 3. 하드웨어: depthwise 창 장치 (`vta-hw-zcu102-systolic`, multi4도 같은 RTL)

빌드 과정, WNS, 자원, 보드 성능은 [evta_dw_hw_report.md](evta_dw_hw_report.md)에, 구조와 설계 기록은 [evta_dw_unit.md](evta_dw_unit.md)에 있다. 여기서는 코드만 본다.

### 3.1 ISA (`vtalib-zcu102-systolic/include/zcu102/vta/hw_spec.h:386`, `:541`)

- GEMM 명령의 pad bit 하나를 `dw_win`으로 썼다. 이 명령의 step은 창 모드다.
- 64-bit uop의 bit 63을 `VTA_UOP_DW_EMIT`로 썼다. 이 step은 acc에 쓴다.
- uop 폭은 그대로다(single 72 bit, multi 64 bit 안).

```c
uint64_t dw_win         : 1;   // win = {win1, win2, inp[src]}; an emit step adds sum_k w[wgt][j][k] * win_k[j] to acc[dst][j]
uint64_t pad_0          : 5;
#define VTA_UOP_DW_EMIT 63
```

### 3.2 HLS compute (`vta.cc:435`)

- compute 모듈이 uop을 `gemm_sa`로 보낼 때 bit 두 개(`dw_win`, emit)를 더 싣는다.
- C 시뮬레이션용 `gemm_sa_csim`에 창 register `win[3][16]`과 창 모드 계산을 넣었다(`#ifdef VTA_DWU`).

### 3.3 `sa_ctrl.v` (`:12`, `:82`, `:93`)

- uop의 `{emit, dw}`를 `DWU`일 때만 읽는다(`DWU = 0`이면 0).
- dw step은 입력 행과 함께 `sa_dw`로 간다.
- emit step만 tag가 valid다(acc 쓰기). tag에 dw를 실어, 출력 행을 `sa_dw`에서 받게 한다.

```verilog
wire [1:0] in_dwe = DWU ? s_uop_tdata[DW+SW+WW+31:DW+SW+WW+30] : 2'b00;   // {emit, dw}
wire       h_wr   = !h_dw || h_emit;   // the step writes acc
```

### 3.4 `sa_dw.v` (78줄)

**구조.**
- `sa_array`(systolic 16 × 16) 옆에 놓인다. 입력이 같고 지연도 같다(a+33). 그래서 뒤의 `sa_rmw`(acc read-modify-write, requantize, out 쓰기, token)는 바꾸지 않았다.
- lane j(채널 j)마다 다음을 둔다.
  - weight 3개: `b1` → `b2`. go 때 `b1`에 받고, latch flag 때 `b2`로 옮긴다. sa_array PE의 채우기 / latch 규칙과 같다.
  - 창 `w0 w1 w2`
  - 곱 3개, 합

```verilog
if (dr) begin w0[j] <= w1[j]; w1[j] <= w2[j]; w2[j] <= xr; end        // window shift on every dw step
p0[j] <= $signed(w0[j]) * $signed(b2[j][7:0]);   ... p2 ...               // a+3
sm[j] <= p0 + p1 + p2;                                                    // a+4, |sum| < 2^19
dl[0] <= sm[j]; ... row_out = dl[D-1];   // D = 29: a+4 -> a+33 (SRL)
```

- **11-25 고침:** flag / dv 한 bit가 16 lane의 `b2` / 창 register 768 bit를 한꺼번에 구동해 400 MHz에서 −0.112 ns였다.
  입력을 lane마다 한 번 더 등록하고(`keep`), 지연선을 30 → 29로 줄여 출력 시각을 지켰다.

### 3.5 `gemm_sa.v` (`:69`)

`DWU`면 `sa_dw`를 만들고, tag의 dw(`tdw`)로 출력 행을 고른다. `DWU = 0`이면 예전 회로 그대로다.

```verilog
if (DWU) begin : g_dw
  sa_dw #(.N(N), .PW(PW)) dwu (.x_in(inp_data), .f_in(flag), .dv(dv), .go(go), .wd(wgt_data), .row_out(drow));
  assign row = tdw ? drow : arow;
```

**하드웨어 리뷰.**
- **지연 결합:** `sa_dw`의 출력 시각(a+33)이 `sa_array`와 같아야 한다. 그래야 tag와 맞는다. 둘 중 하나의 pipeline 단을 바꾸면 다른 쪽의 `D`도 바꿔야 한다.
  `sa_dw.v` 머리 주석에 적혀 있지만 parameter로 묶여 있지는 않다. `localparam D`를 `sa_array`의 지연에서 계산하게 하는 것을 권한다.
- 곱셈 48개(16 lane × 3)는 LUT로 했다(DSP 0, EVTA당 LUT 약 1.3k, BRAM 0).
- 시험 순서는 다음과 같았다.
  1. C 모델(`sim/run_csim.sh DWU=1`)
  2. RTL xsim(창 벡터와 기존 5종, 역압 `GAP=40 BP=50`)
  3. vtalib 시뮬레이터(`tests/run_dw_test.sh`, 10 모양 + 경계 5)
  4. 보드(`dw_evta_test` 창 / 대각, 모델 7개 `EVTA_RES_CHECK` 0 differ)

### 3.6 ALU MUL immediate만 (`vta.cc:746`, Makefile `ALUMUL=1`, `_amul`)

- multi4 bitstream에는 ALU 확장 전체가 배치·배선되지 않았다(행 Y219 근처).
- 그래서 CNN이 쓰는 확장 연산 하나만 넣었다. immediate MUL(`dst = dst × (int16) imm`)이고, `avgpool_evta`의 /n이 쓴다.
- vtalib는 `VTAHasAluMul()`(`_amul` 또는 `_alux*`)로 고른다.

```cpp
#if defined(VTA_ALU_MUL) && !defined(VTA_ALUX)
const bool imm_mul = insn.alu_op_ext == 1 && insn.alu_opcode == 0 && insn.alu_op_ext2 == 0;
const ap_int<16> imm_m = (ap_int<16>) ((acc_T) insn.imm).range(15, 0);
```

## 4. 확인 도구 (리뷰할 때 쓰는 것)

| 도구 | 하는 일 |
|---|---|
| 0 wrong (공통 main) | 매 추론의 출력이 첫 추론과 같은지 센다. 버퍼 재사용, 사본, 이중 buffer 버그를 잡는다(§2.9 버그) |
| `EVTA_RES_CHECK=1` | fused residual, conv + pool, avgpool, depthwise를 매 호출 CPU로 다시 계산해 비교 |
| `EVTA_EPI_CHECK=1` | conv epilogue(어떻게 내렸는지도 출력) |
| `EVTA_DUMP_CONV=<dir>` | EVTA conv 출력을 모두 저장. 바꾸기 전후 바이트 비교(`_CHECK` 변수 없이) |
| `VTA_FUSION_LOG=1` | model-compiler가 무엇을 합쳤고 무엇이 왜 CPU로 갔는지 출력. bundle `.cpp` 머리에 요약 |
| `EVTA_CALL_TIME=1` | replay된 호출마다 CPU 시간을 나눠 보여 준다(bundle, pre, find, start, hook, wait) |
| `vtabundles_xcmpl/ablation_mnv2.sh` | 기능 하나씩 켜기 / 끄기를 같은 때 번갈아 잰다(논문 표 3, 4) |
| `tests/run_dw_test.sh`, `run_epi_test.sh` | PC의 vtalib 시뮬레이터 시험 |

## 5. 이미 있던 기능 (이 일에서 크게 쓰였지만 새 코드는 아님)

| 기능 | 끄면 | 문서 |
|---|---|---|
| record / replay | +15.3 ms | [evta_runtime_aot.md](evta_runtime_aot.md) §5.4 |
| out 쓰기 requantize(`_rq`) | +8.2 ms | [evta_isa_ext.md](evta_isa_ext.md) |
| buffer pool | +7.2 ms | [evta_runtime_compare.md](evta_runtime_compare.md) §7.5 |
| DMA 묶기 | +5.5 ms | 같은 문서 §7.29 |
| LOAD 앞당기기 | +1.4 ms | [evta_load_ahead_guide.md](evta_load_ahead_guide.md) |
| bias reset, ALU imm II 1 | +0.7 ms / (표 3 −3.3 ms) | [evta_isa_ext.md](evta_isa_ext.md), [evta_runtime_compare.md](evta_runtime_compare.md) §7.15 |
| conv epilogue 프로그램 | (1.9의 바탕) | [evta_alux_epilogue.md](evta_alux_epilogue.md), [evta_epilogue_guide.md](evta_epilogue_guide.md) |
| 층 사이 EVTA layout | +35.4 ms | [evta_runtime_compare.md](evta_runtime_compare.md) §7.1 |

## 6. 리뷰 의견 모음

| # | 어디 | 의견 | 심각도 |
|---|---|---|---|
| 1 | `VTASave.cpp` `dropEvtaOnlyInvalidates` | bundle 텍스트 분석이다. `kEvtaReaders`의 함수가 CPU 대체 경로에서 입력을 invalidate한다는 약속에 기댄다. vtalib의 그 함수들에 주석으로 약속을 적어 두기를 권한다 | 중 |
| 2 | `sa_dw.v` `localparam D = 29` | `sa_array` 지연과 손으로 맞춘 값이다. 한쪽 pipeline을 바꾸면 tag가 어긋난다. parameter로 묶기를 권한다 | 중 |
| 3 | `multi_threads_main.cpp` 15벌 + 보드 git 밖 사본 | 이번에 옛 사본으로 빌드한 일이 있었다. 원본 하나에서 만드는 단계를 권한다 | 중 |
| 4 | `VTABufferFreeHook` | 전역 포인터 하나를 depthwise cache가 차지한다. 둘째 사용자가 생기면 사슬로 | 낮 |
| 5 | `avgpool_evta` `static scratch[4]` | 해제하지 않음. EVTA마다 thread 하나라는 전제 | 낮 |
| 6 | `-vta-fuse-residual-norelu` 기본 켬 | `_rq`가 없는 bitstream에서는 더 느리다(표 3 단계 7). 지금 기본 bitstream은 모두 `_rq`라 괜찮다 | 낮 |
| 7 | EVTA depthwise 반올림 | half up과 `std::round`가 음수 .5에서 다르다. MobileNetV2Tf는 결과가 같았지만, 다른 모델에서는 바이트가 달라질 수 있다 | 낮 |
| 8 | 창 모드의 "마지막 step은 emit" | 하드웨어가 검사하지 않는다. `gemmop_dw` 밖에서 dw 명령을 만들면 지켜야 한다 | 낮 |
| 9 | `INPUT_PIPELINE=2` | 시뮬레이터에서 시험할 수 없다(동기 실행). 보드 0 wrong 검사가 유일한 확인 | 낮 |
| 10 | `-vta-pad-channels` | 그 conv의 MAC이 1/3 늘어난다. 조건이 보수적이라 다른 모양은 넓히지 않는다 | 정보 |
