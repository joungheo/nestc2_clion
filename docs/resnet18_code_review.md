# Resnet18 시간 줄이기 코드 리뷰: 바꾸기 전과 후

2026-11-27. [resnet18_optimization.md](resnet18_optimization.md)의 Resnet18 1장 148.7 → 23.27 ms(2026-10-29 ~ 11-10, single, 1코어)를 만든 코드를 기능마다 설명한다.
같은 방식의 MobileNetV2 문서는 [mobilenetv2_code_review.md](mobilenetv2_code_review.md)다.

기능마다 다음을 적는다.
- **왜:** 무엇이 느렸나
- **전 / 후:** 바꾸기 전 코드와 바꾼 코드
- **어떻게 맞는지:** 무엇으로 확인했나
- **리뷰:** 위험과 한계

**읽는 법.**
- **소스의 전 / 후**는 `diff` 블록이다. `-` 줄이 그 기능의 첫 commit 직전(`git show <commit>^:<파일>`), `+` 줄이 그 commit이다.
  긴 함수는 바뀐 줄 둘레만 남겼다. 새 함수뿐인 commit은 "그 전에는 무엇이 그 일을 했나"를 함께 보인다.
- **만들어진 bundle의 전 / 후**는 2026-11-27 보드에서 지금 model-compiler로 다시 만든 Resnet18 bundle에서 뽑았다.
  - `c0`: 이 문서의 컴파일러 옵션을 모두 끔
  - `all`: 다 켬(기본값)
  - `no_<옵션>`: 그 옵션만 끔
  - 만든 곳: 보드 `~/JO/ubun_wt/r18rev/<tag>/mxnet_exported_resnet18.cpp`(git 밖, `gen.sh`). 같은 profile(`mxnet_exported_resnet18_calib_n200_b2048.yaml`)이다.
  - 이름은 줄였다. `resnetv10_`과 `_fwd__2`를 뺐다(`resnetv10_stage1_conv1_fwd__2` → `stage1_conv1`).
- **효과 숫자**는 [resnet18_optimization.md](resnet18_optimization.md) §0의 1장 시간이다(single, OMP 1, "a → b"는 같은 날 번갈아 잰 값).
- **코드 위치**는 `파일:줄`(2026-11-27 `main`)이다. 우리가 고친 곳에는 `//#### JO added (<날짜>)` 표식이 있고, 그 주석이 설계 설명이다.
- **11-10 뒤에 바뀐 것:** 몇 기능은 그 뒤 다른 모델 일에서 모양이 바뀌었다(예: residual 합치기는 11-14부터 epilogue 프로그램). 그런 곳은 그 절에 적었다.

## 0. 한눈에

| § | 기능 | 층 | 주 파일 | 스위치 | 1장 시간 | 첫 commit |
|---|---|---|---|---|---|---|
| 1.1 | 첫 conv를 16채널로 0 채워 EVTA로, 그 schedule | 컴파일러 | `VTASave.cpp`, `VTASchedules.h` | (항상) | OMP 4 131.8 → 73.6 | `7d152631`, `fa996be8` 10-29 |
| 1.2 | 층 사이 EVTA layout 유지 | 컴파일러 | `VTASave.cpp` `findPackedActivations` | `-vta-keep-layout` | 148.7 → 91.1 | `53c4c68c` 10-31 |
| 1.3 / 2.6 | conv + add + relu를 EVTA 호출 하나로 | 컴파일러 + vtalib | `findResidualFusions`, `ConvResidual` | `-vta-fuse-residual` | 91.1 → 67.1 | `9a8257ad` 10-31 |
| 1.4 / 2.7 | maxpool을 EVTA로 | 컴파일러 + vtalib | `findEvtaMaxPools`, `maxpool_evta` | `-vta-evta-maxpool` | 67.1 → 52.5 | `d9837eac` 10-31 |
| 2.1 | NHWC ↔ EVTA layout transpose를 16 byte 복사로 | vtalib | `CPUBundle_generic.cpp` | (항상) | 52.5 → 46.9 | `52c9a514`, `90effa36`, `fe3ceff2` |
| 2.2 | VTA buffer pool | vtalib | `api.cc` | `EVTA_BUF_POOL_MB` | 46.9 → 44.3 | `0b2a0bcd` 11-01 |
| 2.3 | CPU fc NEON | vtalib | `cpuFullyConnected` | (항상) | 44.3 → 40.6 | `cb4a5446` 11-01 |
| 2.4 | cache 정리 뒤 `dsb sy` | vtalib 드라이버 | `pynq_driver.cc` | `EVTA_NO_DSB` | multi4 멈춤 없앰 | `7c6fb2c6` 11-02 |
| 1.5 / 2.8 | fc를 EVTA로 | 컴파일러 + vtalib | `saveEvtaFcPacked`, `fullyconnected_evta_packed` | `-vta-evta-fc` | 40.6 → 39.9 | `8090d38d`, `b30f7f1c` 11-02 |
| 1.6 | 입력 전처리를 첫 conv 입력 복사에 | 컴파일러 + vtalib | `findInputPreprocess`, `preprocess_nhwc2vtaio_zerofill` | `-vta-fuse-preprocess` | 39.9 → 37.1 | `8b83104d`, `7bc3b5ba` |
| 2.5 | EVTA 명령 record / replay | vtalib | `cmd_queue.h` `ReplayBegin` | `EVTA_REPLAY` | 37.1 → 34.5 | `e3f41c84` 11-02 |
| 1.7 | 첫 conv를 묶은 입력의 4 × 1 conv로 | 컴파일러 + vtalib | `getConv0Pack`, `conv0pack_nhwc2vtaio` | `-vta-conv0-pack` | 34.5 → 29.2 | `a7769c61` 11-02 |
| 1.8 | 첫 conv + maxpool을 호출 하나로 | 컴파일러 + vtalib | `findPoolFusions`, `ConvPool` | `-vta-fuse-pool` | 29.2 → 28.83 | `2b4a19f5` 11-02 |
| 3.1 | ALU immediate 연산 II 1 | **HW** (HLS) | `vta.cc` `alu()` | bitstream `_alu1_epr` | 28.98 → 27.32 | `b1755839` 11-02 |
| 3.2 | RMW reset이 bias를 씀 | **HW** (RTL) + vtalib | `sa_rmw.v`, `sa_ctrl.v`, `evtaBiasReset` | bitstream `bias`, `EVTA_BIAS_RESET` | 27.30 → 26.59 | `76aa60e0` 11-04 |
| 3.3 | out 쓰기에 requantize | **HW** (ISA, RTL, HLS) + vtalib | `sa_rmw.v` S4–S6, `VTARequant` | bitstream `_rq`, `EVTA_REQUANT` | 26.61 → 25.49 | `d26c3e30` 11-04 |
| 2.9 | systolic 최소 tile 1, 더미 tile 결함 | vtalib | `evtaMinTile`, `conv_kernel` | `EVTA_MIN_TILE` | 25.53 → 25.42 | `e40d5cca`, `327ddd3a` 11-05 |
| 2.10 | 첫 conv 입력 복사를 conv와 겹침 | vtalib + 드라이버 + 컴파일러 | `InputStream`, `RunStart` / `RunWait` | `EVTA_INPUT_STREAM` | 25.43 → 24.08 | `5eee77e1` 11-06 |
| 2.11 | conv DMA 묶기 | vtalib | `evtaConvBurst`, `loadBuffer2D` | `EVTA_CONV_BURST` | 24.08 → 23.27 | `d8de5781` 11-10 |

### 0.1 만들어진 bundle 전체: 모두 끔(`c0`) → 다 켬(`all`)

`mxnet_exported_resnet18()` 본문의 호출 수다.

| 호출 | `c0` | `all` | 무엇이 바꿨나 |
|---|---|---|---|
| `transpose_nhwc2vtaio` / `transpose_vtaio2nhwc` | 19 / 20 | 0 / 0 | §1.2 |
| `VTABufferFlush` / `VTABufferInvalidate` | 20 / 20 | 0 / 1 | §1.2 (그리고 11-25 `-vta-skip-evta-invalidate`) |
| `elemadd` / `relu` (CPU) | 8 / 8 | 0 / 0 | §1.3 (`VTASetConvEpilogue` 16줄 = 켜기 8, 끄기 8) |
| `maxpool` (CPU) | 1 | 0 | §1.4, §1.8 (`convolution_wo_tr_pool`) |
| `fullyconnected` (CPU) | 1 | 0 | §1.5 (`fullyconnected_evta_packed`) |
| `quantize` + `elemsub` + `elemdiv` + `transpose_nhwc2vtaio_zerofill` | 1 + 1 + 1 + 1 | 0 | §1.6, §1.7 (`preprocess_conv0pack_stream` 1개) |
| `convolution_wo_tr` | 20 | 19 + `convolution_wo_tr_pool` 1 | §1.8 |
| `malloc` / `free` (CPU 활성값) | 28 | 5 | 활성값이 VTA buffer에 남음(§1.2) |

`VTA_FUSION_LOG` 요약(bundle 머리 주석):

```diff
-//   20 EVTA producers, 0 with a tail, 12 read by a tail op not fused
-//     not fused: 11 x an add: -vta-fuse-elemadd-mul is off (elemadd_evta is faster)
-//     not fused: 1 x a CPU pool
-//   1 GEMMs (conv / fc / matmul) on the CPU
-//     on the CPU: 1 x fc: -vta-evta-fc is off
+//   20 EVTA producers, 9 with a tail, 0 read by a tail op not fused
+//   0 GEMMs (conv / fc / matmul) on the CPU
```

모델 첫머리(입력 → 전처리 → 첫 conv → maxpool → stage1 conv0)의 코드다.

```cpp
// c0: 모두 끔
quantize(data, q, 150528, 1/0.25, 0);                                   // float -> int8
elemsub(q, ..., mean, ..., sub, ..., 150528);                            // - mean (per channel)
elemdiv(sub, ..., scale, ..., div, ..., 150528);                         // / std (per channel)
conv0_in = VTABufferAlloc(802816);
transpose_nhwc2vtaio_zerofill(div, conv0_in, 1, 224, 224, 16);          // 3 channels -> 16, 13 of them 0
VTABufferFlush(conv0_in, 0, 802816);
conv0_out = VTABufferAlloc(802816);
convolution_wo_tr(conv0_in, ..., conv0_out, 1, 224, 224, 16, 64, 7, 7, 3, 2, ...);   // 7 x 7 / 2 over 16 channels
VTABufferInvalidate(conv0_out, 0, 802816);
transpose_vtaio2nhwc(conv0_out, conv0_res, 1, 112, 112, 64);            // back to NHWC for the CPU pool
maxpool(conv0_res, pool0_res, 1, 112, 112, 64, 3, 3, 1, 2);             // CPU
s1c0_in = VTABufferAlloc(200704);
transpose_nhwc2vtaio(pool0_res, s1c0_in, 1, 56, 56, 64);                // and to the EVTA layout again
VTABufferFlush(s1c0_in, 0, 200704);
convolution_wo_tr(s1c0_in, ..., s1c0_out, 1, 56, 56, 64, 64, 3, 3, 1, 1, ...);
VTABufferInvalidate(s1c0_out, ...);  transpose_vtaio2nhwc(s1c0_out, ...);

// all: 다 켬
conv0_in = VTABufferAlloc(618240);
preprocess_conv0pack_stream((const float *)data, conv0_in, 1, 224, 224, 3, 0, 1/0.25, 0, conv0_preprocess_lut,
                            7, 7, 2, 3, 48);                            // quantize + mean / std table + pack, while the conv runs
convolution_wo_tr_pool(conv0_in, ..., conv0_vbuf, 1, 115, 112, 48, 64, 4, 1, 0, 1, ...,
                       pool0_vbuf, 3, 2, 1);                            // 4 x 1 over 48 channels, the 3 x 3 / 2 pool in the same call
convolution_wo_tr(pool0_vbuf, ..., s1c0_vbuf, 1, 56, 56, 64, 64, 3, 3, 1, 1, ...);   // no transpose, flush, invalidate
```

## 1. 컴파일러 (`lib/Backends/VTA/VTASave.cpp`)

### 1.1 첫 conv를 EVTA로 (10-29, `7d152631`, `fa996be8`)

**왜.** 첫 conv(7 × 7 / 2, 3 → 64채널)는 입력이 3채널이라 EVTA conv 조건(16의 배수)에 맞지 않았다. 그래서 CPU에서 float로 돌았다(OMP 4 131.8 ms 가운데 큰 몫).

**전 / 후.** 이미 있던 BNN용 "첫 conv를 16채널로 0 채우기" 경로(`saveFirstBNNVTAConvolutionInst`)를 int8 conv에도 쓰게 했다.

```diff
     } else if (Inst->getGroup() == srcDims[3] && ...) {
       saveNonVTAConvolutionInst(Inst, bundle, initConst, ctx);
+    //#### JO added (2026-10-29): a quantized 3-channel conv (the image conv0 ...) runs on EVTA with its input and filter
+    //#### zero-filled to 16 channels, the path of the first BNN conv; it is plain int8 (convolution_wo_tr)
+    } else if (Inst->getGroup() == 1 && scale > 1 && srcDims[3] == 3 &&
+               filterDims[3] == 3 && filterDims[0] % 16 == 0 &&
+               biasScale == matMulScale && outOffset == 0 && inOffset == 0 &&
+               filterOffset == 0 && biasOffset == 0 && shift >= 0) {
+      saveFirstBNNVTAConvolutionInst(Inst, bundle, initConst, ctx);
     } else {
```

그 conv의 schedule을 `include/Backends/VTA/VTASchedules.h`에 넣었다. 기본값(virtual thread 1, tile 8 × 8)이 12.81 ms였고, 2 / 8 × 16이 9.48 ms였다.

```diff
+convTune.ConvolutionTune_.push_back({1, 224, 224, 16, 64, 7, 7, 16, 2, 3, 2, 8,
+                                     16});
```

**리뷰.**
- 16채널 가운데 13채널이 0이라 GEMM의 13/16이 헛일이다. §1.7의 묶기가 이것을 고친다.
- 주석대로 tile이 112를 나누지 않으면(16 × 11) 결과가 달랐다. 그래서 112를 나누는 tile만 쓴다. 원인은 그때 찾지 않았다.

### 1.2 층 사이 EVTA layout 유지 (10-31, `53c4c68c`, `-vta-keep-layout`, `VTASave.cpp:4232`)

**왜.** EVTA conv는 입력과 출력을 EVTA layout(N{C/16}HW16)으로 읽고 쓴다. 그런데 bundle은 conv마다 NHWC로 되돌리고 다음 conv에서 다시 바꿨다. conv 20개마다 transpose 두 번, flush, invalidate, VTA buffer 할당 두 번이었다(§0.1 `c0`). OMP 1 148.7 ms의 대부분이 이것이었다.

**어떻게.** 활성값마다 "모든 사용자가 EVTA layout을 그대로 받을 수 있나"를 고정점 반복으로 정한다.
- 후보: 4차원, N = 1, int8, C % 16 = 0, H × W > 1인 활성값
- EVTA conv는 그대로 받는다.
- elementadd, relu는 원소별이라 그대로 돈다. maxpool, avgpool은 16채널 묶음을 batch로 보면 그대로 돈다.
  그러나 이 셋은 한 명령의 4차원 operand가 모두 같은 layout이어야 한다(함께 packed이거나 함께 아니거나).
- 그 밖의 사용자(그래프 입출력, view, 다른 연산)가 하나라도 있으면 NHWC로 둔다.

```cpp
// VTASave.cpp findPackedActivations (10-31)
for (const auto &I : IR->getInstrs())
  for (const auto &op : I.getOperands())
    if (isPackCandidate(op.first)) packed.insert(op.first);
while (changed) {
  changed = false;
  for (const auto &I : IR->getInstrs()) {
    switch (I.getKind()) {
    case Kinded::Kind::ConvolutionInstKind:
      if (getConvPath(C) == ConvPath::VTA) continue;                 // reads / writes the EVTA layout
      if (path == ConvPath::FirstBNN) { unpack(every operand but the dest); continue; }   // reads NHWC, writes packed
      break;
    case ElementAdd: case Relu: case MaxPool: case AvgPool: group = true; break;
    }
    if (!group) { unpack(every operand); continue; }
    if (!all operands packed or 1 x 1) unpack(every operand);          // all or none
  }
}
```

**전 / 후.** 입력 transpose, 출력 transpose, buffer 해제를 내는 함수 셋이 packed 활성값이면 일찍 돌아가게 했다.

```diff
 void generateInputTranspose(...) {
+  if (ctx->isPacked(src)) {      // the input is already in the EVTA layout (flushed by the CPU op that wrote it)
+    bundle->append("  int8_t* " + name + "_input_transpose = " + src + "_vbuf;\n");
+    return;
+  }
   ... VTABufferAlloc, transpose_nhwc2vtaio, VTABufferFlush ...
 void declareOutputTranspose(...) {
+  if (ctx->isPacked(dest)) {     // the EVTA writes straight into the activation kept in its layout
+    bundle->append("  int8_t* " + name + "_output_bef_transpose = " + dest + "_vbuf;\n");
+    return;
+  }
 void generateOutputTranspose(...) {
   ... VTABufferInvalidate(...) ...
+  if (ctx->isPacked(dest))       // an output kept in the EVTA layout is not transposed
+    return;
   bundle->append("  transpose_vtaio2nhwc(");
```

pool 둘(`saveMaxPoolInst`, `saveAvgPoolInst`)은 packed 입력이면 N을 `N × C / 16`, C를 16으로 바꿔 부른다. 같은 CPU 함수가 묶음을 batch로 돈다.

만들어진 bundle(`no_keep-layout` → `all`): transpose 39 → 0, flush 19 → 0, invalidate 20 → 1. 모양은 §0.1의 `c0` 아래 넷째 줄 이후와 같다.

**리뷰.**
- packed 활성값은 VTA buffer(`<name>_vbuf`)이고, `<name>`은 그 가상 주소다. CPU 연산이 packed 활성값을 쓰면 뒤에서 flush해야 한다. `VTA::save`가 elementadd, relu, pool 뒤에 `VTABufferFlush`를 낸다(10-31 같은 commit).
  새 CPU 연산을 packed로 받게 할 때 그 목록에 넣지 않으면 EVTA가 옛 값을 읽는다. 지금은 그 목록이 `VTA::save` 안의 kind 나열이다.

### 1.3 conv + elementadd + relu를 EVTA 호출 하나로 (10-31, `9a8257ad`, `-vta-fuse-residual`, `VTASave.cpp:4336`)

**왜.** residual block의 끝(conv → add → relu)에서 add와 relu가 CPU였다(8 block). §1.2 뒤에는 이것이 CPU 시간의 큰 몫이었다.

**어떻게.** `findResidualFusions`가 IR에서 다음 모양을 찾는다(사이에는 dealloc만).

```text
EVTA conv C -> D;  elementadd E(D, R) -> O;  relu(O) -> O'
```

- 조건: D, R, O, O'가 모두 packed(§1.2), 배율 비가 2의 거듭제곱(sD / sO = 2^-a, sR / sO = 2^-b, offset 0), relu의 입출력 scale이 같음, O'가 conv 입력이 아님, D와 O의 다른 사용자 없음
- m = max(a, b, 0)이면 O' = relu(round((D << (m − a)) + (R << (m − b))) >> m)다. CPU의 elemadd + relu와 같다.
  - EVTA는 .5를 위로, CPU는 0에서 멀리 반올림한다. 둘은 음수 합에서만 다르고, relu가 음수를 0으로 만드므로 결과는 같다.
- C 자리에서 `convolution_wo_tr_res` 하나를 내고, E와 relu는 아무것도 내지 않는다.

**전 / 후.** 만들어진 bundle(`no_fuse-residual` → `all`, stage1 첫 block):

```diff
-  convolution_wo_tr(s1c0_vbuf, ..., s1c1_vbuf, 1, 56, 56, 64, 64, 3, 3, 1, 1, 0, 1, 7, 56, 56, ...);
-  VTABufferInvalidate(s1c1_vbuf, 0, 200704);                                          // for the CPU add
-  elemadd(pool0_res, 1.0/16, 0, s1c1_res, 1.0/8, 0, s1c1_res, 1.0/8, 0, 200704);       // CPU, both packed: elementwise
-  VTABufferFlush(s1c1_vbuf, 0, 200704);
-  s1_act0_vbuf = VTABufferAlloc(200704);
-  relu(s1c1_res, s1_act0_res, 200704, 0.125, 0.0625);                                 // CPU, the scale halves (reluShl 1)
-  VTABufferFlush(s1_act0_vbuf, 0, 200704);                                            // for the next EVTA conv
+  VTAEpilogue s1c1_epi = {5, {{1, 0, 1, 0, 0}, {2, 2, 1, -128, 127}, {3, 0, 127, 0, 0}, {4, 1, 0, 0, 0}, {3, -128, 127, 0, 0}}};
+  s1c1_epi.t[0] = pool0_vbuf;                                                           // the residual
+  VTASetConvEpilogue(&s1c1_epi);
+  convolution_wo_tr(s1c0_vbuf, ..., s1_act0_vbuf, 1, 56, 56, 64, 64, 3, 3, 1, 1, 0, 1, 7, 56, 56, ...);
+  VTASetConvEpilogue(nullptr);
```

10-31의 bundle은 `convolution_wo_tr_res(..., res, resShl, convShl, sumShr, relu)` 한 줄이었다. 11-14부터는 위처럼 epilogue 프로그램(ADD, RQ, CLIP, SHL, CLIP)이다([evta_alux_epilogue.md](evta_alux_epilogue.md)). vtalib가 그것을 같은 계산으로 내린다(§2.6).

**리뷰.**
- 2의 거듭제곱 비가 아니거나 `convShl > 8`, `resShl > 6`, `sumShr > 15`면 합치지 않는다. 다른 양자화 방식의 모델에서는 조용히 CPU로 간다. `VTA_FUSION_LOG`가 그 이유를 찍는다.
- 10-31 같은 날 `1427b5b5`에서 relu 출력이 conv 뒤에 할당되는 경우도 받게 했다(할당을 conv 앞으로 옮김, `hoistOut`).

### 1.4 maxpool을 EVTA로 (10-31, `d9837eac`, `-vta-evta-maxpool`, `VTASave.cpp:4772`)

**조건(`findEvtaMaxPools`).** 입력과 출력이 packed, N = 1, 정사각 kernel, pad와 stride가 같음, **입력 ≥ 0**이다.
입력 ≥ 0은 그 pool 바로 전에 입력을 쓴 명령이 relu가 있는 EVTA conv, 합친 conv + add + relu, relu일 때다. EVTA의 LOAD는 pad를 0으로 채우므로, 입력이 ≥ 0이면 0 pad를 넣은 max가 CPU의 "범위 안 max"와 같다.

**전 / 후** (`saveMaxPoolInst`):

```diff
   int pad_size = pad[0];
   int stride_size = strides[0];
+  if (ctx->isEvtaMaxPool(Inst)) {         // findEvtaMaxPools
+    bundle->append("  maxpool_evta(" + src + "_vbuf, " + dest + "_vbuf, N, H, W, C, KH, KW, pad, stride, vtaCmdH, idx);\n");
+    bundle->append("  VTABufferInvalidate(" + dest + "_vbuf, 0, " + size + ");\n");
+    return;
+  }
   if (ctx->isPacked(src)) {               // the CPU pool: 16-channel blocks as the batch
     N = N * C / 16;
```

만들어진 bundle(`no_evta-maxpool` → `no_fuse-pool`, 같은 첫 conv 뒤):

```diff
-  maxpool(conv0_res, pool0_res, 4, 112, 112, 16, 3, 3, 1, 2);            // CPU, 64 channels as 4 blocks of 16
+  maxpool_evta(conv0_vbuf, pool0_vbuf, 1, 112, 112, 64, 3, 3, 1, 2, vtaCmdH, 0);
```

`all`에서는 이 pool이 첫 conv의 호출 안으로 들어간다(§1.8).

### 1.5 fc를 EVTA로 (11-02, `8090d38d`, `b30f7f1c`, `-vta-evta-fc`, `VTASave.cpp:4867`)

**왜.** fc(512 → 1000)는 CPU였다. NEON으로 0.74 ms까지 줄였지만(§2.3) EVTA는 0.15 ms다.

**어떻게.** 처음 판(`8090d38d`)은 CPU `fullyconnected`와 같은 인자로 `fullyconnected_evta`를 불렀다. vtalib가 첫 호출에서 weight를 EVTA layout으로 바꿨다.
같은 날 `b30f7f1c`에서 그 일을 컴파일 때로 옮겼다. weight 파일에 EVTA layout(출력 묶음 × 입력 묶음의 16 × 16, 출력 묶음 우선, 0 채움)과 GEMM 배율로 반올림한 bias를 쓴다.

```diff
-  bundle->append(vtaEvtaFc ? "  fullyconnected_evta(" : "  fullyconnected(");
+  bundle->append("  fullyconnected(");          // the CPU call when saveEvtaFullyConnectedInst did not fit
 ...
 void saveFullyConnectedInst(...) {
+  if (vtaEvtaFc && saveEvtaFullyConnectedInst(Inst, bundle, ctx))   // weights / bias packed into the weights file
+    return;
```

만들어진 bundle(`no_evta-fc` → `all`):

```diff
-  fullyconnected(pool1_res__2, 1.0/8, 0, dense0_weight__1, 1.0/128, 0, dense0_bias, 1.0/1024, 0, dense0_res, 1.0/4, 0,
-                 1, 512, 512, 1000, 1, 1000, 1);      // CPU, NEON (§2.3)
+  fullyconnected_evta_packed(pool1_res__2, dense0_weight__1, dense0_bias, dense0_res, 512, 1000, 8, vtaCmdH, 0);
```

**리뷰.**
- 반올림이 CPU(`std::round`, 0에서 멀리)와 같아야 해서 ALU MIN / MAX / ADD / SHR만으로 부호를 처리한다(§2.8). 그래서 bit-exact다.
- 11-05부터 chunk 크기 제한(출력 묶음 8개 이상)을 뺐다. 그것은 HLS GEMM의 acc hazard 때문이었다(Resnet50 2048 → 1000도 EVTA).

### 1.6 입력 전처리를 첫 conv 입력 복사에 (11-02, `8b83104d`, `-vta-fuse-preprocess`, `VTASave.cpp:4620`)

**왜.** 입력은 float quantize → 채널별 elemsub(평균) → 채널별 elemdiv(표준편차) → 3 → 16채널 0 채우기 복사였다. 전부 CPU이고 150,528 byte를 네 번 지났다(2.4 ms).

**어떻게(`findInputPreprocess`).** 첫 conv 앞의 quantize, 채널마다 같은 상수의 elemsub / elemdiv, NCHW → NHWC transpose(하나까지)를 찾는다.
- elemsub / elemdiv 사슬은 quantize된 값과 채널에만 달렸다. 그래서 컴파일 때 `[C][256]` 표로 만든다.
- 표는 CPU elemsub / elemdiv와 같은 식(bundle이 쓰는 scale 그대로)으로 계산한다. 그래서 bit-exact다.
- 첫 conv의 입력 복사가 quantize + 표 + 0 채우기를 한 번에 한다(`preprocess_nhwc2vtaio_zerofill`, `7bc3b5ba`에서 NEON 16 픽셀씩).

만들어진 bundle(`no_fuse-preprocess` → `all`):

```diff
-  quantize(data, q, 150528, 1/0.25, 0);
-  elemsub(q, 1.0/0.25, 0, mean, 1.0/1, 0, sub, 1.0/0.5, 0, 150528);
-  elemdiv(sub, 1.0/0.5, 0, scale, 1.0/2, 0, div, 1.0/32, 0, 150528);
-  conv0pack_nhwc2vtaio(div, conv0_in, 1, 224, 224, 3, 7, 7, 2, 3, 48);
+  preprocess_conv0pack_stream((const float *)data, conv0_in, 1, 224, 224, 3, 0, 1/0.25, 0,
+                              (const int8_t *)VTABufferGetVirtAddr(conv0_preprocess_lut), 7, 7, 2, 3, 48);
```

**리뷰.**
- `all` bundle에는 지운 세 명령의 활성값 `malloc` / `free`(150,528 byte × 3)가 아무 일 없이 남아 있다. 시간은 작지만 정리할 거리다.

### 1.7 첫 conv를 묶은 입력의 4 × 1 conv로 (11-02, `a7769c61`, `-vta-conv0-pack`, `VTASave.cpp:1675`)

**왜.** §1.1의 0 채우기는 7 × 7 / 2 over 16채널(13채널 0)이다. MAC의 13/16이 헛일이고, stride 2라 입력 tile도 크다(9.13 ms).

**어떻게.** KH × KW / stride s over C채널을 ceil(KH / s) × 1 / stride 1 over s × KW × C채널로 바꾼다.
- 묶은 픽셀 (q, x)의 채널 (dh × KW + j) × C + c = pad한 입력 (s q + dh, s x + j, c)다.
- filter 탭 (k, 0, (dh × KW + j) × C + c) = 원래 (s k + dh, j, c)이고, s k + dh ≥ KH면 0이다. 같은 합이라 출력이 바이트까지 같다.
- Resnet18: 7 × 7 / 2 over 3채널 → 4 × 1 / 1 over 48채널(115 × 112). GEMM 채널 48 가운데 42가 쓰인다.

```cpp
// VTASave.cpp getConv0Pack (11-02)
p->KH2 = (KH + s - 1) / s;            // 4
p->CO = (s * KW * C + 15) / 16 * 16;  // 2 * 7 * 3 = 42 -> 48
p->H2 = outH + p->KH2 - 1;            // 115
p->W2 = outW;                         // 112
getConvSchedule(N, p->H2, p->W2, p->CO, KN, p->KH2, 1, 1, 0, &vt, &th, &tw);
return outH % th == 0 && outW % tw == 0 && (th + p->KH2 - 1) * tw * (p->CO / 16) <= 1024;   // half of the multi-EVTA inp
```

```cpp
// the packed filter: tap (k, 0, (dh * KW + j) * C + c) = (s * k + dh, j, c)
for (int dh = 0; dh < s && s * k + dh < KH; dh++)
  ... out[idx] = handle.at({o, s * k + dh, j, c});
```

만들어진 bundle(`no_conv0-pack` → `no_fuse-pool`):

```diff
-  preprocess_nhwc2vtaio_zerofill((const float *)data, conv0_in, 1, 224, 224, 3, 16, 0, 1/0.25, 0, lut);
-  convolution_wo_tr(conv0_in, conv0_weight__1, ..., 1, 224, 224, 16, 64, 7, 7, 3, 2, 1, 1, 10, 112, 112, vtaCmdH, 2, 8, 16, 0);
+  preprocess_conv0pack_stream((const float *)data, conv0_in, 1, 224, 224, 3, 0, 1/0.25, 0, lut, 7, 7, 2, 3, 48);
+  convolution_wo_tr(conv0_in, conv0_weight__1, ..., 1, 115, 112, 48, 64, 4, 1, 0, 1, 1, 1, 10, 112, 112, vtaCmdH, 2, 16, 16, 0);
```

vtalib의 묶기 복사는 처음에 pad한 그림 전체를 만들었다(`a7769c61`). 같은 날 `a114e2e2`(16채널 묶음마다 `vqtbl4q_u8` 하나), `a60b64b4`(thread마다 pad한 행 s개만, 루프 값을 지역 변수로)로 줄였다.

**리뷰.**
- 조건의 1024는 multi-EVTA의 inp SRAM 절반이다. single에서도 같은 값을 쓴다. buffer 크기가 바뀌면 같이 바꿔야 한다.
- 처음 판은 pad가 네 쪽 모두 같아야 했다. 11-07에 MobileNetV2 때문에 위 / 왼쪽, 아래 / 오른쪽을 따로 받게 했다([mobilenetv2_code_review.md](mobilenetv2_code_review.md) §1.4).

### 1.8 첫 conv + maxpool을 호출 하나로 (11-02, `2b4a19f5`, `-vta-fuse-pool`, `VTASave.cpp:4551`)

**왜.** 첫 conv의 출력(112 × 112 × 64, 0.8 MB)을 DRAM에 쓰고, 그것을 maxpool이 다시 읽었다.

**어떻게.** `findPoolFusions`가 묶은 첫 conv(relu 있음) 바로 다음의 EVTA maxpool 3 × 3 / 2 / 1을 찾는다. conv tile이 짝수이고 conv 출력의 다른 사용자가 없어야 한다.
vtalib `convolution_wo_tr_pool`이 conv tile을 pool 창의 halo(위 한 행, 왼쪽 한 열)까지 다시 계산하고, acc 안에서 ALU MAX로 pool한 뒤 pool 결과만 STORE한다.

만들어진 bundle(`no_fuse-pool` → `all`):

```diff
-  convolution_wo_tr(conv0_in, ..., conv0_vbuf, 1, 115, 112, 48, 64, 4, 1, 0, 1, 1, 1, 10, 112, 112, vtaCmdH, 2, 16, 16, 0);
-  maxpool_evta(conv0_vbuf, pool0_vbuf, 1, 112, 112, 64, 3, 3, 1, 2, vtaCmdH, 0);
+  convolution_wo_tr_pool(conv0_in, ..., conv0_vbuf, 1, 115, 112, 48, 64, 4, 1, 0, 1, 1, 1, 10, 112, 112, vtaCmdH, 2, 16, 16, 0,
+                         pool0_vbuf, 3, 2, 1);          // conv0_vbuf is written only when the shapes do not fit
```

같은 날 `093a9cdf`에서 pool을 두 번(행 방향, 열 방향)에 나눠 tile과 thread마다 ALU 명령을 19개에서 6개로 줄였다.
11-17에는 conv + 2 × 2 pool도 받게 넓혔다([evta_conv_pool_guide.md](evta_conv_pool_guide.md)).

**리뷰.**
- `conv0_vbuf`는 합칠 수 없는 모양일 때만 쓰인다. 그래도 bundle은 그 buffer(0.8 MB)를 할당한다.

## 2. vtalib (`vtalib-zcu102-systolic`)

### 2.1 NHWC ↔ EVTA layout transpose를 16 byte 복사로 (10-31 ~ 11-01, `52c9a514`, `90effa36`, `fe3ceff2`, `CPUBundle_generic.cpp:1800`)

**왜.** §1.2 뒤에도 transpose가 남는 곳(그래프 입출력, CPU 연산 앞뒤)이 있었다. byte 하나씩 옮겨 56 × 56 × 64에 1.75 ms(NHWC → EVTA)와 3.3 ms(반대)였다.
두 layout 모두에서 한 묶음의 16채널은 이어져 있다. 그래서 16 byte씩 복사하면 된다.

**전 / 후** (보드가 빌드하는 `CPUBundle_generic.cpp`):

```diff
   for (int r = 0; r < rows; r++) {
     const int n = r / (CB * H), c_ = (r / H) % CB, h = r % H;
     const int8_t *in = input + n*H*W*C + h*W*C + c_*16;
     int8_t *out = output + n*CB*H*W*16 + c_*H*W*16 + h*W*16;
     for (int w = 0; w < W; w++)
-      for (int c = 0; c < 16; c++)
-        out[w*16 + c] = in[w*C + c];
+      memcpy(out + w*16, in + w*C, 16);
   }
```

```diff
 void transpose_vtaio2nhwc(int8_t* input, int8_t* output, int N, int H, int W, int C){
   for (int r = 0; r < rows; r++) {
     const int b = r / H, i = r % H;
-    for (int j = 0; j < W; j++) {
-      int8_t *out = output + b * H * W * C + i * W * C + j * C;
-      for (int f = 0; f < C; f++)
-        out[f] = *(input + ((f / 16) * H * W * 16 + i * W * 16 + j * 16 + f % 16));
+    for (int c_ = 0; c_ < CB; c_++) {
+      const int nc = std::min(16, C - c_ * 16);      // the last block of a C % 16 != 0 tensor
+      const int8_t *in = input + c_ * H * W * 16 + i * W * 16;
+      int8_t *out = output + b * H * W * C + i * W * C + c_ * 16;
+      for (int j = 0; j < W; j++)
+        memcpy(out + j * C, in + j * 16, nc);
```

첫 conv의 0 채우기 복사(`transpose_nhwc2vtaio_zerofill`, `fe3ceff2`)는 그 전에 memset + malloc한 NCHW + byte 재배치였다(6.8 ms, 4 thread). 행마다 한 번에 쓰도록 바꿨다.

```cpp
// after: one pass, 16 bytes per pixel (the 3 input channels, then zeros)
for (int w = 0; w < W; w++) {
  int8_t px[16] = {0};
  px[0] = in[w * 3]; px[1] = in[w * 3 + 1]; px[2] = in[w * 3 + 2];
  memcpy(out + w * 16, px, 16);
}
```

**리뷰.**
- **`transpose_vtaio2nhwc`의 입력 위치가 batch b에 달리지 않는다.** 처음 판(`52c9a514`, `CPUBundle_aarch64.cpp`)은 이것을 고쳤다. 그런데 `90effa36`에서 "예전과 같은 대응"으로 되돌렸다. N > 1이면 모든 batch가 batch 0의 입력을 읽는다. 지금 bundle은 모두 N = 1이라 드러나지 않는다.
- 같은 함수가 `CPUBundle_aarch64.cpp`와 `CPUBundle_generic.cpp` 두 곳에 있다. 보드는 generic을 빌드한다(`NESTC_EVTA_RUN_WITH_GENERIC_BUNDLE=ON`). 한쪽만 고치기 쉽다.

### 2.2 VTA buffer pool (11-01, `0b2a0bcd`, `api.cc:51`)

**왜.** bundle은 매 추론 같은 크기의 VTA buffer를 같은 순서로 할당하고 해제한다(Resnet18 23개). 그때마다 `cma_alloc` / `cma_free` ioctl이라 1장에 2.6 ms였다.

**전 / 후:**

```diff
 void* VTABufferAlloc(size_t size) {
-  return vta::DataBuffer::Alloc(size);
+  {
+    std::lock_guard<std::mutex> lock(buf_pool_mutex);
+    auto it = buf_pool.find(size);               // a freed buffer of this size
+    if (it != buf_pool.end()) {
+      vta::DataBuffer* b = it->second;
+      buf_pool.erase(it);
+      buf_pool_bytes -= size;
+      return b;
+    }
+  }
+  vta::DataBuffer* b = vta::DataBuffer::Alloc(size);
+  buf_size[b] = size;
+  return b;
 }
 void VTABufferFree(void* buffer) {
-  vta::DataBuffer::Free(vta::DataBuffer::FromHandle(buffer));
+  ... if (buf_pool_bytes + size <= BufPoolLimit()) { buf_pool.emplace(size, b); return; }   // EVTA_BUF_POOL_MB, default 32
+  vta::DataBuffer::Free(b);
 }
```

11-25에는 multi4를 위해 열쇠를 (thread, 크기)로 바꿨다([mobilenetv2_code_review.md](mobilenetv2_code_review.md) §2.6).

**리뷰.**
- **다시 받은 buffer에는 앞 사용자의 값이 남아 있다.** `cma_alloc`도 0으로 지우지 않았으므로 새로 생긴 위험은 아니다. 그러나 "새 buffer는 대개 0"이라는 우연이 없어졌다. 0으로 시작해야 하는 buffer는 스스로 지워야 한다.
- 상한을 넘으면 그 buffer는 바로 해제된다. 상한이 작으면 매 추론 할당이 다시 생긴다(조용히 느려진다).

### 2.3 CPU fc NEON (11-01, `cb4a5446`, `CPUBundle_generic.cpp:540`)

**왜.** fc는 출력 64개마다 kernel 512행을 1000 byte 간격으로 읽었다. conv 뒤라 kernel(512 KB)이 cache에 없고, 64 byte 조각이 prefetch되지 않았다(1.92 ms, OMP 1).

**전 / 후:**

```diff
-#pragma omp for schedule(static)
-            for (ssize_t j0 = 0; j0 < J; j0 += 64) {               // 64 outputs at a time
-                for (dim_t k = 0; k < inputDim1; k++) {             // all 512 kernel rows, 64 bytes of each
-                    const int32_t A = (int32_t)a[k] - inputOffset;
-                    const int8_t *w = kernel + k * kernelDim1 + j0;
-                    for (ssize_t j = 0; j < nj; j++)
-                        sum[j] += ((int32_t)w[j] - kernelOffset) * A;
+            // a contiguous range of outputs per thread; FC_KB (8) kernel rows at a time, read in order
+            for (ssize_t k0 = 0; k0 < K; k0 += FC_KB) {
+                for (k = k1; k < k1 + FC_KB; k++) for (j = 0; j < n; j += 64)
+                    __builtin_prefetch(kernel + k * kernelDim1 + jb + j);      // the next 8 rows
+                for (ssize_t j = 0; j < n64; j += 64) {
+                    int32x4_t c[16];                                         // 64 sums in 16 registers
+                    for (ssize_t k = k0; k < k1; k++) {
+                        const int8x8_t av = vdup_n_s8(a[k]);
+                        for (int q = 0; q < 4; q++) {
+                            const int8x16_t wv = vld1q_s8(w + 16 * q);
+                            const int16x8_t lo = vmull_s8(vget_low_s8(wv), av), hi = vmull_high_s8(wv, avq);
+                            c[4 * q] = vaddw_s16(c[4 * q], vget_low_s16(lo)); ...
```

- 정수 합이라 순서를 바꿔도 결과가 같다(bit-identical). offset이 0이 아니면 예전 스칼라 경로다.
- 1.92 → 0.69 ms(OMP 1)였다. 그 뒤 fc는 EVTA로 갔다(§1.5). 이 함수는 EVTA가 받지 못하는 fc의 대체 경로로 남았다.

### 2.4 cache 정리 뒤 `dsb sy` (11-02, `7c6fb2c6`, `pynq_driver.cc:62`)

**왜.** multi4가 copy마다 약 400회에 한 번 멈췄다. 실행 전에 5 us 기다리면 없어졌다.
PYNQ 2.4의 `libcma.so`를 objdump로 보니 `cma_flush_cache`가 `dc civac` 루프이고 `dsb`가 없었다. cache 정리는 `dsb` 뒤에야 끝났다고 보장된다. 그래서 EVTA를 시작하는 MMIO 쓰기가 명령 / uop의 DRAM 쓰기보다 먼저 나갈 수 있었다.

**전 / 후:**

```diff
+static inline void CacheMaintenanceBarrier() {
+#if defined(__aarch64__)
+  if (!off) __asm__ __volatile__("dsb sy" ::: "memory");      // EVTA_NO_DSB=1 leaves it out
+#endif
+}
 void VTAFlushCache(void* vir_addr, vta_phy_addr_t phy_addr, int size) {
   cma_flush_cache(vir_addr, phy_addr, size);
+  CacheMaintenanceBarrier();
 }
 void VTAInvalidateCache(void* vir_addr, vta_phy_addr_t phy_addr, int size) {
   cma_invalidate_cache(vir_addr, phy_addr, size);
+  CacheMaintenanceBarrier();
 }
```

**어떻게 맞는지.** copy마다 4000회를 기다림 없이 돌려 멈춤이 없었다. 기다림(`EVTA_PRE_RUN_US`)은 다시 기본 0이 됐다(`94ab8dd5`).

**리뷰.** 원인을 libcma에서 찾았지만 고친 곳은 vtalib다. `cma_flush_cache`를 직접 부르는 다른 코드가 생기면 같은 문제가 돌아온다. libcma를 부르는 곳은 이 두 함수뿐이어야 한다.

### 2.5 EVTA 명령 record / replay (11-02, `e3f41c84`, `cmd_queue.h:422`)

**왜.** EVTA 호출마다 CPU가 명령과 uop을 새로 만들었다. 매 추론 같은 모양, 같은 순서인데도 그랬다(AOT 0단계, [evta_runtime_aot.md](evta_runtime_aot.md) §5).

**어떻게.**
- bundle 함수(conv, maxpool, fc)가 스칼라 인자를 열쇠로, VTA buffer들을 목록으로 `VTAReplayBegin`을 부른다.
- 처음 보는 열쇠면 기록한다. 그 호출의 run(Synchronize)마다 명령과 uop을 복사하고, buffer를 가리키는 LOAD / STORE의 위치를 relocation 표에 적는다.
- 같은 열쇠가 다시 오면 아무것도 만들지 않고 복사본을 돌린다. buffer 주소가 바뀌었으면 relocation으로 고친다.

**전 / 후** (`convolution_wo_tr_impl`):

```diff
   void *vta_bias_buf = bias;
+  int32_t replayKey[] = {1, N, H, W, C, KN, KH, KW, pad_size, stride_size, doRelu, doBias, shift, out_h, out_w,
+                         nVirtualThread, tileHSize, tileWSize, (int32_t)idEVTA, res != nullptr,
+                         res ? (int32_t)res->resShl : 0, res ? (int32_t)res->convShl : 0,
+                         res ? (int32_t)res->sumShr : 0, res ? res->relu : 0, res ? res->buf == output : 0};
+  void *replayBufs[] = {input, kernel, doBias ? bias : nullptr, output, res ? res->buf : nullptr};
+  if (VTAReplayBegin(vtaCmdH, replayKey, sizeof(replayKey), replayBufs, 5))
+    return 0;                                        // replayed: nothing to generate
   ... conv_kernel(...) for every tile ...
   VTASynchronize(vtaCmdH, 1 << 31);
+  VTAReplayEnd(vtaCmdH, 1);
```

```cpp
// cmd_queue.h ReplayBegin (11-02)
std::string k(static_cast<const char *>(key), key_bytes);
auto it = replays_.find(k);
if (it != replays_.end()) { RunReplay(*it->second, bufs, nbufs); return 1; }
if (replay_bytes_ >= ReplayLimit()) return 0;        // EVTA_REPLAY_MB, default 16
rec_ = new Replay;                                    // record this call
for (int i = 0; i < nbufs; i++) rec_->bufs.push_back(phy_addr(bufs[i]));
uop_queue_.UopReset();                                // every uop of the call is LOADed inside it
return 0;
```

maxpool(열쇠 첫 값 2)과 fc(3)도 같다. fc의 replay는 처음에 빠져 있었다(`1cff27af`에서 넣음).

**리뷰.**
- **열쇠가 명령을 정하는 모든 값을 담아야 한다.** 열쇠에 없는 전역 상태가 명령을 바꾸면, 옛 명령이 조용히 다시 돈다.
  실제로 그 뒤 열쇠에 `gEvtaClipHi`(11-06, ReLU6), depthwise와 창 모드(11-08, 11-24), epilogue 값이 차례로 더해졌다. 새 전역 설정(`VTASet...`)을 만들 때 이 열쇠를 함께 고치는 것이 규칙이다.
- buffer의 **내용**은 열쇠에 없다. 내용이 바뀌는 상수 buffer(예: 다른 weight)를 같은 주소에 두면 맞는 결과가 나온다. 주소만 따라가기 때문이다.

### 2.6 residual을 EVTA에서 더하기 (10-31, `9a8257ad`, `VTABundle.cpp:507` `ConvResidual`)

**계산(10-31 판).** conv의 requantize(bias, shift, clip) 뒤, 아직 acc 안에서 한다.

```text
acc = conv << convShl                      ALU ADD acc, acc (convShl번)
acc += residual * (2^resShl I)             residual tile을 inp에 LOAD, 배율 곱한 identity weight로 GEMM
acc = (acc + 2^(sumShr-1)) >> sumShr       ALU ADD imm, SHR
acc = min(max(acc, relu ? 0 : -128), 127)  ALU MAX, MIN
```

- ALU 연산은 MIN / MAX / ADD / SHR(2 bit opcode)과 GEMM뿐이다. 그 bitstream에 MUL이 없었기 때문이다.
- identity weight(2^k I, k = 0..6)는 EVTA마다 한 번 만든다(`getIdentityWeights`). wgt 마지막 entry(1023)에 LOAD한다.

**전 / 후** (`conv_kernel`의 tile 끝):

```diff
       VTAPushALUOp(vtaCmdH, uopHandle_reset, &alu_min, &alu_ctx, 0);      // the conv's own clip, as before
     }
+    if (res) {
+      for (uint32_t i = 0; i < res->convShl; i++)
+        pushAlu(vtaCmdH, idEVTA, outputSramBaseAddr, outputSramBaseAddr, size, RES_ALU_ADD, 0, 0);
+      VTADepPop(vtaCmdH, 1, 2);                                          // the residual LOAD is done
+      pushResGemm(vtaCmdH, idEVTA, outputSramBaseAddr, resSramBaseAddr, size);
+      if (res->sumShr) {
+        pushAlu(..., RES_ALU_ADD, 1, 1 << (res->sumShr - 1));
+        pushAlu(..., RES_ALU_SHR, 1, res->sumShr);
+      }
+      pushAlu(..., RES_ALU_MAX, 1, res->relu ? 0 : -128);
+      pushAlu(..., RES_ALU_MIN, 1, 127);
+    }
     VTADepPush(vtaCmdH, 2, 3);
```

**어떻게 맞는지.** `EVTA_RES_CHECK=1`이면 매 호출 뒤 CPU elemadd + relu와 비교한다. 그 뒤 11-04 requantize(§3.3)가 SHR / MAX / MIN을 residual GEMM의 out 쓰기로 옮겼다. 11-14부터 epilogue 프로그램으로 바뀌었다(`lowerEpilogue`).

### 2.7 `maxpool_evta` (10-31, `d9837eac`, `VTABundle.cpp:5559`)

**계산.** 출력을 16채널 묶음 하나의 oh × ow tile로 나눈다. tile마다:
1. 입력 창((oh − 1) s + K) × ((ow − 1) s + K)을 halo와 함께 LOAD한다(이웃 tile은 겹치는 행을 다시 읽는다). 그림 가장자리 pad는 LOAD의 pad(0)다.
2. identity weight GEMM으로 acc에 옮긴다.
3. 출력을 0으로 놓고, 창 위치(K × K)마다 ALU MAX 한 명령을 낸다.
4. STORE한다. 두 tile을 inp / acc의 두 반쪽에 번갈아 두어 LOAD, 계산, STORE가 겹친다.

```cpp
// one ALU instruction per window position: a 2-level loop over the output pixels, one uop
int alu_poolmax(void *param, VTACommandHandle cmdh) {
  VTAUopLoopBegin(cmdh, p->oh_, p->ow_, p->stride_ * p->iw_, 0);
  VTAUopLoopBegin(cmdh, p->ow_, 1, p->stride_, 0);
  VTAUopPush(cmdh, 1, 0, p->out_, p->in_, 0, RES_ALU_MAX, 0, 0);
  VTAUopLoopEnd(cmdh);
  VTAUopLoopEnd(cmdh);
}
```

**전 / 후.** 그 전에는 CPU `maxpool`(16채널 묶음을 batch로 본 NHWC)이었다. bundle의 차이는 §1.4에 있다.

**리뷰.**
- 창 위치를 바깥 루프로 둔 까닭이 주석에 있다. K × K uop을 한 루프 반복에 넣으면 같은 acc entry를 ALU pipeline에서 연달아 쓴다(hazard).
- **입력 ≥ 0을 vtalib는 검사하지 않는다.** 컴파일러(`findEvtaMaxPools`)가 보장한다. 다른 곳에서 부르면 음수 입력의 pad 자리에서 틀린다.

### 2.8 `fullyconnected_evta_packed` (11-02, `b30f7f1c`, `VTABundle.cpp:5961`)

**계산.**
1. bias를 acc[o]에 LOAD한다.
2. wgt buffer 절반에 들어가는 출력 묶음 g개씩 weight를 연속 LOAD 한 번으로 올린다(다른 절반은 GEMM 중에 LOAD).
3. GEMM acc[o] += inp[c] × wgt[o][c](c 루프, o마다 uop 하나)
4. ALU만으로 0에서 멀리 반올림한다. s = acc >> 31(음수면 −1), acc = (acc + s + 2^(shift−1)) >> shift, clip
5. STORE하고 OUT byte를 복사해 낸다.

**전 / 후.** 그 전에는 CPU `cpuFullyConnected`(§2.3)였다. 첫 판 `fullyconnected_evta`(`8090d38d`)는 첫 호출에서 weight를 EVTA layout으로 바꿔 cache했다. packed 판은 그 일을 컴파일러로 옮겨 CPU는 입력과 출력만 복사한다.

**리뷰.** 처음 판은 보드에서 bit-exact가 아니었다. 같은 날 `336c77f1`에서 4의 부호를 MIN / MAX로 구하게 고쳤다. `EVTA_FC_PROBE` / `EVTA_FC_DEBUG`가 그때 만든 도구다.

### 2.9 systolic 최소 tile 1과 더미 tile 결함 (11-05, `e40d5cca`, `327ddd3a`, `VTABundle.cpp:915`)

**왜.** `conv_kernel`은 7 × 7보다 작은 출력 tile을 더미 행 / 열로 7 × 7까지 늘렸다. HLS GEMM은 acc의 읽기-고치기-쓰기에 hazard 검사가 없어서, 같은 dst가 tile_h × tile_w uop마다 돌아오기 때문이다.
systolic GEMM(`sa_rmw.v`)은 진행 중인 dst를 forward하므로 더미가 필요 없다. 그 일을 고치다 더미 tile의 STORE / residual LOAD 위치 결함을 찾았다.

**전 / 후:**

```diff
+static uint32_t evtaMinTile(uint32_t dflt) {     // EVTA_MIN_TILE=<n>; 1 on a systolic ("_sa_") bitstream (327ddd3a)
+  ...
+}
-    if (resizedOutTileH < MIN_H_SIZE) {
-      outDummyH = MIN_H_SIZE - resizedOutTileH;
-      resizedOutTileH = MIN_H_SIZE;
+    const uint32_t minH = evtaMinTile(MIN_H_SIZE), minW = evtaMinTile(MIN_W_SIZE);
+    if (resizedOutTileH < minH) {
+      outDummyH = minH - resizedOutTileH;
+      resizedOutTileH = minH;
 ...
     // the dummy-padded store / residual load: block index and acc row offset
-                             ((filterIdx+fIdx) * nVirtualThread + threadIdx) * ...
+                             ((filterIdx * nVirtualThread + threadIdx) * nFilterinLoop + fIdx) * ...
-            ... + fIdx * outDummyH * resizedOutTileH,
+            ... + fIdx * outDummyH * resizedOutTileW,
```

- 옛 블록 식(`(filterIdx + fIdx) * nVirtualThread + threadIdx`)은 nVirtualThread 2에서 틀린 채널 묶음에 썼다. 7 × 7보다 작은 출력에서만 이 경로를 타서 Resnet18에는 드러나지 않았다(VGG16 4 × 4 / 2 × 2에서 드러남).
- 같은 결함을 `vta/vtalib`에서도 고쳤다(`020d041`).

**리뷰.** 최소 tile을 bitstream 이름의 `_sa_`로 정한다(§6 1번).

### 2.10 첫 conv 입력 복사를 conv와 겹침 (11-06, `5eee77e1`, `VTABundle.cpp:3962` `InputStream`)

**왜.** 첫 conv의 입력 복사(quantize + 묶기, 1.65 ms)는 NEON 명령 수가 한계라 더 줄지 않았다. 그래서 EVTA가 conv를 하는 동안 숨겼다.

**어떻게.**
- 드라이버의 `Run`을 `RunStart` + `RunWait`로 나눴다.
- 그 conv(`convolution_wo_tr_impl`)는 tile 행마다 EVTA run 하나로 나뉜다. run마다 그 행이 읽는 입력 행을 tag로 기록한다(`VTASetSegmentTag`).
- replay는 run s를 시작한 뒤, 기다리기 전에 hook(run s + 1의 tag)을 부른다. hook이 다음 행들을 묶고 flush한다(double buffering).
- 기록하는 첫 호출은 모든 행을 먼저 쓴다.

**전 / 후** (`cmd_queue.h` `RunReplay`):

```diff
       PreRunWait();
-      int timeout = VTADeviceRun(device_, g.insn_phy, g.count, 1u << 31);
+      if (seg_hook_ && s == 0) seg_hook_(seg_hook_ctx_, g.tag);                    // rows of run 0, before it
+      VTADeviceRunStart(device_, g.insn_phy, g.count);
+      if (seg_hook_ && s + 1 < r.segs.size()) seg_hook_(seg_hook_ctx_, r.segs[s + 1].tag);   // run s + 1's rows meanwhile
+      int timeout = VTADeviceRunWait(device_, 1u << 31);
```

컴파일러는 묶기 함수 이름만 바꿨다(`VTASave.cpp`):

```diff
-      bundle->append(packed ? "  preprocess_conv0pack((const float *)" : "  preprocess_nhwc2vtaio_zerofill((const float *)");
+      bundle->append(packed ? "  preprocess_conv0pack_stream((const float *)" : "  preprocess_nhwc2vtaio_zerofill((const float *)");
```

`vta/vtalib`의 `preprocess_conv0pack_stream`은 한 번에 다 쓴다. 그래서 같은 bundle이 두 vtalib에서 돈다.

**리뷰.**
- 기다리지 않는 stream은 thread마다 하나다(`thread_local gInputStream`). 한 thread에서 stream conv 둘이 겹치면 안 된다. 지금 bundle은 첫 conv 하나뿐이다.
- 이 구조(run을 나눠 시작하고 기다리기)는 그 뒤 `INPUT_PIPELINE=2`(11-27, MobileNetV2)의 polling hook과 interrupt 계획의 바탕이 됐다.

### 2.11 conv DMA 묶기 (11-10, `d8de5781`, `VTABundle.cpp:1012`)

**왜.** DRAM에서도 SRAM에서도 연속인 2D LOAD / STORE가 여러 행으로 나뉘어 갔다(예: 1 × 1 conv의 출력 tile 9 × 11이 176 byte 행 9개). 1 × 1 conv의 weight도 kernel과 입력 묶음마다 256 byte LOAD 하나씩이었다.

**전 / 후:**

```diff
+static void loadBuffer2D(VTACommandHandle cmd, void *buf, uint32_t off, uint32_t x, uint32_t y, uint32_t stride, ...) {
+  if (evtaConvBurst() && y > 1 && stride == x && !xp0 && !yp0 && !xp1 && !yp1 && (uint64_t)x * y < 65536) {
+    x *= y;                 // contiguous rows, no pads: one row
+    y = 1;
+  }
+  VTALoadBuffer2D(cmd, buf, off, x, y, stride, xp0, yp0, xp1, yp1, sram, type);
+}
 ...
-        VTALoadBuffer2D(vtaCmdH, vta_filter_buf, filter_offset, KH * KW,
-                        nFilterinLoop, KH * KW * cBlocks, 0, 0, 0, 0, weightSramBaseAddr, 1);
+        if (wgtAll) {       // 1 x 1: the thread's nFilterinLoop x cBlocks weights with channel 0
+          if (channelIdx == 0)
+            loadBuffer2D(vtaCmdH, vta_filter_buf, filter_offset, cBlocks, nFilterinLoop, cBlocks, 0, 0, 0, 0,
+                         weightSramBaseAddr, 1);
+        } else
+          loadBuffer2D(vtaCmdH, vta_filter_buf, filter_offset, KH * KW, nFilterinLoop, KH * KW * cBlocks, ...);
+        if (wgtAll) {       // GEMMs read them at channel + kernel x cBlocks
+          gemm_ctx.weight_base_addr_ = weightSramBaseAddr + channelIdx;
+          gemm_ctx.wgt_kernel_factor_ = cBlocks;
+        }
```

폭 전체 tile이고 더미가 없으면 nFilterinLoop개 kernel의 출력을 STORE 하나(kernel마다 한 행, DRAM 보폭 out_h × out_w)로 보낸다.

**효과.** Resnet18은 3 × 3이 대부분이라 −0.8 ms였다. DeiT(1 × 1 conv가 대부분)는 EVTA 38.3 → 19.4 ms였다.

## 3. 하드웨어 (`vta-hw-zcu102-systolic`, multi4도 같은 소스)

빌드와 timing, 자원, 비트스트림 이름은 [evta_isa_ext.md](evta_isa_ext.md)와 [evta_runtime_compare.md](evta_runtime_compare.md) §7.15–7.18에 있다. 여기서는 코드만 본다.

### 3.1 ALU immediate 연산을 II 1로 (11-02, `b1755839`, `c50dd868`, `hardware/xilinx/src/vta.cc:826`)

**왜.** EVTA 시간 26.47 ms를 나눠 보니(`EVTA_REPLAY_DUMP`) ALU가 4.63 ms였다. HLS `alu()` 루프는 src와 dst를 읽으므로 II = 2였다(acc_mem 포트 둘).
그런데 conv의 ALU 일(bias ADD, SHR, clip MIN / MAX)은 대부분 immediate 연산이라 dst만 읽는다.

**전 / 후.** immediate 연산만 따로 II 1 루프로 뺐다.

```diff
   VTAAluInsn insn = *((VTAAluInsn *) &insn_raw);
+#ifdef VTA_SYSTOLIC
+  if (insn.use_imm) {                 // reads only dst: one acc read + one write per step fit the two ports
+    ALU_IMM_OUT_LOOP: for (int it_out = 0; it_out < insn.iter_out; it_out++) {
+      ALU_IMM_IN_LOOP: for (int it_in = 0; it_in < insn.iter_in; it_in++) {
+        ALU_IMM_UOP: for (int upc = insn.uop_bgn; upc < insn.uop_end; upc++) {
+#pragma HLS PIPELINE II = 1
+          acc_idx_T dst_idx = uop.range(VTA_UOP_ALU_0_1, VTA_UOP_ALU_0_0) + dst_offset_in;
+          read_tensor<...>(dst_idx, acc_mem, dst_tensor);
+          for (i, b) alu_lane(insn.alu_opcode, dst_tensor[i][b], (acc_T) insn.imm, dst_tensor[i][b], o_tensor[i][b]);
+          write_tensor<...>(dst_idx, dst_tensor, acc_mem);
+          write_tensor<...>(dst_idx, o_tensor, out_mem);
+        }
+    ...
+    return;
+  }
+#endif
   // Loop offset                      // the src / dst loop below as before (II 2)
```

`alu_lane`은 원래 루프의 연산 부분을 함수로 뺀 것이다(두 루프가 같은 계산을 쓴다). `c50dd868`에서 uop 읽기와 주소 덧셈 뒤에 register를 넣었다. 그것이 없으면 400 MHz에서 WNS −0.819 ns였다.

**어떻게 맞는지.** csim, golden 30 PASS, 층 dump 바이트 동일. 같은 실행 파일과 vtalib에서 bitstream만 바꿔 EVTA 26.42 → 24.89 ms였다.

**리뷰.**
- 원래 루프처럼 acc_mem에 `DEPENDENCE inter false`를 둔다. pipeline 안의 앞 step이 쓴 dst를 뒤 step이 읽지 않는다는 약속이다. bundle의 immediate 연산은 명령마다 dst를 한 번씩만 쓰므로 지킨다. 하드웨어는 검사하지 않는다.

### 3.2 RMW reset이 bias를 씀 (11-04, `76aa60e0`, `rtl/sa_rmw.v:57`, `sa_ctrl.v`)

**왜.** conv tile은 acc를 0으로 reset하는 GEMM으로 시작하고, GEMM이 끝난 뒤 ALU ADD로 bias를 더했다. bias를 처음부터 acc에 두면 그 ADD가 없어진다.

**ISA.** GEMM 명령에 `bias_reset` bit를 더했다(pad 자리). 그 명령의 reset uop은 acc[dst]에 0 대신 acc[src]를 쓴다.

**전 / 후** (`sa_rmw.v`):

```diff
-  assign acc_rd_addr = tag_dst;
+  wire [DW-1:0] ra = tag_bias ? tag_src : tag_dst;     // a bias row reads acc[src] (the bias LOADed before the reset GEMM)
+  assign acc_rd_addr = ra;
 ...
-    f3 <= v2 && d2 == d1;   f3z <= z2;                  // forwarding compares against the dst
-    f4 <= v3 && d3 == d1;   f4z <= z3;
-    f5 <= vw1 && dw1 == d1; f5z <= zw1;
-    r1 <= row; d1 <= tag_dst; z1 <= tag_reset; l1 <= tag_last; k1 <= tag_tok;
+    f3 <= v2 && d2 == a1;   f3z <= z2;                  // ... against the address read (src for a bias row)
+    f4 <= v3 && d3 == a1;   f4z <= z3;
+    f5 <= vw1 && dw1 == a1; f5z <= zw1;
+    r1 <= tag_bias ? {PW*N{1'b0}} : row; d1 <= tag_dst; a1 <= ra; z1 <= tag_reset && !tag_bias; l1 <= tag_last; k1 <= tag_tok;
```

- bias row는 배열의 행을 더하지 않는다(`r1 = 0`). 0 쓰기도 아니다(`z1 = reset && !bias`).
- forwarding은 읽은 주소(a1)와 비교한다. 그래서 아직 쓰이는 중인 bias entry도 forward된다.
- `sa_ctrl.v`는 uop FIFO와 tag에 bias bit 하나를 더 싣는다. C 모델(`sim/systolic_model.cc`)과 HLS `vta.cc`도 같게 바꿨다.

**vtalib.** bitstream 이름에 `bias`가 있으면 켠다(`evtaBiasReset`, `EVTA_BIAS_RESET=0|1`). conv tile의 reset 전에 bias entry를 acc에 LOAD하고, reset uop을 `reset_out 2`로 낸다.

```cpp
int bias_reset_acc(void *param, VTACommandHandle cmdh) {
  VTAUopLoopBegin(cmdh, p->nF_, p->T_, 1, 0);
  VTAUopLoopBegin(cmdh, p->T_, 1, 0, 0);
  VTAUopPush(cmdh, 0, 2, p->base_, p->bias_, 0, 0, 0, 0);   // reset 2 = acc[dst] = acc[src]
  ...
}
```

**효과와 확인.** golden 30 PASS, dump 바이트 동일, EVTA 24.89 → 24.15 ms. ALU ADD 0.91 ms 가운데 0.76 ms만 줄었다. stage 3 / 4에서는 그 ADD가 다음 tile의 inp LOAD / STORE 뒤에 숨어 있었기 때문이다(`1731e2c8`).

### 3.3 out 쓰기에 requantize (11-04, `d26c3e30`, `sa_rmw.v` S4–S6, `vtalib .../hw_spec.h:378`)

**왜.** GEMM과 ALU는 step마다 acc의 하위 8 bit를 out에 쓴다. 그 값이 결과가 되도록 ALU SHR / MAX / MIN(그리고 반올림 ADD)으로 acc 자체를 바꿨다. bias reset 뒤 남은 ALU 일의 대부분이 이것이었다.

**ISA(전 / 후).** `VTAGemInsn`과 `VTAAluInsn`의 pad 자리에 필드 다섯을 넣었다.

```diff
-  uint64_t pad_0          : 31;
+  uint64_t rq_en          : 1;      // 0: the low 8 bits as before
+  uint64_t rq_round       : 2;      // 0 none, 1 + 2^(shift-1), 2 + 2^(shift-1) - (acc < 0) (half away from zero)
+  uint64_t rq_shift       : 5;
+  int64_t  rq_lo          : 8;
+  int64_t  rq_hi          : 8;
+  uint64_t pad_0          : 7;
```

**RTL(전 / 후, `sa_rmw.v`).** acc는 예전처럼 S3에서 쓴다. out은 세 단을 더 지나 S6에서 쓴다.

```diff
-      assign out_wr_data[8*k+7:8*k] = s3[32*k+7:32*k];          // the low 8 bits of the sum
+      assign out_wr_data[8*k+7:8*k] = o6[8*k+7:8*k];
+      always @(posedge clk) begin
+        t4[32*k+31:32*k] <= sk + ((m3 && sk[31]) ? hm3 : hb3);  // S4 + round
+        h5[32*k+31:32*k] <= $signed(tk) >>> sh4;                 // S5 arithmetic shift
+        o6[8*k+7:8*k]    <= !e5 ? hk[7:0] :                      // S6 clip (or the low 8 bits without rq_en)
+                            hk < $signed({{24{lo5[7]}}, lo5}) ? lo5 :
+                            hk > $signed({{24{hi5[7]}}, hi5}) ? hi5 : hk[7:0];
+      end
```

done / token / busy도 out 쓰기와 함께 S6으로 옮겼다. 그래서 store가 out을 그 전에 읽지 않는다. HLS `vta.cc`의 ALU에는 `requant()`를 넣었다.

**vtalib(전 / 후, `conv_kernel`).** `VTARequant(cmd, en, round, shift, lo, hi)`가 그 뒤에 넣는 GEMM / ALU 명령의 `rq_*`를 정한다.

```diff
+    const bool rqOn = evtaRequant();                          // "_rq" in the bitstream name, EVTA_REQUANT=0|1
+    const bool rqConv = rqOn && !res && !pool, rqPool = rqOn && pool && relu, rqRes = rqOn && res;
+    if (rqConv) VTARequant(vtaCmdH, 1, 0, shift, relu ? 0 : -128, 127);   // the tile's GEMMs write the int8
 ...
+      if (!rqConv && !rqPool) {                               // no SHR / MAX / MIN when the out write requantizes
       { ... ALU SHR by shift ... ALU MAX ... ALU MIN ... }
+      }
 ...
+    if (rqConv) VTARequant(vtaCmdH, 0, 0, 0, 0, 0);
```

- residual은 residual GEMM에, conv + pool은 열 방향 pool MAX에, fc는 fc GEMM에 붙인다(max와 requantize는 순서를 바꿔도 같다).
- **함께 찾은 결함:** vtalib `NextInsn()`이 명령을 0으로 채우지 않아 pad bit가 쓰레기였다. 새 필드가 그 자리라 이제 0으로 채운다.

**어떻게 맞는지.** C 모델 `rqout()`이 지금 vtalib의 ALU 순서와 5,750만 값에서 같았다. golden PASS, dump 바이트 동일, `EVTA_RES_CHECK=1` 20건 0 불일치. `EVTA_REQUANT=0`이면 `bias` bitstream과 같다. EVTA 24.15 → 23.05 ms였다.

## 4. 확인 도구 (이 일에서 만든 것)

| 도구 | 하는 일 | 처음 |
|---|---|---|
| `EVTA_RES_CHECK=1` | 합친 residual, conv + pool, maxpool, fc를 매 호출 CPU로 다시 계산해 비교 | 10-31 |
| `EVTA_DUMP_CONV=<dir>` | EVTA conv(와 maxpool) 출력을 모두 저장. 바꾸기 전후 바이트 비교 | 11-02 |
| `EVTA_REPLAY_LOG=1` / `2` | replay 기록 크기 / run마다 GEMM·ALU 반복, DRAM byte, 시간 | 11-02 |
| `EVTA_REPLAY_DUMP=<dir>` | 기록한 run마다 명령과 uop을 글로 저장(§3.1의 ALU 분석) | 11-02 |
| `EVTA_MEMEXP=<mask>` | replay에서 inp / wgt / acc LOAD, STORE를 줄여 DRAM 몫의 상한을 잼(결과는 틀림) | 11-02 |
| golden(single 30, multi 3) | `run_prof.sh -s ... all`의 정답 비교 | 10월 |
| C 모델 → RTL testbench → csim | `sim/systolic_model.cc`, `rtl/tb/`, `sim/run_csim.sh` | 10-27 |

## 5. 그 전에 이미 있던 것 (이 일에서 바탕으로 쓴 것)

| 것 | 어디 |
|---|---|
| systolic GEMM(`gemm_sa`, `sa_array`, `sa_rmw`, `sa_ctrl`) | 10-27, [evta_systolic_design.md](evta_systolic_design.md), [evta_systolic.md](evta_systolic.md) |
| BNN 첫 conv의 0 채우기 경로(`saveFirstBNNVTAConvolutionInst`) | ETRI NEST-C |
| `conv_kernel`, `gemmop_mxn`, virtual thread, `VTASchedules.h` | ETRI vtalib / NEST-C |
| 2 bit `memory_type` / `alu_opcode`의 ISA | ETRI bitstream(CLAUDE.md) |

## 6. 리뷰 의견 모음

| # | 어디 | 의견 | 심각도 |
|---|---|---|---|
| 1 | `evtaBiasReset`, `evtaRequant`, `evtaMinTile` | 하드웨어 기능을 `/tmp/evta_bitstream`의 이름 부분 문자열(`bias`, `_rq`, `_sa_`)로 정한다. 이름이 우연히 그 문자열을 담거나, 기능이 있는데 이름에 없으면 틀린 명령을 낸다. 장치 능력 조회(bitstream의 version register)로 바꾸기를 권한다([unified_runtime_plan.md](unified_runtime_plan.md)) | 중 |
| 2 | `cmd_queue.h` replay 열쇠 | 명령을 정하는 모든 값이 열쇠에 있어야 한다. 그 뒤 `gEvtaClipHi`, depthwise, epilogue 값이 차례로 더해졌다. 새 `VTASet...` 전역 설정을 만들 때 열쇠를 함께 고치는 규칙을 코드 주석에 두기를 권한다 | 중 |
| 3 | `VTA::save`의 packed 활성값 flush 목록 | CPU 연산이 packed 활성값을 쓰면 flush해야 하는데, 그 목록이 kind 나열이다. 새 CPU 연산을 packed로 받게 할 때 빠뜨리면 EVTA가 옛 값을 읽는다 | 중 |
| 4 | `transpose_vtaio2nhwc` | 입력 위치가 batch에 달리지 않는다(`90effa36`이 일부러 예전 대응으로 되돌림). N > 1이면 틀린다. 지금은 모두 N = 1 | 낮 |
| 5 | `CPUBundle_aarch64.cpp` / `CPUBundle_generic.cpp` | 같은 transpose가 두 파일에 있다. 보드는 generic만 빌드한다 | 낮 |
| 6 | buffer pool | 다시 받은 buffer에 앞 값이 남는다. 상한(32 MB)을 넘으면 매번 할당으로 돌아가 조용히 느려진다 | 낮 |
| 7 | `maxpool_evta`, `convolution_wo_tr_pool` | 입력 ≥ 0을 vtalib가 검사하지 않는다. 컴파일러가 보장한다 | 낮 |
| 8 | HLS ALU II 1 루프 | `DEPENDENCE inter false`: 명령 안에서 dst를 한 번만 쓴다는 약속에 기댄다. 하드웨어는 검사하지 않는다 | 낮 |
| 9 | `getConv0Pack` | 입력 tile 조건의 1024가 multi-EVTA inp 절반으로 고정이다 | 낮 |
| 10 | `-vta-fuse-preprocess` 뒤의 bundle | 지운 세 명령의 `malloc` / `free`(150,528 byte × 3)가 남는다 | 낮 |
| 11 | `convolution_wo_tr_pool` | 합칠 때도 conv 출력 buffer(0.8 MB)를 할당한다 | 낮 |
| 12 | `InputStream` | thread마다 기다리지 않는 stream 하나. 한 thread에서 stream conv 둘이 겹치면 안 된다 | 낮 |
| 13 | `dsb` | 고친 곳이 vtalib의 두 함수다. `cma_flush_cache`를 직접 부르는 코드가 생기면 multi4 멈춤이 돌아온다 | 정보 |
