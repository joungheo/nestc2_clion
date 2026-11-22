# EVTA 남은 일과 순서 (2026-11-22)

11-22에 SW / HW 남은 일을 모은 표다. 그 뒤 끝낸 것과 정한 순서를 함께 적는다. 일을 끝내면 상태 칸을 고친다.

## 1. 지금 시간이 어디에 쓰이나 (`_alux3` 400 MHz, EVTA 1개, 1장, OMP 1, 11-22)

| 모델 | 1장 | 큰 몫 |
|---|---|---|
| DeiT-tiny | 37.06 ms (4코어 35.95) | conv 16.7, softmax 5.3, layer norm 4.2, Q K^T 4.0, A x V 3.2, residual add 1.4 |
| MobileNetV2 | 14.45 ms (schedule 전 15.31) | conv 7.9, depthwise 5.8 (둘 다 EVTA) |
| Resnet50 | 64.18 ms (schedule 전 65.45) | 거의 전부 EVTA conv |
| Resnet18 | 22.55 ms | 거의 전부 EVTA conv (22.2) |
| VGG16 CIFAR-10 | 6.74 ms | 거의 전부 EVTA conv (6.6) |

CNN은 CPU에 남은 일이 거의 없다. 더 빨라지려면 EVTA 자체(schedule, DMA, 배열 크기)를 손봐야 한다. DeiT는 아직 conv가 아닌 일이 절반이다.

## 2. 정한 순서 (11-22)

| 순서 | 일 | 상태 |
|---|---|---|
| 1 | 이 표를 문서로 | 끝 (이 문서) |
| 2 | DeiT: softmax를 Q K^T의 epilogue로, 2장 엇갈려 처리(EVTA와 CPU 겹치기) | 할 일. 11-22에 한때 연구 과제로 미뤘다가 다시 하기로 함 (`deit_optimization.md` §6.11, §6.1) |
| 3 | HW H5: 명령 링 + interrupt | 할 일. 상한은 쟀다 (`unified_runtime_plan.md` §4.5: DeiT 9.9 ms 중 실제 이득 약 1 ms) |
| 5 | Resnet50 / Resnet18 / MobileNetV2 calibration을 그림 여러 장으로 | 할 일 (`evta_accuracy.md` §4) |
| 6 | VGG16 CIFAR-10 입력 정규화 | 할 일 (`evta_accuracy.md` §4: float 30 → 87 %) |
| 4 | AMD DPU와 비교: ResNet50, MobileNetV2 | 할 일. 아래 §5 |
| 7 | multi4 bitstream 다시 만들기 (ALU 확장 포함) | 할 일 |

## 3. SW에 남은 일

**정리 또는 운영 (작음)**

| 일 | 상태 |
|---|---|
| 보드 `_sys*` 실행 파일 다시 빌드 (ALU 확장 vtalib) | 끝 (11-22, PC cross build 37 bundle × 3) |
| 확장 없는 bitstream의 CPU 정수 softmax가 느림 (36.9 ms) | 보류. `_alux3`가 기본이면 필요 없다 |
| `vta/vtalib`(HLS bitstream용)에 `EVTA_CONV_BURST`, `EVTA_LOAD_AHEAD` 없음 | 보류. HLS bitstream을 다시 쓸 때만 |
| 보드 fail2ban IP 확인 | 남음 |
| PC onnx venv 시험 | 끝 (`~/.venvs/onnxeval`, `float_ref_cnn.py`) |
| `-vta-round-shift`를 다른 모델에도 기본으로 할지 | 남음 (지금 MobileNetV2, DeiT만) |
| CNN 네 모델의 데이터셋 정확도 | 끝 (`evta_accuracy.md`) |

**성능**

| 후보 | 대상 | 예상 | 상태 / 출처 |
|---|---|---|---|
| softmax를 Q K^T의 epilogue로 | DeiT | 층마다 0.1–0.2 ms, 합 1–2 ms | 순서 2 (deit §6.11) |
| EVTA와 CPU 겹치기: 2장을 엇갈려 처리 | DeiT | 처리량 1.5–2배, 지연은 그대로 | 순서 2 (deit §6.1) |
| 마지막 블록의 2행 attention matmul 6개를 EVTA로 (W < 7이라 CPU) | DeiT | 작음 | 남음 (fusion log) |
| `EVTA_CONV_BURST` 뒤 schedule 다시 찾기 | CNN 네 모델 | MobileNetV2 −5.5 %, Resnet50 −2.0 % | 끝 (`evta_sched_search.md`) |
| MobileNetV2 depthwise schedule / 줄이기 (5.8 ms) | MobileNetV2 | 재지 않음 | 남음 |
| residual conv의 DMA 묶기 | Resnet50 | 재지 않음 | 남음 |
| conv0 첫 행을 더 잘게 나누기 | Resnet50 | 약 −0.2 ms | 남음 |
| 작은 출력 conv의 weight 대역 높이기 | VGG16 | 3.9 ms 중 일부 | 남음 |
| 1장을 EVTA 4개로 나누기 (층마다 출력 채널 분할) | 모든 모델 | Resnet50 약 64 → 20 ms대 | 남음 (runtime 계획) |
| Conv / FirstConv 경로 하나로 합치기 (정리) | 컴파일러 | 성능 같음 | 남음 |

**구조 (큼):** 여러 가속기가 같이 쓰는 runtime / driver(HAL). `unified_runtime_plan.md`에 계획만 있다. 명령 생성을 컴파일할 때로 옮기는 일(AOT)은 record / replay 단계까지 했다.

## 4. HW에 남은 일

| 후보 | 내용 | 효과 | 상태 |
|---|---|---|---|
| EVTA 4개 판에 ALU 확장 | multi4를 `_alux3` 기능으로 다시 합성 | multi4 DeiT 처리량 | 순서 7. BRAM이 결함 행(Y219)을 넘어서 배치가 어렵다 |
| H5 명령 링 + interrupt | CPU가 기다리며 확인하지 않고 명령을 넘김 | CPU / EVTA 겹치기, 공통 driver의 바탕. 시간 이득은 작다(§4.5) | 순서 3 |
| conv requantize를 한 번에 하는 ALU 연산 | | 미리 재 보니 이득 없음 | 보류 (`evta_isa_ext.md` §8.20) |
| GEMM `lut_en` | 지금은 ALU pass로 한다 | 작음 | 자리만 잡아 둠 |
| H3 STORE 배치 옵션 | STORE가 16×16 블록을 전치해 씀 | DeiT 배치 변환 | 남음 |
| H6 하드웨어 loop FSM | tile을 하드웨어가 펼침 | 명령 생성 부담 줄임 | 남음 |
| H7 MAC 늘리기 | 32×32 배열, 또는 DPU처럼 픽셀 병렬(아래 §5) | MAC 4배면 DeiT EVTA 약 25 → 7–8 ms | 남음, 가장 큼 |
| H8 배열만 2배 클럭 (DPU 방식) | | GEMM 최대 2배 | 남음 (`evta_systolic.md` 후보 C) |
| INT8 2-MAC packing | DSP 하나에 곱 2개 (WP486) | 같은 DSP로 MAC 2배 | 남음 (`evta_systolic.md` 후보 D) |
| KV260 이식 | 제품 후보 보드, 약 300 MHz | 제품화 | 합성 안 함 |

host PC 냉각이 고쳐질 때까지 그 PC에서는 모든 코어를 쓰는 합성을 하지 않는다(`host_pc_shutdown.md`). 합성은 `ubuntu2`에서 한다.

## 5. AMD DPU와 견줄 때 (순서 4의 준비)

**DPU B4096이란** (PG338, DPUCZDX8G)
- 4096은 한 클럭에 하는 연산(op) 수다. 곱셈-덧셈(MAC) 하나를 2 op로 센다. 그래서 MAC은 클럭마다 2,048개다.
- 배열은 64 × 64도 32 × 32도 아니다. 픽셀 8개 × 입력 채널 16 × 출력 채널 16 = 2,048이다.
  - 우리 GEMM(입력 16 × 출력 16 = 256 MAC)이 8개 있고, 8개가 같은 weight를 서로 다른 픽셀 8개에 쓰는 셈이다.
  - weight는 8 픽셀이 같이 쓰므로 weight buffer와 대역이 8배로 늘지 않는다.
- DSP는 데이터 클럭의 2배로 돈다(ZCU102 표: 281 MHz, DSP 562 MHz). 그리고 DSP48E2 하나로 operand 하나를 같이 쓰는 int8 곱 2개를 한다(INT8 packing, WP486).
  - 그래서 DSP 하나가 데이터 클럭마다 MAC 4개를 한다. 2,048 MAC에 곱셈 DSP 512개 정도면 된다(계산값. 실제 수는 PG338 표와 설정에 따라 다르다).
  - ZU9EG의 DSP는 2,520개다. 그래서 B4096 3개(MAC 6,144)가 들어간다.
- 우리는 DSP 하나가 MAC 하나다. EVTA 1개에 DSP 268개, multi2 536개였다(`evta_systolic.md`). multi4에서 모자라는 것은 DSP가 아니라 BRAM(556 / 912)과 결함 행 Y219 아래 자리다.

**최대 성능 (peak)**

| | 클럭마다 op | 클럭 | peak |
|---|---|---|---|
| DPU B4096 1개 | 4,096 | 281 MHz | 1,151 GOPS |
| DPU B4096 3개 (ZCU102 표) | 12,288 | 281 MHz | 3,453 GOPS |
| 우리 EVTA 1개 (16 × 16) | 512 | 400 MHz | 204.8 GOPS |
| 우리 multi4 | 2,048 | 400 MHz | 819 GOPS |

**어떻게 견줄까**
- 표의 S 열(thread 1개)은 DPU 하나를 쓰고, T 열(여러 thread)은 DPU 3개를 함께 쓴다. 그래서 우리 EVTA 1개와 S, 우리 multi4와 T를 견주는 것이 맞다. 둘 다 "그 칩 전체로 몇 장을 처리하나"다.
- peak가 다르니 fps와 함께 가동률(실측 GOPS / peak)을 적는다. 이것이 구조끼리의 비교다.

| 모델 | | fps | GOPS | 가동률 |
|---|---|---|---|---|
| ResNet50 (DPU 8.2 GOP, 우리 8.25) | DPU S | 79.9 | 655 | 57 % |
| | DPU T | 189.7 | 1,556 | 45 % |
| | 우리 EVTA 1개 | 15.6 | 128.5 | 63 % |
| MobileNetV2 (DPU 0.602 GOP, 우리 0.657) | DPU S | 270 | 163 | 14 % |
| | DPU T | 764 | 460 | 13 % |
| | 우리 EVTA 1개 | 69.2 | 45.5 | 22 % |

- 우리 multi4는 순서 4에서 같은 모델로 잰다(11-06의 Resnet50 4개 동시 86.0 ms / 장은 그 뒤 바뀐 것이 많다).
- 모델과 정확도가 다르다. DPU 표는 PyTorch / TF 모델을 Vitis AI로 양자화한 것이다(ResNet50 ImageNet top-1 0.760, MobileNetV2 0.677). 순서 4에서 `pt_resnet50_imagenet_224_224_8.2G`를 받아 우리 흐름으로 돌리고, 같은 데이터셋으로 정확도도 잰다.
