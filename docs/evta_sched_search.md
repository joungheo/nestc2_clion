# EVTA conv schedule 다시 찾기 (2026-11-22)

## 1. schedule이란

- EVTA conv는 층마다 세 값으로 돈다. `include/Backends/VTA/VTASchedules.h`에 모양마다 적는다.
  - tile 크기 `tileH × tileW`: 출력을 몇 행 × 몇 열 조각으로 나눌지
  - virtual thread 수 `nVirtualThread`(1 또는 2): 두 조각을 엇갈려 한쪽이 계산하는 동안 다른 쪽이 LOAD / STORE하게 할지
- 모양: 입력 H × W × C, filter 수 KN, kernel, stride, pad
- VTASave가 bundle에 그 값을 인자로 적는다.
- 실행할 때 `EVTA_SCHED=<H>,<W>,<C>,<KN>,<KH>,<stride>:<vt>,<tileH>,<tileW>[;...]`로 바꿔 끼울 수 있다(vtalib-systolic `schedOverride`). 다시 컴파일할 필요가 없다.
- 11-05 ~ 11-08에 찾은 값이었다. 그 뒤 vtalib에 세 가지가 들어와 LOAD / STORE의 비용이 바뀌었다. 그래서 다시 찾았다.
  - DMA 묶기(`EVTA_CONV_BURST`)
  - systolic의 최소 tile 1
  - 다음 tile LOAD 겹치기(`EVTA_LOAD_AHEAD`)

## 2. 방법 (`vtabundles_xcmpl/sched_search.py`)

1. **후보:** `sched_search.py cands <bundle.cpp>`가 bundle의 EVTA conv 호출에서 모양과 지금 schedule을 읽는다. 모양마다 아래 조건을 모두 지키는 후보를 만든다.
   - tileH는 출력 높이의 약수, tileW는 출력 너비의 약수이고 너비의 1/4 이상이다. 나누어떨어지지 않는 tile은 결과를 바꿀 수 있다.
   - vt는 1 또는 2다. 출력 채널 블록 수가 홀수면 1만 쓴다.
   - tile + 1 entry가 thread의 acc 몫(2048 / vt)에 들어가고, 채널 블록 하나의 입력 tile이 thread의 inp 몫(2048 / vt)에 들어간다.
   - tile은 4칸 이상이다. 1 × 1, 1 × 2 tile은 같은 acc entry를 연달아 써서 `uop_kernel.h` `VerifyDep`이 실행을 멈춘다.
   - max pool을 합친 conv(`convolution_wo_tr_pool`)는 tileH와 tileW가 짝수다.
2. **측정:** `sched_search.py run`을 보드에서 root로 돌린다.
   - 한 번 실행할 때 **모든 모양에 후보를 하나씩 동시에** 끼운다. EVTA 시간은 값에 따라 달라지지 않으므로 서로 섞이지 않는다.
   - `_systimed` 실행 파일의 `timing.csv`(문장마다 시간)에서 모양별 conv 시간을 읽는다.
   - 그래서 모델 하나에 "가장 긴 후보 목록의 길이"만큼(35–44번)만 돌리면 된다.
   - 실패한 run(exit ≠ 0)은 모양을 반씩 나눠 다시 돌려, 실패한 (모양, 후보)만 `bad.txt`에 남기고 나머지 시간은 쓴다.
3. **고르기:** `sched_search.py pick` / `final`. 모양마다 가장 빠른 후보를 고르고, 지금보다 1 % 넘게 빠를 때만 바꾼다.
4. **검증:**
   - 고른 값을 한꺼번에 `EVTA_SCHED`로 넣는다.
   - `EVTA_DUMP_CONV`로 모든 EVTA conv 출력이 지금 schedule과 byte 단위로 같은지 본다.
   - 25회씩 두 번 1장 시간을 잰다.
   - 그다음 `VTASchedules.h`를 고치고 bundle을 다시 만들어 같은 검증을 한 번 더 한다. bundle 코드는 schedule 인자만 바뀌고, weights는 같다.

## 3. 결과 (보드 `_alux3`, OMP 1, 25회 중앙값)

| 모델 | 모양 수 | run 수 | 바꾼 모양 | conv dump | 1장 시간 (ms) |
|---|---|---|---|---|---|
| MobileNetV2 | 20 (depthwise 빼고) | 44 + 다시 돌린 것 | 9 | 5,200 / 5,200 같음 | 15.29 → **14.45** (−5.5 %) |
| Resnet50 | 23 | 44 + 다시 돌린 것 | 5 | 106 / 106 같음 | 65.46 → **64.18** (−2.0 %) |
| Resnet18 | 11 | 44 + 다시 돌린 것 | 1 | 40 / 40 같음 | 22.55 → 22.55 |
| VGG16 CIFAR-10 | 9 | 35 | 2 | 26 / 26 같음 | 6.746 → 6.741 |

크게 줄어든 곳:

| 모양 (H, W, C, KN, K, stride) | 전 (vt, tileH, tileW) | 후 | conv 시간 (ms) |
|---|---|---|---|
| Resnet50 56, 56, 256, 512, 1, 2 (1×1 stride 2 downsample) | 2, 7, 28 | 2, 2, 28 | 2.51 → 1.58 |
| MobileNetV2 112, 112, 16, 96, 1, 1 | 2, 16, 28 | 2, 8, 112 | 1.31 → 0.68 |
| Resnet50 14, 14, 256, 1024, 1, 1 | 2, 7, 14 | 2, 14, 14 | 5.37 → 5.17 |
| MobileNetV2 112, 112, 32, 16, 1, 1 | 1, 16, 56 | 1, 14, 112 | 0.30 → 0.20 |

- 대부분은 행 전체를 한 tile로 하는 쪽(tileW = 출력 너비)이 빨랐다. DMA 묶기 뒤로는 연속된 행이 긴 LOAD 하나가 되기 때문이다.
- Resnet50의 1×1 stride 2는 tile을 낮게(2행) 할 때 빠르다. stride 2라 입력의 절반 행만 쓰는데, 낮은 tile이 LOAD와 GEMM을 더 잘 겹친다.
- MobileNetV2 conv0(packed 입력)은 1, 16, 112가 0.02 ms 빨랐지만 넣지 않았다. 입력 tile이 VTASave `getConv0Pack`이 허용하는 1,024 entry를 넘어, bundle이 0 채움 conv0로 돌아가기 때문이다.
- VGG16의 4 × 4, 2 × 2 출력 conv(합 3.9 ms)는 후보가 적고, weight를 읽는 시간이 대부분이라 schedule로 줄지 않는다.
- MobileNetV2의 depthwise(KN 0, 5.8 ms)는 이번에 찾지 않았다. 다음 후보다.

## 4. 다시 돌리기

```sh
# PC: 후보
python3 vtabundles_xcmpl/sched_search.py cands vtabundles_xcmpl/hw/Resnet50Test/resnet50.cpp > Resnet50Test.json
# 보드 (vtabundles_xcmpl/, root): 측정 (모델 하나 약 5–15분)
sudo python3 sched_search.py run Resnet50Test.json /home/xilinx/JO/sched/Resnet50Test Resnet50Test 4
# 고르기, 넣을 EVTA_SCHED
python3 sched_search.py pick Resnet50Test.json /home/xilinx/JO/sched/Resnet50Test
python3 sched_search.py final Resnet50Test.json /home/xilinx/JO/sched/Resnet50Test
```

PC cross build 실행 파일로 찾으려면 `sudo /bin/sh -c 'SCHED_SUFFIX=_pcsystimed python3 sched_search.py run …'`(sudo는 환경 변수를 넘기지 않으므로 sh 안에서 준다; 2026-11-22 torchvision ResNet-50).

같은 보드 lock(`/tmp/evta_run.lock`)을 `run_prof.sh`가 잡는다. 두 탐색을 동시에 돌리면 서로 lock을 못 잡아 모두 실패한다.

## 5. 4개 동시 실행에서 다시 찾기 (2026-11-24, MobileNetV2Tf multi4)

**왜:** multi4에서 MobileNetV2Tf는 copy당 14.7 ms(혼자) → 16.7 ms(4개 동시)로 느려진다. 늘어난 시간은 112×112 앞쪽 층에 몰려 있다([evta_dpu_compare.md](evta_dpu_compare.md) §6.6).
§3의 schedule은 EVTA 1개로 찾았으므로, DDR이 붐빌 때 더 나은 tile이 있는지 4개를 동시에 돌리며 다시 찾았다.

**도구에 더한 것:**
- `SCHED_SET=hw_multievtax4 SCHED_EXE=<multi 실행 파일>`
  - `hw_multievtax4/script/run_prof.sh`로 그 실행 파일만 돌린다(`run_prof.sh`의 새 환경 변수 `PROF_EXE`).
  - 네 copy의 `timing_<cpp>.csv`를 한 파일로 모은다(run 번호 = copy × 10000 + run). 그래서 모양별 시간은 네 copy를 합친 중앙값이다.
- depthwise(`dw_convolution_evta`, key KN 0)도 후보에 넣는다. 조건은 acc에 tile + 2 entry가 들어가는 것, vt는 채널 블록 수의 약수인 것이다.

```sh
# PC
python3 vtabundles_xcmpl/sched_search.py cands <보드의 hw_multievtax4/MultiEVTAMobileNetV2TfTest/mobilenetv2.cpp> > MbTf_m4.json
# 보드 (vtabundles_xcmpl/): 44번, 한 번에 약 8초
sudo /bin/sh -c 'SCHED_SET=hw_multievtax4 SCHED_EXE=multivtaMobileNetV2TfBundle python3 sched_search.py run MbTf_m4.json /home/xilinx/JO/sched/MbTf_m4 MultiEVTAMobileNetV2TfTest 25'
python3 sched_search.py pick MbTf_m4.json /home/xilinx/JO/sched/MbTf_m4
```

**결과:** 모양 30개(depthwise 13개)를 찾았고, 1 %를 넘게 빨라진 것은 3개다.

| 모양 (H, W, C, KN, K, stride) | 전 (vt, tileH, tileW) | 후 | 4개 동시 conv 시간 (ms) |
|---|---|---|---|
| 112, 112, 32, 0, 3, 1 (첫 depthwise) | 2, 14, 56 | 2, 4, 112 | 1.048 → 0.933 |
| 112, 112, 16, 96, 1, 1 (expanded_conv_1 expand) | 2, 8, 112 | 2, 2, 112 | 0.992 → 0.892 |
| 112, 112, 32, 16, 1, 1 (expanded_conv project) | 1, 14, 112 | 1, 16, 112 | 0.292 → 0.283 |

- conv 문장 시간의 합은 15.44 → 15.20 ms(−0.24 ms)다.
- 나머지 모양은 11-22에 찾은 값이 4개 동시에서도 가장 빨랐다.
- 낮고 넓은 tile이 빨라진 이유: 행 전체가 LOAD 하나가 되고, tile이 작아 LOAD와 GEMM이 더 잘 겹친다.

**검증 (`_sys`, 시간 측정 코드 없음, copy당 200회, `EVTA_SCHED`로 넣어 번갈아 3번씩):**
- 모든 EVTA conv dump 208개가 지금 schedule과 byte 단위로 같다.

| | 지금 schedule (장/s) | 새 schedule (장/s) | 차이 |
|---|---|---|---|
| 4개 barrier(추론마다 함께 출발) | 233.0 / 233.6 / 236.2 (평균 234.3) | 235.5 / 236.7 / 237.4 (평균 236.5) | +0.9 % (3번 모두 빠름) |
| 4개 free(`MULTI_FREE=1`) | 239.2 / 241.6 / 235.9 (평균 238.9) | 243.2 / 238.0 / 246.2 (평균 242.5) | +1.5 % (잡음 ±2 %) |
| EVTA 1개(`_alux3`, ms) | 14.51 / 14.55 | 14.43 / 14.45 | −0.08 ms |

- 결론: 1개일 때도 느려지지 않으므로 `VTASchedules.h`에 넣었다. 하지만 경쟁분 2 ms 가운데 0.2 ms만 줄었다. schedule로는 경쟁분이 크게 줄지 않는다.
- bundle은 아직 다시 만들지 않았다. 다음에 MobileNetV2 bundle을 다시 만들 때 함께 반영된다. 그 전에는 위의 `EVTA_SCHED`로 같은 효과를 낸다.
- 남은 경쟁분은 depthwise 효율(GEMM의 1/16만 씀; [evta_todo.md](evta_todo.md) §5)과 층 묶기로 줄여야 한다.
