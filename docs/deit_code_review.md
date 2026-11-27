# DeiT-tiny 시간 줄이기 코드 리뷰: 소프트웨어와 하드웨어, 바꾸기 전과 후

2026-11-27. [deit_optimization.md](deit_optimization.md)의 DeiT-tiny 1장 3,770 → 37.03 ms(2026-11-09 ~ 17, single, 1코어)를 만든 코드를 기능마다 설명한다.
11-26에는 34.38 ms다. 같은 방식의 문서는 [resnet18_code_review.md](resnet18_code_review.md), [mobilenetv2_code_review.md](mobilenetv2_code_review.md)다.

DeiT는 CNN과 연산이 다르다.
- Linear(행 × 채널 행렬곱)가 연산의 83 %다.
- attention의 두 행렬곱은 양쪽 operand가 모두 실행 중에 만들어진다.
- layer norm, softmax, GELU, residual add 같은 원소별·행별 연산이 많다.

그래서 이 일은 두 갈래였다.
1. 11-09 ~ 11: 컴파일러와 vtalib로 행렬곱을 EVTA conv로 바꾸고, 나머지를 CPU에서 빠르게 했다(3,770 → 75.7 ms).
2. 11-11 ~ 17: EVTA ALU를 넓혀(**하드웨어**, ALU 확장 1–3단계) 원소별·행별 연산도 EVTA로 옮겼다(75.7 → 37.03 ms).

기능마다 다음을 적는다.
- **왜:** 무엇이 느렸나
- **전 / 후:** 바꾸기 전 코드와 바꾼 코드
- **어떻게 맞는지:** 무엇으로 확인했나
- **리뷰:** 위험과 한계

**읽는 법.**
- **소스의 전 / 후**는 `diff` 블록이다. `-` 줄이 그 기능의 첫 commit 직전(`git show <commit>^:<파일>`), `+` 줄이 그 commit이다. 긴 함수는 바뀐 줄 둘레만 남겼다.
- **만들어진 bundle의 전 / 후**는 2026-11-27 보드에서 지금 model-compiler로 다시 만든 DeiT bundle에서 뽑았다.
  - `c0`: 이 문서의 컴파일러 옵션 17개를 모두 끔
  - `all`: 다 켬(기본값)
  - `no_<옵션>`: 그 옵션만 끔
  - 만든 곳: 보드 `~/JO/ubun_wt/deitrev/<tag>/deit.cpp`(git 밖, `gen.sh`). 모델과 profile은 지금 `DeiTTest`의 `deit_prune.onnx`, `deit_calib_mc.yaml`이다. `gen_deit_bundle.sh`와 같은 명령(`-quantization-schema=symmetric -vta-round-shift`)이고, 출력 위치만 다르다.
  - 이름은 줄였다(`_deit_encoder_layer_0_attention_attention_` → `L0_att_`).
- **효과 숫자**는 [deit_optimization.md](deit_optimization.md) §2, §2.1의 1장 시간이다(single, OMP 1). "a → b"는 같은 날 번갈아 잰 값이다.
- **코드 위치**는 `파일:줄`(2026-11-27 `main`)이다. 우리가 고친 곳에는 `//#### JO added (<날짜>)` 표식이 있고, 그 주석이 설계 설명이다.
- **하드웨어 회로**의 자세한 설명(데이터 경로, LUT 메모리, 파이프라인, 자원)은 [evta_alux_hw.md](evta_alux_hw.md)에 있다. 이 문서는 코드의 전 / 후만 본다. 필드와 bitstream 이름, 스위치는 [evta_isa_ext.md](evta_isa_ext.md) §8이다.

## 0. 한눈에

| § | 기능 | 층 | 주 파일 / 함수 | 스위치 | 1장 시간 (OMP 1) | 첫 commit |
|---|---|---|---|---|---|---|
| 1.1 | 범용 CPU 연산을 bundle에 쓰기 | 컴파일러 | `VTASave.cpp` `saveGenericCpuInst`, `NESTC_CPU_OPS` | (항상) | 처음 끝까지: 3,770 | `76dcd974` 11-09 |
| 1.2 | 상수 rhs MatMul을 EVTA 1 × 1 conv로 | 컴파일러 | `VTA.cpp` `matMulToConv` | `-vta-matmul-conv` | 3,770 → 475 | `05e02771` 11-09 |
| 1.3 / 2.1 | attention matmul을 EVTA로 | 컴파일러 + vtalib | `matmul_evta` | `-vta-evta-matmul` | 475 → 359 | `996d769f` 11-09 |
| 1.4 | 원소별 연산 사슬을 256칸 표로 | 컴파일러 | `findLutRegions`, `nestc_cpu::lut8` | `-vta-lut-elementwise` | 359 → 206 (§7.25 묶음) | `002ecdf7` 11-09 |
| 1.5 | Linear bias를 conv bias에 접기 | 컴파일러 | `VTA.cpp` | `-vta-fold-matmul-bias` | 206 → 137.2 (§7.26 묶음) | `b7d909d8` 11-09 |
| 1.6 | layer norm 21명령을 함수 하나로 | 컴파일러 | `findLayerNorms`, `nestc_cpu::layerNormI8` | `-vta-fuse-layernorm` | (같은 묶음) | `76f3d671` 11-10 |
| 1.7 / 2.2 | head 셋을 한 호출, Q K^T를 conv 출력에서 바로 | 컴파일러 + vtalib | `findMatMulGroups`, `matmul_evta_qk` | `-vta-matmul-heads`, `-vta-attn-keep-layout` | 137.2 → 130.7 | `ab10a797` 11-10 |
| 1.8 | softmax / LN 역수 곱 | 컴파일러(CPU 함수) | `nestc_cpu::softmaxI8`, `layerNormI8` | `-vta-cpu-approx` | 130.7 → 123.5 | `42a9a171` 11-10 |
| 1.9 | 자유 scale, EVTA 비율만 2의 거듭제곱 | 컴파일러 | `VTA.cpp` `pow2Ratios` | `-vta-pow2-ratio` | 정확도 56.17 → 59.71 % | `db968f7b` 11-10 |
| 1.10 | 정수 layer norm | 컴파일러(CPU 함수) | `nestc_cpu::layerNormI8` | `NESTC_LN_INT` | 88.8 → 75.7 | `1247d0f1`, `4559ece5` 11-10 ~ 11 |
| 1.11 / 2.3 | GELU 표를 fc1 conv의 출력 LUT로 | 컴파일러 + vtalib | `findOutLutConvs`, `VTASetOutputLut` | `-vta-alu-output-lut` | (ALU 확장 묶음) | `55bad819`, `68199a6c` 11-11 ~ 12 |
| 1.12 / 2.4 | layer norm을 EVTA ALU 프로그램으로 | 컴파일러 + vtalib | `layernorm_evta(2)` | `-vta-alu-layernorm` | 84.84 → 75.07 (`_alux`, replay 뒤) | `e29ea7cb` 11-12 |
| 1.13 / 2.5 | softmax를 EVTA ALU 프로그램으로 | 컴파일러 + vtalib | `softmax_evta`, `softmaxIntQ` | `-vta-alu-softmax` | 71.73 → 60.55 (`_alux2`) | `ecf72a0a` 11-12 |
| 1.14 | 행 × 채널 활성값을 EVTA layout으로 | 컴파일러 + vtalib | `findPackedRows`, `layernorm_evta2` | `-vta-pack-rows` | 59.82 → 49.93 | `4d33db3e` 11-15 |
| 1.15 / 2.6 | residual add를 EVTA 정수 add로 | 컴파일러 + vtalib | `elemAddIntParams`, `elemadd_evta` | `-vta-evta-elemadd` | 49.85 → 47.01 | `f2dc32bd` 11-15 |
| 1.16 | attention `Div` 표를 Q K^T의 출력 LUT로 | 컴파일러 + vtalib | `findOutLutMatMuls` | `-vta-alu-output-lut` | 46.17 → 44.21 | `d1ef8040` 11-16 |
| 1.17 | 마지막 블록은 2행만 | 모델 변환 + 컴파일러 | `prune_rows_onnx.py` | `PRUNE_ROWS` | 44.28 → 41.07 | `c19b05a9` 11-16 |
| 1.18 | A x V의 V와 출력을 EVTA layout 그대로 | 컴파일러 + vtalib | `findAttnKeepLayout`, `matmul_evta_av` | `-vta-attn-keep-layout` | 41.00 → 37.82 | `071a66cf` 11-16 |
| 1.19 | patch embedding add를 conv epilogue로 | 컴파일러 | `findEmbedFusions` | `-vta-fuse-embed` | 37.79 → 37.35 | `88d2442c` 11-16 |
| 1.20 | packed 텐서의 앞 행 꺼내기, softmax 기본 켬 | 컴파일러 | `unpackRowsPart` | | 37.37 → 37.03 | `fb98bb02` 11-17 |
| 2.7 | NEON quantize / dequantize / elemadd, 4차원 transpose | vtalib | `CPUBundle_generic.cpp` | (항상) | (§7.25–7.26 묶음) | `c58fd7ec`, `e718cd47` |
| 3.1 | ALU 확장 1단계: 연산 4–11, lut_en (ISA, HLS) | **HW** | `hw_spec.h`, `vta.cc` `alux_step` | Makefile `ALUX=1`, `_alux` | (위 1.11–1.13의 바탕) | `f847659b`, `e6df7fa7` 11-11 |
| 3.2 | 400 MHz를 위한 register | **HW** | `vta.cc` `alu_reg`, `sa_rmw.v` | | 380 → 400 MHz | `7a128c5b` 11-14 |
| 3.3 | ALU 확장 2단계: slot 루프 II 1, 합친 연산 4개 | **HW** + vtalib | `vta.cc` `ALU_SLOT` | `_alux2`, `EVTA_ALUX2` | 76.14 → 71.73 | `c50e9688` 11-15 |
| 3.4 | ALU 확장 3단계: SQACC, LUTC | **HW** + vtalib | `vta.cc`, `hw_spec.h` `alu_op_ext2` | `_alux3`, `EVTA_ALUX3` | 60.82 → 60.14 | `81913204` 11-15 |

이 밖에 DeiT에 쓰인 기능 가운데 이미 다른 문서에 있는 것: conv DMA 묶기(`EVTA_CONV_BURST`, 107.4 → 88.5 ms, [resnet18_code_review.md](resnet18_code_review.md) §2.11), conv epilogue 프로그램([evta_alux_epilogue.md](evta_alux_epilogue.md)), LOAD 앞당기기([evta_load_ahead_guide.md](evta_load_ahead_guide.md)), schedule(`VTASchedules.h`, §7.28).

### 0.1 만들어진 bundle 전체: 모두 끔(`c0`) → 다 켬(`all`)

`deit()` 본문의 호출 수다.

| 호출 | `c0` | `all` | 무엇이 바꿨나 |
|---|---|---|---|
| CPU float `nestc_cpu::matmul` | 121 | 0 | §1.2, §1.3 |
| EVTA conv `convolution_wo_tr` | 0 | 73 | §1.2(Linear 72), patch conv 1 |
| `matmul_evta_qk` / `matmul_evta_av` | 0 / 0 | 11 / 11 | §1.7, §1.18(마지막 블록 Q K^T, A V의 2행은 CPU `matmulI8` 6개) |
| CPU float `fullyconnected` / `nonvtaconvolution` | 2 / 1 | 0 / 0 | §1.9(비율이 2의 거듭제곱이 아니라 EVTA 조건에 맞지 않았다) |
| `quantize` / `dequantize` | 269 / 318 | 4 / 5 | 거의 모든 연산이 int8에 머문다 |
| `elemadd` / `elemdiv` / `elemsub` (CPU float 사슬) | 160 / 100 / 25 | 3 / 4 / 0 | §1.4, §1.6 |
| `reduceAdd` / `mul` / `square` / `pow` | 50 / 49 / 25 / 25 | 2 / 0 / 0 / 0 | §1.6 layer norm |
| `softmaxI8` / `erfQ` (CPU) | 12 / 12 | 0 / 0 | §1.13(`softmax_evta`), §1.11(GELU 표) |
| `layernorm_evta2` | 0 | 24 | §1.12, §1.14 |
| `softmax_evta` | 0 | 12 | §1.13 |
| `elemadd_evta` | 0 | 22 | §1.15(24개 가운데 2개는 마지막 블록의 행 섞인 add) |
| `VTASetOutputLut` (켜기 + 끄기) | 0 | 46 | §1.11 GELU 12 + §1.16 `Div` 11 |
| `transpose` | 38 | 4 | §1.7, §1.14, §1.18 |
| `free` | 1,138 | 757 | |

- `all`의 EVTA 호출은 `if (!layernorm_evta2(...)) nestc_cpu::layerNormI8Rows(...)`처럼 대체 경로가 같은 줄에 있다. ALU 확장이 없는 bitstream에서는 CPU 함수가 돈다. `#ifdef VTA_OUTPUT_LUT`가 없는 vtalib(`vta/vtalib`)로 빌드하면 CPU 함수만 남는다.
- `VTA_FUSION_LOG` 요약(bundle 머리 주석):

```diff
-//   0 EVTA producers, 0 with a tail, 0 read by a tail op not fused
-//   124 GEMMs (conv / fc / matmul) on the CPU
-//     on the CPU: 121 x matmul: float (or int8 with an offset)
-//     on the CPU: 2 x fc: scale ratio not 2^-k with 0 <= k <= 15
-//     on the CPU: 1 x conv: scale ratio out / (in * filter) = 1130.86, not a power of two >= 2
+//   73 EVTA producers, 13 with a tail, 24 read by a tail op not fused
+//     not fused: 24 x an add: -vta-fuse-elemadd-mul is off (elemadd_evta is faster)
+//   6 GEMMs (conv / fc / matmul) on the CPU
+//     on the CPU: 3 x matmul 2 x 198 x 64: 2 rows: no H x W with W >= 7 (largest W 1)
+//     on the CPU: 3 x matmul 2 x 64 x 198: 2 rows: no H x W with W >= 7 (largest W 1)
```

**옵션 하나만 끈 bundle의 호출 수.** 그 옵션이 무엇을 EVTA로 옮겼는지가 보인다(`all` 대비 바뀐 칸만, 굵게).

| bundle | EVTA conv | qk / av | 다른 matmul | `layernorm_evta2` | `softmax_evta` | `elemadd_evta` / CPU `elemadd` | 출력 LUT | transpose(전부) | flush | `memmove` |
|---|---|---|---|---|---|---|---|---|---|---|
| `all` | 73 | 11 / 11 | CPU int8 6 | 24 | 12 | 22 / 3 | 46 | 9 | 16 | 15 |
| `no_matmul-conv` | **1** | **0 / 0** | **CPU float 121** | 24 | 12 | 22 / **51** | **0** | **36** | **26** | **2** |
| `no_evta-matmul` | 73 | **0 / 0** | **CPU int8 72** | 24 | 12 | 22 / 3 | **24** | **97** | **27** | **147** |
| `no_matmul-heads` | 73 | **0 / 0** | **`matmul_evta` 66** | 24 | 12 | 22 / 3 | **24** | **97** | **27** | **147** |
| `no_attn-keep-layout` | 73 | **0 / 0** | **`matmul_evta_n` 22** | 24 | 12 | 22 / 3 | **24** | **97** | **27** | **147** |
| `no_fold-matmul-bias` | 73 | **0 / 0** | **`matmul_evta_n` 22** | 24 | 12 | 22 / 3 | **0** | **73** | **51** | **147** |
| `no_lut-elementwise` | 73 | 11 / 11 | CPU int8 6 | 24 | 12 | 22 / **15** | **0** | **33** | 16 | 15 |
| `no_fuse-layernorm` | 73 | 11 / 11 | CPU int8 6 | **0** | 12 | **0 / 28** | 46 | **11** | **38** | 15 |
| `no_alu-layernorm` | 73 | 11 / 11 | CPU int8 6 | **0**(CPU 직접) | 12 | 22 / 3 | 46 | 9 | 16 | 15 |
| `no_alu-softmax` | 73 | 11 / 11 | CPU int8 6 | 24 | **0**(CPU `softmaxI8` 12) | 22 / 3 | 46 | 9 | 16 | 15 |
| `no_alu-output-lut` | 73 | 11 / 11 | CPU int8 6 | 24 | 12 | 22 / 3 | **0** | **33** | 16 | 15 |
| `no_pack-rows` | 73 | 11 / 11 | CPU int8 6 | **0**(CPU `layerNormI8` 24) | 12 | **0 / 28** | 46 | **106** | **61** | 15 |
| `no_evta-elemadd` | 73 | 11 / 11 | CPU int8 6 | 24 | 12 | **0 / 25** | 46 | **11** | **38** | 15 |
| `no_fuse-embed` | 73 | 11 / 11 | CPU int8 6 | 24 | 12 | 22 / 3 | 46 | **11** | 16 | 15 |
| `no_pow2-ratio` | **0** | **0 / 0** | **CPU int8 72** | 24 | 12 | **0 / 3** | **0** | **50** | **24** | **147** |
| `no_keep-layout` | 73 | 11 / 11 | CPU int8 6 | **0**(CPU `layerNormI8` 24) | 12 | **0 / 28** | 46 | **106** | **61** | 15 |
| `no_cpu-approx`, `no_conv0-pack` | 같음 | | | | | | | | (conv0-pack: 17) | |

- **옵션이 서로 기대는 사슬이 보인다.** `-vta-fold-matmul-bias`를 끄면 head 묶음과 출력 LUT도 함께 빠진다. 앞의 합치기가 뒤의 합치기 모양을 만들기 때문이다.
- `no_pow2-ratio`는 자유 scale 그대로라 EVTA 비율 조건에 맞는 conv가 하나도 없다(전부 CPU int8 행렬곱).
- `no_cpu-approx`는 호출이 같다. CPU 함수 안의 계산(역수 곱)만 바뀐다. `no_conv0-pack`은 patch conv 하나가 0 채우기 경로로 간다.

## 1. 컴파일러 (`lib/Backends/VTA/VTA.cpp`, `VTASave.cpp`)

### 1.1 범용 CPU 연산을 bundle에 쓰기 (11-09, `76dcd974`)

**왜.** VTA backend는 CNN 연산(conv, pool, add, relu, fc)만 bundle에 쓸 줄 알았다. DeiT에는 float matmul, 복사, insert / extract, 아무 transpose, layer norm / GELU 조각, 행 softmax가 있었다. 처음 보는 명령은 bundle을 만들지 못했다.

**어떻게.** `saveGenericCpuInst`가 그런 명령을 C++ 코드로 bundle에 쓴다(`NESTC_CPU_OPS`, bundle 머리의 `namespace nestc_cpu`). tensor view는 그 offset의 포인터로, 상수는 한 번만 쓴다. 다루지 못하는 명령을 만나면 이름을 찍고 멈춘다.

```cpp
// generated bundle (c0): the first Linear of layer 0, float on the CPU
nestc_cpu::matmul(L0_att_query_MatMul_MatMul_0_bigMatMul_res, L0_att_query_..._dequantize__1_res__2,
                  L0_att_query_..._merge_constfold, 198, 192, 576);   // Q, K, V in one 198 x 192 x 576
```

**리뷰.**
- bundle 안에 CPU 함수의 본문이 들어간다. vtalib와 bundle 두 곳에 비슷한 CPU 코드가 생긴다(예: `matmulI8`, `layerNormI8`). 고칠 때 둘을 함께 봐야 한다.
- `softmax`는 vtalib의 것이 한 행에만 맞아서, 행 softmax를 bundle에 따로 썼다.

### 1.2 상수 rhs MatMul을 EVTA 1 × 1 conv로 (11-09, `05e02771`, `-vta-matmul-conv`, `VTA.cpp`)

**왜.** Linear(Q, K, V, proj, fc1, fc2 × 12층)가 연산의 83 %인데 CPU float였다.

**어떻게.** 그래프 단계(lowering 뒤)에서 int8 MatMul(M × K) · (K × N, 상수)을 1 × 1 conv로 바꾼다.
- M 행을 H × W 그림으로 본다. W는 √M 이하에서 M을 나누는 가장 큰 수다(198 → 18 × 11).
- filter는 rhs의 전치(N × 1 × 1 × K), bias는 0이다. 그러면 `getConvPath`가 EVTA 경로를 고른다.
- K, N이 16의 배수여야 한다. 활성 rhs의 MatMul은 CPU int8 `matmulI8`이 한다(§1.3에서 EVTA로).

**전 / 후.** 새 pass다(`matMulToConv`). 그 전에는 float MatMul이 그대로 CPU로 갔다. 만들어진 bundle(`no_matmul-conv` → `all`, layer 0 Q):

```diff
-  nestc_cpu::matmul(L0_att_query_MatMul_MatMul_0_bigMatMul_res, ..._dequantize__1_res__2, ..._merge_constfold, 198, 192, 576);
+  convolution_wo_tr(L0_att_query_MatMul_MatMul_0_conv1x1_input_transpose, onnx__MatMul_1545__2_conv1x1,
+                    (int32_t *)L0_att_query_MatMul_MatMul_0_conv1x1_bias, ..._output_bef_transpose, 1, 18, 11, 192, ...);
```

```cpp
// VTA.cpp matMulToConv (11-09)
dim_t w = 1;
for (dim_t d = 1; d * d <= rows; d++)
  if (rows % d == 0) w = d;                                         // 198: w = 11, h = 18
auto *filter = M->createConstant(ElemKind::Int8QTy, {N, 1, 1, K}, W->getType()->getScale(), 0, ...);
for (n, k) hd.at({n, 0, 0, k}) = hs.at({k, n});                      // rhs transposed
auto *bias = M->createConstant(ElemKind::Int32QTy, {N}, lTy->getScale() * W->getType()->getScale(), 0, ...);
bias->getPayloadMutable().zero();
auto *in = F->createReshape(name + "_conv1x1_in", MM->getLHS(), {1, h, w, K});
auto *conv = F->createConv(name + "_conv1x1", in, filter, bias, convTy, {1, 1}, {1, 1}, {0, 0, 0, 0}, 1);
MM->getResult().replaceAllUsesOfWith(F->createReshape(name + "_conv1x1_out", conv, {rows, N}));
```

같은 날 `25803358`에서 VTASave가 여러 conv가 같이 쓰는 filter / bias를 한 번만 쓰게 고쳤다. 두 번 쓰면 그 뒤 모든 상수의 offset이 밀렸다.

**리뷰.**
- 198 = 18 × 11처럼 W가 작으면 tile 모양이 나빠진다. 마지막 블록의 2행(§1.17)은 W ≥ 7을 만들 수 없어 CPU에 남는다(bundle 머리의 "6 GEMMs on the CPU").

### 1.3 attention matmul을 EVTA로 (11-09, `996d769f`, `-vta-evta-matmul`)

**왜.** Q K^T와 A V는 rhs가 활성값이라 §1.2로 바꿀 수 없다(14 % 연산).

**어떻게.** `matmul_evta`(§2.1)가 실행 중에 두 operand를 EVTA layout으로 묶고 같은 1 × 1 conv를 돌린다. 비율이 2^-shift(1 ≤ shift ≤ 15)일 때만이고, 반올림은 bias 2^(shift−1)로 half up이다.

만들어진 bundle(`no_evta-matmul` → `no_matmul-heads`, head 하나):

```diff
-  nestc_cpu::matmulI8(L0_att_MatMul_concat_..._tv_dest, L0_att_MatMul_sliceA_0_reshape__2_res, L0_att_MatMul_sliceB_0_reshape_res,
-                      198, 64, 198, 0.001953125f);                      // CPU int8, std::round
+  matmul_evta(L0_att_MatMul_sliceA_0_reshape__2_res, L0_att_MatMul_sliceB_0_reshape_res, L0_att_MatMul_concat_..._tv_dest,
+              198, 64, 198, 9, vtaCmdH, 2, 9, 11);                     // shift 9, schedule vt 2, 9 x 11
```

**리뷰.** CPU `matmulI8`은 `std::round`(0에서 멀리), EVTA는 half up이다. 음수 .5에서 다르다. 11-16에 `matmulI8`도 2^-s scale을 half up으로 반올림하게 맞췄다(마지막 블록의 CPU 행렬곱이 EVTA와 같은 바이트가 되도록).

### 1.4 원소별 연산 사슬을 256칸 표로 (11-09, `002ecdf7`, `-vta-lut-elementwise`)

**왜.** GELU(`x * 0.5 * (1 + erf(x / √2))`)는 dequantize → 원소별 float 연산 몇 개 → quantize였다. int8 입력 하나에서 int8 출력 하나로 가는 사슬이다.

**어떻게(`findLutRegions`).** 한 int8 buffer(root)에서 int8 값(out)으로 가는 원소별 명령의 연속을 찾는다. 다른 operand는 모든 원소가 같은 상수여야 한다.
그런 사슬은 256개 입력값에 대한 표로 바뀐다. 표는 실행 중 처음 한 번, **같은 코드를 256원소 probe에 돌려** 만든다. 그래서 결과가 바이트까지 같다.

만들어진 bundle(`no_lut-elementwise` → `all`): CPU `elemadd` 15 → 3, `quantize` 28 → 4, `dequantize` 41 → 5. `all`에서는 그 표들의 대부분(GELU, attention `Div`)이 다시 EVTA 출력 LUT로 간다(§1.11, §1.16). `nestc_cpu::lut8`은 `if (!VTAHasAluExt())` 대체 경로로 남는다.

**리뷰.**
- `adc5d369`(같은 날)에서 "영역의 다른 buffer가 읽히기 전에 해제되는지" 검사가 거꾸로였던 것을 고쳤다. 영역 판단의 조건이 많아, 새 모델에서는 `VTA_FUSION_LOG`로 무엇이 표가 됐는지 봐야 한다.
- 표 만드는 함수는 파일 범위 함수다(`538b5cf9`). 추론 함수 안에서는 한 문장이어야 `timed_bundle.py`가 시간을 잴 수 있다.

### 1.5 Linear bias를 conv bias에 접기 (11-09, `b7d909d8`, `-vta-fold-matmul-bias`, `VTA.cpp`)

**어떻게.** §1.2의 conv 뒤에 오는 상수 bias Add(한 행을 반복)를 conv의 int32 bias에 넣는다. int8 bias × scale 비가 정수일 때만이었고, 11-10 `db968f7b`부터는 정수가 아니어도 반올림해 넣는다(오차 < 0.5, GEMM scale에서).
그러면 conv가 Add의 type을 직접 쓴다. 반올림이 conv와 Add 두 번에서 한 번이 된다.

만들어진 bundle(`no_fold-matmul-bias` → `all`): Add가 남으면 conv 출력과 Add 사이에 transpose와 flush가 끼고(transpose 73, flush 51), 그 뒤 head 묶음(§1.7)과 출력 LUT(§1.11)의 모양도 깨진다.

### 1.6 layer norm 21명령을 함수 하나로 (11-10, `76f3d671`, `-vta-fuse-layernorm`)

**왜.** Glow는 layer norm을 int8 명령 21개로 내린다. dequantize, reduceAdd, quantize, elemdiv, broadcast, elemsub, pow 2, reduceAdd, elemadd eps, pow −0.5, mul weight, elemadd bias ... 평균, 분산, 표준편차를 int8로 양자화하기까지 한다.

**어떻게(`findLayerNorms`).** 그 21명령 모양을 찾아 마지막 명령 자리에서 `nestc_cpu::layerNormI8` 한 번으로 바꾼다. 처음 판은 같은 계산을 한 줄씩 했다(값 동일). 그 뒤 네 행씩(`510463d4`), NEON lane을 행으로(`cc1cc74a`, 1.85 → 0.80 ms) 빨라졌다.

만들어진 bundle(`no_fuse-layernorm` → `all`): `quantize` 100 → 4, `dequantize` 101 → 5. 그리고 layer norm이 하나의 함수가 되어야 §1.12(EVTA)와 §1.14(packed)가 붙을 수 있다(`no_fuse-layernorm`에는 `layernorm_evta2`가 0개).

**리뷰.** 21명령의 정확한 모양에 기댄다. ONNX export가 바뀌어 순서나 상수 모양이 다르면 조용히 합치지 않는다(시간이 되돌아갈 뿐 결과는 맞다).

### 1.7 head 셋을 한 호출로, Q K^T를 conv 출력에서 바로 (11-10, `ab10a797`)

**왜.** head마다 Q, K를 slice 복사하고 NHWC로 transpose한 뒤 `matmul_evta`를 따로 불렀다(12층 × 3 head × 2).

**어떻게.**
- `-vta-matmul-heads`(`findMatMulGroups`): 모양이 같은 head 셋의 matmul을 `matmul_evta_n` 한 호출로 묶는다. vtalib는 `gConvBatch`로 셋의 tile을 한 EVTA run에 넣는다.
- `-vta-attn-keep-layout`(`findAttnKeepLayout`): query / key conv의 출력을 EVTA layout 그대로 두고, `matmul_evta_qk`가 그 buffer에서 head의 view(`VTABufferView`)를 바로 읽는다. head transpose와 slice 복사가 없다.

만들어진 bundle(`no_matmul-heads` → `no_attn-keep-layout` → `all`, layer 0):

```diff
-  matmul_evta(L0_att_MatMul_sliceA_0_reshape__2_res, L0_att_MatMul_sliceB_0_reshape_res, ..., 198, 64, 198, 9, vtaCmdH, 2, 9, 11);
-  ... (one call per head, 66 in all, and the slice copies / transposes before them)
+  matmul_evta_n(3, mmA0, mmB0, mmC0, 198, 64, 198, 9, vtaCmdH, 2, 9, 11);          // 3 heads, one EVTA call
+  matmul_evta_qk(L0_att_query_..._conv1x1_res_vbuf, L0_att_key_..._conv1x1_res_vbuf, mmC0, 3, 198, 64, ...);   // straight from the conv outputs
```

transpose는 97 → 9, flush 27 → 16, `memmove` 147 → 15다(`no_attn-keep-layout` → `all`).

**리뷰.** `e17a8f29`(같은 날): query / key conv 출력의 해제가 `matmul_evta_qk`보다 앞에 있었다. 묶어서 순서를 옮기는 합치기는 "누가 언제 해제하나"를 같이 옮겨야 한다.

### 1.8 softmax / layer norm 역수 곱 (11-10, `42a9a171`, `-vta-cpu-approx`)

`softmaxI8`이 행의 합으로 나누던 것을 1 / sum 곱으로, `layerNormI8`이 분모로 나누던 것을 1 / den 곱으로 바꿨다. **값이 바뀔 수 있는 근사**다(끄면 바이트 동일). 그래서 옵션으로 두었다. 지금은 대부분 EVTA 경로(§1.12, §1.13)라 CPU 대체 경로에만 남는다.

### 1.9 자유 scale, EVTA 비율만 2의 거듭제곱 (11-10, `db968f7b`, `-vta-pow2-ratio`, `VTA.cpp`)

**왜.** power-of-two scale(텐서마다 2^k)로 양자화하면 ImageNetV2 top-1이 56.17 %였다(float 62.34 %). EVTA의 requantize는 shift뿐이라, 필요한 것은 **비율** 입력 × weight / 출력이 2의 거듭제곱인 것이다. 텐서 scale마다 2의 거듭제곱일 필요는 없다.

**어떻게.** Glow는 `-quantization-schema=symmetric`(자유 scale)으로 양자화한다. 그 뒤 VTA backend pass가 int8 Convolution / FullyConnected / MatMul의 출력 scale을 입력 × weight × 2^k로 올림한다.
- 그 뒤의 Reshape / Transpose / Slice / Concat 출력도 같은 scale로 맞춘다. Concat의 상수 입력(cls token)은 다시 양자화하고, attention head들은 함께 바꾼다(`01174f28`).
- 입력이 바뀐 conv의 int32 bias는 다시 양자화한다(`79b6f832`).
- VTASave의 scale 비교를 float 오차를 허용하게 바꿨다(`getExpofPowerofTwo(float)`). 정확 비교라서 7.9999가 −1이 되어 Linear 28개가 CPU로 빠졌었다.

**효과.** 59.71 %(+3.5 %p), 시간은 같다. 만들어진 bundle(`no_pow2-ratio`)은 자유 scale 그대로라 EVTA conv가 0개다(§0.1).

**리뷰.** 출력 scale을 올리면 출력의 정밀도가 최대 1 bit 준다(그 몫이 PC 근사로 −0.15 %p, [deit_optimization.md](deit_optimization.md) §6.6.3).

### 1.10 정수 layer norm (11-10 ~ 11, `1247d0f1`, `4559ece5`, `NESTC_LN_INT`, 기본 1)

**어떻게.** EVTA ALU에서 돌릴 수 있는 정수 식이다(§1.12의 바탕). (x − mean) / sd = (C x − S) / √W, S = Σx, W = C Σx² − S²(C ≤ 1024에서 32 bit 안).
1 / √W는 짝수 정규화 shift(CLZ)와 8 bit 표로 구하고, ((C x − S) × T >> 8) × G[l]에 반올림 shift와 bias를 더한다.

**전 / 후** (`4559ece5`): 처음 판은 bias를 int8 단위로 먼저 반올림했다. b × 비가 .5일 때 출력의 25 %가 1씩 달라져 59.89 %였다. bias를 shift 전의 고정소수점에 더하게(conv bias와 같이) 고쳐 60.29 %가 됐다.

```text
전: out = round(((C x - S) * T >> 8) * G >> s) + round(b * a2b / a2d)         반올림 두 번
후: out = round((((C x - S) * T >> 8) * G + B) >> s), B = 같은 고정소수점의 b   반올림 한 번 (conv bias와 같이)
```

**리뷰.** 1 / √ 표 8 bit로 충분했다(16 bit 보간 `NESTC_LN_INT=2`는 60.30 %). 정확도 영향이 있는 기본값 변경이라 [deit_optimization.md](deit_optimization.md) §6.6.3, §6.10에 단계마다 top-1을 적었다.

### 1.11 GELU 표를 fc1 conv의 출력 LUT로 (11-11 ~ 12, `68199a6c`, `55bad819`, `-vta-alu-output-lut`)

**왜.** §1.4의 GELU 표는 CPU `lut8`(151 KB × 12, 1코어)로 1장에 5.2 ms였다. 그 표는 fc1 conv의 출력에 바로 걸린다.

**어떻게.**
- 컴파일러(`findOutLutConvs`): root가 plain EVTA conv의 출력이고, 그 출력을 그 표만 읽는 영역을 찾는다. conv가 영역의 out에 직접 쓰고, `VTASetOutputLut(T)` ... `VTASetOutputLut(nullptr)`로 감싼다.
- vtalib(§2.3): 표를 acc 0..15에 LOAD하고 LUTSET한 뒤, thread의 STORE 전마다 ALU CPY 하나(conv의 `rq_*`와 `lut_en`)로 표를 지난다.

만들어진 bundle(`no_alu-output-lut` → `all`):

```diff
   convolution_wo_tr(..., L0_intermediate_dense_..., ...);           // fc1
-  transpose_vtaio2nhwc(...);                                        // to NHWC for the CPU table
-  nestc_cpu::lut8(..., lut0, ...);                                  // GELU on the CPU
-  transpose_nhwc2vtaio(...);  VTABufferFlush(...);
+#ifdef VTA_OUTPUT_LUT
+  static const nestc_cpu::Lut8U lut0_u = nestc_cpu::lutByByte(lut0);
+  VTASetOutputLut(VTAHasAluExt() ? lut0_u.v : nullptr);             // the EVTA writes GELU(conv) to out
+#endif
+  convolution_wo_tr(...);
+  if (!VTAHasAluExt()) nestc_cpu::lut8(...);                         // the CPU table without the extended ALU
```

transpose는 33 → 9다. 하나의 bundle이 ALU 확장이 있는 bitstream과 없는 bitstream에서 모두 돈다.

### 1.12 layer norm을 EVTA ALU 프로그램으로 (11-12, `e29ea7cb`, `-vta-alu-layernorm`; 11-14 `cc0e7b96`)

**어떻게.** §1.10의 정수 layer norm을 ALU 명령 프로그램으로 EVTA에서 돈다(§2.4). bundle은 `if (!layernorm_evta2(...)) nestc_cpu::layerNormI8Rows(...)`를 쓴다.

**효과의 굴곡.**
- 11-14 `_alux` 첫 판은 CPU보다 느렸다(EVTA 19.0 ms 대 CPU 8.0 ms). 호출마다 입력 복사 108 us와 명령 만들기 311 us가 들었다.
- `cc0e7b96`에서 replay(프로그램을 한 번 기록하고 다시 돌림), buffer 유지, NEON 입력 복사로 호출당 780 → 400 us, 1장 84.84 → 75.07 ms가 됐다.
- 11-15 `_alux2`의 합친 연산(§3.3)으로 호출당 340 → 약 200 us다.

### 1.13 softmax를 EVTA ALU 프로그램으로 (11-12, `ecf72a0a`, `-vta-alu-softmax`; 11-17 기본 켬)

**어떻게.** float softmax 뒤의 quantize까지 함께, 정수 softmax(8 bit exp 표, CLZ + 역수 표, 2^16 확률)와 정수 quantize로 바꾼다(`nestc_cpu::softmaxIntQ`, EVTA는 `softmax_evta`, §2.5).

만들어진 bundle(`no_alu-softmax` → `all`):

```diff
-  nestc_cpu::softmaxI8(L0_att_Softmax_res, L0_att_Div_res, 594, 198, 0.105531611f, 1);   // float softmax over the int8 scores (CPU)
+  if (!softmax_evta(L0_att_MatMul_1_reshapeLHS3D__1_quantize_res, L0_att_Div_res, 594, 198, 0.105531611f, 127.115198f, vtaCmdH))
+    nestc_cpu::softmaxIntQ(...);                                              // the same integers on the CPU
```

**리뷰.**
- 정수 softmax는 float와 마지막 bit가 다르다. 11-11 ImageNetV2 앞 9,000장이 −0.35 %p(60.29 → 59.94 %, 표준오차 약 ±0.5 %p)라 처음에는 기본 꺼짐이었고, 11-17에 기본으로 켰다([deit_optimization.md](deit_optimization.md) §6.10).
- **ALU 확장이 없는 bitstream에서는 대체 경로 `softmaxIntQ`가 36.9 ms로 매우 느리다**(11-22 `_rq` 83.18 ms의 가장 큰 몫). 대체 경로로만 만들어 다듬지 않았다. 확장 없는 판에서는 `-vta-alu-softmax=0`으로 만든 bundle을 써야 한다.

### 1.14 행 × 채널 활성값을 EVTA layout으로 (11-15, `4d33db3e`, `-vta-pack-rows`)

**왜.** residual stream, layer norm 출력, out-proj / fc2 출력은 rows × C(198 × 192)다. CNN의 `-vta-keep-layout`(4차원만)에 걸리지 않아, conv마다 transpose가 121개 있었다.

**어떻게(`findPackedRows`).** C % 16 = 0인 2 / 3차원 int8 rows × C를 EVTA layout [C/16][rows][16]으로 둔다(H × W = rows인 conv layout). 다른 pass가 끝난 뒤에 돌고, 아직 packed가 아닌 것만 본다.
- vtalib `layernorm_evta2`가 그 layout으로 읽고 쓴다(CPU 대체 `nestc_cpu::layerNormI8Rows`).
- layout이 섞인 add는 `nestc_cpu::elemaddRows`다.

만들어진 bundle(`no_pack-rows` → `all`): transpose 106 → 9, flush 61 → 16. `no_pack-rows`에서는 layer norm이 CPU `layerNormI8`(24)로, add가 CPU `elemadd`(28)로 간다. 59.82 → 49.93 ms였다.

**리뷰.** `no_keep-layout`도 `no_pack-rows`와 같은 호출 수다. pack-rows가 keep-layout의 packed 집합 위에서만 돌기 때문이다(그 옵션을 끄면 둘 다 빠진다).

### 1.15 residual add를 EVTA 정수 add로 (11-15, `f2dc32bd`, `-vta-evta-elemadd`)

**어떻게(`elemAddIntParams`).** 세 operand가 모두 packed인 int8 add를 정수 식 clip(round((a A + b B) >> N))로 쓴다. A, B는 15 bit(`-vta-elemadd-precise`면 22 bit)다. vtalib `elemadd_evta`(§2.6)가 EVTA에서, `nestc_cpu::elemaddInt`가 CPU에서 같은 바이트를 낸다.

만들어진 bundle(`no_evta-elemadd` → `all`):

```diff
-  elemadd(L0_attention_output_dense_MatMul_MatMul_0_conv1x1_res__2, 1.0/31.836929, 0, embeddings_Add_res, 1.0/9.123440, 0,
-          L0_Add_res, 1.0/11.196388, 0, 38016);                              // CPU float add
-  VTABufferFlush(L0_Add_res_vbuf, 0, 38016);
+  if (!elemadd_evta(L0_Add_res_vbuf, L0_attention_output_dense_MatMul_MatMul_0_conv1x1_res_vbuf, embeddings_Add_res_vbuf,
+                    38016, 737536, 2573696, 21, vtaCmdH))                     // n, A, B, N
+    nestc_cpu::elemaddInt(...);
```

24개 add가 4.36 → 1.44 ms, 1장 49.85 → 47.01 ms였다. 정확도는 60.20 %(float add 59.94 %, 잡음 안)였다.

**리뷰.** 같은 add를 out-proj / fc2 conv의 epilogue로 넣는 `-vta-fuse-elemadd-mul`(11-15)은 같은 바이트지만 더 느려 기본 꺼짐이다. bundle 머리의 "24 x an add ... not fused"가 그 표시다.

### 1.16 – 1.20 (11-16 ~ 17)

| § | 기능 | 전 → 후 | 핵심 | 자세히 |
|---|---|---|---|---|
| 1.16 | attention `Div` 표를 Q K^T의 출력 LUT로(`findOutLutMatMuls`) | 46.17 → 44.21 | §1.11과 같은 `VTASetOutputLut`를 `matmul_evta_qk` 둘레에. head를 region의 out에 직접 씀. vtalib가 conv batch에서 표만 있는 epilogue를 돌게 함(그 전 `gConvBatch`는 epilogue가 있으면 꺼져 첫 head만 계산됐다) | isa_ext §8.22 |
| 1.17 | 마지막 블록은 2행만(`prune_rows_onnx.py`) | 44.28 → 41.07 | 출력이 읽는 행만 뒤에서부터 따라가 ONNX에 Slice를 넣는다. key / value는 198행 그대로. 모델 전체 profile + Slice 항목 | [deit_optimization.md](deit_optimization.md) §6.7 |
| 1.18 | A x V에서 V와 출력을 EVTA layout 그대로(`matmul_evta_av`) | 41.00 → 37.82 | V를 value conv 출력에서 읽어 16 × 16 전치로 filter 묶음으로, head를 out-proj conv 입력의 view에 씀 | §6.8 |
| 1.19 | patch embedding add를 conv epilogue로(`findEmbedFusions`) | 37.79 → 37.35 | conv → 전치 → token concat의 행 → + 위치 임베딩을 conv epilogue(정수 add)로. 바이트가 바뀜(60.20 → 60.13 %) | §6.9 |
| 1.20 | packed 텐서의 앞 행 꺼내기(`unpackRowsPart`), softmax 기본 켬 | 37.37 → 37.03 | 행 slice view를 꺼낼 때 원본을 packed로 둔 채 앞 행만 복사 | §6.10 |

만들어진 bundle에서 1.18은 이렇게 보인다(`no_attn-keep-layout` → `all`, A V):

```diff
-  matmul_evta_n(3, mmA1, mmB1, mmC1, 198, 198, 64, 7, vtaCmdH, 2, 18, 11);        // V sliced / transposed per head before it
+  matmul_evta_av(mmA1, L0_att_value_MatMul_MatMul_0_conv1x1_res_vbuf, L0_att_Transpose_3__5_res_vbuf, 3, 198, 198, 64, 7,
+                 vtaCmdH, 2, 18, 11);                                             // V and the output in the EVTA layout
```

1.19는 `no_fuse-embed`에서 transpose 11 → 9다. 1.17은 모델 파일이 달라 옵션으로 끈 bundle이 없다(`PRUNE_ROWS=0`).

**리뷰(1.17).** ONNX 그래프를 손보는 단계라, 모델 모양이 바뀌면(token 수, head 수) 스크립트도 다시 봐야 한다. 출력 hash 2,000장이 같은지(`EVAL_HASH=1`)로 확인했다. 이 도구가 없으면 틀려도 모른다.

## 2. vtalib (`vtalib-zcu102-systolic/lib/Bundle/VTABundle.cpp`)

### 2.1 `matmul_evta` (11-09, `996d769f`, `031c9403`; `VTABundle.cpp:5012`)

**계산.** C[M][N] = clip(floor((A[M][K] B[K][N] + 2^(shift−1)) / 2^shift)).
- M 행을 H × W 그림(§1.2와 같은 W)으로, A를 입력으로, B^T를 filter로 보는 1 × 1 conv다. K와 N은 16의 배수로 0 채움이다.
- 묶기와 풀기는 CPU다. `031c9403`에서 B^T를 NEON 16 × 16 전치로, buffer를 thread마다 유지하게 했다(`EVTA_MATMUL_TIME=1`이 나눠 보여 준다). `d435508d`는 출력을 순서대로 읽어 푼다.
- `EVTA_MATMUL_CHECK=1`이면 매 출력을 CPU 정수 결과와 비교한다.

**그 뒤.** `matmul_evta_n`(head 묶음, `gConvBatch`), `matmul_evta_qk`(Q, K를 conv 출력의 view에서, `:5108`), `matmul_evta_av`(V를 conv 출력에서, `:5022`)로 늘었다. 셋 모두 같은 `convolution_wo_tr_impl`을 부른다.

**리뷰.**
- `VTABufferView`(buffer의 [offset, ...) 부분을 가리키는 handle)는 메모리를 나눠 쓴다. view가 살아 있는 동안 원본을 해제하면 안 된다. 해제 순서는 컴파일러(§1.7 리뷰)가 맞춘다.
- `gConvBatch`가 켜져 있을 때 epilogue가 있으면 꺼지던 결함(11-16, 첫 head만 계산, 오류 없음)처럼, 묶음 경로에서 틀리면 조용하다. `EVTA_MATMUL_CHECK`가 유일한 확인이다.

### 2.2 vt 결함: virtual thread가 출력 묶음 수를 나누지 못하면 (11-10, §7.28)

DeiT schedule(virtual thread 2)을 넣다가 찾았다. 출력 묶음이 13개(208 / 16)일 때 vt 2는 **아무것도 계산하지 않았다**(오류 없음). 두 vtalib 모두 그때는 thread 1로 돌게 고쳤고, matmul N도 그것에 맞춰 0 채운다(`matmulSched`).

### 2.3 conv 출력 LUT (11-11, `68199a6c`, `VTASetOutputLut`, `VTABundle.cpp:684`)

**전 / 후** (`conv_kernel`, `_alux` bitstream일 때만; 줄인 의사 코드):

```diff
+  //#### an elementwise int8 -> int8 table on the output of the plain EVTA convs (GELU after fc1)
+  if (gOutLut) {                                     // VTASetOutputLut(T)
+    LOAD T into acc 0..15;  ALU LUTSET (once per call)
+  }
   ... GEMM tiles ...
+  if (gOutLut)                                       // before each thread's STORE
+    VTAAluLut(cmd, 1);  ALU CPY (dst = src) over the tile with the conv's rq_* ;  VTAAluLut(cmd, 0);
   STORE
```

GEMM은 acc를 그대로 두고, CPY가 requantize와 표를 지나 out에 쓴다. 시뮬레이터에서 CPU 표와 바이트 단위로 비교했다(`tests/run_lut_conv_test.sh`).

### 2.4 `layernorm_evta` / `layernorm_evta2` (11-12, `e29ea7cb`; 11-14 `cc0e7b96`; 11-15 `81913204`; `VTABundle.cpp:6701`)

**계산.** n token씩:
1. token을 int32로 acc에 LOAD한다. 11-15부터는 int8을 inp에 LOAD하고 identity GEMM으로 acc에 옮긴다(`AluInp`, 다음 LOAD가 ALU와 겹친다; `EVTA_ALU_INP=0`이면 예전처럼 int32).
2. ALU 프로그램: S = RSUM(x), Q = RSUM(x²)(3단계부터 SQACC), W = C Q − S², CLZ로 정규화 shift, LUT로 1 / √, (C x − S) × T, × G, + B, 반올림 shift
3. int8 행을 out에서 STORE한다.

상수(G, B, rsqrt 표)는 gamma 포인터마다 한 번 만들어 LOAD하고, 표는 호출마다 한 번 LUTSET한다.

**전 / 후** (`cc0e7b96`, 호출마다의 CPU 일; 줄인 의사 코드):

```diff
-  // every call: copy x (int8 -> int32, scalar), emit every ALU instruction, VTASynchronize      (108 + 311 us)
+  // buffers kept per (EVTA, rows, cols); NEON int8 -> int32; the program recorded once and replayed
+  int32_t key[] = {..., rows, cols, gs};
+  if (!VTAReplayBegin(cmd, key, sizeof(key), bufs, nb)) { emit the program; VTASynchronize(cmd, 1 << 31); VTAReplayEnd(cmd, 1); }
```

**어떻게 맞는지.** `EVTA_LN_CHECK=1`(매 호출 CPU와 비교), 11-15 2,400번 비교 0 differ. 프로그램은 먼저 `tests/alu_ext_test.cc`에서 참조 모델(`alu_ext_ref.h`)과 HLS ALU(C 시뮬레이션)에 대해 CPU 함수와 맞췄다.

### 2.5 `softmax_evta` (11-12, `ecf72a0a`; 11-15 `81913204`; `VTABundle.cpp:6866`)

**계산.** 행마다 정수 softmax와 quantize. 행은 16의 묶음으로 채우고, 채운 칸은 아주 작은 점수(exp가 E[255] = 0)다.

**전 / 후.** 한 함수 안에서 bitstream에 따라 두 프로그램을 고른다. 1단계(`_alux`)는 한 연산씩이고, 2 / 3단계(`fused`)는 합친 연산을 쓴다.

```cpp
// 1. m = max x, d = min(m - x, 255), e = E[d]
I(VTA_ALUX_RMAX, n, 1, Mx, 1, 0, Mx, 1, 0);
if (fused) {
  I(VTA_ALUX_MADD, n, NB, X, NB, 1, Mx, 1, 0, false, -1);   // x * -1 + m          (_alux2)
  if (fused > 1) I(VTA_ALUX_LUTC, n, NB, X, NB, 1, 0, 0, 0, true, 0);   // clamp + table in one   (_alux3)
  else { I(RES_ALU_MIN, ..., 255); I(VTA_ALUX_LUT, ...); }
} else {                                                     // _alux: CPY, SUB, MIN, LUT
  I(VTA_ALUX_CPY, n, NB, D, NB, 1, Mx, 1, 0);
  I(VTA_ALUX_SUB, n, NB, D, NB, 1, X, NB, 1);
  I(RES_ALU_MIN, n, NB, D, NB, 1, 0, 0, 0, true, 255);
  I(VTA_ALUX_LUT, n, NB, D, NB, 1, 0, 0, 0, true, 0);
}
// 2. S = sum e; R = RT[S >> (24 - clz(S))]          (fused: SHRV by a per-row shift; else SHR twice)
// 3. p = (e * R + 2^(t-1)) >> t                      (fused: MUL + SHRR; else CPY, ADD, CPY, SUB, CPY, SHR, MUL, ADD, SHR)
// 4. q = clip((p * M + 2^(k-1)) >> k)                VTARequant + one MUL by M into out
```

11-15에 replay, buffer 유지(`EVTA_SM_TIME=1`), int8 입력(§2.4의 `AluInp`)도 들어갔다. 1장 71.73 → 60.55 ms(`_alux2`)였다.

**어떻게 맞는지.** `EVTA_SM_CHECK=1`, 11-15 1,200번 0 differ.

### 2.6 `elemadd_evta` (11-15, `f2dc32bd`, `VTABundle.cpp:6721`)

**계산.** out = clip(round half away((a A + b B) / 2^N)). 256 entry(16 byte)씩:
1. a와 b를 inp 반쪽에 LOAD하고, reset + identity GEMM으로 acc X / Y에 옮긴다.
2. ALU의 곱셈기가 16 bit라 A = Ah 2^7 + Al로 나눈다. Y = b Bh + a Ah(MUL, MADD), 그 뒤 low 부분을 더하고, requantize(round 2, shift N, clip)로 out에 쓴다.

**어떻게 맞는지.** `EVTA_EA_CHECK=1`. 그리고 CPU `nestc_cpu::elemaddInt`와 같은 바이트다.

### 2.7 CPU 쪽 NEON (11-09 ~ 10, `c58fd7ec`, `e718cd47`, `2a45a12d`)

| 함수 | 전 | 후 |
|---|---|---|
| `quantize` / `dequantize` / `elemadd` / `elemdiv` / `elemsub` | 스칼라 | offset 0일 때 NEON, 바이트 동일 |
| 4차원 int8 `transpose` | 원소 하나씩 | 가장 안쪽이 이어지면 그 run을 복사, 아니면 NEON 16 × 16 2D 전치 |
| pow 2 | `pow` | `nestc_cpu::square` |
| `reduceAdd` | 한 행씩 | 네 행씩 |
| `quantize`(2의 거듭제곱 아닌 scale, 11-11 `c6ec6b91`) | 나눗셈(3 → 7 ms) | 1 / scale 곱(`EVTA_QUANT_EXACT=1`이면 나눗셈; 다른 결과는 원소 약 10⁻⁶) |

## 3. 하드웨어: ALU 확장 (`vta-hw-zcu102-systolic/hardware/xilinx/src/vta.cc`)

원래 VTA ALU 명령은 MIN / MAX / ADD / SHR 네 가지(2 bit opcode)다. DeiT의 layer norm, softmax, GELU에는 곱, 표, 행 합 / 최댓값, CLZ가 필요하다. 회로(데이터 경로, LUT 메모리 bank, 파이프라인 register, 자원)는 [evta_alux_hw.md](evta_alux_hw.md)에 그림과 함께 있다.

### 3.1 1단계: 연산 4–11과 `lut_en` (11-11, `f847659b`, `e6df7fa7`; Makefile `ALUX=1`, `_alux`)

**ISA(전 / 후, `hw_spec.h`).** pad bit를 썼다. 0이면 예전 명령이므로 옛 vtalib의 명령이 그대로 돈다.

```diff
 // VTAAluInsn
-  uint64_t pad_0          : 8;
+  uint64_t alu_op_ext     : 2;      // operation = alu_op_ext << 2 | alu_opcode (4 bits)
+  uint64_t lut_en         : 1;      // the int8 written to out goes through the LUT (after rq)
+  uint64_t pad_0          : 5;
 // VTAGemInsn
-  uint64_t pad_0          : 7;
+  uint64_t lut_en         : 1;      // reserved (done as an ALU pass, §2.3)
+  uint64_t pad_0          : 6;
+#define VTA_ALUX_MUL 4     /* dst = dst * (int16) src            */
+#define VTA_ALUX_SUB 5     /* dst = dst - src                    */
+#define VTA_ALUX_CPY 6     /* dst = src                          */
+#define VTA_ALUX_LUT 7     /* dst = T[dst & 255]                 */
+#define VTA_ALUX_LUTSET 8  /* T[16 * (imm + step) + lane] = src  */
+#define VTA_ALUX_RSUM 9    /* dst[every lane] = sum of the lanes */
+#define VTA_ALUX_RMAX 10   /* dst[every lane] = max of the lanes */
+#define VTA_ALUX_CLZ 11    /* dst = leading zeros of (uint32) dst */
```

**HLS(전 / 후, `alu()`).** imm II 1 루프(Resnet18 일, [resnet18_code_review.md](resnet18_code_review.md) §3.1)는 원래 네 연산만 받게 하고, 확장 연산은 모두 vector 루프의 `alux_step`으로 보낸다.

```diff
 #ifdef VTA_SYSTOLIC
+#ifdef VTA_ALUX
+  if (insn.use_imm && op < 4 && !insn.lut_en) {     // only MIN / MAX / ADD / SHR without lut_en: the II 1 imm loop
+#else
   if (insn.use_imm) {
+#endif
 ...
         // the vector loop (II 2)
+#ifdef VTA_ALUX
+        alux_step(af, op, lut_entry, dst_tensor, src_tensor, o_tensor);   // every operation, rq, lut_en
+        lut_entry++;
+        dst / o through alu_reg;  if (op != VTA_ALUX_LUTSET) write acc, out
+#else
         for (i, b) alu_lane(insn.alu_opcode, ...);       // the 4 original operations
+#endif
```

- 확장 연산의 데이터 경로는 한 곳(vector 루프)에만 있다. 두 루프 모두에 두면 C 합성에서 LUT 42.6k → 82.0k였다.
- LUT 메모리는 lane마다 복사본, 16 bank의 LUT RAM이다(`alux_lut_d`, `alux_lut_o`).

**어떻게 맞는지.** 참조 모델 `alu_ext_ref.h`(vtalib 시뮬레이터에도 들어감)와 무작위 프로그램 C 시뮬레이션(`sim/run_csim.sh`), layer norm / softmax 프로그램(`PROG=1`), 11-14 C/RTL cosim 20 seed PASS(`sim/run_cosim.sh`).

**리뷰.** vector 루프는 깊이가 있어(II 2, 깊이 33) **한 step이 앞 17 step 안에서 쓴 entry를 읽으면 안 된다**(hazard). 하드웨어는 검사하지 않는다. `alu_ext_ref.h` `hazards()`, `tests/hazard_dump.py`(replay dump 전체 검사), 시뮬레이터 `EVTA_ALU_HAZARD=17`로 소프트웨어가 지킨다. 다섯 모델 모두 위반 0이다.

### 3.2 400 MHz를 위한 register (11-14, `7a128c5b`)

`_alux`가 400 MHz에서 WNS −0.055 ns였다. 경로마다 register를 넣었다.
- `vta.cc`: uop 읽기와 주소 덧셈 뒤, operand, 연산 결과, requantize 덧셈 뒤(`requant_p`), lut_en 읽기 전(`alu_reg`, `#pragma HLS INTERFACE ap_none port=return register`로 FF 한 단을 강제)
- `sa_rmw.v`: forward 비교를 한 단 앞당김(RMW 3 → 4 cycle, 여전히 forward하므로 GEMM 프로그램 규칙은 그대로)
- `vivado.tcl`: 380 MHz 위에서는 명령 queue 뒤에 AXIS register slice(`VTA_QUEUE_RS`)

```diff
-  out_T o = requant(r, (out_T) r.range(VTA_OUT_WIDTH - 1, 0), f.rq_en, f.rq_round, f.rq_shift, f.rq_lo, f.rq_hi);
+  r = alu_reg(r);
+  out_T o = requant_p(r, (out_T) r.range(VTA_OUT_WIDTH - 1, 0), f.rq_en, f.rq_round, f.rq_shift, f.rq_lo, f.rq_hi);
+  o = alu_reg(o);
```

**리뷰.** vector 루프 깊이가 33 → 39가 됐다. 그래도 hazard 창은 16 step 그대로다(읽기와 쓰기가 함께 밀림). cosim의 `VTA_ALUX_DIST`로 실제 창을 재서 확인했다.

### 3.3 2단계: slot 루프 II 1, 합친 연산 (11-15, `c50e9688`; `_alux2`)

**왜.** vector 루프는 II 2(step마다 src와 dst를 읽음)다. 그런데 layer norm / softmax 프로그램의 step 대부분은 operand 하나만 읽거나, 행마다 같은 src(행별 상수)를 읽는다.

**어떻게.** 한 slot에 acc 읽기 하나인 II 1 루프를 만든다(acc_mem은 읽기 하나, 쓰기 하나 포트).
- operand 하나만 읽는 step(imm, CPY / RSUM / RMAX / LUTSET, CLZ / LUT)은 slot 하나
- 둘을 읽으면 slot 둘(src 다음 dst)
- uop 하나에 `src_factor_in` 0(행 안에서 src가 같음, "broadcast")이면 src를 안쪽 루프마다 한 번만 읽는다(`iter_in + 1` slot)
- 합친 연산 12–15: MADD(x × imm + src), MULSHR((x × src) >> imm), SHRV(src만큼 shift), SHRR(반올림 shift). layer norm의 원소별 사슬이 한 명령씩으로 준다.

```diff
+#if defined(VTA_ALUX) && !defined(VTA_ALUX_II2)
+  const bool two = rd_src && rd_dst;
+  const bool bc = two && nu == 1 && insn.src_factor_in == 0;                   // a per-row src: read once per inner loop
+  const int per_out = bc ? insn.iter_in + 1 : (two ? 2 : 1) * insn.iter_in * nu;
+  ALU_SLOT: for (int t = 0; t < insn.iter_out * per_out; t++) {
+#pragma HLS PIPELINE II = 1
+    const bool fetch = bc ? first : (two && !ph);                               // this slot reads src into s_reg
+    acc_idx_T a = at_src ? src index : dst index;
+    read_tensor(a, acc_mem, rd_tensor);                                         // one acc read per slot
+    if (fetch) s_reg = rd;  else { alux_step(af, op, lut_entry, dst = rd, src = two ? s_reg : rd, o); write acc, out; }
+    ... next slot / uop / inner loop ...
+  }
+#endif
```

`VTA_ALUX_II2`면 예전 루프다. MUL / MADD / MULSHR는 lane마다 곱셈기 하나를, SHRV / SHRR는 shifter 하나를 나눠 쓴다.

**효과.** LUT +12.5k, FF +16.4k. layer norm 호출당 340 → 약 200 us, 1장 76.14 → 71.73 ms.

**리뷰.** hazard 규칙이 step 단위에서 **slot 34개** 단위로 바뀌었다(`hazard_slots`, 시뮬레이터 `EVTA_ALU_SLOTS=34`). broadcast src는 안쪽 루프에서 쓰는 명령이면 안 된다(C 시뮬레이션이 그런 명령을 버린다). 지킬 곳이 vtalib 프로그램뿐이다.

### 3.4 3단계: SQACC, LUTC (11-15, `81913204`; `_alux3`)

```diff
 // hw_spec.h
-#define VTA_ALU_FULL_OP(insn) ((int)((insn).alu_op_ext << VTA_ALU_OPCODE_BIT_WIDTH | (insn).alu_opcode))
+#define VTA_ALUX_SQACC 16  /* dst = dst + (int16) acc[src] * (int16) acc[src] */
+#define VTA_ALUX_LUTC 17   /* dst = T[dst clamped to 0..255]                  */
+#define VTA_ALU_FULL_OP(insn) \
+  ((int)((insn).alu_op_ext2 << 4 | (insn).alu_op_ext << VTA_ALU_OPCODE_BIT_WIDTH | (insn).alu_opcode))
-  uint64_t pad_0          : 5;
+  uint64_t alu_op_ext2    : 1;
+  uint64_t pad_0          : 4;
 // vta.cc alux_step
-      const ap_int<16> m = op == VTA_ALUX_MADD ? (ap_int<16>) imm.range(15, 0) : (ap_int<16>) (op == VTA_ALUX_MULSHR ? sv : y).range(15, 0);
-      const acc_T p = (acc_T) (x * m);
+      const bool sq = op == VTA_ALUX_SQACC;                                    // a mux before the multiplier
+      const ap_int<16> m = op == VTA_ALUX_MADD ? (ap_int<16>) imm.range(15, 0)
+                                               : (ap_int<16>) (op == VTA_ALUX_MULSHR || sq ? sv : y).range(15, 0);
+      const acc_T xa = sq ? (acc_T) (ap_int<16>) sv.range(15, 0) : x;
+      const acc_T p = (acc_T) (xa * m);
-      } else if (op == VTA_ALUX_MADD) {
-        r = (acc_T) (p + sv);
+      } else if (op == VTA_ALUX_MADD || sq) {
+        r = (acc_T) (p + (sq ? x : sv));
-      } else if (op == VTA_ALUX_LUT) {
-        ap_uint<8> v = alux_lut_d[c][x.range(3, 0)][x.range(7, 4)];
+      } else if (op == VTA_ALUX_LUT || op == VTA_ALUX_LUTC) {
+        const ap_uint<8> li = op == VTA_ALUX_LUTC && x < 0 ? 0 : op == VTA_ALUX_LUTC && x > 255 ? 255 : x.range(7, 0);
+        ap_uint<8> v = alux_lut_d[c][li.range(3, 0)][li.range(7, 4)];
```

- SQACC는 layer norm의 Σx²를, LUTC는 softmax의 min(d, 255) + 표를 한 명령으로 만든다(§2.5의 `fused > 1`).
- 곱셈기 앞 mux 하나와 표 주소 clamp만 더한다.
- vtalib는 bitstream 이름의 `_alux<k>`(k ≥ 3)로 켠다(`VTAHasAluExt3`, `EVTA_ALUX3=0|1`).

**효과.** 60.82 → 60.14 ms(OMP 4 51.77 → 51.12). 그 뒤의 소프트웨어(§1.14 ~ 1.20)가 모두 이 bitstream 위에서 했다.

**리뷰.** multi4 bitstream에는 2 / 3단계가 없다(EVTA 4개에 넣으면 287.9k LUT로 들어가지 않음, [evta_multi4_alux3.md](evta_multi4_alux3.md)). 그래서 multi4 DeiT는 layer norm / softmax가 CPU로 가고, copy 하나가 single보다 훨씬 느리다([deit_optimization.md](deit_optimization.md) §6.5).

## 4. 확인 도구 (이 일에서 만든 것)

| 도구 | 하는 일 |
|---|---|
| `EVTA_MATMUL_CHECK=1` | 매 `matmul_evta*` 출력을 CPU 정수와 비교(Q K^T는 출력 LUT까지) |
| `EVTA_LN_CHECK` / `EVTA_SM_CHECK` / `EVTA_EA_CHECK` / `EVTA_EPI_CHECK=1` | layer norm, softmax, elemadd, epilogue를 매 호출 CPU와 비교 |
| `EVTA_LN_TIME` / `EVTA_SM_TIME` / `EVTA_MATMUL_TIME=1` | 호출의 CPU 몫(복사, 명령 만들기, 기다림)을 나눠 보여 줌 |
| `alu_ext_ref.h` + `tests/alu_ext_test.cc` | ALU 확장의 참조 모델과 layer norm / softmax 프로그램 시험(시뮬레이터) |
| `sim/run_csim.sh`(`DWU`, `PROG=1`), `sim/run_cosim.sh` | HLS ALU의 C 시뮬레이션, C/RTL cosim(`VTA_ALUX_DIST`로 hazard 창 재기) |
| `tests/hazard_dump.py`, 시뮬레이터 `EVTA_ALU_HAZARD` / `EVTA_ALU_SLOTS` | 기록된 모든 ALU 명령의 hazard 검사 |
| `imagenet_eval/`(`EVAL_LIST`, `EVAL_HASH=1`, `fakequant.py`) | 보드 ImageNetV2 top-1, 출력 hash 비교, PC fake-quant 근사 |

## 5. 리뷰 의견 모음

| # | 어디 | 의견 | 심각도 |
|---|---|---|---|
| 1 | ALU 확장 hazard(§3.1, §3.3) | 하드웨어가 검사하지 않는다. vector 루프 17 step, slot 루프 34 slot, broadcast src 규칙을 vtalib 프로그램이 지켜야 한다. 새 ALU 프로그램을 넣을 때 `hazard_dump.py`를 돌리는 단계를 시험 스크립트에 넣기를 권한다 | 중 |
| 2 | `-vta-alu-softmax`의 대체 경로 | ALU 확장이 없는 bitstream(`_rq`, multi4)에서는 `softmaxIntQ`가 36.9 ms다. bitstream에 맞는 bundle을 고르는 규칙이 없다. 대체 경로를 다듬거나, 확장 없는 대상에는 `=0` bundle을 만드는 것을 권한다 | 중 |
| 3 | 컴파일러 합치기의 사슬(§0.1 표) | 앞 옵션을 끄면 뒤 합치기가 조용히 빠진다(fold-matmul-bias → head 묶음, 출력 LUT). 모양 의존이 깊어, 다른 transformer 모델에서는 `VTA_FUSION_LOG`를 먼저 봐야 한다 | 중 |
| 4 | `gConvBatch` / `VTABufferView` | 묶음 경로의 결함은 오류 없이 일부만 계산한다(11-16 epilogue, 11-10 vt). `EVTA_MATMUL_CHECK` 같은 비교 변수가 유일한 확인이다. CI 같은 정기 비교가 없다 | 중 |
| 5 | `findLayerNorms` | Glow의 21명령 모양에 기댄다. export가 바뀌면 합치지 않는다(느려질 뿐, 결과는 맞음) | 낮 |
| 6 | `prune_rows_onnx.py` | ONNX를 손본다. 모델이 바뀌면 다시 봐야 한다. 출력 hash 비교가 없으면 틀려도 모른다 | 낮 |
| 7 | `-vta-pow2-ratio` | 출력 scale을 올려 정밀도 최대 1 bit를 쓴다(−0.15 %p). `rq_mul`이 생기면 필요 없다 | 낮 |
| 8 | CPU 함수가 bundle 안과 vtalib 두 곳 | `matmulI8`, `layerNormI8`, `elemaddInt`의 CPU 판이 bundle(`nestc_cpu`)에, EVTA 판이 vtalib에 있다. 같은 바이트를 내야 하는데 두 파일을 함께 고쳐야 한다 | 낮 |
| 9 | 바이트가 바뀐 기본값(정수 LN, 정수 softmax, embed add) | 단계마다 top-1을 쟀다([deit_optimization.md](deit_optimization.md) §6.10). 다음 바꿈도 같은 기록을 남겨야 한다 | 정보 |
| 10 | 마지막 블록 2행의 행렬곱 6개 | W ≥ 7을 못 만들어 CPU다(합 0.2 ms 안팎). EVTA matmul이 작은 M을 받으면 없어진다 | 정보 |
