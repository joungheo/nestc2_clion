# DeiT-tiny 실행 시간을 줄여 온 과정과 다음 계획 (2026-11-09 ~ 11-10)

ZCU102 위 EVTA(16×16 int8 GEMM)와 NEST-C로 DeiT-tiny 1장을 3.77 s에서 88.5 ms(1코어) / 55.7–57.9 ms(4코어)로 줄인 과정을 정리했다. 뭘 바꿔서 얼마나 줄었는지, 지금 시간이 어디에 쓰이는지, 앞으로 할 만한 것(소프트웨어, 하드웨어)을 담았다. 하드웨어는 전체 계획에 맞춰 어떤 순서로 바꾸면 좋을지도 적었다.

- **자세한 기록:** `docs/evta_runtime_compare.md` §7.22–§7.29
- **이 모델을 고른 이유와 다른 NPU 비교:** `docs/kv260_product_compare.md`
- **전체 하드웨어 / 소프트웨어 계획:** `docs/npu_stack_survey.md` §1, §5.3, `docs/evta_runtime_compare.md` §8

## 1. 대상과 측정 조건

| 항목 | 내용 |
|---|---|
| 모델 | `facebook/deit-tiny-distilled-patch16-224` ONNX(Xenova, opset 11), 입력 1×3×224×224로 접음(`vtabundles_xcmpl/fold_onnx.py`) |
| 양자화 | int8, power-of-2 scale(`symmetric_with_power2_scale`), calibration은 cat / dog / zebra 3장 |
| 번들 | `vtabundles_xcmpl/gen_deit_bundle.sh`(`-vta-round-shift` 포함) → `vta/bundles/DeiTTest` |
| 하드웨어 | ZCU102, EVTA 1개. §7.22–§7.25는 HLS GEMM 380 MHz, §7.26부터는 systolic 400 MHz `..._sa_400mhz_rq.bin` |
| 측정 | `hw/script/run_prof.sh -s _systimed`(문장별 wall time), `-s _sysprof`(EVTA 바쁜 시간), 25장 중앙값, OMP 1(1코어) 또는 `-t 4` |
| 정답 확인 | cat 281 / dog 207 / zebra 340. 정확도는 아직 ImageNet 검증 이미지로 재지 않았다 |

**연산량** (198 token)

| 연산 | MAC | 비중 | 지금 어디서 |
|---|---|---|---|
| Linear(Q/K/V, proj, fc1, fc2) × 12층 | 1.045 G | 83 % | EVTA 1×1 conv |
| attention Q·Kᵀ, A·V (3 head × 12층) | 0.179 G | 14 % | EVTA(실행 중에 배치 변환) |
| patch embedding 16×16 / 16 | 0.029 G | 2 % | EVTA(conv0 pack, 14×14×768 위의 1×1) |
| classifier 2개 | 0.0004 G | – | EVTA fc |
| LayerNorm / softmax / GELU | MAC 없음(원소 약 0.95 M / 1.4 M / 1.8 M) | – | CPU |
| **합계** | **약 1.25 G MAC (2.52 GOP)** | | |

16×16 @ 400 MHz는 102.4 GMAC/s다. 그래서 MAC만 100 % 가동해도 하한이 **12.2 ms**다.

## 2. 한눈에: 시간이 줄어 온 길

| 날짜 | 단계 (§) | 1장 OMP 1 | `-t 4` | EVTA 바쁜 시간 | 바뀐 것의 성격 |
|---|---|---|---|---|---|
| 11-09 | 처음 보드에서 끝까지 (§7.22) | 3,770 ms | 1,520 ms | – | 대부분 CPU float |
| 11-09 | Linear를 EVTA로 (§7.23) | 475 ms | | 56 ms (HLS) | 컴파일러 |
| 11-09 | attention matmul을 EVTA로 (§7.24) | 359 ms | | | 컴파일러 + vtalib |
| 11-09 | GELU 표, softmax int8, LayerNorm 조각 (§7.25) | 206 ms | | | CPU 함수 |
| 11-10 | matmul 포장 NEON (HLS) / 같은 번들 systolic 400 MHz (§7.26) | 198.9 / 184.4 ms | | 46.2 ms (conv) | vtalib / 비트스트림 |
| 11-10 | Linear bias 접기, int8 attention 전치, LayerNorm 한 함수 (§7.26) | 137.2 ms | | | 컴파일러 + CPU 함수 |
| 11-10 | Q/K를 EVTA 배치 그대로, 3 head 한 호출 (§7.27) | 130.7 ms | | | 컴파일러 + vtalib |
| 11-10 | softmax / LayerNorm 역수 곱 (§7.27) | 123.5 ms | 91.9 ms | 55.8 ms | CPU 함수 (값이 바뀔 수 있는 근사) |
| 11-10 | schedule 가상 스레드 2개, vt 2 결함 수정 (§7.28) | 107.4 ms | 75.2 ms | 38.3 ms | schedule + vtalib |
| 11-10 | conv DMA 묶기 `EVTA_CONV_BURST` (§7.29) | **88.5 ms** | **55.7–57.9 ms** | **19.4 ms (130 GOPS)** | vtalib (명령 모양) |

- 1코어 기준으로 **42.6배**(3,770 → 88.5 ms) 빨라졌다. 하드웨어(RTL) 변경은 없었다. 바꾼 것은 컴파일러(VTA.cpp / VTASave.cpp), 런타임(vtalib), schedule, CPU 함수다.
- **확률의 변화:** cat / dog / zebra 확률은 §7.22 0.547 / 0.918 / 0.980 → §7.24 0.738 / 0.900 / 0.953 → §7.26 이후 **0.755 / 0.950 / 0.963**이다. 분류는 처음부터 같다. 중간 양자화가 줄면서 확률이 올라갔다.
- **EVTA 시간:** 19.4 ms다. DRAM 전송을 모두 뺀 하한(19.6 ms, §7.28 `EVTA_MEMEXP`)과 같은 수준이다. MAC 하한(12.2 ms)과의 차이는 ALU, 패딩(198 → 208), 고정비다.

## 3. 단계별로 뭘 바꿨나

### 3.1 §7.22 일단 끝까지 돌리기: 3.77 s

| 바꾼 것 | 이유 |
|---|---|
| ONNX를 고정 shape로 상수 접기(1,261 → 608 node) | Glow importer가 동적 shape(Shape / Gather / Where 약 300개)를 못 받음 |
| VTASave generic CPU 연산(MatMul, reduce, erf, pow, copy, insert / extract, transpose, 여러 행 softmax) | VTA backend에 없는 연산은 `llvm_unreachable`(Release에서는 segfault)이었음 |
| tensor view offset, 상수 피연산자를 한 번만 씀 | offset 무시, 상수를 매번 다시 써서 뒤 상수의 위치가 밀림 |
| patch embedding을 conv0 pack으로(채널 상한 256 → 1024) | zero-fill 경로의 입력 tile이 inp SRAM을 넘어 오답(611) |

결과는 1장 3,770 ms(OMP 1), 1,520 ms(OMP 4)였다. 시간은 거의 전부 float naive matmul과 float ↔ int8 변환이었다.

### 3.2 §7.23 Linear를 EVTA로: 3,770 → 475 ms

| 단계 | 1장 | 바꾼 것 |
|---|---|---|
| 시작 | 3,770 ms | |
| Linear EVTA + int8 attention(스칼라) | 1,754 ms | `VTA.cpp`: MatMul을 int8로, 상수 rhs MatMul을 1×1 conv로(`matMulToConv`, 198행 = 18×11 이미지) |
| + NEON matmul, erf 표, pow | 944 ms | CPU int8 matmul NEON, dequantize된 int8 입력의 erf / exp는 256칸 표 |
| + vtalib NEON elementwise | 539 ms | quantize / dequantize / elemadd / elemdiv offset 0 경로 |
| + matmul 두 행, softmax 표 | **475 ms** | |

**고친 기존 결함:** 공유 상수(0 bias 9종을 72 conv가 공유)를 두 번째로 쓸 때부터 weights offset이 밀렸다.

### 3.3 §7.24 attention matmul을 EVTA로: 475 → 359 ms

- **방법:** 오른쪽이 activation인 int8 MatMul(Q·Kᵀ, A·V 72개)을 vtalib `matmul_evta`로 보낸다. 같은 1×1 conv에, 두 입력을 실행 중에 CPU가 EVTA 배치로 옮긴다. bias는 2^(shift−1)(반올림)이다.
- **결과:** attention matmul 146 → 30.2 ms. `EVTA_MATMUL_CHECK=1`로 288 호출 모두 0 differ였다.

### 3.4 §7.25 CPU 비선형 연산: 359 → 206 ms (값 동일)

| 단계 | 1장 | 바꾼 것 |
|---|---|---|
| §7.24 | 359 ms | |
| + GELU / attention ÷8 표 | 257 ms | `-vta-lut-elementwise`: int8 → … → int8 원소별 구간을 원래 생성 코드로 256칸 probe에 돌려 표를 만들고, NEON `vqtbl4q`로 찾음. 층당 8.6 → 0.4 ms |
| + softmaxI8 | 219 ms | dequantize → softmax를 int8을 직접 읽는 한 함수로. 54.1 → 18.5 ms |
| + LayerNorm 조각 | **206 ms** | pow 2 → square, reduceAdd 4행씩, elemsub NEON |

### 3.5 §7.26 systolic으로 옮기고 데이터 이동 줄이기: 206 → 137.2 ms

| 단계 | 1장 | cat / dog / zebra | 바꾼 것 |
|---|---|---|---|
| matmul 포장 NEON (HLS 380 MHz) | 198.9 ms | | B를 NEON 16×16 전치, 버퍼 재사용, 출력 순서대로 읽기. matmul 30.3 → 19.2 ms |
| 같은 번들, systolic 400 MHz | 184.4 ms | 0.738 / 0.900 / 0.953 | EVTA conv 55.7 → 46.2 ms. 이때부터 systolic에서 잼 |
| Linear bias를 conv bias에 접기 | 167.7 ms | 0.695 / 0.873 / 0.981 | `-vta-fold-matmul-bias`. elemadd 160 → 88개 |
| attention slice / concat / 전치를 int8로 | 152.3 ms | 0.755 / 0.950 / 0.963 | `isOpSupported`에 Slice / Concat. vtalib `transpose` 빠른 경로 30.3 → 2.5 ms |
| LayerNorm 한 함수 | **137.2 ms** | 같음 | `-vta-fuse-layernorm`: 21개 명령을 `layerNormI8` 하나로. 행 합은 NEON lane = 행 |

### 3.6 §7.27 attention의 배치와 호출 수: 137.2 → 123.5 ms

| 단계 | 1장 | 바꾼 것 |
|---|---|---|
| §7.26 | 137.2 ms | |
| Q/K를 EVTA 배치 그대로 + 3 head 한 호출 | 130.7 ms | `-vta-attn-keep-layout`: Q는 `VTABufferView`로 복사 없이 입력, K는 256 byte 블록 순서만 바꿔 weight. `-vta-matmul-heads`: conv batch(`gConvBatch`)로 72 → 24 호출. matmul 19.2 → 15.3 ms |
| softmax / LayerNorm 역수 곱 | **123.5 ms** | `-vta-cpu-approx`: ÷sum, ÷den을 ×역수로. softmax 18.7 → 12.5 ms, LayerNorm 21.0 → 19.9 ms. 세 장의 logit은 그대로 |

### 3.7 §7.28 schedule: 123.5 → 107.4 ms (`-t 4` 91.9 → 75.2 ms)

**원인:** 모든 Linear가 가상 스레드 1개(vt 1)로 돌아 LOAD와 GEMM이 겹치지 않았다.

`EVTA_MEMEXP`(DRAM 전송을 뺌, 결과는 틀어짐)로 잰 EVTA 시간:

| 뺀 것 | EVTA 시간 |
|---|---|
| 없음 | 56.2 ms |
| inp LOAD | 43.9 ms |
| wgt LOAD | 39.7 ms |
| STORE | 48.5 ms |
| 모두 | **19.6 ms** |

하나씩 뺀 효과의 합(36.6 ms)이 전체 차이와 같다. 즉 전송이 계산과 전혀 겹치지 않는다.

**schedule** (`VTASchedules.h`, `EVTA_SCHED`로 탐색):

| 모양 (입력 H×W×C → N) | 이전 → 새 값 | 호출 하나 |
|---|---|---|
| patch 14×14×768 → 192 | 1,7,2 → 2,7,14 | 4.75 → 0.66 ms |
| 18×11×192 → 192 (q / k / v / proj) | 1,14,14 → 2,9,11 | 0.295 → 0.219 ms |
| 18×11×192 → 768 (fc1) | 1,14,14 → 2,9,11 | 1.136 → 0.800 ms |
| 18×11×768 → 192 (fc2) | 1,14,14 → 2,9,11 | 1.012 → 0.715 ms |
| A·V 18×11×208 → 64 (3 head) | 1,14,14 → 2,18,11 | 0.477 → 0.35 ms |
| Q·Kᵀ 18×11×64 → 208 (3 head) | 1,14,14 → 2,9,11 | 0.44 → 약 0.36 ms |

- **vt 2 결함 (두 vtalib):** 출력 블록이 13개(홀수)면 conv_kernel이 블록 묶음 수를 0으로 골라 아무것도 계산하지 않았다. 오류도 없었다. 이제 vt 1로 낮추고, matmul은 N을 0 블록으로 짝수로 맞춘다.
- **patch embedding:** 1×1 conv0 pack은 tile 검사를 채널 블록 하나 단위로 한다. 그래서 7×2에 묶여 있던 것이 7×14가 됐다.

### 3.8 §7.29 DMA를 길게: 107.4 → 88.5 ms (`-t 4` 75.0 → 55.7 ms)

**원인 (replay dump):**
- weight는 K 블록마다, 스레드마다 256 byte짜리 6개로 나뉘었다.
- 9×11 tile은 연속인데도 176 byte짜리 행 9개로 나뉘었다.

**바꾼 것** (vtalib-systolic `EVTA_CONV_BURST`, 기본 켜짐):
- 1×1 conv는 스레드의 weight 전체를 한 번에 LOAD한다(행 하나가 cBlocks × 256 byte, `wgt_kernel_factor_`).
- 연속인 행들은 한 행으로 합친다.
- 폭이 이미지 폭과 같은 tile은 출력 블록 전부를 STORE 하나로 보낸다.

| | EVTA | 1장 `-t 4` | 1장 OMP 1 |
|---|---|---|---|
| burst 0 | 38.3 ms (66 GOPS) | 75.0 ms | 107.3 ms |
| burst 1 | **19.4 ms (130 GOPS)** | **55.7 ms** | **88.5 ms** |

다른 모델도 빨라졌다(1장 OMP 1). conv dump가 byte 단위로 같고, golden 29개가 모두 PASS다.

| 모델 | burst 0 | burst 1 |
|---|---|---|
| Resnet50 | 82.2 ms | 69.6 ms |
| MobileNetV2 | 24.3 ms | 18.8 ms |
| Resnet18 | 24.1 ms | 23.3 ms |
| VGG16 | 7.27 ms | 7.01 ms |

### 3.9 줄어든 시간을 성격별로

1코어 기준(3,770 → 88.5 ms)으로, 각 단계에서 줄어든 시간을 바꾼 것의 성격으로 묶었다.

| 성격 | 단계 (1장 OMP 1의 변화) | 줄어든 시간 |
|---|---|---|
| 연산을 EVTA로 보냄 (컴파일러) | Linear 3,770 → 1,754, attention 475 → 359 | 약 2,130 ms |
| CPU 연산을 NEON / 표로 | §7.23의 1,754 → 475(NEON matmul, erf / softmax 표, elementwise), §7.25 359 → 206(GELU 표, softmaxI8, LayerNorm 조각) | 약 1,430 ms |
| CPU 함수 합치기 / 근사 | LayerNorm 한 함수 152.3 → 137.2, 역수 곱 130.7 → 123.5 | 약 22 ms |
| 데이터 이동 없애기 | matmul 포장 206 → 198.9, bias 접기 184.4 → 167.7, int8 전치 167.7 → 152.3, Q/K 배치 유지 + head 묶음 137.2 → 130.7 | 약 46 ms |
| 비트스트림 (HLS 380 → systolic 400 MHz) | 198.9 → 184.4 | 14.5 ms |
| EVTA 효율 (schedule, DMA) | 123.5 → 107.4 → 88.5 (EVTA 55.8 → 19.4 ms) | 35 ms |

처음에는 float CPU 연산이 거의 전부라서, EVTA로 보내기와 NEON / 표가 대부분을 줄였다. 100 ms대에 들어선 뒤로는 데이터 이동과 EVTA 효율(DMA, schedule)이 남은 시간을 좌우했다.

## 4. 작업 중 발견하고 고친 결함

| 결함 | 증상 | 고친 곳 |
|---|---|---|
| 공유 상수를 두 번째 쓸 때 weights offset이 밀림 | 상수 값이 틀어짐 | VTASave `prepareVTAConvolutionInst` (§7.23) |
| vtalib `softmax`가 1행만 맞음 | 594×198 attention softmax 오답 | generic softmax 사용 (§7.22) |
| LUT 구간 안의 tensor view를 상수로 등록 | bias 접은 번들 segfault | `emitLutRegion` (§7.26) |
| Q/K conv 출력 해제가 matmul보다 앞 | 버퍼가 V conv 입력으로 재사용돼 분류 599 | `findAttnKeepLayout` freeAfter (§7.27) |
| vt 2 + 홀수 출력 블록 → nFilterinLoop 0 | 계산 없이 쓰레기, 오류 없음 | 두 vtalib conv / matmul (§7.28) |
| `-vta-round-shift` 빠뜨림 | bias가 128씩 다름(조용히) | `gen_deit_bundle.sh`가 늘 넣음 (§7.28) |
| `prof_summary.py`가 stream conv에서 표를 건너뜀 | `_sysprof` 표 없음 | stream run 합치기 (§7.28) |

## 5. 지금 시간이 어디에 쓰이나 (`-t 4`, 11-10, 1장 57.9 ms)

| 항목 | 시간 | 어디서 | 비고 |
|---|---|---|---|
| EVTA conv (Linear 72, patch 1) | 17.1 ms | EVTA | 1.07 G MAC, 125 GOPS |
| matmul_evta_qk (Q·Kᵀ) | 4.3 ms | 거의 EVTA | 0.36 ms × 12 |
| matmul_evta_n (A·V) | 4.1 ms | 거의 EVTA | 0.35 ms × 12, V 배치 변환 포함 |
| layerNormI8 | 5.6 ms | CPU (4코어) | 원소별 float 계산 |
| lut8 (GELU) | 5.2 ms | CPU **1코어** | 151 KB × 12, 병렬화 안 됨 |
| transpose_nhwc2vtaio / vtaio2nhwc | 3.0 / 2.4 ms | CPU | LayerNorm / softmax 앞뒤의 배치 변환 |
| elemadd (residual) | 3.9 ms | CPU **1코어** | |
| softmaxI8 | 3.9 ms | CPU (4코어) | |
| quantize | 3.0 ms | CPU | |
| 기타 (memmove, flush, alloc) | 약 2.5 ms | CPU | |

**합계는 EVTA 약 25.5 ms, CPU 약 32 ms다. 그리고 둘이 겹치지 않는다.** EVTA가 도는 동안 CPU는 기다리고, CPU가 도는 동안 EVTA는 논다.

## 6. 앞으로 할 만한 것

### 6.1 소프트웨어만으로 (지금 비트스트림)

| 후보 | 내용 | 예상 효과 (`-t 4`) | 값 |
|---|---|---|---|
| lut8 / elemadd 병렬화 | OpenMP로 행을 나눔 | 약 −6 ms | 같음 |
| LayerNorm / softmax / GELU 앞뒤 배치 변환 없애기 | EVTA 배치(N{C/16}HW16)에서 바로 계산하는 CPU 함수(행 = token, 16 채널 블록 단위) | transpose 5.4 + 일부 quantize, 약 −6 ms | 같음 |
| residual add를 EVTA로 | conv + elementadd 합치기(§7.2의 `convolution_wo_tr_res`)를 Linear에도 | elemadd 약 −3.9 ms | 같음 (power-of-2 scale이면) |
| EVTA와 CPU 겹치기 | 층 사이 의존이 있어 같은 장에서는 어려움. 2장을 엇갈려 처리(pipeline)하거나 head / 행 단위로 쪼개 겹침 | 처리량 최대 약 1.5–2배, 지연은 그대로 | 같음 |
| 정수 LayerNorm / softmax | I-BERT식 i-exp, 정수 sqrt, power-of-2 scale 정수 연산 | LayerNorm + softmax 9.5 → 약 3–4 ms | 바뀜 (ImageNet 확인 필요) |
| softmax 출력 quantize 합치기 | head별 quantize를 softmaxI8 안에서 | 1–2 ms | 같음 |
| A·V의 V 배치 변환 | V conv 출력을 weight 배치로 바로 쓰기(K처럼). V는 [token][채널]이 [k][n]이라 블록 안 전치가 필요 | 1–2 ms | 같음 |
| ImageNet 검증 | 정확도 측정 | – | – |

모두 하면 `-t 4`로 약 35 ms 안팎으로 추정한다. EVTA 약 25 ms가 남고, CPU는 약 10 ms다.

### 6.2 하드웨어를 바꿔야 하는 것

**EVTA ISA의 한계:**
- ALU opcode가 2 bit라 MIN / MAX / ADD / SHR만 된다.
- 곱셈, exp, 표, 행 방향 reduction이 없다.

그래서 비선형 연산(LayerNorm, softmax, GELU)은 CPU에 남는다. 다른 NPU는 이 부분에 vector / 특수함수 장치를 둔다(TPU VPU, Hexagon HVX, Gemmini 정규화 장치, NVDLA SDP, Ethos-U85의 elementwise / reduction).

| 후보 | 내용 | DeiT 효과 | 다른 모델 효과 | 비용 |
|---|---|---|---|---|
| H1. ALU opcode 확장 (2 → 3–4 bit) | MUL(×상수, ×vector), MOVE 인코딩. ISA 개정 | LayerNorm의 ×weight, ×역수 일부를 EVTA로 | avgpool, BN 안 접힌 scale, BNN | 중간: RTL ALU, `hw_spec.h`, vtalib, simulator |
| H2. 출력 경로 LUT (int8 → int8, 256칸) | requantize(`rq_*`, §7.18) 뒤에 표 하나. 표는 LOAD로 채움 | GELU 5.2 ms → 0 (Linear fc1 출력에 바로) | sigmoid / swish / hard-swish / tanh 등 모든 원소별 activation | 작음–중간: `sa_rmw.v` S6 뒤 BRAM 1개, GEMM / ALU 명령의 비트 하나 |
| H3. STORE 배치 옵션 | STORE가 16×16 블록을 전치해 쓰거나 weight 배치로 씀 | V 배치 변환, LayerNorm 앞뒤 transpose를 EVTA가 | im2col / depthwise 재배치 | 중간: store 모듈 |
| H4. 행 reduction이 있는 vector unit | 행 max / 합, 원소별 ×, 정수 exp(i-exp) / rsqrt 근사 | softmax 3.9, LayerNorm 5.6 ms → EVTA. CPU 거의 0 | 모든 Transformer, normalization | 큼: 새 블록, ISA 명령, 컴파일러 패턴 |
| H5. 명령 링 + interrupt + doorbell | CPU 폴링 없이 명령열 제출, 완료 알림 | EVTA와 CPU 겹치기의 기반 | 모든 모델, driver 공통화 | 중간: PS-PL IRQ, fetch 모듈 |
| H6. 하드웨어 loop FSM | tile을 하드웨어가 펼침(Gemmini `LoopConv`) | 명령 생성과 replay 부담 감소 | 모든 모델 | 큼 |
| H7. 배열 확대 / 여러 EVTA | 32×32(MAC 4배) 또는 multi4로 head / 행 분할 | EVTA 25 → 약 7–8 ms | 모든 모델 | 큼: SRAM, schedule, backend 블록 16 고정 |
| H8. 배열만 2배 클럭 | DSP / FF만 약 600 MHz | GEMM 처리량 최대 2배 | 모든 모델 | 큼: 클럭 경계 |

H1, H2와 H4의 reduction 일부를 한 번에 묶은 ALU 확장 설계(연산 12개, `alu_op_ext` 비트, 비용, 일정): [evta_isa_ext.md](evta_isa_ext.md) §8. 11-11에 3단계(HLS ALU, CSim)까지 끝났고, 비트스트림은 PC 냉각을 고친 뒤에 만든다(§8.8). EVTA 시간 추정은 확장 연산이 모두 II 2라 LayerNorm + softmax 약 11 ms다.

### 6.3 전체 계획에 비춘 순서

전체 계획의 축(`npu_stack_survey.md` §1, §5.3, `evta_runtime_compare.md` §8)은 이렇다.

1. 여러 가속기가 같은 제어 규약(링 버퍼 + doorbell + interrupt)과 HAL(buffer / executable / dispatch / fence)을 쓴다.
2. 명령 생성을 AOT로 옮긴다(record / replay가 0단계).
3. ALU 연산 범위와 LOAD / STORE 배치 유연성을 넓힌다.
4. 그 다음에 배열을 키운다.

DeiT가 필요로 하는 것도 같은 순서로 놓으면 서로 도움이 된다.

| 순서 | 할 일 | 왜 이 순서인가 | DeiT `-t 4` 예상 |
|---|---|---|---|
| 0 | §6.1 소프트웨어(병렬화, 배치 변환 없애기, residual EVTA, 정수 LayerNorm / softmax, ImageNet 정확도) | 하드웨어 없이 CPU 몫을 먼저 줄인다. 정수 식이 정해져야 H4의 하드웨어 식도 정해진다 | 57.9 → 약 35 ms |
| 1 | **ISA 개정 한 번에 H1 + H2 + H3** | 셋 다 명령 필드를 건드린다(opcode 폭, 출력 경로 LUT 비트, STORE 옵션). `evta_isa_ext.md`의 requantize와 같은 방식으로 한 번에 바꿔야 bitstream과 vtalib 짝이 한 번만 바뀐다 | 약 −10 ms (GELU, V 변환, transpose, LayerNorm 일부) |
| 2 | H5 명령 링 + interrupt | HAL 계획의 하드웨어 규약. CPU가 폴링하지 않아야 EVTA와 CPU를 겹칠 수 있다. 여러 가속기 공통 driver의 기반이기도 하다 | 처리량 개선(2장 pipeline) |
| 3 | H4 vector unit | 가장 크다. 0번의 정수 식과 1번의 ISA 개정 뒤에 정의가 명확해진다. 출력 경로(H2) 뒤나 ALU 옆에 둔다 | CPU 거의 0 → EVTA 약 25 ms가 전부 |
| 4 | H7 배열 확대 / multi4 (그리고 KV260) | CPU 몫이 남아 있으면 배열을 키워도 전체가 거의 안 준다(Amdahl). 1–3 뒤에 해야 효과가 그대로 나온다. KV260(ZU5EV, DSP 1,248, URAM)에서는 32×32가 가능하다 | 약 7–10 ms |
| 5 | H6 loop FSM, H8 2배 클럭 | 배열이 커지면 명령 생성과 고정비가 상대적으로 커진다. 그때 효과가 크다 | 5–8 ms 목표 |

- **H2(출력 경로 LUT)는 가장 싸고 효과가 확실하다.** requantize가 이미 출력 경로에 있어서 그 뒤에 표 하나만 붙이면 된다. GELU 말고도 sigmoid / swish / hard-swish 같은 activation 전부에 쓰인다. 그래서 1번 ISA 개정의 첫 후보로 둔다.
- **H4(vector unit)는 크기가 크다.** 먼저 0번에서 CPU 정수 구현으로 식과 정확도를 확인한다. 그 다음 같은 식을 RTL로 옮기는 순서가 위험이 적다.
- **배열 확대(H7)를 먼저 하면 손해다.** 지금 EVTA 몫은 25.5 ms이고, 4배 빨라져도 1장은 57.9 → 약 39 ms에 그친다. CPU 32 ms가 그대로 남기 때문이다.
- **KV260으로 옮길 때:** 클럭이 300 MHz 안팎이라 EVTA 부분이 약 1.3배 길어진다(`kv260_product_compare.md` §6.3). 32×32와 URAM을 쓰는 설계는 KV260 이식과 함께 계획하는 것이 좋다.

### 6.4 목표

| 단계 | 내용 | `-t 4` 1장 | Hailo-8 (11.2 ms) 대비 |
|---|---|---|---|
| 지금 | | 55.7–57.9 ms | 약 5배 느림 |
| 0 | 소프트웨어 | 약 35 ms | 약 3배 |
| 0 + 1 | + ISA 개정(LUT, MUL, STORE 배치) | 약 25–28 ms | 약 2.3배 |
| 0–3 | + 링 / irq + vector unit | 약 20–25 ms (EVTA가 전부, 하한 12.2 ms) | 약 2배 |
| 0–4 | + 32×32 또는 multi4 | 약 7–10 ms | 비슷하거나 빠름 |
| 0–5 | + loop FSM / 2배 클럭 | 5–8 ms | 빠름 |

추정값이다. 0번의 정수 LayerNorm / softmax와 H4는 정확도를 ImageNet으로 확인해야 한다.

## 6.5 EVTA 4개에 copy 4개 (multi4 systolic, 11-10)

같은 DeiT 번들을 copy 4개로 만들어(copy i = EVTA i) multi4 systolic 비트스트림(`..._4evta_sa_400mhz_rq.bin`)에서 동시에 돌렸다. 처리량을 늘리는 구성이고, 1장의 지연은 줄지 않는다. 한 장을 EVTA 4개로 나누는 일은 아직 하지 않았다.

- **번들:** `vta/multievta_bundles/MultiEVTADeiTTest`, `vtabundles_xcmpl/gen_multi4_bundles.sh MultiEVTADeiTTest`. copy마다 graph hash를 고쳐 가며 다시 시도한다.
- **고친 것:** VTASave의 attention matmul 호출(`matmul_evta_n` / `_qk`)에 copy의 명령 핸들(`vtaCmdH<n>`)과 EVTA 번호가 빠져 있었다(`matMulCallTail`). single 번들은 바뀌지 않는다. copy 0은 single 번들과 코드, weights 모두 같다.
- **vtalib:** 같은 `vtalib-zcu102-systolic`을 multi4 설정(`build_multievtax4_sys*`)으로 빌드했다.
- **검증:** copy 4개 모두 281(0.754596)이다. `EVTA_MATMUL_CHECK=1`로 288 head(4 × 72)가 0 differ였다.

| (OMP 1, copy마다 25장 중앙값) | 1장 | EVTA conv | 처리량 |
|---|---|---|---|
| single 비트스트림, 1코어 (§6.1) | 88.5 ms | 17.1 ms | 11.3장/s |
| single 비트스트림, `-t 4` | 55.7–57.9 ms | | 약 17.6장/s |
| multi4 비트스트림, copy 0 혼자 | 88.3 ms | 18.05 ms | 11.3장/s |
| **multi4, copy 4개 동시** | **106.6 ms** (copy마다) | 19.2–19.4 ms | **37.5장/s** |

- 처리량은 copy 하나의 **3.32배**, single EVTA를 4코어로 돌릴 때의 약 2.1배다.
- 4개를 같이 돌리면 copy마다 18 ms 느려진다. copy 0의 함수별 시간을 비교했다(혼자 → 4개 동시, ms).

| 함수 | 혼자 | 4개 동시 | 차이 |
|---|---|---|---|
| transpose_nhwc2vtaio / vtaio2nhwc | 6.56 / 6.29 | 10.59 / 8.29 | +4.0 / +2.0 |
| quantize | 2.68 | 5.57 | +2.9 |
| matmul_evta_n / _qk | 3.54 / 4.19 | 5.76 / 5.72 | +2.2 / +1.5 |
| softmaxI8 | 12.41 | 13.87 | +1.5 |
| convolution_wo_tr (EVTA) | 18.04 | 19.23 | +1.2 |
| VTABufferAlloc / Free, memmove | 0.24, 0.41 | 1.65, 1.29 | +2.3 |
| layerNormI8, lut8, elemadd | 19.95, 5.14, 3.90 | 19.38, 5.32, 4.03 | 거의 그대로 |

- **늘어난 것:** 대부분 메모리를 많이 쓰는 CPU 연산이다(배치 변환, quantize, memmove, matmul 포장). 4코어가 L2(1 MB)와 DDR를 나눠 쓰기 때문이다. EVTA conv도 Resnet50 multi4처럼(`resnet50_optimization.md` §4) DDR 경합으로 약 1 ms 는다. buffer pool은 mutex 하나를 copy들이 같이 쓴다.
- **그대로인 것:** 계산 위주 연산(LayerNorm, GELU 표, elemadd)이다.
- **줄이는 방법:** §6.1의 배치 변환 없애기와 quantize 합치기가 multi4에서는 효과가 두 배 가까이 된다. buffer pool은 thread별 list로 바꿀 수 있다.

```bash
cd vtabundles_xcmpl
./gen_multi4_bundles.sh MultiEVTADeiTTest && python3 collect_hw_bundles.py --set multievtax4 MultiEVTADeiTTest
(cd build_multievtax4_systimed && cmake . && make -j2 multivtaDeiTBundle singlevtaDeiTBundle)
sudo hw_multievtax4/script/run_prof.sh -s _systimed -r MultiEVTADeiTTest     # 4개 동시와 copy 0 혼자
```

## 6.6 정확도 (ImageNetV2, 2026-11-10)

ImageNet val은 계정 동의가 필요해 받을 수 없었다. 그래서 공개된 **ImageNetV2 matched-frequency**(10,000장, 1000 클래스 × 10장)로 쟀다.
- 전처리는 DeiT와 같다: 짧은 변을 256으로 bicubic resize한 뒤 가운데 224를 crop한다.
- V2는 val보다 보통 10 %p 이상 낮게 나온다. 그래서 절대값보다 float과의 차이를 본다.

### 6.6.1 도구 (`vtabundles_xcmpl/imagenet_eval/`)

| 파일 | 하는 일 |
|---|---|
| `prep_imagenetv2.py` | JPEG → 224 PNG와 `list.txt`(클래스를 번갈아 늘어놓아, 앞부분만 써도 모든 클래스가 들어감). PC `~/data2/datasets/imagenetv2/png224`, 보드 `~/JO/imagenetv2_png224` |
| `float_ref.py` | float ONNX(onnxruntime)의 top-1 |
| `fakequant.py` | PC에서 ONNX에 int8 fake quantization(QDQ)을 넣어 근사. power-of-2 여부와 float로 둘 블록을 옵션으로 고른다. 1,000장에 약 40초 |
| `eval_board.sh` | 보드 실행 파일의 `EVAL_LIST` 모드(weight를 한 번 올리고 여러 장을 분류, `DeiTTest/multi_threads_main.cpp`). 9,000장에 약 10–18분 |

예:
```bash
# 보드 (vtabundles_xcmpl/, build_sys = -DBUNDLE_EXE_SUFFIX=_sys -DVTALIB=../vtalib-zcu102-systolic)
sudo /bin/sh -c "NESTC_LN_INT=1 imagenet_eval/eval_board.sh hw/DeiTTest/vtaDeiTBundle_sys lnint_9k 9000"
# PC (onnxruntime이 있는 python)
python fakequant.py deit_fold.onnx ~/data2/datasets/imagenetv2/png224 --n 9000 --pow2 --ln-float
```

- 평가는 앞 9,000장으로 한다.
- 뒤 1,000장 중 10장마다 하나(100장)는 calibration용이다(`deit_calib100.yaml`). 그전 번들은 cat / dog / zebra 3장으로 calibration했다.
- 지금 `Debug2_remote/vta/bundles/DeiTTest/deit_calib.yaml`은 100장 profile이다. 3장 profile은 `deit_calib_3img.yaml`이다.

### 6.6.2 결과 (앞 9,000장)

| 설정 | top-1 | float 대비 |
|---|---|---|
| float ONNX | **62.34 %** | |
| PC 근사: int8, scale 자유, LayerNorm 내부 float | 60.66 % | −1.7 |
| PC 근사: 위 + power-of-2 scale | 58.34 % | −4.0 |
| 보드, calibration 3장 | 55.84 % | −6.5 |
| 보드, calibration 100장 | 56.17 % | −6.2 |
| 보드, calibration 100장 + LayerNorm float | 56.61 % | −5.7 |
| 보드, calibration 100장 + 정수 LayerNorm / softmax (`evta_isa_ext.md` §8.6) | 56.49 % | −5.9 |
| Glow int8 Interpreter (300장, 같은 profile) | 32 % (float 68 %) | 참고: Glow 기본 int8 lowering은 훨씬 나쁨. VTAInterpreter는 이 그래프에서 segfault |

**손실을 나누면** (±0.5 %p 잡음):
- int8 자체: −1.7 %p
- **power-of-2 scale: −2.3 %p.** EVTA의 requantize가 shift뿐이기 때문이다. 텐서 하나를 풀어서는 크게 좋아지지 않았고, 여러 곳에 퍼져 있다(1,000장 실험: residual 쪽이 +1 %p로 가장 큼).
- 번들 구현 쪽(PC 근사와의 차이): −1.7 %p. 원인은 아직 모른다. 후보는 Glow lowering이 더 만드는 중간 텐서의 양자화와 attention ÷8 처리다.
- LayerNorm int8 체인: −0.4 %p
- calibration 3장 → 100장: +0.3 %p (차이 거의 없음)
- 정수 LayerNorm / softmax는 float 계산과 같은 정확도다. 하드웨어로 옮겨도 정확도 걱정은 없다.

**다음** (11-11에 했다, §6.6.3):
1. power-of-2를 풀 requantize 곱셈. GEMM 출력 경로의 `rq_mul`, 소프트웨어로는 2^n이 아닌 scale을 CPU에서. PC 근사로 최대 +2.3 %p
2. 번들 구현 −1.7 %p의 원인. PC 근사와 보드의 층별 출력 비교
3. 채널별(per-channel) weight scale

### 6.6.3 정확도 개선 (2026-11-11): 56.17 → 60.47 %, 그리고 더 빠름

| 단계 | 바꾼 것 | top-1 (9,000장) | 1코어 / 4코어 |
|---|---|---|---|
| 시작 | power-of-2 scale, Glow int8 LayerNorm 체인 | 56.17 % | 88.8 / 55.7 ms |
| ① | scale 자유(`-quantization-schema=symmetric`) + EVTA 연산은 비율만 2의 거듭제곱(`-vta-pow2-ratio`) | 59.71 % | 89.5 / 55.9 ms |
| ② | 정수 LayerNorm (bias를 shift 전에 더함) | **60.29 %** | **76.5 / 52.9 ms** |
| 기본값으로 다시 만든 번들 (`gen_deit_bundle.sh`, ① + ②) | 같음 | **60.47 %** | **75.7** / 52.9 ms |
| 참고 | LayerNorm float (`NESTC_LN_FLOAT=1`) | 60.41 % | |
| 참고 | float ONNX | 62.34 % | |

**① `rq_mul` 없이 power-of-2 손실 줄이기 (`VTA.cpp` `-vta-pow2-ratio`, 기본 on)**
- EVTA의 requantize는 shift뿐이지만, 필요한 것은 **비율** 입력 × weight / 출력이 2의 거듭제곱인 것이다. 텐서 scale마다 2의 거듭제곱일 필요는 없다.
- 그래서 Glow는 `symmetric`(자유 scale)으로 양자화하고, VTA backend pass가 int8 Convolution / FullyConnected / MatMul의 출력 scale을 입력 × weight × 2^k로 올림한다.
  - weight는 그대로 둔다.
  - 그 뒤의 Reshape / Transpose / Slice / Concat 출력도 같은 scale로 맞춘다. Concat의 상수 입력(cls 토큰)은 다시 양자화하고, attention head들은 함께 바꾼다.
  - 입력이 바뀐 conv의 int32 bias는 다시 양자화한다.
- PC 근사로 고른 방식이다(`fakequant.py --ratio`):

| 맞추는 쪽 | top-1 (PC 근사, LayerNorm float) |
|---|---|
| 출력 scale을 올림 | 60.51 % |
| weight scale을 올림 | 60.43 % |
| 연산마다 둘 중 작은 쪽 | 60.31 % |
| scale 완전 자유 (`rq_mul`가 있을 때) | 60.66 % |
| 모두 power-of-2 (전) | 58.34 % |

- 같이 고친 것:
  - VTASave의 scale 비교를 float 오차를 허용하게 바꿨다(`sameScale`, `getExpofPowerofTwo(float)`). 정확 비교라서 Linear 28개가 CPU conv로 빠졌었다.
  - matmul bias 접기를 비율이 정수가 아닐 때도 한다(반올림).
  - vtalib `quantize`는 2의 거듭제곱이 아닌 scale에서도 역수를 곱한다. 나눗셈이 3 → 7 ms였다.
    - .5 경계 검사는 A53에서 나눗셈만큼 비쌌다(117,612개: 나눗셈 0.50, 곱 + 검사 0.66 / 0.52, 곱 0.28 ms).
    - 곱으로 결과가 달라지는 것은 원소 약 10⁻⁶ 비율이다. `EVTA_QUANT_EXACT=1`이면 나눗셈을 쓴다.
  - `layerNormI8`는 `-vta-cpu-approx`일 때 출력 양자화도 역수로 곱한다.
- **결론: DeiT에서는 하드웨어 `rq_mul`가 필요 없다.** 남는 이득은 PC 근사로 0.15 %p다.

**② 번들 구현 −1.7 %p의 원인**
- ①에서 대부분 사라졌다(보드 59.71 %, PC 근사 60.51 %).
- 남은 0.7 %p는 Glow의 int8 LayerNorm 체인이었다. 평균 / 분산 / 표준편차를 int8로 양자화한다. 보드에서 LayerNorm을 float로 하면 60.41 %로, PC 근사와 같다.
- 정수 LayerNorm(`evta_isa_ext.md` §8.6)은 처음에 59.89 %였다.
  - 원인은 bias를 int8 단위로 먼저 반올림한 것이다. b × scale 비가 .5일 때 출력의 25 %가 1씩 달라졌다.
  - bias를 shift 전의 고정소수점에 더하자(conv bias와 같이) 60.29 %가 되었다.
  - 1/√ 표는 8비트로 충분하다. 16비트 보간은 60.30 %였다.
- 정수 LayerNorm을 NEON int32로 짜서 Glow 체인보다 빠르다(1코어 20.2 → 8.0 ms). γ / β 계수는 호출마다 한 번만 만든다.
- 그래서 기본으로 삼았다(`NESTC_LN_INT=0`이면 예전 체인). 정수 softmax(16비트 exp)는 60.13 %라 기본으로 하지 않았다.

**③ 채널별 weight scale: 효과 없음 (PC 근사, 9,000장, 4스레드)**

| 설정 | top-1 |
|---|---|
| 텐서별 weight, 출력 scale로 비율 맞춤 (지금 방식) | 60.32 % |
| 채널별 weight, 채널마다 비율을 2의 거듭제곱으로 (하드웨어에 채널별 shift 필요) | 60.48 % |
| 채널별 weight, scale 자유 | 60.16 % |

- DeiT의 Linear weight는 채널 사이 범위 차이가 작아, 채널별 scale이 이득이 없다. 하드웨어 / 소프트웨어 작업 대상에서 뺀다.

**남은 손실** (float 62.34 % 대비 −2.0 %p):
- int8 자체 −1.7 %p
- 출력 scale 올림 −0.15 %p
- 나머지는 잡음 범위다.
- 더 줄이려면 다음이 필요하다(아직 안 함):
  - 민감한 층(첫 patch embedding, 마지막 classifier)을 16비트로 두기
  - percentile / KL calibration

## 7. 재현

끄는 방법은 세 종류다. `-vta-*`는 번들 생성(`model-compiler`) 옵션이라 번들을 다시 만들어야 하고, `EVTA_*`는 실행할 때 주는 환경변수(`sudo /bin/sh -c "EVTA_X=0 hw/script/run_prof.sh ..."`), 하드웨어 단계는 `BIN=<비트스트림>`이다. 자세한 절차와 vtalib마다 읽는 변수의 차이는 `resnet18_optimization.md` §9.2–§9.5에 있다.

```bash
# 번들 생성 (보드, Debug2_remote의 model-compiler)
vtabundles_xcmpl/gen_deit_bundle.sh                 # -vta-round-shift는 스크립트가 넣음
python3 vtabundles_xcmpl/collect_hw_bundles.py DeiTTest
(cd vtabundles_xcmpl/build_systimed && make -j2 vtaDeiTBundle)
(cd vtabundles_xcmpl/build_sysprof && make -j2 vtaDeiTBundle)

# 측정 (vtabundles_xcmpl/)
sudo hw/script/run_prof.sh -s _systimed DeiTTest          # 1코어, 문장별 시간
sudo hw/script/run_prof.sh -s _systimed -t 4 DeiTTest     # 4코어
sudo hw/script/run_prof.sh -s _sysprof -t 4 DeiTTest      # EVTA 호출별 시간과 GOPS

# 검증용 환경변수 (sudo /bin/sh -c "X=1 hw/script/run_prof.sh ..." 형태로)
EVTA_MATMUL_CHECK=1        # attention matmul을 CPU 정수 결과와 비교
EVTA_DUMP_CONV=<dir>       # conv 출력 dump
EVTA_SCHED='<shape>:<vt>,<th>,<tw>;...'   # schedule 바꿔 끼우기 (~/JO/deit_tmp/schedsearch.sh)
EVTA_CONV_BURST=0          # §7.29 이전 DMA
EVTA_MEMEXP=<mask>         # DRAM 전송 빼기 실험 (결과는 틀어짐)
EVTA_REPLAY_LOG=2 / EVTA_REPLAY_DUMP=<dir>   # run별 GEMM / DRAM byte, 명령어 dump
```

VTASave 옵션은 모두 기본 켜짐이고, 끄면 그 단계 이전과 같은 코드가 나온다: `-vta-matmul-conv`, `-vta-fold-matmul-bias`, `-vta-evta-matmul`, `-vta-lut-elementwise`, `-vta-fuse-layernorm`, `-vta-matmul-heads`, `-vta-attn-keep-layout`, `-vta-cpu-approx`.
