# VGG16 (CIFAR-10) 실행 시간을 줄여 온 과정 (2026-11-05 ~ 11-10)

ZCU102 위 EVTA와 NEST-C로 VGG16 CIFAR-10(32×32 입력) 1장을 줄인 과정이다. 처음 잰 값은 16.1 ms(systolic 400 MHz 기본 비트스트림, 1코어)였고, 지금은 **7.0 ms**다. 단계마다 왜 바꿨는지, 어떻게 바꿨는지, 얼마나 줄었는지, 어떻게 검증했는지를 적었다.

- **자세한 기록:** `docs/evta_runtime_compare.md` §7.19, §7.29
- **Resnet18 과정:** `docs/resnet18_optimization.md`. VGG16은 Resnet18에서 만든 컴파일러 / runtime 기능을 그대로 물려받았다.

## 0. 한눈에

| 날짜 | 단계 | 1장 OMP 1 | EVTA 바쁜 시간 | 바꾼 곳 | § |
|---|---|---|---|---|---|
| 11-05 | 처음 잼 (기본 400 MHz 비트스트림) | 16.11 ms | 15.82 ms (40 GOPS) | – | §7.19 |
| 11-05 | 하드웨어 세 단계 (ALU II = 1, bias reset, requantize) | 15.51 ms | 15.23 ms (41 GOPS) | 비트스트림 | §7.19 |
| 11-05 | ② VGG16 모양의 schedule | 13.59 ms | 13.30 ms (47 GOPS) | schedule | §7.19 ② |
| 11-05 | ③ 7×7보다 작은 출력: vt 2 결함 수정, systolic 최소 tile 1 | **7.33 ms** | **7.03 ms (89 GOPS)** | 두 vtalib + schedule | §7.19 ③ |
| 11-05 | ④ fc를 EVTA로 | 7.28 ms | | 컴파일러 + vtalib | §7.19 ④ |
| 11-06 | ⑥ conv0 입력 복사를 conv와 겹침 | 7.29 → 7.27 ms | | vtalib runtime | §7.19 ⑥ |
| 11-10 | conv DMA 묶기 (`EVTA_CONV_BURST`) | 7.27 → **7.01 ms** | conv 6.78 → 6.52 ms | vtalib | §7.29 |

- 1코어 기준 16.1 → 7.0 ms로 **2.3배** 빨라졌다.
- 분류는 airplane(0)이고, 확률 0.453709는 모든 단계에서 그대로다. HLS 결함을 고친 뒤 HLS / 시뮬레이터 값도 같아졌다(§3.5).
- 연산량은 627 Mops다. 하한은 3.06 ms이고, 지금 EVTA 호출 합 6.52 ms는 약 96 GOPS, 하한의 47 %다.

## 1. 측정 조건

| 항목 | 내용 |
|---|---|
| 모델 | VGG16 CIFAR-10(`vgg16_cifar10.onnx`), 1×3×32×32, int8 power-of-2 |
| 번들 | `vta/bundles/Vgg16Cifar10Test` → `vtabundles_xcmpl/hw/Vgg16Cifar10Test` |
| 하드웨어 | ZCU102, single EVTA, systolic 400 MHz `..._sa_400mhz_rq.bin` |
| 측정 | `run_prof.sh -s _systimed -r` / `-s _sysprof -r`, OMP 1, 25장 중 runs 2–25 중앙값 |

## 2. 출발점 (11-05)

| EVTA 시간 (`_sysprof`) | `sa_400mhz` | `alu1_epr` | `bias` | `rq` | 합 |
|---|---|---|---|---|---|
| Resnet18 (3,628 Mops) | 26.43 ms (137 GOPS) | 24.89 | 24.15 | 23.05 ms (157 GOPS) | −12.8 % |
| **VGG16 CIFAR-10 (627 Mops)** | 15.82 ms (40 GOPS) | 15.60 | 15.46 | **15.23 ms (41 GOPS)** | −3.8 % |

| 추론 한 장 (`_systimed`) | `sa_400mhz` | `rq` | EVTA 밖 (`rq`) |
|---|---|---|---|
| VGG16 CIFAR-10 | 16.11 ms | 15.51 ms | 0.2 ms |

- **CPU 몫:** 0.2 ms뿐이었다. maxpool 4개는 EVTA로 갔고, 1개(`MaxPool_30`)와 fc 1개만 CPU였다.
- **하드웨어 세 단계의 효과:** −3.8 %로 가장 작았다. 시간이 ALU가 아니라 다른 곳에 쓰이고 있었다.
- **원인: 뒤쪽 conv의 weight LOAD**
  - 2×2 출력의 512 → 512 conv 3개가 각 1.92 ms(9.8 GOPS)였다.
  - 4×4의 conv 3개는 1.0–1.95 ms(38 GOPS)였다.
  - weight 2.36 MB를 1.92 ms에 읽는 셈이라 약 1.2 GB/s다. HP 포트 대역(333 MHz × 16 B = 5.3 GB/s)보다 훨씬 느리다.
- 공간이 작은데(2×2, 4×4) weight가 큰 모양은 Resnet18에 없었다.

## 3. 단계별로 뭘 바꿨나

### 3.1 ② VGG16 모양의 schedule (`include/Backends/VTA/VTASchedules.h`)

- **왜:** `VTASchedules.h`에는 Resnet18 모양만 있었다. 그래서 VGG16의 conv가 전부 기본값(virtual thread 1, tile 14×14)이었다. vt 1이면 다음 입력 채널의 LOAD가 앞 GEMM을 기다린다.
- **어떻게:** Resnet50과 같은 방법으로 찾았다(`resnet50_optimization.md` §3.2). vtalib-systolic `EVTA_SCHED`로 후보를 바꿔 끼우고 `_sysprof`로 쟀다.
- **처음 확인에서 찾은 문제:** 출력이 7×7보다 작은 conv(4×4, 2×2)에서 vt 2가 틀렸다. 그래서 그 둘은 일단 기본값으로 뒀다. 원인은 §3.2에서 찾았다.

| | 전 | 후 |
|---|---|---|
| EVTA | 15.23 ms (41 GOPS) | **13.30 ms (47 GOPS)** |
| 추론 | 15.51 ms | **13.59 ms** |

### 3.2 ③ 7×7보다 작은 출력: vt 2 결함과 최소 tile (두 vtalib)

**원인 1: virtual thread 2가 틀린 이유.**
- vtalib `conv_kernel`은 출력 tile이 `MIN_H_SIZE` / `MIN_W_SIZE`(7)보다 작으면 더미 행·열을 붙여 7×7로 계산한다. 그리고 STORE(와 fused residual의 LOAD)를 행마다 나눠 낸다.
- 이 경로에서 출력 채널 블록의 식이 `((filterIdx + fIdx)·vt + thread)`였다. 맞는 식은 다른 경로와 같은 `(filterIdx·vt + thread)·nFilterinLoop + fIdx`다.
- vt 1이고 filter 루프가 한 번이면(작은 출력에서는 대개 그렇다) 두 식이 같아서, 그동안 드러나지 않았다.
- acc 쪽 행 offset도 `fIdx·outDummyH·resizedOutTileH`를 `…TileW`로 고쳤다. H와 W를 같은 7로 늘릴 때만 둘이 같다.
- `vta/vtalib`(020d041)와 `vtalib-zcu102-systolic` 둘 다 고쳤다.

**원인 2: 왜 하필 7×7인가.**
- HLS GEMM(`vta.cc` `gemm`)은 `DEPENDENCE variable=acc_mem inter false`라 acc 읽기-더하기-쓰기에 hazard 검사가 없다.
- 3×3 conv의 uop 순서에서는 같은 acc 주소가 `tile_h·tile_w` uop마다 다시 온다. 그 간격을 파이프라인 깊이보다 길게 하는 것이 최소 tile이다.
- systolic RTL `sa_rmw.v`는 처리 중인 dst(≤ 3)를 forwarding하므로 최솟값이 필요 없다. Gemmini는 같은 경우 `AccumulatorMem`에서 stall한다.
- 남는 것은 weight를 바꿀 때 16클럭 채우기뿐이다. 그래서 2×2 출력은 weight 블록당 49 → 16클럭이 되고, ALU는 49 → 4 entry가 된다.

**바꾼 것:**
- systolic vtalib의 최소 tile은 systolic 비트스트림(`/tmp/evta_bitstream`에 `_sa_`)에서 1, 아니면 7이다(`EVTA_MIN_TILE=<n>`으로 바꿀 수 있다).
- `VTASchedules.h`에 VGG16 4×4 / 2×2 conv 세 개를 vt 2로 다시 넣었다.
- HLS vtalib(`vta/vtalib`)은 7 그대로다.

**검증:**
- 같은 실행 파일에서 기본 / vt 2 / 최소 1 / 최소 1 + vt 2의 VGG16 층 dump 51개가 모두 같다.
- Resnet50(159개)과 Resnet18(60개)도 최소 1과 기본이 같다.
- 분류는 그대로다.

| (`rq`, single) | ② 후 | ③ 후 |
|---|---|---|
| VGG16 EVTA | 13.30 ms (47 GOPS) | **7.03 ms (89 GOPS)** |
| VGG16 추론 | 13.59 ms | **7.33 ms** |
| Resnet50 EVTA / 추론 | 78.08 / 85.71 ms | 78.04 / 85.69 ms (7×7 미만 tile 없음) |
| Resnet18 EVTA / 추론 | 23.05 / 25.53 ms | 22.99 / 25.42 ms |

VGG16 추론은 11-05 하루에 15.5 → 7.3 ms(−53 %)가 됐다. 최소 1에서 schedule을 다시 찾지는 않았다(vt 2, tile = 출력 전체만 재 봄).

### 3.3 ④ fc를 EVTA로

- **왜:** VGG16의 마지막 fc(512 → 10)는 출력 블록이 1개라 "chunk당 ≥ 8 블록" 조건에서 빠져 CPU로 갔다.
- **어떻게:** systolic 비트스트림에서는 1블록부터 EVTA로 보낸다. 조건이 맞지 않으면 packed weight로 CPU가 같은 산술로 계산한다(`fc_packed_cpu`). 자세한 내용은 `resnet50_optimization.md` §3.3에 있다.
- **검증:** `EVTA_RES_CHECK`로 512 → 512, 512 → 10 fc가 0 differ였다.
- **결과:** 추론 7.33 → **7.28 ms**

### 3.4 ⑥ conv0 입력 복사를 conv와 겹침

- VGG16의 conv0 입력은 32×32라 작다. 입력 복사 + conv0이 0.05 + 0.11 → 0.14 ms가 됐다.
- 추론은 7.29 → 7.27 ms로 거의 그대로다. 방법은 `resnet50_optimization.md` §3.5와 같다.

### 3.5 검증 중 찾은 vtalib 결함: bias entry가 thread 1을 덮음 (§7.19 ⑤)

- **발견:** HLS 비트스트림에서 VGG16 확률이 0.4247이었다(systolic은 0.4537). 시뮬레이터도 0.4247이었다(같은 vtalib).
- **원인:** `conv_kernel`은 thread마다 acc 절반(1024)에 tile × `nFilterinLoop` 행을 두고, bias를 그 바로 뒤(`base + T·nF`)에 LOAD한다.
  - `nFilterinLoop`는 `1024 / T`까지 허용해서 bias 자리를 빼지 않았다.
  - T·nF가 절반을 꽉 채우면, thread 0의 bias LOAD가 thread 1의 첫 행(GEMM이 끝난 결과)을 덮는다.
  - VGG16 conv0(vt 2, 16×32, nF 2), Conv_5 / Conv_7(16×16, nF 4)이 이 경우였다. dump 차이도 thread 1의 tile마다 첫 nF 픽셀에만 있었다.
  - systolic rq / bias 비트스트림은 bias reset이 GEMM 전에 thread 1 영역을 다시 쓰므로 결과가 맞았다.
- **수정(두 vtalib):** bias reset이 없을 때 `nFilterinLoop ≤ 1024 / (T + 1)`로 정한다. T는 최소 tile로 늘린 뒤의 크기를 쓴다. 늘리기 전 크기로 정하면 큰 KN에서 넘칠 수 있었다.
- **검증:** VGG16 시뮬레이터 / HLS / systolic dump 17개가 모두 같고, 셋 다 0.453709다. HLS와 systolic의 3회 dump가 VGG16 51, Resnet50 159, Resnet18 60개 모두 같다.

### 3.6 conv DMA 묶기 (§7.29, `EVTA_CONV_BURST`)

- **어떻게:** 연속인 LOAD / STORE 행을 한 행으로 합치고, 1×1 conv의 weight를 한 번에 읽는다(`resnet50_optimization.md` §3.6).
- VGG16은 3×3 conv뿐이라 1×1 weight 묶기는 해당이 없다. 연속 행 합치기만 효과가 있었다.
- **검증:** `EVTA_DUMP_CONV` 34개가 burst 0 / 1에서 같다.

| | burst 0 | burst 1 |
|---|---|---|
| EVTA 호출 합 | 6.78 ms (93 GOPS) | **6.52 ms (97 GOPS)** |
| 추론 | 7.27 ms | **7.01 ms** |

## 4. multi4 (§7.19 ⑦)

| (systolic rq multi4, OMP 1, copy별 25회 중앙값) | stream 끔 | stream 켬 |
|---|---|---|
| 4 copy 동시 | 8.18–8.51 ms | 8.19–8.53 ms |
| copy 0 혼자 | 7.38 ms | 7.37 ms |

- 4 copy를 같이 돌리면 copy마다 +0.8–1.1 ms가 붙는다. DDR 경합 때문이다(`resnet50_optimization.md` §4).
- 실행 파일 위치: `vta/multievta_bundles/MultiEVTAVgg16Cifar10Test`
- **multi4에서 찾은 결함:** systolic multi4에서만 VGG16 conv0(vt 2 / 16×32)이 틀렸다. 마지막 thread의 bias가 multi buffer의 물리 acc(2048) 밖에 LOAD됐기 때문이다. bias 자리를 남기도록 고쳤다.

## 5. 지금 시간 분해 (11-10, single, OMP 1, 7.01 ms)

| 항목 | 시간 | 어디서 |
|---|---|---|
| `convolution_wo_tr` (conv 13개) | 6.52 ms | EVTA |
| `maxpool_evta` | 0.30 ms | EVTA |
| fc (EVTA) | 0.09 ms | EVTA |
| 그 밖 | 약 0.1 ms | CPU |

## 6. 다음 후보

| 후보 | 내용 | 예상 | 비고 |
|---|---|---|---|
| 최소 tile 1에서 schedule 다시 찾기 | ③ 뒤에는 vt 2, tile = 출력 전체만 재 봄 | 미측정 | `EVTA_SCHED` 탐색 |
| 작은 출력 conv의 weight 재사용 | 2×2 / 4×4 conv는 tile 하나에 weight 전부라 LOAD가 대부분. weight 2.36 MB의 실효 대역을 높이기(burst 길이, 두 thread가 weight를 나눠 읽기) | 뒤쪽 conv 수 ms 중 일부 | `EVTA_REPLAY_LOG=2`로 DRAM byte부터 확인 |
| conv + maxpool 합치기 | maxpool 0.3 ms | −0.2 ms | Resnet18 conv0 + pool과 같은 방식(`convolution_wo_tr_pool`) |
| multi4 하드웨어 | register slice, HP 포트 | copy당 약 1 ms | `resnet50_optimization.md` §4 |

## 7. 재현

끄는 방법은 세 종류다. `-vta-*`는 번들 생성(`model-compiler`) 옵션이라 번들을 다시 만들어야 하고, `EVTA_*`는 실행할 때 주는 환경변수(`sudo /bin/sh -c "EVTA_X=0 hw/script/run_prof.sh ..."`), 하드웨어 단계는 `BIN=<비트스트림>`이다. 자세한 절차와 vtalib마다 읽는 변수의 차이는 `resnet18_optimization.md` §9.2–§9.5에 있다.

```bash
cd vtabundles_xcmpl
sudo hw/script/run_prof.sh -s _systimed -r Vgg16Cifar10Test
sudo hw/script/run_prof.sh -s _sysprof -r Vgg16Cifar10Test
sudo /bin/sh -c "hw_multievtax4/script/run_prof.sh -s _sysprof -n 25 -l -r MultiEVTAVgg16Cifar10Test"
```

끄는 방법: `EVTA_CONV_BURST=0`, `EVTA_MIN_TILE=7`, `EVTA_INPUT_STREAM=0`, `EVTA_SCHED='<모양>:1,14,14'`
