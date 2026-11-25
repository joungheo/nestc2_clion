# MobileNetV2Tf multi4: 소프트웨어로 줄인 시간 (2026-11-25)

DPU S(B4096 하나, 270장/s)를 EVTA 4개(`MultiEVTAMobileNetV2TfTest`)로 넘으려고, 2026-11-25에 하드웨어를 바꾸지 않고 소프트웨어만 고친 기록이다.
- 측정과 단계별 자세한 표는 [evta_dpu_compare.md](evta_dpu_compare.md) §6.7–§6.10에 있다. 여기서는 무엇을 왜 바꿨고 얼마나 줄었는지를 한곳에 모은다.
- 모델: TF-slim float MobileNetV2(`mobilenetv2_tf_f32.onnx`, DPU `tf_mobilenetv2_1.0`의 float 모델). 입력 224 × 224 × 3, 연산량 0.658 GOP.
- 보드: ZCU102. bitstream은 multi4 `..._4evta_sa_amul_400mhz_hp322_rq.bin`, single `..._sa_alux3_400mhz_rq.bin`이다(둘 다 systolic 400 MHz).
- 시간: `run_prof.sh -s _systimed` 계열, 25회, run 2부터의 중앙값. "4개 동시"는 copy 넷이 한 장씩 함께 도는 처리량이다(lockstep 없음, OMP 1).

## 1. 요약

| 단계 | 무엇을 | 혼자 1장 | 4개 동시 | commit |
|---|---|---|---|---|
| 출발 (§6.7) | 11-22 bundle + 새 schedule(`EVTA_SCHED`) | 14.84 ms | 238.3 fps | |
| conv0 입력 묶기 (§6.8) | float → int8 + 묶기를 NEON으로 다시 씀 | 14.35 ms | 246.5 fps | `b5c4e7d` |
| invalidate 빼기 (§6.9) | EVTA만 읽는 출력의 cache invalidate를 안 함 | 13.85 ms | 254.5 fps | `0b1439b` |
| 지금 컴파일러로 bundle 다시 만듦 | 위 둘 + schedule이 bundle 안에 | 13.43 ms¹ | **251.6 fps**² | 보드 bundle 19:00 |
| int8 입력 (§6.10) | 미리 quantize한 `.i8`을 읽음(선택, 기본은 float) | 13.79 ms³ | **257.0 fps**² | `d4d8ddf2` |

1. single bitstream(`_alux3`) 위의 `MobileNetV2TfTest`. 나머지 "혼자"는 multi4 bitstream 위 copy 0(`singlevta`)다. 같은 bundle이라도 bitstream에 따라 0.4–0.5 ms 다르다.
2. 같은 때 번갈아 3번씩 잰 평균(§4). DPU S 270장/s의 93.2 %(float), 95.2 %(int8)다.
3. multi4 bitstream 위 copy 0, float 14.01 ms와 같은 때 잼.

- 하루 동안 multi4가 238.3 → 251.6 fps(+5.6 %, float 입력), int8 입력이면 257.0 fps(+7.8 %)다.
- 모든 단계에서 출력 바이트가 같다. EVTA conv 출력 dump, `EVTA_RES_CHECK`, ImageNet val 출력 hash(200장, 1,000장)로 확인했다. 분류는 cat_285가 계속 282 (0.322088)이다.

## 2. 어디에 시간이 갔나 (출발점, §6.7)

`timing_<cpp>.csv`(문장별 벽시계)와 `EVTA_REPLAY_LOG=2`(EVTA run마다 시간, GEMM / ALU 반복 수, DRAM 바이트)로 나눴다.

| | 혼자 | 4개 동시 copy |
|---|---|---|
| conv(1×1 / 3×3, conv0 포함) | 7.89 ms | 8.83–9.01 ms |
| depthwise | 5.95 ms | 6.29–6.37 ms |
| `VTABufferInvalidate` | 0.48 ms | 0.50 ms |
| fc | 0.29 ms | 0.32–0.36 ms |

conv 쪽에서 GEMM 하한(GEMM 반복 / 400 MHz) 밖의 시간이 소프트웨어로 줄일 수 있는 몫이었다.

| 무엇 | 시간(혼자) | 왜 |
|---|---|---|
| conv0 입력 묶기 | 1.31 ms (stream으로 0.5 ms만 겹침) | float 600 KB를 읽어 quantize하고 3×3 / 2 conv용으로 재배치 |
| invalidate | 0.48 ms | EVTA 출력마다 `dc civac` 루프. 다음 EVTA만 읽는 출력에도 불렀다 |
| 호출마다 CPU 쪽 | 1.3 ms (conv 0.89 + depthwise 0.39) | 호출 54개 × 20–25 us |

## 3. 바꾼 것

### 3.1 conv0 입력 묶기 (`b5c4e7d`, §6.8)
- **문제:** conv0(3×3 / 2, 3채널)의 입력은 CPU가 float을 int8로 quantize하고, EVTA가 읽을 수 있게 다시 배치한다(18 / 32채널, `preprocess_conv0pack_stream`). 이 묶기가 conv0의 EVTA run보다 길었다.
- **바꾼 것**(`vtalib-zcu102-systolic/lib/Bundle/CPUBundle_generic.cpp`):
  - pixel마다 `vqtbl4q` 두 번 하던 묶기를 `vext` + `vbsl` / `vand`로 바꿨다.
  - 같은 plane의 다음 줄을 prefetch한다.
  - offset 0, scale이 2의 거듭제곱이면 `vaddq`를 뺀다.
- **결과:** 묶기 1.23 → 0.73 ms(bench 1.24 → 0.86). conv0 호출이 1.66 → 0.97 ms다. 혼자 돌 때는 이제 EVTA run에 묶이지만, 4개 동시이면 DRAM을 함께 써서 묶기가 1.0 ms로 늘어 여전히 CPU에 묶인다.

### 3.2 EVTA만 읽는 출력의 invalidate 빼기 (`0b1439b`, §6.9)
- **문제:** 보드는 cache coherent가 아니다. 그래서 bundle은 EVTA 출력마다 `VTABufferInvalidate`를 불렀다. 이것은 CPU가 그 출력을 읽을 때만 필요하다.
- **바꾼 것:** VTASave `-vta-skip-evta-invalidate`(기본 켬). 생성된 코드를 줄 단위로 보고, 출력을 free할 때까지 EVTA 호출만 그 buffer를 읽으면 invalidate를 내보내지 않는다. 그 사이에 CPU 함수나 대체 경로가 하나라도 있으면 남긴다.
- **결과:** MobileNetV2 52 → 1개, Resnet18 21 → 1, VGG16 17 → 1, DeiT 75 → 75(호출이 허용 목록 밖). invalidate 시간 0.48 → 0.005 ms.
- **확인:** 보드에서 model-compiler를 다시 빌드해 MobileNetV2(QAT / Tf), Resnet18, VGG16 bundle을 옵션을 켜고 끄고 만들었다. 차이는 invalidate 줄과 옵션 주석뿐이고, conv dump는 같고, `EVTA_RES_CHECK`는 0 differ다.

### 3.3 bundle 다시 만들기 (보드, 19:00)
- 보드의 MobileNetV2TfTest / MultiEVTAMobileNetV2TfTest bundle을 지금 컴파일러로 다시 만들고, `_sys` / `_systimed` 실행 파일을 다시 빌드했다.
- 들어간 것: invalidate 빼기, ubuntu2가 4개 동시 실행에서 다시 찾은 112×112 schedule 세 개(`744b1fb`, 전에는 `EVTA_SCHED`로 넣었다).
- 이전 실행 파일과 바이트가 같다: dump 208개, `EVTA_RES_CHECK` 228개, ImageNet val 1,000장 hash, top-1 680 / 1000.
- 이전 bundle과 실행 파일은 `~xilinx/JO/ubun_wt/old_mbv2tf/`에 있다.

### 3.4 int8 입력 (`d4d8ddf2`, §6.10)
- **문제:** float 입력을 받는 한 묶기는 600 KB를 읽고 quantize해야 한다. DPU 쪽 성능 수치는 미리 준비한 입력으로 잴 수 있다(확인 필요).
- **바꾼 것:** 입력을 float(기본)과 int8 중에서 고른다.
  - `INPUT_INT8_SAVE=<dir>`이면 실행 파일이 bundle의 quantize(scale 1/128, offset 0)로 그림마다 `<name>.i8`을 쓴다. 묶기와 같은 함수라 바이트가 같다.
  - `.i8`을 입력으로 주면 묶기는 바이트만 옮긴다. scale / offset / table이 bundle과 다르면 exit 6이다.
  - `run_prof.sh`의 `PROF_INPUT=<file>`로 잰다.
- **결과:** 묶기 0.88 → 0.37 ms(혼자), 1.02–1.09 → 0.59–0.61 ms(4개 동시). 4개 동시 처리량 +2.3 %다. ImageNet val 200장의 출력 hash가 float 입력과 같다.

## 4. 측정의 흔들림

같은 실행 파일도 때에 따라 몇 fps씩 다르다. 그래서 비교는 같은 때 번갈아 잰 값으로만 한다.
- §6.9의 254.5 fps(오후)와 같은 코드가 저녁에는 247–248 fps였다. 바꾸기 전 vtalib(`167ab44`)로 다시 빌드해도 같았다(묶기 0.88 ms, 1장 14.016 ms). 그래서 코드가 아니라 보드 상태 차이로 본다.
- 다시 만든 bundle의 첫 측정 250.7 fps가 이전 252–253 fps보다 낮아 보였다. 번갈아 3번씩 다시 재 보니 차이가 없었다.

  | 4개 동시 (fps) | 1 | 2 | 3 | 평균 |
  |---|---|---|---|---|
  | 공유 `_systimed` (보드 빌드), float | 250.4 | 252.1 | 252.2 | 251.6 |
  | 따로 빌드 `_pcsysubun` (같은 bundle), float | 251.3 | 250.9 | 251.9 | 251.4 |
  | 공유 `_systimed`, int8 입력 | 256.4 | 257.3 | 257.4 | 257.0 |

- 앞의 252.2 fps는 한 번만 잰 값이고, 253.2 fps는 int8 입력(그때 bundle) 값이다. 한 번 잰 값끼리의 ±1–2 fps는 잡음 안이다.

## 5. 남은 것

- **호출마다 CPU 쪽 시간**(§6.7의 3번, 한 장 약 1.3 ms) — 아래 §6에 이어 적는다.
- **depthwise**(copy당 약 6 ms, 연산에 묶임): ubuntu2가 depthwise 장치를 만들고 있다. tap 9개를 한 번에 하고 채널별 shift를 넣으면 2–2.5 ms가 된다는 계산이다(§6.7).
- **7×7 project conv 세 개**는 weight LOAD에 묶여 있다(1.02 ms 대 GEMM 하한 0.30 ms).
