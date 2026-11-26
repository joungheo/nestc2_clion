# MobileNetV2Tf multi4: 소프트웨어로 줄인 시간 (2026-11-25, 하드웨어 몫 §9: 11-26)

DPU S(B4096 하나, 270장/s)를 EVTA 4개(`MultiEVTAMobileNetV2TfTest`)로 넘으려고, 2026-11-25에 하드웨어를 바꾸지 않고 소프트웨어만 고친 기록이다.
- 그 사이 ubuntu2가 depthwise 창 장치(`_dw`)를 bitstream에 넣었다(multi4 11-25 밤, single 11-26). 하드웨어로 줄어든 몫과 single에서의 효과는 §9에 따로 모은다.
- 측정과 단계별 자세한 표는 [evta_dpu_compare.md](evta_dpu_compare.md) §6.7–§6.10에 있다. 여기서는 무엇을 왜 바꿨고 얼마나 줄었는지를 한곳에 모은다.
- 모델: TF-slim float MobileNetV2(`mobilenetv2_tf_f32.onnx`, DPU `tf_mobilenetv2_1.0`의 float 모델). 입력 224 × 224 × 3, 연산량 0.658 GOP(EVTA에 보내는 conv / depthwise / fc의 2 × MAC, `run_prof.sh`가 찍는 값). GOPS = 0.658 × fps.
- 보드: ZCU102. bitstream은 multi4 `..._4evta_sa_amul_400mhz_hp322_rq.bin`, single `..._sa_alux3_400mhz_rq.bin`이다(둘 다 systolic 400 MHz).
- 시간: `run_prof.sh -s _systimed` 계열, 25회, run 2부터의 중앙값. "4개 동시"는 copy 넷이 한 장씩 함께 도는 처리량이다(lockstep 없음, OMP 1).
- 혼자 1장 시간 옆의 fps = 1000 / ms, GOPS = 한 장의 GOP × fps(MobileNetV2 / MobileNetV2Tf 0.658 GOP, 다른 모델은 §8).

## 1. 요약

| 단계 | 무엇을 | 혼자 1장 | 4개 동시 | commit |
|---|---|---|---|---|
| 출발 (§6.7) | 11-22 bundle + 새 schedule(`EVTA_SCHED`) | 14.84 ms · 67.4 fps (44 GOPS) | 238.3 fps (157 GOPS) | |
| conv0 입력 묶기 (§3.1) | float → int8 + 묶기를 NEON으로 다시 씀 | 14.35 ms · 69.7 fps (46 GOPS) | 246.5 fps (162 GOPS) | `b5c4e7d` |
| invalidate 빼기 (§3.2) | EVTA만 읽는 출력의 cache invalidate를 안 함 | 13.85 ms · 72.2 fps (48 GOPS) | 254.5 fps (167 GOPS) | `0b1439b` |
| bundle 다시 만듦 (§3.3) | 위 둘 + schedule이 bundle 안에 | 13.43 ms · 74.5 fps (49 GOPS)¹ | 251.6 fps (166 GOPS)² | 보드 19:00 |
| int8 입력 (§3.4) | 미리 quantize한 `.i8`을 읽음(저녁부터 기본, 없으면 float) |  | 257.0 fps (169 GOPS)² | `d4d8ddf2`, `9680e43f` |
| 호출마다 CPU 시간 (§6) | 명령 사본, thread별 buffer pool, 다음 replay 짐작 |  | 262.2 fps (173 GOPS)² | `1cdf422d` |
| 창 장치 `_dw` (ubuntu2, [evta_dw_unit.md](evta_dw_unit.md) §9) | depthwise 3×3를 창 장치로 (multi4 `..._amul_dw_..._hp304_rq.bin`) |  | 284.9 fps (187 GOPS)⁴ | `818a73c3` |
| 1×1 weight 한 번 LOAD (§7) | 7×7 project conv 세 개의 weight를 thread마다 한 LOAD로 | 11.77 ms · 85.0 fps (56 GOPS)³ | **292.0 fps (192 GOPS)**⁴ | `38e3da66` |
| 11-25 저녁 (공유 `_systimed`, 번갈아 3번) | int8 입력 | **13.04 ms · 76.7 fps (50 GOPS)**¹ / 13.54 ms · 73.9 fps (49 GOPS)³ | **263.5 fps (173 GOPS)** | |
| | float 입력(`INPUT_INT8=0`) | 13.28 ms · 75.3 fps (50 GOPS)¹ / 13.77 ms · 72.6 fps (48 GOPS)³ | 256.4 fps (169 GOPS) | |

1. single bitstream(`_alux3`) 위의 `MobileNetV2TfTest`.
2. 그 단계를 잴 때 같은 때 번갈아 3번씩 잰 평균(§4, §6.3). 위아래 줄의 측정 때가 달라서 단계끼리는 ±2 fps쯤 흔들린다.
3. multi4 bitstream 위 copy 0(`singlevta`). 같은 bundle이라도 bitstream에 따라 0.4–0.5 ms 다르다. §7 줄은 `_dw` bitstream 위다.
4. 같은 실행 파일(`_pcsysubun` timed 빌드)로 `EVTA_WGT_FIT=0/1`을 번갈아 4번씩 잰 평균(§7.3). 11-25 저녁 줄과는 bitstream, 빌드가 다르다.

- 하루 동안 4개 동시 처리량이 238.3 → 263.5 fps(157 → 173 GOPS, +10.6 %, int8 입력)이다. float 입력이면 256.4 fps(169 GOPS, +7.6 %)다.
- DPU S 270장/s와 견주면 int8 입력이 97.6 %, float 입력이 95.0 %다. DPU 수치가 미리 int8로 만든 입력으로 잰 것인지는 확인하지 못했다.
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

- **호출마다 CPU 쪽 시간**(§6.7의 3번) — §6에서 줄였다. 남은 것은 호출 사이 bundle 코드(한 장 0.27 ms, 4개 동시 0.44 ms)와 run 시작(0.06 ms)이다.
- **depthwise**(copy당 약 6 ms, 연산에 묶임): ubuntu2가 depthwise 장치를 만들고 있다. tap 9개를 한 번에 하고 채널별 shift를 넣으면 2–2.5 ms가 된다는 계산이다(§6.7).
- ~~**7×7 project conv 세 개**는 weight LOAD에 묶여 있다(1.02 ms 대 GEMM 하한 0.30 ms).~~ §7에서 weight를 thread마다 한 번에 LOAD하게 했다(0.99 → 0.63 ms).
- 호출 사이 bundle 코드, 장 사이 구간은 §7.1에서 보았다. 줄일 것이 1 % 안팎이라 손대지 않았다.

## 6. 호출마다의 CPU 시간 (`1cdf422d`)

### 6.1 나눠 잼 (`EVTA_CALL_TIME=1`, `9680e43f`)
- vtalib-systolic `cmd_queue.h` `CallTimes`: replay된 EVTA 호출마다 시간을 아래로 나눠 thread가 끝날 때 replay id별로 찍는다(`EVTA_CALL_TIME` 줄).

  | 이름 | 구간 |
  |---|---|
  | bundle | 앞 호출이 끝난 때 → vtalib conv 진입(`VTACallTimeMark`). bundle 코드, buffer alloc / free, epilogue 설정, `_timed`의 시간 재기 |
  | pre | conv 진입 → `ReplayBegin`(key 만들기 등) |
  | find | `ReplayBegin` → replay를 찾음(key 문자열, hash, buffer 주소가 바뀌었으면 명령 고치기 + flush) |
  | start | `VTADeviceRunStart`(레지스터 쓰기) |
  | hook | segment hook(conv0 입력 묶기, EVTA run과 겹침) |
  | wait | `VTADeviceRunWait`(EVTA run) |

- MobileNetV2Tf, int8 입력, 한 장(호출 54개)당:

  | | 혼자 | 4개 동시 copy당 |
  |---|---|---|
  | bundle | 0.29 ms | 0.49–0.57 ms |
  | pre | 0.02 ms | 0.03 ms |
  | **find** | **0.24 ms** | **0.54–0.71 ms** |
  | start | 0.06 ms | 0.06 ms |
  | wait | 12.9 ms | 13.5–13.8 ms |

- §6.7에서 본 "호출마다 20–25 us"(벽시계 − EVTA run)는 invalidate(§3.2)를 뺀 뒤 이렇게 남았다. EVTA run 밖의 CPU 시간은 혼자 한 장 0.6 ms, 4개 동시 copy당 1.1–1.3 ms다.
- **find가 큰 까닭:**
  - 같은 모양의 호출은 replay 하나를 같이 쓴다(40개 id, 호출 54개). buffer가 다르면 그때마다 명령의 주소(relocation)를 고치고 명령 전체를 flush했다. 혼자일 때 한 장에 44번이고, 한 번에 5–10 us다.
  - 4개 동시이면 buffer pool이 copy 넷에 하나라서 copy들이 서로의 buffer를 매번 다른 순서로 가져갔다. 그래서 거의 모든 호출의 주소가 바뀌었다(25장에 1,281번).

### 6.2 바꾼 것 (vtalib-systolic, 출력 바이트는 같음)
1. **buffer 조합마다 명령 사본**(`RunReplay`, `MakeVariant`; `EVTA_REPLAY_VARIANTS`, 기본 8, `0`이면 전처럼).
   - 처음 보는 buffer 주소 조합이면 명령만 복사해 주소를 넣고 flush해 둔다. uop는 주소와 상관없어서 같이 쓴다.
   - 다음에 같은 조합이면 그 사본을 그대로 돌린다. 한도를 넘으면 전처럼 원본을 고친다.
2. **buffer pool을 thread마다**(`api.cc` `VTABufferAlloc` / `Free`; `EVTA_BUF_POOL_SHARED=1`이면 전처럼 하나).
   - 크기별 free list를 thread마다 두고, 마지막에 돌려준 것을 먼저 준다. 그러면 copy마다 매 장 같은 buffer를 받는다.
   - 자기 list에 없으면 다른 thread의 것을 가져간다(첫 장). 전체 상한(`EVTA_BUF_POOL_MB`)은 그대로다.
3. **다음 replay를 먼저 짐작**(`ReplayBegin`): bundle은 매 장 같은 순서로 EVTA를 부른다. 그래서 지난번 이 replay 다음에 온 replay의 key와 먼저 견준다(`memcmp` 한 번). 틀리면 전처럼 문자열 + hash로 찾는다.

### 6.3 결과
- 주소 고치기: 혼자 4,370 → 0번(100장), 4개 동시 1,281 → 0–61번(copy당 25장). 사본은 53–100개다.
- find: 혼자 한 장 0.24–0.32 → **0.09 ms**, 4개 동시 copy당 0.54–0.71 → 0.39–0.45 ms.
- 같은 때 번갈아 잼(따로 빌드한 실행 파일, 환경 변수로 전 / 후, int8 입력):

  | | 전 (`EVTA_REPLAY_VARIANTS=0 EVTA_BUF_POOL_SHARED=1`) | 후 |
  |---|---|---|
  | 4개 동시 | 257.5 / 257.8 / 257.2 fps (평균 257.5, 169 GOPS) | 261.0 / 262.0 / 263.6 fps (평균 **262.2**, 173 GOPS, +1.8 %) |
  | 혼자 1장 (multi4 bitstream copy 0) | 13.80 / 13.78 ms · 72.5 fps (48 GOPS) | **13.54** / 13.54 ms (−0.25 ms) · 73.9 fps (49 GOPS) |

- 맞는지: conv 출력 dump 416개(2장)가 전과 같다. `EVTA_RES_CHECK`(multi4) 228건 0 differ, 282 (0.322088).

### 6.4 남은 CPU 시간
- bundle(호출 사이 bundle 코드): 혼자 한 장 0.27 ms, 4개 동시 0.44 ms. 호출 하나에 3–5 us이고, epilogue가 있는 호출이나 CPU 문장이 끼는 곳은 9–19 us다. `_timed` 빌드의 문장별 시간 재기도 여기에 들어 있다.
- start 0.06 ms, find 0.09 ms. 이 둘은 더 줄여도 이득이 작다.


## 7. bundle 코드 몫과 7×7 project conv (2026-11-26, `38e3da66`)

bitstream은 multi4 창 장치 판 `..._4evta_sa_amul_dw_400mhz_hp304_rq.bin`이다(11-26부터 multi4 기본).

### 7.1 호출 사이와 장 사이의 bundle 코드
`EVTA_CALL_TIME=1`의 id 0 "bundle" 구간(전 장의 마지막 호출이 끝난 때부터 다음 장의 첫 호출까지)과 호출 사이 구간을 나눠 봤다.

| | 4개 동시 (barrier) | 4개 동시 (`MULTI_FREE=1`) | 혼자 |
|---|---|---|---|
| 장 사이 (id 0) | copy당 100–500 us | 67–80 us | 67 us |
| 호출 사이 bundle 코드 (호출 53개) | | | 한 장 약 0.17 ms (호출당 약 3 us) |

- 장 사이 구간은 실제로 하는 일이 67 us이고 나머지는 네 copy가 `runBarrier`에서 서로 기다리는 시간이다. barrier를 빼도(`MULTI_FREE=1`) 처리량은 276–278 대 276–284 fps로 같다. 가장 느린 copy가 처리량을 정하기 때문이다.
- 혼자 99장에 bundle 구간 합이 23.3 ms, 한 장에 236 us = 장 사이 67 us + 호출당 약 3 us × 53이다. find는 한 장 82 us, run 시작 55 us다.
- 다 없애도 1 % 안팎이라 측정 잡음(±1–2 %) 안이다. 더 손대지 않았다.

### 7.2 7×7 project conv: 무엇이 느렸나
`EVTA_REPLAY_DUMP`로 명령을 봤다. 7×7×960 → 160 (두 번)과 7×7×960 → 320, schedule 2 thread, 타일 7×7.
- 전에는 thread의 weight(filter 묶음 10 × 입력 블록 60 = 600 entry)가 thread 몫(wgt 1024 entry / 2 = 512)에 안 들어가서 weight 한 번 LOAD 길(`wgtAll`)을 못 썼다.
- 그래서 입력 블록 60개를 돌며 매번 wgt LOAD 2개(`y 5 x 1 stride 60`, 256 B 줄 5개씩), inp LOAD 1개, GEMM을 한다. GEMM은 1.2 us인데 한 걸음이 4.45 us였다. 작은 DMA의 지연에 묶여 있었다.
- 960 → 160은 residual add가 있다. multi buffer는 inp에 위쪽 entry가 없어서(`canUp` 거짓) `wgtAll`이 residual을 막고 있었다.
- `EVTA_SCHED`로 다른 schedule(1,7,7 / 2,4,7 / 1,4,7 / 2,7,4 / 2,2,7)을 다 봤지만 지금의 2,7,7보다 느렸다.

### 7.3 바꾼 것 (vtalib-systolic `conv_kernel`)
1. 1×1 conv에서 `nFilterinLoop × cBlocks`가 thread 몫보다 크면 filter 묶음을 줄인다. 몫에 들어가고 `KN/16`을 `n × nVirtualThread`로 나누는 가장 큰 n을 쓴다. 960 → 320: 10 → 5 묶음, 2 group(입력은 group마다 다시 LOAD: 49 entry × 블록).
2. residual add가 있어도, 마지막 thread의 weight가 identity weight entry(`RES_WGT_ENTRY` 1023) 아래에서 끝나면 `wgtAll`을 쓴다. residual은 전처럼 타일의 GEMM 뒤에 inp 아래쪽으로 LOAD한다. 960 → 160 multi4: 512 + 5 × 60 ≤ 1023.
3. `EVTA_WGT_FIT=0`이면 전과 같다.

| conv | 전 | 후 | 명령 수 |
|---|---|---|---|
| 7×7×960 → 160 (×2) | 267 us | 179 us | 346 → 462 |
| 7×7×960 → 320 | 459 us | 271 us | 322 → 746 (inp 47,040 → 94,080 B) |

### 7.4 결과 (같은 실행 파일, `EVTA_WGT_FIT=0/1` 번갈아)
| | 끔 | 켬 |
|---|---|---|
| MobileNetV2Tf 4개 동시 | 284.5 / 282.3 / 287.8 fps (258.5 하나는 튐; 셋 평균 284.9, 187 GOPS) | 288.2 / 292.0 / 296.2 / 291.4 fps (평균 **292.0**, 192 GOPS, +2.5 %) |
| MobileNetV2Tf 혼자 1장 (copy 0) | 12.10 / 12.10 ms · 82.6 fps (54 GOPS) | **11.76** / 11.77 ms · 85.0 fps (56 GOPS) |

- 맞는지: copy 0 conv dump 208개가 공유 `_sys` 실행 파일과 바이트까지 같다. `EVTA_RES_CHECK`(multi4) 228건 0 differ, 282 (0.322088).

## 8. 다른 모델에 미친 영향 (2026-11-26)

한 장의 연산량(GOP, `run_prof.sh`가 찍는 값): Resnet18 3.700, Resnet50 / Resnet50Tv 8.178, VGG16 0.630, MobileNetV2 / MobileNetV2Tf 0.658, DeiT 2.182. GOPS = GOP × fps(4개 동시면 네 EVTA 합).

### 8.1 11-25까지의 vtalib 변경 (11-24 실행 파일 대 새 실행 파일)
bundle은 그대로(invalidate 빼기는 bundle을 다시 만들어야 들어간다), vtalib만 다르다(§6의 replay 사본, thread별 pool 등). 25회 중앙값.

| 모델 | single 전 | single 후 | multi4 전 | multi4 후 |
|---|---|---|---|---|
| Resnet18 | 22.72 ms · 44.0 fps (163 GOPS) | 22.48 ms · 44.5 fps (165 GOPS) | 169.9 fps (629 GOPS) | 172.0 fps (636 GOPS) |
| Resnet50 | 64.48 ms · 15.5 fps (127 GOPS) | 63.33 ms · 15.8 fps (129 GOPS) | 52.5 fps (429 GOPS) | 53.3 fps (436 GOPS) |
| Resnet50Tv | 63.75 ms · 15.7 fps (128 GOPS) | 62.86 ms · 15.9 fps (130 GOPS) | 56.1 fps (459 GOPS) | 57.0 fps (466 GOPS) |
| VGG16 | 6.75 ms · 148.1 fps (93 GOPS) | 6.73 ms · 148.6 fps (94 GOPS) | ~501.7 fps (316 GOPS) | ~507.3 fps (320 GOPS) |
| MobileNetV2 | 14.40 ms · 69.4 fps (46 GOPS) | 13.86 ms · 72.2 fps (47 GOPS) | 232.8 fps (153 GOPS) | 243.2 fps (160 GOPS)¹ / 270.2 fps (178 GOPS)² |
| DeiT | 37.06 ms · 27.0 fps (59 GOPS) | 36.64 ms · 27.3 fps (60 GOPS) | ~43.8 fps (96 GOPS) | ~43.9 fps (96 GOPS) |

1. `EVTA_DWU=0`(창 장치 끔): 소프트웨어 몫만 +4.5 %. 2. 창 장치 켬.
- 나빠진 모델은 없다. 결과(분류, 0 wrong)는 본 로그에서 모두 같았다(VGG16, Resnet50Tv 로그는 안 봄).
- int8 입력은 multi main에만 있었고 다른 모델은 `.i8` 파일이 없어서 이 비교에 들어가지 않았다(11-26 ubuntu2가 범용 main으로 바꿈, `8f3bda02`).

### 8.2 §7의 1×1 weight 한 번 LOAD
같은 실행 파일(`~/JO/ubun_wt/build_m4b`, timed)로 `EVTA_WGT_FIT=0/1`을 번갈아 잼, `_dw` bitstream.

| 모델 | multi4 끔 | multi4 켬 | 혼자 끔 (200회) | 혼자 켬 | copy 0 dump |
|---|---|---|---|---|---|
| Resnet18 | 173.2 fps (641 GOPS) | 173.5 fps (642 GOPS) | 22.546 ms · 44.4 fps (164 GOPS) | 22.549 ms · 44.3 fps (164 GOPS) | 80 / 80 같음 |
| Resnet50 | 57.35 fps (469 GOPS) | **58.9 fps (482 GOPS)** | 66.72 ms · 15.0 fps (123 GOPS) | **64.41 ms · 15.5 fps (127 GOPS)** | 212 / 212 같음 |
| VGG16 | 511.5 fps (322 GOPS) | 508.8 fps (321 GOPS)³ | 7.084 ms · 141.2 fps (89 GOPS) | 7.083 ms · 141.2 fps (89 GOPS) | 52 / 52 같음 |
| MobileNetV2 | 278.9 fps (184 GOPS) | **288.0 fps (190 GOPS)** | 12.31 ms · 81.2 fps (53 GOPS) | **11.98 ms · 83.5 fps (55 GOPS)** | 208 / 208 같음 |
| DeiT | 43.8 fps (96 GOPS) | 43.75 fps (95 GOPS) | 82.89 ms · 12.1 fps (26 GOPS) | 82.89 ms · 12.1 fps (26 GOPS) | 295 / 295 같음 |
| MobileNetV2Tf | 286.4 fps (188 GOPS) | **294.7 fps (194 GOPS)** | 12.09 ms · 82.7 fps (54 GOPS) | **11.77 ms · 85.0 fps (56 GOPS)** | 208 / 208 같음 |

3. VGG16은 3×3 conv뿐이라 명령이 같다. 잡음이다.
- multi4 `EVTA_RES_CHECK`는 모든 모델에서 0 differ다.
- 혼자 시간은 multi4 bitstream 위 copy 0(`singlevta`)이다. DeiT 82.9 ms는 timed 빌드에 OMP 1, multi 버퍼라 single `_alux3` 값(36.6 ms)과 다르다.
- 혜택을 보는 것은 weight가 thread 몫(multi 512 entry, single 1024)을 넘는 1×1 conv다. single bitstream(wgt 2048 entry)은 몫이 두 배라 들어맞는 conv가 적다: MobileNetV2 두 모델만 −1.2–1.6 %([evta_perf_latest.md](evta_perf_latest.md) §6, §9.2).

## 9. 하드웨어가 바뀐 몫 (depthwise 창 장치 `_dw`, 2026-11-25 / 26)

§1–§8은 소프트웨어(vtalib, bundle, main)만 바꾼 것이다. 그 사이 ubuntu2가 depthwise 3×3을 GEMM 대각 weight 대신 창(window) 장치로 하는
bitstream을 만들었다. 하드웨어 자체(ISA, `sa_dw.v`, 빌드, 타이밍)는 [evta_dw_unit.md](evta_dw_unit.md)에 있고, 여기서는 성능만 그 문서의 표를 따라 모은다.
GOPS = 0.658 GOP × fps (MobileNetV2 / MobileNetV2Tf).

| bitstream | EVTA | 클럭 | 언제부터 | 성능 표 |
|---|---|---|---|---|
| `..._16_15_18_17_4evta_sa_amul_400mhz_hp322_rq.bin` | 4 | 400 / HP 322 MHz | 11-24 multi4 기본 (§1–§6의 측정) | |
| `..._16_15_18_17_4evta_sa_amul_dw_400mhz_hp304_rq.bin` | 4 | 400 / HP 304 MHz | 11-26 multi4 기본 | [evta_dw_unit.md](evta_dw_unit.md) §9.5, [evta_perf_latest.md](evta_perf_latest.md) §2 |
| `..._16_16_19_18_sa_alux3_400mhz_rq.bin` | 1 | 400 MHz | single 기본 (그대로) | |
| `..._16_16_19_18_sa_alux3_dw_387mhz_rq.bin` | 1 | 387.5 MHz | 11-26 새로 (기본 아님) | [evta_perf_latest.md](evta_perf_latest.md) §3, §6 |

### 9.1 multi4
[evta_dw_unit.md](evta_dw_unit.md) §9.5(같은 실행 파일 `_pcsysdwt`, 세 판을 번갈아 3번, vtalib `1cdf422` + dw):

| | `_amul` hp322 | `_amul_dw` hp304, `EVTA_DWU=0` | `_amul_dw` hp304, 창 모드 |
|---|---|---|---|
| 4개 동시 | 260.1 fps (171 GOPS) | 260.6 fps (171 GOPS) | **287.4 fps (189 GOPS)**, +10.5 % |
| 혼자 1장 (copy 0) | 13.54 ms · 73.9 fps (49 GOPS) | 13.71 ms · 72.9 fps (48 GOPS) | **12.09 ms · 82.7 fps (54 GOPS)** |
| copy당 EVTA conv / fc (4개 동시) | 6.71–6.76 ms | 6.73–6.76 ms | 5.26–5.37 ms |

- 같은 bitstream에서 창 모드를 끄면(`EVTA_DWU=0`) `_amul`과 같다. 이득은 모두 창 장치에서 왔다. HP 클럭(322 → 304 MHz)은 혼자 +0.17 ms, 4개 동시는 잡음 안이다.
- depthwise 17층 합은 6.05 → 4.21 ms다(single 375 MHz 시험판, [evta_dw_unit.md](evta_dw_unit.md) §8).
- 다른 모델에는 depthwise가 없어 빨라지지 않는다. HP 클럭 때문에 VGG16이 혼자 +2.4 %, Resnet50 +0.8 % 느리다([evta_perf_latest.md](evta_perf_latest.md) §2).

**소프트웨어와 하드웨어로 나누면** (4개 동시, MobileNetV2Tf; 줄마다 측정 때가 달라 ±2 fps쯤 흔들린다):

| 단계 | 바뀐 것 | 4개 동시 | 늘어난 비율 |
|---|---|---|---|
| 11-25 출발 | | 238.3 fps (157 GOPS) | |
| 11-25 저녁 | 소프트웨어 (§3, §6) | 263.5 fps (173 GOPS) | +10.6 % |
| 11-26 `_amul_dw` | **하드웨어** (창 장치) | 284.9–287.4 fps (187–189 GOPS) | +8–10 % |
| 11-26 `38e3da66` | 소프트웨어 (§7) | **292.0 fps (192 GOPS)** | +2.5 % |

- 이틀에 238.3 → 292.0 fps(+22.5 %)다. 소프트웨어가 약 +13 %(§3–§7), 하드웨어가 약 +10 %다. DPU S 270 fps의 108 %다.

### 9.2 single (EVTA 1개)
single은 기본 bitstream(`_alux3` 400 MHz)이 그대로라 처음에는 소프트웨어만 효과가 있었다. 11-26 `_dw` 판(387.5 MHz)이 생겼다.

MobileNetV2Tf 혼자 1장:

| 단계 | bitstream | 1장 | fps (GOPS) |
|---|---|---|---|
| 11-25 출발 (§1) | `_alux3` 400 | 14.84 ms | 67.4 (44) |
| 11-25 저녁, 소프트웨어 (§1, `_systimed`) | `_alux3` 400 | 13.04 ms | 76.7 (50) |
| 11-26 공유 `_sys`, `38e3da6`, `EVTA_WGT_FIT=0` | `_alux3` 400 | 13.05 ms | 76.6 (50) |
| 같은 실행 파일, `EVTA_WGT_FIT=1` (§7) | `_alux3` 400 | 12.89 ms | 77.6 (51) |
| 같은 실행 파일, **하드웨어** (창 장치) | `_alux3_dw` 387.5 | **11.52 ms** | **86.8 (57)** |

- 아래 세 줄은 같은 실행 파일로 같은 때 번갈아 2번 잰 평균이다([evta_perf_latest.md](evta_perf_latest.md) §6). 위 두 줄은 빌드(`_systimed`)와 측정 때가 다르다.
- 이틀에 14.84 → 11.52 ms(−22 %)다. 소프트웨어 −13 %, 창 장치 −10.6 %(클럭이 3 % 낮은데도)다.

모델 7개, 11-24 실행 파일과 지금 공유 `_sys`(`38e3da6`, 11-26 보드에서 다시 만든 bundle, int8 입력). 혼자 1장 ms · fps (GOPS = GOP × fps):

| 모델 (GOP) | 11-24 실행 파일 `_alux3` | 지금 `_alux3` 400 | 지금 `_alux3_dw` 387.5 |
|---|---|---|---|
| Resnet18 (3.700) | 22.72 ms · 44.0 fps (163 GOPS) | **22.16** ms (−2.5 %) · 45.1 fps (167 GOPS) | 22.81 ms · 43.8 fps (162 GOPS) |
| Resnet50 (8.178) | 64.48 ms · 15.5 fps (127 GOPS) | **62.27** ms (−3.4 %) · 16.1 fps (131 GOPS) | 63.90 ms · 15.6 fps (128 GOPS) |
| Resnet50Tv (8.178) | 63.75 ms · 15.7 fps (128 GOPS) | **61.73** ms (−3.2 %) · 16.2 fps (132 GOPS) | 63.28 ms · 15.8 fps (129 GOPS) |
| VGG16 (0.630) | 6.75 ms · 148.1 fps (93 GOPS) | **6.66** ms (−1.3 %) · 150.2 fps (95 GOPS) | 6.78 ms · 147.5 fps (93 GOPS) |
| MobileNetV2 (0.658) | 14.40 ms · 69.4 fps (46 GOPS) | 12.86 ms (−10.7 %) · 77.8 fps (51 GOPS) | **11.47** ms (−20.3 %) · 87.2 fps (57 GOPS) |
| MobileNetV2Tf (0.658) | (14.84 ms · 67.4 fps (44 GOPS), 11-25 출발) | 12.89 ms · 77.6 fps (51 GOPS) | **11.52** ms · 86.8 fps (57 GOPS) |
| DeiT (2.182) | 37.06 ms · 27.0 fps (59 GOPS) | **34.38** ms (−7.2 %) · 29.1 fps (63 GOPS) | 35.15 ms · 28.4 fps (62 GOPS) |

- 11-24 줄은 §8.1의 측정(25회 중앙값)이고, 지금 두 줄은 `perf_record.sh`(4 × 25장 평균)다. 재는 법과 때가 달라 1 % 안쪽은 차이로 보지 않는다.
- 줄어든 것은 소프트웨어에서 왔다: vtalib의 호출마다 CPU 시간(§6), 1×1 weight 한 번 LOAD(§7), ubuntu2가 다시 만든 bundle(EVTA만 읽는 출력의 invalidate 빼기, §3.2)과 범용 main의 int8 입력(`70a02e07`).
  모델마다 어느 것이 얼마인지는 나눠 재지 않았다. DeiT −2.7 ms의 출처도 나누지 않았다(single DeiT bundle은 다시 만들지 않았다).
- 창 장치는 MobileNetV2 두 모델만 빠르게 한다. 다른 모델은 클럭이 3 % 낮아 1–3 % 느리다. 그래서 single 기본은 `_alux3` 400 MHz 그대로다.

