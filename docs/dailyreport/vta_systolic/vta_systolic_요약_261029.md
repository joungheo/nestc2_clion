# 2026-10-29 EVTA systolic 요약: conv0 schedule(가상 스레드 2, tile 8×16)로 conv0 −26%, multi4 Resnet18 lockstep 4-EVTA 617 GOPS, `chrt -f 10`(-r)으로 흔들리는 run 제거

- 기간: 2026-10-29 07:00 ~ 10-30 07:00 (KST)
- 이날 기록은 세션 하나뿐이다. [PC]는 PC `main`에서 한 작업이고, 보드 실험도 이 세션에서 ssh로 돌렸다(보드 `main`은 git pull만 함).

## 시간순

**12:33 [PC] "Resnet18PartitionTest는 conv0를 float으로 그냥 두자. 지운 vtalib-zcu102-multi4의 ACL 라이브러리는…? systolic 빌드 dir 2개는…? systolic bit에서 돌려서 결과를 보자…" / 12:37 "오늘 아침 7시에 systolic 요약 문서가 왜 생성이 안 됐지? … 5page 이내로"**
- 중요: 7시 요약이 안 생긴 원인은 cron 환경에 `$USER`가 없는데 `daily_report.sh`가 `set -u`라 바로 종료된 것이다. `id -un`으로 대체하고 분량을 "5쪽 이내"로 바꿨다. 빠진 10-28 요약을 새로 만들었다(`11c97a22`).
- ACL: 다시 빌드하지 않았다. 같은 파일이 `vta/vtalib/lib/acl/`에 있어서, cache의 옛 경로를 지우고 cmake를 다시 돌려 링크했다. 다른 build dir의 `VTALIB`을 바꿀 때는 `cmake -UEXTERN_ACL_*`도 줘야 한다.
- systolic 빌드 dir 2개는 systolic 비트스트림용 실행 파일 빌드다. 옛 uop 순서로 돌려도 결과는 같지만 속도가 절반쯤으로 떨어진다. 이 dir들이 git 밖 사본 `/home/xilinx/JO/npuprg/systolic/vtalib-zcu102-systolic`을 쓰고 있어서 저장소 것으로 바꿔 다시 빌드했다.
- systolic 비트스트림 결과: single 30 PASS, multi4 모두 PASS, Resnet18 281 / 0.656(HLS 판과 같음). `systolic` branch는 `main..systolic` 0 커밋이라 PC·origin·보드에서 지웠다. `Resnet18PartitionTest`는 conv0을 float로 둔다(281, confidence 0.530).
- Resnet18 시간: HLS 380 MHz와 systolic 400 MHz를 비교했다. multi4 두 줄은 `OMP_NUM_THREADS=1`로 두 비트스트림을 번갈아 두 번씩 잰 값이다.

  | | HLS 380 MHz | systolic 400 MHz | 변화 |
  |---|---|---|---|
  | single, EVTA 시간 (추론 2회) | 68.56 ms | 65.99 ms | −3.8 % |
  | multi4 multi, 추론 100회 | 4049 / 4033 ms | 4001 / 4004 ms | −1.0 % |
  | multi4 single, 추론 100회 | 15367 / 15367 ms | 15276 / 15283 ms | −0.6 % |

  추론 시간의 대부분이 CPU 시간이라 EVTA 클럭을 올려도 거의 줄지 않는다. `run_all.sh`로 잰 시간(multi 6.2 s, single 8.0 s)과 차이가 나는데 원인은 아직 보지 않았다(13:14에 `OMP_NUM_THREADS` 차이로 추정).

**12:48 [PC] "/home/xilinx/JO/npuprg/systolic/ 은 언제 만들어진거야? … 문제 없으면 지우자."**
- 10-27 10:05에 이 세션이 `git archive FETCH_HEAD vtalib-zcu102-systolic`으로 풀어 둔 dir이었다(보드 branch를 바꾸지 않으려고). 저장소 것과 주석만 달랐고, 가리키는 build dir이 0건이라 지웠다.

**13:08 [PC] "multi4는 hw_multievtax4/script/run_prof.sh -s _timed 로 돌리면 되지? … systolic bit 400MHz 로 돌리는 것도 만들자. single/multi4 두 경우 모두."**
- `build_systimed`(single), `build_multievtax4_systimed`(multi4)를 만들었다. suffix가 `_sys`로 시작하면 `run_prof.sh`가 `SYS_BIN`을 쓰므로 스크립트는 고치지 않았다.
- 주의: `run_prof.sh`/`run_all.sh`는 `OMP_NUM_THREADS`를 정하지 않는다. multi4는 직접 붙여야 한다.
- Resnet18을 두 판에서 같은 날 번갈아 쟀다. 결과는 모두 281.

  | | HLS 380 `_timed` | systolic 400 `_systimed` | 변화 |
  |---|---|---|---|
  | single 추론 전체 (run 2) | 78.35 ms | 77.27 ms | −1.4 % |
  | 그중 EVTA conv(`convolution_wo_tr`) 합 | 36.64 ms | 35.49 ms | −3.1 % |
  | 그 밖 (CPU) | 41.67 ms | 41.72 ms | 0 |
  | multi4 multi, copy 4개 평균 | 159.2 ms | 156.9 ms | −1.4 % |
  | multi4 single (run 100) | 153.33 ms | 153.05 ms | −0.2 % |

  systolic이 빨라지는 것은 EVTA conv 부분뿐이고 그것도 3 % 정도다(`894be969`).

**13:38 [PC] "… 각 evta3/1/2/0 를 25번씩 돌리고 결과를 따로 보여주는 거겠지? … 왜 single이 돌아? OMP_NUM_THREADS=4 … thread=2로 돌려야 load가 compute 뒤로 숨지?"**
- multi4 실행 파일은 copy 4개가 pthread 하나씩으로 동시에 돈다(`.cpp` 이름 없음/`_1`/`_2`/`_3` → EVTA 0/1/2/3). 횟수는 공유 카운터라 고정이 아니고(합 99회, 예: 25/25/24/25), 표 순서는 `atexit` 역순이라 실행마다 다르다.
- `singlevtaMxnetResnet18Bundle`은 EVTA 0 하나로 100번 도는 비교 기준이다. 중요: 두 실행 파일이 같은 `systimed.log`에 써서 multi 로그가 덮였다 → `<suffix>_<exe>.log`로 나눴다(`2b059a53`).
- `OMP_NUM_THREADS`는 CPU 연산(OpenMP 루프)에만 영향을 주고 pthread마다 따로 팀을 연다. Resnet18 100회를 systolic 400 판에서 스레드 수만 바꿔 쟀다.

  | `OMP_NUM_THREADS` | 1 | 2 | 3 | 4 |
  |---|---|---|---|---|
  | multi4 (pthread 4개) | **4014 ms** | 4955 | 5813 | 6025 |
  | single (pthread 1개) | 15276 | 10285 | 8638 | **7989 ms** |

  multi는 1, single은 기본값(4)으로 재는 것이 맞다(README에 적음). load가 compute 뒤로 숨는 것은 EVTA 하드웨어 안의 일이라 CPU 스레드 수와 관계없다.

**13:57 [PC] "run_prof.sh -s _systimed 이면 bit 도 systolic 이고 runtime 빌드도 build_systimed 걸 써서…? build_sysprof/은 또 뭐야?"**
- 맞다. `_sys*`면 `SYS_BIN`(single `..._16_16_19_18_sa_400mhz.bin`, multi4 `..._4evta_sa_400mhz.bin`), 실행 파일은 `hw/<Dir>/<exe>_systimed`이다.
- `_prof`/`_sysprof`는 VTA_SYNC별 EVTA 시간(`-DNESTC_EVTA_PROFILE=ON`), `_timed`/`_systimed`는 문장별 wall 시간(`-DBUNDLE_TIMED=ON`), suffix 없음/`_sys`는 측정 코드 없음. systolic 쪽에는 multi4 `_sysprof`와 single `_sys`가 빠져 있었다.

**14:05 [PC] "… vta 시간만이라고 했는데 몇번 돌렸는지 그럼 GOPS는 얼마인지 등이 없으니 빠른 건지 어떤지 모르겠네."**
- `run_prof.sh`가 추론 횟수, 1회당 EVTA 시간, GOPS를 찍게 했다. 새 `prof_summary.py`가 번들 .cpp의 `convolution_wo_tr` 인자에서 conv별 연산량을 구해 VTA_SYNC 시간과 짝짓는다(`d09fa3b7`, `9129299d`). 번들 main은 Resnet18을 2번 돌린다(events 40 = 2 × conv 20).
- HLS 380(`_prof`)과 systolic 400(`_sysprof`)을 연달아 쟀다.

  | | HLS 380 | systolic 400 |
  |---|---|---|
  | 추론 1회 EVTA 시간 | 34.26 ms | 32.99 ms (−3.7 %) |
  | Resnet18 전체 | 135.7 GOPS | 140.9 GOPS |
  | 큰 3×3 conv (stage4) | 180.7 GOPS (최대 194.6의 93 %) | 190.0 GOPS (최대 204.8의 93 %) |
  | conv0 | 12.81 ms | 12.43 ms |
  | 1×1 downsample | 38~49 GOPS | 39~50 GOPS |

- 큰 conv는 이론 최대의 93 %다. conv0이 추론 EVTA 시간의 38 %를 쓴다(실제 3채널로 치면 19 GOPS 정도). 1×1 downsample은 데이터 이동에 묶여 40 GOPS 안팎이다.

**14:28 [PC] "runtime 으로 만들때인지 … thread=1/2로 줬었는데 이거는 뭐였지? … multi4 … OMP_NUM_THREADS=4로 하면 … 서로 경쟁해서 오히려 느려지는 건가?"**
- 기억하신 thread=1/2는 conv schedule의 가상 스레드(`nVirtualThread`)다. 번들 생성 때 `VTASchedules.h`가 정하고 `VTASave.cpp`가 `convolution_wo_tr(..., vtaCmdH, <가상 스레드>, <tileH>, <tileW>, <EVTA 번호>)`에 박는다. 2면 weight/acc SRAM을 둘로 나눠 load가 GEMM 뒤에 숨는다.
- 중요: Resnet18의 3×3·1×1 conv 19개는 가상 스레드 2인데, 전날 EVTA로 옮긴 **conv0만 1, tile 8×8**이었다(표에 항목이 없어 기본값).
- multi4는 pthread 4개 고정, 스레드 i → EVTA i. `OMP_NUM_THREADS=N`이면 4 × N 스레드가 되어, 4로 주면 16개가 코어 4개를 다툰다(13:38 표와 같음).

**14:34 [PC] "표에 conv0 항목을 넣어 가상 스레드 2와 더 큰 tile로 바꿔보자. multi4 … 각각이 돌 때 thread=2로 … 2 영역으로 나눠서 써?"**
- 네. SRAM은 EVTA마다 따로이고, weight·acc는 가상 스레드별 두 영역, inp는 가상 스레드로 나누지 않고 입력 채널 블록마다 double buffer를 쓴다.
- conv0 schedule sweep: HLS 380 single 판에서 conv0 한 번의 EVTA 시간이다(추론 2회 모두 같은 값).

  | 가상 스레드 | tile | conv0 | 결과 |
  |---|---|---|---|
  | 1 | 8×8 (지금 기본값) | 12.81 ms | 281 / 0.6555 |
  | 2 | 8×8 | 11.17 ms | 같음 |
  | 1 | 11×16 | 10.51 ms | 같음 |
  | 2 | 16×8 | 10.37 ms | 같음 |
  | 2 | 7×16 | 9.62 ms | 같음 |
  | 2 | 8×14 | 9.72 ms | 같음 |
  | **2** | **8×16** | **9.48 ms (−26 %)** | 같음 |
  | 2 | 14×14 | 9.30 ms | 같음, 하지만 multi4에서 쓸 수 없음 |
  | 2 | 16×11 | 10.10 ms | **confidence 0.6276로 달라짐** |
  | 2 | 4×28 | 15.31 ms | 같음 |

- 가상 스레드 2, tile 8×16을 골랐다. 14×14는 입력 tile 33×33 = 1089 entry라 multi 버퍼 inp 한 영역(1024)을 넘는다. 중요: 112를 나누지 못하는 tile(16×11)에서 결과가 달라져, W 쪽 경계 처리 문제로 보고 약수 tile만 골랐다. `VTASchedules.h`에 넣었다(`fa996be`, BNN 첫 conv도 같은 schedule).
- 다시 만든 범위: model-compiler, single/x2/x4 번들(7개 copy 모두 `vtaCmdH<i>, 2, 8, 16, <i>`), build dir 13개.

  | 세트 | golden 결과 |
  |---|---|
  | single, HLS 380 | 30 PASS |
  | single, systolic 400 (`_sysprof`) | 30 PASS |
  | multi2 | 3 PASS |
  | multi4, HLS 380과 systolic 400 (`_sys`) | 3 PASS |

  Resnet18 281 / 0.6555, Resnet18Partition 281 / 0.530, Resnet50 281 / 0.702로 그대로다. 아래는 바꾸기 전 → 후 속도다(multi4는 `OMP_NUM_THREADS=1`).

  | | HLS 380 | systolic 400 |
  |---|---|---|
  | conv0 | 12.81 → **9.49 ms** | 12.43 → **9.13 ms** |
  | 추론 1회 EVTA 시간 | 34.26 → 30.96 ms (150.3 GOPS) | 32.99 → 29.69 ms (156.6 GOPS) |
  | single 추론 전체 (`_timed` run 2) | 78.35 → 75.01 ms | 77.27 → 74.00 ms |
  | multi4 multi, copy 평균 | 159.2 → 155.0 ms | 156.9 → 154.9 ms |
  | multi4 single (run 100) | 153.33 → 149.87 ms | 153.05 → 149.05 ms |

  추론 1회 EVTA 시간 약 −3.3 ms, 추론 전체 약 4 % 빨라졌다(`fa996be`, `49fb6119`, `9c14607`).

**14:52 [PC] "run_prof.sh -s _prof/_sysprof 를 돌릴 때 … 25번 돌리고 1,2,3,25번째를 나란히 … 돌리는 횟수도 정해줄 수 있게"**
- `run_prof.sh -n <횟수>`(기본 25, single 세트만). `timed_bundle.py`가 추론 함수를 감싸 `BUNDLE_RUNS=n`이면 모두 n번 돈다. `_prof` 표는 run 1, 2, 3, 마지막의 ms·GOPS와 `sum` 줄을 붙인다. vtalib profile 이벤트 버퍼를 1024 → 8192개로 늘렸다(Resnet50 25회 = 1300개)(`9a85678d`, vtalib `43c60e8`).
- 보드 확인 결과다.

  | 명령 | 결과 |
  |---|---|
  | `Resnet18Test` (`_prof`, HLS 380) | 25 run, VTA_SYNC 500개, run별 EVTA 시간 31.19 / 31.12 / 31.08 / 31.10 ms, 149.5 GOPS |
  | `-s _sysprof Resnet18Test` | 25 run, 29.69 / 29.70 / 29.69 / 29.68 ms, 156.6 GOPS |
  | `-s _timed Resnet18Test` | 25 run(s) 표 |
  | `-s _systimed -n 5 Resnet18Test Resnet50Test` | 둘 다 5 run(s), 결과 281 |

- `_timed` run 25 전체가 96 ms로 run 2~3(75 ms)보다 길었다. 늘어난 곳은 CPU transpose 두 개(0.13 → 9~12 ms)다. OpenMP 스레드가 잠깐 CPU를 못 받은 탓으로 보며, 다른 사용자 실행이 겹쳤는지는 확인하지 않았다. EVTA 시간은 25 run 내내 ±0.1 ms 안이었다.

**15:07 [PC] (보드 콘솔의 `vtaResConv2Test` translation fault 로그 붙여넣기) / 15:10 "다른 유저가 쓰고 있으면 에러 메시지를 내고 바로 멈추는 걸 … 다른 유저들도 쓰게 하려면?"**
- 보드는 재부팅되지 않았다(uptime 3일 23시간). 크래시는 다른 사용자의 `vtaResConv2Test` NULL 포인터 segfault다. jinhu의 `sudo gdb`가 06:00부터 멈춘 상태로 EVTA·CMA를 잡고 있을 수 있어 끝난 뒤 돌리길 권했다. 암호를 묻는 것은 `sudo VAR=값 명령` 꼴 때문이다 → `sudo /bin/sh -c "..."`.
- 잠금(`/tmp/evta_run.lock`)은 우리 스크립트만 잡는다. 다른 사용자는 자기 트리(git 아님)에서 빌드한 실행 파일을 직접 돌린다. 제안: 1) vtalib `pynq_driver.cc`에서 `flock`(부모가 잠금 주인이면 통과), 2) `fuser /dev/xlnk`로 감지(`/dev/xlnk`를 실제로 여는지 보드에서 미확인), 3) `/etc/ld.so.preload`(권하지 않음). 1+2를 권하고 진행 여부를 물었다.

**15:18 [PC] "run_prof.sh -s _sysprof 로 돌렸을 때가 _systimed로 돌렸을 때보다 GOPS 속도가 왜 더 크게 나오지?"**
- 재는 구간이 다르다. `_sysprof`는 EVTA 실행 시간만, `_systimed`는 `convolution_wo_tr()` 호출 전체(CPU 명령어·uop 생성 포함, EVTA와 겹치지 않음)다. 아래는 systolic 400 Resnet18의 두 측정 비교다.

  | conv | `_sysprof` (EVTA만) | `_systimed` (호출 전체) | 차이 = CPU 준비 |
  |---|---|---|---|
  | conv0 (tile 98개) | 9.131 ms | 9.786 ms | 0.66 ms |
  | stage1_conv0 | 1.516 | 1.660 | 0.14 |
  | stage2_conv0 | 0.875 | 0.975 | 0.10 |
  | stage4_conv4 | 1.218 | 1.318 | 0.10 |
  | **conv 20개 합 (run 25)** | **29.68 ms → 156.7 GOPS** | **32.15 ms → 144.6 GOPS** | **2.47 ms** |

- `_systimed` 요약 줄의 127 GOPS(36.58 ms)에는 CPU fc 4.43 ms가 들어 있다. run 1이 느린 것은 첫 run에서 uop를 새로 만들기 때문이다.

**15:22 [PC] "_timed 요약 줄을 conv(EVTA 호출)와 fc로 나눠 찍자. … multi4 systolic 에서도 … prof 없을 때, _sysprof, _systimed 일 때 만들자."**
- `_timed` 요약 줄을 `EVTA calls … GOPS; CPU conv/fc …`로 나눴다(`vtaCmdH`를 인자로 받는 문장 = EVTA 호출). multi4 systolic에 `build_multievtax4_sysprof`를 새로 만들었다. vtalib이 표마다 `[profile] VTA<i> events`를 찍고 `prof_summary.py`가 EVTA별 표를 만든다(`6d3470a1`, vtalib `e9a4436`).
- 첫 multi 측정은 남은 build dir 9개의 `make -j2`와 겹쳐 느렸다(아래). 빌드가 끝난 뒤 다시 쟀다.

  | | 빌드와 겹친 측정 | 이전 측정 |
  |---|---|---|
  | 추론 한 번 전체 시간 | 223~307 ms | 약 155 ms |
  | copy별 EVTA 시간 | 30~56 ms | — |

- 빌드 끝난 뒤 multi4 systolic 400 Resnet18, `OMP_NUM_THREADS=1`. 결과는 모두 281이다.

  | | `_sysprof` (EVTA 시간, 마지막 run) | `_systimed` (copy별 마지막 run 전체) | `_sys` (100회 전체) |
  |---|---|---|---|
  | multi (EVTA 4개 동시) | VTA0 29.85 / VTA1 29.86 / VTA2 29.82 / VTA3 29.81 ms, 각 156 GOPS | 156.6 / 153.0 / 160.1 / 153.0 ms | 3909 ms |
  | single (EVTA 0, 1개) | 29.75 ms, 156.3 GOPS | 148.7 ms (EVTA 호출 32.2 ms = 144.4 GOPS, CPU fc 4.4 ms) | 14959 ms |

  4개 동시여도 EVTA 시간은 하나일 때와 같다(DDR/HP 경쟁 안 보임). 처리량은 3.8배(초당 25.3회 대 6.7회), 추론 한 번 시간은 둘 다 약 150~155 ms다.

**15:47 [PC] (`-s _sysprof MultiEVTAResnet18Test` 출력 붙여넣기: VTA2 표가 빠지고 single로 넘어감)**
- 중요: 4개 스레드가 동시에 찍어 한 줄이 `100VTA_SYNC 62 ...`가 되어 VTA2 이벤트가 499개로 세어졌다 → vtalib 표 출력을 mutex로 묶고 이벤트 줄에 EVTA 번호를 붙였다.
- `multi_threads_main.cpp`: copy마다 `MULTI_RUNS`번(기본 25, `-n`), run마다 barrier로 같이 출발. `single_thread_main.cpp`는 4 × `MULTI_RUNS`번(`5b1559e8`, `40ee8dc4`). VTA0~3 모두 25 runs, 29.81~29.85 ms(156 GOPS).
- 4 × 156 = 623 GOPS를 단순히 더하는 것은 억지라서, 실제 겹친 시간(busy union = EVTA가 하나라도 일한 시간)으로 합친 GOPS를 계산했다. run 시작을 맞춰도 conv 사이 CPU 연산 차이로 시작이 6.7 ms쯤 벌어진다.

  | | busy union | 평균 동시 EVTA 수 | 4개 합친 GOPS |
  |---|---|---|---|
  | run 2 | 66.7 ms | 1.79개 | 278.8 |
  | run 25 | 50.8 ms | 2.35개 | **366.2** |

  conv0만 530 GOPS 가까이, 나머지는 110~260 GOPS. 100회 처리량은 multi 4.0 s, single 14.9 s로 3.7배다.

**15:57 [PC] "좋아. 넣어보자." (lockstep)**
- `run_prof.sh -l` = `MULTI_LOCKSTEP=1`. `timed_bundle.py`가 EVTA 호출마다 `evta_lockstep_hook()`를 넣고 main이 그 hook에서 4개 스레드를 barrier로 만나게 한다(`_prof`/`_timed` 계열 빌드만)(`a245e014`).
- 결과(systolic 400, run 25). 모두 281이다.

  | conv | 시작 시각 차이 | 4개 합친 GOPS |
  |---|---|---|
  | stage4 3×3 | 0.007 ms | **745~753** (최대 4 × 204.8 = 819의 92 %) |
  | stage3 3×3 | 0.002~0.005 ms | 729~737 |
  | stage2 3×3 | 0.001 ms | 676~688 |
  | stage1 3×3 | 0.001~0.007 ms | 602~612 |
  | conv0 | 0.019 ms | 544 |
  | 1×1 downsample | 0.001 ms | 147~182 |
  | **추론 전체 (busy union 30.1 ms)** | | **617.2 GOPS, 평균 3.98개 동시** |

- EVTA마다 conv 20개 합이 29.9~30.0 ms로 혼자일 때(29.75 ms)와 거의 같다. run 2는 VTA0 run 2가 40.0 ms로 튀어 424 GOPS였다(원인 미확인). 100회 처리량은 4072 ms로 lockstep 없을 때(4038 ms)와 같다.

**16:04 [PC] (사용자가 `-l`로 돌린 출력 붙여넣기: 마지막 `sum` 501.91 GOPS)**
- 성능 저하가 아니라 마지막 run이 흔들린 run이었다. 25개 run의 busy union GOPS는 다음과 같고 19개가 617, 중앙값 **616.9 GOPS**다.
  ```
  566 437 618 499 617 617 617 617 617 617 617 516 616 477 617 617 617 617 617 617 617 617 617 617 558
  ```
- `sum` 줄 501.91은 conv별 "첫 시작~마지막 끝" 합으로 계산한 값이라 어긋남에 크게 깎인다(run 25 busy union은 558).
- 흔들린 run에서는 EVTA 하나의 conv 하나가 2.7~4.1 ms 길어진다. 보드 커널이 `CONFIG_HZ=250`(tick 4 ms), `CONFIG_PREEMPT_NONE`이라, 폴링 스레드가 tick 하나 동안 CPU를 못 받아 완료를 늦게 알아챈 것으로 추정한다. 확실히 가르려면 PROFILE=1 비트스트림이 필요하다.
- summary 줄에 run 전체 중앙값을 찍게 했다: `median 616.94 GOPS (min 436.7, max 617.6, 25 runs)`.

**16:08 [PC] "chrt -f 10 시험해보자. 실험을 돌릴 땐 다른 proc 보다 우선해야 하니까."**
- `run_prof.sh -r` = `chrt -f 10`으로 실행(`sched_rt_runtime_us` 950000이라 다른 프로세스에 5 % 남음). lockstep 상태에서 일반 우선순위와 `-r`을 번갈아 두 번씩 돌렸다. 모두 281이다.

  | | 600 GOPS 미만 run (25개 중) | 가장 낮은 run | 중앙값 | 100회 전체 시간 |
  |---|---|---|---|---|
  | 일반 #1 | 9개 | 338.5 | 616.7 | 4187 ms |
  | **`-r` #1** | **1개** | 390.4 | 617.2 | 4106 ms |
  | 일반 #2 | 7개 | 404.3 | 617.1 | 4134 ms |
  | **`-r` #2** | **0개** | **608.7** | 617.2 | 4146 ms |

- 중요: 흔들림은 다른 프로세스가 폴링 스레드의 코어를 가져가던 것이 원인이었다. 최고값(중앙값 617)은 그대로다. `-r` #1의 390 한 번은 원인 미확인(`c56d0d5d`, `feb3906`).

**16:15 [PC] "PC에서 바꾼 건 push 해서 보드에서 pull … 다 맞추고 있어? 문서도 다 update 했어?"**
- PC에서 커밋·push, 보드는 pull만 했다(scp 안 씀). vtalib은 `nestc2_vtalib`에서 따로 push, 보드 `vta/vtalib` 안에서 pull. PC·origin·보드 모두 nest-compiler `1b940cd`, vtalib `4295aab`, 커밋 안 된 변경 없음.
- `CLAUDE.md`에 측정 방법(`run_prof.sh` suffix, `-n/-l/-r`, multi `OMP_NUM_THREADS=1`)과 conv0 EVTA 내용을, README에 `-r` 설명을 넣었고, `evta_runtime_aot.md`의 영어 단락을 한국어로 고쳤다(`aaa4d089`, `1b940cd`).

**16:24 [PC] "여러 문서에 명령에 대해 설명을 넣었는데 그 설명들 아래 예) 라고 한가지 example을 넣어줘. 왠만하면 모든 옵션을 다 넣어서."**
- `CLAUDE.md`, `vtabundles_xcmpl/README.md`, `docs/evta_runtime_aot.md`, `run_prof.sh` 3개 사본, `prof_summary.py`·`timed_bundle.py`·`daily_report.sh` 머리말에 전체 옵션 예를 넣었다. sudo 예는 모두 `sudo /bin/sh -c "..."` 꼴(`35100a4`).
- 보드에서 jinhu의 커밋 안 된 변경을 발견했다(`vtabundles_xcmpl/CMakeLists.txt`의 `#### jinhu added (2026-10-29)` 블록 = `MultiEVTAResConv1to4Test`, `run_all.sh` 2개, `make_multi4mix.py`, `multi4mixMain.cpp`, `*.bak_jinhu_20261029`). 손대지 않았다. 다음 pull 때 충돌할 수 있다.

**16:31 [PC] "내 dir에 있는 파일들 dir 들은 그룹네 다른 유저들에게 read, copy만 되게하고 write, execute은 못하게 하자."**
- `/home/xilinx/JO` 아래 파일 45,099개(그룹/다른 사용자 `r--`)와 dir 10,182개(`r-x`, 들어가야 읽을 수 있음)의 권한을 바꿨다. root 소유 `*.log.sum` 등 69개를 xilinx 소유로 돌렸고, `run_prof.sh`가 umask 022로 돌며 `.sum`을 dir 주인 소유·644로 만들게 했다(`0fec3d5a`).
- 중요: 이것만으로는 막지 못한다. jinhu·songmi도 `sudo` 그룹이고, jinhu 파일들도 xilinx 소유로 쓰였다(xilinx 계정 세션이나 sudo). 실제로 막힌 것은 우리 실행 파일의 직접 실행뿐이다.
- 기본 umask 002라 새 파일은 다시 느슨해진다. 셸 시작 파일 수정은 권한 확인에서 막혀, 사용자가 직접 실행할 `sed` 명령과 재조정용 `find ... chmod` 명령을 안내했다.

**16:54 [PC] "systolic GEMM 사이즈를 키우면, 32x32, 64x64 등, 성능이 좋아지겠지. 근데 이런 runtime과 driver 방식 … 다른 방식과 비교해서 좋은 편이야? …" / 17:00 "좀 자세히 써줘"**
- 지난 조사(`docs/evta_systolic.md` §9)는 하드웨어 어레이 중심이었고 runtime·제어 방식 비교는 하지 않았다. `docs/evta_runtime_compare.md`를 새로 썼다(`5527a330`).
- Resnet18 한 번(single, OpenMP 4스레드, 73.6 ms)의 시간 분해다.

  | 항목 | 시간 | 비율 |
  |---|---|---|
  | EVTA가 실제로 계산 | 29.7 ms | 40 % |
  | 층마다 하는 layout transpose (CPU) | 22.1 ms | 30 % |
  | elemadd, relu, maxpool (CPU) | 10.4 ms | 14 % |
  | conv마다 버퍼 할당·해제·flush | 5.0 ms | 7 % |
  | conv마다 명령어 생성 | 2.5 ms | 3 % |

  OpenMP 1스레드(149 ms)에서는 EVTA 32.2 ms(22 %), transpose 약 66 ms(44 %), maxpool 12.8 ms, elemadd·relu 약 25 ms, fc·quantize 등 약 8 ms다.
- 중요: DAE(fetch/load/compute/store + token) 자체는 좋은 편이다(큰 3×3 conv 93 %, 4개 복제해도 시간 그대로). 약점은 "conv마다 함수 하나, 동기화 한 번, layout 왕복 한 번"이라는 runtime·backend 단위에서 온다. 원래 VTA+TVM은 `graph_pack`으로 layout을 구간 입출구에서만 바꿔 transpose 22 ms가 없다 → NEST-C backend가 만든 문제다.
- 다른 설계 비교(보고서 원문 요약).

  | | EVTA (지금) | Xilinx DPU | Gemmini | NVDLA |
  |---|---|---|---|---|
  | 명령어를 만드는 때 | 추론 중 CPU가 conv마다 | 컴파일할 때 네트워크 전체를 미리 | CPU는 층 단위 명령만, 하드웨어가 안에서 펼침 | CPU가 층마다 설정 레지스터만 씀 |
  | 동기화 | conv마다 CPU가 완료를 폴링 | 네트워크 끝에 한 번 | 하드웨어가 의존성 추적 | 층 단위 인터럽트 |
  | 층 사이 데이터 layout | conv마다 CPU transpose | 가속기 형식 그대로 유지 | scratchpad 형식 유지 | 가속기 형식 유지 |
  | pool·add·relu | CPU | 가속기 | 일부 가속기 | 가속기 파이프라인에 붙임 |

- GEMM 32×32: EVTA 29.7 → 약 12 ms(2.5배, stage4 weight 대역과 conv0 0 채움 때문에 4배 안 됨), 추론 73.6 → 약 56 ms(−24 %). 64×64는 DSP 2,520개에 들어가지 않는다. 권장 순서(측정값에서 항목을 빼서 계산한 추정)는 다음과 같다.

  | 순서 | 할 일 | 하드웨어 변경 | 예상 추론 |
  |---|---|---|---|
  | ① | 층 사이 EVTA layout 유지 (transpose, 버퍼 할당 제거) | 없음 | 약 53 ms |
  | ② | elemadd, relu, maxpool을 EVTA ALU로 (지금 2비트 opcode의 ADD, MAX로 가능) | 없음 | 약 44 ms |
  | ③ | 명령 AOT, 여러 conv를 한 번에 제출 | 없음 | 약 41 ms |
  | ④ | conv0 입력 처리 개선 | 없음 | 약 36 ms |
  | ⑤ | GEMM 32×32 | 큼 | 약 19 ms |

  같은 32×32라도 지금은 −24 %, ①~④ 뒤에는 −47 %다. 미확인: DPU 층 사이 형식·내부 의존성, TPU v1·Tensil 원문, TVM-VTA residual add 위치, 32×32 예상의 대역(실효 4 GB/s)·클럭 가정.

**19:13 [PC] "EK-SCU35-G 에 evta를 넣고 runtime으로 실행하려면 하드웨어 사이즈랑 SW 돌리는 거랑 많이 어려울까?"**
- 지금 EVTA는 들어가지 않는다. EVTA 수치는 저장소 utilization report, SU35P 수치는 웹 출처다.

  | 자원 | 지금 single EVTA (보드 380 MHz 빌드 기준) | SU35P (EK-SCU35-G) | 필요량 대비 |
  |---|---|---|---|
  | CLB LUT | 약 26K | 16K | 1.6배 부족 |
  | FF | 약 36K | 33K | 거의 다 씀 |
  | DSP | 약 268 | 48 | 5.6배 부족 |
  | BRAM36 | 258 (약 9 Mb) | 48개 (on-chip 전체 1.93 Mb) | 5배 부족 |

- GEMM·ALU 시간 분할(II 8, MAC 32개), 버퍼 축소로 들어갈 수도 있으나 LUT는 합성 전에는 장담할 수 없고, 최대 약 12.8 GOPS다. CPU가 없어 MicroBlaze V baremetal, driver 새로 작성, HyperRAM 8 MB라 Resnet18(weight 11.7 MB) 불가, Vivado 2020.1 미지원. 결론: 사실상 새 포팅. 흐름 그대로라면 Kria KV260이 가깝다(K26 수치 미확인).
- 19:22: 보드 `git pull`이 GitHub ssh timeout으로 실패해 보드는 `0fec3d5`에 있다. 빠진 것은 문서 커밋 `5527a330`뿐이다.

**19:26 [PC] "scu200p 에는 들어갈까? 가격은 훨씬 비싸겠지만."**
- single EVTA 하나는 들어갈 것으로 본다(합성으로 확인하지 않음).

  | 자원 | single EVTA (16_16_19_18) | 버퍼를 줄인 16_15_18_17 | SU200P |
  |---|---|---|---|
  | CLB LUT | 약 26K | 약 26K | 100K |
  | FF | 약 36K | 약 36K | 200K |
  | DSP | 약 268 | 약 268 | **384** |
  | BRAM36 | 258 | **약 139** (multi4 556 ÷ 4) | **192** |
  | UltraRAM (288Kb) | 0 | 0 | 64 |

- 권장: multi 버퍼 설정 16_15_18_17을 그대로 쓴다(SW 거의 그대로). EVTA 2개는 DSP 부족. LPDDR4X/5 hard controller 2개. CPU가 없어 runtime은 A. PCIe Gen4 x8 host(권장, PCIe DMA driver 새로) 또는 B. MicroBlaze V Linux. 미확인: 가격, LPDDR4X 용량, PCIe 카드 형태, speed grade별 클럭, MicroBlaze V Linux 지원 범위.

**10-30 01:04 [PC] "kria260 에 있는 mpsoc 보다 싸고 LUT 수가 작은 mpsoc에 넣을 수 있을까?"**
- Kria K24(ZU3급, SOM $250)에는 16_15_18_17 설정으로 EVTA 하나가 들어갈 것으로 본다(합성 미확인).

  | 자원 | EVTA 16_16_19_18 | EVTA 16_15_18_17 | ZU1 | ZU2 | **K24 (ZU3급)** | K26 (ZU5급) |
  |---|---|---|---|---|---|---|
  | LUT | 약 26K | 약 26K | (못 찾음) | 47K | **70.5K** | 117K |
  | DSP | 약 268 | 약 268 | 216 | 240 | **360** | 1,248 |
  | BRAM36 | 258 | 약 139 | 108 | 150 | **216** | 144 + URAM 64 |

- ZU2는 DSP가 조금 모자라 packing 등 HLS 수정이 필요하고, ZU1은 권하지 않는다. K24는 PS8이 같아 RTL·backend·bundle·JIT는 거의 그대로이고, 가장 큰 SW 일은 `pynq_driver`의 `libcma`(xlnk)를 u-dma-buf/dma-heap으로 바꾸는 것이다. 300 MHz면 약 120 GOPS(ZCU102 380 MHz의 약 80 %)로 예상한다. KD240 kit $399, KV260 kit 가격은 미확인.
- 다음 단계로 Vivado 2020.1에서 XCZU3EG로 16_15_18_17 EVTA 합성을 제안했다.

## 결정이 남은 것 (10-30 07:00 기준)
- 다른 사용자도 걸리는 EVTA 잠금: vtalib `flock`(1) + `fuser /dev/xlnk` 감지(2)로 진행할지. 다른 사용자들이 vtalib을 어디서 받는지 알려 주셔야 한다.
- jinhu의 보드 커밋 안 된 변경(`vtabundles_xcmpl/CMakeLists.txt`, `run_all.sh` 2개, 새 파일들): 그쪽에서 커밋·push해 달라고 전달할지. 다음 pull 때 충돌 가능.
- 로그인 셸 umask 022: 사용자가 보드에서 안내한 `sed` 명령을 직접 실행해야 한다.
- 보드 `git pull --ff-only`: GitHub ssh timeout으로 보드는 `0fec3d5`(문서 커밋 `5527a330` 빠짐).
- 다음 방향: `docs/evta_runtime_compare.md` 권장 순서 ①~⑤(layout 유지 → ALU 이동 → AOT → conv0 → 32×32) 중 무엇부터 할지.
- 저가 보드: SCU35/SCU200 내용을 `docs/`에 문서로 정리할지, XCZU3EG로 16_15_18_17 EVTA 합성을 해 볼지.
- 미정 옵션: multi4 `_systimed` summary에 copy별 EVTA/CPU 분리를 넣을지, single `_sys`(측정 코드 없음) 빌드를 만들지.
- 이미 결정된 것: `Resnet18PartitionTest`는 conv0 float 유지. conv0 schedule은 가상 스레드 2, tile 8×16(112의 약수 tile만). `systolic` branch와 `/home/xilinx/JO/npuprg/systolic/` 삭제. 측정은 multi `OMP_NUM_THREADS=1`, single 기본값, 실험은 `-r`(`chrt -f 10`)로 우선 실행, multi4 합친 GOPS는 busy union 중앙값으로 본다.
