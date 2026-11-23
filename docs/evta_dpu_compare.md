# EVTA와 AMD DPU 비교: ResNet50, MobileNetV2 (2026-11-22)

ZCU102에서 우리 EVTA와 AMD의 DPU(Vitis AI의 NPU IP)를 **같은 float 모델, 같은 테스트 이미지**로 견준다.
여기에는 무엇으로, 어떻게 재었는지와 그 결과를 모두 적는다.
- 남은 일과 순서는 [evta_todo.md](evta_todo.md) §5에 있다.
- calibration 방법을 찾은 과정은 [evta_accuracy.md](evta_accuracy.md) §7에 있다.

**모든 bundle은 int8이고 보드의 EVTA 하드웨어(bitstream)에서 돈다.** "float"는 받은 원본 모델과 PC(onnxruntime)에서 잰 기준값을 말한다.
- 원본(float32, QAT는 uint8)은 PC에서 profile과 `model-compiler`로 int8 weight와 EVTA 명령 코드(bundle)로 바뀐다(§4.2).
- float와 int8의 차이가 양자화 손실이고, DPU 표의 float / quantized와 같은 구조다.

용어:
- **float / int8**: 32비트 실수 모델과 8비트 정수로 양자화한 모델.
- **calibration**: int8의 scale(실수 ↔ 정수 비율)을 정하려고 몇백 장을 float로 돌려 값의 분포를 모으는 일. 그 결과가 **profile**(YAML)이다.
- **top-1**: 가장 높은 점수의 클래스가 정답인 비율.
- **peak**: 곱셈기가 매 클럭 모두 일할 때의 이론 성능.
- **가동률**: 실측 GOPS / peak.

## 1. 요약

| 모델 | | fps | GOPS | 가동률 | ImageNet val top-1: float → int8 |
|---|---|---|---|---|---|
| ResNet50 | DPU S (B4096 1개, 1 thread) | 79.9 | 655 | 57 % | 76.1 → 76.0 % (−0.1 %p) |
| | DPU T (B4096 3개, 여러 thread) | 189.7 | 1,555 | 45 % | |
| | 우리 EVTA 1개 `Resnet50TvTest` | 14.6 | 119.6 | **58 %** | 76.15 → **75.29 %** (−0.86 %p) |
| | 우리 EVTA 4개 `MultiEVTAResnet50TvTest` | 52.9 | 432 | **53 %** | 같은 bundle |
| MobileNetV2 | DPU S | 270.0 | 163 | 14 % | 70.13 → 67.67 % (−2.46 %p) |
| | DPU T | 764.4 | 460 | 13 % | |
| | 우리 EVTA 1개 `MobileNetV2TfTest` | 69.5 | 45.7 | **22 %** | 71.80 → **68.43 %** (−3.37 %p) |
| | 우리 EVTA 4개 `MultiEVTAMobileNetV2TfTest` | 233 | 153 | **19 %** | 같은 bundle |

- **가동률은 우리가 같거나 높다.** 곱셈기를 놀리지 않는 효율은 뒤지지 않는다.
- **절대 성능은 DPU가 3.3–5.5배다.** peak 차이만큼이다.
  - EVTA 1개 204.8 GOPS 대 B4096 1개 1,151 GOPS (5.6배).
  - EVTA 4개 819 대 B4096 3개 3,453 GOPS (4.2배).
- **EVTA 4개 판은 아직 ALU 확장이 없는 bitstream이다(§3).** MobileNetV2의 residual add와 avgpool이 CPU에서 돈다. ALU 확장 multi4 bitstream이 나오면 다시 잰다.

## 2. 비교 대상: AMD DPU 수치

- **출처:** `docs/ref/performance/DPU_AMD/dpu_3.0_performance_tops_0.6.xlsx`. Vitis AI Model Zoo의 "Details & Performance" 표다(Copyright 2022 AMD). 보드 revision 0432055-05 기준이다.
- **ZCU102 구성:** `3* DPUCZDX8G B4096 @ 281MHz`.
  - B4096은 클럭마다 4,096 연산(곱셈 + 덧셈 = 2,048 MAC)이다.
  - peak는 1개 4,096 × 281 MHz = 1,151 GOPS, 3개 3,453 GOPS다.
- **"E2E throughput (fps)":**
  - "Single Thread"(S)는 1 thread로, DPU 하나만 바쁘다.
  - "Multi Thread"(T)는 여러 thread로, DPU 3개를 다 쓴다.
  - 전처리·후처리를 포함한 끝에서 끝까지의 처리량이다.

| Vitis AI 모델 | framework | 데이터 | 입력 | float | 양자화 | 연산 (G) | ZCU102 S / T fps |
|---|---|---|---|---|---|---|---|
| `pt_resnet50_imagenet_224_224_8.2G` | PyTorch | ILSVRC2012 | 224×224×3 | 0.761 / 0.929 | 0.760 / 0.928 | 8.2 | 79.92 / 189.67 |
| `tf_mobilenetv2_1.0_imagenet_224_224_602M` | TensorFlow | ILSVRC2012 | 224×224×3 | 0.7013 | 0.6767 | 0.602 (표에는 "62") | 270.02 / 764.41 |

- 정확도는 top-1 / top-5이고, ILSVRC2012 val 50,000장에서 쟀다.
- DPU GOPS = fps × 연산량이다. 예: 79.92 × 8.2 = 655.

## 3. 우리 하드웨어와 소프트웨어

| 구분 | 파일 / 값 |
|---|---|
| EVTA 1개 bitstream | `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_alux3_400mhz_rq.bin` (보드 `/lib/firmware`, 2026-11-15; `vtabundles_xcmpl/hw/script/run_all.sh`의 `SYS_BIN`) |
| | systolic GEMM 16 × 16, 400 MHz, requantize(`_rq`), ALU 확장 3단계(`_alux3`), WNS +0.022 ns ([evta_isa_ext.md](evta_isa_ext.md) §8.15) |
| | peak 16 × 16 MAC × 2 × 400 MHz = **204.8 GOPS** |
| EVTA 4개 bitstream | `vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta_sa_400mhz_rq.bin` (2026-11-04; `hw_multievtax4/script/run_all.sh`의 `SYS_BIN`) |
| | EVTA 4개, 각 400 MHz `_rq`, **ALU 확장 없음**, WNS +0.002 ns ([evta_isa_ext.md](evta_isa_ext.md) §7.18) |
| | peak 4 × 204.8 = **819.2 GOPS** |
| | ALU 확장 3단계까지 넣은 판(`_ni`, LUT 243,945 = 89.3 %)은 배치·배선 중이다([evta_todo.md](evta_todo.md) 순서 7) |
| runtime | `vtalib-zcu102-systolic` (commit `a9b4ad1` 시점), 보드 실행 파일은 PC docker `nestc-build`의 cross build |
| 컴파일러 | NEST-C `model-compiler` (`Debug2_remote`, commit `a9b4ad1`); `-quantization-pow2-mse`는 `ab4aacf`부터 |
| 보드 | ZCU102 (Cortex-A53 4코어 1.2 GHz), PYNQ 2.4 |

## 4. 새로 만든 bundle

규칙은 다음과 같다.
- **새로 받은 모델은 새 dir이다.**
- **같은 모델이라도 테스트 셋이 다르면 새 dir이다.**
- 한 dir의 정확도는 늘 같은 이미지로 잰 값이다. 셋은 `vtabundles_xcmpl/imagenet_eval/eval_sets.txt`에 적고, `eval_board.sh`가 실행 파일의 dir로 셋을 고른다.
- 기존 `Resnet50Test`(Caffe2 모델)와 `MobileNetV2Test`(QAT 모델)는 ImageNetV2 그대로 둔다.

| dir (vta/bundles) | 모델 | 받은 곳 | 형식 | 테스트 셋 |
|---|---|---|---|---|
| `Resnet50TvTest` | torchvision ResNet-50 `IMAGENET1K_V1` (= DPU `pt_resnet50`의 float 모델) | PyTorch hub (`torchvision.models.resnet50(weights=IMAGENET1K_V1)`, torchvision 0.19.1) | float32 | ImageNet val, torchvision 전처리 |
| `MobileNetV2TfTest` | TF-slim `mobilenet_v2_1.0_224` (= DPU `tf_mobilenetv2_1.0`의 float 모델) | `https://storage.googleapis.com/download.tensorflow.org/models/tflite_11_05_08/mobilenet_v2_1.0_224.tgz` (checkpoint, frozen pb, float `.tflite`; 2018-03-22) | float32 | ImageNet val, TF-slim 전처리 |
| `MobileNetV2ValTest` | TFLite QAT `mobilenet_v2_1.0_224_quant` (기존 `MobileNetV2Test`의 모델) | `.../tflite_11_05_08/mobilenet_v2_1.0_224_quant.tgz` | uint8 QAT → float32로 풂 | ImageNet val, TF-slim 전처리 |

EVTA 4개용(`vta/multievta_bundles`):
- `MultiEVTAResnet50TvTest`, `MultiEVTAMobileNetV2TfTest`. 같은 모델, 같은 profile, 같은 옵션으로 만든 4개 복사본이다.
- copy i는 EVTA i에서 돈다(`-index-multi-evta=2^i`).
- copy 0의 `.cpp`와 weights는 single bundle과 byte 단위로 같다. 그래서 정확도도 single bundle과 같다.

### 4.1 float ONNX를 만든 방법

- **`Resnet50TvTest/resnet50.onnx`** (md5 `24723e1213f2…`): `vtabundles_xcmpl/export_tv_resnet50.py`로 만들었다.
  - 기존 Caffe2 `resnet50.onnx`와 입력·출력이 같다. 입력은 `gpu_0/data_0` 1×3×224×224 NCHW, BGR, 0..1이고, 출력은 `gpu_0/softmax_1`(1000 클래스 softmax)다. 그래서 main과 `multi_model.h`를 그대로 쓴다.
  - BGR 입력: conv1 weight의 입력 채널 순서를 뒤집었다. 결과는 정확히 같다.
  - torchvision의 mean / std 정규화 `(x - mean) / std`를 모델 앞에 Sub / Div로 넣었다. VTASave `-vta-fuse-preprocess`가 이것을 첫 conv의 입력 복사에 합친다.
  - opset 10, torch 2.4.1+cpu (`~/.venvs/onnxeval`).
- **`MobileNetV2TfTest/mobilenetv2_tf_f32.onnx`** (md5 `2b5735a5629a…`): tgz 안의 float `mobilenet_v2_1.0_224.tflite`(md5 `d8065bc96469…`)를 `vta/bundles/MobileNetV2Test/tfl2onnx.py`로 바꿨다.
  - 2026-11-22에 float 모델 처리를 넣었다. 양자화 정보가 없으면 RELU6는 fused activation 그대로 Clip(0, 6)이 된다.
  - 마지막 softmax는 뺐다. QAT 모델처럼 logits를 내므로 main이 같고, top-1도 같다.
  - 1001 클래스(0 = background)라서, main의 `MODEL_LABEL_OFFSET 1`이 ImageNet 번호에 1을 더해 맞춘다.
- **`MobileNetV2Test/mobilenetv2_qat_f32.onnx`** (md5 `7088cbdc5264…`): 기존 파일이다. uint8 weight를 `(q - zero_point) × scale`로 float로 풀었다.

모든 ONNX는 git에 없다. `Debug2_remote/vta/bundles/<Dir>/`에 둔다. 만드는 명령은 각 dir의 `CMakeLists.txt` 머리에 있다.

### 4.2 int8로 만든 방법 (양자화, calibration)

- **양자화 방식:** `symmetric_with_power2_scale`이다. 대칭 int8이고, scale은 2의 거듭제곱이다. EVTA는 shift로 scale을 맞추므로 거듭제곱이 필요하다.
  - weight는 tensor마다 scale 하나다.
  - MobileNetV2의 depthwise conv만 채널마다 scale을 둔다(`-enable-channelwise-depthwise`).
  - DPU 양자화기(vai_q)도 거듭제곱 scale을 쓰므로 조건이 같다.
- **calibration 이미지:** ImageNetV2 200장이다(`imagenet_eval/calib_imagenetv2_200.txt`, list.txt의 50장마다 1장).
  - ImageNet val과 겹치지 않는다.
  - DPU는 보통 ImageNet train에서 일부를 쓴다.
- **profile:** `image-classifier -backend=VTAInterpreter -dump-profile -num-histogram-bins=2048`로 만든다.
  - Glow의 기본 10칸으로는 KL calibration이 동작하지 않는다(255칸 넘게 필요).
  - git의 `vta/bundles/<Dir>/calib/*.yaml.gz`:

    | dir | profile |
    |---|---|
    | `Resnet50TvTest` | `resnet50_tv_calib_n200_b2048.yaml.gz` |
    | `MobileNetV2TfTest` | `mobilenetv2_tf_calib_n200_b2048.yaml.gz` |
    | `MobileNetV2ValTest` | `MobileNetV2Test/calib/mobilenetv2_calib_n200_b2048.yaml.gz` |

- **scale 고르기:** 두 옵션을 함께 쓴다. 이 dir들에서는 `gen_calib_bundle.sh`가 기본으로 넣는다([evta_accuracy.md](evta_accuracy.md) §7).
  - `-quantization-calibration=KL`: 분포를 가장 덜 잃는 자르기 범위를 고른다.
  - `-quantization-pow2-mse=2`: Glow는 거듭제곱 scale을 늘 큰 쪽으로 올린다. 대신 s, s/2, s/4 가운데 histogram 제곱 오차가 가장 작은 것을 고른다.
- **반올림:** `-vta-round-shift`. EVTA conv의 출력 shift를 반올림한다(bias에 2^(shift-1)을 더함).
- **생성 명령:** `nestc-build` 안에서 `vtabundles_xcmpl/`에서 실행한다.

  ```bash
  ./gen_calib_bundle.sh Resnet50TvTest ../vta/bundles/Resnet50TvTest/calib/resnet50_tv_calib_n200_b2048.yaml.gz
  ./gen_calib_bundle.sh MobileNetV2TfTest ../vta/bundles/MobileNetV2TfTest/calib/mobilenetv2_tf_calib_n200_b2048.yaml.gz
  ./gen_calib_bundle.sh MobileNetV2ValTest ../vta/bundles/MobileNetV2Test/calib/mobilenetv2_calib_n200_b2048.yaml.gz
  # profile을 새로 만들 때: BINS=2048 CALIB=n200_b2048 IMGDIR=<ImageNetV2 png dir> ./gen_calib_bundle.sh <Dir> imagenet_eval/calib_imagenetv2_200.txt
  ./gen_multi4_bundles.sh MultiEVTAResnet50TvTest MultiEVTAMobileNetV2TfTest
  python3 collect_hw_bundles.py Resnet50TvTest MobileNetV2TfTest MobileNetV2ValTest   # 그리고 --set multievtax4 <Dir>
  ```

- **생성된 bundle (weights md5):**

  | bundle | weights md5 |
  |---|---|
  | `Resnet50TvTest` | `1e6b5d084986` |
  | `MobileNetV2TfTest` | `31cd75acb6dc` |
  | `MobileNetV2ValTest` | `640a795bd0a2` |

- **어디서 도는가:** fusion 요약에 따르면 conv, fc, pool은 모두 EVTA에서 돈다.
  - ResNet50: EVTA producer 53개이고, CPU GEMM은 0개다.
  - MobileNetV2: producer 35개다. depthwise도 EVTA에서 돈다(`-vta-evta-depthwise`). avgpool은 따로 한 번 EVTA에서 돈다.

### 4.3 테스트 이미지 (ImageNet val)

- **받은 곳:** Hugging Face `ILSVRC/imagenet-1k`의 `data/validation-000{00..13}-of-00014.parquet`. 14개, 6.3 GB, 50,000장, 1000 클래스 × 50장이다.
  - 로그인과 데이터셋 약관 동의가 필요하다.
  - 받는 명령: `hf download ILSVRC/imagenet-1k --repo-type dataset --include 'data/validation-*.parquet'`.
  - `label` 열이 표준 클래스 번호(synset 정렬 순서)다. 예: `ILSVRC2012_val_00027555_n01824575.JPEG` → 91.
- **전처리:** `vtabundles_xcmpl/imagenet_eval/prep_imagenet_val.py <tv|tf> <parquet dir> <out>`. 모델이 원래 평가에 쓰는 전처리를 따라 224 × 224 RGB PNG로 만든다. bundle main은 바이트를 범위만 바꿔 넣는다.

  | 이름 | 방식 | 대상 | PC / 보드 위치 |
  |---|---|---|---|
  | `tv` | torchvision `Resize(256)` (짧은 변 256, PIL bilinear) + `CenterCrop(224)` | ResNet50 | `~/data/imagenet_val_tv224` / `~xilinx/JO/imagenet_val_tv224` (4.2 GB) |
  | `tf` | TF-slim `inception_preprocessing` eval: `central_crop(0.875)` + `resize_bilinear` 224 × 224 (TF 1 방식, antialias 없음; 바이트로 반올림) | MobileNetV2 두 모델 | `~/data/imagenet_val_tf224` / `~xilinx/JO/imagenet_val_tf224` (4.6 GB) |

- **전처리 확인:** PC float top-1(`float_ref_cnn.py`, onnxruntime)이 공개 값과 맞는다.

  | float 모델 | 우리 PC | 공개 값 |
  |---|---|---|
  | torchvision ResNet-50 | **76.15 %** | torchvision 76.13 %, DPU 표 76.1 % |
  | TF-slim MobileNetV2 | **71.80 %** | TF-slim 71.8 % |
  | QAT MobileNetV2 (float로 풂) | **70.91 %** | TFLite 표 70.8 % |

  DPU 표의 MobileNetV2 float는 70.13 %다. Vitis AI가 다른 전처리를 쓰는 것으로 보인다. 그래서 MobileNetV2는 정확도의 절대값보다 **float → int8 손실**을 견준다.

## 5. 측정 방법

- **시간:** `sudo hw/script/run_prof.sh -s _pcsystimed -n 25 <Dir>`.
  - `_pcsystimed`는 PC cross build에 문장마다 시간을 재는 판이다.
  - 25회 가운데 2–25회의 중앙값을 쓴다.
  - OMP 1(CPU 1코어)로 돈다.
  - 입력은 `cat_285.png` 한 장이다. 이미지는 시간에 영향이 없다.
  - "whole run"은 float 입력의 양자화·전처리부터 출력까지다. PNG 읽기는 넣지 않는다.
- **EVTA 4개:** `sudo hw_multievtax4/script/run_prof.sh -s _pcsystimed -n 25 <MultiDir>`. 4장을 동시에 처리한다(copy 4개가 각자 EVTA 하나).
  - copy마다 장당 시간의 중앙값을 낸다.
  - fps = 4 / (copy 평균 시간)이다.
- **GOPS:** 우리 bundle이 실제로 EVTA에 보낸 연산량 × fps다. ResNet50은 8.178 GOP, MobileNetV2는 0.658 GOP다.
  - MobileNetV2는 24채널을 32채널로 넓힌 연산(`-vta-pad-channels`)이 들어 있어서, DPU 표의 0.602 G보다 크다. 그래서 MobileNetV2의 우리 가동률은 그만큼 높게 나온다.
  - 가동률 = GOPS / peak다.
- **정확도:** 보드에서 `sudo imagenet_eval/eval_board.sh <exe> <name>`을 쓴다.
  - SYS_BIN을 올리고, dir의 셋 전체를 1장씩 분류한다.
  - OMP 4, EVTA 1개 실행 파일(`_sys`)이다.
  - 결과는 `imagenet_eval/result/<name>.log`의 마지막 `EVAL` 줄이다.

## 6. 결과

### 6.1 시간 (보드, 2026-11-22)

| bundle | bitstream | 장당 시간 | fps | GOPS | 가동률 |
|---|---|---|---|---|---|
| `Resnet50TvTest` | `..._sa_alux3_400mhz_rq` | 68.39 ms (EVTA conv 61.72) | 14.6 | 119.6 | 58 % |
| `MultiEVTAResnet50TvTest` (4장 동시) | `..._4evta_sa_400mhz_rq` | 75.44 / 75.50 / 75.87 / 75.87 ms | 52.9 | 432 | 53 % |
| `MobileNetV2TfTest` | `..._sa_alux3_400mhz_rq` | 14.39 ms (conv 7.63, depthwise 5.76, fc 0.28, 모두 EVTA) | 69.5 | 45.7 | 22 % |
| `MobileNetV2ValTest` | `..._sa_alux3_400mhz_rq` | 14.43 ms | 69.3 | 45.6 | 22 % |
| `MultiEVTAMobileNetV2TfTest` (4장 동시) | `..._4evta_sa_400mhz_rq` | 17.06 / 17.07 / 17.30 / 17.30 ms | 233 | 153 | 19 % |

- MobileNetV2의 시간은 마지막 실행의 호출별 합이다(`hw/MobileNetV2TfTest/timing.csv`). conv, depthwise, fc, avgpool 모두 EVTA에서 돈다.
  - depthwise는 tap마다 대각 16 × 16 weight GEMM이라 곱셈기의 1/16만 쓴다. 가동률이 낮은 주된 까닭이다.
  - `run_prof.sh` 요약의 "cpu conv/fc 6.05 ms"는 이름으로 나눈 값이다. `dw_convolution_evta`와 `fullyconnected_evta_packed`가 이쪽에 들어가므로 실제 CPU 시간이 아니다.
- EVTA 4개 판은 ALU 확장이 없다. 그래서 residual add와 avgpool도 CPU에서 돌고, 장당 시간이 늘어난다.
- 같은 칸끼리 DPU와 견주면 다음과 같다.
  - ResNet50: DPU S가 우리 1개의 5.5배, DPU T가 우리 4개의 3.6배.
  - MobileNetV2: S가 3.9배, T가 3.3배.

### 6.2 정확도 (ImageNet val 50,000장)

| bundle | float (PC) | int8 EVTA (보드) | 손실 | DPU 손실 |
|---|---|---|---|---|
| `Resnet50TvTest` | 76.15 % | **75.29 %** (37,643 / 50,000) | **−0.86 %p** | −0.1 %p (76.1 → 76.0) |
| `MobileNetV2TfTest` | 71.80 % | **68.43 %** (34,217 / 50,000) | **−3.37 %p** | −2.46 %p (70.13 → 67.67) |
| `MobileNetV2ValTest` (QAT) | 70.91 % | **69.24 %** (34,622 / 50,000) | −1.67 %p | (다른 모델) |

- 측정: 보드 `_alux3`, OMP 4, 2026-11-23. 걸린 시간은 4,082 s / 1,516 s / 1,494 s다. PNG 읽기를 포함하고, `imagenet_eval/result/dpu_{r50tv,mbv2tf,mbv2val}.log`에 있다.
- **ResNet50:** 손실이 DPU보다 0.76 %p 크다. 같은 거듭제곱 scale이지만 DPU 양자화기는 다음을 쓴다.
  - weight / activation / bias마다 따로 정한 fix position.
  - ImageNet train calibration.
  - 필요하면 fast finetune.
  - 우리 남은 손실의 후보: weight scale이 tensor마다 하나(DPU도 tensor마다 하나), calibration 200장(ImageNetV2), conv 출력 shift의 반올림 방식. 층별 비교로 어디서 잃는지 찾아야 한다.
- **MobileNetV2:** float 모델 그대로 양자화하면 −3.37 %p로 DPU(−2.46 %p)보다 0.9 %p 크다.
  - int8 절대값은 68.43 %로 DPU 표(67.67 %)보다 높다. 전처리가 달라 float가 1.67 %p 높은 덕이다.
  - QAT 모델은 −1.67 %p로 손실이 가장 작다. 다만 QAT는 학습을 다시 한 모델이라 DPU의 float 모델과 같은 조건은 아니다.
- **ImageNetV2에서 본 손실과 다르다.** ResNet50 −0.81 → −0.86 %p로 비슷하다. TF-slim MobileNetV2는 hold1000 −1.7 → val −3.37 %p로 커졌다. 1,000장의 표준 오차가 약 1.5 %p라 그 정도는 흔들린다.

### 6.3 먼저 해 본 진단 (ImageNetV2, dir의 정식 셋이 아님)

| bundle | 이미지 | float | int8 | 손실 |
|---|---|---|---|---|
| `Resnet50TvTest` | ImageNetV2 10,000 | 63.34 % | 62.53 % | −0.81 %p |
| `MobileNetV2TfTest` | ImageNetV2 hold1000 | 61.6 % | 59.9 % | −1.7 %p |
| `MobileNetV2ValTest` | ImageNetV2 10,000 | 58.71 % | 56.66 % | −2.05 %p |

- ImageNetV2는 같은 모델에서도 ImageNet val보다 12–13 %p 낮다(float: ResNet50 76.15 대 63.34, TF-slim MobileNetV2 71.80 대 59.24, QAT 70.91 대 58.71 %). 그래서 DPU 수치와는 ImageNet val로만 견준다.
- calibration을 바꾸기 전(Glow 기본: 10칸 histogram, 거듭제곱 scale을 늘 올림) torchvision ResNet50의 손실은 −4.3 %p였다(hold1000). 자세한 것은 [evta_accuracy.md](evta_accuracy.md) §7에 있다.

## 7. 조건이 다른 점 (해석할 때 주의)

- **calibration 셋:**
  - 우리는 ImageNetV2 200장이고, DPU는 보통 ImageNet train에서 고른다.
  - DPU에는 fast finetune(양자화 후 짧게 다시 학습하는 기능)도 있다. 표의 값이 그것을 썼는지는 알 수 없다.
- **시간에 넣은 것:**
  - DPU의 E2E에는 JPEG 읽기와 resize가 들어 있는 것으로 보인다.
  - 우리 시간에는 PNG 읽기가 없다. 우리 main은 이미 224로 자른 PNG를 읽는다.
  - 대신 float 입력의 양자화와 첫 conv용 재배치는 들어 있다.
- **thread:**
  - 우리 EVTA 1개는 CPU 1코어(OMP 1)로 쟀다. DPU S와 같은 조건이다.
  - EVTA 4개는 copy마다 1코어, 모두 4코어다.
- **보드:** DPU는 281 MHz이고 보드 revision 05다. 우리는 연구실 ZCU102다. PL에 결함 행(Y219)이 있어 EVTA 4개 판은 그 행을 피해 배치한다.
- **연산량:** MobileNetV2는 채널을 넓힌 것 때문에 우리 연산량이 9 % 크다(§5).

## 8. 다시 재는 법

```bash
# PC: float 기준
~/.venvs/onnxeval/bin/python vtabundles_xcmpl/imagenet_eval/float_ref_cnn.py resnet50 Debug2_remote/vta/bundles/Resnet50TvTest/resnet50.onnx ~/data/imagenet_val_tv224 50000 val_float_r50tv
~/.venvs/onnxeval/bin/python vtabundles_xcmpl/imagenet_eval/float_ref_cnn.py mobilenetv2 Debug2_remote/vta/bundles/MobileNetV2TfTest/mobilenetv2_tf_f32.onnx ~/data/imagenet_val_tf224 50000 val_float_mbv2tf
# 보드 (vtabundles_xcmpl/): 정확도, 시간
sudo /bin/sh -c "imagenet_eval/eval_board.sh hw/Resnet50TvTest/vtaTvResnet50Eval_sys dpu_r50tv"
sudo /bin/sh -c "imagenet_eval/eval_board.sh hw/MobileNetV2TfTest/vtaMobileNetV2TfBundle_sys dpu_mbv2tf"
sudo /bin/sh -c "imagenet_eval/eval_board.sh hw/MobileNetV2ValTest/vtaMobileNetV2ValBundle_sys dpu_mbv2val"
sudo /bin/sh -c "hw/script/run_prof.sh -s _pcsystimed -n 25 Resnet50TvTest MobileNetV2TfTest MobileNetV2ValTest"
sudo /bin/sh -c "hw_multievtax4/script/run_prof.sh -s _pcsystimed -n 25 MultiEVTAResnet50TvTest MultiEVTAMobileNetV2TfTest"
```

- PC cross build dir:
  - single `_sys`: `build_sys`.
  - single `_pcsystimed`: `build_pc_systimed`.
  - EVTA 4개 `_pcsystimed`: `build_pc_m4_systimed`.
- target 이름:
  - `vtaTvResnet50Bundle`, `vtaTvResnet50Eval`
  - `vtaMobileNetV2TfBundle`, `vtaMobileNetV2ValBundle`
  - `singlevta` / `multivtaTvResnet50Bundle`, `singlevta` / `multivtaMobileNetV2TfBundle`

## 9. 남은 일

- ResNet50 / MobileNetV2 Tf의 남은 손실(DPU보다 0.8–0.9 %p 큼)이 어느 층에서 나는지 찾는다. Glow Interpreter 층 출력과 float 층 출력을 견주고, per-channel weight scale과 calibration 그림 수를 시험한다.
- ALU 확장 3단계 EVTA 4개 bitstream(`_ni`)이 나오면 §6.1의 EVTA 4개 줄을 다시 잰다. DeiT도 같이 잰다.
- 보드의 ImageNet val PNG(`~xilinx/JO/imagenet_val_tv224`, `imagenet_val_tf224`, 합 8.8 GB)는 다시 잴 때를 위해 둔다. 보드 디스크가 차면 실행이 조용히 끝나므로 다른 큰 파일(dump 등)은 쓴 뒤 지운다(2026-11-23 여유 11 GB).
