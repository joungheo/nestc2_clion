# EVTA bundle의 데이터셋 정확도 (2026-11-22)

보드 EVTA(`_alux3` 400 MHz)에서 돌린 bundle의 top-1을 데이터셋으로 잰 기록이다. 같은 그림을 PC의 float ONNX(onnxruntime)로 돌린 값과 나란히 적었다. 두 값의 차이가 int8 EVTA 경로에서 잃은 정확도다.

## 1. 지금까지 "정확도가 그대로다 / 떨어졌다"고 한 것은 어떻게 쟀나

| 모델 | 지금까지 쓴 방법 | 무엇을 보장하나 |
|---|---|---|
| Resnet18 / Resnet50 / VGG16 / MobileNetV2 | 그림 **한 장**(`cat_285.png`, VGG16은 CIFAR 그림 한 장)의 class와 confidence, 그리고 EVTA conv 층 출력 dump(`EVTA_DUMP_CONV`)가 바꾸기 전과 byte 단위로 같은지 | 바꾼 것이 **결과를 바꾸지 않았다**는 것. 모델 자체의 정확도는 재지 않았다 |
| DeiT-tiny | ImageNetV2 앞 9,000장 top-1(보드 `EVAL_LIST`, `deit_optimization.md` §6.6, §6.10), 출력 byte hash 2,000장(`EVAL_HASH`) | 데이터셋 정확도. 바이트가 바뀌는 단계마다 다시 쟀다 |

- 층 출력이 byte 단위로 같으면 어떤 그림을 넣어도 결과가 같다. 그래서 schedule, DMA, 융합처럼 계산 순서만 바꾸는 변경은 그림 한 장 + dump 비교로 충분했다.
- 하지만 그것은 "그 전과 같다"는 뜻이다. 양자화(int8로 바꾸기) 자체가 정확도를 얼마나 잃는지는 CNN 네 모델에서 한 번도 재지 않았다. 이번에 처음 잰다.

## 2. 측정 방법

- **보드(EVTA):** `imagenet_eval/eval_board.sh <eval 실행 파일> <이름> 10000`
  - 공통 main `multi_threads_main.cpp`의 `EVAL_LIST` 모드가 weights를 한 번 올리고 목록의 그림을 차례로 분류한다.
  - Resnet18 / Resnet50 / VGG16은 이번에 eval 실행 파일을 더했다: `vtaMxnetResnet18Eval`, `vtaCaffe2Resnet50Eval`, `vtaVGG16Cifar10Eval`(`vta/bundles/<Dir>/<x>EvalMain.cpp` + `multi_model.h`).
  - 입력 tensor는 원래 main과 똑같이 만든다(채널 순서, 값 범위, 배치). `cat_285.png`의 class와 confidence가 원래 main과 소수점 여섯째 자리까지 같다.
  - `run_all.sh` / `run_prof.sh`가 쓰는 실행 파일은 원래 main 그대로다.
- **PC(float):** `imagenet_eval/float_ref_cnn.py <model> <onnx> <png dir> 10000`
  - bundle main과 같은 입력 tensor로 float ONNX를 돌린다.
  - venv: `python3 -m venv ~/.venvs/onnxeval; ~/.venvs/onnxeval/bin/pip install onnxruntime numpy pillow onnx`.
  - Resnet50의 ONNX는 opset 6이라 onnxruntime이 못 읽는다. `onnx.version_converter`로 opset 11로 바꿔 돌렸다.
- **데이터:**
  - ImageNetV2 matched-frequency 10,000장, 224 × 224 PNG(`prep_imagenetv2.py`, 보드 `~/JO/imagenetv2_png224`).
  - CIFAR-10 test 10,000장(`prep_cifar10.py`, 공식 `cifar-10-python.tar.gz`, md5 `c58f30108f718f92721af3b95e74349a`, 보드 `~/JO/cifar10_png`).
  - ImageNetV2는 ImageNet 검증 세트보다 어려워서 top-1이 보통 10 %p 안팎 낮다.

## 3. 결과 (보드 `_alux3`, 2026-11-22)

| 모델 | 데이터셋 | float (PC) | EVTA int8 (보드) | 차이 | 보드 시간 |
|---|---|---|---|---|---|
| DeiT-tiny | ImageNetV2 10,000 | 62.41 % | **60.37 %** | −2.04 %p | 481 s (OMP 4) |
| MobileNetV2 (TFLite QAT) | ImageNetV2 10,000 | 58.71 % | **56.19 %** | −2.52 %p | 262 s |
| Resnet18 (MXNet) | ImageNetV2 10,000 | 58.16 % | **52.61 %** | −5.55 %p | 395 s |
| Resnet50 (Caffe2) | ImageNetV2 10,000 | 57.50 % | **44.66 %** | **−12.84 %p** | 773 s |
| VGG16 CIFAR-10 | CIFAR-10 test 10,000 | 30.07 % (지금 입력) | **29.27 %** | −0.80 %p | 78 s |

## 4. 알게 된 것

- **CNN 셋은 양자화 손실이 크다.** Resnet50 −12.8 %p, Resnet18 −5.6 %p다.
  - 가장 큰 원인으로 보는 것은 calibration(int8 scale을 정하는 profile)이다. 그림 한 장(`cat_285.png`)으로만 만들었다(`vta/bundles/<Dir>/CMakeLists.txt`의 `image-classifier -dump-profile`).
  - 그리고 scale이 2의 거듭제곱이다. DeiT는 `-vta-pow2-ratio`, 대칭 양자화, 정수 layer norm으로 손실을 2 %p까지 줄였다(`deit_optimization.md` §6.6.3). MobileNetV2는 QAT(양자화를 고려해 학습한) weights다.
  - 개선 후보: 그림 수백 장으로 profile 만들기, DeiT에 쓴 scale 규칙을 CNN에도 쓰기, per-channel weight scale. 출력 byte가 바뀌므로 이 표로 다시 잰다.
- **VGG16 CIFAR-10은 입력 정규화가 빠져 있다.**
  - 이 ONNX 모델은 채널마다 (x − 평균) / 표준편차로 정규화한 입력으로 학습됐다. PC float로 1,000장을 재 보면 이렇다.
    - 지금 main처럼 0..1 RGB: 28.2 %
    - CIFAR 평균 / 표준편차로 정규화: 87.3 %
    - ImageNet 평균 / 표준편차로 정규화: 87.6 %
  - 원래 ETRI 설정(`image-classifier -image-mode=0to1`, main의 0..1 입력)부터 그랬다. 보드 결과 29.27 %는 EVTA가 아니라 입력 탓이다.
  - 고치려면 profile을 정규화된 입력으로 다시 만들고 main에 정규화를 넣어야 한다. 그러면 bundle의 출력이 바뀌고, 성능 측정도 다시 해야 한다.
- 지금까지의 "byte 단위로 같다" 검증은 그대로 유효하다. 바꾼 것들이 정확도를 바꾸지 않았다는 뜻이고, 위 손실은 그 전부터 있던 것이다.

## 5. 기록하는 곳

- `vtabundles_xcmpl/hw*/script/accuracy.txt`: Dir마다 지금 bundle의 데이터셋 정확도. `run_prof.sh`가 요약 줄에 그 실행의 class(`top-1 281 (0.655530)`)와 함께 보여 준다.
- 바이트가 바뀌는 변경을 하면 eval을 다시 돌려 이 문서와 `accuracy.txt`를 고친다.
