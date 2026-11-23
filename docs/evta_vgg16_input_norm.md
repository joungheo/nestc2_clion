# VGG16 CIFAR-10 입력 정규화 (2026-11-22)

[evta_todo.md](evta_todo.md) 순서 6의 기록이다.
- VGG16 CIFAR-10 bundle의 정확도가 29.27 %였는데, EVTA 탓이 아니라 모델이 학습한 입력 정규화가 빠진 탓이었다.
- 정규화를 ONNX 앞에 넣고, CIFAR-10 학습 그림으로 calibration을 다시 했다.
- 86.41 %가 됐고(float 86.58 %), 시간은 같다.

용어:
- **입력 정규화**: 채널마다 (x − 평균) / 표준편차. 모델이 이렇게 바꾼 입력으로 학습했으면, 추론에도 같은 변환이 필요하다.
- **`-vta-fuse-preprocess`**: VTASave가 첫 conv 앞의 양자화와 채널별 상수 빼기 / 나누기를 첫 conv의 입력 복사에 합치는 옵션. 기본으로 켜져 있다. int8 입력값 256개마다 결과를 미리 계산한 표([C][256])로 바꾸므로 따로 돌리는 단계가 없다.

## 1. 증상과 원인

- 처음 잰 데이터셋 정확도([evta_accuracy.md](evta_accuracy.md) §3): CIFAR-10 test 10,000장 EVTA **29.27 %**. PC float도 **30.07 %**였다.
  - float도 낮으므로 EVTA나 양자화 문제가 아니라 입력 문제다.
- PC float로 1,000장을 재 보았다.

  | 입력 | top-1 |
  |---|---|
  | 지금 main처럼 0..1 RGB | 28.2 % |
  | CIFAR-10 평균 / 표준편차로 정규화 | **87.3 %** |
  | ImageNet 평균 / 표준편차로 정규화 | 87.6 % |

- 이 ONNX(`vgg16_cifar10.onnx`)는 정규화한 입력으로 학습됐다. 원래 ETRI 설정부터 그랬다: `image-classifier -image-mode=0to1`이었고, main도 0..1 입력을 넣었다.
- 지금까지의 "그림 한 장 + dump byte 비교" 검증으로는 드러나지 않는다. 그 검증은 바뀐 것이 없는지만 보기 때문이다.

## 2. 고친 방법

- **정규화를 모델 앞에 넣는다:** `vtabundles_xcmpl/add_input_norm_onnx.py <in.onnx> <out.onnx> <mean> <std>`(새로 만듦).
  - NCHW 입력 바로 뒤에 Sub(평균), Div(표준편차) 노드를 넣고, 원래 입력을 읽던 노드가 정규화된 값을 읽게 한다.
  - 값은 CIFAR-10 평균 0.4914, 0.4822, 0.4465와 표준편차 0.2470, 0.2435, 0.2616이다(RGB, 0..1 기준).
  - 출력은 `Debug2_remote/vta/bundles/Vgg16Cifar10Test/norm/vgg16_cifar10.onnx`. `gen_calib_bundle.sh Vgg16Cifar10Test`가 없으면 만든다.
- **main은 그대로다.** 0..1 RGB를 넣는다.
  - Sub / Div는 `-vta-fuse-preprocess`가 첫 conv의 입력 복사 표에 합친다. 그래서 1장 시간이 6.75 → 6.75 ms로 같다.
  - main에 정규화를 넣는 방법도 있었지만, 그러면 CPU 단계가 하나 늘어난다.
- **calibration:** CIFAR-10 **학습** 그림 100장이다(`imagenet_eval/calib_cifar10_train100.txt`, `prep_cifar10.py train 100`). 시험 그림과 겹치지 않는다.
  - profile은 `vta/bundles/Vgg16Cifar10Test/calib/vgg16_cifar10_calib_n100.yaml`.
  - 100장, 500장, KL(10칸 histogram)이 byte까지 같은 bundle이었다. scale이 2의 거듭제곱이라 대개 같은 값으로 떨어진다.
- **`-vta-round-shift`:** 켠다(`gen_calib_bundle.sh`가 늘 켠다).

## 3. 결과 (CIFAR-10 test 10,000장, 보드 `_alux3`)

| | float (PC) | EVTA int8 (보드) | 1장 시간 (OMP 1) |
|---|---|---|---|
| 전 (0..1 입력, 그림 1장 profile) | 30.07 % | 29.27 % | 6.75 ms |
| **후 (정규화, 학습 그림 100장)** | **86.58 %** | **86.41 %** (−0.17 %p) | **6.75 ms** |
| 참고: KL + 거듭제곱 MSE k = 2, 2048칸 ([evta_accuracy.md](evta_accuracy.md) §7) | 86.58 % | 86.64 % | 같음 |

- PC float의 1,000장 값(87.3 %)과 10,000장 값(86.58 %)은 그림 수가 달라 차이가 난다.
- 지금 저장소의 `Vgg16Cifar10Test` bundle은 '후' 줄이다(weights md5 `79ba584d59a6`). `vtabundles_xcmpl/hw/script/accuracy.txt`에 86.41 %로 적었다.
- multi4 `MultiEVTAVgg16Cifar10Test`는 아직 정규화 전의 bundle이다. 다시 만들려면 `gen_multi4_bundles.sh`에 `MODEL=` / `PROF=`로 정규화 모델과 profile을 준다.

## 4. 명령

```bash
# PC, nestc-build 컨테이너 안 (vtabundles_xcmpl/)
python3 imagenet_eval/prep_cifar10.py <cifar-10-python.tar.gz> <out> [train 100]   # 시험 10,000장 / 학습 100장 PNG
sudo -n docker exec -u ubuntu nestc-build bash -c "cd $PWD && IMGDIR=<cifar10 train png dir> \
    ./gen_calib_bundle.sh Vgg16Cifar10Test imagenet_eval/calib_cifar10_train100.txt"
# 저장한 profile로 같은 bundle:
sudo -n docker exec -u ubuntu nestc-build bash -c "cd $PWD && ./gen_calib_bundle.sh Vgg16Cifar10Test ../vta/bundles/Vgg16Cifar10Test/calib/vgg16_cifar10_calib_n100.yaml"
python3 collect_hw_bundles.py Vgg16Cifar10Test
sudo -n docker exec -u ubuntu -w $PWD/build_sys nestc-build sh -c "make -j16 vtaVGG16Cifar10Eval"

# 정규화만 따로 (다른 모델에도 쓸 수 있다)
python3 add_input_norm_onnx.py in.onnx out.onnx 0.4914,0.4822,0.4465 0.2470,0.2435,0.2616

# PC float 기준
~/.venvs/onnxeval/bin/python imagenet_eval/float_ref_cnn.py vgg16 ../Debug2_remote/vta/bundles/Vgg16Cifar10Test/norm/vgg16_cifar10.onnx <cifar10 test png dir> 10000

# 보드 (vtabundles_xcmpl/): 정확도 (셋은 eval_sets.txt의 ~xilinx/JO/cifar10_png), 시간
sudo /bin/sh -c "imagenet_eval/eval_board.sh hw/Vgg16Cifar10Test/vtaVGG16Cifar10Eval_sys vgg_norm"
sudo /bin/sh -c "hw/script/run_prof.sh -s _systimed -n 25 Vgg16Cifar10Test"
```

## 5. 바뀐 파일

| 파일 | 무엇 |
|---|---|
| `vtabundles_xcmpl/add_input_norm_onnx.py` | ONNX 앞에 Sub / Div (새로 만듦) |
| `vtabundles_xcmpl/gen_calib_bundle.sh` | `Vgg16Cifar10Test`: 정규화 모델을 만들고 씀, RGB 0..1 |
| `vtabundles_xcmpl/imagenet_eval/calib_cifar10_train100.txt`, `prep_cifar10.py` | 학습 그림 100장 |
| `vta/bundles/Vgg16Cifar10Test/calib/vgg16_cifar10_calib_n100.yaml` | profile |
