# EVTA on ZCU102 따라 하기 매뉴얼 (컴파일러 빌드 → 번들 생성 → 보드 빌드 → 실행 → 로그)

처음부터 끝까지 명령을 그대로 복사해 실행할 수 있게 썼다. 각 명령은 2026-11-10의 보드와 저장소 상태에서 확인한 것이다.

- **옵션 참고서:** [evta_user_manual.md](evta_user_manual.md). 비트스트림 만들기, 모든 `-vta-*` 옵션, 모든 `EVTA_*` 환경변수, 하드웨어 시험이 있다.
- **이 문서의 범위:** 소프트웨어 흐름. 비트스트림 합성(PC docker)은 참고서 §1을 본다.

## 0. 전체 흐름

```
[PC]  코드 수정 → git commit / push
                       │ (git만으로 동기화)
[보드] git pull
  ① 컴파일러 빌드        Debug2_remote/          cmake(처음 한 번) + make -j2 model-compiler image-classifier VTASaverTest
  ② 번들 소스 생성       Debug2_remote/vta/bundles/<Dir>/   VTASaverTest | image-classifier(profile) + model-compiler | 스크립트
  ③ 보드용으로 모으기    vtabundles_xcmpl/hw/<Dir>/         collect_hw_bundles.py
  ④ 실행 파일 빌드       vtabundles_xcmpl/build*/           cmake(처음 한 번) + make -j2 <target>
  ⑤ 실행 / 측정          vtabundles_xcmpl/                  sudo hw/script/run_all.sh | run_prof.sh
  ⑥ 로그 / 결과          hw/<Dir>/*.log, hw/result/*.txt, timing.csv
```

- **보드 한 대에서 두 가지를 다 한다.**
  - `Debug2_remote`는 NEST-C 전체(Glow + VTA backend)의 빌드 디렉터리다. EVTA 시뮬레이터(`NESTC_USE_VTASIM=ON`)로 빌드돼 있다. 여기서 번들 **소스**(`.cpp`, `.weights.bin`)를 만든다.
  - `vtabundles_xcmpl/`은 그 소스를 실제 EVTA용 vtalib과 링크해 **실행 파일**을 만드는 독립 CMake 프로젝트다.
- **왜 나뉘었나:** `NESTC_EVTA_RUN_ON_ZCU102=ON`으로 NEST-C를 빌드하면 model-compiler 등이 빌드되지 않는다(`cmake/glow/CMakeLists.txt`). 그래서 생성은 시뮬레이터 빌드에서 하고, 실행 파일은 따로 만든다.

경로 약어 (아래 명령에서 쓴다):

```bash
R=~/JO/npuprg/nestc2_clion/nest-compiler        # 보드의 저장소
D=$R/Debug2_remote                               # 컴파일러 빌드 디렉터리
X=$R/vtabundles_xcmpl                            # 보드용 빌드 / 실행
IMG=$R/glow/tests/images/imagenet                # 시험 이미지 (cat_285.png, dog_207.png, zebra_340.png)
```

## 1. 준비

### 1.1 접속과 소스 맞추기

```bash
# PC에서 보드로
ssh xilinx@192.168.20.2
# 보드: 최신 소스 (PC에서 push한 뒤)
cd $R && git pull && (cd vta/vtalib && git pull origin main)
git log -1 --oneline; git -C vta/vtalib log -1 --oneline
```

- **PC와 보드는 git으로만 맞춘다.** scp로 소스를 덮지 않는다. 비트스트림 파일만 scp로 보낸다.
- 보드에서 GitHub 접속이 막히면, PC에서 보드 저장소로 직접 push한다: `git push ssh://xilinx@192.168.20.2/home/xilinx/JO/npuprg/nestc2_clion/nest-compiler main:refs/remotes/origin/main`. 그다음 보드에서 `git merge --ff-only origin/main`을 한다.
- **보드에서 수정된 파일이 있어 pull이 안 되면:** `git checkout`으로 지우지 않는다. `git stash`로 치운 뒤 pull한다. 보통은 `collect_hw_bundles.py`가 다시 쓴 `hw/script/run_all.sh`다.

### 1.2 다른 사용자 확인 (보드를 같이 쓴다)

```bash
ps aux | grep -E "[r]un_prof|[r]un_all|[l]oad_bit|[m]ake |[c]c1plus|Bundle"    # 누가 무엇을 돌리는지
cat /tmp/evta_bitstream                                                      # 지금 올라간 비트스트림
grep CmaFree /proc/meminfo                                                   # 남은 CMA (128 MB 중)
```

- 다른 사람이 측정 중이면 빌드(`make`)나 실행으로 끼어들지 않는다. 빌드는 CPU를 써서 시간 측정을 흔든다.
- run 스크립트는 `/tmp/evta_run.lock`으로 동시에 하나만 돈다(`another EVTA run is active`가 나오면 기다린다).

### 1.3 sudo 쓰는 법

EVTA 실행은 `/dev/mem`을 쓰므로 sudo가 필요하다. 환경변수는 **sudo 안의 셸 문자열에** 넣는다.

```bash
sudo hw/script/run_prof.sh -s _systimed Resnet18Test                       # 변수가 없으면 이렇게
sudo /bin/sh -c "EVTA_CONV_BURST=0 hw/script/run_prof.sh -s _systimed Resnet18Test"   # 변수가 있으면
```

`sudo EVTA_X=0 cmd`처럼 쓰면 sudo가 변수를 지우거나 비밀번호를 다시 묻는다.

## 2. 컴파일러 빌드 (`Debug2_remote`)

### 2.1 처음 한 번: cmake

**지금 보드에는 이미 만들어져 있다. 새 빌드 디렉터리를 만들지 않는다.** 보드 메모리(3.9 GB, swap 없음)로는 처음부터 하는 빌드가 메모리 부족으로 실패한다. 아래 명령은 지금 `Debug2_remote`의 설정(`CMakeCache.txt`)을 옮긴 기록이다.

```bash
cd $D
export TVM_HOME=$R/tvm PYTHONPATH=$R/tvm/python TVM_LIBRARY_PATH=$D/tvm     # TVM은 cmake 전에 이 변수가 있어야 한다
cmake -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-8 -DCMAKE_CXX_COMPILER=clang++-8 \
  -DLLVM_DIR=/usr/lib/llvm-8/lib/cmake/llvm \
  -DNESTC_WITH_EVTA=ON -DNESTC_USE_VTASIM=ON -DNESTC_EVTA_RUN_ON_ZCU102=OFF \
  -DNESTC_EVTA_RUN_WITH_GENERIC_BUNDLE=ON -DNESTC_EVTA_BUNDLE_TEST=ON \
  -DNESTC_USE_PRECOMPILED_BUNDLE=OFF -DNESTC_USE_PRECOMPILED_EVTA_LIBRARY=OFF \
  -DVTA_RESNET18_WITH_SKIPQUANT0=OFF \
  -DGLOW_WITH_BUNDLES=ON -DGLOW_WITH_CPU=ON -DGLOW_BUILD_TESTS=ON \
  $R
```

| 옵션 | 지금 값 | 뜻 |
|---|---|---|
| `CMAKE_BUILD_TYPE` | Release | 디렉터리 이름(Debug2)과 달리 Release다 |
| `CMAKE_C(XX)_COMPILER` | clang-8 / clang++-8 | 보드의 `c++`는 g++ 7.3이라 직접 지정한다 |
| `LLVM_DIR` | `/usr/lib/llvm-8/lib/cmake/llvm` | 문서들의 `llvm-8.0` 경로는 이 보드에 없다 |
| `NESTC_WITH_EVTA` | ON | VTA / VTAInterpreter backend |
| `NESTC_USE_VTASIM` | ON | vtalib을 시뮬레이터로 빌드한다(그래서 model-compiler와 VTASaverTest가 빌드된다) |
| `NESTC_EVTA_RUN_ON_ZCU102` | OFF | ON이면 `lib`, `tools`를 건너뛰어 model-compiler가 빌드되지 않는다 |
| `NESTC_USE_PRECOMPILED_*` | OFF | ETRI S3 버킷이 없어져(`NoSuchBucket`) 다운로드가 모두 실패한다. 직접 생성한다 |
| `VTA_RESNET18_WITH_SKIPQUANT0` | OFF | Resnet18 conv0도 EVTA로(OFF), conv0을 float로 두기(ON) |
| `NESTC_WITH_TVM` | ON (기본) | Relay backend. TVM 환경변수가 필요하다 |

**cmake가 다시 도는 경우를 피한다.** 모델의 `CMakeLists.txt`를 고치거나, 날짜가 바뀐 뒤 처음 make하면 cmake가 다시 돈다. 그러면 `-DGLOW_BUILD_DATE`와 `NodeGen` / `InstrGen` 헤더가 바뀌어 Glow 거의 전체가 다시 빌드된다(`-j2`로 약 1시간). 그래서 모델별 옵션은 CMakeLists가 아니라 스크립트와 직접 호출로 준다(§3.4, §3.5).

### 2.2 평소: 바꾼 것만 다시 빌드

```bash
cd $D
make -j2 model-compiler            # VTA backend(VTA.cpp, VTASave.cpp), VTASchedules.h를 바꿨을 때
make -j2 image-classifier          # profile(calibration) 만들 때 쓰는 도구
make -j2 VTASaverTest              # 작은 시험 번들 생성기(§3.2)
```

- **반드시 `-j2`까지만 쓴다.** 더 높이면 메모리가 모자란다.
- 빌드 결과: `$D/glow/bin/model-compiler`, `$D/glow/bin/image-classifier`, `$D/tests/VTASaverTest`
- `VTASave.cpp` 한 파일만 바꾸면 수 분이다. 헤더(`include/glow/...`)를 바꾸면 길어진다.
- **로그 남기며 빌드하기:** `make -j2 model-compiler 2>&1 | tee ~/mc_build.log | grep -E "error|Built target"`

## 3. 번들 소스 생성

번들 하나는 다음 파일로 이루어진다. 생성 위치는 `$D/vta/bundles/<Dir>/`다.

| 파일 | 내용 |
|---|---|
| `<name>.cpp` | 생성된 C++. 맨 앞 주석에 쓰인 `-vta-*` 옵션 값이 찍힌다 |
| `<name>.h` | 함수 선언과 메모리 크기 |
| `<name>.weights.bin` | 상수(weight, bias, 표). 실행할 때 읽는다 |
| `VTARuntime.h` | vtalib 함수 선언 |
| (작은 시험) `*Input.bin`, `*Golden.bin`, `*MainEntry.h` | 입력과 정답 출력 |

### 3.1 가장 쉬운 방법: make target (`build_sim_bundles.sh`)

```bash
cd $D
./build_sim_bundles.sh vtaMxnetResnet18Bundle                  # 하나
./build_sim_bundles.sh vtaConvTestBundle vtaFCTestBundle       # 여러 개
./build_sim_bundles.sh                                         # 전부 (시험 번들 + Resnet18 / 50 / VGG16, 수십 분)
cat bundle_logs/summary.txt                                    # target마다 OK / FAIL, 걸린 시간
less bundle_logs/vtaMxnetResnet18Bundle.log                    # 실패 원인
```

- 이 스크립트는 target마다 `make -j2 <target>`을 부른다. make는 각 모델의 `vta/bundles/<Dir>/CMakeLists.txt`에 적힌 순서대로 profile → model-compiler → 시뮬레이터용 실행 파일을 만든다.
- TVM 환경변수는 스크립트가 넣는다.
- **이 방법으로는 `-vta-*` 옵션을 바꿀 수 없다.** 옵션은 CMakeLists에 고정돼 있고, 거기를 고치면 cmake가 다시 돈다. 옵션을 바꾸려면 §3.4의 직접 호출을 쓴다.

### 3.2 작은 시험 번들: `VTASaverTest`

ConvTest, ResConv1–10Test, FCTest, MaxPool 같은 작은 시험 번들은 model-compiler가 아니라 gtest 실행 파일 `VTASaverTest`가 만든다. 시험 하나가 그래프 하나를 코드로 만들고, 번들 소스와 입력 / 정답을 현재 디렉터리에 쓴다.

```bash
cd $D/tests && ./VTASaverTest --gtest_list_tests          # 만들 수 있는 시험 목록
cd $D/vta/bundles/ConvTest                                # 결과를 둘 디렉터리에서 실행한다
$D/tests/VTASaverTest --gtest_filter=VTASaverTest.convTest
ls    # vtaConvTestBundle.cpp  vtaConvTestBundle.weights.bin  vtaConvTestInput.bin  vtaConvTestGolden.bin  vtaConvTestMainEntry.h
```

| 시험 이름 (`--gtest_filter=VTASaverTest.<이름>`) | 번들 Dir |
|---|---|
| `convTest`, `convTest2` – `convTest5` | ConvTest, Conv2Test – Conv5Test |
| `ResConv1Test` – `ResConv10Test` | ResConv1Test – ResConv10Test |
| `resnetv10S1C0Test`, `Conv_inception_5b_5x5_1__1` | Resnetv10S1C0Test, Conv_inception_5b_5x5_1__1 |
| `fullyconnected`, `maxPoolTest`, `avgPoolTest`, `reluTest`, `softMaxTest`, `transposeTest` | FCTest, MaxPool, AvgPoolTest, ReluTest, SoftMax, Transpose |
| `elementadd`, `elementsub`, `elementdiv`, `splatMax`, `quantizeTest`, `dequantizeTest` | ElemAdd, ElemSub, ElemDiv, SplatMax, Quantize, Dequantize |
| `multievtaConvTest` | (multi-EVTA) |

- 보통은 §3.1의 make target(`vtaConvTestBundle` 등)이 이 명령을 대신 부른다.
- `VTASaverTest`도 model-compiler처럼 VTA backend 옵션(`-vta-*`)을 명령줄로 받는다.

### 3.3 모델 번들 1단계: profile(calibration) 만들기 (`image-classifier`)

양자화에 쓸 값의 범위를 float 실행으로 모아 YAML로 남긴다. 모델과 입력 전처리가 바뀌지 않으면 한 번만 만든다. 지금 있는 profile은 이렇다.

| 모델 | profile |
|---|---|
| Resnet18 | `$D/vta/bundles/Resnet18Test/mxnet_exported_resnet18_calib_1.yaml` |
| Resnet50 | `$D/vta/bundles/Resnet50Test/resnet50_calib_2.yaml` |
| VGG16 | `$D/vta/bundles/Vgg16Cifar10Test/vgg16_calib_1.yaml` |
| MobileNetV2 | `$D/vta/bundles/MobileNetV2Test/mobilenetv2_calib.yaml` |
| DeiT | `$D/vta/bundles/DeiTTest/deit_calib.yaml` |

```bash
# Resnet18 (vta/bundles/Resnet18Test/CMakeLists.txt와 같은 명령)
cd $D/vta/bundles/Resnet18Test
$D/glow/bin/image-classifier $IMG/cat_285.png \
  -m=$D/models/mxnet_exported_resnet18.onnx -model-input-name=data \
  -backend=VTAInterpreter -image-layout=NHWC -image-mode=0to255 -compute-softmax -topk=5 \
  -dump-profile=mxnet_exported_resnet18_calib_1.yaml \
  -quantization-schema=symmetric_with_power2_scale
```

| 옵션 | 뜻 |
|---|---|
| `<이미지 ...>` | calibration에 쓸 이미지(여러 장 가능) |
| `-m=<model.onnx>`, `-model-input-name=<이름>` | 모델과 입력 tensor 이름 |
| `-backend=VTAInterpreter` | EVTA 산술을 흉내 내는 interpreter로 실행 |
| `-image-layout=NHWC\|NCHW`, `-image-mode=0to1\|0to255\|neg1to1`, `-image-channel-order=RGB\|BGR`, `-mean=`, `-stddev=` | 입력 전처리. 모델이 학습된 방식과 맞춰야 한다 |
| `-minibatch=1` | 한 장씩. 여러 장을 한 batch로 하면 graph hash가 달라진다(DeiT) |
| `-dump-profile=<yaml>` | profile을 쓸 곳 |
| `-quantization-schema=symmetric_with_power2_scale` | EVTA는 2의 거듭제곱 scale만 쓴다 |
| `-do-not-lower-nodes-for-profiling=Convolution` | depthwise conv를 풀어 node가 수천 개가 되는 것을 막는다(MobileNetV2) |
| `-load-profile=<yaml>` (`-dump-profile` 없이) | 양자화된 결과를 Interpreter로 확인 |
| `-compute-softmax -topk=5` | 결과를 top-5로 출력 |

### 3.4 모델 번들 2단계: `model-compiler`로 번들 생성

```bash
# Resnet18: 빈 작업 디렉터리에서 (model-compiler는 ./VTASkipQuantizeNodes.txt를 읽는다)
O=~/work/r18 && mkdir -p $O && cd $O
export TVM_HOME=$R/tvm PYTHONPATH=$R/tvm/python TVM_LIBRARY_PATH=$D/tvm
$D/glow/bin/model-compiler -g \
  -model=$D/models/mxnet_exported_resnet18.onnx -model-input=data,float,[1,224,224,3] \
  -backend=VTA -emit-bundle=$O -bundle-api=dynamic \
  -load-profile=$D/vta/bundles/Resnet18Test/mxnet_exported_resnet18_calib_1.yaml \
  -quantization-schema=symmetric_with_power2_scale \
  > gen.log 2>&1; echo rc=$?
head -20 $O/mxnet_exported_resnet18.cpp      # 어떤 옵션으로 만들어졌는지
```

**일반 옵션:**

| 옵션 | 뜻 |
|---|---|
| `-g` | ONNX 등 모델 파일 import 모드 |
| `-model=<파일>` | 모델 (ONNX, Caffe2 protobuf) |
| `-model-input=<이름>,<type>,[<shape>]` | 입력 tensor 이름, 형식, 모양. 예 `data,float,[1,224,224,3]` |
| `-backend=VTA` | EVTA 번들. `CPU`면 LLVM CPU 번들 |
| `-emit-bundle=<dir>` | 번들을 쓸 디렉터리 |
| `-bundle-api=dynamic` | 메모리를 실행할 때 할당하는 API(우리 main들이 쓰는 형태) |
| `-network-name=<이름>` | 파일과 함수 이름의 앞부분(기본은 모델 파일 이름) |
| `-load-profile=<yaml>` | §3.3의 profile로 int8 양자화 |
| `-quantization-schema=symmetric_with_power2_scale` | profile을 만들 때와 같게 |
| `-keep-original-precision-for-nodes=SoftMax` | 이 종류의 node는 float로 둔다(Resnet50) |
| `-enable-channelwise-depthwise` | depthwise conv만 채널별 scale(MobileNetV2) |
| `-index-multi-evta=<mask>` | 번들을 EVTA 하나에 고정(1, 2, 4, 8 = EVTA 0–3). multi4 copy를 만들 때 |
| `-dump-graph-DAG=<file.dot>` | 최적화된 그래프를 dot으로 |

**VTA backend 옵션 (`-vta-*`):** 모두 켜고 끄는 스위치다(`=false` / `=0` / `=true` / `=1`). 기본값과 뜻의 전체 표는 [evta_user_manual.md](evta_user_manual.md) §2.2에 있다. 자주 쓰는 것은 이렇다.

| 옵션 | 기본 | 언제 |
|---|---|---|
| `-vta-round-shift` | 꺼짐 | MobileNetV2, DeiT는 켠다(스크립트가 넣음) |
| `-vta-keep-layout`, `-vta-fuse-residual`, `-vta-evta-maxpool`, `-vta-evta-fc`, `-vta-fuse-preprocess`, `-vta-conv0-pack`, `-vta-fuse-pool` | 켜짐 | 비교하려고 끌 때만 `=false` |
| `-vta-evta-depthwise`, `-vta-pad-channels` | 켜짐 | MobileNetV2 |
| `-vta-matmul-conv`, `-vta-evta-matmul`, `-vta-lut-elementwise`, `-vta-fuse-layernorm`, `-vta-matmul-heads`, `-vta-attn-keep-layout`, `-vta-cpu-approx`, `-vta-fold-matmul-bias` | 켜짐 | DeiT(Transformer) |

**모델별 명령** (모두 위와 같은 형태):

| 모델 | 모델 파일 | `-model-input` | 더하는 옵션 |
|---|---|---|---|
| Resnet18 | `$D/models/mxnet_exported_resnet18.onnx` | `data,float,[1,224,224,3]` | – |
| Resnet50 | `$D/vta/bundles/Resnet50Test/resnet50.onnx` | `gpu_0/data_0,float,[1,3,224,224]` | `-keep-original-precision-for-nodes=SoftMax` |
| VGG16 CIFAR-10 | `$D/vta/bundles/Vgg16Cifar10Test/vgg16_cifar10.onnx` | `input,float,[1,3,32,32]` | – |
| MobileNetV2 | `$D/vta/bundles/MobileNetV2Test/mobilenetv2_qat_f32.onnx` | `input,float,[1,3,224,224]` | `-network-name=mobilenetv2 -enable-channelwise-depthwise -vta-round-shift` (스크립트 §3.5) |
| DeiT-tiny | `$D/vta/bundles/DeiTTest/deit_fold.onnx` | `pixel_values,float,[1,3,224,224]` | `-network-name=deit -vta-round-shift` (스크립트 §3.5) |

네 모델(Resnet18 / 50, VGG16, MobileNetV2)을 한 번에 다시 만들어 기존 번들과 비교하는 스크립트가 `~/JO/deit_tmp/regen2.sh`에 있다(작업 디렉터리 `~/JO/deit_tmp/regen2/`). 결과는 `r18: header 20 lines, rest of cpp same, weights same` 식으로 나온다.

### 3.5 스크립트로 만드는 모델 (MobileNetV2, DeiT, multi4 copy)

```bash
$X/gen_mobilenetv2_bundle.sh [추가 옵션]       # → $D/vta/bundles/MobileNetV2Test/  (gen.log, prof.log)
$X/gen_deit_bundle.sh [추가 옵션]              # → $D/vta/bundles/DeiTTest/          (gen.log, prof.log)
$X/gen_multi4_bundles.sh                       # multi4 copy (Resnet18 / 50, VGG16)
```

- 스크립트가 profile(없을 때만)과 model-compiler를 부르고, 필요한 옵션을 넣는다. `[추가 옵션]`은 model-compiler에 그대로 넘어간다(예: `-vta-evta-depthwise=0`).
- **결과가 `Debug2_remote/vta/bundles/<Dir>`에 바로 쓰인다.** 비교용으로 옵션을 바꿔 만들었으면, 끝난 뒤 옵션 없이 다시 만들어 둔다.
- **DeiT의 graph hash 문제:** DeiT는 Glow graph hash가 실행마다 달라진다. 그래서 스크립트가 profile 사본의 hash를 고쳐 가며 4번까지 다시 돈다. 4번 다 실패하면(`rc=134`) 스크립트를 다시 돌린다.
  ```bash
  for k in 1 2 3; do $X/gen_deit_bundle.sh | tail -1 | tee /tmp/rc; grep -q "rc=0" /tmp/rc && break; done
  ```
- DeiT의 `deit_fold.onnx`는 PC에서 `vtabundles_xcmpl/fold_onnx.py model.onnx deit_fold.onnx 1,3,224,224`로 만든다(보드의 onnx 1.11에는 `onnx.reference`가 없다).

## 4. 보드용 실행 파일 빌드 (`vtabundles_xcmpl/`)

### 4.1 번들 소스를 모으기: `collect_hw_bundles.py`

```bash
cd $X
python3 collect_hw_bundles.py Resnet18Test                   # Debug2_remote/vta/bundles/Resnet18Test → hw/Resnet18Test/
python3 collect_hw_bundles.py Resnet18Test DeiTTest          # 여러 개
python3 collect_hw_bundles.py                                # 전부
python3 collect_hw_bundles.py --set multievtax4              # multi4 copy → hw_multievtax4/
tail -3 hw/result/collect_report.txt                         # "Resnet18Test  OK  3 updated  vtaMxnetResnet18Bundle"
```

- 바뀐 파일만 복사하고, `hw/script/run_all.sh`(실행 목록)를 다시 쓴다.
- `hw/` 아래에서 git이 관리하는 것은 `script/`뿐이다.

### 4.2 build 디렉터리 (처음 한 번 cmake)

빌드 디렉터리는 git에 없다. 보드에는 이미 있다. 새로 만들 때만 아래 명령을 쓴다.

```bash
cd $X
mkdir build          && cd build          && cmake .. && cd ..                                                        # 기본, golden
mkdir build_timed    && cd build_timed    && cmake -DBUNDLE_TIMED=ON -DBUNDLE_EXE_SUFFIX=_timed .. && cd ..
mkdir build_prof     && cd build_prof     && cmake -DNESTC_EVTA_PROFILE=ON -DBUNDLE_EXE_SUFFIX=_prof .. && cd ..
mkdir build_sys      && cd build_sys      && cmake -DBUNDLE_EXE_SUFFIX=_sys -DVTALIB=$R/vtalib-zcu102-systolic .. && cd ..
mkdir build_systimed && cd build_systimed && cmake -DBUNDLE_TIMED=ON -DBUNDLE_EXE_SUFFIX=_systimed -DVTALIB=$R/vtalib-zcu102-systolic .. && cd ..
mkdir build_sysprof  && cd build_sysprof  && cmake -DNESTC_EVTA_PROFILE=ON -DBUNDLE_EXE_SUFFIX=_sysprof -DVTALIB=$R/vtalib-zcu102-systolic .. && cd ..
# multi4 (예: systolic, 문장별 시간)
mkdir build_multievtax4_systimed && cd build_multievtax4_systimed && \
  cmake -DBUNDLE_SET=multievtax4 -DNESTC_EVTA_MULTI=ON -DNESTC_EVTA_MAX_XFER=4194304 -DBUNDLE_TIMED=ON \
        -DBUNDLE_EXE_SUFFIX=_systimed -DVTALIB=$R/vtalib-zcu102-systolic .. && cd ..
```

| cmake 옵션 | 뜻 |
|---|---|
| `-DVTALIB=<dir>` | 링크할 vtalib. 기본 `vta/vtalib`(HLS GEMM 비트스트림용), systolic 비트스트림은 `vtalib-zcu102-systolic` |
| `-DBUNDLE_TIMED=ON` | 번들 함수의 문장마다 시간을 재는 사본으로 컴파일(`timed_bundle.py`) |
| `-DNESTC_EVTA_PROFILE=ON` | EVTA run마다 시간 기록(vtalib의 `VTA_SYNC` 이벤트) |
| `-DBUNDLE_EXE_SUFFIX=<접미사>` | 실행 파일 이름 끝. 같은 `hw/<Dir>/`에 여러 종류가 함께 있게 한다 |
| `-DBUNDLE_SET=single\|multievtax2\|multievtax4` | 버퍼 / queue 크기와 `hw[_<set>]/` 디렉터리 |
| `-DNESTC_EVTA_MULTI=ON` | multi-EVTA 경로 |

| 실행 파일 접미사 | 빌드 디렉터리 | vtalib | 비트스트림 | 용도 |
|---|---|---|---|---|
| 없음 | `build/` | `vta/vtalib` | HLS 380 MHz (`BIN`) | golden(`run_all.sh`) |
| `_timed` / `_prof` | `build_timed/` / `build_prof/` | `vta/vtalib` | HLS | 문장별 시간 / EVTA run별 시간 |
| `_sys` / `_systimed` / `_sysprof` | `build_sys/` / `build_systimed/` / `build_sysprof/` | `vtalib-zcu102-systolic` | systolic 400 MHz `_rq` (`SYS_BIN`) | 위와 같고, **성능 측정은 `_systimed` / `_sysprof`로** |

### 4.3 빌드: `make`

```bash
cd $X/build_systimed && make -j2 vtaMxnetResnet18Bundle      # 번들 하나 → hw/Resnet18Test/vtaMxnetResnet18Bundle_systimed
cd $X/build_systimed && make -j2 hw_bundles                   # 전부 (수십 분)
cd $X/build_systimed && make -j2 vtaDeiTBundle 2>&1 | grep -E "error|Built target"   # 출력 줄이기
```

- **번들 Dir이 새로 생겼으면 그 build 디렉터리에서 `cmake .`를 먼저 한다.** 모르는 target은 make가 cmake를 다시 돌리기 전에 멈춘다.
- **vtalib을 바꿨으면** 같은 make가 vtalib도 다시 빌드한다. 헤더(`cmd_queue.h` 등)만 바꾼 경우 의존성이 잡히지 않을 수 있다. 결과가 이상하면 `make -j2 VTABundle vta_runtime` 뒤에 다시 빌드한다.

**Dir과 target 이름** (`hw/script/run_all.sh`의 `run` 줄):

| Dir | make target = 실행 파일 이름 | 입력 |
|---|---|---|
| Resnet18Test | `vtaMxnetResnet18Bundle` | 이미지 `$IMG/cat_285.png` |
| Resnet18PartitionTest | `vtaMxnetResnet18PartitionBundle` | 이미지 |
| Resnet50Test | `vtaCaffe2Resnet50Bundle` | 이미지 |
| Vgg16Cifar10Test | `vtaVGG16Cifar10Bundle` | `$R/examples/bundles/vgg16_cifar10/cifar10/test/airplane/0001.png` |
| MobileNetV2Test | `vtaMobileNetV2Bundle` | 이미지 |
| DeiTTest | `vtaDeiTBundle` | 이미지 |
| ConvTest, Conv2–5Test, ResConv1–10Test, FCTest, MaxPool, AvgPoolTest, ElemAdd, ... | `vta<이름>Bundle` (예 `vtaConvTestBundle`, `vtaResConv10TestBundle`, `vtaFCTestBundle`) | `<target 앞부분>Input.bin`, 정답 `...Golden.bin` |
| (multi4) MultiEVTAResnet18Test / 50 / Vgg16Cifar10Test | `multivta<...>Bundle`(4 copy), `singlevta<...>Bundle`(copy 하나) | 이미지 |
| (multi4) MultiEVTAConv4Test, MultiEVTAResConv10Test | `multi4conv4Bundle`, `multi4resconv10Bundle` | `<EVTA mask> <반복 수> [conv]` (예 `15 100`) |

## 5. 실행

### 5.1 golden 확인: `run_all.sh` (HLS 비트스트림, 접미사 없는 실행 파일)

```bash
cd $X
sudo hw/script/run_all.sh                          # 전부. 결과 hw/result/results.txt
sudo hw/script/run_all.sh Resnet18Test ConvTest    # 일부 (Dir 이름은 대소문자까지 정확히)
sudo hw/script/run_all.sh -t 4 Resnet18Test        # OMP_NUM_THREADS=4
sudo /bin/sh -c "BIN=vtahw_zcu102_1x16_i8w8a32_16_16_19_18_380mhz.bin hw/script/run_all.sh Resnet18Test"   # 다른 비트스트림
sudo hw_multievtax4/script/run_all.sh              # multi4
hw/script/run_all.sh --help                        # 옵션, BIN / SYS_BIN (sudo 없이)
```

출력 예:
```
== run_all.sh hw 2026-11-10 14:35:47
  bitstream : /lib/firmware/vtahw_zcu102_1x16_i8w8a32_16_16_19_18_380mhz.bin ...
================ ConvTest (vtaConvTestBundle)
ConvTest                       vtaConvTestBundle                  PASS
Resnet18Test                   vtaMxnetResnet18Bundle             RAN (no golden; see Resnet18Test/run.log)
```

- **PASS / MISMATCH:** 출력 `output.bin`을 `*Golden.bin`과 byte 비교한 결과다.
- **RAN:** 정답 파일이 없는 모델(분류)이다. 분류 결과는 로그(`hw/<Dir>/run.log`)에서 본다.
- **잘못 입력했을 때:** 없는 Dir이면 실행 전에 멈추고 비슷한 이름을 알려준다(`DeitTest: not a run line ... (did you mean: DeiTTest)`). `-s` 같은 측정 옵션을 주면 `run_prof.sh`를 쓰라고 알려준다.

### 5.2 측정: `run_prof.sh` (기본 `_prof`, systolic은 `-s _sys*`)

```bash
cd $X
sudo hw/script/run_prof.sh -s _systimed -r Resnet18Test             # systolic 400 MHz, 문장별 시간, 25장, chrt
sudo hw/script/run_prof.sh -s _sysprof -r Resnet18Test              # EVTA 호출별 시간과 GOPS
sudo hw/script/run_prof.sh -s _systimed -r -t 4 DeiTTest            # 4코어
sudo hw/script/run_prof.sh -s _systimed -n 5 Resnet18Test Resnet50Test   # 5장씩, 여러 모델
sudo hw/script/run_prof.sh -s _systimed all                         # golden이 있는 것 포함 전부
sudo /bin/sh -c "hw_multievtax4/script/run_prof.sh -s _sysprof -n 25 -l -r MultiEVTAResnet18Test"
hw/script/run_prof.sh --help
```

| 옵션 | 뜻 |
|---|---|
| `-s _prof` (기본) / `_timed` / `_sysprof` / `_systimed` / `_sys` | 어느 build 디렉터리의 실행 파일인지. `_sys*`면 `SYS_BIN`(systolic `_rq`)을 올린다 |
| `-n <runs>` | 추론 수(기본 25). multi4는 copy당 |
| `-r` | `chrt -f 10`(실시간 우선순위). **시간을 잴 때는 붙인다** |
| `-t <n>` | `OMP_NUM_THREADS`(기본 1) |
| `-l` | multi-EVTA lockstep(EVTA 호출마다 copy들이 같이 시작) |
| 환경 `BIN=<파일>` | 비트스트림 바꾸기 |

옵션은 Dir 이름 앞뒤 어디에 써도 된다(11-10부터).

### 5.3 직접 실행 (스크립트 없이)

```bash
cd $X/hw
sudo script/load_bit.sh vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_400mhz_rq.bin   # 비트스트림 올리기 (실행마다)
cd Resnet18Test
sudo /bin/sh -c "OMP_NUM_THREADS=1 ./vtaMxnetResnet18Bundle_systimed $IMG/cat_285.png"
sudo /bin/sh -c "BUNDLE_RUNS=5 EVTA_DUMP_CONV=/tmp/dump_a ./vtaMxnetResnet18Bundle_systimed $IMG/cat_285.png"
```

- **비트스트림과 실행 파일의 짝:** `_sys*` 실행 파일은 `_sa_` 비트스트림, 나머지는 HLS 비트스트림과 쓴다. 짝이 다르면 결과가 틀리거나 멈춘다.
- **multi-EVTA 실행 파일을 EVTA가 적은 비트스트림에서 돌리면** 커널 Oops가 난다(vtalib이 실행 전에 검사해 막는다).
- **멈춘 실행 뒤에는 비트스트림을 다시 올린다.** run 스크립트는 매번 올린다.
- 분류 모델의 main(`multi_threads_main.cpp`: MobileNetV2, DeiT)은 `MULTI_RUNS`(기본 25) × 4번 돈다. `BUNDLE_RUNS`(timed / prof 실행 파일)가 있으면 그 수만큼 돈다.

### 5.4 단계 켜고 끄기, 검증용 변수

| 종류 | 예 | 다시 빌드 |
|---|---|---|
| 번들 생성 옵션 `-vta-*` | `-vta-fuse-pool=false` | 번들 다시 생성(§3.4) + 실행 파일 빌드 |
| 실행 환경변수 `EVTA_*` | `sudo /bin/sh -c "EVTA_CONV_BURST=0 hw/script/run_prof.sh ..."` | 없음 |
| 비트스트림 | `sudo /bin/sh -c "BIN=..._bias.bin hw/script/run_prof.sh ..."` | 없음 |

- 자세한 절차: [resnet18_optimization.md](resnet18_optimization.md) §9
- 모든 환경변수: [evta_user_manual.md](evta_user_manual.md) §5
- 자주 쓰는 검증용 환경변수:

| 변수 | 하는 일 |
|---|---|
| `EVTA_DUMP_CONV=<dir>` | EVTA 출력을 `conv_<n>.bin`으로 모두 저장(변경 전후 byte 비교) |
| `EVTA_RES_CHECK=1` | fused 경로를 CPU 경로와 실행 중에 비교 |
| `EVTA_MATMUL_CHECK=1` | DeiT attention matmul을 CPU 정수 결과와 비교 |
| `EVTA_REPLAY_LOG=2` | run마다 GEMM / ALU 반복, DRAM byte, µs (`EVTA_RUN ...` 줄) |
| `EVTA_REPLAY_DUMP=<dir>` | 기록된 명령과 uop를 텍스트로 |
| `EVTA_SCHED='<H>,<W>,<C>,<KN>,<K>,<stride>:<vt>,<th>,<tw>'`, `EVTA_SCHED_LOG=1` | schedule 바꿔 끼우기, conv 모양 출력 (systolic vtalib) |

## 6. 로그와 결과 보기

### 6.1 어디에 무엇이 남나

| 단계 | 파일 | 내용 |
|---|---|---|
| 컴파일러 빌드 | (직접 `tee`한 파일) | make 출력 |
| 번들 생성(make target) | `$D/bundle_logs/<target>.log`, `$D/bundle_logs/summary.txt` | target마다 make 출력, OK / FAIL |
| 번들 생성(스크립트) | `$D/vta/bundles/<Dir>/gen.log`, `prof.log` | model-compiler / image-classifier 출력 |
| 번들 내용 | `hw/<Dir>/<name>.cpp` 맨 앞 20줄 | 쓰인 `-vta-*` 옵션 |
| collect | `hw/result/collect_report.txt` | Dir마다 OK, 갱신 파일 수 |
| golden 실행 | `hw/<Dir>/run.log`, `hw/<Dir>/run.log.rc` | 실행 출력, 종료 코드 |
| golden 요약 | `hw/result/results.txt` | Dir마다 PASS / MISMATCH / RAN / FAIL |
| 측정 실행 | `hw/<Dir>/<접미사>.log` (예 `systimed.log`, `sysprof.log`) | 실행 출력 + 표 |
| 측정 요약 | `hw/result/<접미사>_summary.txt` | Dir마다 한 줄(시간 중앙값, GOPS) |
| `_timed` / `_systimed` | `hw/<Dir>/timing.csv`, `<접미사>.log.runs` | 문장마다, run마다 시간(`run,idx,start_ms,kind,node,call,ms,ops,gops`) |
| `_prof` / `_sysprof` | `hw/<Dir>/<접미사>.log.sum` | EVTA 호출마다 run 1 / 2 / 3 / 마지막의 ms와 GOPS |
| 멈춤 | `/tmp/evta_hang_<pid>_*.insn`, 실행 로그의 상태 dump | `EVTA_RUN_TIMEOUT_MS`(기본 5 s)를 넘은 run, exit 5 |
| 커널 | `dmesg` | SError, CMA 할당 실패 |

### 6.2 자주 쓰는 보기 명령

```bash
cd $X/hw
cat result/results.txt                                         # golden 결과 전체
grep -E "MISMATCH|FAIL|TIMEOUT" result/results.txt             # 실패만
tail -5 Resnet18Test/run.log                                   # 분류 결과 (top-5, Result: 281 ...)
cat result/systimed_summary.txt                                # 측정 요약 (Dir마다 한 줄)
grep "whole run median" Resnet18Test/systimed.log              # 1장 시간 중앙값과 GOPS (요약 줄)

# 문장별 표 (run 1, 2, 3, 마지막 열): 처음 20줄
sed -n '/==== timing per statement/,+22p' Resnet18Test/systimed.log

# 함수(call)별 합계, 마지막 run 기준, 큰 것부터: 시간이 어디에 쓰이나
L=$(tail -1 Resnet18Test/timing.csv | cut -d, -f1)
awk -F, -v r=$L '$1==r && $6!="" {t[$6]+=$7} END {for (k in t) printf "%9.3f %s\n", t[k], k}' Resnet18Test/timing.csv | sort -rn | head -15
#   (call 열이 빈 행은 run 전체 합계라 뺀다)

# EVTA 호출별 시간과 GOPS (_sysprof)
sed -n '1,30p' Resnet18Test/sysprof.log.sum
tail -3 Resnet18Test/sysprof.log.sum                           # 합계, run별 시간, 중앙값
# 요약을 다시 만들기 (run 수는 BUNDLE_RUNS로 준다: stream conv의 run을 합칠 때 쓴다)
(cd $X && BUNDLE_RUNS=25 python3 prof_summary.py hw/Resnet18Test hw/Resnet18Test/sysprof.log | tail -3)

# 두 dump 비교 (변경 전후)
diff -rq /tmp/dump_a /tmp/dump_b && echo same
for f in /tmp/dump_a/*.bin; do cmp -s $f /tmp/dump_b/$(basename $f) || echo "differ: $f"; done

# 실행 중 무엇이 도는지, CMA, 커널 메시지
ps aux | grep -E "[r]un_prof|Bundle"; grep CmaFree /proc/meminfo; dmesg | tail -20
```

### 6.3 로그에서 보는 줄

| 찾는 것 | 줄 |
|---|---|
| 어떤 비트스트림, 어떤 설정으로 돌았나 | 맨 앞 `== run_prof.sh ...`, `bitstream :`, `features :`, `env :`, `OMP_NUM_THREADS:` |
| 분류 결과 | `Result: 281 Confidence: 0.75...`, `top1: 281 ...` |
| 1장 시간 | `runs 2-25: whole run median ... ms` |
| EVTA 바쁜 시간 | `_sysprof`의 `median runs 2-25: ... ms ... GOPS` |
| 짝이 안 맞는 비트스트림 | `WARNING : -s _sys... on a bitstream without _sa_` |
| 실행 파일 없음 | `<Dir>/<exe> not built (cd ../build_... && make ...)` |

## 7. 시뮬레이터에서 돌리기 (보드의 EVTA 없이)

`Debug2_remote`의 번들 실행 파일은 EVTA 시뮬레이터에 링크돼 있다. EVTA 비트스트림 없이, sudo 없이 돈다. 매우 느리다(작은 시험 번들에 쓴다).

```bash
cd $D/vta/bundles/ConvTest && ./vtaConvTestBundle vtaConvTestInput.bin && cmp output.bin vtaConvTestGolden.bin && echo PASS
cd $D && ctest -L NESTC -N                         # 등록된 시험 목록
cd $D && ctest -R vtaConvTestBundle --output-on-failure
```

## 8. 처음부터 한 번에: Resnet18 예

```bash
# 0) 준비
R=~/JO/npuprg/nestc2_clion/nest-compiler; D=$R/Debug2_remote; X=$R/vtabundles_xcmpl; IMG=$R/glow/tests/images/imagenet
cd $R && git pull && (cd vta/vtalib && git pull origin main)
ps aux | grep -E "[r]un_prof|[r]un_all|[m]ake "               # 아무도 안 돌릴 때

# 1) 컴파일러 (backend를 바꿨을 때만)
cd $D && make -j2 model-compiler 2>&1 | grep -E "error|Built target model-compiler"

# 2) 번들 소스
cd $D && ./build_sim_bundles.sh vtaMxnetResnet18Bundle && tail -1 bundle_logs/summary.txt

# 3) 보드용으로 모으기
cd $X && python3 collect_hw_bundles.py Resnet18Test && tail -1 hw/result/collect_report.txt

# 4) 실행 파일
(cd $X/build && make -j2 vtaMxnetResnet18Bundle)               # golden용
(cd $X/build_systimed && make -j2 vtaMxnetResnet18Bundle)      # 측정용
(cd $X/build_sysprof && make -j2 vtaMxnetResnet18Bundle)

# 5) 실행
cd $X
sudo hw/script/run_all.sh Resnet18Test                         # 동작 확인 (HLS 380 MHz)
sudo hw/script/run_prof.sh -s _systimed -r Resnet18Test        # 1장 시간 (systolic 400 MHz)
sudo hw/script/run_prof.sh -s _sysprof -r Resnet18Test         # EVTA 시간

# 6) 결과
tail -3 hw/Resnet18Test/run.log
cat hw/result/systimed_summary.txt hw/result/sysprof_summary.txt
```

**vtalib(runtime)만 바꿨을 때:** 1–3을 건너뛰고 4–6만 한다. 결과가 같은지는 `EVTA_DUMP_CONV`로 변경 전후 dump를 비교한다.

**backend(VTASave.cpp)를 바꿨을 때:** 다른 모델의 번들이 바뀌지 않았는지 `~/JO/deit_tmp/regen2.sh`로 확인한다.

## 9. 문제가 생기면

| 증상 | 원인과 할 일 |
|---|---|
| `<Dir>: not a run line ... (did you mean: ...)` | Dir 이름 오타(대소문자까지) |
| `... not built (cd ../build_... && make ...)` | 그 build 디렉터리에서 make. 새 Dir이면 `cmake .` 먼저 |
| `another EVTA run is active (/tmp/evta_run.lock)` | 다른 실행이 끝날 때까지 기다린다 |
| `run with sudo` | sudo로 실행 |
| 환경변수가 안 먹음 (`env : no EVTA_* ...`) | `sudo /bin/sh -c "VAR=... cmd"` 형태로 |
| 비트스트림 load 실패, ENOMEM | 이전 실행이 비정상 종료해 CMA가 남아 있다. 스크립트가 `xlnk_reset` 후 120 s까지 다시 시도한다. 다른 사람이 EVTA를 쓰는 중에는 손으로 `xlnk_reset`하지 않는다 |
| exit 5, `/tmp/evta_hang_*` | run이 5 s를 넘었다(멈춤). 비트스트림 짝(`_sys*` ↔ `_sa_`)부터 확인 |
| 결과가 틀림 / 확률이 다름 | 번들 옵션 확인(`head -20 <bundle>.cpp`; DeiT / MobileNetV2는 `-vta-round-shift`), 비트스트림 짝, vtalib 헤더만 바꿨다면 vtalib 다시 빌드 |
| model-compiler `rc=134`, "Graph hash mismatch" | DeiT: 스크립트를 다시 돌린다(§3.5) |
| make하자 Glow 전체가 다시 빌드됨 | cmake가 다시 돌았다(CMakeLists 변경, 날짜 변경). 기다리거나, CMakeLists를 고치지 않는 방법(직접 호출)을 쓴다 |
| 보드 ssh가 가끔 끊김 | `ping 192.168.20.2`의 ttl이 128이면 같은 IP / MAC의 다른 기기(Windows PC)가 응답한 것이다 |
| `git pull`이 보드에서 멈춤 | GitHub 22번 포트 문제. §1.1의 PC에서 보드로 push하는 방법 |
