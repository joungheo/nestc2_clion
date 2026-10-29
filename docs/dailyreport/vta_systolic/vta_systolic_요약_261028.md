# 2026-10-28 EVTA systolic 요약: systolic 400 MHz 보드 통과(single +18%, multi4 709.6 GOPS), 기본 비트스트림을 380 MHz(HP 333)로 전환, conv0을 EVTA로 옮겨 Resnet18 추론 −40%

- 기간: 2026-10-28 07:00 ~ 10-29 07:00 (KST)
- [PC]는 PC의 `systolic` branch에서 한 작업이다(10-29 02:20 merge 뒤로는 `main`). 보드 실험도 이 세션에서 ssh로 돌렸다. [보드]는 보드 `main`을 맡던 vta_synth 세션이다.

## 시간순

**08:28 [보드] "systolic 쪽은 계속 하고 있어?"**
- 돌고 있는 작업은 없었다. 마지막 활동은 10-28 03:36 commit `ac1faeb0`였다.
- 밤사이 systolic commit은 3개였다: `bb1e13e9`(probe 통과), `f24a5504`(`gemm_sa`가 dependence token을 직접 보냄, 333에서 HLS와 같은 속도), `ac1faeb0`(HP 분리, BRAM mux select 레지스터, 380 보드 동작).
- systolic이 main보다 3개 앞서 있었고, main은 systolic보다 1개(`8d29663`) 앞서 있었다. 두 branch를 맞추자고 제안했다.

**08:48 [PC] "1번 먼저 해보자." (systolic 400 MHz 마무리)**
- 계획을 두 가지 바꿨다.
  - HLS 재합성은 하지 않는다. HLS IP가 이미 2 ns(500 MHz) 목표로 합성돼 있었다(`pkg_config.py`의 `fpga_per = 2`).
  - 중요: PS 레지스터 포트(HPM0)는 32비트로 바꾸지 않는다. 이 포트 폭은 비트스트림이 아니라 부트 이미지가 정하므로, 바꾸면 레지스터 접근이 깨지고 공유 보드가 멈출 수 있다. 이 변경으로 시작한 빌드는 보드에 올리기 전에 중단했다.
- 대신 PS와 xbar 사이에 폭 변환기 `reg_dwc`(128→32)를 넣어 100 MHz PS 클럭에서 돌게 했다(333 MHz 이하 빌드는 그대로).
- Makefile에 `IMPL_STRATEGY`, `BUILD_TAG` 옵션을 넣고 400 MHz를 두 전략으로 동시에 빌드했다.

**09:18 [PC] "문서 보기에 들어갔는데 dir가 바껴있는데 루트 dir로 어떻게 가?"**
- 명령 실행 중 `cd`가 남아 있던 것이라 저장소 루트로 되돌렸다. 직접 갈 때는 `! cd "$(git rev-parse --show-toplevel)"`를 쓴다.

**09:44 [PC] systolic 400 MHz single 보드 통과 (`22eae543`)**
- 기존 HLS GEMM은 400 MHz 타이밍을 통과하지 못했다. 그래서 systolic으로 400 MHz를 맞추려고 세 단계를 거쳤다.

  | 단계 | 한 일 | 이유 |
  |---|---|---|
  | 1 | BRAM mux 선택 신호를 레지스터 `sel`로 | 10-28 03:36의 첫 400/380 빌드가 조금 모자람 |
  | 2 | 레지스터 버스 폭 변환기를 PS 클럭 쪽(`reg_dwc`, 100 MHz)으로 | 400 실패 경로 112개가 `axi_xbar` 폭 변환기였고, 이것이 EVTA 클럭(400) 쪽에 있었음 |
  | 3 | Vivado 구현 전략 변경 | HLS compute/load 경로(로직 6~7단, 배선 70% 안팎) |

- 두 전략 모두 통과했다: `ExplorePostRoutePhysOpt` WNS +0.012, `ExtraTimingOpt` WNS +0.015(보드에 올린 판). WHS +0.010, WPWS +0.001, stuckchk 0 extra, Y219 행 PIP 0.
- 보드(`bit/evta_single_systolic_261027/..._sa_400mhz.bin`): gemmprobe 5, sramscan 4, donerace 16/16 통과(프로브 10/10). `reg_dwc` 경로로 레지스터 접근이 정상이었다. golden 30 PASS, Resnet18/Resnet18Partition/Resnet50 281.
- EVTA 시간: `_sysprof`로 쟀고, 같은 날 333 MHz HLS 기준판도 함께 쟀다.

  | 번들 | 333 MHz HLS | 400 MHz systolic | 시간 변화 | 속도 변화 |
  |---|---|---|---|---|
  | ResConv10 | 2.893 ms | 2.435 ms | −15.8% | +18.8% |
  | Conv4 | 3.564 ms | 3.008 ms | −15.6% | +18.5% |
  | Resnet18 | 48.54 ms | 41.10 ms | −15.3% | +18.1% |
  | Vgg16Cifar10 | 33.52 ms | 30.10 ms | −10.2% | +11.4% |
  | Resnet50 | 245.77 ms | 229.00 ms | −6.8% | +7.3% |

  클럭만으로 기대할 최대치는 +20%이고, conv와 Resnet18은 거의 그만큼 나왔다. Resnet50과 Vgg16은 HP 포트가 333 MHz에 묶여 load/store가 많은 층이 덜 빨라진 것으로 본다.
- 다음 후보: 1) multi4 적용, 2) HP 포트 대역, 3) C안. 권고는 1번.

**10:03 [PC] "1번 가자." (multi4 systolic 400 MHz)**
- multi4 트리에 HLS 소스(`vta.cc`, `vta.h`, `hls.tcl`)를 systolic 것으로 교체했고, RTL은 systolic 트리의 `rtl/`을 그대로 참조한다. `SYSTOLIC=1`이 아니면 기존과 같다.
- `vivado.tcl`: EVTA마다 `gemm_sa_<i>`, `acc_mem_<i>`(2048×512), 333 초과 시 `reg_dwc`. 기존 pblock 이름 규칙에 맞아 각 EVTA 영역에 자동으로 들어간다.
- HLS CSIM(16/15/18/17) 오류 0, BD 구성 오류 없음. commit `352dbd6a`. `make bin` 뒤 Y219 재배선(`run_flow.sh msa400`)이 자동으로 이어지게 했다.

**14:21 [PC] "PC가 갑자기 멈췄어. … 왜 멈췄는지 확인해봐." / 14:26 "JOBS를 4로 줄이고 빌드를 시작해줘."**
- 중요: journal이 11:10:28에서 끊겼고 shutdown 기록이 없다. 그때 multi4 빌드의 `route_design`(Phase 4)이 돌고 있었다. Vivado 메모리 최대 9.3 GB(여유 67 GB)라 OOM은 아니었고, 커널 로그에도 MCE나 lockup이 없었다.
- 10-25 14:05에도 multi4 Vivado 구현 중에 같은 일이 있었다. 장시간 전체 부하에서 로그 없이 멈추는 양상이라 하드웨어 안정성 문제(전원/VRM, 온도, PBO·XMP, 메모리)로 추정한다.
- 권장(sudo 필요, 하지 않음): BIOS PBO·XMP 끄기, memtest86, `lm-sensors`·sysstat 켜기.
- JOBS=4로 처음부터 다시 빌드했다. 죽은 빌드는 `m4_sa_400_crash1028.log`, `..._sa_400mhz_crash1028`로 보관했다.

**14:33~15:12 [PC] daily report 만들기 ("매 턴 요약…하루 단위로 문서", "시간 순서대로", "너무 길어…1-3p", "성능 측정한 표는 다 넣어줘", "타이밍 수정 과정 표도", "매일 오전 7시에 자동으로")**
- 10-27 보고서를 여러 번 고쳐 지금 형식으로 정했다: 요청 원문, 요약, "중요:", 성능 표는 모두 넣고 실험 설명 붙이기, 수정 과정 표, 결정이 남은 것.
- `docs/dailyreport/tools/`에 `daylog.py`, `prompt.md`, `daily_report.sh`를 만들고 crontab `0 7 * * *`에 걸었다. 빠진 날은 다음 실행 때 채운다. 10-26 보고서를 자동 경로로 만들어 시험했다.
- 중요: 자동 세션이 쓰기 권한이 없어 파일을 못 만들었다 → `--permission-mode acceptEdits`. 저장소 SessionEnd hook이 `git add -A`, commit, push를 하므로 자동 실행은 `disableAllHooks`로 돌린다. 첫 시험 때 hook이 로컬 commit `8b000f76 auto: ...`를 만들었다(push 안 됨).
- vta_synth 세션(`57e1dbd5`)용 `SET=vta_synth`를 추가해 `docs/dailyreport/vta_synth/vta_synth_요약_261019.md`를 만들었다. [보드] 세션도 같은 요청을 받고 이 파일을 확인했다.

**16:10 [PC] "daily report는 이 세션에서 매일 아침 7:00…섞여 있어도 괜찮아. … google TPU…NVDLA는…?"**
- daily report는 지금 설정(vta_systolic 하나, 모든 세션을 읽음)으로 둔다.
- TPU v1은 256×256 weight-stationary systolic으로 우리와 같은 구조이고, 크기만 우리 multi4의 64배다. RTL은 공개되지 않았다. 수치는 기억에 의존했고 원문은 다시 확인하지 않았다.
- NVDLA는 systolic이 아니라 broadcast + 덧셈 트리로, 원래 VTA HLS GEMM과 같은 방식이다. 빠른 이유는 MAC 수×ASIC 클럭, 고정 기능 파이프라인, direct conv, 적은 명령 부담이다. FPGA 포팅은 100~150 MHz 정도로 기억한다(미확인).
- 결론: systolic 선택은 맞다. 다음은 MAC 수 확대(BLOCK_IN 32), conv+bias+ReLU 융합, im2col·transpose의 HW 이전이다.

**16:24 [PC] "driver와 runtime 코드의 구조는 어떤 게 더 좋아? … unified low level API" / 16:38 "비교 문서도 만들어 보고 AOT로 바꾸는데 얼마나…"**
- VTA, NVDLA, Gemmini, TPU(PJRT), XRT, IREE HAL, TVM UMA, DRM accel을 비교했다. 층을 나눠 AOT executable, command buffer 제출, interrupt + fence, dma-buf, IREE HAL 모양의 공통 API를 쓰는 구조를 제안했다.
- `docs/evta_runtime_aot.md`를 썼다. 보드에 있던 기록으로 Resnet18(single, 333 HLS)의 시간 구성을 구했다. `_timed` conv 호출 시간에서 `_prof` VTA_SYNC를 뺐고, 10-26과 10-27 로그라 몇 % 오차가 있을 수 있다.

  | 항목 | ms | 비율 |
  |---|---:|---:|
  | conv0 (CPU float) | 76.65 | 56.9 % |
  | 기타 CPU 연산 (elemadd, maxpool, quantize, relu, fc …) | 15.88 | 11.8 % |
  | transpose (conv마다 두 번) | 12.31 | 9.1 % |
  | `VTABufferAlloc`/`Free` | 3.61 | 2.7 % |
  | EVTA 하드웨어 | 24.16 | 17.9 % |
  | **명령 생성 + 실행 시작 (JIT)** | **1.62** | **1.2 %** |

- 중요: 병목은 명령 생성이 아니라 EVTA 밖의 CPU 작업(80 % 이상)이다. 그래서 AOT가 가장 급한 일은 아니다. 명령 생성은 conv당 0.056~0.111 ms이고, multi4 ResConv10에서는 4.6 %다.
- HLS load/store는 base register + 상대 주소를 쓰므로 HW 수정 없이 위치 무관 명령이 가능하다(base≠0 동작은 보드 미확인). AOT 노력(추정):

  | 단계 | 바뀌는 곳 | 기간 | 얻는 것 |
  |---|---|---|---|
  | 0. 첫 실행 기록·재사용 | `cmd_queue.h`, `VTABundle.cpp` | 3~5일 | 1.6 ms → 수십 µs |
  | 1. 컴파일 때 명령 blob | `VTASave.cpp` 4곳, host용 vtalib, bundle 형식 | 2~4주 | 실행 중 명령 생성 없음 |
  | 2. HAL + 커널 드라이버 + 다층 제출 | 새 층 | 수개월 | sudo 없음, 다중 사용자, hang 복구, CPU·EVTA 겹치기 |

- 권하는 순서: conv0을 EVTA로 → activation을 VTA layout으로 유지 → AOT 1단계 → HAL.

**17:41 [PC] multi4 systolic 400 MHz 보드 통과, 709.6 GOPS**
- 재배선 결과 Y219 흐름 그대로 통과했다(위반 수정 단계는 필요 없었다).

  | 항목 | 값 |
  |---|---|
  | 타이밍 | WNS +0.002 / WHS +0.009 / WPWS +0.001 ns |
  | 배선 | 미배선 0, Y219 행 PIP 0, stuck cell에 걸린 PIP 0 |
  | stuckchk | 기준 대비 추가 0 |
  | 자원 | BRAM 580/886, DSP 1072/2448 |
  | 보드 위치 | `/lib/firmware/vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta_sa_400mhz.bin` |

  EVTA 4개 프로브 40개 통과.
- 첫 측정: 같은 날 세 비트스트림에서 옛 multi4 번들(`multi4resconv10Bundle 15/1 400 conv`, `OMP_NUM_THREADS=1`, 3회)로 쟀다.

  | 비트스트림 | golden | ResConv10 EVTA 1개 | EVTA 4개 동시 (run 1 / 2 / 3) |
  |---|---|---|---|
  | HLS 333 (`vtahw_multi4_y219`) | 모두 일치 | 1.516 ms, 152.4 GOPS | 583.8 / 609.5 / 584.5 GOPS |
  | HLS 380 | 모두 일치 | 1.348 ms, 171.6 GOPS | 652.8 / 684.9 / 685.3 GOPS |
  | **systolic 400** | 모두 일치 | **2.895 ms, 79.9 GOPS** | 301.9 / 319.2 / 314.6 GOPS |

  가끔 EVTA 하나가 4 % 정도 늦어져 그 run 값이 떨어진다(이전에도 본 편차).
- 중요: systolic이 2배 느린 원인은 HW가 아니라 runtime이었다. multi4 번들이 원래 uop 순서의 `vtalib-zcu102-multi4`로 빌드돼 있어 GEMM 명령마다 weight를 다시 실었다. 보드에서 systolic vtalib로 `build_multievtax4_sys`(접미사 `_sys`)를 다시 빌드했다.
- 다시 잰 결과: 두 비트스트림 모두 같은 `_sys` 번들로 쟀다. ResConv10을 EVTA마다 400회 돌렸다. golden(Conv4, Conv, ResConv10) PASS. Resnet18 두 개는 exit=0이지만 golden이 없어 정답은 확인하지 못했다.

  | 비트스트림 | EVTA 1개 (3회) | EVTA 4개 동시 (3회) |
  |---|---|---|
  | HLS 380 | 171.4 / 171.5 / 171.5 GOPS | 650.7 / 650.7 / 660.4 GOPS |
  | **systolic 400** | **179.3 / 179.7 / 179.8 GOPS** | **681.0 / 709.6 / 678.0 GOPS** |
  | 차이 | **+4.8 %** | 같은 날끼리 +3~9 %, 최고값끼리 +3.5 % |

  - EVTA 1개 +4.8 %는 클럭 비율(+5.3 %)과 거의 같다. HLS 333(152.4) 대비 +17.9 %로 single의 +18 %와 맞는다.
  - 오늘 HLS 380은 650~660 GOPS로 어제 685보다 낮았다(같은 날 옛 번들 run 2·3에서는 685). 이론 최대 4 × 179.8 = 719 GOPS이고, 709.6은 그 98.7 %다.

**18:04 [PC] "1번 진행 하고 2번에서는 vtalib 사본을 합치는 걸 생각해보자. … conv0는 원래 bnn 옵션이 있어서 …"**
- 보드 dir를 `bit/evta_multi4_systolic_400MHz_pass_261028/`로 바꾸고 README를 넣었다. 설계서 §12와 `docs/evta_runtime_aot.md`를 commit `13be04fc`로 push했다.
- vtalib 사본 비교: single/multi2/multi4는 `vta/vtalib`(`f716ed3`)과 같다. systolic만 3개 파일이 다르다(`VTABundle.cpp` WS uop 순서, `dat_queue.h`와 `runtime.cc` uop 캐시 버그 수정).
- 숨은 위험: 사본의 `VTABundle.cpp`가 submodule 헤더를 include한다. 사본마다 `include/`를 고치면 일부만 반영된다.
- conv0: vta_synth 세션에서 conv0을 EVTA로 돌린 기록은 찾지 못했다(10-25 제안, 10-26 CPU float 가속만). `VTASave.cpp:3108`의 `VTA_BNN` 분기 `saveFirstBNNVTAConvolutionInst`가 3채널 입력을 16채널로 0 채움해 일반 `convolution_wo_tr`를 부른다. 지금 conv0이 float인 것은 보드 캐시가 `VTA_RESNET18_WITH_SKIPQUANT0=ON`이기 때문이다.

**19:08 [PC] "1. 기본으로 도는 버전을 380MHz로…systolic 에서는 400MHz로 2. vtalib_systolic은 일단 유지…cmake 옵션이 달라지나? 3. SKIPQUANT0=OFF 해서 성능부터 보자."**
- 버퍼 크기 등은 파일이 아니라 cmake 옵션이 정한다. `-DBUNDLE_SET`만 주면 `vtabundles_xcmpl/CMakeLists.txt:42-60`이 FORCE로 정한다: single은 `NESTC_EVTA_MULTI` OFF와 32 MB, multievtax2는 ON과 8 MB, multievtax4는 ON과 4 MB. 그래서 `_sys` 빌드 때 따로 준 옵션은 필요 없었다.
- 사본을 참조하는 것은 보드의 multi4 빌드 dir 3개뿐이었다. single과 multi2는 이미 `vta/vtalib`를 썼다.
- SKIPQUANT0=OFF 측정: Glow 재빌드를 피하려고 보드 model-compiler를 skip 파일 없이 직접 돌렸다. 보드 single, `vtahw_single_y219`.

  | | SKIPQUANT0=ON (지금) | SKIPQUANT0=OFF |
  |---|---|---|
  | 분류 결과 | 281 | **281** (같음) |
  | conv0 | `convolutionFloat` 76.2 ms | `nonvtaconvolution`(CPU int8) **1153.7 ms** |
  | 전처리와 양자화 | 2.9 ms 전후 | 비슷함 |
  | 전체 추론 (timed) | 134.3 ms | **1210.3 ms** |
  | EVTA 시간 (`_prof`) | 48.37 ms | 48.38 ms |

  conv0 양자화로 정확도는 떨어지지 않았다(이미지 1장). 느려진 것은 int8 CPU 함수가 최적화되지 않았기 때문이다. OFF는 conv0을 EVTA로 보낼 때만 의미가 있다.

**22:28 [PC] 기본 비트스트림 380 MHz(HP 333) 전환 (`709b6164`, systolic에 push)**
- HP 규격을 지키는 HLS 380판은 multi4에만 있었다. single과 multi2의 기존 380판은 HP 포트도 380이라 규격 밖이다. 그래서 HP 분리판을 새로 빌드했다(JOBS=4).
  - single은 `vta-hw-zcu102-systolic`에서 `SYSTOLIC` 없이 빌드했다.
  - multi2는 `vta-hw-zcu102-multi4`에서 `NUM_EVTA=2`로 빌드했다.
- 첫 빌드는 setup이 모자랐고, 400 때 맞췄던 전략으로 구현만 다시 돌렸다(HLS IP는 그대로).

  | 판 | 첫 빌드 WNS | 실패 경로 | 재빌드 (`Performance_ExtraTimingOpt`) WNS / WHS / WPWS |
  |---|---|---|---|
  | single 380 | −0.043 ns (14개) | `reg_dwc` FIFO, SmartConnect | +0.041 / +0.010 / +0.001 |
  | multi2 380 | −0.023 ns (76개) | `axi_xbar` 클럭 변환기 FIFO, `hpslice1` | +0.009 / +0.010 / +0.001 |

  첫 빌드도 WHS +0.010, WPWS +0.001로, HP 333 분리는 의도대로 들어갔다. 실패는 GEMM이 아니라 배치에 따라 달라지는 AXI 인터커넥트였다.
- 보드 검증:

  | | single 380 | multi2 380 |
  |---|---|---|
  | stuckchk, Y219 PIP | 0 extra, 0 | 0 extra, 0 |
  | probes | 10/10 | 20/20 |
  | golden | 30 PASS. Resnet18, Partition, Resnet50 → 281, Vgg16 → 0 (이전과 같음) | 3 PASS. Resnet18 100회 모두 281 |
  | 보드 위치 | `bit/evta_single_380MHz_pass_261028/` | `bit/evta_multi2_380MHz_pass_261028/` (`..._2evta_380mhz_hp333.bin`) |

- EVTA 시간(같은 날 333 MHz와 비교):

  | 번들 | 333 MHz | 380 MHz | 변화 |
  |---|---|---|---|
  | single ResConv10 | 2.894 ms | 2.561 ms | −11.5 % |
  | single Conv4 | 3.547 ms | 3.134 ms | −11.6 % |
  | single Resnet18 | 48.38 ms | 42.94 ms | −11.2 % |
  | single Vgg16Cifar10 | 33.53 ms | 31.09 ms | −7.3 % |
  | single Resnet50 | 252.2 ms | 246.1 ms | −2.4 % |
  | multi2 Conv4 | 179.2 ms | 158.4 ms | −11.6 % |
  | multi2 Resnet18 ×2 | 1184.8 ms | 1053.5 ms | −11.1 % |

  Resnet50이 적게 준 것은 load/store가 HP 333에 묶이기 때문으로 본다.
- 기본값: single `..._16_16_19_18_380mhz.bin`(SYS_BIN `..._sa_400mhz.bin`), multi2 `..._2evta_380mhz_hp333.bin`(SYS_BIN 없음), multi4 `..._4evta_380mhz.bin`(SYS_BIN `..._4evta_sa_400mhz.bin`). `run_prof.sh -s _sys*`면 `SYS_BIN`을 쓰고, `BIN=`이 우선한다. `run_all.sh`/`run_prof.sh` 3개씩, `collect_hw_bundles.py`, CLAUDE.md, 문서에 반영했다.

**10-29 01:55 [PC] "vtalib를 빌드할 때 cmake 하면서 옵션을 다르게…multi 경우에 ddr addr 등을 다르게 해야하는 건…?"**
- 옵션은 빌드 dir마다 `CMakeCache.txt`에 남고, .a도 dir마다 그 옵션으로 컴파일돼 실행 파일에 static link된다. 그래서 single과 multi를 오갈 때 cmake를 다시 할 필요가 없다.
- EVTA 번호는 번들 소스를 만들 때 정해진다. 레지스터 주소(`0xA0000000 + i*0x10000`)와 DDR 버퍼(`cma_alloc`)는 실행할 때 runtime이 정한다. 따로 하는 단계는 없다. 제약은 CMA 128 MB 하나라서 `MAX_XFER`를 8/4 MB로 줄였다.

**10-29 02:05 [PC] "build_multievtax4는 왜 vtalib-zcu102-multi4를 참조했어? … 3개 dir는 없애도 되겠다."**
- 10-20에 hw dir과 vtalib 사본을 쌍으로 만들었고(`97d2146e`), 드라이버 변경을 사본에 먼저 넣었다. 10-26에 `vta/vtalib`가 따라왔지만(`401d9a87`, `f716ed3`) multi4 빌드 dir 캐시에 사본 경로가 남아 있었다.
- 보드 multi4 dir 3개를 `-DVTALIB=$N/vta/vtalib`로 다시 설정해 빌드했다(설정은 이전과 같음). 380 multi4판 `run_all.sh`에서 Conv4, ConvTest, ResConv10 PASS, Resnet18 두 개 exit 0.
- PC에서 사본 3개를 `git rm`했고, README, CLAUDE.md, 주석을 고쳤다. submodule `vta/vtalib/CMakeLists.txt` 주석은 그대로 두었다.

**10-29 02:18 [PC] "PC systolic branch에서 다 커밋, push 하고나서 main에 merge하자. … 보드에서 pull 해서 맞추자."**
- `d6277600`(사본 삭제), `a5169fc8`(daily report `SET=vta_synth`, 261019·261026)을 push했다. `8d296637`과 충돌 없이 merge해 `116cd122`로 `origin/main`에 push했다. PC 작업 트리는 이제 `main`이다.
- 보드도 pull해서 `116cd122`가 됐고, 남은 `vtalib-zcu102-multi4/config.mk`를 지웠다. 이제 기본 비트스트림 5개가 보드에 반영됐다. `systolic` branch는 지우지 않았다.

**10-29 02:24 [PC] "conv0를 EVTA에서 돌려보자"**
- `VTASave.cpp` `saveConvolutionInst`에 분기를 넣었다(`7d15263`). 대상은 EVTA 조건을 만족하는 3채널 int8 conv다(scale 2의 거듭제곱, offset 0, 출력 채널 16의 배수). 이 conv는 `VTA_BNN` 없이도 0 채움 경로 `saveFirstBNNVTAConvolutionInst`로 간다.
- 보드에서 `make VTA/fast`, `make model-compiler/fast`로 VTASave.cpp만 다시 컴파일했다(33초). 그냥 `make`는 cmake 재실행으로 Glow 전체가 다시 빌드된다.
- 결과: cat_285, 380 MHz single판에서 2회씩 돌렸고 두 번 같았다.

  | | conv0 float (기본) | conv0 int8, CPU (Q0) | conv0 int8, EVTA (Q0E) |
  |---|---|---|---|
  | 분류 / confidence | 281 / 0.588 | 281 / 0.533 | 281 / 0.656 |
  | conv0 | 76.3 ms | 1156 ms | 23.8 ms (아래 세 단계 합) |
  | 추론 전체 (`_timed`, run 2) | 131.8 ms | 1209.8 ms | **78.4 ms** |
  | EVTA 시간 (`_prof`, 추론 2회분) | 42.94 ms | 42.95 ms | 68.57 ms |

  Q0E conv0은 입력 0 채움(CPU) 6.7 ms, EVTA 13.5 ms, 출력 transpose(CPU) 3.5 ms로 나뉜다. EVTA 13.5 ms는 예상 약 8 ms보다 길다. 16채널 중 13채널이 0이라 실제 연산량(236 Mops) 기준 17.5 GOPS다.

**10-29 02:38 [PC] "confidence 차이, VTA_RESNET18_WITH_SKIPQUANT0을 OFF로 바꿔서 기존 번들도 돌릴 수 있게 하자. 문서도 diff 해봐." / 02:39 "confidence 차이도 비교해보자."**
- confidence 차이: 세 번들의 conv0 출력을 덤프해 비교했다. 기준은 float 출력을 int8 단위(×32)로 바꾼 값이다.

  | | 기준과의 차 평균 | 평균 \|차\| | 최대 \|차\| | 기준과 일치하는 비율 |
  |---|---|---|---|---|
  | Q0 (CPU `nonvtaconvolution`) | +0.015 | 0.315 | 3.77 | 반올림한 기준과 75.8 % |
  | Q0E (EVTA) | −0.340 | 0.402 | 3.93 | 내림한 기준과 77.2 % |

  중요: EVTA − CPU는 0(64.5 %) 아니면 −1(35.5 %)뿐이다. EVTA는 shift로 내림하고 CPU는 반올림하는 차이라서 계산 오류가 아니다. 어느 쪽이 더 정확한지는 이미지 한 장으로 말할 수 없다.
- 문서 diff: 다른 세션의 수정은 없었다. "디스크에서 바뀜" 알림은 이 세션 자신의 commit 때문이었다.
- 중요: 소스 기본값은 원래 OFF였고 보드 캐시만 ON이었다. 그런데 OFF로 바꿔도 conv0이 양자화되지 않았다. OFF 분기가 예전에 복사된 `./VTASkipQuantizeNodes.txt`를 지우지 않기 때문이다. CMakeLists 3개를 고쳤다(`f126a2b`): Resnet18Test는 OFF일 때 남은 목록을 지우고, x2·x4 번들은 옵션을 따르게 했다. Glow 빌드 10 % 시점에 발견해서 cmake를 한 번만 다시 돌렸다. 기존 번들은 `/home/xilinx/JO/skipq0_backup/`에 백업했다.

**10-29 04:00 [PC] 기존 Resnet18 번들 전부 conv0 EVTA로 (`8d9c82da`)**
- single·x2는 Glow 재빌드 뒤 `build_sim_bundles.sh`로 다시 만들었다. x4는 sim 빌드에 target이 없어서, copy 0/1은 x2에서 가져오고 copy 2/3은 model-compiler를 직접 돌려 만들었다. EVTA 번호 0~3이 각 copy에 맞게 들어간 것을 확인했다.
- 문제: `build_multievtax4` dir 3개는 cmake 캐시에 지운 `vtalib-zcu102-multi4`의 ACL 경로가 남아 링크가 깨졌다. 그 항목만 지우고 cmake를 다시 돌려 고쳤다.
- 보드(380 기본 비트스트림): golden은 single 30, multi2, multi4 모두 PASS다. Resnet18은 281(confidence 0.656), Partition과 Resnet50도 281이다. multi/single 100회 모두 281.

  | 번들 | EVTA 시간 전 | 후 | 추론 전체 |
  |---|---|---|---|
  | single Resnet18 | 42.94 ms | 68.56 ms | 131.8 → **78.6 ms** (−40 %) |
  | multi2 Resnet18 x2 | 1053.49 ms | 1719.76 ms | 100회 6.60 s |
  | multi4 multi | 516.57 ms | 937.76 ms | 100회 5.77 s |
  | multi4 single | 1157.39 ms | 1779.75 ms | 100회 4.08 s |

  EVTA 시간은 conv0을 EVTA가 맡게 된 만큼 늘었다. multi 번들의 예전 추론 전체 시간은 기록이 없어 전후를 비교하지 못했다.
- 그대로 둔 것: `Resnet18PartitionTest`는 conv0 float다(분할 계획이 그것을 전제로 함). systolic 빌드 dir 2개는 다시 빌드만 했고 systolic 비트스트림으로 실행하지는 않았다. 보드에 `Resnet18Q0ETest`와 `dump_*`가 남아 있다.

## 결정이 남은 것 (10-29 07:00 기준)
- `systolic` branch를 로컬과 origin에서 지울지(내용은 모두 `main`에 있음).
- `Resnet18PartitionTest`의 conv0도 EVTA로 옮길지(분할 계획을 새로 만들어야 함).
- 새 번들(conv0 EVTA)을 systolic 비트스트림(`_sys`)으로 확인할지.
- conv0 0 채움(CPU 6.7 ms)을 줄일지. 지금은 NCHW를 거치고 malloc을 두 번 한다.
- uop 캐시 버그 수정과 WS uop 순서를 `vta/vtalib`에 넣을지. 이번 결정은 "`vtalib-zcu102-systolic`은 일단 유지"다.
- AOT 문서의 다음 단계(layout 유지, AOT 0/1단계)를 할지.
- PC 멈춤: BIOS PBO·XMP 확인, 온도·sysstat 기록 켜기(사용자 조치).
- 이미 결정된 것:
  - 기본 비트스트림은 380 MHz(HP 333), systolic은 400 MHz다.
  - vtalib 사본 3개를 지웠다.
  - systolic을 main에 merge했다.
  - Resnet18 번들은 SKIPQUANT0=OFF로 conv0을 EVTA에서 돌린다.
  - daily report는 이 세션의 07:00 cron 하나로 만든다(vta_synth 내용도 섞여 들어감).
