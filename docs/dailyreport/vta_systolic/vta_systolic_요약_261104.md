# 2026-11-04 EVTA systolic 요약: bias reset(single·multi4) 보드 통과, requantize(acc→out) single 23.05 ms, multi4 rq 400 MHz 610.4 GOPS

- 기간: 2026-11-04 07:00 ~ 11-05 07:00 (KST)
- 이날은 세션 하나뿐이다. [PC]는 PC `main`에서 한 작업이다. 보드 실험과 보드 쪽 git pull은 이 PC에서 ssh로 했다. 사용자가 보드에서 직접 돌린 것은 따로 적었다.

## 시간순

**07:51 [PC] "bias reset 돌리고 있어?" / 08:01 "_sysprof runtime 빌드하는데 default target이 아니라 hw_bundles로 빌드한다는 게 뭐야? default를 바꿔놓아야 맞지 않아?" / 08:14 "single에 acc 쓰기 전 reg는 안 넣는 게 나을까? EXCLUDE_FROM_ALL을 바꾸면 어떻게 되는 거야?"**
- **acc 쓰기 전 register:** single 소스(`vta-hw-zcu102-systolic`)에는 이미 들어가 있다. multi4와 같은 `vta.cc`로 `f7df508c`에 커밋했다. 빠진 것은 single bitstream뿐이라 bias single 빌드에 자동으로 들어간다.
  - 넣은 채로 두기를 권했다. single `alu1_epr`이 WNS +0.001로 겨우 통과했고 실패 경로가 이 acc 쓰기 쪽이었다. 비용은 ALU 명령당 1~2 clk, Resnet18에서 수 µs(0.1 % 미만)다.
- **기본 target (`b65b5666`):** 실행 파일이 모두 `EXCLUDE_FROM_ALL`이라 그냥 `make`는 아무것도 빌드하지 않고 오류 없이 끝났다. `add_custom_target(hw_bundles ALL)`로 바꿔 `make` = `make hw_bundles`가 됐다.
  - 중요: 바꾼 이유는 `_sysprof`가 11-01 실행 파일로 남아 측정이 죽은 일이다. `make -j2`가 오류 없이 끝나 재빌드된 것처럼 보였지만 실제로는 아무것도 하지 않았다.
- bias reset 진행: RTL(`sa_rmw`, `sa_ctrl`, `gemm_sa`, `gemm_sa_bd`, uop 폭), C 모델(random 300/300), ISA `bias_reset` 비트(hw_spec 3곳), HLS, vtalib(`cmd_queue.h`, `conv_kernel`, 이름에 `bias`가 있는 bitstream에서만, `EVTA_BIAS_RESET`으로 강제). RTL 시뮬레이션은 `xvlog`가 CPU 100 %로 멈춰 아직이었다.

**08:19 [PC] "RTL 시뮬레이션 돌리고 있어?" / 08:34 "하고 있어?"**
- `xvlog`가 멈춘 원인은 `build/rtl_sim`에 남은 옛 `xsim.dir`이었다. 이름을 바꿔 치우니 정상으로 돌았다.
- RTL 시뮬레이션 결과다. acc와 out을 C 모델과 bit 단위로 비교했고 token 수도 확인했다.

  | vector | 내용 | 결과 |
  |---|---|---|
  | bias_10, bias_3, bias_6 | bias reset uop 74~160개 섞음 | PASS |
  | biashazard_4 | 직전에 쓰인 dst를 bias src로 읽음 | PASS |
  | hazard_1, same_1, reload_1, conv_1 | 기존 경우 회귀 | PASS |

- 이어서 bias 비율을 높인 hazard 등 vector 48개를 그대로 한 번, GAP/BP 30 %로 한 번 더 돌려 **96/96 PASS**였다(처음 46개라고 한 것은 48개로 정정).
  - "쓰는 중인 bias를 바로 다시 읽는" forwarding은 vector당 0~1번만 나왔다. vtalib에서 bias는 GEMM이 쓰지 않는 영역에 있어 실제로는 일어나지 않으므로 이 정도로 충분하다고 봤다.
- single HLS CSim 오류 0. GEMM uop loop II=1, 깊이 5, uop stream 48비트가 RTL과 맞다. CSim(`vta_test.cc`)은 bias 경로를 쓰지 않아 회귀 확인용이다.
- 커밋 `76aa60e0`(보드 미확인이라고 메시지에 적음), 보드 pull. single bias bitstream 빌드를 시작했다(`FREQ=400 IMPL_STRATEGY=Performance_ExplorePostRoutePhysOpt BUILD_TAG=bias`).

**09:00~09:01 [PC] 새 vtalib, bias 끈 상태의 회귀 확인**
- 보드에서 single `_systimed`/`_sysprof`를 새 vtalib로 다시 빌드했다(rc=0). 지금 bitstream `alu1_epr`(이름에 `bias` 없음 → bias reset 꺼짐)에서 돌렸다.
- golden 30 PASS, golden 없는 5개(Resnet18, Resnet50, Resnet18Partition, Resnetv10S1C0, VGG16) exit 0.
- Resnet18 1회 27.42 ms, EVTA 25.22 ms. `-n 2`라서 전날 25회 측정(24.89 ms)과는 조건이 다르다.

**09:37~09:41 [PC] single bias bitstream: timing, 보드 검증, SYS_BIN 전환 (`ed468d49`)**
- 빌드 결과를 전 bitstream `alu1_epr`과 비교했다.

  | 항목 | single bias | 비교: `alu1_epr` |
  |---|---|---|
  | WNS | +0.000 ns (여유 0) | +0.001 |
  | WHS | +0.010 ns | +0.010 |
  | WPWS | +0.001 ns | +0.001 |
  | LUT | 34.3K | 34.1K |
  | BRAM / DSP | 258 / 273 | 같음 |
  | stuck cell | 0 beyond the base | — |

- 보드(`bit/evta_sys_bias_400MHz_pass_261104/`): probes 10/10, golden 30 PASS, 분류 전과 같음(Resnet18 281 / 0.655530, Resnet50 281 / 0.581379, Partition 281, VGG16 0 / 0.453709), layer dump Resnet18 40개·Resnet50 106개가 `alu1_epr`과 byte 단위로 같다.
- 시간: 같은 실행 파일, 같은 날, 25회.

  | 측정 | `alu1_epr` | `bias` |
  |---|---|---|
  | Resnet18 EVTA 시간 | 24.889 ms | **24.154 ms** (−0.735 ms) |
  | Resnet18 추론 (single, OMP 1) | 27.303 ms | **26.592 ms** |

- 기대치 0.91 ms(bias ADD 시간)보다 0.18 ms 덜 줄었다. 이때는 "bias LOAD가 남아서"라고 설명했는데, 10:48에 틀린 것으로 정정했다.
- 문서: `evta_runtime_compare.md` §7.16 신설, overview, CLAUDE.md, survey §5.3. multi4는 새 `m4_build.sh bias`로 빌드를 시작했다.

**10:47 [PC] "bias LOAD 가 왜 남아?" / 11:15 "측정하자."**
- 중요: **앞 설명이 틀렸다고 정정했다.** bias LOAD는 전과 개수·크기가 같고 위치만 GEMM 뒤에서 앞으로 옮겼다. 0.91 ms도 ALU ADD 반복 × 2 cycle만 센 값이라 LOAD는 들어 있지 않다.
- 측정: 같은 `bias` bitstream에서 `EVTA_BIAS_RESET=0`과 `1`로 Resnet18을 25회씩 돌려 conv별 EVTA 시간을 비교했다. `EVTA_MEMEXP`로 DRAM 전송을 모두 뺀 측정도 했다(결과값은 쓰레기, 시간만 본다).

  | conv | 실제로 줄어든 시간 | 전송을 모두 뺐을 때 | ADD 계산값 |
  |---|---|---|---|
  | conv0 (+ pool) | −0.316 ms | −0.292 | 0.28 |
  | stage1 (개당) | −0.064 ~ −0.068 | −0.063 | 0.063 |
  | stage2 (개당) | −0.019 ~ −0.035 | −0.033 | 0.031 |
  | stage3 (개당) | +0.001 ~ −0.010 | −0.016 | 0.016 |
  | stage4 (개당) | +0.002 ~ −0.004 | −0.008 | 0.008 |
  | 합 | **−0.760** | **−0.832** | 0.91 |

- 원인: stage3/4는 tile이 적어 메모리 전송이 시간을 정하고, tile 끝의 ADD는 다음 tile inp LOAD나 앞 tile STORE와 겹쳐 있었다. 전송을 빼면 모든 층이 계산값만큼 줄어들므로 bias reset HW와 LOAD 위치에는 손해가 없다. 0.91은 반복 × 2 cycle로 셈해 조금 크게 잡힌 값이다.
- §7.16 문장을 고쳤다(`1731e2c8`). multi4: `m4_build.sh`가 opt_design 뒤에서 impl_1을 의도대로 멈췄다. 로그의 `make bin rc=2`는 bin 변환 단계가 `.bit`를 못 찾은 것이라 무해하다.

**11:47~11:51 [PC] multi4 bias bitstream 보드 통과, SYS_BIN 전환 (`e9d90b70`)**
- 빌드(`rs_mbias`): WNS +0.037, WHS +0.010, WPWS +0.001(holdpin.tcl 불필요), Y219 행 PIP 0, 미배선 0, stuckchk 기준 대비 0. LUT 133K(49 %), BRAM 580, DSP 1092. HLS부터 bitstream까지 약 1.5시간(전에는 impl_1 route만 4시간 넘게).
- 보드(`bit/evta_multi4_bias_400MHz_pass_261104/`): probes 40/40, golden Conv4/ConvTest/ResConv10 PASS, Resnet18은 golden 없음(4개 동시·1개 모두 정상 종료), MultiEVTAResnet18 dump 80개가 `alu1`과 byte 단위로 같다.
- 시간: Resnet18 EVTA 시간 25회씩, 두 bitstream을 번갈아 두 번씩. 두 번 모두 ±0.02 ms 안에서 같았다.

  | Resnet18 EVTA 시간 | `alu1` | `bias` |
  |---|---|---|
  | 4개 동시 실행, EVTA마다 | 25.23–25.37 ms | **24.49–24.62 ms** |
  | 1개만 실행 (100회) | 24.97 ms | **24.22 ms** |

  single의 −0.735 ms와 거의 같다. Resnet18 로그의 "uop buffer overflowed" 경고는 10-29 로그에도 있던 것이다.
- `IMPL_TO_STEP`을 주면 Makefile이 bin 변환을 건너뛰게 고쳤다.

**12:40 [PC] "make만 해도 모든 bundle 빌드 유지하자. single acc reg 제안에 동의해. IMPL_TO_STEP ... rc=2 자세히 설명해줘."**
- 두 가지 결정됨: `make` = 모든 bundle 빌드 유지, single에도 acc 쓰기 전 register 유지.
- `rc=2` 설명: `bin: $(BIN_PATH)` → `$(BIN_PATH): $(BIT_PATH)`. `IMPL_TO_STEP=opt_design`이면 vivado.tcl이 `.bit` 없이 0으로 끝나고, make는 파일 생성을 확인하지 않아 bit2bin.py가 `FileNotFoundError`(exit 1) → GNU make rc 2. `m4_build.sh`는 rc가 아니라 `vta_wrapper_opt.dcp` 유무로 다음 단계를 정해 결과에 영향은 없었다.
- 고친 이유: 로그가 실패처럼 보이고, 진짜 실패도 rc 2라 구분이 안 된다. 이제 `IMPL_TO_STEP`이면 메시지만 찍고 0으로 끝난다. (16:15에 multi4 `_rq` 빌드에서 `make bin rc=0`으로 동작 확인.)

**12:51 [PC] "acc reg도 bias reset도 다 한거지? 더 해 볼게 뭐가 남았지?"**
- Resnet18 EVTA 24.15 ms의 구성(§7.14 측정에 이후 변경을 반영한 **추정**):

  | 덩어리 | 대략 | 비고 |
  |---|---|---|
  | GEMM | 18.85 ms | 이론 최소 17.7 ms(3628 Mops ÷ 204.8 GOPS)의 94 % 수준 |
  | ALU 남은 것 | 약 2.0 ms | imm 연산 약 1.5(II = 1), pool MAX 0.45(II = 2) |
  | 그 밖의 고정 비용 | 약 3 ms | conv0 tile 1.08, run마다 60~130 µs, 메모리에 묶인 층 |
  | CPU (추론 − EVTA) | 약 2.4 ms | 26.59 − 24.15 |

- 후보: 1) requantize(약 −1.5 ms 예상), 2) reg-reg ALU II = 1(약 −0.25 ms), 3) GEMM 확대(큰 작업), 4) run 사이 고정 비용(약 −1 ms), 5) conv0 tile·1×1 downsample 합치기, 6) 한 장을 EVTA 4개로(지연 약 26 → 8~10 ms 예상), 7) CPU 2.4 ms, 8) runtime·driver(HAL, DRM accel), 9) 다른 모델. 추천: EVTA 하나 속도는 1번, 추론 지연은 6번.

**13:11 [PC] "더 해볼 만한 것들을 문서에 넣어줘. requantize는 ISA 필드를 2bit에서 3bit으로 바꿔야 해?" / 13:24 "난 고친 적 없는데. 누가 고쳤지? overview 말고 다른 문서가 좋을 것 같은데."**
- 2비트 필드(`memory_type`, `alu_opcode`)는 건드리지 않는다. GEMM 명령 `pad_0`의 남은 31비트에 새 필드를 넣는다.
- 중요: **"STORE 길에 넣는다"는 앞 답변을 정정했다.** GEMM/ALU가 acc 하위 8비트를 out에 쓰므로(`sa_rmw.v` `out_wr_data`, `vta.cc` `accum.range(7, 0)`) STORE는 32비트 값을 못 본다. requantize는 acc → out 변환 자리에 넣어야 한다.
- "디스크에서 바뀜" 알림은 다른 사람이 아니라 Edit 뒤 python 스크립트로 고친 자기 수정이었다. 확인 없이 사용자에게 물은 것을 인정했다.
- 후보 목록은 `evta_overview.md` §5.1(`d1456d04`)에서 `evta_runtime_compare.md` §7.17로 옮기고, overview에는 요약과 링크만 남겼다(`a34c6fac`).

**13:29 [PC] "requantize를 acc - out 사이에 넣는 건 resnet18에만 쓰일까, VGG, mobilenet 등에도?"**
- int8 CNN의 conv/fc 출력마다 필요하므로 일반적으로 쓰인다. VGG16(conv, fc), Resnet50, conv0 + pool(requantize가 단조 증가라 max와 순서 무관)에 그대로 적용된다.
- 일반화가 필요한 곳: relu6을 위해 relu 1비트 대신 clip 하한·상한(int8 두 개). 필드는 shift 5 + 반올림 1 + 켜기 1 + clip 16 = 23비트. 2의 거듭제곱이 아닌 scale은 곱셈기(DSP 16개 정도)가 필요해 다음 단계로 둔다. residual의 순서와 depthwise conv의 EVTA 처리 여부는 **미확인**이다.

**13:36 [PC] "시작해보자. 필드를 shift + 반올림 + clip 하한·상한으로, GEMM과 ALU의 out 쓰기 두 곳에." / 13:41 "HLS, RTL도 바뀌는 거야?" / 13:54 "ISA 필드 문서를 따로 자세히. 이 문서만 보고도 어떻게 돌릴지 알 수 있게."**
- 14:19 진행: ISA 필드(hw_spec.h 3곳), HLS `vta.cc`, RTL `sa_ctrl.v`·`sa_rmw.v`, vtalib `VTARequant`(conv/pool/residual/fc 경로)를 구현했다. C 모델 300/300, RTL testbench 98/98, HLS CSim 0 errors. 커밋 `d26c3e30`, `2feb536`.
- 작업 중 고친 것:
  - 중요: vtalib `NextInsn()`과 HLS 시험용 `getGEMMInsn()`이 명령을 0으로 초기화하지 않아, 지금까지 pad 비트에 쓰레기 값이 들어갔을 수 있다. 새 필드가 그 자리에 들어가므로 0으로 초기화하게 고쳤다.
  - `vtalib-zcu102-systolic`의 `VTABundle.cpp`가 submodule `vta/vtalib`의 `runtime.h`를 include해서 보드 첫 빌드가 실패했다. 선언을 파일 안에 넣어 해결했다.
- 새 문서 `docs/evta_isa_ext.md`: 명령 비트 위치표, bitstream별 기능, 켜고 끄는 조건과 환경 변수, vtalib 사용처, 다른 모델 돌리는 순서, 새 필드 더하는 방법.

**14:58 [PC] "requantize 로직은 필요없을 땐 우회할 수 있게 넣었어? bit 필드에서 명령으로?"**
- `rq_en` 비트(GEMM 141, ALU 144)가 0이면 out은 전과 bit 단위로 같다. 남는 차이는 GEMM 완료가 3 cycle 늦어지는 것뿐이고, 측정에서는 보이지 않았다. 보드에서 `_rq` bitstream에 `EVTA_REQUANT=0`을 주니 층 출력이 `bias` bitstream과 같았다.
- **single `_rq`:** 정확도와 출력 동일성 검증(probes, golden 30, `bias`와 층 출력 비교, `EVTA_RES_CHECK=1`)을 통과해 single SYS_BIN으로 바꿨다(`8ff0e4b8`, `bit/evta_sys_rq_261104/`, stuckchk 기준 대비 0). 아래는 `bias` 대비 결과다(`rq`와 `bias`를 번갈아 측정).

  | Resnet18 single | `bias` | `rq` |
  |---|---|---|
  | EVTA 시간 | 24.15 ms | **23.05 ms** |
  | 추론 시간 | 26.61 ms | **25.49 ms** |

- 문서: `evta_isa_ext.md`, `evta_runtime_compare.md` §7.18, overview, CLAUDE.md.

**15:04 [PC] "acc 쓰기 reg, bias reset, requantize 등을 그림으로." / 15:11 "블록으로 단순화하지 말고 실제 SRAM, register, GEMM 구조로. 처음부터 해 온 시간 단축 작업들도 각각 그림으로, % 괄호로."**
- 처음 그린 블록 그림 16~19(`21252dd7`)는 지우고, 같은 EVTA/CPU 구조도 위에 이전/이후를 나란히 그린 단계별 그림 17개 `docs/figs/evta_st01`~`st17`로 바꿨다(`83e677e8`, `make_evta_struct_figs.py`). 초록 = 새로 생기거나 옮긴 것, 빨강 점선 = 없어진 것. PC 렌더러에 한글 글꼴이 없어 레이아웃만 확인했다.
- 그림에 넣은 단계별 시간(문서에서 모은 값):

  | 그림 | 단계 | 추론 시간 (single) |
  |---|---|---|
  | 1 | conv0을 CPU float에서 EVTA로 (OMP 4) | 131.8 → 78.6 ms (−40.4 %) |
  | 2 | 층 사이 EVTA layout 유지 | 148.7 → 91.1 (−38.7 %) |
  | 3 | residual을 EVTA로 | 90.93 → 67.13 (−26.2 %) |
  | 4 | maxpool을 EVTA로 | 67.13 → 52.48 (−21.8 %) |
  | 5 | layout 변환을 16바이트 복사로 | 52.48 → 46.90 (−10.6 %) |
  | 6 | 버퍼 pool | 46.97 → 44.27 (−5.7 %) |
  | 7 | CPU fc를 NEON으로 | 44.27 → 40.56 (−8.4 %) |
  | 8 | fc를 EVTA로 | 40.56 → 39.92 (−1.6 %) |
  | 9 | 전처리 합치기 | 39.92 → 37.08 (−7.1 %) |
  | 10 | 명령 기록/재생 | 37.08 → 34.50 (−7.0 %) |
  | 11 | conv0을 4×1로 접기 | 34.48 → 29.19 (−15.3 %) |
  | 12 | conv0 + pool을 한 호출로 | 29.19 → 28.83 (−1.2 %) |
  | 13 | systolic GEMM (HW) | EVTA 48.54 → 41.10 (−15.3 %) |
  | 14 | ALU II=1과 쓰기 앞 register (HW) | EVTA 26.42 → 24.89 (−5.8 %) |
  | 15 | bias reset (HW) | EVTA 24.89 → 24.15 (−3.0 %) |
  | 16 | requantize (HW) | EVTA 24.15 → 23.05 (−4.6 %) |
  | 17 | 전체 요약 막대그래프 | 148.7 → 25.49 ms (−83 %) |

**15:26 [PC] "기존 HLS evta 구조도를 그려보자. 전체, fetch, load, GEMM, store, interface와 버퍼, dep 연결도." / 16:10 "systolic evta 도 같은 순서로."**
- 새 문서 `docs/evta_hw_structure.md` 1장에 HLS EVTA 7장(`a549b0c5`): 전체, fetch, load, compute, store, interface와 버퍼, dependence. `vta.cc`, `vivado.tcl`, config의 크기를 그대로 옮겼다.
- st01~st17의 SRAM 크기 표기가 실제와 달라 inp 4096, wgt 2048, acc 4096, out 4096 entry로 고쳤다.
- 2장에 systolic EVTA 11장(`b2f9185a`): 전체, fetch, load, compute(HLS 쪽, uop 65비트 필드), gemm_sa, sa_array/PE(DSP48E2 내부), sa_ctrl(fill 17 clk 규칙), sa_rmw(S0~S6), store, interface, dependence. 바뀐 곳은 초록.

**17:21~19:05 [PC] multi4 `_rq` timing 수정 → 395.8 MHz판 보드 통과 (`4d411c19`)**
- multi4는 Y219 결함 행 때문에 RapidWright 재배선 흐름을 쓴다. 통과 조건은 WNS ≥ 0이고 Y219 행 PIP를 지나는 net 0개다(이전 보드 시험에서 행 안 PIP도 실패했기 때문). 단계별 과정:

  | 단계 | 한 일 | WNS (위반 endpoint) | Y219 행 PIP net | 비고 |
  |---|---|---|---|---|
  | `rs_mrq` | 재배선 흐름 결과 | −0.127 ns (115개), place 직후 −0.007 | 1개 | 최악 경로: `axi_xbar` grant → `load_0` 제어 레지스터 CE fanout. hold +0.006, HP 333 +0.004 |
  | `rs_mrqo` | 배선 뒤 `phys_opt_design`(`route_s.tcl opt`) | −0.007 ns (66개) | 1개 | RapidWright reroute는 EDIF를 안 만들어 돌지 않음 |
  | 2차 | EDIF 쓰고 RapidWright로 행 net 이동 → `opt2` | −0.007 ns (66개) | 0개 | 최악 경로: `store_queue_1` FIFO36 cascade RDCLK → RDEN, 로직 2단 1.768 ns |
  | (a) | MMCM 배수 12 → 11.875 (`mmcm_set.tcl`), 배치·배선 그대로 | 여유 약 +0.026 예상 → 통과 | 0개 | 395.8 MHz, EVTA 시간 약 1 % 증가 |
  | (b) | `phys_opt` AggressiveExplore 등 400 MHz 유지 시도 | — | — | 이후 placer 지시어 두 갈래 재구현으로 바뀜(21:01) |

- 395.8 MHz판 보드: probes 40/40, golden Conv4/Conv/ResConv10 PASS, Resnet18 conv dump 80개가 bias 판과 byte 단위로 같다.
- 시간: `_sysprof -n 25 -l -r`(4개 lockstep), bias 판과 비교해 2회 측정, 값은 거의 같았다. copy 3 값은 처음에 ssh가 끊겨 나중에 확인했다.

  | Resnet18 (4개 lockstep, runs 2–25 중앙값) | bias 400 MHz | rq 395.8 MHz |
  |---|---|---|
  | copy 0 / 1 / 2 / 3 EVTA 시간 | 24.49 / 24.62 / 24.62 / 24.51 ms | **23.65 / 23.77 / 23.77 / 23.66 ms** (−0.85 ms, −3.5 %) |
  | 25회 전체 시간 (`total_time`) | 706.4 ms | 684.0 ms (−22.4 ms, −3.2 %) |
  | 4개 동시 처리량 (busy union) | 583.8 GOPS | **605.1 GOPS** |

  single(−1.10 ms)보다 덜 준 것은 clock이 1 % 낮기 때문이다.
- LUT 133.3K → 164.1K(EVTA 하나에 약 7.7K). 400 MHz를 못 맞춘 것도 이 증가 때문으로 **추정**한다.
- `SYS_BIN` = `..._4evta_sa_396mhz_rq.bin`, 보드 dir `bit/evta_multi4_rq_396MHz_pass_261104`. y219 flow에 `VTA_PLACE_DIRECTIVE`, `RW_XMX` 추가(기본값 동일), 몇 ps 모자랄 때의 순서를 README에 적었다.
- 18:58경부터 이 PC → 보드 ssh(포트 22)가 자주 시간 초과로 끊겼다. ping 응답 TTL이 128이었다.

**19:07 [PC] "run_prof.sh -l -s _systimed 로는 왜 581.34GOPS 밖에 안 나오지? 넌 어떻게 계산해서 605GOPS가 나와?"**
- 사용자가 보드에서 직접 돌린 `_systimed` 결과(4개 lockstep, runs 2–25): copy별 conv 중앙값 24.739 / 24.786 / 24.805 / 24.753 ms, conv x4 busy union 581.34 GOPS, whole run x4 27.595 ms 525.91 GOPS. single 실행은 conv 중앙값 24.377 ms, 148.80 GOPS.
- 중요: 원인은 **그 실행이 bias bitstream으로 돌았기 때문**이다. `4d411c19` push 뒤 보드 pull이 ssh 시간 초과로 실패해 보드의 `SYS_BIN`이 아직 bias였다(`/tmp/evta_bitstream`, dmesg로 확인).
- 계산식은 같다: 3628.1 Mops × 4 copy = 14512.6 Mops ÷ 4개 중 하나라도 일하는 구간의 합집합 시간(rq 약 23.98 ms → 605.1, bias 약 24.88 ms → 583.8). `_systimed`는 conv 호출 앞뒤 CPU 시간까지 더해 `_sysprof`보다 조금 크다.

**19:18 [PC] "git pull 지금 했는데 멈춘 것 같아." / 19:21 ps, curl 결과 / 19:23 포트 443 pull 결과**
- 보드 remote가 ssh(`git@github.com`, 포트 22)인데 포트 22만 막혀 있었다. HTTPS는 `HTTP/2 200`으로 정상. 남아 있던 git 프로세스 12524~12527은 이 세션의 재시도 loop가 남긴 것이었다.
- `GIT_SSH_COMMAND="ssh -p 443" git pull git@ssh.github.com:joungheo/nestc2_clion.git main`으로 보드가 `4d411c1`까지 받았다. 이 우회 방법은 메모에 남겼다.

**19:28 [PC] "svg 파일들도 pull이 안 됐네. 보드를 껐다 켤까? run_prof.sh 출력 맨 앞에 어떤 bit 파일로 도는지, 옵션 설명도. --help도."**
- 재부팅은 필요 없다고 봤다. 원인은 **IP 충돌로 추정**한다. 끊겼을 때 `192.168.20.2` ping이 TTL 128(Windows 기본), 3~40 ms였고 지금은 TTL 64, 0.1 ms다. 같은 subnet이라 router가 없다. 다시 끊기면 `ip neigh show 192.168.20.2`의 MAC(지금 `64:79:f0:2e:f4:87`)이 바뀌는지 보면 확인된다.
- svg가 없던 것은 포트 22 때문이 아니라 그림 커밋 뒤 보드에서 pull하지 않았기 때문이다. 앞으로는 커밋마다 보드에서도 pull하기로 했다.
- `run_prof.sh`/`run_all.sh`/`load_bit.sh` 시작 시 bitstream(이름, 출처, 날짜, md5, EVTA 수, clock, GEMM, 기능)과 실행 설정을 출력하고 summary 맨 앞에도 넣었다(`bininfo.sh`). `-s`와 bitstream이 안 맞으면 WARNING, `--help`, 모르는 옵션은 오류. `collect_hw_bundles.py`의 `SYS_BINS`가 옛 bitstream이라 다시 만들면 되돌아갈 상태였던 것도 고쳤다.
- 보드에서 `-s _systimed -n 2 -l -r MultiEVTAConvTest`로 header 출력과 PASS, `-x` 오류를 확인했다. 커밋 번호는 `ad3d4b0d`가 아니라 `c2e69793`으로 정정했다. 불일치 WARNING은 보드에서 아직 실제로 보지 못했다. GitHub `Permission denied (publickey)`와 push 500 오류는 2분쯤 뒤 풀려 GitHub 쪽 일시 장애로 보았다.

**19:53 [PC] `--help` 출력 / 19:55 세 스크립트 `--help` / 19:56 "넣어줘."**
- 19:53 출력의 첫 줄이 섞인 것은 CLion 원격 터미널 초기화 명령과 입력이 화면에서 섞인 것이다. `--help`는 sudo 없이 된다.
- help 맨 앞에 그 dir의 실제 `BIN`/`SYS_BIN` 이름을 넣었다(`60a0eb7e`). `BIN=`을 주면 `(used instead)` 줄, `/lib/firmware`에 없으면 `(not in /lib/firmware)`. multi2는 `SYS_BIN`이 없어 `BIN`만. 생성기 `collect_hw_bundles.py`에도 같은 내용을 넣었다.

**19:58 [PC] "보드가 5 개 ahead of commit 이라는데. 보드에서 바꾼 게 PC로 안 온 게 있어?"**
- 없다. URL을 직접 주고 pull해서 `origin/main`이 `8ff0e4b`에 머문 표시였다. 5개(`21252dd7`, `83e677e8`, `a549b0c5`, `b2f9185a`, `4d411c19`)는 모두 PC에서 push한 것이다. 그 뒤 `origin`으로 pull해서 지금은 `main...origin/main`, ahead/behind 없음.

**21:01~21:59 [PC] multi4 `_rq` 400 MHz 재구현 → 보드 통과, SYS_BIN 전환 (`797a4ad8`)**
- 395.8 MHz판을 만든 뒤 400 MHz 유지를 위해 placer 지시어 두 갈래로 다시 place했다. 처음 결과는 둘 다 실패였다.

  | 후보 | 배치 방식 | Y219 행 우회 전 WNS | 우회 후 WNS (위반 경로 수) | 행 안에 경로가 지나는 net |
  |---|---|---|---|---|
  | p1 | ExtraTimingOpt | −0.009 ns | −0.120 ns (7개), hold도 −0.028 ns | 0개 |
  | p2 | ExtraNetDelay_high | +0.002 ns | −0.205 ns (20개) | 0개 |

- 그래서 395.8 MHz판과 같은 후처리(배선 후 phys_opt → 행 안 net 재우회 → phys_opt)를 다시 돌렸다. 최종 과정: ExtraNetDelay_high → 배선 뒤 phys_opt로 +0.002 ns, 행 안에 net 1개가 남음 → 그 net은 100 MHz 레지스터 쪽이라 타이밍 경로가 없어 RapidWright timing-driven 모드가 예외를 냄 → `Y219RWRoute.java`에 `-Dtd=0`(타이밍을 보지 않는 우회)을 추가해 다시 우회. ExtraTimingOpt는 −0.009 ns에서 더 줄지 않았다.
- Vivado 결과: WNS +0.002, WHS +0.005, WPWS +0.001 ns. Y219 행 net 0, 미배선 0, stuck cell bit 194개가 기준과 같고 벗어난 bit 0. `.bin` md5 `6a7b3df3…`.
- 보드: probes 40/40, golden 3개 PASS, Resnet18 dump 80개가 395.8 MHz판과 byte 단위로 같다.
- 시간: Resnet18 `_sysprof -n 25 -l -r`, 395.8 MHz판과 번갈아 두 번.

  | | 395.8 MHz | 400 MHz |
  |---|---|---|
  | copy별 EVTA 시간 | 23.64–23.76 ms | **23.44–23.56 ms** |
  | EVTA 4개 동시 | 604.8 GOPS | **610.4 GOPS** |
  | 25회 total_time | 683.3 ms | **678.6 ms** |

- `SYS_BIN` = `vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta_sa_400mhz_rq.bin`(`run_all.sh`, `collect_hw_bundles.py`). 문서(CLAUDE.md, §7.18, `evta_isa_ext.md`, overview, 구조 그림 16), y219route README. 보드 `bit/evta_multi4_rq_400MHz_pass_261104/`. 395.8 MHz판도 남겨 `BIN=`으로 쓸 수 있다. 보드도 pull해 `main...origin/main`.

## 결정이 남은 것 (11-05 07:00 기준)
- 사용자 답을 기다리는 것:
  - §7.17 후보 중 다음 방향: requantize는 끝났다. 남은 큰 갈래는 6번(한 장을 EVTA 4개로, 지연 감소)과 4번(run 사이 고정 비용) 등이다.
  - requantize의 채널별 곱셈기(2의 거듭제곱이 아닌 scale), residual 순서, depthwise conv 처리 여부는 미확인이고, 다음 단계로 남아 있다.
  - 보드 IP 충돌 추정: 같은 망에서 `192.168.20.2`를 쓰는 다른 장비를 찾을지.
- 이날 결정된 것:
  - `vtabundles_xcmpl`에서 `make` = 모든 bundle 빌드 유지. single에도 acc 쓰기 전 register 유지.
  - 기본 SYS_BIN: single `..._sa_400mhz_rq.bin`, multi4 `..._4evta_sa_400mhz_rq.bin`(395.8 MHz판은 `BIN=`으로 계속 사용 가능).
  - requantize 필드는 "shift + 반올림 + clip 하한·상한", GEMM과 ALU의 out 쓰기 두 곳에 넣는다(`rq_en = 0`이면 우회).
  - 후보 목록은 overview가 아니라 `evta_runtime_compare.md` §7.17에 둔다. 커밋할 때마다 보드에서도 pull한다.
