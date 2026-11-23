# CNN calibration을 그림 여러 장으로, 그리고 찾은 첫 conv 반올림 버그 (2026-11-22)

[evta_todo.md](evta_todo.md) 순서 5의 기록이다. 대상은 Resnet50, Resnet18, MobileNetV2이고, DeiT는 버그 수정만 해당한다.
- 그림 한 장으로 만들던 calibration을 200장으로 바꿨다.
- 정확도를 층마다 따라가다가 VTASave의 첫 conv bias 버그를 찾아 고쳤다.
- 같은 날 이어서 한 KL + 거듭제곱 MSE는 [evta_accuracy.md](evta_accuracy.md) §7에 있다.
- VGG16 입력 정규화(순서 6)는 [evta_vgg16_input_norm.md](evta_vgg16_input_norm.md)에 있다.

용어:
- **calibration / profile**: int8의 scale을 정하려고 그림 몇 장을 float로 돌려 tensor마다 값의 범위와 분포를 모은 것(YAML).
- **LSB**: int8 한 칸(값 1).
- **shift**: EVTA conv는 int32 누적값을 오른쪽 shift로 줄여 int8로 만든다. `-vta-round-shift`는 shift 전에 2^(shift−1)을 더해 버림 대신 반올림을 한다.
- **Glow Interpreter**: Glow의 참조 실행기(PC). 같은 양자화로 계산하는 기준이다.

## 1. 시작점: 데이터셋으로 처음 잰 정확도 (11-22 오전)

[evta_accuracy.md](evta_accuracy.md) §1–3에 적은 측정이다.
- 그 전까지 CNN은 그림 한 장과 층 출력 dump가 byte 단위로 같은지로만 검증했다.
- 이번에 처음으로 데이터셋(ImageNetV2 10,000장, 보드 `_alux3`)으로 쟀다.

| 모델 | float (PC) | EVTA int8 (보드) | 손실 |
|---|---|---|---|
| Resnet50 (Caffe2) | 57.50 % | 44.66 % | **−12.84 %p** |
| Resnet18 (MXNet) | 58.16 % | 52.61 % | −5.55 %p |
| MobileNetV2 (TFLite QAT) | 58.71 % | 56.19 % | −2.52 %p |
| DeiT-tiny | 62.41 % | 60.37 % | −2.04 %p |

- 손실이 큰 첫 의심은 calibration이었다. `vta/bundles/<Dir>/CMakeLists.txt`의 `image-classifier -dump-profile`이 `cat_285.png` 한 장만 쓴다.

## 2. 바꾼 것

### 2.1 그림 200장 profile (`gen_calib_bundle.sh`)

- **그림:** `vtabundles_xcmpl/imagenet_eval/calib_imagenetv2_200.txt`. ImageNetV2 list.txt에서 50장마다 1장씩 뽑았다. 클래스가 섞인 순서라 200장이 고르게 퍼진다.
  - 평가 10,000장의 2 %와 겹친다. 그래서 calibration 방식을 견줄 때는 겹치지 않는 1,000장(`~xilinx/JO/hold1000.txt`)을 썼다.
- **스크립트:** `vtabundles_xcmpl/gen_calib_bundle.sh <Dir> <그림 목록 | profile.yaml[.gz]> [model-compiler 옵션]` (새로 만듦).
  - CMakeLists와 같은 `image-classifier` / `model-compiler` 옵션을 쓴다.
  - 그 위에 `-image-channel-order`를 bundle main이 넣는 순서로 맞추고, `-minibatch=1`을 쓴다. CMakeLists는 Resnet18 / VGG16에서 Glow 기본인 BGR을 썼지만, main은 RGB를 넣는다.
  - 출력은 CMake 규칙과 같은 곳인 `Debug2_remote/vta/bundles/<Dir>`다. 그다음 `collect_hw_bundles.py <Dir>`.
  - 왜 CMake가 아닌가: Debug2_remote에서 cmake를 다시 돌리면 Glow 전체를 다시 빌드한다.
- **profile 파일:** `vta/bundles/<Dir>/calib/`에 저장했다. 같은 profile로 만들면 bundle이 byte 단위로 같게 다시 나온다.
  - `resnet50_calib_n200.yaml`, `mxnet_exported_resnet18_calib_n200.yaml`, `mobilenetv2_calib_n200.yaml`
- **`-vta-round-shift`:** 이 스크립트는 늘 켠다. 그 전에는 MobileNetV2와 DeiT만 켰다. Resnet18 / 50에는 새로 켜졌다.

**Glow Interpreter로 본 그림 수의 효과 (Resnet50, 1,000장):** 1장 50.4 % → 200장 56.4 %.
- 그런데 보드는 200장으로 바꿔도 46 %대에 머물렀다. Interpreter와 보드 사이에 다른 차이가 있다는 뜻이었다. 그것이 §2.2다.

### 2.2 첫 conv bias에 반올림 항이 빠져 있었다 (VTASave 버그)

**찾은 방법**
1. Glow Interpreter의 층별 출력을 뽑았다: `image-classifier <그림> -backend=Interpreter -load-profile=<같은 profile> -instrument-debug -instrument-debug-format=rawbin -minibatch=0`.
2. 보드의 EVTA conv 층 출력을 뽑았다: `EVTA_DUMP_CONV=<dir>`.
3. 층마다 int8 값을 견줬다. 첫 층(conv0 + max pool)부터 보드 쪽이 **평균 0.35 LSB 낮았고**, 뒤로 갈수록 차이가 커졌다.
4. conv 하나를 보드의 입력으로 numpy에서 다시 계산했다. 보통 conv는 99.5–99.9 % 같았고, 첫 층만 어긋났다. 평균이 한쪽으로 0.35 LSB 낮은 것은 "반올림이 버림이 됐다"는 모양이다.

**원인 (`lib/Backends/VTA/VTASave.cpp`)**
- `-vta-round-shift`는 `convRoundHalf()`가 2^(shift−1)을 구하고, bias에 더해 weights 파일에 쓴다.
- 첫 conv(packed 입력 경로)는 bias를 다른 함수 `prepareFirstBNNVTAConvolutionInst()`에서 쓴다. 이 함수는 그 항을 더하지 않았다.
  - 호출하는 쪽은 "반올림이 들어간 bias가 있다"고 보고 bias LOAD를 넣었다. 그래서 반올림 없는 bias로 shift = 버림이 됐다.
- 그리고 pool을 합친 conv(`getPoolFusion`)는 `convRoundHalf()`가 아예 0을 돌려주었다.
  - 막을 이유는 없다. max pool과 채널마다 같은 상수 더하기는 순서를 바꿔도 같다: max(x + c) = max(x) + c. shift도 단조 함수다.
  - 그래서 conv에 c를 먼저 더하고 pool해도 CPU의 conv → relu → pool과 같은 값이 나온다.

**고친 코드 (commit `d5838ff`)**

```cpp
// convRoundHalf(): pool-fused conv도 반올림
//if (!vtaRoundShift || ctx->getPoolFusion(Inst))
if (!vtaRoundShift)
  return 0;

// prepareFirstBNNVTAConvolutionInst(): 첫 conv bias에도 2^(shift-1)
const int32_t roundHalf = convRoundHalf(Inst, ctx);
doBias = roundHalf != 0;
...
int32_t data = handleBias.raw(i) + roundHalf;
fos->write((const char *) &data, 4);
```

- 고친 뒤 Resnet50(hold1000): 46.2 → **55.4 %**. Glow Interpreter의 같은 양자화(56.4 %)에 가까워졌다.
- `-vta-round-shift`를 쓰던 MobileNetV2와 DeiT도 conv0 weights 파일이 바뀐다. 코드는 같다.
- 다른 conv의 출력은 그대로다. EVTA 층 dump로 확인했다.

### 2.3 해 보았지만 기본으로 하지 않은 것: scale을 2의 거듭제곱으로 묶지 않기

- **방식:** `QSCHEMA=symmetric gen_calib_bundle.sh ...`. DeiT에 쓴 방식이다(`deit_optimization.md` §6.6). tensor scale은 자유롭게 두고, EVTA 비율(in × w / out)만 `-vta-pow2-ratio`로 2의 거듭제곱에 맞춘다.
- **Glow Interpreter:** Resnet50 64 %(100장). float(62 %, 그 그림들)만큼 나온다.
- **보드:** Resnet18 32.30 %였다. 고친 것 두 가지:
  - vtalib CPU `relu`의 scale 비율이 `(int)` 버림이었다(1.48 → 1, 0.5 → 0). 16.16 고정소수점 곱에 반올림으로 고쳤다.
    - 정수 비율이면 byte가 전과 같다. 두 vtalib 모두 고쳤다(`vta/vtalib` `e6362d0`, `vtalib-zcu102-systolic`).
    - 32.30 → 47.65 %.
  - `-vta-pow2-ratio`가 conv 출력 scale을 바꿀 때, 뒤의 MaxPool / Relu(입력 scale을 그대로 쓰는 op)가 따라가지 않아 컴파일이 멈췄다. `VTA.cpp` `pow2Ratios`의 `follower`로 고쳤다.
- **남은 문제:**
  - 47.65 %는 거듭제곱 판보다 낮다.
  - residual add가 conv에 합쳐지지 않는다. 합치려면 거듭제곱 scale이 필요하다. 그래서 느리다(Resnet18 eval 330 → 465 s).
  - §2.2 버그를 고치기 전에 잰 값이라 다시 재야 한다.
  - KL + 거듭제곱 MSE(§4)가 손실을 DPU 수준으로 줄였으므로 우선순위는 낮다.

## 3. 결과 (ImageNetV2 10,000장, 보드 `_alux3`)

시간은 `run_prof.sh -s _systimed` OMP 1, 25회 중앙값이다.

| 모델 | float | 전 | **후** | 1장 시간 전 → 후 (ms) | 바꾼 것 |
|---|---|---|---|---|---|
| Resnet50 | 57.50 % | 44.66 % | **52.92 %** | 64.18 → 64.59 | §2.2 버그, 200장, `-vta-round-shift` |
| Resnet18 | 58.16 % | 52.61 % | **55.67 %** | 22.55 → 22.72 | 같음 |
| MobileNetV2 | 58.71 % | 56.19 % | **56.27 %** | 14.45 → 14.44 | §2.2 버그, 200장 |
| DeiT-tiny | 62.41 % | 60.37 % | **60.50 %** | 37.06 → 37.01 | §2.2 버그만(그림, 설정은 그대로) |

- Resnet18 / 50이 0.2–0.4 ms 느려진 원인은 따로 재지 않았다. 새로 켠 `-vta-round-shift`로 보인다: 반올림 항이 bias로 들어가므로, bias가 없던 conv도 bias를 읽는다.
- 각 Dir의 지금 값은 `vtabundles_xcmpl/hw/script/accuracy.txt`에 있다. `run_prof.sh`가 요약 줄에 보여 준다.
- MobileNetV2는 QAT weights라 그림 수의 효과가 작다.

## 4. 그다음 (같은 날 저녁): KL + 거듭제곱 MSE

200장으로도 Resnet50은 −4.6 %p였다. DPU는 같은 거듭제곱 scale로 −0.1 %p다.
- 두 가지가 문제였다.
  - Glow는 거듭제곱 scale을 늘 큰 쪽으로 올린다.
  - profile histogram이 10칸이라 KL calibration이 꺼져 있었다.
- `-num-histogram-bins=2048`, `-quantization-calibration=KL`, 새 옵션 `-quantization-pow2-mse=2`로 torchvision ResNet50의 손실이 −0.81 %p가 됐다(ImageNetV2).
- 자세한 것은 [evta_accuracy.md](evta_accuracy.md) §7에 있다.
- DPU 비교 bundle은 이 방식이 기본이다([evta_dpu_compare.md](evta_dpu_compare.md)). 기존 Dir의 bundle은 이 문서의 200장 판 그대로다.

## 5. 명령

```bash
# PC, nestc-build 컨테이너 안 (vtabundles_xcmpl/): 200장 profile + bundle
sudo -n docker exec -u ubuntu nestc-build bash -c "cd $PWD && IMGDIR=/home/jerry/data/imagenetv2_png224 \
    ./gen_calib_bundle.sh Resnet50Test imagenet_eval/calib_imagenetv2_200.txt"
# 저장한 profile로 같은 bundle (byte 단위로 같다)
sudo -n docker exec -u ubuntu nestc-build bash -c "cd $PWD && ./gen_calib_bundle.sh Resnet50Test ../vta/bundles/Resnet50Test/calib/resnet50_calib_n200.yaml"
#   Resnet18Test: calib/mxnet_exported_resnet18_calib_n200.yaml,  MobileNetV2Test: calib/mobilenetv2_calib_n200.yaml
#   QSCHEMA=symmetric ./gen_calib_bundle.sh ...  (§2.3, 기본 아님)
python3 collect_hw_bundles.py Resnet50Test
sudo -n docker exec -u ubuntu -w $PWD/build_sys nestc-build sh -c "make -j16 vtaCaffe2Resnet50Eval"

# 보드 (vtabundles_xcmpl/): 10,000장 top-1, 시간
sudo /bin/sh -c "imagenet_eval/eval_board.sh hw/Resnet50Test/vtaCaffe2Resnet50Eval_sys r50_n200"
sudo /bin/sh -c "hw/script/run_prof.sh -s _systimed -n 25 Resnet50Test"

# 층 비교 (§2.2): Glow Interpreter 층 출력과 보드 EVTA conv dump
Debug2_remote/glow/bin/image-classifier <png> -m=<model.onnx> -model-input-name=<in> -backend=Interpreter \
    -load-profile=<yaml> -quantization-schema=symmetric_with_power2_scale -instrument-debug -instrument-debug-format=rawbin -minibatch=0 <image 옵션>
sudo /bin/sh -c "cd hw/Resnet50Test && EVTA_DUMP_CONV=/home/xilinx/JO/dump_r50 ./vtaCaffe2Resnet50Bundle_sys <png>"
```

- `<image 옵션>`은 `gen_calib_bundle.sh`의 `PF`다. 보드 dump는 다 쓴 뒤 지운다. 보드 디스크가 차면 실행이 조용히 끝난다.

## 6. 바뀐 파일

| 파일 | 무엇 |
|---|---|
| `lib/Backends/VTA/VTASave.cpp` | 첫 conv bias의 반올림 항, pool을 합친 conv의 반올림 (§2.2) |
| `lib/Backends/VTA/VTA.cpp` | `pow2Ratios`: 입력 scale을 쓰는 MaxPool / Relu가 따라감 (§2.3) |
| `vta/vtalib`, `vtalib-zcu102-systolic` | CPU relu rescale 반올림 (§2.3) |
| `vtabundles_xcmpl/gen_calib_bundle.sh` | 새로 만듦 (§2.1) |
| `vtabundles_xcmpl/imagenet_eval/calib_imagenetv2_200.txt` | calibration 그림 목록 |
| `vta/bundles/{Resnet50Test,Resnet18Test,MobileNetV2Test}/calib/*_n200.yaml` | 200장 profile |
| `vtabundles_xcmpl/hw/script/accuracy.txt` | Dir별 정확도 |

## 7. 남은 일

- CMake 규칙(`build_sim_bundles.sh`)은 아직 그림 1장 profile을 만든다. 그것으로 다시 만들면 §1의 bundle로 돌아간다. 200장(또는 2048칸) profile을 CMake에도 넣을지 정해야 한다.
- 다른 모델에도 `-vta-round-shift`를 기본으로 할지 정해야 한다(`evta_todo.md` §3).
- symmetric scale(§2.3)은 버그를 고친 뒤 다시 재야 한다.
