# 2026-10-27 EVTA systolic 요약: multi4 380 MHz 685 GOPS(+12.4%), systolic GEMM 첫 보드 통과(380 MHz, HLS와 같은 속도), HP 포트 333 MHz 규격 발견

- 기간: 2026-10-27 07:00 ~ 10-28 07:00 (KST)
- [PC]는 PC의 `systolic` branch에서 한 작업이고, [보드]는 보드 `main`에서 한 작업이다.

## 시간순

**08:59 [PC] "중지된 작업 계속 해줘."**
- Phase A 빌드 5개(single, 클럭 sweep)는 이미 끝나 있었다.
- 조사 결과로 §8.5 설계를 개정했다.
  - acc는 출력 쪽 RMW로 처리한다(Gemmini 방식).
  - weight는 DSP B1 prefetch chain에 싣고, CEB1/CEB2를 wavefront로 켠다(UG579, arXiv 2409.03508).

**09:14 [PC] "기준판은 23일 16 bank판 아냐? 보드 검증 시작해줘." / 09:19 "왜 보드랑 PC가 다르지?"**
- 기준판을 10-23판(wgt 16 bank × 128비트)으로 정정했다.
- PC와 보드가 달라 보인 이유: git은 같았고, git이 무시하는 생성물 `hw/run_all.sh`의 PC 옛 사본만 달랐다.
- single 360 MHz 두 판(base, oreg)의 보드 검증. 프로브는 HW 기능 시험이고, golden은 번들 출력 비교다.

  | 검증 | base 360 | oreg 360 | 기준 333 |
  |---|---|---|---|
  | WNS | +0.011 | +0.041 | — |
  | stuckchk / Y219 PIP | 0 extra / 0 | 0 extra / 0 | — |
  | gemmprobe 5개, sramscan, donerace | 모두 통과 | 모두 통과 | 모두 통과 |
  | 번들 golden | 30 PASS | 30 PASS | — |
  | ResConv10, Conv4 VTA 시간 | −7.2% | −7.2% | 기준 |

  기대치는 클럭 비율 333→360의 −7.5%로, 거의 그만큼 빨라졌다. oreg는 base와 차이가 없다.

**09:35~12:01 [보드] hw dir 정리 (`run_all.sh` 등을 git에, 결과와 trace 분리)**
- 세 hw dir를 정리했다. 출력은 그대로 두고, git에는 `script/`만 들어간다.

  | dir | 내용 |
  |---|---|
  | `script/` | `run_all.sh`, `run_prof.sh` |
  | `result/` | 결과 txt |
  | `trace/` | `sds_trace_data.dat` |

- 비트스트림 로드 helper `hw/script/load_bit.sh`를 추가했다. PC와 보드를 같은 구조로 맞췄다(`f7230ea`, `3f0d49b`, `10be7ce`, `2a3db89`).

**09:46 [PC] Phase A 결론**

클럭 sweep: single을 클럭별로 빌드한 타이밍 결과(WNS, ns)다. oreg는 BRAM 출력 레지스터를 켠 변형이다.

| single, WNS ns | 333 | 360 | 380 | 400 |
|---|---|---|---|---|
| base(지금 구조) | +0.004 | +0.011, 보드 통과 | +0.007, 보드 통과 | −0.106 |
| oreg | +0.026 | +0.041 | −0.007 | −0.162 |

- 400 MHz 실패 경로 200개 중 169개가 GEMM 어레이 경로다. systolic 구조에서는 이 경로가 없어진다.
- 제안: 2) 지금 구조로 클럭 올리기, 1) systolic Phase B/C.

**09:49 [PC] "2와 1을 같이 진행하자."**
- **single 380 보드 측정:** `_prof` 비트스트림의 EVTA 시간(total_vta_time)이다. 프로브와 golden 30개도 통과했다.

  | 번들 | 333 MHz | 380 MHz | 변화 (클럭 비율 기대 −12.4%) |
  |---|---|---|---|
  | ResConv10 | 2.895 ms | 2.545 ms | −12.1% |
  | Conv4 | 3.547 ms | 3.124 ms | −11.9% |
- **Phase B (런타임 uop 순서를 weight-stationary로):** 보드 golden 30 PASS, 시간 ±0.5%.
  - 중요: 이 과정에서 **uop cache 버그 2개**를 찾았다. SRAM wrap 때 주소가 밀리고, sync 뒤에 "cached" 표시가 남는다.
  - 증상은 Resnet18 849, Resnet50 51이었고, 수정 후 281이다.
  - **원본 `vta/vtalib`와 사본 3개에도 같은 버그가 있다(미수정, 결정 대기).**
- **Phase B 보드 측정:** 333 HLS 비트스트림에서 옛 순서와 새 순서 런타임으로 35개 번들을 비교했다. golden 30 PASS, 분류값은 같다. 시간은 Resnet18 +0.3%, Resnet50 +0.2%, 나머지 ±0.5%로 거의 같다.
- **Phase C 사이클 모델:** systolic 어레이의 레지스터 수준 C++ 모델이다. 무작위 300개에서 비트 일치했다. 아래는 모델로 계산한 어레이 가동률이다.

  | 경우 | 옛 순서 | 새 순서, 명령마다 drain | 새 순서, drain 없음 |
  |---|---|---|---|
  | ResConv10형 (3x3, 7x7 tile) | 41.7% | 88.2% | 99.8% |
  | Conv4형 (3x3, 8x56 tile) | 49.7% | 98.6% | 99.9% |
  | 1x1 conv | 39.4% | 76.9% | 99.6% |

  drain을 하면 ResConv10이 지금 HLS(93.5%)보다 낮아진다. 그래서 명령 사이 drain 생략을 설계에 넣었다.

**12:12 [PC] "background 작업 2개를 resume 하자."**
- multi2 360/380(profile 없음)의 빌드 검증과 보드 측정이다. VTA 시간은 MultiEVTAConv4(EVTA 2개 동시)로 쟀다.

  | 검증 | 360 MHz | 380 MHz |
  |---|---|---|
  | WNS / WHS | +0.003 / +0.010 | +0.001 / +0.010 |
  | stuckchk / Y219 PIP | 0 extra / 0 | 0 extra / 0 |
  | 프로브 (EVTA 0, 1) | 통과 | 통과 |
  | 번들 (Conv4, ResConv10, x2Conv, Resnet18 2×100회 281) | PASS | PASS |
  | MultiEVTAConv4 VTA 시간 (333: 179.19 ms) | 165.97 ms (−7.4%) | 157.34 ms (−12.2%) |
- multi4 360 빌드를 시작했다(Y219 재배선 흐름에 `VTA_IMPL_DIR` 추가).

**16:12 [PC] multi4 360 보드 통과. 그리고 중요한 발견**
- **333 MHz를 넘는 모든 판이 PS8 HP 포트 최소 주기(3.0 ns) 규격을 어긴다**(WPWS 360: −0.222, 380: −0.368).
  - 지금까지 WNS/WHS만 보고 통과로 판정했다. 이후로는 **WPWS까지 본다**(메모리에 저장).
- 해결 방법은 HP 포트만 별도 333 MHz 클럭으로 분리하는 것이다.
- multi4 360(HP 분리 전)의 결과: WNS +0.018, WHS +0.009, Y219 PIP 0. stuckchk는 1 extra였는데, 쓰지 않는 PIP 비트로 확인돼 무해하다고 판단했다. 프로브는 40/40 통과했다. 아래는 EVTA 시간이다.

  | 번들 | 333 MHz | 360 MHz | 변화 |
  |---|---|---|---|
  | ResConv10 | 146.42 ms | 135.56 ms | −7.4% |
  | Resnet18 multi | 582.09 ms | 539.29 ms | −7.4% |
  | Resnet18 single | 1303.72 ms | 1208.40 ms | −7.3% |
  | Conv | 54.18 ms | 50.90 ms | −6.0% |
  | Conv4 | 194.39 ms | 166.35 ms | −14.4% (333 기준을 한 번만 잰 편차. 21:25 기준으로 다시 계산하면 −7.4%) |

**16:46 [PC] "HP 클럭 분리를 먼저 하자." / "single은 두고 multi4 380MHz 부터."**
- multi4 `vivado.tcl`을 바꿨다: `hp_pll` 333 MHz, smc `NUM_CLKS 2`. 333 MHz 이하 빌드는 예전과 같다(`70ccf2f7`).

**18:32 [PC] "실행 명령을 정확히, 어느 dir에서 하는지, 모든 절에서."**
- `vta_synth_notes.md`에 §2.5 명령 모음을 넣었다. 모든 절에 "실행 위치 + 명령"을 넣었다.
- git 밖에만 있던 스크립트 5개(`probes.sh`, `StuckLoc.java` 등)를 저장소에 추가했다(`e7c5148e`, `d3644803`).

**19:16 [PC] "systolic 계획과 오픈소스·논문 분석을 문서로." / "새 문서로." / "보고서처럼, 외부 자료는 docs/ref/에."**
- 새 문서 `docs/evta_systolic.md`를 만들었다.
  - §0 방향 결정 A~E안. 권고는 A(클럭) → B(systolic 400) → C(어레이만 2배 클럭)이다.
  - §9는 소스 9개의 보고서다. FireFly v2가 ZU+ 보드 전체에서 DSP만 2배 클럭(300/600)으로 돌린 사례라서, C안 목표를 ~600 MHz로 잡았다.
- `docs/ref/`는 라이선스에 따라 나눴다. 재배포가 안 되는 PDF는 git에서 빼고 `fetch.sh`로 받게 했다.

**19:38 [PC] "uop 순서를 HDL·bit까지 만들어 보드에서 검증했어? 열마다 다른 출력 시각은 어떻게 맞췄어?"**
- **아니다. HW는 아직 바꾸지 않았다.** Phase B는 런타임만 바꿔서 확인한 것이다.
- 열 시각은 deskew(열 j에 15−j단)와 dst 태그 pipe로 맞춘다. 이 방식은 C++ 모델에서만 확인했다. 검증 범위를 문서에 명시했다.

**19:43 [PC] "설계를 글과 그림으로. 그 다음 Phase D 시작할까?"**
- `docs/evta_systolic_design.md`와 SVG 그림 4개(top, array, timing, rmw)를 작성했다(`40393239`).

**21:25 [PC] multi4 380 MHz(HP 333) 보드 통과 (`d38fb2f5`)**
- **타이밍 수정 과정:**
  - multi4는 Y219 결함 행 때문에 Vivado 기본 구현을 쓸 수 없다. RapidWright 재배선 흐름(`run_flow.sh m380`)으로 Y219 행 PIP를 0개로 만든 결과(`rs_m380`)에서 출발한다.
  - 이 흐름은 행 주변을 비우고 다시 배치·배선하므로 타이밍이 Vivado 기본 결과보다 나빠진다. 그래서 남은 위반을 단계별로 고쳤다.
  - 단계마다 새로 깐 배선이 Y219 행 PIP를 쓰면 보드에서 결함 셀에 걸린다. 그래서 각 단계는 **행 PIP 0개, 미배선 0개일 때만 bit를 쓰게** 했다.

  | 판 | 한 일 | WNS (setup) | WHS (hold) | WPWS (HP 포트) | Y219 PIP |
  |---|---|---|---|---|---|
  | `rs_m380` | 재배선 흐름 결과 | −0.046 ns (2곳) | −0.001 ns (16곳) | +0.001 | 0 |
  | `rs_m380o` | 배선 뒤 `phys_opt_design` 3회 (`route_s.tcl` opt) | **+0.021** | −0.001 (16곳) | +0.001 | 0 |
  | `rs_m380h` | `phys_opt_design -hold_fix` (`holdfix.tcl`) | +0.021 | −0.001 (16곳, 변화 없음) | +0.001 | 0 |
  | `rs_m380n` | 위반 net 하나만 풀고 다시 배선 (`holdnets.tcl`) | +0.021 | **+0.001** | +0.001 | 0 |

  - **setup 2곳:** 레지스터 버스 `axi_xbar` → `fetch_2` 제어 레지스터 경로였다. 지연의 91%가 배선이라 Y219를 건너는 경로로 보이며, `phys_opt`로 해결했다.
  - **hold 16곳:** 모두 `smc_2` FIFO 안의 로직 0단 경로였고, 각각 1 ps가 부족했다. 원인은 같은 net 하나였다.
    - `-hold_fix`로는 줄지 않았다.
    - 그 net만 풀어서 라우터가 우회 배선으로 hold를 맞추게 하니 해결됐다.
    - `smc_2`가 Y219 행 바로 위에 있어서 우회 배선이 행 PIP를 쓸 위험이 있었는데, 결과는 0이었다.
  - 두 클럭(EVTA 380, HP 333) 사이 경로 168개는 SmartConnect의 비동기 변환 제약으로만 잡혔고, 여유는 1.5 ns다.
  - 최종 `rs_m380n`: stuckchk 0 extra, 보드 프로브 40/40. `holdnets.tcl`은 `y219route/flow/`로 저장소에 넣었다. 10-28 multi4 systolic 빌드에도 같은 순서를 쓴다.
- EVTA 시간: `hw_multievtax4` 번들을 `run_prof.sh`로 돌렸다(`OMP_NUM_THREADS=1`). 같은 날 333 MHz판도 다시 쟀다.

  | 번들 | 333 MHz | 380 MHz (HP 333) | 변화 |
  |---|---|---|---|
  | ResConv10 | 146.41 ms | 129.58 ms | −11.5% |
  | Conv4 | 179.60 ms | 158.77 ms | −11.6% |
  | Conv | 54.32 ms | 54.35 ms | 0% |
  | Resnet18 multi | 582.03 ms | 516.57 ms | −11.2% |
  | Resnet18 single | 1303.82 ms | 1157.39 ms | −11.2% |

  **Conv 번들은 0%다.** load/store가 HP 333에 묶이는 것으로 추정하며, 확인하지 않았다.

**21:56 [PC] "Phase D 시작하자." / "빌드한 multi4로 ResConv10이 609 GOPS에서 12% 이상 빨라지는지."**

ResConv10(3x3, 7x7x512→512) conv만 반복하는 실험이다. 지난번 609 GOPS와 같은 방법으로 쟀다: `multi4resconv10Bundle <mask> 400 conv`, `OMP_NUM_THREADS=1`, 실행마다 비트스트림 재로드. 333과 380을 번갈아 3회씩 돌렸고, 출력은 모두 golden과 같았다.

| ResConv10 conv만 반복 | EVTA 4개 | EVTA 1개 | conv 1회 |
|---|---|---|---|
| 333 MHz | 609.60, 609.67 GOPS | 152.53~152.62 | 1.516 ms |
| **380 MHz (HP 333)** | **685.07, 685.24, 685.06 GOPS (+12.4%)** | 171.57~171.60 (+12.5%) | 1.349 ms (−11.0%) |

333 1회차의 586.76 GOPS는 EVTA3 하나만 느렸던 편차라서 표에서 뺐다.

- 이론 최대(778 GOPS)의 88%다. 클럭 +14%에 비해 덜 오른 이유는 CPU의 명령 생성 시간(약 66 µs) 때문이다.
- **D1~D3:** RTL `gemm_sa`(`sa_pe`, `sa_array`, `sa_ctrl`, `sa_rmw`)를 작성했다.
  - xsim 벡터 31개가 모델과 비트 일치한다.
  - ResConv10 모양 7,056 GEMM이 7,114클럭(99.2%)에 끝난다.

**22:32 [PC] "진행하자." (D4 OOC, HLS 쪽, BD 통합)**
- **OOC:** `gemm_sa`만 따로 배치·배선(y219 fence 영역)해서 클럭 상한을 봤다. 자원은 DSP 256, LUT 2,825, FF 6,811이다.

  | 클럭 | 첫 RTL WNS | RMW 비교를 S1으로 옮긴 뒤 WNS | WHS |
  |---|---|---|---|
  | 400 MHz | +0.125 | **+0.226** | +0.045 |
  | 500 MHz | −0.071 | **+0.121** | +0.042 |
  | 600 MHz | −0.344 | **+0.079** | +0.039 |

  처음에는 RMW의 forwarding 경로(dst 비교 → 512비트 mux → 덧셈이 한 클럭)가 막았다.
- **HLS (`SYSTOLIC=1`):** GEMM 루프는 uop을 AXIS로 보낸다(II=1). acc_mem은 BD BRAM으로 꺼냈다. C 시뮬레이션 오류 0.
- **BD 통합:** 래퍼 `gemm_sa_bd`를 넣었고, D5(PL 시뮬레이션)는 건너뛰고 바로 비트스트림을 만들었다(`13fcd93b`).

**10-28 00:28 [보드] "문서 정리하고 커밋하자. systolic을 main에 merge 하는 게?"**
- main을 origin/systolic으로 fast-forward했다(28 commits, 충돌 없음). 기본 빌드는 그대로다.
- 문서 경로를 정리했다(`8d29663`).

**10-28 00:20~01:59 [PC] systolic 비트스트림 첫 보드 실행: 결함 4개 수정 → 333 MHz에서 HLS와 같은 속도**

| # | 증상 | 원인 → 수정 |
|---|---|---|
| 1 | IO 배치 실패 | Vivado가 `gemm_sa_bd`를 top으로 자동 선택 → `set_property top` |
| 2 | 모든 곱이 0 | BRAM 출력 레지스터는 EN=1일 때만 갱신 → 읽기 EN 상시 1 (Makefile이 RTL 변경을 못 보던 것도 수정) |
| 3 | gemmprobe 0 | 같은 wgt 주소면 다시 채우지 않는데, 그 사이 LOAD가 새 weight를 씀 → 명령 첫 uop에 `first` 비트 |
| 4 | 333에서 +29% 느림 | g2l 토큰이 늦게 나가 LOAD가 겹치지 못함 → RTL이 토큰을 직접 push |

- 333 MHz 첫 빌드: WNS +0.075 (HLS판은 +0.004), stuckchk 0 extra, Y219 PIP 0.
- 결과: 프로브 10/10, golden 30, Resnet18/Resnet18Partition/Resnet50 281.
- EVTA 시간은 새 uop 순서 런타임(`_sysprof`)으로 쟀다. 같은 333 MHz에서 기존 HLS GEMM과 비교했다.

  | 번들 | HLS 333 | systolic 333, 토큰 수정 전 | systolic 333, 토큰 수정 후 |
  |---|---|---|---|
  | ResConv10 | 2.893 ms | 3.741 ms (+29%) | 2.891 ms |
  | Conv4 | 3.565 ms | 3.851 ms (+8%) | 3.570 ms |
  | Resnet18 | 48.53 ms | 56.07 ms (+16%) | 48.95 ms (+0.9%) |
- 중요: 3번은 시뮬레이션에서 메모리가 바뀌지 않아 놓친 결함이다. 테스트벤치에 reload 모드를 추가했다.

**10-28 03:36 [PC] systolic 380 보드 통과, 400은 주변 블록이 막음 (`ac1faeb0`)**
- 첫 400/380 빌드는 둘 다 조금 모자랐다(400: −0.061, 88개 / 380: −0.023, 26개). BRAM mux 선택 신호를 레지스터 `sel`로 바꿔 다시 빌드했다.
- 380: WNS +0.046, WHS +0.010, WPWS +0.001. stuckchk 0 extra, 프로브 10/10, golden 30, Resnet 281.
- systolic 380과 기존 HLS 380의 EVTA 시간을 같은 날 비교했다. HLS 380판은 HP 포트도 380이라 규격 밖이다.

  | 번들 | HLS 380 | systolic 380 (HP 333) | 차이 |
  |---|---|---|---|
  | ResConv10 | 2.544 ms | 2.559 ms | +0.6% |
  | Conv4 | 3.138 ms | 3.156 ms | +0.6% |
  | Resnet18 | 42.76 ms | 43.14 ms | +0.9% |
  | Resnet50 | 221.61 ms | 236.99 ms | +6.9% |
  | Vgg16Cifar10 | 29.85 ms | 31.23 ms | +4.6% |

  conv는 같다. Resnet50과 Vgg16의 차이는 load/store가 많은 층에서 나서, HP 클럭(333 대 380) 영향으로 추정한다.
- 400(sel 수정 후): WNS −0.135, 175개 실패. 실패 경로는 **모두 `gemm_sa` 밖**이다.

  | 실패 경로 | 개수 | 최악 slack |
  |---|---|---|
  | `axi_xbar` 레지스터 버스 폭 변환 → 클럭 변환 | 112 | −0.112 ns |
  | HLS compute → acc_mem 쓰기 | 34 | −0.132 ns |
  | HLS compute 내부 상태기계 | 11 | −0.135 ns |
  | HLS load → inp_mem 쓰기 | 12 | −0.128 ns |
- 중요: **systolic은 같은 클럭에서는 이득이 없다.** 이득은 HLS가 못 가는 클럭(400 MHz 이상, C안)에서만 나온다.
- 다음 방향 선택지: 1) 400 MHz 마무리, 2) C안, 3) multi4 적용. 권고는 1번.

## 결정이 남은 것 (10-28 07:00 기준)
- 다음 방향: 10-28 08:48에 "1번 먼저"로 결정됐다.
- 기본 비트스트림을 380 MHz(HP 333)로 바꿀지. single과 multi2의 HP 분리 재빌드가 먼저다.
- uop cache 버그를 원본 `vta/vtalib`와 사본 3개에 고칠지.
- HP 333 분리로 load/store가 많은 번들이 손해를 보는지 `PROFILE=1`로 확인할지.
