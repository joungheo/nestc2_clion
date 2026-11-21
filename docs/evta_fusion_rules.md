# EVTA 융합 찾기 규칙 (VTASave, 2026-11-19)

VTASave(번들 생성)가 "EVTA 호출 하나에 같이 넣을 연산"을 찾는 규칙을 한곳에 모은 정리다.
VGG16의 2x2 pool이 빠져 있던 일([evta_conv_pool_guide.md](evta_conv_pool_guide.md))을 계기로 정리했다.
그 일은 finder마다 생산자 조건을 따로 적다 보니 "첫 conv만" 같은 조건이 남아서 생겼다.

- 코드: `lib/Backends/VTA/VTASave.cpp` (`EvtaGemm`, `evtaGemmOf`, `evtaTails`, `canTail`, `VTASaveContext::tailFree`, `IrSeq`, `reportFusions`)
- 점검 로그: `VTA_FUSION_LOG=1`로 `model-compiler`를 돌리면 stderr에 나온다(§5).
- 코드 전 / 후와 그림 5개로 본 설명: [evta_fusion_rules_guide.md](evta_fusion_rules_guide.md).

## 1. 규칙 한 줄

**융합 = 생산자 + 꼬리.**

- **생산자:** EVTA GEMM tile로 도는 연산. IR 명령의 이름이 아니라 그 명령이 실제로 무슨 EVTA 호출이 되는지로 정한다.
- **꼬리:** 생산자의 호출이 STORE 전에 acc 안에서 하는 후처리. epilogue 프로그램, pool, 출력 LUT가 있다.

finder는 다음 세 가지만 묻는다.

1. 이 명령은 어떤 생산자인가? (`evtaGemmOf`)
2. 그 생산자의 호출이 이 꼬리를 할 수 있나? (`evtaTails`)
3. 아직 꼬리가 없나? (`tailFree`: 꼬리는 하나뿐이다)

꼬리마다 다른 조건(scale, 모양, tile)은 각 finder에 남는다.

## 2. 생산자 (`evtaGemmOf`)

| 생산자 | 무엇이 되나 | IR 명령 | 판정 |
|---|---|---|---|
| `EvtaGemm::Conv` | `convolution_wo_tr[_res\|_pool]` (`generateVTAConvolutionCall`) | `ConvolutionInst` | `getConvPath == VTA` |
| `EvtaGemm::FirstConv` | conv0 pack / zero-fill 첫 conv (`saveFirstBNNVTAConvolutionInst`) | `ConvolutionInst` | `getConvPath == FirstBNN` |
| Q K^T heads | `matmul_evta_qk` / `matmul_evta_n` | `MatMulInst` 묶음 | `findMatMulGroups` (`MatMulGroup::qk`) |
| A × V heads | `matmul_evta_av` | `MatMulInst` 묶음 | `MatMulGroup::av` |

- Linear(MatMul + 상수 가중치)는 `VTA.cpp` `-vta-matmul-conv`가 1x1 conv로 바꾸므로 `Conv`다. DeiT의 fc1 / fc2 / qkv / out-proj가 여기에 든다.
- EVTA fc(`fullyconnected_evta_packed`), depthwise(`dw_convolution_evta`), avgpool은 생산자로 세지 않는다. 그 호출에는 꼬리를 받는 자리가 없다.

## 3. 꼬리와 받을 수 있는 생산자 (`evtaTails`)

| 꼬리 | 내용 | finder | Conv | FirstConv | Q K^T |
|---|---|---|---|---|---|
| `TAIL_CHAIN` | packed residual add(2의 거듭제곱 scale) + relu들, 출력은 다른 버퍼여도 됨 | `findResidualFusions` | ✔ | | |
| `TAIL_MUL_ADD` | packed 텐서와의 정수 add(MUL, ADD d, RQ) | `findMulResidualFusions` (기본 off) | ✔ | | |
| `TAIL_CONST_ADD` | 상수와의 정수 add, 출력 = conv 자기 버퍼 | `findEmbedFusions` | ✔ | ✔ | |
| `TAIL_POOL3` | max pool 3x3 / 2 / pad 1 | `findPoolFusions` | ✔ | ✔ (conv0 pack) | |
| `TAIL_POOL2` | max pool 2x2 / 2 / pad 0 | `findPoolFusions` | ✔ | | |
| `TAIL_OUT_LUT` | elementwise 구간의 256칸 표 | `findOutLutConvs`, `findOutLutMatMuls` | ✔ | | ✔ |

빈칸인 이유:
- **FirstConv에 `TAIL_CHAIN` / `TAIL_OUT_LUT`이 없다.** 첫 conv의 번들 코드는 자기 출력 버퍼에만 쓴다(다른 버퍼로 내보내는 chain이 없다). 출력 LUT 호출도 없다.
- **FirstConv에 `TAIL_POOL2`가 없다.** conv0 pack 경로에서 2x2를 시험한 적이 없다.
- **Q K^T는 표만 받는다.** heads를 묶은 호출에서 table-only epilogue만 batch로 돈다(§8.22).

새 생산자나 꼬리를 더할 때는 vtalib 호출과 번들 생성이 그것을 받는지 확인한 뒤 이 표(`evtaTails`)에 한 칸을 켠다.

## 4. 모든 꼬리에 공통인 조건

| 조건 | 왜 | 코드 |
|---|---|---|
| 꼬리는 하나 | 한 tile의 STORE 전 자리가 하나다. 나중 finder는 앞 finder가 준 꼬리를 본다 | `tailFree` = `!getFusion && !getPoolFusion && !outLutConv` |
| 생산자 출력을 꼬리만 읽음 | 융합하면 생산자 출력이 DRAM에 없다 | `users[D]`(`IrSeq`)를 셈 |
| 꼬리의 출력 버퍼가 생산자 전에 있음 | 생산자 호출이 거기에 쓴다. 뒤에서 할당되면 앞으로 당긴다(hoist) | `allocAt` |
| 같은 바이트 | CPU 경로와 비트까지 같아야 한다. scale 비가 2의 거듭제곱이거나 정수 add 형태(`elemAddIntParams`)여야 한다 | finder마다 |

finder의 순서는 `VTASave.cpp`의 호출 순서다: Residual → EvtaMaxPools → InputPreprocess → **Pool** → LutRegions → **OutLutConvs** →
… → **OutLutMatMuls** → PackedRows → **MulResidual** → **Embed**. 앞에서 꼬리를 받은 생산자는 뒤 finder가 건너뛴다.

## 5. 점검: `VTA_FUSION_LOG`

**단계 (2026-11-21):** 값으로 정한다. 변수가 없거나 비어 있으면 **요약만**(기본, 늘 켜짐), `0`이면 아무것도 안 찍고, 그 밖의 값(`1` 등)이면 명령마다 자세히 찍는다.
요약은 생산자 수 / 꼬리 있음 / 빠진 꼬리, CPU로 간 GEMM 수와 그 이유별 개수다. 같은 요약이 번들 `.cpp` 맨 위(옵션 주석 바로 뒤)에 `// EVTA fusion summary` 주석으로 **값과 상관없이** 늘 들어간다(번들이 환경 변수에 따라 달라지지 않게).
`build_sim_bundles.sh`, `gen_deit_bundle.sh`, `gen_mobilenetv2_bundle.sh`는 끝에 요약을 화면에 찍는다.

```
// EVTA fusion summary (VTA_FUSION_LOG=1 for every instruction; docs/evta_fusion_rules.md):
//   73 EVTA producers, 13 with a tail, 24 read by a tail op not fused
//     not fused: 24 x an add: -vta-fuse-elemadd-mul is off (elemadd_evta is faster)
//   6 GEMMs (conv / fc / matmul) on the CPU
//     on the CPU: 3 x matmul 2 x 198 x 64: 2 rows: no H x W with W >= 7 (largest W 1)
//     on the CPU: 3 x matmul 2 x 64 x 198: 2 rows: no H x W with W >= 7 (largest W 1)
```

아래는 `VTA_FUSION_LOG=1`의 자세한 줄이다.
모든 생산자에 대해 받은 꼬리를 적는다. 꼬리가 없으면 그 출력을 처음 읽는 명령을 적는다.
그 명령이 꼬리가 될 수 있는 연산(max / avg pool, add, relu, clip, 표 구간)인데 융합되지 않았으면 `<- a tail op not fused`가 붙는다.
이유를 아는 경우에는 그 이유도 함께 적는다.
다른 생산자의 epilogue에 들어간 add가 이 출력을 residual로 읽는 경우는 빠진 것으로 세지 않는다.

```bash
# 시뮬레이터 빌드(Debug2_remote)에서
VTA_FUSION_LOG=1 ../vtabundles_xcmpl/build_sim_bundles.sh -f vtaVGG16Cifar10Bundle;   # -f: 번들이 최신이어도 model-compiler를 다시 돌림;  grep VTA_FUSION_LOG bundle_logs/vtaVGG16Cifar10Bundle.log
VTA_FUSION_LOG=1 ../vtabundles_xcmpl/gen_deit_bundle.sh;        grep VTA_FUSION_LOG vta/bundles/DeiTTest/gen.log
```

2026-11-19 결과:

| 모델 | 생산자 | 꼬리 있음 | 빠진 것 | 빠진 것의 이유 |
|---|---|---|---|---|
| Resnet18 | 20 | 9 | 0 | |
| Resnet50 | 53 | 17 | 0 | |
| VGG16 | 13 | 4 | 1 | 마지막 `Conv_28` → `MaxPool_30`(2x2 → 1x1)은 CPU pool이다. 0.04 ms라 하지 않기로 했다 |
| MobileNetV2 | 35 | 10 | 1 | 마지막 conv → global avgpool은 `avgpool_evta`로 따로 돈다 |
| DeiT | 73 | 13 | 24 | out-proj / fc2 + residual add. `-vta-fuse-elemadd-mul`(기본 off)이면 융합되지만 `elemadd_evta`가 더 빠르다(§8.18) |

DeiT의 Q K^T 출력 표 12개는 생산자가 matmul 묶음이라 이 표에 세지 않았다(`findOutLutMatMuls`, 12개 모두 융합됨).

2026-11-20부터 번들을 다 낸 뒤 **EVTA에서 돌지 않는 GEMM**(conv / channelwise conv / fc / matmul)도 이유와 함께 적는다
(`VTA_FUSION_LOG cpu <명령>: <이유>`, 마지막 줄 `N GEMMs on the CPU`). 이유는 EVTA 경로를 떠나는 자리의 판정 코드가 낸다
(`getConvPath(I, &why)`, `saveEvtaFcPacked(…, &why)`, `matmul_evta` / `dw_convolution_evta` 조건; `VTASaveContext::noteCpuGemm`).
지금은 DeiT 마지막 블록의 2행 attention 곱 6개뿐이다(`2 rows: no H x W with W >= 7`); 다른 네 모델은 0.

## 6. 이번에 바꾼 것

- `EvtaGemm` / `evtaGemmOf` / `evtaTails` / `canTail`: 생산자 판정과 꼬리 가능 여부를 한곳에 모았다.
  `findResidualFusions`, `findPoolFusions`, `findOutLutConvs`, `findEmbedFusions`, `findMulResidualFusions`가 이것을 쓴다.
  `findAttnKeepLayout`의 "꼬리 없는 conv" 조건도 이것을 쓴다.
- `VTASaveContext::tailFree`: "꼬리가 이미 있나"를 finder마다 다른 조합으로 보던 것을 하나로 했다.
  예를 들어 `findPoolFusions`는 `outLutConv`를 봤지만 그 finder가 더 뒤에 돌아 늘 비어 있었고, 첫 conv에는 아무 확인도 없었다.
- `IrSeq`: 세 finder에 복사돼 있던 IR 순회(명령 순서, 할당 위치, 읽는 명령 수, 다음 명령)를 하나로 했다.
- `reportFusions` (`VTA_FUSION_LOG=1`): 위의 점검 로그.
- 결과: 다섯 모델(Resnet18 / 50, VGG16, MobileNetV2, DeiT)의 번들 `.cpp`가 바꾸기 전과 바이트까지 같다. 규칙은 그대로이고 표현만 모았다.
  보드 실행 파일도 바뀌지 않는다.

## 7. 용어

- **acc:** EVTA 안의 int32 누적 버퍼. GEMM 결과가 STORE 전까지 여기 있다.
- **epilogue 프로그램 (`VTAEpilogue`):** ADD / RQ / CLIP / SHL / LUT / MUL 단계. tile마다 STORE 전에 돈다([evta_epilogue_guide.md](evta_epilogue_guide.md)).
- **finder:** `find…` 함수. IR을 보고 융합할 곳을 `VTASaveContext`에 적는다. 번들 코드를 낼 때 그 기록을 보고 합친 호출을 내고, 흡수된 명령은 내지 않는다(`getFusedAway`).
- **hoist:** 꼬리의 출력 버퍼 할당을 생산자 앞으로 옮기는 것.
- **packed:** EVTA layout(N{C/16}HW16). `findPackedActivations` / `findPackedRows`가 정한다.
