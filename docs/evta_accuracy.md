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

## 6. 손실 줄이기 (2026-11-22)

**결과 (보드 `_alux3`, 10,000장).** 시간은 `_systimed` OMP 1, 25회 중앙값.

| 모델 | float | 전 (§3) | 후 | 1장 시간 전 → 후 (ms) | 무엇을 바꿨나 |
|---|---|---|---|---|---|
| Resnet50 | 57.50 % | 44.66 % | **52.92 %** | 64.18 → 64.59 | conv0 반올림 버그, 그림 200장, `-vta-round-shift` |
| Resnet18 | 58.16 % | 52.61 % | **55.67 %** | 22.55 → 22.72 | 같음 |
| MobileNetV2 | 58.71 % | 56.19 % | **56.27 %** | 14.45 → 14.44 | conv0 반올림 버그, 그림 200장 |
| VGG16 CIFAR-10 | 86.58 % (정규화한 입력) | 29.27 % | **86.41 %** | 6.75 → 6.75 | 입력 정규화, 학습 그림 100장, `-vta-round-shift` |
| DeiT-tiny | 62.41 % | 60.37 % | **60.50 %** | 37.06 → 37.01 | conv0 반올림 버그만(그림, 설정은 그대로) |

**① 가장 큰 원인: 첫 conv의 bias에 반올림 항이 빠져 있었다 (VTASave 버그).**
- `-vta-round-shift`는 bias에 2^(shift−1)을 더해 conv 출력 shift를 반올림으로 만든다(`convRoundHalf`).
- 첫 conv(conv0, packed 입력)는 bias를 다른 함수에서 weights 파일에 쓰는데, 그 함수가 이 항을 더하지 않았다. 호출 쪽은 "반올림한 bias가 있다"고 보고 bias를 넘겼다.
- 그리고 pool을 합친 conv는 `convRoundHalf`가 아예 0을 돌려주었다. max pool과 채널마다 같은 상수 더하기는 순서를 바꿔도 같다(max(x + c) = max(x) + c). shift도 단조라서 막을 이유가 없었다.
- 그래서 conv0 + pool 출력이 Glow Interpreter보다 평균 0.35 LSB 낮았다. 그 뒤 층으로 갈수록 오차가 커졌다.
- 찾은 방법:
  - Glow Interpreter의 층별 출력(`image-classifier -instrument-debug -instrument-debug-format=rawbin -minibatch=0`)과 보드의 `EVTA_DUMP_CONV`를 층마다 견줬다.
  - 그리고 conv 하나를 보드의 입력으로 numpy에서 다시 계산했다. plain conv는 99.5–99.9 % 같았고, 첫 층만 어긋났다.
- 고친 뒤 Resnet50(평가와 겹치지 않는 1,000장): 46.2 → 55.4 %. Glow Interpreter의 같은 양자화는 56.4 %다.
- `-vta-round-shift`를 쓰는 MobileNetV2와 DeiT도 conv0 weights가 바뀐다(코드는 같다).

**② calibration 그림 수.**
- 그림 1장(`cat_285.png`) 대신 ImageNetV2 200장을 쓴다(`imagenet_eval/calib_imagenetv2_200.txt`, list.txt의 50장마다 1장). 평가 10,000장의 2 %와 겹친다.
- VGG16은 CIFAR-10 학습 그림 100장이다(`calib_cifar10_train100.txt`). 시험 그림과 겹치지 않는다.
- Glow Interpreter로 보면 Resnet50은 그림 1장 50.4 % → 200장 56.4 %로 효과가 크다(1,000장).
- 하지만 scale이 2의 거듭제곱이라 그림 수가 더 늘면 대개 같은 scale로 떨어진다. VGG16은 100장, 500장, KL 방식이 byte까지 같은 bundle이었다.

**③ VGG16 입력 정규화.**
- `add_input_norm_onnx.py`가 ONNX 앞에 (x − 평균) / 표준편차(CIFAR-10 평균 0.4914, 0.4822, 0.4465, 표준편차 0.2470, 0.2435, 0.2616)를 넣는다.
- main은 그대로 0..1 RGB를 넣는다. 정규화는 conv0의 입력 복사에 합쳐진다(`-vta-fuse-preprocess`, 256칸 표). 그래서 시간이 같다.
- float 30.07 → 86.58 %(ImageNet 평균 / 표준편차로도 86.69 %).

**④ 해 보았지만 기본으로 하지 않은 것: scale을 2의 거듭제곱으로 묶지 않기 (`QSCHEMA=symmetric`).**
- DeiT에 쓴 방식이다. EVTA 비율(in × w / out)만 `-vta-pow2-ratio`로 2의 거듭제곱으로 맞춘다.
- Glow Interpreter로는 Resnet50이 64 %로 float(62 %)만큼 나온다(100장). 그러나 보드에서는 Resnet18이 32.30 %였다.
- 고친 것 두 가지:
  - vtalib CPU `relu`의 scale 비율이 `(int)` 버림이었다(1.48 → 1, 0.5 → 0). 반올림한 고정소수점 곱으로 고쳤다. 정수 비율이면 byte가 전과 같다. 두 vtalib 모두 고쳤다. 32.30 → 47.65 %.
  - `-vta-pow2-ratio`가 conv 출력 scale을 바꿀 때 뒤의 MaxPool / Relu가 따라가지 않아 컴파일이 멈췄다(`VTA.cpp` `follower`).
- 남은 문제:
  - 47.65 %는 아직 거듭제곱 판보다 낮다. residual add가 conv에 합쳐지지 않아 느리다(Resnet18 eval 330 → 465 s).
  - ① 버그를 고치기 전에 잰 값이라 다시 재야 한다. 다음 후보로 둔다.

**⑤ 측정 도구.**
- `imagenet_eval/` 밖의 그림 목록으로 PC에서 Glow로 잰다: `image-classifier <PNGs> -backend=Interpreter -load-profile=<yaml> -quantization-schema=<schema>`. 1,000장에 몇 분이다.
- `-backend=VTAInterpreter`는 relu를 2의 거듭제곱 shift(버림)로 계산하는 등 보드와 다르다. symmetric에서는 0 %가 나온다. 비교 기준으로 쓰지 않는다.

**다시 만들기.** `vtabundles_xcmpl/gen_calib_bundle.sh <Dir> ../vta/bundles/<Dir>/calib/<profile>.yaml`(PC 컨테이너 `nestc-build`, 또는 보드). 그다음 `collect_hw_bundles.py <Dir>`.
- 위 네 bundle은 저장한 profile에서 byte까지 같게 다시 나온다.
- CMake 규칙(`build_sim_bundles.sh`)은 아직 그림 1장 profile을 만든다. 그것으로 다시 만들면 §3의 bundle로 돌아간다.

## 7. calibration을 DPU처럼: KL + 거듭제곱 MSE (2026-11-22)

**왜.** DPU(Vitis AI)도 scale이 2의 거듭제곱(fix position)인데 ResNet50 손실이 0.1 %p다. 우리는 4.6 %p였다. 차이는 scale을 고르는 방법이었다.
- Glow는 거듭제곱 schema에서 scale을 늘 큰 쪽 거듭제곱으로 올린다(`std::exp2(std::ceil(std::log2(scale)))`, `glow/lib/Quantization/Base/Base.cpp`). 잘라내는 값은 없지만 최대 1비트의 해상도를 버린다.
- Vitis AI 양자화기는 tensor마다 오차가 가장 작은 지수를 고른다. 일부를 잘라내는(clip) 쪽이 될 수 있다.
- 그리고 profile의 histogram이 Glow 기본 **10칸**이었다. KL calibration은 칸이 255개보다 많아야 동작한다(`-num-histogram-bins` 설명). 그래서 §6 ②의 "KL도 byte까지 같다"는 KL이 실제로는 꺼져 있었던 것이다.

**바꾼 것.**
- `image-classifier -num-histogram-bins=2048`로 profile을 만든다(`gen_calib_bundle.sh` `BINS=2048`).
- NEST-C `lib/Quantization/Quantization.cpp`에 `-quantization-pow2-mse=<k>`를 넣었다. Glow가 고른 거듭제곱 scale s와 s/2, …, s/2^k 가운데 profile histogram의 제곱 오차가 가장 작은 것을 쓴다. 0(기본)이면 Glow 그대로다.
- `-quantization-calibration=KL`(Glow에 있던 것)과 함께 쓴다. KL이 범위를 정하고, 그다음 거듭제곱 MSE가 지수를 다시 고른다.
- 2048칸 profile은 `vta/bundles/<Dir>/calib/*_b2048.yaml.gz`(그림은 §6과 같다). `gen_calib_bundle.sh`가 `.yaml.gz`를 받는다.

**결과 1: 평가와 겹치지 않는 1,000장(`hold1000`, 보드 `_alux3`), torchvision ResNet-50 (float 65.9 %).**

| 방식 | top-1 |
|---|---|
| 지금 방식 (Glow의 올림) | 61.6 % |
| KL | 63.3 % |
| 거듭제곱 MSE k = 1 / 2 / 3 | 63.2 / 63.8 / 63.8 % (k = 2와 3은 같은 bundle) |
| KL + 거듭제곱 MSE k = 1 / 2 | 64.3 / **64.9 %** |

**결과 2: 10,000장 (보드 `_alux3`).**

| 모델 | float | 지금 방식 | KL + MSE k = 2 | 손실 |
|---|---|---|---|---|
| ResNet-50 torchvision (DPU의 pt_resnet50과 같은 float 모델) | 63.34 % | (1,000장 −4.3 %p) | **62.53 %** | −0.81 %p |
| MobileNetV2 | 58.71 % | 56.27 % | **56.66 %** | −2.05 %p |
| VGG16 CIFAR-10 | 86.58 % | 86.41 % | **86.64 %** | +0.06 %p (MSE만 써도 같다) |
| Resnet18 (1,000장) | 60.1 % | 57.8 % | 57.7 % | 바뀌지 않음 (MSE만 57.7 %, KL만 57.5 %) |

- DPU 표(ImageNet val): ResNet-50 76.1 → 76.0 %(−0.1 %p), MobileNetV2 70.13 → 67.67 %(−2.46 %p). 데이터셋이 달라(우리는 ImageNetV2) 손실 크기만 견준다.
- MobileNetV2는 1,000장에서는 −2.2 → −0.6 %p였는데 10,000장에서는 +0.39 %p만 늘었다.
- 시간은 scale만 바뀌므로 같다(shift 값만 다르다).

**torchvision ResNet-50 (`vtabundles_xcmpl/export_tv_resnet50.py`).**
- `torchvision.models.resnet50(IMAGENET1K_V1)`, ImageNet top-1 76.13 %. Vitis AI `pt_resnet50_imagenet_224_224_8.2G`의 float 모델이다.
- 지금의 Caffe2 resnet50.onnx와 입력 / 출력이 같게 내보낸다(`gpu_0/data_0` BGR 0..1 NCHW, `gpu_0/softmax_1`). BGR은 conv1 weight의 입력 채널을 뒤집어 맞추고(정확), mean / std는 입력 앞의 Sub / Div다(conv0 입력 복사에 합쳐진다). 그래서 main과 `multi_model.h`를 그대로 쓴다.
- ImageNetV2 float 63.34 %(문헌값 63.3 %). Caffe2 판은 57.50 %였다.
- 11-22 저녁부터는 자기 dir `vta/bundles/Resnet50TvTest`다(모델 `Debug2_remote/vta/bundles/Resnet50TvTest/resnet50.onnx`, ImageNet val로 잰다: [evta_dpu_compare.md](evta_dpu_compare.md)). 그 전의 `R50=tv gen_calib_bundle.sh Resnet50Test …`(`Resnet50Test/tv/resnet50.onnx`)는 옛 방식으로 남아 있다.
- 1장 68.4 ms(`_pcsystimed` OMP 1). Caffe2 판은 64.6 ms다. torchvision(v1.5)은 stride 2가 1x1이 아니라 3x3 conv에 있어 conv 모양이 다르고, `sched_search.py`로 23개 모양을 다시 찾았지만 거의 그대로였다(conv 합 61.74 → 61.68 ms, `SCHED_SUFFIX=_pcsystimed`). 차이는 모델 구조에서 온다.

**어디에 기본으로 쓰나.**
- DPU 비교 dir(`Resnet50TvTest`, `MobileNetV2TfTest`, `MobileNetV2ValTest`; [evta_dpu_compare.md](evta_dpu_compare.md))은 KL + MSE k = 2가 기본이다(`gen_calib_bundle.sh`의 `DEF`).
- 기존 dir(`Resnet18Test`, `Resnet50Test`, `MobileNetV2Test`, `Vgg16Cifar10Test`; §6)의 bundle은 그대로다. 바꾸려면 그 Dir의 보드 실행 파일을 모두 다시 빌드해야 한다(bundle `.cpp`의 shift 값이 바뀐다).
- 한 Dir에는 테스트 셋이 하나다(`vtabundles_xcmpl/imagenet_eval/eval_sets.txt`). 같은 모델을 다른 이미지로 재려면 Dir을 새로 만든다.

## 8. 명령 모음

```bash
# PC, nestc-build 컨테이너 안 (vtabundles_xcmpl/): profile (2048칸) + bundle
sudo -n docker exec -u ubuntu nestc-build bash -c "cd $PWD && BINS=2048 CALIB=n200_b2048 IMGDIR=/home/jerry/data/imagenetv2_png224 \
    ./gen_calib_bundle.sh <Dir> imagenet_eval/calib_imagenetv2_200.txt [-quantization-calibration=KL -quantization-pow2-mse=2]"
#   저장한 profile로:     ./gen_calib_bundle.sh <Dir> ../vta/bundles/<Dir>/calib/<profile>.yaml.gz [options]
#   VGG16: IMGDIR=<cifar10 train png dir> ... imagenet_eval/calib_cifar10_train100.txt
gzip -9c ../Debug2_remote/vta/bundles/<Dir>/<profile>.yaml > ../vta/bundles/<Dir>/calib/<profile>.yaml.gz   # git에 남길 때

# PC: 보드용 파일 모으기, cross build (build_sys: EVTA 1개 systolic vtalib, 실행 파일 끝 _sys)
python3 collect_hw_bundles.py <Dir>          # hw/script/run_all.sh를 새로 쓴다: 여러 Dir이면 빠진 줄이 없는지 git diff로 본다
sudo -n docker exec -u ubuntu -w $PWD/build_sys nestc-build sh -c "make -j16 <target>"
rsync -a hw/<Dir> xilinx@192.168.20.2:<board vtabundles_xcmpl>/hw/

# 보드 (vtabundles_xcmpl/): 데이터셋 top-1. 이미지는 Dir의 셋(eval_sets.txt), n 기본은 전체
sudo /bin/sh -c "imagenet_eval/eval_board.sh hw/<Dir>/<exe>_sys <name> [n]"   # -> imagenet_eval/result/<name>.log 마지막 EVAL 줄
#   빠른 비교: 평가와 겹치지 않는 1,000장 (~xilinx/JO/hold1000.txt)
sudo /bin/sh -c "hw/script/load_bit.sh vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_alux3_400mhz_rq.bin; cd hw/<Dir> && \
    OMP_NUM_THREADS=4 EVAL_LIST=/home/xilinx/JO/hold1000.txt EVAL_QUIET=1 ./<exe>_sys | tail -1"

# PC: float 기준 (bundle main과 같은 입력)
~/.venvs/onnxeval/bin/python imagenet_eval/float_ref_cnn.py <resnet18|resnet50|vgg16|mobilenetv2> <model.onnx> <png dir> [n] [out]

# PC: Glow Interpreter의 같은 양자화 (보드 없이 몇 분)
Debug2_remote/glow/bin/image-classifier <PNGs> -m=<model.onnx> -model-input-name=<in> -backend=Interpreter \
    -load-profile=<yaml> -quantization-schema=symmetric_with_power2_scale [-quantization-calibration=KL -quantization-pow2-mse=2] <image 옵션>
```

- `<image 옵션>`은 `gen_calib_bundle.sh`의 `PF`(Dir마다 layout, 값 범위, 채널 순서)다.
- 시간은 `sudo hw/script/run_prof.sh -s _pcsystimed -n 25 <Dir>`로 잰다([evta_user_manual.md](evta_user_manual.md)).
