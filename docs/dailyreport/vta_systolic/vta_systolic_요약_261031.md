# 2026-10-31 EVTA systolic 요약: Resnet18 CPU 작업을 없애고 EVTA로 옮겨 single OMP 1 90.93 → 46.90 ms, multi4 100회 40.5 → 76.1 추론/s, bitstream 불일치 SError 차단

- 기간: 2026-10-31 07:00 ~ 11-01 07:00 (KST)
- 세션은 하나다. [PC]는 PC의 `main`에서 코드를 고치고 push한 뒤, ssh로 보드에서 pull·빌드·측정한 작업이다. 이날의 측정은 모두 보드 값이다.

## 시간순

**12:06 [PC] "보드 비교는 docs/에 문서로 남겨줘. runtime 작업 이어서 ①(층 사이 EVTA layout 유지)부터 ... detranspose - transpose를 안 한다는 건 software 적으로 매우 어려운 것 같은데."**
- ①을 넣었다. 층 사이 transpose가 conv0 입력 하나만 남고 모두 없어졌고, 결과는 이전과 같다(Result 281, Confidence 0.655530, multi4 copy 4개 모두 281).
- 어렵지 않은 이유: EVTA layout은 블록 크기 16으로 고정된 N{C/16}HW16이다. 채널이 64에서 128로 바뀌면 16채널 블록 수만 4개에서 8개로 는다. elemadd, relu는 그대로 돌고, pool은 "배치가 N·C/16이고 채널이 16인 NHWC"로 인자만 바꿔 넘긴다.
- `VTASave.cpp` `findPackedActivations`: 쓰는 명령이 EVTA conv, elemadd, relu, maxpool, avgpool뿐인 activation만 EVTA layout으로 둔다. 그래프 입출력, tensor view, 그 밖의 연산이 닿는 것은 NHWC 그대로다. 옵션은 `-vta-keep-layout`(기본 켬).
- 보드 비교 문서는 `docs/evta_board_compare.md`, ① 결과는 `docs/evta_runtime_compare.md` §7.1이다(`53c4c68c`, `6142b804`, `9660bfc`). multi2는 빌드만 하고 실행하지 않았다.
- ① 전후 비교: systolic 400 MHz, 25회 반복, `chrt` 사용.

  | 경우 | 이전 | 이후 |
  |---|---|---|
  | single, OMP 4, 1회 | 73.6 ms | **56.4 ms** (−23 %) |
  | multi4 copy 하나, OMP 1 | 153~160 ms | **94.9 ms** (−39 %) |
  | multi4 4 copy 100회 | 3909 ms | **2793 ms** (초당 35.8회 추론, −29 %) |
  | single exe 100회, OMP 1 | 14959 ms | 9227 ms (−38 %) |

- 예상한 53 ms보다 3 ms 남은 이유로 두 가지를 본다: `VTABufferAlloc` 2.2 ms, 채널 16개짜리가 된 maxpool 안쪽 루프(4.1 ms, 이전 단독 값이 없어 비교 못 함).

**14:05 [PC] "내가 돌리니 56.578ms 이네. ... mutli4 copy 하나만 쓴 건 왜 시간이 2배 가까이 걸리지? ... single exe 100회, OMP 1은 왜 56ms의 100배가 아니고 거의 200배야?"**
- 사용자가 직접 돌린 Resnet18Test `_systimed` 출력이다(golden 없음, exit=0).

  | run | EVTA calls 4649.9 Mops | CPU conv/fc 1.0 Mops | whole run |
  |---|---|---|---|
  | #1 | 35.537 ms, 130.85 GOPS | 1.173 ms, 0.87 GOPS | 62.605 ms, 74.29 GOPS |
  | #2 | 32.114 ms, 144.80 GOPS | 1.155 ms, 0.89 GOPS | 56.939 ms, 81.68 GOPS |
  | #3 | 32.076 ms, 144.97 GOPS | 1.154 ms, 0.89 GOPS | 56.588 ms, 82.19 GOPS |
  | #25 | 32.081 ms, 144.94 GOPS | 1.156 ms, 0.89 GOPS | 56.578 ms, 82.20 GOPS |

- `VTASave.cpp`와 model-compiler 모두 바뀌었고, transpose 제거가 기본이다. 예전 동작은 `-vta-keep-layout=false`로 볼 수 있다.
- 중요: 시간 차이는 대부분 OpenMP thread 수에서 나온다. single 56.4 ms는 OMP 4이고, OMP 1이면 91.1 ms다. multi4 copy(OMP 1) 94.9 ms와 거의 같다. 남은 3.8 ms는 multi 쪽 버퍼가 작은 탓으로 보지만 확인하지 않았다.
- 100회 9227 ms는 1회 92 ms의 100배로 맞다. multi4 100회는 thread 4개가 `MULTI_RUNS`(기본 25)번씩 돈 값이다. 2793 ms는 25번 도는 wall time이고, 동시 실행 때 한 번에 약 112 ms다.

**14:09 [PC] "single evta를 돌릴 때도 ... OMP_NUM_THREADS=1을 기본으로 하자." / 14:17 "-t 4 옵션이 OMP_NUM_THREADS=4 하고 똑같은 거지? 입력 버퍼를 2개로 나눠서 쓰게하는 thread=1/2는 어떻게 바꿔?"**
- `run_prof.sh` 3곳에 `-t <threads>`(기본 1)를 넣고, summary에 `omp=` 값을 찍게 했다(`e64f1e32`). 문법 검사만 했고 보드에서 돌려 보지는 않았다. multi2/4의 `run_all.sh`는 `collect_hw_bundles.py`에 넣어 다음 수집 때 반영되게 했다.
- `-t 4`는 `OMP_NUM_THREADS=4` export와 같다.
- 입력 버퍼를 나누는 것은 EVTA의 virtual thread(`nVirtualThread`)로, CPU thread와 별개다. 코드 생성 때 conv마다 정해진다(`VTASchedules.h`). 표에 없는 모양은 `VTASave.cpp:2246`에서 `1, 14, 14`다. 바꾸려면 이 표를 고치고 model-compiler와 bundle을 다시 만든다. 나누어떨어지지 않는 tile은 결과가 틀린 적이 있어 281 확인이 필요하다.

**14:21 [PC] "summary에서 total 시간들과 각 run 마다 걸린 시간 ... averga 시간과 GOPS도" / 14:24 "average를 없애고 median을 하자."**
- 새 스크립트 `run_times.py`와 `prof_summary.py`에 run별 시간, warm-up인 run 1을 뺀 median, min/max, GOPS를 넣었다(`e5fbacb7`, `dc18f3a`, `506d8b9`). 보드에 남은 로그로 확인했다. 첫 run은 261 ms였고, run 20 하나가 75 ms로 튀었다(`-r` 없이 돌린 것으로 보임).

  | 로그 | 값 |
  |---|---|
  | `systimed.log` (OMP 4 실행, 평균 계산) | runs 2-25 평균 57.769 ms (median 56.938, min 56.554, max 75.480), 80.51 GOPS |
  | `systimed.log` (다시 돌린 OMP 1 실행) | median 91.121 ms (min 90.958, max 91.475), 51.04 GOPS |
  | `sysprof.log` (EVTA 시간) | median 29.682 ms (min 29.673, max 29.691), 156.66 GOPS |

**14:31 [PC] "multi2/4 일 때는 summary에 2/4개 다 합한 GOPS도 ... evta 들 간에 conv 시작시간을 맞추는 lock을 걸고 돌려야 GOPS가 제대로 나오겠지?" / 14:42 "각각을 먼저 출력하고 다 합한 걸 나중에"**
- 합산 방법: 전체 run은 run마다 가장 느린 copy의 시간으로, conv는 4개 EVTA의 busy union으로 계산한다(`2b5577f`, `12911f6`, `e582bb0`, `6b876ae`).
- lockstep 답: conv 동시 성능은 `-l`을 걸고, 실제 처리량은 `-l` 없이 잰다. lockstep은 conv마다 barrier 대기가 붙는다.
- multi4 `_systimed` 로그의 copy 하나와 4개 합을 비교했다. lockstep 없는 실행이고, 평균 3.74개가 동시에 돌았다.

  | | copy 하나 | 4개 합 |
  |---|---|---|
  | 전체 run (CPU 포함) | median 95.0 ms, 48.9 GOPS | median 95.1 ms, **195.7 GOPS** |
  | conv만 (EVTA call) | median 32.3 ms, 143.8 GOPS | median 34.6 ms, **538.3 GOPS** |

- 사용자가 `-l`로 다시 돌린 로그다. 4개 합의 conv는 538 → 567 GOPS로 오르고, 전체 run은 195.7 → 192.1 GOPS로 조금 내렸다.

  | copy | whole run median | conv median |
  |---|---|---|
  | `mxnet_exported_resnet18` (EVTA 0) | 96.800 ms, 48.05 GOPS | 32.578 ms, 142.73 GOPS |
  | `_1` | 96.771 ms, 48.06 GOPS | 32.645 ms, 142.44 GOPS |
  | `_2` | 96.805 ms, 48.04 GOPS | 32.647 ms, 142.43 GOPS |
  | `_3` | 96.796 ms, 48.05 GOPS | 32.574 ms, 142.75 GOPS |
  | 4 copies lockstep | x4 96.828 ms, 192.13 GOPS | busy union 32.812 ms, 566.86 GOPS |

- 538이 예전 617 GOPS보다 낮은 이유: 617은 `_sysprof`의 EVTA 하드웨어 timestamp 값이고, `_timed`의 conv 시간은 명령 생성과 polling까지 든 wall time이다.

**14:48 [PC] "②: elemadd, relu, maxpool을 EVTA ALU로 ... alu_opcode bit 이 2 bit인데 할 수 있어?" / 15:34 "conv가 끝나고 나서 ddr로 안 옮기고 relu를 바로 한다든지 ... 하드웨어가 조건이 많이 추가되어야겠어."**
- 비트스트림은 그대로 두고 vtalib과 VTASave만 바꾸면 된다. 필요한 `MIN`, `MAX`, `ADD`, `SHR`은 2 bit로 된다. 쓸 수 없는 것은 `MUL`뿐이다. 코드를 읽고 정리한 내용으로, 이 시점에는 구현이나 측정 전이다.
- ALU는 int32 acc만 다루므로, int8 activation은 inp로 LOAD한 뒤 단위행렬 weight의 GEMM으로 acc에 올린다.
- 중요: 처음 제안(conv 뒤 별도 EVTA call)을 **conv 안에 넣는 방식**으로 바꿨다. 하드웨어에 조건을 더할 필요 없이 명령 순서로 된다. relu는 이미 STORE 전 `MAX 0`으로 이렇게 하고 있다. 별도 call은 DDR 왕복과 동기화 비용이 더 든다.
- maxpool은 창이 tile 경계를 넘어 conv 안에 넣기 어려우므로, 따로 EVTA call로 한다.
- 다른 NPU 비교: VTA는 범용 vector ALU 하나, Gemmini는 mvin 누적과 mvout 쪽 고정 기능, NVDLA는 SDP/PDP/CDP 전용 pipeline, Xilinx DPU는 misc engine이다. EVTA의 비용은 ALU가 GEMM과 compute 모듈을 같이 쓰는 점이다. ALU의 LUT 수와 II는 확인하지 못했다.

**15:47 [PC] "resnet18 v2는 relu 가 앞에 오고 ... 이렇게 고치면 할수 있는 거지?" / 15:53 "MxNet_Restnet18이 v2 아니야?" / 15:56 "이걸 진행하면 ... 원래대로는 못 돌리는 거잖아."**
- v2의 residual 더하기도 같은 경로로 된다. 다만 합 s에 대한 BN+ReLU는 이웃 conv에 접을 수 없어, 대각 weight GEMM을 쓰는 별도 call이 필요하다. relu 없이 끝나므로 음수 .5에서 CPU와 1 LSB 다를 수 있다. importer가 이 BN을 어떻게 내리는지와 v2 모델 유무는 미확인이다.
- 저장소의 `mxnet_exported_resnet18.onnx`는 **v1**이다. 노드 이름이 `resnetv10_...`이고, block마다 elemadd 다음에 relu가 온다. 반올림도 relu 덕분에 CPU와 bit 단위로 같다.
- 원래대로 돌릴 수 있게 한다: 옵션 `-vta-fuse-residual`을 두고, vtalib에는 함수를 추가만 하며, 비트스트림은 그대로 둔다.

**15:59 [PC] "좋아 진행해보자. 이전과 이후 성능을 비교해서 표를 만들어 보자."**
- 이제 conv가 끝난 직후 같은 EVTA 호출 안에서 residual 더하기와 relu까지 처리한다(`convolution_wo_tr_res`, `findResidualFusions`). Resnet18의 elemadd+relu 8개가 모두 EVTA로 넘어갔다.
- 검증: `EVTA_RES_CHECK=1`로 일반 경로와 원소마다 비교했다. single, multi4(copy 4개), single-thread exe 모두 8개 conv에서 불일치 0개이고, Result 281, Confidence 0.655530이다. 확인 후 기본값을 켬으로 바꿨다(`179effb`, `fbecb76`, `9a8257ad` ~ `dbc9e6f8`, `docs/evta_runtime_compare.md` §7.2).
- 전후 비교: systolic 400 MHz, `-r`, 25회 중 2~25회의 median. 같은 보드에서 이전과 새 bundle을 이어서 쟀다. 12:28 표의 multi4 100회(2793 ms)와 이 표의 이전 값(2470 ms)이 다른 이유는 기록에 없다.

  | 경우 | 이전 (①만) | ② 후 | 변화 |
  |---|---|---|---|
  | single, OMP 1, 전체 1회 | 90.93 ms (51.2 GOPS) | **67.13 ms** (69.3 GOPS) | −26.2 % |
  | single, OMP 4, 전체 1회 | 56.64 ms (82.1 GOPS) | **50.23 ms** (92.6 GOPS) | −11.3 % |
  | single, EVTA 하드웨어 시간 (`_sysprof`) | 29.68 ms (156.7 GOPS) | 30.80 ms (151.0 GOPS) | +1.1 ms |
  | multi4, 4개 동시 실행 중 copy 하나의 전체 1회 | 95.6 ms | **70.7 ms** | −26 % |
  | multi4, 4 copy 합 GOPS (전체 실행 기준) | 194.6 GOPS | **262.7 GOPS** | +35 % |
  | multi4, 4 EVTA 동시 (lockstep, conv만) | 616.6 GOPS | 593.9 GOPS | −4 % |
  | multi4, 4 copy 100회 wall time | 2470 ms (40.5 추론/s) | **1851 ms (54.0 추론/s)** | −25 % |
  | multi4 비트스트림, single-thread exe 100회 | 9231 ms | 6818 ms | −26 % |

- CPU의 elemadd와 relu가 빠져 OMP 1에서 23.8 ms, OMP 4에서 6.4 ms가 줄었다. EVTA는 1.1 ms 늘었다. GOPS는 conv 곱셈·덧셈만 세므로 EVTA GOPS는 조금 내려간다.
- 다시 빌드한 것은 측정에 쓴 `_sys*` 5개 dir뿐이다.

**16:33 [PC] "4개 돈 걸합해서 GOPS 계산하는 게 뒤에 오게 해줘. 각 evta가 돈 것도 한 줄씩 ..." / 16:36 "여기까지 돌다가 멈췄는데." / 16:43 "cpu conv/fc 도 분리해서 적어줘."**
- summary를 copy마다 한 줄, 합을 마지막 줄로 나누고(`d0a4193`), CPU conv/fc를 추가했다(`b82b34f`). 순서는 이미 바뀌어 있었는데 한 줄에 묻혀 안 보인 것이었다.
- fusion 후 multi4 `_systimed -l` 로그다. CPU conv/fc는 ops가 있는데 EVTA call이 아닌 문장의 시간으로, Resnet18에서는 fc 하나다.

  | copy | whole run median | conv median | cpu conv/fc median |
  |---|---|---|---|
  | `mxnet_exported_resnet18` | 72.939 ms, 63.76 GOPS | 33.855 ms, 137.35 GOPS | 4.586 ms, 0.22 GOPS |
  | `_1` | (원문 생략) | (원문 생략) | 4.591 ms, 0.22 GOPS |
  | `_2` | (원문 생략) | (원문 생략) | 4.586 ms, 0.22 GOPS |
  | `_3` | (원문 생략) | (원문 생략) | 4.590 ms, 0.22 GOPS |
  | 4 copies lockstep | x4 72.948 ms, 255.03 GOPS | busy union 34.102 ms, 545.41 GOPS | slowest copy 4.604 ms, 0.89 GOPS |

- 그 직전 실행의 로그다(16:34 형식 확인).

  | copy | whole run median | conv median |
  |---|---|---|
  | `mxnet_exported_resnet18` | 72.703 ms, 63.97 GOPS | 33.846 ms, 137.38 GOPS |
  | `_1` | 72.723 ms, 63.95 GOPS | 33.919 ms, 137.09 GOPS |
  | `_2` | 72.720 ms, 63.96 GOPS | 33.927 ms, 137.05 GOPS |
  | `_3` | 72.712 ms, 63.96 GOPS | 33.857 ms, 137.34 GOPS |
  | 4 copies lockstep | x4 72.731 ms, 255.79 GOPS | busy union 34.103 ms, 545.40 GOPS |
  | single exe (`singlevta...`, runs 2-100) | 67.779 ms, 68.62 GOPS | 33.459 ms, 138.97 GOPS |

- 중요: **사용자의 multi4 lockstep 실행이 멈췄다.** 100회 중 32회째에서 진행이 없었고, 프로세스는 CPU 100 %로 EVTA 완료를 기다렸다. 한 copy가 멈추면 나머지도 barrier에서 멈춘다.
  - 원인은 모른다. fusion 뒤 multi4 lockstep 실행 4번 중 1번 멈춰 fusion을 배제할 수 없다. `uop buffer overflowed` 경고는 fusion 전 로그에도 있어 원인이 아닌 것으로 본다.
  - kill은 권한이 거부되어 하지 못했고, 프로세스는 그 뒤 없어졌다.

**16:49 [PC] "②에서 남은 maxpool(conv0 뒤 하나, CPU 약 4 ms) 해보자. 이건 tile 경계에 걸리는 경우는 어떻게 해야 해?"**
- tile 경계: 출력 tile(16채널 블록 하나씩 3행 × 56열)은 겹치지 않게 하고, 입력은 tile마다 halo까지 DDR에서 다시 읽는다. 이웃 tile과 입력 1행이 겹쳐 두 번 읽힌다. tile 사이에 넘길 상태가 없다.
- 가장자리는 LOAD의 zero padding이므로, 입력을 relu가 붙은 EVTA conv나 relu가 썼을 때만 EVTA로 보낸다.
- 처리 순서는 LOAD → 단위행렬 GEMM → 출력 자리 0 → 창 9개 위치마다 ALU MAX 명령 1개 → STORE다.
- `EVTA_RES_CHECK=1`로 CPU maxpool과 비교해 single, multi4 모두 0개 불일치이고, Result 281, Confidence 0.655530이다. `maxpool_evta`, `findEvtaMaxPools`, `-vta-evta-maxpool`(기본 켬), `d9837eac`, `6b74b369`, §7.3.
- 단계별 측정: systolic 400 MHz, `-r`, run 2~25 median.

  | 경우 | ①만 | + conv/elemadd/relu fusion | + EVTA maxpool | ① 대비 |
  |---|---|---|---|---|
  | single, OMP 1 | 90.93 ms | 67.13 ms | **52.48 ms** (88.6 GOPS) | −42 % |
  | single, OMP 4 | 56.64 ms | 50.23 ms | **48.71 ms** (95.5 GOPS) | −14 % |
  | single, EVTA 하드웨어 시간 | 29.68 ms | 30.80 ms | 31.98 ms | +2.3 ms |
  | multi4 copy 하나 (4개 동시, median) | 95.6 ms | 70.7 ms | **~56.3 ms** | −41 % |
  | multi4 4 EVTA lockstep (conv MAC 기준) | 616.6 GOPS | 593.9 GOPS | 571.5 GOPS | −7 % |
  | multi4 4 copy 100회 wall | 2470 ms (40.5/s) | 1851 ms (54.0/s) | **1471 ms (68.0/s)** | −40 % |
  | single-thread exe 100회 (multi4 비트스트림) | 9231 ms | 6818 ms | **5393 ms** | −42 % |

- maxpool은 CPU에서 OMP 1일 때 약 15 ms, OMP 4일 때 약 3 ms였고, EVTA에서는 1.2 ms다.
- 설명이 덜 된 숫자: lockstep 없는 multi4 `_systimed`의 "4 copy 합"이 78.0 ms, 238.5 GOPS로 copy median 56 ms보다 컸다. 그래서 처리량은 `_sys` 100회 wall(copy당 58.8 ms/run)로 본다(00:12에 49.7 ms로 해소).

**20:17 [PC] "summary 출력에 각 런별로 줄바꿈을 넣어줘. MultiEVTARestconv10Test 도 ... 다 적용된거지? ... 총 속도가 600GOPS 넘었었는데 571GOPS 밖에 안 되네."**
- 사용자가 돌린 multi4 Resnet18 `_sysprof -n 25 -l` 결과다(exit=0, omp=1, golden 없음). EVTA 시간 기준이다. multi 전체는 total_time 1473.174 ms, total_vta_time 805.799 ms, single exe는 5253.883 ms, 3205.931 ms다.

  | 실행 | last | median |
  |---|---|---|
  | VTA0 | 32.208 ms, 144.37 GOPS | runs 2-25 32.224 ms, 144.30 GOPS |
  | VTA1 | 32.343 ms, 143.77 GOPS | 32.323 ms, 143.86 GOPS |
  | VTA2 | 32.333 ms, 143.81 GOPS | 32.335 ms, 143.81 GOPS |
  | VTA3 | 32.263 ms, 144.13 GOPS | 32.273 ms, 144.08 GOPS |
  | 4 EVTAs busy union | — | 571.51 GOPS (min 512.5, max 572.2, 25 runs) |
  | single exe (100 run, 21 EVTA calls) | 145.04 GOPS | runs 2-100 32.058 ms, 145.05 GOPS |

- MultiEVTAResConv10Test에는 적용되지 않았다. conv 하나만 반복하는 테스트라 609.7 GOPS, 380 MHz 685.1 GOPS는 그대로다.
- 중요: Resnet18 GOPS가 616 → 571로 내려간 것은 EVTA가 MAC이 아닌 일을 더 맡았기 때문이고, 처리량은 1.68배가 됐다. conv 부분은 따로 재면 지금도 copy마다 ~156 GOPS다.

  | | ①만 | + fusion | + EVTA maxpool |
  |---|---|---|---|
  | EVTA 시간/run (copy 하나) | 29.68 ms | 30.80 ms | 31.98 ms |
  | 더해진 일 | – | residual LOAD + 단위행렬 GEMM + ALU (+1.1 ms) | maxpool 21번째 EVTA 호출 (+1.2 ms) |
  | 4 EVTA GOPS (MAC 기준) | 616.6 | 593.9 | 571.5 |
  | **4 copy 100회 wall** | 2470 ms | 1851 ms | **1471 ms** |

**20:23 [PC] "(MultiEVTAResConv10Test `_sysprof` 출력) 이건 왜 아무것도 안 나오지?"**
- bundle .cpp에 같은 conv 호출이 본래 entry와 `_convonly`로 두 번 적혀 있어, `prof_summary.py`가 run마다 2개로 세어 "not a multiple"을 냈다. `_convonly`를 세지 않게 고쳤다(`e54cceb`).
- 고친 뒤 같은 로그의 값이다. 이 테스트는 `-n`, `-l`이 적용되지 않고 `'15 100'`으로 돈다. 사용자 로그의 total_time은 212.1~212.4 ms, total_vta_time은 123.1~123.4 ms다.

  | EVTA | last | median runs 2-101 |
  |---|---|---|
  | VTA0 | 1.218 ms, 189.83 GOPS | 1.218 ms, 189.83 GOPS |
  | VTA1 | 1.219 ms, 189.67 GOPS | 1.219 ms, 189.67 GOPS |
  | VTA2 | 1.222 ms, 189.21 GOPS | 1.221 ms, 189.36 GOPS |
  | VTA3 | 1.219 ms, 189.67 GOPS | 1.219 ms, 189.67 GOPS |
  | 4 EVTAs busy union | — | 620.28 GOPS (min 612.9, max 707.6, 101 runs) |

- 4 × 189.6 = 758이 아니라 620인 이유: run마다 어긋나 평균 3.3개만 동시에 돈다. run 2는 3.65개, 692 GOPS였다.

**20:33 [PC] "이건 왜 전체가 425GOPS만 나오지? ... EVTA0/1 결과는 안 나오네."**
- 분자는 conv MAC(231.2 Mops × 100회 × 4)이고 분모는 run 전체 wall이라서다. 609.7, 685.1 GOPS는 세 번째 인자 `conv` 모드 값이라 직접 비교할 수 없다.

  | ResConv10 `run_all.sh` (`15 100`) | 값 |
  |---|---|
  | EVTA2 | 100회 216.914 ms (2.169 ms each, 106.59 GOPS), output = golden |
  | EVTA3 | 100회 217.169 ms (2.172 ms each, 106.47 GOPS), output = golden |
  | 4 EVTAs | 217.186 ms wall, 425.83 GOPS, all = golden |
  | run 하나의 EVTA 실행 (`_sysprof`) | 1.22 ms |
  | run 합계 (alloc, transpose, flush, conv, free) | 2.17 ms, 나머지 약 0.95 ms가 CPU |

- EVTA0/1이 안 보인 것은 `run_all.sh`의 `tail -3` 때문이라 고쳤다(`7c2f13ba`, `a09b5206`). 보드의 미커밋 한 줄은 `git stash`로 옮겼고(`stash@{0}`, 같은 한 줄뿐), 보드는 `a09b520`이다.

**20:46 [PC] "Conv4Test가 타일 크기가 커서 속도가 빠른 건데 왜 이 속도 밖에 안 나오지?"**
- 사용자가 `./multi4conv4Bundle`을 그대로 실행한 결과: EVTA0 806.014 ms (8.060 ms each, 28.69 GOPS), EVTA1 832.352 ms (8.324, 27.78), EVTA2 832.400 ms (8.324, 27.78), EVTA3 834.672 ms (8.347, 27.70), 전체 834.709 ms wall, 110.80 GOPS, all = golden.
- 조건을 바꿔 쟀다. multi4 380 MHz bitstream, 실행마다 bitstream 재로드.

  | 실행 | run당 시간 | GOPS (4개 합) |
  |---|---|---|
  | 4 EVTA, 전체(entry), OMP 4 (그냥 실행했을 때와 같음) | 9.87 ms | 92.8 |
  | 4 EVTA, 전체, OMP 1 | 7.57 ms | 122.2 |
  | **4 EVTA, conv만** (`15 100 conv`), OMP 4 | **1.76 ms** | **525.6** |
  | 4 EVTA, conv만, OMP 1 | 1.71 ms | 487.8 (wall 기준이 어긋남, copy마다는 135 GOPS) |
  | EVTA 1개, 전체, OMP 1 | 7.03 ms | 32.9 |
  | EVTA 1개, conv만, OMP 1 | 1.71 ms | 135.5 |

- 중요: EVTA conv는 1.7 ms로 빠르다. 나머지 5.3~5.9 ms는 CPU의 byte 단위 transpose다. NHWC → N{C/16}HW16은 사실 픽셀마다 16바이트 복사로 끝난다.

**20:50 [PC] "해보자."**
- transpose를 16바이트 `memcpy`로 바꿨다. 모든 실행에서 golden과 같다.
- 중요: **처음 고친 파일이 틀렸다.** `CPUBundle_aarch64.cpp`를 고쳤는데 보드 빌드는 `NESTC_EVTA_RUN_WITH_GENERIC_BUNDLE=ON`이라 `CPUBundle_generic.cpp`를 쓴다. 두 파일 모두 고쳤다(`2a6b781`, `634f448`, `52c9a514`, `90effa36`).
- CPU 쪽 시간 분석: 보드에서 따로 만든 측정 프로그램, 56×56×64, run당. CMA buffer는 cache가 켜져 있어 malloc 메모리와 속도가 같고, 원인은 메모리가 아니라 byte 단위 코드였다.

  | 단계 | 바꾸기 전 OMP 1 | 바꾼 뒤 OMP 1 | 바꾼 뒤 OMP 4 |
  |---|---|---|---|
  | `transpose_nhwc2vtaio` | 1.75 ms | **0.20 ms** | 0.11 ms |
  | `transpose_vtaio2nhwc` (원소마다 `f/16`, `f%16`) | 3.31 ms | **0.26 ms** | 0.08 ms |
  | `VTABufferAlloc` ×2 + Free ×2 | 0.29 ms | 0.29 ms | 0.29 ms |
  | flush + invalidate | 0.04 ms | 0.03 ms | 0.03 ms |

- 다시 잰 결과: 380 MHz multi4 bitstream, OMP 1, 100회. ResConv10 전체 run의 바꾸기 전 값만 다른 조건이라 비교가 정확하지 않다. 같은 380 MHz 조건의 바꾸기 전 값은 재지 않았다.

  | 테스트 | 바꾸기 전 | 바꾼 뒤 | conv만 (`conv` 인자) |
  |---|---|---|---|
  | Conv4 EVTA 4개 전체 | 757 ms, 122 GOPS | **316 ms, 293 GOPS** | 167 ms, 553 GOPS |
  | Conv4 EVTA 1개 전체 | 703 ms, 33 GOPS | **243 ms, 95 GOPS** | 166 ms, 139 GOPS |
  | ResConv10 EVTA 4개 전체 | 425.8 GOPS (`run_all.sh`, systolic 400 MHz 실행 파일) | **161 ms, 574 GOPS** | 135 ms, 683 GOPS |
  | ResConv10 EVTA 1개 전체 | – | 157 ms, 147 GOPS | 135 ms, 171 GOPS |

- Conv4 EVTA 1개에서 conv만은 1.66 ms, 전체는 2.43 ms다. 남은 0.77 ms 중 0.75 ms가 alloc/free와 transpose다.
- 이유 미확인: 전체 run은 4개(316 ms)가 1개(243 ms)보다 느린데, conv만은 같다. CPU 작업이 동시에 돌 때 서로 방해하는 것으로 본다.

**23:49 [PC] "다른 dir 들도 빌드해서 재보자. transpose가 byte 단위에서 16byte 단위 copy로 바뀌는 것만으로 많이 빨라지네." / 11-01 00:15 "vtalib_zcu102-systolic/lib/ 에서만 바꿔서 ... 아니면 vta/vtalib/lib 에서 바꿘거야?"**
- 중요: 두 transpose만으로 Resnet18은 52.48 → 52.45 ms로 거의 그대로였다. Resnet18에는 그 둘이 남아 있지 않기 때문이다. 실제 효과는 timing table에서 찾은 conv0 입력의 `transpose_nhwc2vtaio_zerofill`(OMP 4에서도 6.8 ms, conv0 자체 9.8 ms와 맞먹음)을 같이 고쳐서 나왔다. **0.2 ms**가 됐다(`f158ccb`, `fe3ceff2`, `cdcfae3d`, §7.4). 결과는 예전 코드와 같다(N=1). 예전 코드에 있던 N>1 batch 혼합 버그는 쓰는 곳이 없어 문제가 없었다.
- Resnet18 측정: systolic 400 MHz, `-r`, run 2~25 median. EVTA 하드웨어 시간 31.98 ms와 lockstep 571.5 GOPS는 그대로이고, Result 281, Confidence 0.655530도 같다.

  | 경우 | 7.3 후 (maxpool까지) | + 16바이트 transpose | 변화 |
  |---|---|---|---|
  | single, OMP 1 | 52.48 ms | **46.90 ms** (99.2 GOPS) | −10.6 % |
  | single, OMP 4 | 48.71 ms | **41.81 ms** (111.3 GOPS) | −14.2 % |
  | multi4 copy 하나 (4개 동시) | ~56.3 ms | **49.5 ms** | −12 % |
  | multi4 4 copy 100회 wall | 1471 ms (68.0/s) | **1314 ms (76.1/s)** | −10.7 % |
  | single-thread exe 100회 (multi4 bitstream) | 5393 ms | **4724 ms** | −12.4 % |

- 하루 동안의 단계: CPU가 병목이라는 측정을 따라 차례로 옮겼다. ① 시작 전과 비교하면 single OMP 1은 −48 %, multi4 처리량은 1.9배다.

  | 단계 | 왜 | single OMP 1 | multi4 100회 wall |
  |---|---|---|---|
  | ① layout 유지 | conv 사이 transpose 왕복 제거 | 90.93 ms | 2470 ms (40.5/s) |
  | + fusion | conv 뒤 CPU elemadd, relu를 conv 호출 안으로 | 67.13 ms | 1851 ms (54.0/s) |
  | + EVTA maxpool | conv0 뒤 CPU maxpool을 EVTA로 | 52.48 ms | 1471 ms (68.0/s) |
  | + 16바이트 transpose | conv0 zerofill 6.8 → 0.2 ms | 46.90 ms | 1314 ms (76.1/s) |

- multi4 `_systimed`의 "4 copy 합"은 78.0 → 49.7 ms가 되어 copy median과 거의 같아졌다. CPU에서 긴 transpose가 겹쳐 늦는 copy가 생겼던 것으로 본다.
- 첫 수정 뒤 `run_all.sh`를 hw, multi2, multi4 전부 돌려 golden이 있는 테스트는 모두 PASS였다. zerofill 수정 뒤에는 Resnet18 결과만 확인했다. 빌드 루프 둘이 실수로 같은 dir에서 동시에 돌았지만, 둘 다 rc=0이고 다시 make할 것이 없었다.
- 소스는 `vta/vtalib`과 `vtalib-zcu102-systolic` 둘 다 고쳤다. `_systimed`, `_sysprof`, multi4 `_sys`는 systolic 복사본으로, Conv4와 ResConv10 숫자와 `run_all.sh` PASS는 `vta/vtalib`(`build_multievtax4`, 380 MHz)으로 빌드했다. 실제 빌드에 들어가는 것은 `CPUBundle_generic.cpp`뿐이다.

**11-01 01:08 [PC] "(`./multi4resconv10Bundle 15 100 conv`) terminate called ... dmlc::Error ... Aborted (core dumped)" / 01:12 "막아줘."**
- 중요: **보드에 single bitstream이 남은 상태에서 multi4 실행 파일을 돌려** 없는 EVTA1~3 register(`0xA0010000~`)에 접근해 AXI SError가 났다. 앞선 측정 묶음의 끝이 `hw/script/run_all.sh Resnet18Test`였다는 점을 안내하지 않은 것이 발단이다.
- `dmesg`에 SError Oops #8~#14가 있었다. 확인 중 multi4 bitstream load가 `Cannot allocate memory`로 실패(CMA 88 MB가 남아도 `cma_alloc` 실패)했는데, load 실패 후에도 실행해 #14를 또 냈다. 다른 접속이 있어 재부팅하지 않고 사용자에게 물었다.
- 막는 방법: 없는 EVTA 주소는 읽기만 해도 SError이므로, run script가 load한 이름을 `/tmp/evta_bitstream`에 기록(실패 시 `FAILED <이름>`)하고, vtalib `pynq_driver.cc` `CheckLoadedBitstream(index)`가 EVTA register를 건드리기 전에 검사해 `exit 3`으로 끝낸다. `EVTA_NO_BIT_CHECK=1`로 건너뛴다(`daf1119`, `b3ea4a77`). 14개 build dir을 모두 다시 빌드했다(모두 rc=0).
- 보드 확인(보드가 16:11쯤 재부팅되어 있었고 그 뒤에 확인). 모든 확인 동안 `dmesg` SError 0번이다. single bitstream에서 mask 15로 돌리는 경우는 시험하지 않았다.

  | 상황 | 결과 |
  |---|---|
  | 기록 없음 (재부팅 직후), `./multi4resconv10Bundle 15 100 conv` | `EVTA0: no bitstream recorded in /tmp/evta_bitstream ...`, rc=3 |
  | single bitstream load 후 multi4 실행 (EVTA1~3, mask 14) | `EVTA1: the loaded bitstream vtahw_..._380mhz.bin has 1 EVTA(s) ...`, rc=3 |
  | 4evta bitstream load 후 `15 100 conv` | 684.3 GOPS, all = golden, rc=0 |
  | `run_all.sh MultiEVTAConv4Test MultiEVTAResConv10Test` | 둘 다 PASS |

- 한계: 다른 방법(Jupyter, 직접 `echo`)으로 bitstream을 바꾸면 기록이 낡아 못 막는다. `/tmp`는 부팅 때 비워지므로 재부팅 뒤에는 script로 한 번 load해야 한다.

**11-01 01:19 [PC] "이제 남은 게 뭐라 그랬어?" / 01:23 "VTABufferAlloc을 처음에 한번 잡고 재사용 하게 하는 건 그냥 빼면 돼?"**
- OMP 1 single 46.9 ms 중 EVTA call이 33.4 ms, 나머지 약 13.5 ms가 CPU다. 아래에서 fc의 OMP 1 값만 직접 쟀고, 나머지는 OMP 4 로그에서 읽었다. OMP 1에서는 더 길다.

  | 항목 | 시간 (run당) | 할 수 있는 것 |
  |---|---|---|
  | fc (dense0) | OMP 1 4.4 ms, OMP 4 1.2 ms | EVTA GEMM으로 옮기기, 또는 CPU 코드 최적화 |
  | 전처리 quantize, elemsub, elemdiv | 0.58 + 0.68 + 0.94 ≈ 2.2 ms | 하나의 loop로 합치기 |
  | `VTABufferAlloc`/Free | 여러 번, 각 0.15~0.35 ms | buffer pool |
  | maxpool_evta | CPU에서 본 시간 1.58 ms (EVTA 하드웨어 시간 ~1.2 ms) | |
  | avgpool | 0.24 ms | |

- conv0 9.8 ms는 EVTA 연산의 13/16이 0과의 곱이다. 채널을 묶으면 줄일 수 있지만 컴파일러와 vtalib을 크게 고쳐야 한다.
- 원인을 아직 보지 않은 것: multi4 `_systimed -l` 멈춤(그 뒤 재현되지 않음, fusion 때문인지 미확인), Conv4 전체 run의 4 EVTA가 1 EVTA보다 느림, `VTABufferAlloc` 198 ms(OMP 4 로그, run 1과 2의 conv0 출력 buffer, run 3부터 0.35 ms).
- `VTABufferAlloc`은 그냥 빼면 안 된다. EVTA가 DMA로 읽고 쓰는 물리 연속 메모리(CMA)이기 때문이다. 바꿀 것은 run마다 커널에서 받고 돌려주는 부분이다. Resnet18 single OMP 4 로그, run 25에서 Alloc 23번 2.22 ms, Free 23번 0.37 ms, 합 약 2.6 ms(41.8 ms의 6 %)다.
- 권고는 A안이다. vtalib `DataBuffer::Alloc`/`Free`에서 크기별 목록으로 재사용하고(두 복사본만 수정, bundle은 다시 빌드만, multi4용 mutex 필요), B안은 VTASave가 `load_module`에서 한 번 잡는 방식(모든 bundle 재생성, 재사용 계획 필요)이다. 재사용 buffer의 이전 데이터는 결과와 golden으로 확인하기로 했고, 198 ms는 pool로도 없어지지 않을 수 있다.

## 결정이 남은 것 (11-01 07:00 기준)
- 사용자 답을 기다리는 것:
  - `VTABufferAlloc` pool을 A안으로 진행할지.
  - 남은 CPU 시간 중 어느 것부터 할지. 효과는 fc 4.4 ms(OMP 1)가 가장 크고, 전처리 합치기(~2.2 ms)와 alloc pool이 그다음이다.
  - multi4 lockstep 멈춤을 반복 실행으로 재현해 볼지. `-vta-fuse-residual=false` bundle과 비교하면 원인을 가를 수 있다.
  - summary에 maxpool 호출을 뺀 conv만의 GOPS를 넣을지(fusion의 residual 덧셈은 분리할 수 없다), 보드 `stash@{0}`를 지울지.
- 이날 결정된 것:
  - single도 `OMP_NUM_THREADS=1`이 기본이고, 4 thread는 `-t 4`로 준다.
  - summary는 평균 대신 median(warm-up인 run 1 제외)을 쓰고, copy별 먼저, 합은 마지막 줄에 쓴다.
  - `-vta-keep-layout`, `-vta-fuse-residual`, `-vta-evta-maxpool`은 기본 켬이고, `=false`로 원래 코드를 낼 수 있다. 하드웨어는 바꾸지 않았다.
  - load된 bitstream보다 EVTA가 많은 실행 파일은 `CheckLoadedBitstream`으로 시작 전에 막는다.
