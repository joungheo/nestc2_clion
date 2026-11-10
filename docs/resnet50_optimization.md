# Resnet50 실행 시간을 줄여 온 과정 (2026-11-05 ~ 11-10)

ZCU102 위 EVTA와 NEST-C로 Resnet50 1장을 줄인 과정이다. 처음 잰 값은 160.3 ms(systolic 400 MHz 기본 비트스트림, 1코어)였고, 지금은 **69.6 ms**다. 단계마다 왜 바꿨는지, 어떻게 바꿨는지, 얼마나 줄었는지, 어떻게 검증했는지를 적었다.

- **자세한 기록:** `docs/evta_runtime_compare.md` §7.19, §7.29
- **Resnet18 과정:** `docs/resnet18_optimization.md`. Resnet50은 Resnet18에서 만든 컴파일러 / runtime 기능을 그대로 물려받았다.

## 0. 한눈에

| 날짜 | 단계 | 1장 OMP 1 | EVTA 바쁜 시간 | 바꾼 곳 | § |
|---|---|---|---|---|---|
| 11-05 | 처음 잼 (Resnet18의 컴파일러 / runtime 기능 포함, 기본 400 MHz 비트스트림) | 160.27 ms | 125.56 ms (65 GOPS) | – | §7.19 |
| 11-05 | 하드웨어 세 단계 (ALU II = 1, bias reset, requantize) | 147.92 ms | 113.36 ms (72 GOPS) | 비트스트림 | §7.19 |
| 11-05 | ① res2_0 residual 합치기 | 122.46 ms | 115.27 ms | 컴파일러 + vtalib | §7.19 ① |
| 11-05 | ② Resnet50 모양의 schedule | 85.71 ms | 78.08 ms (105 GOPS) | schedule | §7.19 ② |
| 11-05 | ④ fc를 EVTA로 | 83.40 ms | 78.48 ms | 컴파일러 + vtalib | §7.19 ④ |
| 11-06 | ⑥ conv0 입력 복사를 conv와 겹침 | 83.41 → 82.21 ms | | vtalib runtime | §7.19 ⑥ |
| 11-10 | conv DMA 묶기 (`EVTA_CONV_BURST`) | 82.20 → **69.56 ms** | conv 79.79 → 67.17 ms | vtalib | §7.29 |

- 1코어 기준 160.3 → 69.6 ms로 **2.3배** 빨라졌다.
- 분류는 281이고, 확률 0.581379는 모든 단계에서 그대로다.
- 연산량은 8,174 Mops다. 16×16 @ 400 MHz(204.8 GOPS) 하한은 39.9 ms이고, 지금 EVTA 호출 합 67.2 ms는 약 122 GOPS, 하한의 60 %다.

## 1. 측정 조건

| 항목 | 내용 |
|---|---|
| 모델 | Caffe2 Resnet50(`resnet50.onnx`), 224×224, int8 power-of-2, softmax는 float(`-keep-original-precision-for-nodes=SoftMax`) |
| 번들 | `vta/bundles/Resnet50Test` → `vtabundles_xcmpl/hw/Resnet50Test` |
| 하드웨어 | ZCU102, single EVTA, systolic 400 MHz `..._sa_400mhz_rq.bin`(11-05 하드웨어 비교 때만 네 비트스트림) |
| 측정 | `run_prof.sh -s _systimed -r` / `-s _sysprof -r`, OMP 1, 25장 중 runs 2–25 중앙값 |

## 2. 출발점: Resnet18에서 한 일은 들어갔는데 왜 느린가 (11-05)

같은 실행 파일을 systolic 비트스트림 네 개에서 차례로 돌렸다.

| EVTA 시간 (`_sysprof`) | `sa_400mhz` | `alu1_epr` | `bias` | `rq` | 합 |
|---|---|---|---|---|---|
| Resnet18 (3,628 Mops) | 26.43 ms (137 GOPS) | 24.89 | 24.15 | **23.05 ms (157 GOPS)** | −12.8 % |
| **Resnet50 (8,174 Mops)** | 125.56 ms (65 GOPS) | 120.27 | 118.58 | **113.36 ms (72 GOPS)** | −9.7 % |

| 추론 한 장 (`_systimed`) | `sa_400mhz` | `rq` | EVTA 밖 (`rq`) |
|---|---|---|---|
| Resnet18 | 28.84 ms | 25.53 ms | 2.3 ms |
| **Resnet50** | 160.27 ms | 147.92 ms | **33.4 ms** |

- 하드웨어 세 단계는 Resnet50에서도 효과가 있었다(EVTA −12.2 ms). 하지만 GOPS가 Resnet18의 절반 이하였다.
- 원인은 하드웨어가 아니라, Resnet18에 없던 모양이었다.
  1. **1×1 conv(bottleneck):** 3×3 conv 16개는 178–192 GOPS(이론의 90 % 이상)인데, 1×1 conv 36개는 27–69 GOPS다. EVTA 113.4 ms 가운데 약 87 ms가 1×1이다. 1×1은 GEMM 대비 LOAD가 커서, 지금 schedule(기본 vt 1, 14×14)로는 LOAD가 숨지 않는다.
  2. **res2_0 residual이 합쳐지지 않음:** residual 16개 중 15개는 EVTA 한 호출(`convolution_wo_tr_res`)이었다. res2_0(56×56×256, 두 입력이 모두 conv)만 CPU `elemadd` 17.9 ms + `relu` 9.3 ms = **27.2 ms**였다. EVTA 밖 33.4 ms의 대부분이다.
  3. **나머지 CPU:** fc 2.7 ms(2048 → 1000, EVTA fc 조건 밖), avgpool 0.92 ms(7×7 평균은 ALU에 나눗셈이 없어 CPU), conv0 입력 복사 1.39 ms

## 3. 단계별로 뭘 바꿨나

### 3.1 ① res2_0 residual 합치기 (`VTASave.cpp` `findResidualFusions`, vtalib `convolution_wo_tr_res`)

- **왜 안 합쳐졌나:** relu의 출력 scale(1/16)이 elementadd 출력(1/8)과 달랐다. 합치는 조건이 "같은 scale"이었다.
- **함께 찾은 결함:** CPU `relu`는 이때 `int8_t v *= 2`로 scale을 맞췄다. 그래서 64 이상이 음수로 넘어갔다(wrap).
- **어떻게:**
  - 이제 sO / sO' = 2^q(q = 0–7)도 합친다. vtalib `sumRelu`를 bool에서 int로 바꿨다: 0 없음, 1 relu, 1 + q는 relu 뒤 << q(ALU SHR에 음수 imm) 뒤 MIN 127이다.
  - q > 0이면 requantize 대신 ALU 명령으로 한다. 반올림 shift와 왼쪽 shift를 한 번에 할 수 없기 때문이다.
  - 예전 번들은 0 / 1을 넘기므로 그대로 돈다.
  - CPU `relu`(generic, x86)는 127에서 자르게 고쳤다.
- **검증:** `EVTA_RES_CHECK=1`로 Resnet50의 합친 호출 16개와 conv0 + pool이 모두 0 differ였다. 분류도 그대로다. Resnet18 번들은 다시 생성해도 byte 단위로 같다.

| Resnet50 (`rq`) | 전 | 후 |
|---|---|---|
| 추론 한 장 | 147.92 ms | **122.46 ms** (−25.5 ms, −17 %) |
| EVTA 시간 | 113.36 ms | 115.27 ms (res2_0 elemadd + relu가 EVTA로, +1.9 ms) |

### 3.2 ② Resnet50 모양의 schedule (`include/Backends/VTA/VTASchedules.h`)

- **왜:** `VTASchedules.h`에는 Resnet18 모양만 있었다. 그래서 Resnet50의 1×1 conv 36개와 3×3 일부가 기본값(virtual thread 1, tile 14×14)으로 돌았다. virtual thread가 1이면 다음 입력 채널의 LOAD가 앞 GEMM을 기다린다.
- **참고한 것:** TVM VTA(`tvm/vta/python/vta/top/vta_conv2d.py`)는 층마다 AutoTVM으로 tile_h / w / ci / co와 virtual thread(`oc_nthread`, `h_nthread` 1–2)를 찾고, 미리 찾은 값(TopHub log)을 쓴다. 같은 방식으로 보드에서 찾았다.
- **어떻게:**
  - vtalib-systolic에 `EVTA_SCHED=<H>,<W>,<C>,<KN>,<K>,<stride>:<vt>,<tileH>,<tileW>[;...]`(실행할 때 schedule 바꾸기)와 `EVTA_SCHED_LOG=1`을 넣었다.
  - 모양마다 후보(virtual thread 1 / 2 × 출력을 나누는 tile, inp 반쪽 1024 · acc 2048 entry 안, 최대 16개)를 `_sysprof`로 재어 가장 빠른 것을 골랐다.
- **결과:** 거의 모든 모양에서 virtual thread 2와 큰 tile이 이겼다.

| 모양 | 새 schedule | 호출 하나 |
|---|---|---|
| 1×1, 56×56 | (2, 8, 56) | −48 ~ −61 % |
| 28×28 | (2, 7, 28) | −45 % |
| 14×14 | (2, 7, 14) | −30 % |
| 7×7 | (2, 7, 7) | −23 % |

- **검증:** 같은 실행 파일에서 기본 schedule과 새 schedule의 `EVTA_DUMP_CONV` 106개가 같다.
- **처음 확인의 실수:** 처음에는 `EVTA_SCHED`가 없는 실행 파일의 dump로 비교해서 무효였다. 다시 확인하니 출력이 7×7보다 작은 conv에서 vt 2가 틀렸다. 이 문제는 VGG16에서 드러났고 Resnet50에는 그런 conv가 없다. 고친 내용은 `vgg16_optimization.md` §3.3에 있다.

| (`rq`, single) | 전 | 후 |
|---|---|---|
| Resnet50 EVTA | 115.27 ms (71 GOPS) | **78.08 ms (105 GOPS)** |
| Resnet50 추론 | 122.46 ms | **85.71 ms** |

11-05 하루, 두 단계(① + ②)로 147.9 → 85.7 ms(−42 %)가 됐다.

### 3.3 ④ fc를 EVTA로 (vtalib `fc_evta_plan`, `fc_packed_cpu`)

- **왜:** Resnet50 fc(2048 → 1000, shift 7)는 chunk당 출력 블록이 4개다(wgt 반쪽 512 entry / 입력 블록 128). 그래서 EVTA fc의 "chunk당 ≥ 8 블록" 조건에서 빠져 CPU 2.69 ms였다. 이 조건은 HLS GEMM의 acc hazard 때문이다(`gemm_fc`는 같은 acc가 g uop마다 다시 온다).
- **어떻게:**
  - VTASave는 크기와 상관없이 `fullyconnected_evta_packed`를 낸다.
  - systolic vtalib `fc_evta_plan`은 systolic 비트스트림에서는 1블록부터 받는다(systolic RMW에 forwarding이 있어 hazard가 없다).
  - plan이 맞지 않으면(HLS GEMM, 또는 다른 vtalib) packed weight로 CPU에서 같은 반올림과 clip으로 계산한다(`fc_packed_cpu`, 두 vtalib).
- **검증:** `EVTA_RES_CHECK` fc 3개(2048 → 1000, 512 → 512, 512 → 10)가 0 differ였다.

| | 전 | 후 |
|---|---|---|
| fc | 2.69 ms (CPU) | 0.44 ms (EVTA) |
| 추론 | 85.69 ms | **83.40 ms** |
| EVTA | 78.04 ms | 78.48 ms |

### 3.4 검증 중 찾은 vtalib 결함: bias entry가 thread 1을 덮음 (§7.19 ⑤)

- **발견:** VGG16을 HLS 비트스트림으로 돌리다 찾았다(`vgg16_optimization.md` §3.5).
- **원인:** `conv_kernel`은 bias를 thread 0의 tile 바로 뒤에 LOAD한다. 그런데 `nFilterinLoop`를 정할 때 그 자리를 빼지 않아, tile이 acc 절반을 꽉 채우면 bias가 thread 1의 첫 행을 덮었다.
- **수정:** 두 vtalib에서 bias 자리를 빼고 `nFilterinLoop`를 정한다.
- **확인:** HLS와 systolic의 3회 dump가 Resnet50 159개 모두 같다. systolic(bias reset 있음)에서는 원래 계산이 같았다.

### 3.5 ⑥ conv0 입력 복사를 conv와 겹침 (vtalib `InputStream`, `preprocess_conv0pack_stream`)

- **왜:** conv0 입력 복사(float quantize + 7×7/2 packing, 224×224×3 → 115×112×48)가 OMP 1에서 1.42 ms였다.
  - 보드에서 따로 재 보니 quantize만 0.47 ms, packing만 0.44 ms, 같은 크기 memset 0.06 ms였다.
  - DRAM이 아니라 NEON 명령 수가 한계였다(A53은 128-bit 연산이 2 cycle).
  - 명령을 다듬어도 1.25 → 1.14 ms 정도였다.
- **어떻게:** 줄이지 않고 숨겼다.
  - pynq driver `Run`을 `RunStart` + `RunWait`로 나눴다.
  - replay가 run마다 hook을 부른다.
  - conv0을 tile 행마다 run으로 나눠, run i가 도는 동안 CPU가 행 i+1의 입력을 채운다(double buffering).
- **검증:** systolic(stream 켬 / 끔)과 HLS의 3회 dump가 전날 기준과 같다(159개).

| | 입력 복사 + conv0 | 추론 |
|---|---|---|
| stream 끔 | 1.42 + 3.31 ms | 83.41 ms |
| stream 켬 | 3.56 ms | **82.21 ms** |

conv0이 0.25 ms 늘어난 것은 대부분 첫 run 전에 채우는 첫 행 묶음(115행 중 19행)이다.

### 3.6 conv DMA 묶기 (§7.29, vtalib-systolic `EVTA_CONV_BURST`)

- **왜:** Resnet50 1×1 conv의 LOAD / STORE가 짧은 전송으로 쪼개져 있었다.
  - weight는 K 블록마다, 스레드마다 256 byte짜리 행으로 나뉘었다. DRAM 배치가 [출력 블록][K 블록]이라, K 블록 하나의 출력 블록들이 떨어져 있기 때문이다.
  - 입력 / 출력 tile은 메모리에서 연속인데도 행 단위로 전송됐다.
  - load / store 모듈은 행마다 따로 전송해서, 짧은 행이 많으면 DRAM 지연이 그대로 드러난다.
- **어떻게:**
  - 1×1 conv(residual / pool / depthwise 제외)는 첫 입력 채널 블록에서 스레드의 weight 전체를 한 번에 LOAD한다(행 하나가 cBlocks × 256 byte). GEMM은 channel + kernel × cBlocks에서 weight를 읽는다(`gemmop_mxn` `wgt_kernel_factor_`).
  - LOAD / STORE의 행들이 DRAM과 SRAM 모두에서 연속이면 한 행으로 합친다.
  - 폭이 이미지 폭과 같은 tile은 출력 블록 전부를 STORE 하나로 보낸다.
- **검증:** `EVTA_DUMP_CONV` 106개가 burst 0 / 1에서 byte 단위로 같다. 분류는 281이다.

| | burst 0 | burst 1 |
|---|---|---|
| EVTA 호출 합 (conv median) | 79.79 ms (103 GOPS) | **67.17 ms (123 GOPS)** |
| 추론 | 82.20 ms | **69.56 ms** |

Resnet18(3×3이 대부분, −0.8 ms)보다 효과가 훨씬 컸다. Resnet50은 1×1 conv가 EVTA 시간의 대부분이기 때문이다.

## 4. multi4 (EVTA 4개, copy 4개 동시, §7.19 ⑦ ⑧)

- **CMA:** copy마다 weight를 VTA buffer에 올리면 4 × 25.6 MB = 102 MB가 되어, 128 MB CMA에 들어가지 않았다(단일 Resnet50도 queue 64 MB 포함 96 MB). 그래서 load_module의 상수 buffer를 내용이 같으면 process 안에서 하나로 공유한다(`VTAConstBufferAlloc`, `EVTA_CONST_SHARE=0`이면 copy마다). 실행 중 CMA 여유는 85 MB다.
- **함께 찾은 결함 둘:**
  - 새 main의 PNG 읽기가 단일 main과 달랐다(RGB 순서). `MODEL_PNG_REVERSE`로 맞췄다.
  - systolic vtalib `conv_kernel`에서 마지막 thread의 bias가 물리 acc(2048) 밖에 LOAD됐다. multi buffer에서만 드러났다.
- **multi4 실행 파일 위치:** `vta/multievta_bundles/MultiEVTAResnet50Test`, `vtabundles_xcmpl/gen_multi4_bundles.sh`

| (systolic rq multi4, OMP 1, copy별 25회 중앙값) | stream 끔 | stream 켬 |
|---|---|---|
| 4 copy 동시 | 86.88–87.49 ms | **85.68–86.26 ms** |
| copy 0 혼자 (multi4 비트스트림) | 84.32 ms | 83.09 ms |

**multi4가 단일보다 느린 두 원인(⑧):**
1. **copy 0 혼자인데도 단일 비트스트림보다 0.9 ms 느림(83.09 대 82.18 ms):**
   - schedule 때문이 아니다. 호출마다 ops가 같고, 차이가 conv마다 1–3 %씩 퍼져 있다. load 비중이 큰 stride 2 1×1 conv에서 가장 크다.
   - multi4는 Y219 timing 때문에 HP 포트 앞에 `axi_register_slice`가 하나 더 있다. 그만큼 load latency가 늘어난다.
2. **4 copy를 같이 돌리면 느려짐 (`MULTI_MASK`로 EVTA 조합별로 잼):**

| 돌린 EVTA | copy별 | 혼자 대비 |
|---|---|---|
| 하나씩 (0, 1, 2, 3) | 83.12 / 83.28 / 83.31 / 83.27 ms | |
| 0+1, 0+2, 0+3, 1+3, 2+3 | 83.95–84.22 ms | +0.85–0.95 ms |
| 1+2 | 84.56 / 84.53 ms | +1.25 ms |
| 0+1+2+3 | 85.65 / 86.25 / 86.24 / 85.79 ms | +2.5 / +2.95 ms |

- 어느 쌍이든 약 +0.9 ms다. DDR controller를 같이 쓰기 때문이다.
- 1+2만 0.35 ms가 더 붙는다. HP1과 HP2가 DDR port를 같이 쓰는 구조와 맞다.
- 한 추론의 EVTA DRAM 전송은 86.6 MB(weight 42.5, inp 30.5, out 12.1)다. EVTA 하나 평균 약 1 GB/s라 DDR 대역에는 한참 못 미친다. 대역이 아니라 load 구간의 latency가 경합으로 늘어나는 것이다.
- 4 copy 처리량은 단일의 3.87배다.

## 5. 지금 시간 분해 (11-10, single, OMP 1, 69.56 ms)

| 항목 | 시간 | 어디서 |
|---|---|---|
| `convolution_wo_tr` (residual 없는 conv) | 36.30 ms | EVTA |
| `convolution_wo_tr_res` (conv + elemadd + relu, 16개) | 27.28 ms | EVTA |
| `convolution_wo_tr_pool` (conv0 + maxpool) | 3.56 ms | EVTA |
| avgpool | 0.95 ms | CPU (ALU에 나눗셈 없음) |
| VTABufferInvalidate | 0.79 ms | CPU |
| fc (EVTA) | 0.44 ms | EVTA |
| softmax (float) | 0.06 ms | CPU |

EVTA가 97 %다. CPU에 남은 것은 avgpool과 invalidate 정도다.

## 6. 다음 후보

| 후보 | 내용 | 예상 | 비고 |
|---|---|---|---|
| burst 뒤 schedule 다시 찾기 | DMA 모양이 바뀌어 최적 tile이 달라졌을 수 있음 | 미측정 | `EVTA_SCHED` 탐색 다시 |
| avgpool을 EVTA로 | 7×7 평균 = 합 >> 6이 아니라 ÷49라 MUL이나 표가 필요 | −0.9 ms | ISA 개정(ALU opcode 확장, `deit_optimization.md` §6.2 H1) |
| residual 경로의 DMA 묶기 | 이번 burst는 residual conv(`_res`)의 weight 묶기를 제외함 | 미측정 | vtalib |
| conv0 첫 행 묶음 | 첫 tile 행을 더 잘게 나눔 | 약 −0.2 ms | vtalib |
| multi4 하드웨어 | Y219 fence가 필요 없는 보드에서 register slice 빼기, EVTA 1이나 2를 HPC 포트로, HP read outstanding 늘리기 | copy당 1–3 ms | 비트스트림 |
| 한 장을 EVTA 4개로 | 층마다 출력 채널을 나눔 | 1장 약 69 → 20 ms대 | runtime (`unified_runtime_plan.md`) |
| GEMM 32×32 | MAC 4배 | EVTA 약 2–2.5배 | weight 대역에 막힘 |

## 7. 재현

```bash
cd vtabundles_xcmpl
sudo hw/script/run_prof.sh -s _systimed -r Resnet50Test      # 1장, 1코어
sudo hw/script/run_prof.sh -s _sysprof -r Resnet50Test       # EVTA 호출별
sudo /bin/sh -c "hw_multievtax4/script/run_prof.sh -s _sysprof -n 25 -l -r MultiEVTAResnet50Test"
```

끄는 방법: `EVTA_CONV_BURST=0`, `EVTA_INPUT_STREAM=0`, `EVTA_SCHED='<모양>:1,14,14'`(이전 schedule), `EVTA_CONST_SHARE=0`(multi4). residual fusion은 번들 생성 옵션 `-vta-fuse-residual=false`로 끈다.
