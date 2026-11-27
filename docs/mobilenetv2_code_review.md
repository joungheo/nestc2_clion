# MobileNetV2 시간 줄이기 코드 리뷰: 컴파일러, vtalib, 하드웨어에 넣은 코드

2026-11-27 (같은 날 전 / 후 코드 추가). [mobilenetv2_evta_paper.md](mobilenetv2_evta_paper.md)의 487 → 11.6 ms(EVTA 1개), 291 fps(EVTA 4개)를 만든 코드를 기능마다 설명한다.
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
- **전 / 후**는 기능마다 두 가지로 보인다.
  - 소스: `diff` 블록이다. `-` 줄이 그 기능의 첫 commit 직전(`git show <commit>^:<파일>`), `+` 줄이 그 commit이다. 긴 함수는 바뀐 줄 둘레만 남겼다.
  - 만들어진 bundle: 논문 측정에 쓴 bundle(보드 `vtabundles_xcmpl/hw/AblMnv2_<tag>/mobilenetv2.cpp`)에서 뽑았다. `c0`은 모두 끔, `all`은 다 켬, `no_<옵션>`은 그것만 끔이다.
    긴 이름은 줄였다(`P1` = `Conv_MobilenetV2_expanded_conv_1_project_BatchNorm_FusedBatchNorm_f__2` 같은 식).

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

### 0.1 만들어진 bundle 전체: 모두 끔(`c0`) → 다 켬(`all`)

컴파일러 옵션이 bundle을 어떻게 바꿨는지는 만들어진 코드에서 가장 잘 보인다. 아래는 `mobilenetv2()` 본문의 호출 수다.

| 호출 | `c0` | `all` | 무엇이 바꿨나 |
|---|---|---|---|
| `convolution_wo_tr` (EVTA conv) | 30 | 35 | 24채널 conv 4개(§1.3), 첫 conv(§1.4) |
| `nonvtaconvolution` (CPU conv) | 6 | 0 | §1.3, §1.4, §1.7 |
| `cw_convolution` (CPU depthwise) | 17 | 0 | §1.8 |
| `dw_convolution_evta` | 0 | 17 | §1.8, §2.1 |
| `elemadd` (CPU add) | 10 | 0 | §1.9 (`VTASetConvEpilogue` 20줄 = 켜기 10, 끄기 10) |
| `transpose_nhwc2vtaio` / `transpose_vtaio2nhwc` | 30 / 30 | 0 / 0 | `-vta-keep-layout`(§1.5) |
| `VTABufferFlush` / `VTABufferInvalidate` | 30 / 30 | 0 / 1 | §1.5, §1.11 |
| `quantize` + `transpose` | 1 + 1 | 0 | `preprocess_conv0pack_stream` 1개로(§1.4) |
| `avgpool` | 1 | `avgpool_evta` + 대체 | §1.10 |
| logits | `nonvtaconvolution` | `fullyconnected_evta_packed` | §1.7 |
| `malloc` / `free` (CPU 활성값) | 57 | 4 | 활성값이 VTA buffer에 남음 |

`VTA_FUSION_LOG` 요약(bundle 머리 주석)도 그대로 비교된다.

```diff
-//   30 EVTA producers, 0 with a tail, 10 read by a tail op not fused
-//     not fused: 9 x an add: -vta-fuse-elemadd-mul is off (elemadd_evta is faster)
-//     not fused: 1 x avgpool_evta on its own
-//   23 GEMMs (conv / fc / matmul) on the CPU
-//     on the CPU: 17 x channelwise conv: -vta-evta-depthwise is off
-//     on the CPU: 2 x conv: input channels 24 not a multiple of 16 (nor the 3 of a first conv)
-//     on the CPU: 2 x conv: output channels 24 not a multiple of 16
-//     on the CPU: 1 x conv: output channels 1001 not a multiple of 16
-//     on the CPU: 1 x conv: pads 0 0 1 1 not all equal
+//   35 EVTA producers, 10 with a tail, 1 read by a tail op not fused
+//     not fused: 1 x avgpool_evta on its own
+//   0 GEMMs (conv / fc / matmul) on the CPU
```

모델 첫머리(입력 → 첫 conv → depthwise → project 1 × 1)의 코드다.

```cpp
// c0: 모두 끔 (487 ms)
quantize(input, q, 150528, 1/128.0, 0);                                    // float -> int8 (NCHW)
transpose(q, x, 1, 3, 224, 224, 1, 224, 224, 3, 0, 2, 3, 1);               // NCHW -> NHWC
nonvtaconvolution(x, ..., c0_res, ..., 1, 224, 224, 3, 32, 3, 3, 0, 2, ...); // 첫 conv: CPU (pad 0 0 1 1)
cw_convolution(c0_res, filter, bias, mult, dw0_res, 1, 112, 112, 32, ..., 0, 96, 0, 0);  // depthwise: CPU, NHWC
P0_in = VTABufferAlloc(401408);
transpose_nhwc2vtaio(dw0_res, P0_in, 1, 112, 112, 32);                     // NHWC -> EVTA layout
VTABufferFlush(P0_in, 0, 401408);
P0_out = VTABufferAlloc(200704);
convolution_wo_tr(P0_in, ..., P0_out, 1, 112, 112, 32, 16, 1, 1, ...);     // project 1 x 1: EVTA
VTABufferInvalidate(P0_out, 0, 200704);
transpose_vtaio2nhwc(P0_out, P0_res, 1, 112, 112, 16);                      // EVTA layout -> NHWC

// all: 다 켬 (11.70 ms)
preprocess_conv0pack_stream((const float *)input, c0_in, 1, 224, 224, 3, 1, 1/128.0, 0, nullptr,
                            3, 3, 2, 0, 32, 1);                            // quantize + 묶기, 마지막 1 = padEnd
convolution_wo_tr(c0_in, ..., c0_vbuf, 1, 113, 112, 32, 32, 2, 1, ...);    // 첫 conv: EVTA (2 x 1 over 32채널)
dw_convolution_evta(c0_vbuf, filter, bias, dw0_shift, dw0_vbuf, 1, 112, 112, 32, 3, 3, 1, 1, 112, 112, 0, 96, ...);
convolution_wo_tr(dw0_vbuf, ..., P0_vbuf, 1, 112, 112, 32, 16, 1, 1, ...); // transpose, flush, invalidate 없음
```

## 1. 컴파일러 (NEST-C VTA backend)

### 1.1 depthwise만 채널별 양자화 (`Quantization.cpp:30`, `enableChannelwise(groupedOnly)` `:834`)

**왜.** MobileNetV2의 depthwise filter는 BN을 접어 넣어서, 한 filter 안의 채널 최댓값이 0.3 ~ 56으로 벌어진다.
scale을 하나만 쓰면 작은 채널에 0 ~ 2단계만 남는다. 그래서 cat_285가 293으로 틀렸다.

**어떻게.** Glow에는 `-enable-channelwise`가 있었지만 모든 conv를 채널별로 만든다. EVTA conv는 scale 하나를 shift로 쓰므로 그러면 안 된다.
그래서 `groupedOnly`로 group > 1인 conv만 `ChannelwiseQuantizedConvolution`으로 바꾼다.

**전 / 후** (`69183921`): Glow 원래 함수에 인자 하나와 건너뛰기 한 줄을 넣었다.

```diff
-  void enableChannelwise() {
+  //#### JO changed (2026-11-07): \p groupedOnly: only the convs with group > 1 (-enable-channelwise-depthwise)
+  void enableChannelwise(bool groupedOnly = false) {
     ...
       if (auto *convNode = llvm::dyn_cast<ConvolutionNode>(Q->getInput())) {
+        if (groupedOnly && convNode->getGroup() == 1) {
+          continue;
+        }
 ...
   if (quantConfig.enableChannelwise) {
     quantizer.enableChannelwise();
+  } else if (enableChannelwiseDepthwise) {
+    quantizer.enableChannelwise(/* groupedOnly */ true);
   }
```

bundle에서는 depthwise가 `convolution`(scale 하나)에서 `ChannelwiseQuantizedConvolution`(채널마다 `mult`)으로 바뀐다. 그 호출이 `cw_convolution(..., mult, ...)`다(§0.1 `c0`).

**리뷰.**
- NEST-C의 `lib/Quantization/Base/Base.cpp`(int8 지수 하한 −7)는 `NESTC_WITH_NMP`일 때만 빌드된다. 이 보드는 Glow 것을 쓴다(CLAUDE.md).
  채널별 scale도 power-of-two여야 1.8의 EVTA depthwise가 shift로 받을 수 있다. `VTASave`가 `m[d] == 2^-k`가 아닌 채널을 만나면 CPU로 둔다.

### 1.2 ReLU6를 conv의 relu + 위 clip으로 (`2944ce3b`)

- TF 모델의 `Clip(0, 6)`을 int8로 남기고, 앞 conv의 fused relu와 위 clip으로 합친다.
- EVTA conv는 이미 clip(MIN / MAX 또는 requantize의 `rq_lo` / `rq_hi`)을 하므로 비용이 없다.
- 1.8의 depthwise도 `lo`, `hi`를 받는다(`VTASave.cpp:990` 근처, `lround(args / outScale)`).

**전 / 후** (`2944ce3b`, `include/Backends/VTA/VTA.h`, `VTASave.cpp`):

```diff
     bool V = parent->getKind() == Kinded::Kind::ConvolutionNodeKind &&
-             activation->getKind() == Kinded::Kind::ReluNodeKind;
+             (activation->getKind() == Kinded::Kind::ReluNodeKind ||
+              (activation->getKind() == Kinded::Kind::ClipNodeKind &&
+               llvm::cast<ClipNode>(activation)->getMin() == 0.0f &&
+               llvm::cast<ClipNode>(activation)->getMax() > 0.0f));
 ...
-  bool doRelu = Inst->getFusedActivation() == RELU;
+  bool doRelu = convRelu(Inst);            // RELU or CLIP(0, max)
 ...
+  const int clipHi = convClipHi(Inst);     // lround(6 / outScale), 127 = none
+  if (clipHi < 127)
+    bundle->append("  VTASetActivationClip(" + std::to_string(clipHi) + ");\n");
```

그 전에는 `Clip`이 VTA에서 int8로 지원되지 않았다. 그래서 양자화기가 float로 남기고, conv마다 dequantize → clip → quantize가 CPU에 들어갔다.
지금 bundle에서는 그 conv 앞뒤의 `VTASetActivationClip(96)` / `VTASetActivationClip(127)` 두 줄로 남는다(§0.1).

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

**전 / 후.** 소스는 새 pass(`14c77182`, 111줄 추가)라 비교할 이전 코드가 없다. 만들어진 bundle(`no_pad-channels` → `all`)의 첫 24채널 project conv(96 → 24, 56 × 56)는 다음과 같다.

```diff
-  // CPU conv, NHWC; the depthwise before it and the conv after it on the CPU too (cw_convolution, nonvtaconvolution)
-  nonvtaconvolution(DW1_res, 1.0/16, 0, Const_73__1, 1.0/32, 0, P1_bias, 1.0/512, 0, P1_res, 1.0/4, 0,
-                    1, 56, 56, 96, 24, 1, 1, 0, 1, 1, 1, 0, 1, 56, 56);
+  // EVTA conv, output 32 channels (8 of them 0), the weights / bias widened at compile time (_pad16)
+  convolution_wo_tr(DW1_res_vbuf, Const_73__1_pad16, (int32_t *)P1_bias_pad16, P1_res_vbuf,
+                    1, 56, 56, 96, 32, 1, 1, 0, 1, 0, 1, 7, 56, 56, vtaCmdH, 2, 14, 56, 0);
```

이 옵션 하나를 끄면 conv 4개만 CPU로 가는 것이 아니다. `no_pad-channels` bundle에는 CPU conv(`nonvtaconvolution`) 4개, 그 이웃 depthwise 3개(`cw_convolution`, EVTA layout을 못 받음), CPU `elemadd` 1개가 남는다. 이 옵션의 +267.8 ms가 표 4에서 가장 크다.

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

**전 / 후** (`8bde38b4`, `VTASave.cpp` `getConv0Pack`, `getConvPath`):

```diff
-  int s = strides[0], pad = pads[0];
-  if (strides[1] != s || pads[1] != pad || pads[2] != pad || pads[3] != pad || s < 1 || KN % 16)
+  int s = strides[0], pad = pads[0], padEnd = pads[2];
+  if (strides[1] != s || pads[1] != pad || pads[3] != padEnd || s < 1 || KN % 16)
     return false;
   int outH = destDims[1], outW = destDims[2];
-  if ((H + 2 * pad - KH) / s + 1 != outH || (W + 2 * pad - KW) / s + 1 != outW)
+  if ((H + pad + padEnd - KH) / s + 1 != outH || (W + pad + padEnd - KW) / s + 1 != outW)
     return false;
 ...
   // getConvPath: the 3-channel first conv
-  } else if (symPad && Inst->getGroup() == 1 && scale > 1 && srcDims[3] == 3 &&
+  } else if ((symPad || getConv0Pack(Inst, nullptr)) && Inst->getGroup() == 1 && scale > 1 && srcDims[3] == 3 &&
```

vtalib(`VTABundle.h`)의 묶기 함수 넷은 끝에 `int padEnd = -1`(−1이면 `pad`와 같음)을 받는다. 그래서 대칭 pad 모델(Resnet18 / 50)의 bundle은 그대로다.

만들어진 bundle(`c0` → `all`)은 §0.1의 첫 세 줄이다. CPU `quantize` + `transpose` + `nonvtaconvolution` 3 × 3 / 2 over 3채널이 `preprocess_conv0pack_stream` + EVTA 2 × 1 / 1 over 32채널(`..., 3, 3, 2, 0, 32, 1`의 마지막 1이 `padEnd`)이 된다.

**리뷰.**
- 위 = 왼쪽, 아래 = 오른쪽이 아닌 pad(예: 위 1, 왼쪽 0)는 받지 않는다. TF / PyTorch의 보통 모델은 모두 이 형태다.
- 18채널을 32로 채우므로 GEMM의 14/32가 0이다. 3채널 zero-fill(3/16)보다는 훨씬 낫다.

### 1.5 depthwise가 EVTA layout을 읽고 씀 (`59d5a908`, vtalib `cw_convolution` inPacked / outPacked)

- 처음 depthwise(CPU, NEON `cw_convolution`)는 NHWC를 읽고 썼다. 앞뒤 EVTA conv와의 사이에 transpose가 두 번씩 들어갔다.
- `findPackedActivations`가 depthwise의 입력과 출력도 EVTA layout(N{C/16}HW16)으로 두게 했다. `cw_convolution`은 두 layout을 모두 읽고 쓴다.
- 1.8의 EVTA depthwise는 입력과 출력이 모두 EVTA layout일 때만 탄다. 그래서 `-vta-keep-layout`을 끄면 depthwise 17개가 모두 CPU로 간다. 논문 표 4의 +35.4 ms에는 이것이 들어 있다.

**전 / 후** (`59d5a908`). 컴파일러는 `findPackedActivations`에 depthwise를 넣었다.

```diff
+      case Kinded::Kind::ChannelwiseQuantizedConvolutionInstKind:
+        if (isDepthwiseCW(llvm::cast<ChannelwiseQuantizedConvolutionInst>(&I)))   // group = C = KN, C % 16 = 0
+          continue;     // reads and writes either layout: its input / output can stay packed
+        break;
       case Kinded::Kind::ElementAddInstKind:
```

vtalib `cw_convolution`(`CPUBundle_generic.cpp`)은 주소 계산을 layout별 보폭으로 바꿨다.

```diff
-                   int dilH, int dilW, int group, int outH, int outW, int lo, int hi) {
+                   int dilH, int dilW, int group, int outH, int outW, int lo, int hi, int inPacked, int outPacked) {
+  // element (y, x, c) at y * rs + x * ps + (c / 16) * bs + c % 16: NHWC rs = W * C, ps = C, bs = 16;
+  // EVTA layout rs = W * 16, ps = 16, bs = H * W * 16
 ...
-                const int8x16_t x = vld1q_s8(in + ((size_t)iy * W + ix) * C + c);
+                const int8x16_t x = vld1q_s8(ib + iy * irs + ix * ips);
 ...
-            vst1q_s8(o + c, vcombine_s8(vqmovn_s16(l), vqmovn_s16(h)));
+            vst1q_s8(out + (size_t)(c / 16) * obs + oy * ors + ox * ops, vcombine_s8(vqmovn_s16(l), vqmovn_s16(h)));
```

bundle에서는 depthwise 앞뒤의 `transpose_nhwc2vtaio` / `transpose_vtaio2nhwc`와 그 flush / invalidate가 빠진다(§0.1의 `c0` 7 ~ 11줄).

### 1.6 EVTA conv shift 반올림 (`VTASave.cpp:1319` 옵션, `:1327` `convRoundHalf`)

- EVTA conv의 출력 shift는 원래 버림이었다. CPU conv와 Glow Interpreter는 `std::round`다.
- 그래서 컴파일 때 bias에 2^(shift−1)을 더한다. 첫 conv의 bias에는 11-22에야 넣었다(`:1868`, Resnet50 44.66 → 52.92 %).
- MobileNetV2 bundle은 늘 켠다(`gen_calib_bundle.sh`).
- **전 / 후** (`32c11097`, `prepareVTAConvolutionInst`의 bias 쓰기):

  ```diff
  +  const int32_t roundHalf = convRoundHalf(Inst, ctx);   // 2^(shift - 1), 0 when -vta-round-shift is off
     {
  +    doBias = roundHalf != 0;      // a conv with no bias gets one now
       for (size_t i = 0, e = handleBias.size(); i < e; i++) {
  ...
       for (size_t i = 0, e = handleBias.size(); i < e; i++) {
  -      auto data = handleBias.raw(i);
  +      int32_t data = handleBias.raw(i) + roundHalf;
         fos->write((const char *)&data, 4);
  ```

  bundle의 호출 줄은 같다. weight 파일의 bias 값만 바뀐다.
- **리뷰:** bias에 2^(shift−1)을 더하고 산술 shift하므로 음수의 .5는 위로 간다(half up). `std::round`(half away from zero)와 음수 .5에서만 다르다. 정확도 기록은 [evta_accuracy.md](evta_accuracy.md) §6에 있다.

### 1.7 logits를 EVTA fc로 (`VTASave.cpp:4975` `isConvAsFc`, `:4983` `saveEvtaConvAsFc`)

- MobileNetV2의 마지막 층은 1 × 1 입력 위 1 × 1 conv(1280 → 1001)다.
- 출력 1001이 16의 배수가 아니라 conv 경로가 거절했다. 그래서 같은 계산인 EVTA fc(`saveEvtaFcPacked`)로 보낸다.
- EVTA fc는 출력 묶음 단위로 chunk를 돌고, 남는 묶음은 packed weight로 CPU에서 한다(`fc_packed_cpu`).
- 조건: batch 1, 1 × 1 입력 / 출력 / 커널, group 1, 활성 함수 없음, pad 0.

**전 / 후** (`b66c755a`). fc 함수의 몸통을 `saveEvtaFcPacked`로 떼고, conv filter 순서(`[OUT][1][1][IN]`)를 받게 했다.

```diff
-static bool saveEvtaFullyConnectedInst(const glow::FullyConnectedInst *Inst, std::string *bundle,
-                                       VTASaveContext *ctx) {
-  ... (shape checks) ...
+static bool saveEvtaFcPacked(Value *src, Value *dest, Value *filter, Value *bias, uint32_t IN, uint32_t OUT,
+                             bool convFilter, std::string *bundle, VTASaveContext *ctx) {
 ...
-      w[((size_t)(j / 16) * CIN + k / 16) * 256 + (j % 16) * 16 + k % 16] = handle.raw((size_t)k * OUT + j);
+      w[((size_t)(j / 16) * CIN + k / 16) * 256 + (j % 16) * 16 + k % 16] =
+          handle.raw(convFilter ? (size_t)j * IN + k : (size_t)k * OUT + j);
 ...
 void saveConvolutionInst(const glow::ConvolutionInst *Inst, std::string *bundle,
                          std::string *initConst, VTASaveContext *ctx) {
+  if (saveEvtaConvAsFc(Inst, bundle, ctx))   // isConvAsFc && saveEvtaFcPacked(..., convFilter = true)
+    return;
```

만들어진 bundle(`c0` → `all`):

```diff
-  nonvtaconvolution(AvgPool_res, 1.0/32, 0, Const_222__1, 1.0/512, 0, Logits_bias, 1.0/16384, 0, Logits_res, ...,
-                    1, 1, 1, 1280, 1001, 1, 1, 0, 1, ...);
+  fullyconnected_evta_packed(AvgPool_res, Const_222__1, Logits_bias, Logits_res, 1280, 1001, 11, vtaCmdH, 0);
```

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

**전 / 후** (`5e0685ea`). `saveChannelwiseConvolutionInst`는 그 전에 늘 CPU용 weight(`[KH][KW][C]`)와 float 배율 `mult`를 쓰고 `cw_convolution`을 불렀다. 그 앞에 EVTA 분기가 들어갔다.

```diff
+  std::vector<int32_t> sh(KN, -1);           // per-channel shift k when m[d] == 2^-k
+  ...
+  if (vtaEvtaDepthwise && dw && C % 16 == 0 && N == 1 && ctx->isPacked(src) && ctx->isPacked(dest) && ...) {
+    ... diagonal weights, bias + 2^(sh - 1), shifts to the weight file ...
+    bundle->append("  dw_convolution_evta(");
+    ...
+    ctx->getEvtaDepthwise()->insert(Inst);   // no CPU flush of its packed output after it
+    return;
+  }
   auto fos = ctx->getWeightFileStream();
   fos->write((const char *)w.data(), w.size());        // the CPU path as before
```

만들어진 bundle(`no_evta-depthwise` → `all`, 첫 depthwise):

```diff
-  cw_convolution(c0_res, filter, bias, (float *)dw0_mult, dw0_res,
-                 1, 112, 112, 32, 32, 3, 3, 1, 1, 1, 1, 1, 1, 32, 112, 112, 0, 96, 1, 1);
+  dw_convolution_evta(c0_vbuf, filter, bias, dw0_shift, dw0_vbuf,
+                      1, 112, 112, 32, 3, 3, 1, 1, 112, 112, 0, 96, vtaCmdH, 2, 4, 112, 0);
```

`cw_convolution`의 끝 `1, 1`은 입력과 출력이 EVTA layout이라는 뜻이다(§1.5). `dw_convolution_evta`의 끝 넷은 schedule(vt 2, tile 4 × 112)과 EVTA 번호다.

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

**전 / 후** (`bf20900a`, `findResidualFusions`): add 다음이 relu가 아니면 건너뛰던 것을, relu 없이도 합친다.

```diff
     size_t k = next(j);
-    if (k >= ins.size())
-      continue;
-    auto *Rl = llvm::dyn_cast<ReluInst>(ins[k]);
-    if (!Rl || Rl->getSrc() != O)
-      continue;
-    const Value *O2 = Rl->getDest();
+    auto *Rl = k < ins.size() ? llvm::dyn_cast<ReluInst>(ins[k]) : nullptr;
+    if (Rl && Rl->getSrc() != O)
+      Rl = nullptr;
+    if (!Rl) {
+      if (!vtaFuseResidualNoRelu)
+        continue;
+      k = j;                                // the fused chain ends at the add
+    }
+    const Value *O2 = Rl ? Rl->getDest() : O;
 ...
-    ResidualFusion f = {R, O2, m + eD, m + eR, m, q, hoist};
+    ResidualFusion f = {R, O2, m + eD, m + eR, m, q, hoist, Rl != nullptr};
```

같은 commit에서 `convRoundHalf`가 residual을 합친 conv에도 반올림 bias를 넣게 됐다. vtalib가 add 전에 conv를 보통 conv처럼 requantize하기 때문이다.
만들어진 bundle(`c0` → `all`, 첫 residual add):

```diff
-  // project conv on the CPU (24 channels), then the add on the CPU
-  nonvtaconvolution(DW2_res, ..., P2_res, ..., 1, 56, 56, 144, 24, ...);
-  elemadd(P2_res, 1.0/4, 0, P1_res, 1.0/4, 0, P2_res, 1.0/4, 0, 75264);
+  VTAEpilogue P2_epi = {2, {{1, 0, 0, 0, 0}, {2, 2, 0, -128, 127}}};   // ADD t[0], RQ round 2 clip -128..127
+  P2_epi.t[0] = P1_res_vbuf;                                           // the residual
+  VTASetConvEpilogue(&P2_epi);
+  convolution_wo_tr(DW2_res_vbuf, Const_115__1_pad16, P2_bias_pad16, P2_res_vbuf, 1, 56, 56, 144, 32, 1, 1, ...);
+  VTASetConvEpilogue(nullptr);
```

**리뷰.**
- 하드웨어(`_rq`)를 전제로 한 컴파일러 기능이다. 기준 bitstream에서는 오히려 4.3 ms 느렸다(논문 표 3 단계 7).
  옵션 기본값이 bitstream과 무관하게 켜져 있는 것은, 지금 기본 bitstream이 모두 `_rq`라서다.
- `EVTA_EPI_CHECK=1`로 매 호출을 CPU와 비교할 수 있다.

### 1.10 global avgpool을 EVTA ALU로 (`VTASave.cpp:5162` 옵션, `:5272` emit)

- bundle은 `avgpool_evta(...)`를 부르고, 0을 돌려받으면 CPU avgpool로 간다.
- 그래서 같은 bundle이 ALU MUL이 없는 bitstream에서도 돈다. vtalib 쪽은 §2.3에서 다룬다.
- **전 / 후** (`bf20900a`). `saveAvgPoolInst`는 `bundle`에 바로 쓰던 호출을 `callBuf`에 만든 뒤 붙인다. 만들어진 bundle(`c0` → `all`):

  ```diff
  -  avgpool(Conv_1_res, 1.0/16, 0, AvgPool_res, 1.0/32, 0, 80, 7, 7, 16, 80, 1, 1, 16, 7, 7, 0, 1);
  +  if (!avgpool_evta(Conv_1_res_vbuf, 1.0/16, AvgPool_res, 1.0/32, 80, 7, 7, 7, 7, 0, 1, vtaCmdH))
  +    avgpool(Conv_1_res, 1.0/16, 0, AvgPool_res, 1.0/32, 0, 80, 7, 7, 16, 80, 1, 1, 16, 7, 7, 0, 1);
  ```

  `VTA_OUTPUT_LUT`이 없는 vtalib(`vta/vtalib`)로 빌드하면 `#else`의 `avgpool`만 남는다.

### 1.11 EVTA만 읽는 출력의 invalidate 빼기 (`VTASave.cpp:9869` `dropEvtaOnlyInvalidates`)

**왜.** bundle은 EVTA 호출 뒤마다 출력에 `VTABufferInvalidate`(`dc civac` 루프)를 불렀다. 다음 EVTA 호출만 읽는 출력에는 필요 없다. EVTA는 DRAM을 읽기 때문이다. 1장에 0.5 ms였다.

**어떻게.** 만들어진 bundle 본문 **텍스트**에서 한다. 한 줄에 문장 하나라는 규칙에 기댄다.
1. invalidate 뒤로, 그 buffer의 이름과 그 이름에서 선언한 포인터(별칭, 가상 주소)를 따라간다.
2. buffer가 해제되거나 다음 invalidate가 나올 때까지, 이름이 나오는 줄을 하나씩 본다. 허용되는 줄은 다음 셋뿐이다.
   - `kEvtaReaders`(`convolution_wo_tr*`, `dw_convolution_evta`, `maxpool_evta`, `VTASetConvEpilogue`)의 호출
   - epilogue residual 저장(`<x>_epi.t[k] = <name>;`)
   - 그 이름에서 포인터를 선언하는 줄
3. 모든 줄이 이 셋이면 invalidate를 지운다. 다른 줄(CPU 함수, `if`, 함수 끝)이 하나라도 있으면 남긴다.

**전 / 후** (`0b1439b7`). bundle을 다 만든 뒤 한 번 도는 새 함수(138줄)라 소스의 이전 코드는 없다. 만들어진 bundle(`no_skip-evta-invalidate` → `all`)에서는 52개 가운데 51개가 빠진다.

```diff
   convolution_wo_tr(c0_in, Const_90__1, ..., c0_out, 1, 113, 112, 32, 32, 2, 1, ...);
-  VTABufferInvalidate(c0_out, 0, 401408);
   VTABufferFree(c0_in);
 ...
   dw_convolution_evta(c0_vbuf, filter, bias, dw0_shift, dw0_vbuf, ...);
-  VTABufferInvalidate(dw0_vbuf, 0, 401408);
 ...
   (49 more)
```

남는 하나는 마지막 conv(`Conv_1`, 7 × 7 × 1280)의 출력이다. 다음이 CPU `avgpool`일 수 있는 `if` 줄이라 지우지 않는다.

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

**전 / 후** (`5e0685ea`). 그 전에 depthwise는 CPU의 `cw_convolution`(NEON, 16채널씩 int8 곱을 int32로)이었다.

```cpp
// before: CPUBundle_generic.cpp cw_convolution, per output pixel and 16 channels
for (int ky = 0; ky < KH; ky++) for (int kx = 0; kx < KW; kx++) {
  const int8x16_t x = vld1q_s8(ib + iy * irs + ix * ips), w = vld1q_s8(kernel + ((size_t)ky * KW + kx) * C + c);
  const int16x8_t pl = vmull_s8(vget_low_s8(x), vget_low_s8(w)), ph = vmull_high_s8(x, w);
  a0 = vaddw_s16(a0, vget_low_s16(pl)); ...                       // 4 x int32x4 sums
}
r = round(float(sum + bias) * mult), clip(lo, hi)                 // float multiplier per channel
```

`conv_kernel`에는 depthwise 분기 셋이 들어갔다. 입력 묶음 loop가 없어지고, 출력 묶음마다 자기 입력 tile을 LOAD하고, 끝에 채널별 SHR을 한다.

```diff
-    for (uint32_t channelIdx = 0; channelIdx < numInputChannel / 16; channelIdx++) {
+    const uint32_t cBlocks = dw ? 1 : numInputChannel / 16;
+    for (uint32_t channelIdx = 0; channelIdx < cBlocks; channelIdx++) {
 ...
         VTADepPop(vtaCmdH, 2, 1);
+        if (dw) {                         // each output block's own input block, into the thread's inp half
+          for (uint32_t fIdx = 0; fIdx < nFilterinLoop; fIdx++) {
+            const uint32_t blk = (filterIdx * nVirtualThread + threadIdx) * nFilterinLoop + fIdx;
+            VTALoadBuffer2D(vtaCmdH, vta_input_buf, blk * inputHStride * inputWStride + ..., 
+                            threadIdx * (INP_MEM_ENTRY / nVirtualThread) + fIdx * inTile, 2);
+          }
+        } else
         if(threadIdx==0) {                // dense conv: one input tile for all kernels
 ...
+      if (dw) {                           // per-channel shift, then clip
+        VTALoadBuffer2D(vtaCmdH, dw->shift, ..., shiftSram, 3);
+        if (evtaRequant()) {
+          VTARequant(vtaCmdH, 1, 0, 0, dw->lo, dw->hi);
+          pushDwShr(vtaCmdH, idEVTA, outputSramBaseAddr, shiftSram, T, nFilterinLoop);
+          VTARequant(vtaCmdH, 0, 0, 0, 0, 0);
+        } else { pushDwShr(...); pushAlu(... RES_ALU_MAX ... dw->lo); pushAlu(... RES_ALU_MIN ... dw->hi); }
+      } else
       if (!rqConv && !rqPool) {           // the dense conv's scalar SHR / MAX / MIN as before
```

`gemmop_mxn`에는 `in_kernel_factor_`(kernel마다 입력 tile 간격, dense conv는 0)만 더했다. uop 순서는 dense conv와 같다.

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

**전 / 후** (`818a73c3`). 대각 모드는 dense conv와 같은 `gemmop_mxn`을 쓴다. 출력 픽셀마다 탭 수(3 × 3 = 9)만큼 GEMM step이 들고, step마다 16 × 16 배열의 대각선만 쓴다.

```cpp
// diagonal (before, and still without "_dw"): gemmop_mxn, it_out = blocks, it_in = kx (src + 1, wgt + 1)
for (int dx = 0; dx < ctx->kernel_h_; dx++)                       // ky
  for (int y = 0; y < ctx->tile_size_h_; y++)
    for (int x = 0; x < ctx->tile_size_w_; x++)                   // one uop per output pixel and ky
      VTAUopPush(cmdh, 0, 0, out + y * tile_w + x, in + (y * stride_h + dx) * in_tile_w + x * stride_w,
                 wgt + dx * kernel_w, 0, 0, 0);
// steps per output pixel: KH x KW = 9
```

```cpp
// window (after, "_dw"): gemmop_dw, it_out = blocks, it_in = ky (src + one input row, wgt + 1)
for (uint32_t oy = 0; oy < tile_h; oy++)
  for (uint32_t x = 0; x < (tile_w - 1) * s + KW; x++) {          // one uop per input pixel of the row
    const bool e = x + 1 >= KW && (x + 1 - KW) % s == 0;           // the window is full: write
    VTAUopPushDw(cmdh, out + oy * tile_w + (e ? (x + 1 - KW) / s : 0), in + oy * stride_h * in_tile_w + x, wgt, e);
  }
// steps per output pixel: KH x s (stride 1: 3, stride 2: 6), each using all 16 x 3 multipliers of sa_dw
```

`conv_kernel`에서는 weight LOAD와 uop kernel 고르기만 바뀌었다.

```diff
-        } else
+        } else if (dw && dw->winW)      // window-mode weights, KH entries per block
+          loadBuffer2D(vtaCmdH, dw->winW, filter_offset / KW, KH, nFilterinLoop, KH, 0, 0, 0, 0, weightSramBaseAddr, 1);
+        else
           loadBuffer2D(vtaCmdH, vta_filter_buf, filter_offset, KH * KW, nFilterinLoop, KH * KW * cBlocks, ...);
 ...
+        if (gemm_ctx.dw_win_)
+          VTAPushGEMMOp(vtaCmdH, uopHandle_gemm, &gemmop_dw, &gemm_ctx, 0);
+        else
         if(KH>1)
           VTAPushGEMMOp(vtaCmdH, uopHandle_gemm, &gemmop_mxn, &gemm_ctx, 0);
```

bundle은 그대로다. 같은 `dw_convolution_evta` 호출이 `_dw` bitstream에서 창 모드로 간다.

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

**전 / 후.** 새 함수(`bf20900a`)다. 그 전에는 CPU `avgpool`(16채널 묶음을 batch로 본 NHWC)이었다. bundle의 차이는 §1.10에 있다.

**리뷰.**
- 출력 scratch가 `static void *scratch[4]`(EVTA마다 하나)이다. 해제하지 않고, 같은 idEVTA를 두 thread가 동시에 쓰면 안 된다. 지금 main은 copy마다 EVTA가 다르므로 괜찮다.
- 계획 찾기는 (M, k) 후보마다 모든 합을 검사한다. 처음 한 번만 하지만, 큰 area에서는 시간이 든다.

### 2.4 첫 conv 입력 묶기 NEON (`CPUBundle_generic.cpp`, `b5c4e7db`)

- 3 × 3 / 2 over 3채널(run 9, 32채널 중 18) 전용 경로를 넣었다. 한 픽셀에 `vqtbl4q` 두 번 하던 것을 `vext` + `vbsl` 하나와 `vext` + `vand` 하나로 바꿨다.
- 다음 행 prefetch를 넣고, offset 0이면 `vaddq`를 뺐다(`preprocessQuant16Mul`). 1.24 → 0.86 ms였다.
- **전 / 후** (`b5c4e7db`, `conv0packRows`):

  ```cpp
  // before (any run): two padded input rows as 4 registers, one 4-register table lookup per 16 output channels
  for (int x = 0; x < W2; x++, r0 += step, r1 += step) {
    uint8x16x4_t t = {{vld1q_u8(r0), vld1q_u8(r0 + 16), vld1q_u8(r1), vld1q_u8(r1 + 16)}};
    vst1q_u8(o0 + 16 * x, vqtbl4q_u8(t, i0));
    if (CB > 1) vst1q_u8(o1 + 16 * x, vqtbl4q_u8(t, i1));            // MobileNetV2: CB = 2
    ...
  }
  ```

  ```cpp
  // after (s 2, run 9, CO 32): channels 0-15 = row 0 bytes 0-8 + row 1 bytes 0-6, 16-17 = row 1 bytes 7-8
  for (int x = 0; x < W2; x++, r0 += 6, r1 += 6) {
    const uint8x16_t a = vld1q_u8(r0), b = vld1q_u8(r1);
    vst1q_u8(o0 + 16 * x, vbslq_u8(k9, a, vextq_u8(z, b, 7)));
    vst1q_u8(o1 + 16 * x, vandq_u8(vextq_u8(b, z, 7), k2));
  }
  ```

  ```diff
  -            q.val[c] = preprocessQuant16(x[c], vs, vo, mul);
  +            q.val[c] = noadd ? preprocessQuant16Mul(x[c], vs) : preprocessQuant16(x[c], vs, vo, mul);
  ```
- 11-27에는 NCHW quantize의 평면 나누기를 `vld3q_s8`로 바꿨다(`preprocess_quantize_input_rows`, 1.47 → 0.64 ms, §2.9).

### 2.5 replay 명령 사본 (`cmd_queue.h:775` `ReplayVariants`, `MakeVariant`)

**왜.** 모양이 같은 호출(replay 하나)이 다른 buffer로 불리면, 매 호출 명령의 주소를 고치고 flush했다(5–10 us). 4개 동시에서는 더 컸다.

**어떻게.** buffer 주소 조합마다 명령 사본을 만든다. replay당 최대 `EVTA_REPLAY_VARIANTS`(기본 8)개이고, 그 조합의 첫 호출에서 만든다. 다음부터는 주소가 맞는 사본을 그대로 보낸다.
상한을 넘으면 예전처럼 원본을 고친다. 메모리는 `EVTA_REPLAY_MB` 안이다. 다음에 올 replay도 짐작해 찾기 시간을 줄였다(같은 commit).

**전 / 후** (`1cdf422d`, `RunReplay`):

```diff
-  void RunReplay(Replay &r, void *const *bufs, int nbufs, double tb = 0) {
-    bool moved = false;
-    for (int i = 0; i < nbufs; i++) {
-      vta_phy_addr_t a = bufs[i] ? DataBuffer::FromHandle(bufs[i])->phy_addr() : 0;
-      if (a != r.bufs[i]) moved = true;
-      r.bufs[i] = a;
-    }
-    if (moved) {                 // any buffer moved: rewrite every relocated instruction and flush them all
-      for (auto &c : r.relocs)
-        InsnAt(r.segs[c.seg].insn, c.idx)->dram_base = (uint32_t)(r.bufs[c.buf] / c.elem_bytes) + c.elem_offset;
-      for (auto &g : r.segs) VTAFlushCache(g.insn, g.insn_phy, g.count * sizeof(VTAGenericInsn));
+  void RunReplay(Replay &owner, void *const *bufs, int nbufs, double tb = 0) {
+    for (int i = 0; i < nbufs; i++) a[i] = bufs[i] ? DataBuffer::FromHandle(bufs[i])->phy_addr() : 0;
+    Replay *pick = same(owner) ? &owner : nullptr;              // the owner or a variant with these addresses
+    for (size_t k = 0; !pick && k < owner.variants.size(); k++)
+      if (same(*owner.variants[k])) pick = owner.variants[k];
+    if (!pick && ReplayVariants() > 0 && !owner.relocs.empty())
+      made = (pick = MakeVariant(owner, std::vector<vta_phy_addr_t>(a, a + nbufs))) != nullptr;   // copy + relocate once
+    if (!pick) { ... rewrite and flush the owner as before ... }
```

`ReplayBegin`의 찾기도 바뀌었다. 그 전에는 매 호출 key를 `std::string`으로 만들어 hash map에서 찾았다. 지금은 지난번 이 replay 다음에 온 replay(`last_replay_->next`)와 먼저 `memcmp` 한 번으로 비교한다.

### 2.6 thread별 buffer pool (`api.cc:28`)

**왜.** pool이 크기별 목록 하나였다. 4개 동시에서는 copy들이 매 추론 다른 순서로 서로의 buffer를 가져갔다.
그러면 replay된 호출마다 주소가 새로워 2.5의 사본도 소용이 없었다.

**어떻게.** 열쇠를 (thread id, 크기)로 바꿨다. 나중에 해제한 것을 먼저 꺼낸다. 자기 목록이 비면 다른 thread의 같은 크기를 가져온다(첫 추론). `EVTA_BUF_POOL_SHARED=1`이면 예전 방식이다.

**전 / 후** (`1cdf422d`, `api.cc`):

```diff
-static std::multimap<size_t, vta::DataBuffer*> buf_pool;     // free buffers by size
+static std::map<std::pair<std::thread::id, size_t>, std::vector<vta::DataBuffer*>> buf_pool;  // free buffers
 ...
 void* VTABufferAlloc(size_t size) {
-    auto it = buf_pool.find(size);                 // any thread's buffer of this size
-    if (it != buf_pool.end()) {
-      vta::DataBuffer* b = it->second;
-      buf_pool.erase(it);
+    auto it = buf_pool.find({BufPoolOwner(), size});   // this thread's first
+    if (it == buf_pool.end() || it->second.empty())
+      for (auto j = buf_pool.begin(); j != buf_pool.end(); ++j)   // else another thread's (first inference)
+        if (j->first.second == size && !j->second.empty()) { it = j; break; }
+    if (it != buf_pool.end() && !it->second.empty()) {
+      vta::DataBuffer* b = it->second.back();      // last freed first out
+      it->second.pop_back();
 ...
 void VTABufferFree(void* buffer) {
-      buf_pool.emplace(it->second, b);
+      buf_pool[{BufPoolOwner(), it->second}].push_back(b);
```

**리뷰.** 상한(`EVTA_BUF_POOL_MB`)은 전체 하나다. 한 thread가 많이 쥐고 있으면 다른 thread는 새로 할당한다. 지금 쓰임(같은 bundle 4 copy)에서는 대칭이라 문제가 없다.

### 2.7 1 × 1 weight 한 LOAD (`VTABundle.cpp:1397`)

- `nFilterinLoop × cBlocks`가 thread의 wgt 몫을 넘는 1 × 1 conv에서는 filter 묶음 수를 줄여 몫에 맞춘다.
- 그러면 DMA 묶기(`wgtAll`)가 thread와 group마다 LOAD 한 번으로 한다. 묶음을 줄이지 않으면 입력 묶음마다 256 byte LOAD를 nFilterinLoop번 해야 한다.
- MobileNetV2 7 × 7 960 → 320은 10 × 60 = 600 > 512 entry였다. 5 묶음 × 2 group이 되고, 입력은 두 번 LOAD한다.
- 조건: `EVTA_WGT_FIT`(기본 1), `evtaConvBurst()`, depthwise와 pool이 아님, 1 × 1, `cBlocks > 1`
- **전 / 후** (`38e3da66`, `conv_kernel`): 묶음 수를 줄이는 블록이 새로 들어갔고, `wgtAll`의 조건이 넓어졌다.

  ```diff
  +  if (wgtFitEnv && evtaConvBurst() && !dw && !pool && KH == 1 && KW == 1 && cBlocks > 1 &&
  +      nFilterinLoop * cBlocks > WGT_MEM_ENTRY / nVirtualThread) {
  +    uint32_t n = min(nFilterinLoop, WGT_MEM_ENTRY / nVirtualThread / cBlocks);
  +    for (; n > 0; n--)
  +      if ((KN / 16) % (n * nVirtualThread) == 0) break;
  +    if (n > 0)
  +      nFilterinLoop = n;
  +  }
   ...
  -    const bool wgtAll = evtaConvBurst() && wgtAllFits && !dw && (!(res && res->add) || resUp) && !pool;
  +    const bool resLowOk = !(res && res->add) || resUp || (wgtFitEnv && lastWgt + nFilterinLoop * cBlocks <= RES_WGT_ENTRY);
  +    const bool wgtAll = evtaConvBurst() && wgtAllFits && !dw && resLowOk && !pool;
  ```

  `wgtAll`이 거짓이면 weight를 입력 묶음마다 `nFilterinLoop`행 × 256 byte의 2D LOAD로 읽는다(아래 `else` 쪽, 바뀌지 않음).

### 2.8 int8 입력 `.i8` (`VTABundle.cpp:4024` `VTASetInputInt8`, `VTAInputInt8.h`)

**흐름.**
1. main이 첫 그림을 float로 한 번 돌린다.
2. bundle의 quantize(scale, offset, `[C][256]` 표, 표의 FNV hash)를 `VTAInputInt8`로 얻는다.
3. 그것으로 만든 int8을 `<그림>.i8`로 저장한다(`INPUT_INT8_SAVE`).
4. 다음부터는 `VTASetInputInt8`로 넘기면 conv0 묶기가 quantize 없이 바이트만 옮긴다.

**맞는지.** quantize 기술자(N, H, W, C, nchw, scale, offset, hash)가 다르면 stream이 멈추고 메시지를 낸다(`:4107`). 그래서 다른 bundle의 `.i8`을 잘못 읽지 않는다.

**전 / 후** (`d4d8ddf2`). bundle은 그대로(`preprocess_conv0pack_stream((const float *)input, ...)`)이고, vtalib가 그 `input`을 어떻게 읽는지만 바뀐다.

```diff
 static void inputStreamTo(InputStream *s, int rows) {      // packs the next rows while the conv runs
-  preprocess_conv0pack_rows(s->input, ..., s->scale, s->offset, s->lut, s->KH, s->KW, s->stride, s->pad, s->CO,
-                            s->done, rows, s->padEnd);       // float -> quantize -> table -> pack
+  if (s->i8)                                                 // VTASetInputInt8: the bytes are already int8
+    preprocess_conv0pack_rows_i8((const int8_t *)s->input, ..., s->KH, s->KW, s->stride, s->pad, s->CO,
+                                 s->done, rows, s->padEnd);  // pack only
+  else
+    preprocess_conv0pack_rows(s->input, ..., s->scale, s->offset, s->lut, ...);   // as before
```

`preprocess_conv0pack_stream`은 첫 호출의 quantize 기술자를 기록한다(`VTAInputInt8Info`, `VTAInputQuantize`가 그것을 쓴다).

### 2.9 다음 그림 quantize 겹치기 (`INPUT_PIPELINE`, main `multi_threads_main.cpp:382` `QuantPipe`, `:440` `WaitQuant`, `pynq_driver.cc:459`, `VTABundle.cpp:4012`)

**두 방식.**
- `=1`(`QuantPipe`): copy마다 둘째 thread가 이중 buffer의 다른 쪽에 다음 그림을 quantize한다(`request(k)`, `wait(k)`).
- `=2`(`WaitQuant`): thread를 따로 두지 않는다. copy 자신이 EVTA run을 기다리는 polling마다 한 행씩 quantize한다.
  - 드라이버에 thread-local hook을 넣었다.
  - run 뒤에 남은 행은 `finish()`가 마저 한다.

**전 / 후.** 드라이버(`6c862f40`, `pynq_driver.cc` `RunWait`의 polling loop):

```diff
+static thread_local int (*tWaitWork)(void *) = nullptr;   // VTASetWaitWork
+static thread_local void *tWaitArg = nullptr;
 ...
       }
-      std::this_thread::yield();
+      if (tWaitWork && !tWaitWork(tWaitArg))   // one piece of this thread's work per poll
+        tWaitWork = nullptr;
+      else
+        std::this_thread::yield();
     }
```

main(`24ac3ef1`, `6c862f40`, `multi_threads_main.cpp:500` 근처): 그 전에는 추론마다 bundle의 conv0 묶기가 float 그림을 quantize했다. 그 시간이 추론 시간 안에 있었다.

```cpp
// before: every run
mobilenetv2(constantWeight, mutableWeight /* float image */, activations);   // conv0 pack quantizes it
// after, INPUT_PIPELINE=1: run k uses buffer k & 1, the second thread quantizes image k + 1 meanwhile
VTASetInputInt8(&pd, pipe->wait(run), 1);
pipe->request(run + 1);
mobilenetv2(...);
// after, INPUT_PIPELINE=2: wq->finish() (rows left), VTASetInputInt8(&pd, wq->buf[run & 1].data(), 1), wq->start(run + 1);
// no thread: the copy quantizes one row per poll of its EVTA waits
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

**전 / 후** (`818a73c3`, `VTAGemInsn`의 끝과 uop):

```diff
   uint64_t lut_en         : 1;
-  uint64_t pad_0          : 6;
+  uint64_t dw_win         : 1;   // win = {win1, win2, inp[src]}; an emit step adds sum_k w[wgt][j][k] * win_k[j] to acc[dst][j]
+  uint64_t pad_0          : 5;
   uint64_t pad_1          : 64;
 } VTAGemInsn;
 ...
+#define VTA_UOP_DW_EMIT 63    // bit 63 of a 64-bit uop of a dw_win GEMM: this step writes acc[dst]
```

pad bit를 썼으므로 그 전 vtalib가 만든 명령(`dw_win = 0`, bit 63 = 0)은 `_dw` bitstream에서 예전과 같이 돈다.

### 3.2 HLS compute (`vta.cc:435`)

- compute 모듈이 uop을 `gemm_sa`로 보낼 때 bit 두 개(`dw_win`, emit)를 더 싣는다.
- C 시뮬레이션용 `gemm_sa_csim`에 창 register `win[3][16]`과 창 모드 계산을 넣었다(`#ifdef VTA_DWU`).

### 3.3 `sa_ctrl.v` (`:12`, `:82`, `:93`)

- uop의 `{emit, dw}`를 `DWU`일 때만 읽는다(`DWU = 0`이면 0).
- dw step은 입력 행과 함께 `sa_dw`로 간다.
- emit step만 tag가 valid다(acc 쓰기). tag에 dw를 실어, 출력 행을 `sa_dw`에서 받게 한다.

**전 / 후** (`818a73c3`): uop 폭이 2 bit, FIFO entry가 2 bit, tag가 1 bit 늘었다.

```diff
-  input  wire [DW+SW+WW+29:0] s_uop_tdata,   // {rq, bias, g2s, g2l, first, last, reset, wgt, src, dst}
+  input  wire [DW+SW+WW+31:0] s_uop_tdata,   // {emit, dw, rq, bias, g2s, g2l, first, last, reset, wgt, src, dst}
 ...
-  localparam UW = UB0 + 24;                  // FIFO entry: {rq, bias, g2s, g2l, last, reset, wgt, src, dst}
+  localparam UW = UB0 + 26;                  // {emit, dw, rq, bias, g2s, g2l, last, reset, wgt, src, dst}
 ...
+  wire [1:0]    in_dwe   = DWU ? s_uop_tdata[DW+SW+WW+31:DW+SW+WW+30] : 2'b00;   // {emit, dw}
 ...
-  wire [23:0]   h_rq     = head[UW-1:UB0];
+  wire [23:0]   h_rq     = head[UB0+23:UB0];
+  wire          h_dw     = head[UB0+24];
+  wire          h_emit   = head[UB0+25];
+  wire          h_wr     = !h_dw || h_emit;   // the step writes acc
 ...
-        fifo[wp[FD-1:0]] <= {in_newrun, in_rq, in_bias, in_tok, s_uop_tdata[DW+SW+WW+1:0]};
+        fifo[wp[FD-1:0]] <= {in_newrun, in_dwe, in_rq, in_bias, in_tok, s_uop_tdata[DW+SW+WW+1:0]};
 ...
-  reg [L_RD:0] flag_p, go_p;
+  reg [L_RD:0] flag_p, go_p, dv_p;
+      dv_p   <= {dv_p[L_RD-1:0], issue && h_dw};     // arr_dv: a dw step, with the inp data
 ...
-  reg [DW+SW+29:0] tagp [0:TD];                // {rq, src, bias, tok[1:0], valid, last, reset, dst}
+  reg [DW+SW+30:0] tagp [0:TD];                // {dw, rq, src, bias, tok[1:0], valid, last, reset, dst}; valid = h_wr
```

### 3.4 `sa_dw.v` (78줄)

**전 / 후.** 새 파일이다. 그 전에는 depthwise도 `sa_array`가 대각 weight로 계산했다(§2.2의 대각 모드). `sa_array` 자신은 바뀌지 않았다.

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

**전 / 후** (`818a73c3`):

```diff
-  wire [PW*N-1:0] row;
+  wire [PW*N-1:0] row, arow, drow;
+  wire dv, tdw;
 ...
-    .arr_flag(flag), .arr_go(go),
+    .arr_flag(flag), .arr_go(go), .arr_dv(dv), .tag_dw(tdw),
 ...
   sa_array #(.N(N), .PW(PW)) array (
-    .clk(clk), .rst(rst), .x_in(inp_data), .f_in(flag), .go(go), .wd(wgt_data), .row_out(row));
+    .clk(clk), .rst(rst), .x_in(inp_data), .f_in(flag), .go(go), .wd(wgt_data), .row_out(arow));
+  generate
+    if (DWU) begin : g_dw
+      sa_dw #(.N(N), .PW(PW)) dwu (
+        .clk(clk), .rst(rst), .x_in(inp_data), .f_in(flag), .dv(dv), .go(go), .wd(wgt_data), .row_out(drow));
+      assign row = tdw ? drow : arow;
+    end else begin : g_nodw
+      assign drow = 0;
+      assign row = arow;
+    end
+  endgenerate
```

`sa_rmw`(acc 읽기-고치기-쓰기)의 연결은 그대로 `row`다.

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

**전 / 후** (`398f358c`, `vta.cc` `alu()`의 immediate loop):

```diff
+#if defined(VTA_ALU_MUL) && !defined(VTA_ALUX)
+    const bool imm_mul = insn.alu_op_ext == 1 && insn.alu_opcode == 0 && insn.alu_op_ext2 == 0;
+    const ap_int<16> imm_m = (ap_int<16>) ((acc_T) insn.imm).range(15, 0);
+#endif
 ...
             for (int b = 0; b < VTA_BLOCK_OUT; b++) {
+#if defined(VTA_ALU_MUL) && !defined(VTA_ALUX)
+              const acc_T x = dst_tensor[i][b];
+              const acc_T p = (acc_T) (x * imm_m);
+              alu_lane(insn.alu_opcode, x, (acc_T) insn.imm, dst_tensor[i][b], o_tensor[i][b]);
+              if (imm_mul) {
+                dst_tensor[i][b] = p;
+                o_tensor[i][b] = (out_T) p.range(VTA_OUT_WIDTH - 1, 0);
+              }
+#else
               alu_lane(insn.alu_opcode, dst_tensor[i][b], (acc_T) insn.imm, dst_tensor[i][b], o_tensor[i][b]);
+#endif
```

`ALUMUL=0`(`_amul` 아닌 multi4)이면 `#else` 쪽, 곧 예전 회로다. single의 `_alux*`에서는 확장 ALU가 같은 MUL을 이미 한다.

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
