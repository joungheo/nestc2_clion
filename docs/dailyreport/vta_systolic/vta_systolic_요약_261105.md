# 2026-11-05 EVTA systolic 요약: Resnet50 추론 147.9 → 82.2 ms, VGG16 15.5 → 7.3 ms(schedule·최소 tile·fc·conv0 stream), vtalib 버그 3개 수정, multi4 Resnet50/VGG16 set(weight 공유)

- 기간: 2026-11-05 07:00 ~ 11-06 07:00 (KST)
- 이날은 세션 하나 [PC]만 있었다. PC `main`에서 코드와 문서를 고치고 커밋·push했다. 보드 실험(빌드, dump 비교, 시간 측정)은 이 PC에서 ssh로 돌렸고, 보드는 pull로 맞췄다.
- 따로 적지 않았으면 측정 조건은 single EVTA, systolic rq bitstream, OMP 1, 25회 중앙값이다.

## 시간순

**13:34 [PC] "time_waterfall.svg 최근 결과 포함하게 수정해줘. 나머지 그림도…" / 13:36 "evta_time_now.svg도 수정해야겠다."**
- 그림 14 `time_waterfall`: bias reset 줄을 "예상"에서 실측 27.30 → 26.59 ms(§7.16)로 바꾸고, requantize 줄 26.61 → 25.49 ms(§7.18)를 넣었다. 요약도 새 값으로 고쳤다(전체 148.9 → 25.49 ms, 5.8배 / EVTA 26.42 → 23.05 ms / multi4 400 MHz는 EVTA마다 23.44~23.56 ms, 610.4 GOPS).
- 그림 13 `alu_ii`, 그림 2 `change_map`(ISA에 `bias_reset`·`rq_*` 추가, vtalib에 `VTARequant`), `evta_overview.md`도 고쳤다. 나머지 그림은 고칠 곳이 없었다. 커밋 `93bc14fb`.
- 그림 15 `evta_time_now`는 "11-02 측정 → ALU II = 1 → bias reset → requantize" 네 줄로 바꾸고, 줄마다 추정값과 측정값을 함께 적었다.

  | 단계 | 추정 | 측정 |
  |---|---|---|
  | ALU II = 1 | 24.83 ms | 24.89 ms |
  | bias reset | 23.92 ms | 24.15 ms |
  | requantize | 22.29 ms | 23.05 ms |

  측정값이 추정보다 0.4~0.8 ms 크다. stage3/4에서는 ALU가 LOAD / STORE와 겹쳐 있어서, ALU를 없애도 그만큼 줄지 않기 때문이다.

**14:01 [PC] "runtime / driver … HAL 경계 … DRM accel driver … 다른 모델 … ALU 연산 범위 … 이런 것들을 해보고 싶어. 순서도 잘 잡고 … 먼저 Resnet50, VGG16 부터 돌려서 성능을 보자."**
- 같은 실행 파일로 bitstream 네 개를 바꿔 가며 EVTA 시간을 쟀다(single EVTA, 25회 중앙값). 오늘 하드웨어에서 바꾼 것들이 다른 모델에도 효과가 있는지 보려는 실험이다.

  | | 기본 400 MHz | ALU II = 1 | bias reset | requantize | 줄어든 비율 |
  |---|---|---|---|---|---|
  | Resnet18 | 26.43 ms (137 GOPS) | 24.89 | 24.15 | **23.05 ms (157 GOPS)** | −12.8 % |
  | Resnet50 | 125.56 ms (65 GOPS) | 120.27 | 118.58 | **113.36 ms (72 GOPS)** | −9.7 % |
  | VGG16 (CIFAR-10) | 15.82 ms (40 GOPS) | 15.60 | 15.46 | **15.23 ms (41 GOPS)** | −3.8 % |

- 추론 한 장은 Resnet50 160.3 → 147.9 ms, VGG16 16.1 → 15.5 ms였다. Resnet50은 그중 33.4 ms가 EVTA 밖에서 걸렸다.
- 병목 세 가지:
  - Resnet50 1×1 conv 36개가 27~69 GOPS로, EVTA 113 ms 중 약 87 ms를 차지한다(3×3 conv는 178~192 GOPS).
  - res2_0 residual 하나만 CPU로 빠졌다(elemadd 17.9 + relu 9.3 = 27 ms).
  - VGG16 뒤쪽 2×2 출력 conv가 1.92 ms(9.8 GOPS)씩 걸린다. 이때는 weight LOAD가 원인이라고 짐작했는데, 15:02에 틀린 것으로 정정했다.
- 순서 제안: 1) 모델 병목(①res2_0, ②1×1 schedule, ③VGG LOAD, ④MobileNet) → 2) ISA 한 번에 개정 → 3) 완료 interrupt + 링 버퍼 → 4) HAL → UIO → DRM accel → 5) 하드웨어 loop FSM. 단계마다 다른 오픈소스를 먼저 본다. 문서 §7.19, 커밋 `ca028f36`.

**14:26 [PC] "좋아. 1번 모델 병목부터 해보자." / 14:32 "Resnet18 성능에는 상관 없겠지?" / 14:35 "bundle.cpp 생성할 때 … vtalib를 참조해서 해?"**
- Resnet18에 대한 답: 바뀌지 않을 것으로 봤다(아직 측정 전). 새로 허용한 것은 relu 출력 scale이 합의 scale과 다른 경우뿐이고, vtalib 변경도 q = 0이면 전과 같다. Resnet18 bundle에는 CPU `relu` 호출이 없다.
- 14:35의 vtalib 참조 질문에 대한 답은 기록에 없다.
- ① res2_0 합치기 결과: `EVTA_RES_CHECK`로 합친 호출 16개와 conv0+pool을 비교해 0 differ였고, 분류는 281 / 0.581379로 같았다.

  | Resnet50 | 전 | 후 |
  |---|---|---|
  | 추론 한 장 (`_systimed`) | 147.9 ms | **122.5 ms** (−25.4 ms, −17 %) |
  | EVTA 시간 (`_sysprof`) | 113.36 ms | 115.27 ms |

  EVTA 시간이 1.9 ms 는 것은 CPU에서 하던 elemadd·relu 27 ms가 EVTA로 옮겨 왔기 때문이다. res2_0은 relu 뒤에 왼쪽 shift가 필요해서 ALU 명령으로 처리하고, virtual thread는 1개다.

**15:02 [PC] 1-①② 완료: Resnet50 147.9 → 85.7 ms, VGG16 15.5 → 13.6 ms**
- ① res2_0만 relu 출력 scale이 합의 scale의 1/2이어서 fusion 조건에서 빠져 있었다. 이 경우도 합치게 했다. CPU `relu`가 scale을 키울 때 127에서 자르지 않고 음수로 넘어가던 버그도 고쳤다.
- ② schedule 표에 Resnet18 모양만 있어서, 다른 모델의 conv는 기본값(vt 1, tile 14×14)으로 돌고 있었다. TVM VTA의 AutoTVM처럼 보드에서 찾기로 했다. vtalib에 `EVTA_SCHED`를 넣어 모양마다 후보를 재고, 가장 빠른 것을 `VTASchedules.h`에 넣었다(14:50에 27개 커밋, 15:02 보고 24개). 대부분 vt 2와 큰 tile이 이겼고, 1×1 conv는 30~60 % 빨라졌다.

  | | EVTA 시간 | 추론 한 장 |
  |---|---|---|
  | Resnet50 | 115.3 → 78.1 ms (71 → 105 GOPS) | 122.5 → **85.7 ms** |
  | VGG16 | 15.2 → 13.3 ms | 15.5 → **13.6 ms** |

- 기본 schedule과 새 schedule의 레이어 dump가 같았다(Resnet50 106개, VGG16 51개). Resnet18은 bundle과 시간 모두 그대로였다.
- 중요: **판단을 정정했다.**
  - 처음 dump 비교는 `EVTA_SCHED`가 없는 실행 파일로 해서 무효였다.
  - 다시 비교하니 출력이 7×7보다 작은 conv(VGG16 4×4, 2×2)에서 vt 2가 틀렸다. 이 둘은 표에서 뺐다.
  - VGG16이 느린 원인도 weight LOAD가 아니었다. vtalib이 tile을 최소 7×7로 늘리기 때문이다(2×2 출력이면 계산이 12배).

**16:53 [PC] "1번 부터 하자. vtalib가 tile을 7x7로 늘리는 것과 virtual thread 2 버그 원인 찾기지?"**
- 중요: **vt 2 버그는 vtalib 주소 계산 버그였다.**
  - 출력 tile이 7×7보다 작으면 더미 행·열을 붙여 계산하는데, 이 저장 경로에서 STORE와 residual LOAD가 출력 채널 블록 번호를 잘못 계산했다. 틀린 식은 `(filterIdx + fIdx)·vt + thread`, 맞는 식은 `(filterIdx·vt + thread)·nFilterinLoop + fIdx`다.
  - vt 1에서는 두 식이 같은 값을 내서 드러나지 않았다.
  - acc 주소에도 W 자리에 H를 쓴 실수가 있었다(지금은 H = W = 7이라 결과에는 영향이 없었다).
  - 두 vtalib을 함께 고쳤다(`e40d5cca`).
- 최소 7×7은 HLS GEMM의 acc hazard를 피하려고 둔 것이다(`DEPENDENCE inter false`). Gemmini는 같은 경우에 stall하고, 우리 `sa_rmw.v`는 forwarding한다. 그래서 systolic vtalib은 `_sa_` bitstream에서만 최소 tile을 1로 한다(`EVTA_MIN_TILE`). HLS vtalib은 7 그대로다. VGG16 4×4·2×2의 vt 2 schedule은 표에 다시 넣었다.
- 검증: VGG16은 기본, vt 2, 최소 1, 최소 1에 vt 2의 네 경우에서 dump 51개가 모두 같았다. Resnet50 159개, Resnet18 60개도 같았다. 분류 결과:

  | 모델 | 결과 |
  |---|---|
  | VGG16 | 0 / 0.4537 |
  | Resnet50 | 281 / 0.5814 |
  | Resnet18 | 281 / 0.6555 |

- 시간(25회 중앙값). 최소 tile을 1로 바꾼 뒤 schedule을 다시 찾지는 않았고, 작은 conv는 vt 2와 출력 전체 크기 tile 조합 하나만 쟀다.

  | | EVTA | 추론 한 장 |
  |---|---|---|
  | VGG16 | 13.30 → **7.03 ms** (47 → 89 GOPS) | 13.59 → **7.33 ms** |
  | Resnet50 | 78.0 ms (그대로) | 85.7 ms (7×7보다 작은 tile이 없음) |
  | Resnet18 | 23.05 → 22.99 ms | 25.53 → 25.42 ms |

- 커밋은 020d041(vtalib), `e40d5cca`, `327ddd3a`, `145a780c`다. survey에 썼던 "VGG16은 weight LOAD가 병목"은 정정했다.

**22:27 [PC] "1번 fc 부터 하자."**
- Resnet50 fc(2048 → 1000, chunk당 4블록)와 VGG16 512 → 10(1블록)은 "chunk당 출력 블록 ≥ 8" 조건에 걸려 CPU에서 돌고 있었다. 이 조건도 HLS acc hazard 회피용이었다. 참고로 TVM VTA도 batch 1의 마지막 dense는 CPU에 둔다(`stop_pack`).
- 바꾼 것:
  - VTASave는 블록 수와 상관없이 EVTA fc를 낸다.
  - systolic vtalib은 1블록 chunk도 EVTA로 보낸다.
  - GEMM으로 할 수 없는 경우는 두 vtalib 모두 packed weight로 CPU에서 계산한다(`fc_packed_cpu`).
- 결과: fc 3개 모두 `EVTA_RES_CHECK` 0 differ, 분류는 그대로였다. Resnet50 fc는 CPU 2.69 ms → EVTA 0.44 ms, 추론은 85.7 → **83.4 ms**가 됐다. VGG16은 7.33 → 7.28 ms.
- 중요: **bias가 다른 thread의 결과를 덮어쓰던 vtalib 버그**를 찾았다.
  - 증상: HLS bitstream과 simulator에서 VGG16 confidence가 0.4247로, systolic(0.4537)과 달랐다.
  - 원인: thread마다 acc 절반(1024 entry)을 쓰는데, 필터 수를 정할 때 bias 자리를 빼지 않았다. tile × 필터 수가 절반을 꽉 채우면 thread 0의 bias LOAD가 thread 1의 첫 결과 행을 덮어쓴다. VGG16 conv0(vt 2, 16×32), Conv_5·Conv_7(16×16)이 이 경우였다.
  - systolic rq bitstream은 bias reset이 그 행을 다시 써서 결과가 맞았다.
  - 수정 `f071fe00`. 이후 simulator·HLS·systolic dump가 모두 같고, 셋 다 0.453709다. HLS·systolic 3회 dump(VGG16 51, Resnet50 159, Resnet18 60)도 같았다.
- 정정: 중간에 "4×4 conv에 별개 원인이 있다"고 했던 것은 비교 목록을 잘못 읽은 것이었다. 원인은 이 버그 하나였다.
- 오늘 누적(single rq, 추론 한 장):

  | 모델 | 처음 | 지금 |
  |---|---|---|
  | Resnet50 | 147.9 ms | **83.4 ms** (−44 %) |
  | VGG16 | 15.5 ms | **7.3 ms** (−53 %) |
  | Resnet18 | 25.5 ms | 25.4 ms |

- 커밋은 `949e5a4f`, `f071fe00`(vtalib 2c093fc, ea1b9c8), 문서 `aff32009`다.

**23:49 [PC] "conv0 입력 복사를 먼저 하고 MobileNet으로 넘어가자." / 11-06 00:05 "Pynq driver의 run을 Runstart와 Runwait로 바꿨는데 Resnet18에는 영향 없어?"**
- 원인 조사: 입력 복사의 한계는 DRAM이 아니라 A53 한 코어의 NEON 연산이었다(quantize 0.47 ms, packing 0.44 ms. 같은 크기 memset은 0.06 ms, float 읽기는 0.13 ms). 코드를 다듬어도 1.25 → 1.14 ms 정도라서, 줄이는 대신 EVTA 계산 뒤에 숨기기로 했다.
- 방식:
  - pynq driver `Run`을 `RunStart` / `RunWait`로 나눴다. 다른 호출은 둘을 이어 부르므로 동작이 같다.
  - conv0을 타일 행마다 run 하나로 나누고, run이 도는 동안 CPU가 다음 행의 입력을 채우고 flush한다.
  - 코드 생성은 `preprocess_conv0pack_stream`을 낸다. `EVTA_INPUT_STREAM=0`이면 예전처럼 한 번에 채운다.
- Resnet18에 대한 답: driver 분리는 동작을 바꾸지 않는다. Resnet18에 영향을 주는 것은 conv0 stream이고, 의도대로 Resnet18에도 적용된다.
- 진행 중 문제: systolic `VTABundle.cpp`가 vta/vtalib 헤더를 써서 빌드가 실패했다. `runtime.h`에 선언을 추가해 고쳤다(`93799e88`). 실패한 동안 측정 스크립트가 옛 실행 파일로 돌았는데, 보드 프로세스 kill이 권한 검사에 막혀서 끝날 때까지 기다린 뒤 다시 돌렸다.
- 검증: systolic(stream 켬/끔)과 HLS의 layer dump가 전날 기준과 같았다(Resnet50 159, Resnet18 60, VGG16 51). 분류도 그대로였다.
- 결과(25회 중앙값, stream 끔 → 켬):

  | 모델 | 입력 복사 + conv0 | 추론 |
  |---|---|---|
  | Resnet50 | 1.42 + 3.31 → 3.56 ms | 83.41 → **82.21 ms** |
  | Resnet18 | 1.65 + 3.36 → 3.66 ms | 25.43 → **24.08 ms** |
  | VGG16 | 0.05 + 0.11 → 0.14 ms | 7.29 → 7.27 ms |

  conv0이 0.25~0.30 ms 는 것은 대부분 첫 run 전에 채워야 하는 첫 행 묶음(115행 중 19행) 때문이고, run을 7개로 나눈 비용은 거의 없다. 커밋은 `5eee77e1`, `93799e88`(vtalib b7422fa, 800723e), 문서 `59f8e3d4`(§7.19 ⑥).

**11-06 00:12 [PC] "Mobilenet으로 넘어가기 전에 multi4에 conv0 입력 복사를 먼저 고치자. multi4에도 Resnet18 테스트도 하고 Resnet50, vgg16도 같은 방식으로 만들자."**
- **weight 공유(`VTAConstBufferAlloc`):** copy마다 weight를 따로 두면 Resnet50 4개가 102 MB라 CMA 128 MB에 들어가지 않는다(single Resnet50 실행만으로 약 96 MB를 쓴다). 그래서 내용이 같은 상수 buffer는 process 안에서 하나로 공유하게 했다. Resnet50 multi4 weight가 25.6 MB, Resnet18은 47 → 11.7 MB로 줄었다. `EVTA_CONST_SHARE=0`이면 예전처럼 copy마다 따로 둔다.
- **새 multi4 bundle:** `MultiEVTAResnet50Test`, `MultiEVTAVgg16Cifar10Test`를 만들었다. main은 공통 `multi_threads_main.cpp` 하나이고, 모델별 차이는 `multi_model.h`에 둔다. copy는 `vtabundles_xcmpl/gen_multi4_bundles.sh`가 model-compiler를 직접 불러 만든다. NEST-C CMake에는 일부러 넣지 않았다(Debug2_remote에서 cmake가 다시 돌면 Glow 전체를 다시 빌드한다).
- 진행 중 문제:
  - multi4 build dir에서 cmake를 다시 돌리지 않아 새 target 빌드가 실패했다. `cmake .`을 넣은 `/tmp/st4.sh`로 다시 돌렸다.
  - 보드의 `run_all.sh` 두 개가 수정된 상태였던 것은 다른 사용자 작업이 아니었다. collect가 손으로 넣은 주석을 지운 것이라, 주석을 `collect_hw_bundles.py`로 옮겼다(`a07b3524`). 보드는 `git stash` → pull → `git stash pop`으로 맞췄다.
- 중간 측정(00:37, 아래 두 버그를 고치기 전, 마지막 run의 copy별 추론 시간). 입력 값은 시간에 영향이 없어서 유효하다고 봤다.

  | 모델 | stream 끔 | stream 켬 | 차이 |
  |---|---|---|---|
  | Resnet18 | 25.9~26.0 ms | **24.6~24.7 ms** | 약 −1.3 ms |
  | Resnet50 | 86.7~87.5 ms | **85.5~86.0 ms** | 약 −1.3 ms |
  | VGG16 (stream 켬만 측정) | — | 8.1~8.5 ms | — |

  이때 Resnet50 copy를 multi4 bitstream에서 혼자 돌리면 83.13 ms였다(single bitstream 82.21 ms). Resnet50 4 copy 실행 중에도 CMA가 85 MB 남았다.
- 중요: **검증 중 고친 버그 2개**

  | # | 증상 | 원인 → 수정 |
  |---|---|---|
  | 1 | Resnet50·VGG16 multi4의 dump가 single과 모두 다름(분류는 맞음) | 새 main의 PNG 읽기가 single main과 달랐다(Resnet50은 RGB를 뒤집지 않음, scale은 double로 계산). `MODEL_PNG_REVERSE`를 넣고 scale을 double로 바꿈(`0ccbb35a`) |
  | 2 | 1을 고친 뒤에도 VGG16 systolic multi4만 dump 51개가 모두 다름(0.447279). HLS multi4는 같음 | bias reset이 있으면 `conv_kernel`이 bias 자리를 남기지 않아, 마지막 thread의 bias가 물리 acc 2048 뒤에 LOAD됐다(single은 acc가 4096이라 우연히 괜찮았다). 넘칠 때만 bias 자리를 남기게 고침(`543bbf91`). single 계산은 그대로 |

- 최종 결과(00:46, systolic rq, copy 4개 동시, OMP 1, copy별 25회 중앙값, stream 끔 → 켬). nF가 바뀌는 층이 있어서 앞의 multi4 수치를 이 값으로 대신한다.

  | 모델 | 4개 동시 (copy별) | copy 0 혼자 |
  |---|---|---|
  | Resnet18 | 25.86~25.97 → **24.58~24.67 ms** | 25.52 → 24.15 ms |
  | Resnet50 | 86.88~87.49 → **85.68~86.26 ms** | 84.32 → 83.09 ms |
  | VGG16 | 8.18~8.51 → 8.19~8.53 ms (입력이 작아 효과 없음) | 7.38 → 7.37 ms |

- 검증: 세 모델 모두 copy 0의 dump가 single과 같았다(Resnet50·VGG16은 HLS multi4도 확인). copy 4개 × 25회 분류가 모두 single과 같았고, single bitstream의 dump와 시간은 바뀌지 않았다. 커밋은 `56687864` → `0ccbb35a` → `543bbf91` → `f111635f`(문서)다.
- 남은 문제 두 가지: copy 0 혼자도 multi4 bitstream에서 더 느리다(83.09 vs 82.18 ms). 4개를 동시에 돌리면 copy마다 Resnet50 2.6~3.1 ms, VGG16 0.8~1.1 ms가 늘고, copy 1·2가 더 느리다.

**11-06 01:04 [PC] "multi4 두 남은 문제를 먼저 보자. 그 다음 mobileNet으로 넘어가보자."**
- 첫 EVTA 쌍 측정은 `/bin/sh`(dash)가 bash 문법의 `bininfo.sh`를 source하다 끝나서 수치가 없었다. 그 줄을 빼고 다시 돌렸다.
- 문제 1(혼자 돌려도 0.9 ms 느림): schedule 때문이 아니다. 두 bitstream에서 호출마다 하는 연산이 같다. 0.9 ms는 conv마다 1~3 %씩 퍼져 있고, load가 대부분인 stride-2 1×1 conv가 가장 많이 늘었다. multi4는 Y219 행 주변의 타이밍을 맞추려고 HP 포트 앞에 register slice를 넣었는데, 늘어난 load 지연이 이것과 맞는다. 코드가 아니라 bitstream 문제로 판단했다.
- 문제 2: `MULTI_MASK`로 고른 EVTA만 돌려 Resnet50 copy별 추론 시간을 쟀다(혼자, 쌍 6개, 4개 모두).

  | EVTAs | per copy | 혼자 대비 |
  |---|---|---|
  | 하나씩 | 83.1–83.3 ms | — |
  | 0+1, 0+2, 0+3, 1+3, 2+3 | 84.0–84.2 ms | +0.9 ms |
  | 1+2 | 84.5 ms | +1.25 ms |
  | 4개 모두 | 0과 3: 85.7–85.8, 1과 2: 86.2 ms | +2.5 / +2.95 ms |

  - 어느 쌍이든 0.9 ms가 늘어서 DDR controller 공유로 본다. 1+2만 0.35 ms가 더 늘어난 것은 HP1과 HP2가 DDR 포트 하나를 공유한다는 것과 맞는다. 다만 이 대응(UG1085)은 기억에 따른 것이고, 검색으로는 확인하지 못했다(미확인).
  - 대역폭 문제는 아니다. Resnet50 한 장이 86.6 MB를 옮기므로, EVTA 4개를 합쳐도 평균 약 4 GB/s로 DDR 약 17 GB/s보다 훨씬 낮다. 경합 때문에 load가 많은 부분의 대기가 길어지는 것이다. 4 copy의 처리량은 1개의 3.87배다.
  - 고치려면 하드웨어를 바꿔야 한다(register slice 제거, 또는 EVTA 1이나 2를 HPC 포트로). 소프트웨어로는 EVTA를 일부만 쓸 때 1과 2를 같이 고르지 않는 것뿐이다. 문서 §7.19 ⑧(`c5a79891`), `MULTI_MASK`(`3558de23`).
- MobileNet 진행:
  - ETRI 모델 다운로드는 없어져서, ONNX model zoo의 MobileNetV2(opset 7, MXNet export)를 보드 `Debug2_remote/models`에 받았다.
  - 구성은 1×1 conv 36개, depthwise 3×3 17개(그중 stride 2가 4개), 3채널 stride-2 첫 conv, BN, Relu, residual Add, GlobalAveragePool이다.
  - FP32 Interpreter 결과는 281 / 0.892다. calibration과 VTA bundle 생성은 보드에서 진행 중이었다.

**11-06 06:34 [PC] MobileNet CPU depthwise 경로 확인**
- CPU depthwise는 scalar loop `cpunonvtaconvolution`이다. 출력 채널마다 입력을 C stride로 읽고 출력마다 `std::round`를 불러서, 가장 큰 병목이 될 것으로 본다(추정). quantization 변형 결과를 먼저 본다.

## 결정이 남은 것 (11-06 07:00 기준)
- 이날 결정된 것:
  - 14:01 제안 순서(모델 병목 → ISA 개정 → interrupt/링 버퍼 → HAL/DRM → loop FSM)의 1단계부터 시작했다. ①res2_0, ②schedule, 최소 tile, fc, conv0 stream은 끝났다.
  - multi4의 두 남은 문제는 MobileNet 전에 보기로 했다. 원인은 HP register slice와 DDR 공유(HP1·HP2)로 판단했다.
- MobileNetV2 작업은 진행 중이다. quantize 결과와, EVTA와 CPU에 각각 어떤 연산이 가는지 확인하는 일이 남았다. depthwise를 CPU NEON으로 할지, 2단계 ISA에서 다룰지는 정해지지 않았다.
- multi4 성능을 하드웨어로 고칠지(register slice 제거, EVTA 1/2를 HPC 포트로)는 결정되지 않았다. HP1·HP2의 DDR 포트 공유도 미확인이다.
- 최소 tile을 1로 바꾼 뒤 schedule을 다시 찾지 않았다. 작은 conv는 조합 하나만 쟀다.
- Resnet50에 남은 CPU 몫은 avgpool 0.9 ms다. ALU에 나눗셈이 없어 2단계 ISA 항목이다.
