# EVTA와 AMD DPU 비교: ResNet50, MobileNetV2 (2026-11-22, 11-24 다시 잼)

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
| | 우리 EVTA 1개 `Resnet50TvTest` (11-24 §6.5) | 15.7 | 128.4 | **63 %** | 76.15 → **75.29 %** (−0.86 %p) |
| | 우리 EVTA 4개 `MultiEVTAResnet50TvTest` (`_amul`, 11-24 §6.5) | 56.6 | 463 | **57 %** | 같은 bundle |
| MobileNetV2 | DPU S | 270.0 | 163 | 14 % | 70.13 → 67.67 % (−2.46 %p) |
| | DPU T | 764.4 | 460 | 13 % | |
| | 우리 EVTA 1개 `MobileNetV2TfTest` | 69.5 | 45.7 | **22 %** | 71.80 → **68.43 %** (−3.37 %p) |
| | 우리 EVTA 4개 `MultiEVTAMobileNetV2TfTest` (`_amul`, 11-24) | 235.6 | 155 | **19 %** | 같은 bundle |

- **가동률은 우리가 같거나 높다.** 곱셈기를 놀리지 않는 효율은 뒤지지 않는다.
- **절대 성능은 DPU가 3.3–5.5배다.** peak 차이만큼이다.
  - EVTA 1개 204.8 GOPS 대 B4096 1개 1,151 GOPS (5.6배).
  - EVTA 4개 819 대 B4096 3개 3,453 GOPS (4.2배).
- **EVTA 4개는 `_amul`(ALU MUL immediate만, 11-24)로 다시 쟀다(§6.4).** avgpool이 EVTA로 갔지만 `_rq` 때와 거의 같다(52.9 / 233 → 52.9 / 235.6 fps). ALU 확장 3단계 multi4는 배치·배선이 안 되어 멈췄다([evta_multi4_alux3.md](evta_multi4_alux3.md) §4.2).
- **클럭당으로 보면 우리 EVTA 4개(2,048 op)가 B4096 1개(4,096 op)의 반이다.** 그래서 우리 multi4를 DPU **S**(B4096 1개)와 견주면 ResNet50 56.6 대 79.9 fps(71 %, §6.5), MobileNetV2 235.6 대 270.0 fps(87 %)다. 이 두 모델에서 이 차이를 줄이는 것이 다음 목표다(§6.4).

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

**같은 표의 다른 Zynq 보드** (11-23 추가; 나중에 KV260에 EVTA 1개를 넣어 견줄 때)

| 보드 | DPU 구성 (표 머리글 그대로) | ResNet50 S / T fps | MobileNetV2 S / T fps |
|---|---|---|---|
| ZCU102 | `3* DPUCZDX8G B4096 @ 281MHz` | 79.92 / 189.67 | 270.02 / 764.41 |
| ZCU104 | `2 * DPUCZDX8G B4096 @ 300MHz` | 84.74 / 161.92 | 284.71 / 621.04 |
| **KV260** | `1* DPUCZDX8G B4096F @ 300MHz` | **85.51 / 89.34** | **290.01 / 338.08** |

- KV260은 DPU가 1개라 T가 S와 비슷하다. S는 300 MHz라 ZCU102(281 MHz)보다 조금 빠르다.
- B4096 1개의 peak는 4,096 × 300 MHz = 1,229 GOPS다. KV260 ResNet50 S는 85.51 × 8.2 = 701 GOPS(57 %)다.
- `B4096F`의 F가 무엇인지는 이 표에 설명이 없다(PG338에서 확인할 것).

## 3. 우리 하드웨어와 소프트웨어

| 구분 | 파일 / 값 |
|---|---|
| EVTA 1개 bitstream | `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_alux3_400mhz_rq.bin` (보드 `/lib/firmware`, 2026-11-15; `vtabundles_xcmpl/hw/script/run_all.sh`의 `SYS_BIN`) |
| | systolic GEMM 16 × 16, 400 MHz, requantize(`_rq`), ALU 확장 3단계(`_alux3`), WNS +0.022 ns ([evta_isa_ext.md](evta_isa_ext.md) §8.15) |
| | peak 16 × 16 MAC × 2 × 400 MHz = **204.8 GOPS** |
| EVTA 4개 bitstream | 11-24부터 `vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta_sa_amul_400mhz_hp322_rq.bin` (`hw_multievtax4/script/run_all.sh`의 `SYS_BIN`): `_rq` + ALU MUL immediate, HP 포트 321.7 MHz ([evta_multi4_alux3.md](evta_multi4_alux3.md) §6.2) |
| | 11-22의 §6.1은 `vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta_sa_400mhz_rq.bin` (2026-11-04, 각 400 MHz `_rq`, **ALU 확장 없음**, WNS +0.002 ns, [evta_isa_ext.md](evta_isa_ext.md) §7.18) |
| | peak 4 × 204.8 = **819.2 GOPS** |
| | ALU 확장 3단계 multi4(`_ni`, crossbar, 1단계)는 배치·배선이 안 되어 멈췄다([evta_multi4_alux3.md](evta_multi4_alux3.md) §4.2) |
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

### 6.4 다시 잼: EVTA 1개 `_alux3`, EVTA 4개 `_amul` (2026-11-24)

- **bitstream:** EVTA 1개 `..._16_16_19_18_sa_alux3_400mhz_rq.bin`, EVTA 4개 `..._4evta_sa_amul_400mhz_hp322_rq.bin` (각 `run_all.sh`의 `SYS_BIN`).
- **실행 파일:** 보드에서 11-24 17:18에 다시 링크(`build_systimed`, `build_multievtax4_systimed`; vtalib-zcu102-systolic `398f358`, `run_prof.sh`의 `exe :` 줄). bundle `.cpp`는 11-24 이전 생성(11-22 정확도 수정 뒤; model-compiler 줄 없음). 11-22의 §6.1은 PC cross build(`_pcsystimed`)였다.
- **조건:** OMP 1(copy마다 CPU 1코어), 25회 중 2–25회 중앙값, `cat_285.png`(VGG16은 CIFAR-10 `airplane/0001.png`). 로그: 보드 `~/JO/dpu_meas_261124/`.
- **EVTA 4개는 lockstep 없이**(`-l` 없이, copy가 서로 기다리지 않고 각자 돈다) 잰 값을 쓴다. DPU T(여러 thread가 각자 돈다)와 같은 조건이다. `-l`(매 추론을 4 copy가 함께 시작)은 가장 느린 copy를 기다리므로 1.5–2 ms 길다(아래 표 괄호).
  - 11-22 §6.1(17.06 ms)과 ubun의 `_amul` 측정(18.69 ms, [evta_multi4_alux3.md](evta_multi4_alux3.md) §6.2)의 차이도 이것이다: 앞은 `-l` 없이, 뒤는 `-l`.

**DPU와 견주는 두 모델** (DPU와 같은 float 원본, ImageNet val 정확도는 §6.2)

| 모델 | | 장당 시간 | fps | GOPS | 가동률 | DPU 대비 fps |
|---|---|---|---|---|---|---|
| ResNet50 | 우리 EVTA 1개 `Resnet50TvTest` | 68.41 ms (EVTA conv 61.73) | 14.6 | 119.6 | 58 % | DPU S 79.9의 18 % |
| | 우리 EVTA 4개 `MultiEVTAResnet50TvTest` | 75.57 ms (`-l` 77.88) | **52.9** (`-l` 51.4) | 433 | 53 % | DPU T 189.7의 28 %, **DPU S 79.9의 66 %** |
| | DPU S / T | | 79.9 / 189.7 | 655 / 1,555 | 57 / 45 % | |
| MobileNetV2 | 우리 EVTA 1개 `MobileNetV2TfTest` | 14.41 ms (conv 7.65) | 69.4 | 45.7 | 22 % | DPU S 270.0의 26 % |
| | 우리 EVTA 4개 `MultiEVTAMobileNetV2TfTest` | 16.98 ms (`-l` 18.72) | **235.6** (`-l` 213.7) | 155 | 19 % | DPU T 764.4의 31 %, **DPU S 270.0의 87 %** |
| | DPU S / T | | 270.0 / 764.4 | 163 / 460 | 14 / 13 % | |

- 11-22(`_rq`, `_pcsystimed`)와 같다: ResNet50 52.9 → 52.9, MobileNetV2 233 → 235.6 fps. `_amul`의 EVTA avgpool은 장당 0.1 ms 남짓이다.
- **EVTA 4개는 클럭당 2,048 op로 B4096 1개(4,096 op)의 반이다.** 클럭도 400 대 281 MHz라 peak는 819 대 1,151 GOPS(71 %)다. 그래서 multi4를 DPU S와 견주는 것이 구조끼리의 비교에 가깝다.
  - ResNet50: peak 비(71 %)보다 조금 낮은 66 %. 가동률 53 % 대 57 %다.
  - MobileNetV2: 87 %. 가동률 19 % 대 14 %로 우리가 높다(우리 연산량이 9 % 큰 것을 빼도 높다).
- **EVTA 4개의 copy 하나는 EVTA 1개보다 느리다:** ResNet50 68.4 → 75.6 ms(+10 %), MobileNetV2 14.4 → 17.0 ms(+18 %).
  - 같은 multi4 bitstream에서 copy 하나만 돌리면(`singlevta*`, 100회) 72.1 / 14.85 ms다. buffer가 작은 것(16/15/18/17 대 16/16/19/18)과 ALU 확장이 없는 것이 +3.7 / +0.45 ms이고, 나머지 +3.5 / +2.1 ms는 4 copy가 같이 돌 때의 DDR / CPU 경쟁이다.
  - MobileNetV2는 CPU 쪽 시간(`cpu conv/fc` 6.85 ms, 이름으로 나눈 값이라 depthwise EVTA 호출도 들어 있다)이 커서 경쟁에 더 약하다.

**나머지 모델** (int8, 같은 조건; 정확도는 [evta_accuracy.md](evta_accuracy.md))

| 모델 (연산량) | EVTA 1개 `_alux3` | | | EVTA 4개 `_amul` (4장 동시, `-l` 없이) | | | 참고: EVTA 4개 bitstream에서 copy 1개만 |
|---|---|---|---|---|---|---|---|
| | 장당 ms | fps | GOPS (가동률) | copy 장당 ms (`-l`) | fps | GOPS (가동률) | ms |
| `Resnet50Test` Caffe2 (8.25 GOP) | 64.48 | 15.5 | 128.0 (62 %) | 75.60 (78.04) | 52.9 | 433 (53 %) | 72.11 |
| `MobileNetV2Test` QAT (0.658 GOP) | 14.39 | 69.5 | 45.7 (22 %) | 16.98 (18.74) | 235.6 | 155 (19 %) | 14.91 |
| `Resnet18Test` (3.70 GOP) | 22.73 | 44.0 | 162.8 (79 %) | 23.46 (24.00) | 170.5 | 631 (77 %) | 22.93 |
| `Vgg16Cifar10Test` (0.630 GOP) | 6.75 | 148.1 | 93.3 (46 %) | 8.01 (8.13) | 499.5 | 315 (38 %) | 6.99 |
| `DeiTTest` DeiT-tiny (2.18 GOP) | 37.09 | 27.0 | 58.8 (29 %) | 90.66 (97.07) | 44.1 | 96 (12 %) | 83.37 |

- 가동률 = GOPS / peak(EVTA 1개 204.8, 4개 819.2 GOPS). GOPS는 bundle이 EVTA에 보낸 연산량 × fps다.
- **Resnet18**이 가장 잘 돈다(EVTA 4개 631 GOPS, 77 %). 3 × 3 conv가 대부분이고 CPU 일이 거의 없다.
- **VGG16 CIFAR-10**은 32 × 32 입력이라 tile이 작아(마지막 층 2 × 2) 가동률이 낮다.
- **DeiT**는 EVTA 4개에서 나쁘다(장당 90.7 ms, EVTA 1개 37.1 ms의 2.4배). `_amul`에는 ALU 확장 2·3단계가 없어 layer norm / softmax / residual add가 CPU로 가고, 4 copy가 CPU와 DDR을 나눠 쓴다(copy 1개만이면 83.4 ms). DeiT는 EVTA 1개 `_alux3`가 4개 `_amul`의 61 %의 처리량을 낸다.
- **이미 있던 문제:** `MultiEVTAResnet50Test`의 copy는 torchvision 모델로 만들어져 있다(분류 281 (0.832588) = `Resnet50TvTest`; [evta_multi4_alux3.md](evta_multi4_alux3.md) §6.2). 그래서 그 줄의 EVTA 4개 값은 Tv 모델의 값이고 연산량도 8.178 GOP다.

### 6.5 ResNet50Tv의 CPU 일을 EVTA로 (2026-11-24)

§6.4에서 `Resnet50TvTest`에만 CPU 일이 4.7 ms 남아 있었다(Caffe2 `Resnet50Test`는 64.48 ms로 3.9 ms 빠르다).

| CPU 일 (EVTA 1개) | 왜 남았나 | 바꾼 것 |
|---|---|---|
| 입력 전처리 2.87 ms: `quantize` 0.32, `elemsub`(mean) 0.50, `elemdiv`(std) 0.71, `transpose` 1.34 | torchvision 모델의 `_Sub`가 quantize 출력 버퍼에 제자리(in place)로 쓴다. VTASave `findInputPreprocess`가 `dest == 입력`이면, 그리고 그 버퍼의 사용자가 3이면 멈췄다 | 제자리 elemsub / elemdiv를 받는다(그런 명령 수만큼 사용자 수를 더 센다). 이제 `preprocess_conv0pack_stream`이 첫 conv 입력 복사 안에서 채널별 256칸 표로 한다(Caffe2 판과 같은 경로, 표는 CPU elemsub / elemdiv와 같은 계산) |
| layer4.0 relu 1.80 ms (`relu(..., 1/32, 1/16)`) | relu 출력 scale이 입력의 2배(거칠다). conv epilogue는 같거나 2^q 작은 scale만 받았다(q ≥ 0). 또 add가 residual 버퍼에 제자리로 써서, chain 검사("chain의 값은 chain만 읽는다")가 residual을 만든 conv3을 세어 chain 전체(add 포함)를 버렸다 | VTASave `findResidualFusions`: q = −1..−7이면 `CLIP 0..127` 뒤에 `RQ round 1, shift −q`(CPU relu와 같은 반올림). residual은 만든 conv 하나를 더 센다. vtalib-systolic `lowerEpilogue`: 단항 부분의 세 번째 꼴 `clip((x + 2^(k−1)) >> k, a, b)`(kind 3)를 기본 ALU ADD / SHR / MAX / MIN으로(표가 아니라서 `_amul`처럼 ALU 확장이 없어도 EVTA에서 돈다) |

- **같은지:** 보드(`_alux3`)에서 EVTA conv dump 106개(53개 × 2회) 중 layer4.0 downsample conv의 2개만 다르다. 그 conv의 출력이 이제 relu와 반올림 shift 뒤의 값이라서이고, 새 dump = 옛 dump에 `(max(x, 0) + 1) >> 1`을 한 것과 100,352 byte 모두 같다. 나머지 dump와 결과(281, 0.832588)는 같다. 그래서 정확도(ImageNet val 75.29 %)는 그대로다. `EVTA_EPI_CHECK=1 EVTA_RES_CHECK=1` 38건 0 differ.
- **vtalib 시뮬레이터:** `tests/run_epi_test.sh`에 kind 3 경우 5개(이 모양 그대로, 제자리, add 없는 relu 4배, 반올림 shift + clip, add 뒤 shift 7)와 무작위 12개를 더했다. 네 방식(rq 끔 / 켬 / MADD 없음 / ALU 확장 없음) 모두 0 differ이고, `EVTA_EPI_CHECK`로 kind 3이 EVTA에서 돈 것(ALU 확장 없음 포함)을 확인했다.
- weights 파일은 mean / std 상수(300 KB)가 빠져 25.63 MB가 됐다. multi4 copy 4개도 같은 컴파일러로 다시 만들었다(`gen_multi4_bundles.sh MultiEVTAResnet50TvTest`, copy 0 = single).

| | 전 (§6.4) | 후 | fps | GOPS (가동률) |
|---|---|---|---|---|
| EVTA 1개 `Resnet50TvTest` (`_alux3`) | 68.41 ms | **63.68 ms** | 14.6 → **15.7** | 119.6 → 128.4 (58 → 63 %) |
| EVTA 4개 `MultiEVTAResnet50TvTest` (`_amul`, `-l` 없이) | 75.57 ms | **70.67 ms** | 52.9 → **56.6** | 433 → 463 (53 → 57 %) |

- GOPS는 모델의 연산량 8.178 GOP로 셈했다. `run_prof.sh`는 이제 8.251 GOP를 찍는다: 전처리가 들어간 첫 conv가 행마다 나눠 도는 판(stream, 겹치는 행을 다시 계산)이라 EVTA가 하는 연산이 0.9 % 많다(Caffe2 판과 같다).
- DPU S 대비: EVTA 4개 56.6 대 79.9 fps(71 %), EVTA 1개 15.7 대 79.9(20 %).

### 6.6 MobileNetV2Tf multi4의 경쟁분 (2026-11-24)

DPU S(270장/s)를 넘으려면 copy당 14.81 ms가 되어야 한다. 지금은 16.7 ms다. 혼자 돌 때 14.7 ms이므로, 늘어난 2 ms가 4개가 함께 돌아서 생긴 경쟁분이다.

**잰 방법:** 시간 측정 코드가 없는 `_sys` 실행 파일로 벽시계 처리량을 쟀다. `multi_threads_main.cpp`에 두 가지를 더했다.
- `MULTI_FREE=1`: copy들이 추론마다 barrier에서 기다리지 않고 각자 돈다.
- `MULTI_STAGGER_US=<us>`: copy i의 출발을 i × us 늦춘다.
- 둘 다 `THROUGHPUT ... images/s (ms per image per copy)` 줄을 찍는다(2번째 run부터 센다).

**알게 된 것:**
- barrier는 거의 상관없다: barrier 231–236장/s, free 236–245장/s, stagger 224–245장/s(잡음 ±2–4 %). `run_prof.sh`의 235.6 fps는 실제 처리량과 맞다.
- 같이 도는 copy 수에 비례한다(copy당 ms): 1개 14.72, 2개 15.2, 3개 15.7, 4개 16.7. HP 포트 쌍과는 거의 상관없다(DDR 포트를 함께 쓰는 HP1 + HP2도 +0.1 ms).
- EVTA 하나가 추론 한 번에 DDR로 옮기는 양은 11.7 MB다(`EVTA_REPLAY_LOG=2`: wgt 4.43, inp 3.94, out 3.12 MB). 평균 1.5 GB/s, 4개를 합쳐 약 6 GB/s로 DDR4 최대치의 1/3이다. 대역이 꽉 찬 것이 아니라 늦어진 응답을 기다리는 시간으로 본다.
- 늘어난 시간이 큰 층: conv0 +0.39, expanded_conv_1 expand +0.29, 첫 depthwise +0.13, expanded_conv project +0.09 ms(모두 112×112). 나머지는 층마다 +0.01–0.03 ms다.
- 4개 동시 실행에서 schedule을 다시 찾으면 3개 모양이 바뀐다. 처리량은 +0.9–1.5 %(copy당 약 −0.2 ms)다([evta_sched_search.md](evta_sched_search.md) §5; `VTASchedules.h`에 넣었다).

**다음:** depthwise를 더 효율적으로 한다. 지금은 16×16 GEMM의 대각선만 쓴다. depthwise는 copy당 약 6 ms로 가장 큰 덩어리다([evta_todo.md](evta_todo.md) §5).

### 6.7 MobileNetV2Tf multi4: 호출 종류별 시간, EVTA run 안의 GEMM 하한 (2026-11-25, ubun)

depthwise 장치와 conv 개선을 정하려고 다시 쟀다.

**조건:**
- bitstream: `..._4evta_sa_amul_400mhz_hp322_rq.bin`. lockstep 없음, 25회.
- 실행 파일: 11-24 `build_multievtax4_systimed`(vtalib `16ff33a`). bundle은 11-22에 만든 것이다.
- §6.6의 새 schedule 세 개는 `EVTA_SCHED`로 넣었다.

**처리량:** 4개 동시 copy당 16.43–16.75 ms, **238.3 fps**. copy 0 혼자 14.84 ms(67.4 fps).

**호출 종류별 시간**(`timing_<cpp>.csv`를 run 2부터 run마다 더한 값의 중앙값, ms):

| | 혼자 (`singlevta`) | 4개 동시 (copy 넷) |
|---|---|---|
| conv(1×1 / 3×3, conv0 포함, EVTA) | 7.89 | 8.83 / 8.95 / 9.01 / 8.83 |
| depthwise(`dw_convolution_evta`) | 5.95 | 6.29 / 6.35 / 6.37 / 6.31 |
| `VTABufferInvalidate` | 0.48 | 0.50 |
| fc(EVTA) | 0.29 | 0.32–0.36 |
| 그 밖 | 약 0.2 | 약 0.3 |

- 4개가 함께 돌아 늘어나는 몫은 대부분 conv다(+1.0 ms). depthwise는 +0.35 ms다.

**EVTA run 안**(`EVTA_REPLAY_LOG=2`, `singlevta` 혼자, 마지막 run). GEMM 하한 = GEMM 안쪽 반복 수 / 400 MHz.

| | 호출 | 벽시계 | EVTA run 합 | GEMM 하한 | ALU 반복 | DRAM wgt / inp / out |
|---|---|---|---|---|---|---|
| conv(`convolution_wo_tr`) | 35 | 8.66 ms | 7.77 ms | 3.73 ms | 46,550 | 2.2 / 4.0 / 4.2 MB |
| depthwise | 17 | 6.24 ms | 5.84 ms | 3.60 ms | 143,864 | 1.3 / 3.9 / 2.2 MB |
| fc | 1 | 0.31 ms | 0.29 ms | 0.01 ms | 0 | 1.2 MB |
| avgpool | 1 | 0.08 ms | 0.06 ms | 0.02 ms | 4,000 | |

- **depthwise는 연산이 묶고 있다.**
  - 대각 GEMM 하한 3.60 ms와 채널별 SHR(ALU 143,864번, II 2로 약 0.72 ms)을 더하면 run 시간의 약 75 %다.
  - 무거운 층은 112×112 / 56×56의 첫 세 층이다(0.75–1.0 ms씩).
  - tap 9개를 한 번에 하는 장치(144 MAC/cycle)에 채널별 shift까지 넣으면, GEMM 하한은 약 0.4 ms가 되고 ALU 패스는 없어진다.
  - 그러면 DMA(inp 3.9 + out 2.2 MB, weight는 대각을 풀어 1/16)가 남는다. depthwise는 2–2.5 ms 정도로 보고, multi4는 copy당 12.5–13 ms, 약 305–320 fps가 된다(계산값).
- **conv는 EVTA run이 GEMM 하한의 2.1배다**(7.77 대 3.73 ms). 4개 동시이면 1.0 ms가 더 는다. depthwise 다음의 큰 덩어리다.

**conv 쪽에서 GEMM 하한 밖의 시간**(같은 측정을 층별로 나눈 것; 혼자 / 4개 동시 copy 0, ms):

| 무엇 | 시간 | 내용 |
|---|---|---|
| conv0(stream) | 1.66 / 1.83 (GEMM 하한 0.31) | **CPU 입력 묶기가 묶고 있다.** `EVTA_INPUT_STREAM=0`으로 나누면 CPU `preprocess_conv0pack_stream` 1.31 + EVTA run 0.78(0.84 벽시계)이다. stream은 그중 0.5 ms를 겹칠 뿐이다 |
| 호출마다 CPU 쪽 | conv 0.89 + depthwise 0.39 | 벽시계 − EVTA run. 호출마다 20–25 us(replay 재배치, flush, 시작 / 기다림) |
| `VTABufferInvalidate` | 0.48 / 0.50 | EVTA conv / depthwise 출력마다 한 번(가장 큰 것은 expanded_conv_1 expand 출력 1.2 MB의 0.09 ms). 출력을 읽는 쪽이 EVTA뿐이어도 부른다 |
| 7×7 project 세 개(960 → 160 / 320) | 1.02 (GEMM 하한 0.30) | weight LOAD에 묶임(150–300 KB, 0.6–0.65 GB/s) |
| expanded_conv_1 expand(112×112, 16 → 96) | 0.78 / 0.88 (GEMM 하한 0.38) | 출력 1.2 MB STORE |
| 나머지 conv 28개 | 하한의 1.5–2배 | run마다 약 30–60 us 더 |

- 먼저 할 것(소프트웨어만, EVTA 1개에도 듣는다):
  1. conv0 입력 묶기를 빠르게 한다(1.31 ms; 입력 600 KB float → 416 KB).
  2. 출력을 EVTA만 읽는 buffer의 invalidate를 뺀다(0.5 ms). 단, CPU가 그 buffer에 남긴 dirty line이 없는지(buffer pool 재사용) 확인해야 한다.
  3. 호출마다의 CPU 쪽 시간을 줄인다(1.3 ms).
- 셋을 합쳐 copy당 2 ms 남짓이면, depthwise 장치 없이도 DPU S(14.8 ms)에 닿는다(계산값).

**다시 재는 법**(`vtabundles_xcmpl/`):
```bash
S='112,112,32,0,3,1:2,4,112;112,112,32,16,1,1:1,16,112;112,112,16,96,1,1:2,2,112'
sudo /bin/sh -c "EVTA_SCHED=\"$S\" hw_multievtax4/script/run_prof.sh -s _systimed -n 25 MultiEVTAMobileNetV2TfTest"
sudo /bin/sh -c "PROF_EXE=singlevtaMobileNetV2TfBundle EVTA_REPLAY_LOG=2 EVTA_SCHED=\"$S\" hw_multievtax4/script/run_prof.sh -s _systimed -n 3 MultiEVTAMobileNetV2TfTest"
# conv0 without the input stream (CPU pack and EVTA run apart): add EVTA_INPUT_STREAM=0 to the line above
```
- `EVTA_RUN` 줄의 필드는 EVTA, replay id, segment, 명령 수, us, GEMM 반복, ALU 반복, 그리고 uop / wgt / inp / acc / out 바이트다.
- 같은 모양의 호출은 replay id를 같이 쓴다(40개 id, EVTA 호출 54개). 그래서 마지막 run의 `segment 0` 순서와 `timing.csv`의 EVTA 호출 순서로 짝을 지었다. EVTA run이 없는 호출(`preprocess_conv0pack_stream`)은 빼야 한다.

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
# 11-24 (§6.4): 보드에서 빌드한 실행 파일, 7개 모델; EVTA 4개는 -l 있이 / 없이 둘 다
(cd build_systimed && cmake . && make -j2 vtaTvResnet50Bundle vtaMobileNetV2TfBundle vtaCaffe2Resnet50Bundle vtaMobileNetV2Bundle vtaMxnetResnet18Bundle vtaVGG16Cifar10Bundle vtaDeiTBundle)
(cd build_multievtax4_systimed && cmake . && make -j2 multivtaTvResnet50Bundle multivtaMobileNetV2TfBundle multivtaCaffe2Resnet50Bundle multivtaMobileNetV2Bundle multivtaMxnetResnet18Bundle multivtaVGG16Cifar10Bundle multivtaDeiTBundle)
sudo /bin/sh -c "hw/script/run_prof.sh -s _systimed -n 25 Resnet50TvTest MobileNetV2TfTest Resnet50Test MobileNetV2Test Resnet18Test Vgg16Cifar10Test DeiTTest"
sudo /bin/sh -c "hw_multievtax4/script/run_prof.sh -s _systimed -n 25 [-l] MultiEVTAResnet50TvTest MultiEVTAMobileNetV2TfTest MultiEVTAResnet50Test MultiEVTAMobileNetV2Test MultiEVTAResnet18Test MultiEVTAVgg16Cifar10Test MultiEVTADeiTTest"
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
- ~~ALU 확장 3단계 EVTA 4개 bitstream이 나오면 다시 잰다~~: 멈춤. EVTA 4개는 `_amul`로 다시 쟀다(§6.4).
- EVTA 4개 ResNet50 / MobileNetV2를 DPU S에 가깝게: copy 하나가 EVTA 1개보다 10–18 % 느린 까닭(DDR / CPU 경쟁, 작은 buffer)부터 줄인다. `MultiEVTAResnet50Test` copy를 Caffe2 모델로 다시 만든다.
- 보드의 ImageNet val PNG(`~xilinx/JO/imagenet_val_tv224`, `imagenet_val_tf224`, 합 8.8 GB)는 다시 잴 때를 위해 둔다. 보드 디스크가 차면 실행이 조용히 끝나므로 다른 큰 파일(dump 등)은 쓴 뒤 지운다(2026-11-23 여유 11 GB).
