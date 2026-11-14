# ALU 확장으로 CPU 일을 EVTA로: 지금까지와 conv epilogue 구조 (2026-11-14)

방향: **CPU에서 하는 일을 거의 다 EVTA로 보낸다.** NEON CPU 코드는 비교용으로만 두고 기본값으로 쓰지 않는다.
모델마다 구조가 조금씩 다르므로 "conv + add + relu" 같은 패턴별 융합을 계속 늘리지 않는다. 대신 backend가 conv 뒤의 원소별
연산을 **명령열(epilogue 프로그램)** 로 만들고, vtalib이 그것을 tile마다 ALU / GEMM 명령으로 바꿔 STORE 전에 실행한다.

- ISA 비트, 연산 표, 하드웨어 비용, HLS / cosim: [evta_isa_ext.md](evta_isa_ext.md) §8
- 이 문서: ALU 확장을 모델에 쓴 곳(§1), epilogue 구조(§2–§5), 검증(§6), 남은 일(§7)

## 1. 지금까지 ALU 확장으로 EVTA에 옮긴 것

| 연산 | 모델 | 어떻게 | 켜는 것 | 자세히 |
|---|---|---|---|---|
| GELU (fc1 뒤 표) | DeiT | conv 출력에 표: ALU CPY + rq + `lut_en` (tile마다 한 번) | VTASave `-vta-alu-output-lut` | isa_ext §8.9, §8.10 |
| LayerNorm | DeiT | 정수 LayerNorm ALU 명령열 (`layernorm_evta`) | `-vta-alu-layernorm` | §8.10 |
| softmax | DeiT | 정수 softmax ALU 명령열 (`softmax_evta`) | `-vta-alu-softmax` (기본 꺼짐, 정확도 −0.35 %p) | §8.10 |
| global avgpool | Resnet18 / 50, MobileNetV2 | identity GEMM + ALU ADD + MUL 한 번 + rq (`avgpool_evta`) | `-vta-evta-avgpool` | §8.12 |
| relu 없는 residual add | MobileNetV2 | conv 융합, rq round 2 (half away from zero) | `-vta-fuse-residual-norelu` | §8.12 |
| **conv epilogue (일반 구조)** | 모든 EVTA conv | 아래 §2–§5 | `-vta-conv-epilogue` | 이 문서 |

- 비트스트림: `..._sa_alux_380mhz_rq.bin`(이름에 `_alux`와 `_rq`). 같은 실행 파일이 확장 없는 비트스트림에서도 돈다.
  vtalib이 `/tmp/evta_bitstream`의 이름을 보고 못 하는 부분은 CPU로 한다.
- CPU에 남은 일(11-14 기준): conv0 입력 전처리(약 0.4 ms), cache invalidate(MobileNetV2 0.48 ms, Resnet50 0.79 ms), 마지막 dequantize /
  softmax, DeiT의 layout transpose / quantize 일부.

## 2. 왜 epilogue 구조인가

전에는 conv 뒤에 붙일 수 있는 것이 **정해진 패턴**이었다.
- `convolution_wo_tr_res`: add → (relu 하나)
- `VTASetOutputLut`: 표 하나(일반 conv, NHWC 출력만)
- `VTASetActivationClip`: ReLU6

그래서 다음 같은 것은 EVTA에서 못 했다.
- add 뒤의 relu가 아닌 activation(표)
- add 앞의 activation
- add 뒤의 ReLU6
- 두 relu

모델마다 새 함수와 인자를 만들어야 했다. epilogue에서는 순서가 자유인 단계들의 목록 하나로 이것을 모두 표현한다.
vtalib이 그 목록을 그때의 비트스트림에 맞는 명령으로 바꾼다.

## 3. 프로그램 형식 (vtalib `VTABundle.h`, 두 vtalib 같음)

```c
enum { VTA_EPI_ADD = 1, VTA_EPI_RQ = 2, VTA_EPI_CLIP = 3, VTA_EPI_SHL = 4, VTA_EPI_LUT = 5 };
struct VTAEpiStep { int32_t op, a, b, c, d; };
struct VTAEpilogue { int32_t n; VTAEpiStep s[8]; void *t[2]; const int8_t *lut[2]; };
void VTASetConvEpilogue(const VTAEpilogue *e);                       // 다음 convolution_wo_tr들에 적용, nullptr = 끔
int8_t VTAEpilogueRef(const VTAEpilogue *e, int8_t x, const int8_t *tv);  // 한 출력의 CPU 값 (기준)
```

v는 conv 자신의 requantize(shift, relu, ReLU6) 뒤의 int8 출력에서 시작해 단계를 차례로 지난다. 마지막 값은 int8이다.

| 단계 | 인자 | 식 |
|---|---|---|
| ADD | a = 텐서 번호, b, c | v = (v << b) + (t_a << c). t_a는 conv 출력과 같은 모양의 EVTA 배치 VTA 버퍼 |
| RQ | a = round, b = shift, c = lo, d = hi | v = clip((v + r) >> b, c, d). r = 0 (round 0), 2^(b−1) (1), 2^(b−1) − (v < 0) (2: CPU `std::round`) |
| CLIP | a = lo, b = hi | v = min(max(v, a), b) |
| SHL | a = k | v = v << k |
| LUT | a = 표 번호 | v = lut_a[(uint8_t) v] |

지금 모델의 프로그램 (VTASave가 내는 그대로):

| 모델 | conv 뒤 | 프로그램 |
|---|---|---|
| Resnet18 / 50 | add, relu | ADD(0, shlV, shlT), RQ(2, m, −128, 127), CLIP(0, 127) |
| Resnet50 res2_0 | add, relu (scale 1/2) | … CLIP(0, 127), SHL(1), CLIP(−128, 127) |
| MobileNetV2 | add (relu 없음) | ADD(0, shlV, shlT), RQ(2, m, −128, 127) |
| DeiT fc1 | GELU 표 | LUT(0) (`VTASetOutputLut`가 내부에서 이 프로그램을 만든다) |

bundle에 나오는 코드 (Resnet18의 한 conv):

```c
  VTAEpilogue resnetv10_stage1_conv1_fwd__2_epi = {3, {{1, 0, 1, 0, 0}, {2, 2, 1, -128, 127}, {3, 0, 127, 0, 0}}};
  resnetv10_stage1_conv1_fwd__2_epi.t[0] = resnetv10_pool0_fwd__1_res_vbuf;
  VTASetConvEpilogue(&resnetv10_stage1_conv1_fwd__2_epi);
  convolution_wo_tr(..., vtaCmdH, 2, 8, 56, 0);
  VTASetConvEpilogue(nullptr);
```

- 구조체는 지역 변수다. 여러 thread가 같은 함수를 돌려도 섞이지 않는다.
- 한 줄 한 문장이다(`timed_bundle.py`가 문장 줄마다 시간 코드를 끼운다).

## 4. vtalib이 하는 일 (`vtalib-zcu102-systolic/lib/Bundle/VTABundle.cpp`)

### 4.1 lowering (`lowerEpilogue`)

ADD가 아닌 단계는 모두 int8 → int8 함수다. 그래서 프로그램은 늘 다음 꼴로 줄어든다.

```
conv의 int8 x --f1--> [ADD (v << shlV) + (t << shlT) --RQ(rnd, shift, lo, hi)--> int8 --f2-->] out
```

f1과 f2는 값의 범위(conv의 clip, RQ의 clip) 위에서 256개 입력을 실제로 계산해 보고 셋 중 하나로 분류한다.

| 종류 | 뜻 | EVTA에서 |
|---|---|---|
| 0 | clip(x, a, b) (아무것도 안 함 포함) | 앞의 clip 범위에 합친다. 명령이 늘지 않는다 |
| 1 | clip(x << q, a, b), q = 1..7 | acc에서 ALU SHR −q, MAX, MIN |
| 2 | 그 밖 (표) | ALU LUT(acc 안), 또는 ALU CPY + rq + `lut_en`. `_alux`만 |

- 표는 conv마다 하나다. 시작할 때 acc 0–15에 LOAD하고 LUTSET 한 번(이전 output LUT와 같다).
- RQ round 2(half away from zero)는 rq가 있는 비트스트림에서만 된다. ALU ADD / SHR로는 음수 .5를 내릴 수 없다.
  다만 뒤의 f2가 [lo, min(0, hi)]에서 한 값이면(add 뒤 relu) round 1과 결과가 같다. 그래서 round 1로 바꾸고 rq 없이 acc에서 한다.
- 결과는 프로그램 내용, 표 내용, conv의 relu / clip, EVTA 번호로 캐시한다(같은 프로그램은 다시 계산하지 않는다).

### 4.2 tile마다 나오는 명령 (`conv_kernel`, thread마다 STORE 전)

| 경우 | 명령 |
|---|---|
| ADD 없음, f 종류 0 | conv GEMM의 rq clip 범위만 바뀜. 추가 명령 없음 |
| ADD 없음, 표 | (rq) GEMM이 32비트 합을 남기고 ALU CPY + conv rq + `lut_en` 한 번. (rq 없음) ALU SHR / MAX / MIN 뒤 ALU LUT |
| ADD 없음, 종류 1 | conv requantize를 acc에서(SHR / MAX / MIN), SHR −q, MAX / MIN |
| ADD 있음 | conv requantize를 acc에서, f1(종류 1: SHR / MAX / MIN, 2: ALU LUT), SHR −shlV, residual LOAD + identity GEMM |
| ADD 뒤, rq, f2 종류 0 | residual GEMM에 RQ(rnd, shift, lo, hi) (이전 `rqRes`와 같음) |
| ADD 뒤, rq, f2 표 | residual GEMM, 그다음 ALU CPY + RQ + `lut_en` |
| ADD 뒤, acc에서 (rq 없음, 또는 f2 종류 1) | ADD 2^(shift−1), SHR shift, MAX / MIN, 종류 1이면 SHR −q, MAX / MIN, 표면 ALU LUT |

- 이전 경로와 달라진 점은 하나다. conv 값을 add 전에 키우는 shlV를 ALU ADD shlV번 대신 SHR −shlV 한 번으로 한다(결과 같음).
- Resnet50 res2_0(relu 뒤 scale ½)은 MAX 0, SHR −1, MIN 127로 이전과 같은 명령 수다.
  f2 종류 1에서 MAX를 shift 전에 하고, 필요 없는 MIN은 뺀다.
- replay key에 lowering 결과 전체(종류, q, 범위, shift, round)를 넣었다. 텐서와 표 버퍼는 relocation 대상이다.
- `VTASetOutputLut(T)`는 프로그램 `LUT(0)`, `convolution_wo_tr_res`는 `ADD, RQ(2), [CLIP 0..127[, SHL q, CLIP]]`로 바뀐다.
  그래서 예전 bundle도 다시 생성하지 않고 같은 길로 돈다. `ConvResidual`과 `convResidualCpuAdd`는 없앴다.

### 4.3 EVTA에서 못 할 때

- 예: 표 둘, ADD 둘, 확장 없는 비트스트림의 표, rq 없는 비트스트림의 round 2.
- 이때는 보통 conv를 돌리고, CPU에서 `VTAEpilogueRef`로 프로그램을 적용한다.
- 텐서가 출력 버퍼와 같으면(in-place residual) conv는 임시 버퍼에 쓴다. 결과는 같고, 느릴 뿐이다.
- `EVTA_EPI_CHECK=1`: 융합 실행과 함께 보통 conv + CPU 프로그램을 돌려 다른 출력 수를 찍는다. EVTA에서 돈 conv만 해당한다.
  예전 bundle의 `EVTA_RES_CHECK=1`은 그대로 float 식과 비교한다.

### 4.4 vta/vtalib (HLS GEMM 비트스트림)

- API는 같다.
- residual 형태 `ADD, RQ(round 2 또는 shift 0, −128..127)[, CLIP 0..127[, SHL q, CLIP]]`는 그 vtalib의 `convolution_wo_tr_res`로 간다. 이전과 같다.
- 나머지 형태는 보통 conv를 돌리고 CPU에서 적용한다.
- 그래서 한 bundle이 두 vtalib 모두와 링크된다(`#ifdef`가 필요 없다).

## 5. 컴파일러 (`lib/Backends/VTA/VTASave.cpp`)

`findResidualFusions`가 일반 chain walker가 됐다(`-vta-conv-epilogue`, 기본 켜짐).

- EVTA conv(EVTA 배치 출력)에서 시작해, alloc / dealloc을 건너뛰며 다음 명령을 본다.
  - elementadd: 지금 값 + EVTA 배치 residual. 하나만 받는다. scale 비는 2의 거듭제곱이어야 한다. ADD + RQ(2, m)를 넣는다.
  - relu: scale 비 2^q, q = 0..7. CLIP 0..127과, q가 있으면 SHL q, CLIP을 넣는다.
  - 다른 명령이 나오면 멈춘다.
- 조건(이전과 같음):
  - 사슬의 값은 사슬 밖에서 읽지 않는다.
  - 마지막 값(out)은 conv 전에 할당되어 있거나 앞으로 옮길 수 있다.
  - out은 conv 입력이 아니다.
- `ResidualFusion`에 `steps`(프로그램)와 `classic`(이전 `convolution_wo_tr_res` 형태인지)을 더했다. `relu`는 "out ≥ 0"의 뜻이 됐다
  (maxpool이 EVTA에서 돌 수 있는지 판정에 쓴다). 프로그램 위의 범위로 계산한다.
- `-vta-conv-epilogue=0`: 이전처럼 add → relu 하나 형태만 묶고 `convolution_wo_tr_res`를 낸다.
- 지금 네 CNN에서 묶이는 conv는 이전과 같다(Resnet18 8, Resnet50 16, MobileNetV2 10, VGG16 0). weights 파일은 바이트 단위로 같다.

## 6. 검증

### 6.1 시뮬레이터 (PC, `vtalib-zcu102-systolic/tests/run_epi_test.sh`)

PC에서 `_rq` 경로를 보려고 시뮬레이터부터 고쳤다(`lib/simulator/sim_driver.cc`, `runtime.cc`, `include/simulator/hw_spec.h`).
- 하드웨어처럼 out 버퍼(int8)를 따로 둔다. GEMM / ALU가 쓰고(하위 8비트, 또는 `rq_*` / `lut_en`), STORE가 읽는다.
- GEMM에도 `rq_*`가 있다.
- 전에는 STORE가 acc를 잘라 썼다. ALU rq / lut 결과는 acc에 덮어썼고, GEMM rq는 없었다.

`epi_conv_test.cc`: 보통 conv + `VTAEpilogueRef`와 융합 실행을 바이트 단위로 비교한다.
- 모양: 14×14×32 → 32 (3×3, vt 2, tile 7), 28×28×16 → 64 (1×1), 13×13×32 → 32 (끝 tile이 작음)
- 지금 모델의 형태 5가지
- 새 순서 8가지: add 뒤 표, 표 뒤 add + relu, add + ReLU6, add 전 shift, clip만, shift만, in place, shift 2 / 3
- EVTA에서 못 하는 것 2가지
- 무작위 프로그램 24개

| 모드 | 결과 |
|---|---|
| requantize 끔 (acc에서 ALU) | 39 / 39, 0 differ |
| requantize 켬 (`EVTA_REQUANT=1`) | 39 / 39, 0 differ |
| 확장 없음 (`EVTA_ALUX=0`: 표는 CPU) | 39 / 39, 0 differ |

기존 시험(`run_lut_conv_test.sh`: output LUT, layernorm, softmax)도 새 vtalib으로 모두 0 differ다.

### 6.2 보드

2026-11-14, `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_alux_380mhz_rq.bin`, 실행 전마다 비트스트림 다시 load, OMP 1.
old = 지금까지의 `_systimed` 실행 파일(`convolution_wo_tr_res`), new = PC에서 cross build한 `_pcsystimed`(이 vtalib + epilogue 번들).

ALU 테스트(PC에서 cross build, `tests/run_alux_board_tests.sh CROSS=1`, `EVTA_EPI_CHECK=1`):

| env | lut_conv | ln_evta | softmax_evta | epi_conv_test |
|---|---|---|---|---|
| 기본(`_rq` → rq on) | all equal | all equal | all equal | 39 / 39 0 differ |
| `EVTA_REQUANT=0` | all equal | all equal | all equal | 39 / 39 0 differ |
| `EVTA_ALUX=0` | all equal | FAILED (NOT RUN) | FAILED (not run) | 39 / 39 0 differ |

`EVTA_ALUX=0`에서 ln / softmax는 `layernorm_evta` / `softmax_evta`가 0을 돌려주는 것(번들은 그때 CPU 함수)이라 테스트가 FAILED로
찍는 것이 맞다. epilogue는 그 모드에서 table을 CPU로 돌려 같은 값을 낸다.

모델(`EVTA_DUMP_CONV`로 EVTA conv / pool 출력 전부, check 없이 old와 new를 byte 비교; `EVTA_EPI_CHECK=1 EVTA_RES_CHECK=1`의 실행은
따로, check가 돌리는 비교용 conv도 dump되어 번호가 밀리므로 dump 비교는 check 없이):

| 모델 | dump 비교 | check (0이 아닌 것) | 결과 old / new | 25회 old → new (ms) |
|---|---|---|---|---|
| MobileNetV2 | 5200 (100 runs) / 0 differ | 2900 / 0 | 282 0.944157 / 같음 | 18.06 → 18.09 |
| Resnet18 | 40 / 0 differ | 22 / 0 | 281 0.655530 / 같음 | 24.17 → 24.11 |
| Resnet50 | 106 / 0 differ | 38 / 0 | 281 0.581379 / 같음 | 71.27 → 71.39 |
| VGG16 CIFAR-10 | 34 / 0 differ | 12 / 0 | 0 0.453709 / 같음 | 7.16 → 7.25 |

명령열이 같은 모양(add → rq round 2 → relu)이라 시간도 같다(차이는 run 사이 흔들림 정도). 이득은 이 구조로 새 순서 / table을
붙일 수 있게 된 것이다(§7).

## 7. 남은 일

1. **EVTA 배치 activation 위의 표.**
   - 지금 `findLutRegions`는 EVTA 배치 값을 받지 않는다. `findPackedActivations`도 원소별 연산 중 elementadd / relu / pool만 배치를 유지한다.
   - CNN의 sigmoid / SiLU / leaky relu / hard-swish 사슬이 conv 바로 뒤에 오면, 배치를 유지하고 그 표를 epilogue LUT로 넣어야 한다.
   - `lutEligibleKind`에 해당 명령(sigmoid, tanh, IntLookupTable, clip 등)도 더한다.
   - 이것을 시험할 모델을 onnx/models에서 받는다(YOLO 계열, EfficientNet-Lite, MobileNetV3 등; [host_pc_build.md](host_pc_build.md) §6).
2. **2의 거듭제곱이 아닌 scale.** ADD의 identity weight를 2^k I 대신 임의의 int8 m I로 하고, conv 쪽은 ALU MUL을 쓴다.
   avgpool처럼 컴파일 때 모든 (conv, residual) 쌍(256 × 256)에서 CPU 식과 같은지 확인하는 방식이다.
3. **ADD 두 개, 표 두 개.** residual LOAD를 inp의 다른 자리에 하나 더 두고, 표는 tile마다 다시 LUTSET한다.
4. **pool도 epilogue 단계로.** conv0 + maxpool(`convolution_wo_tr_pool`)은 따로 남아 있다. avgpool 뒤 fc, concat도 같은 틀로 넣는다.
5. **GEMM `lut_en` (하드웨어).** add 뒤 표는 ALU CPY 한 번이 더 든다. GEMM의 out 쓰기에 표를 두면 그 pass가 없어진다.
6. **multi4 / vta/vtalib의 일반 프로그램.** vta/vtalib은 residual 형태만 EVTA에서 한다(HLS GEMM 비트스트림에는 rq / ALU 확장이 없다).
