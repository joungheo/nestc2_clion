# 2026-11-03 EVTA systolic 요약: ALU imm II=1 400 MHz single·multi4 보드 통과(Resnet18 EVTA 26.42 → 24.89 ms), Resnet18 Mops에서 conv0·fc 누락 수정(162 → 145.8 GOPS), 전체 요약 문서·user manual·그림 15장

- 기간: 2026-11-03 07:00 ~ 11-04 07:00 (KST). 기록은 12:59부터 있다(PC가 11-02 22:46에 꺼져 있었다).
- 세션은 하나다. [PC]는 PC의 `main`에서 한 작업이다. 보드 실험(검증, 측정, `git pull`)도 이 세션이 PC에서 ssh로 돌렸다.

## 시간순

**12:59 [PC] "PC에서 실행하던 evta 합성이 멈췄지? PC가 어제 저녁에 꺼졌어. 왜 꺼졌는지 분석해봐. 보드에 serial 연결도 안 되네."**
- 중요: PC는 **11-02 22:46:43**, systolic ALU II=1 bitstream의 `route_design`("Phase 4.1 Global Iteration 0") 중에 꺼졌다. 정상 종료도, kernel panic도 없이 로그가 끊겼다(`last -x`에 `crash`).
  - 10-25 14:05(multi4 합성), 10-28 11:10(multi4 systolic 배선, dir 이름 `..._crash1028`)에도 같은 형태로 꺼졌다.
  - machine check, OOM, thermal, NVMe 오류 기록은 없다. Ryzen 9 5950X 16코어가 전부 부하일 때만 생기므로 하드웨어 보호 동작으로 본다. 후보: 파워 과전류·과열 보호, CPU 과열 차단, BIOS PBO/메모리 오버클럭(원인은 확정하지 못함).
  - 회피책: Vivado를 4 thread로 돌리고, CPU 온도를 5초마다 기록한다.
- 보드 serial: 보드는 정상이었다(uptime 2일 11시간). PC 재부팅 뒤 장치 번호가 바뀌어 `/dev/ttyUSB0`이 Digilent JTAG(FT232H)이고, 보드 콘솔은 `/dev/ttyUSB1`(CP2108 Interface 0)이다 → `sudo picocom -b 115200 /dev/ttyUSB1`.
- 지난번 질문(bias를 GEMM으로, DSP48E2)에 답했다: PE마다 DSP 안에서 `P = A×B + PCIN`으로 열 합을 하고(`sa_pe.v`), acc 덧셈은 fabric 덧셈기 16개가 한다(`sa_rmw.v`). bias는 RMW reset 때 0 대신 bias를 쓰는 RTL 방법이 가장 단순하다. 소프트웨어만으로는 bias를 int8 weight 블록으로 쪼갠 GEMM 단계를 하나 더 넣는 방법이 있다(약 −0.3~0.45 ms, |bias| 약 24만 초과는 불가).

**13:05 [PC] "합성과 PnR 다시 돌리자." / "오늘 아침에 일지도 못 썼겠네. 이것도 해줘."**
- 400 MHz systolic ALU II=1 빌드를 `JOBS=4`로 다시 시작했다. 꺼질 때의 dir은 `..._sa_400mhz_alu1_crash1102`로 남겼다.
- 로그는 `/home/ubuntu/npuprg/templog/`(`viv_alu1.log`, 온도 `temp_261103_1305.log`)에 둔다. `/tmp`는 PC가 꺼지면 지워지기 때문이다.
- 11-02 일지를 `docs/dailyreport/tools/daily_report.sh 261102`로 직접 만들었다(cron이 PC가 꺼져 안 돌았다). PC가 꺼진 일을 22:18 항목 아래에 한 줄 덧붙였다.

**13:27 [PC] "systolic은 시작 시점에 input, weight, acc를 같이 읽어서 acc 값을 제일 위 행에 더하는 방식 맞지? 기존 broadcast는 곱하고 다 더한 뒤 acc에 더하고 저장?"**
- 아니다. 지금 `gemm_sa`는 시작 때 inp만 skew해 넣고(weight는 미리 B 레지스터에), 열 맨 아래 합에 **마지막 RMW 단계에서** acc를 더한다. inp를 넣고 33 cycle 뒤 RMW(S0 주소, S2 덧셈, S3 쓰기, 진행 중 쓰기 forwarding)다.
- 기존 HLS `gemm()`은 inp, weight, acc[dst]를 한 반복에서 함께 읽고, 256곱 → 덧셈 트리 → acc 덧셈 → 쓰기다. 400 MHz에서 막힌 경로가 여기였다.
- 위 행 주입은 가능하지만 acc를 33 cycle 먼저 읽어야 해서 같은 dst의 짧은 재사용이 깨지고, 열별 skew 레지스터가 필요하다. 속도는 같고 덧셈기 16개(LUT 약 500)만 준다.

**13:33 [PC] "RMW에서 reset할 때 0 대신 bias를 쓰는 건 어렵지 않겠네. 뭘 바꿔야 하고 어느 정도 일이야?"**
- 전체 약 2일, 이득은 EVTA 약 −0.9 ms(3%). ALU "ADD reg" 0.91 ms(conv0 0.28 포함)가 모두 bias ADD다. ALU II=1과 합치면 약 −2.4 ms.
- 바꿀 곳 4곳: ① RTL `sa_rmw.v`(약 20줄)와 tag 전달(`gemm_sa.v`, `sa_ctrl.v`, `bias` 비트와 `src`; `r2 <= reset ? 0 : r1`로 S2 경로에 logic 추가 없음) ② HLS `gemm_sa_issue` / `gemm_sa_csim`(uop 스트림 40 → 41비트) ③ ISA `VTAGemInsn`의 `pad_0` 비트 ④ vtalib 명령 순서(bias LOAD → reset(새 비트) → GEMM, ALU ADD 제거; conv, residual, conv+pool, fc 4경로). 옛 bitstream은 `/tmp/evta_bitstream` 이름(예: `_rbias`)으로 가른다.

  | 단계 | 시간 |
  |---|---|
  | RTL과 tag 전달, testbench(xsim) | 반나절 |
  | HLS, ISA 비트, C simulation | 2~3시간 |
  | vtalib 4개 경로와 bitstream 구분 | 반나절 |
  | bitstream 빌드 | 약 1시간 |
  | 보드 확인: golden, 모든 conv 출력 byte 비교, 시간 측정 | 2~3시간 |
- 제안: ALU II=1을 먼저 보드에서 확인한 뒤 bias reset을 얹는다(두 변경을 한꺼번에 넣으면 원인을 가르기 어려움).

**13:39 [PC] "좋아. ALU II=1이 제대로 돌면 bias reset을 하자. 다른 유저가 수정들을 빨리 파악하려면 어떤 문서를 봐야 해?"**
- 읽는 순서: `CLAUDE.md` "EVTA on this board" → `docs/evta_runtime_compare.md` §7.1~7.14(§2, §3 먼저) → 하드웨어 문서(`evta_systolic_design.md`, `evta_systolic.md`, `vta_synth_notes.md`, `evta_multi4_609gops_report.md`) → `evta_runtime_aot.md` → `vtabundles_xcmpl/README.md` → 일지.
- 부족한 점: `evta_runtime_compare.md` §1 요약이 10-29 계획 단계에 머물러 있고, 단계별 누적표, 고친 파일, 끄는 옵션이 흩어져 있다.

**13:47 [PC] "§1 요약을 최근 것까지. 전체를 시간순으로 요약하는 문서, 명령을 정리한 user manual도 만들자."**
- 세 문서를 commit·push했다(`75c8bee6`).
  - `evta_runtime_compare.md` §1.1 지금 상태(2026-11-03): Resnet18 single 73.6 → 27.71 ms(OMP 4), 91.1 → 28.83 ms(OMP 1)의 단계별 표(절 번호, 시간, 끄는 옵션). 기존 10-29 요약은 §1.2로 두었다.
  - 새 `docs/evta_overview.md`: 시기별 6절(보드 첫 실행, 자체 bitstream·Y219, 클럭·systolic, CPU 일을 EVTA로(변경 14개 표), EVTA 시간 분석·HW 개선, 운영 중 생긴 일)과 문서 지도.
  - 새 `docs/evta_user_manual.md`: §0 기계와 dir ~ §8 자주 하는 일의 순서(make 옵션 11개, backend 옵션 8개, runtime 환경 변수 18개 등). `CLAUDE.md`에서 두 문서로 링크했다.
- ALU II=1 첫 빌드는 PC가 꺼지지 않고 끝났지만(최고 56 °C) timing 실패. 아래 단계별 표 참고.

**13:57~15:55 [PC] single ALU II=1 400 MHz: timing을 맞추기까지 (14:03, 14:47, 15:00, 15:48 결과 통합)**
- 첫 빌드는 새 imm 루프의 `uop_mem` 읽기 → 주소 덧셈 → acc_mem 주소(BRAM 58개 팬아웃)가 한 cycle이라 실패했다. register를 넣은 뒤로는 실패가 흩어져 있어서, 전략을 바꿔 가며 다시 빌드했다.

  | 판 | 한 일 | WNS | 실패 끝점 | 결과 |
  |---|---|---|---|---|
  | 첫 빌드 (`..._alu1_wns0819`로 보관) | ALU imm II=1 루프 | −0.819 ns | 9,031 | 실패. 실패 path 3,000개 중 739개가 uop → 주소 → acc_mem |
  | register 1차 | `hls::reg` 사용 | — | — | csim 컴파일 실패: 2020.1에서는 `hlstmp` namespace → `vta.cc`에 같은 함수(`alu_reg`) 직접 정의 |
  | register 넣은 판 (`c50dd868`) | uop 읽은 뒤·주소 계산 뒤 register, `ExtraTimingOpt` | **−0.126 ns** | **322** | 실패. 주 원인 경로는 사라짐 |
  | 배선 후 최적화 | `phys_opt_design -directive AggressiveExplore` | −0.124 ns | 286 | 실패 |
  | `alu1_epr` | `Performance_ExplorePostRoutePhysOpt` 전략으로 재빌드 | **+0.001 ns** | 0 | 통과 (WHS +0.010, WPWS +0.001, 최고 57 °C) |

- register 넣은 판의 HLS: C simulation 0 errors, imm 루프 II=1 유지, 깊이 7 → 9, HLS 클럭 추정 2.635 ns(이전과 같음).
- register 넣은 판의 세부: WHS / WPWS +0.010 / +0.001, LUT 34.1K(이전 systolic 28.3K). 남은 실패는 compute → acc_mem 쓰기 enable·데이터(6단 logic) 약 100, load → wgt_mem 35, acc_mem → gemm_sa 29, reset 팬아웃 약 60.
- ALU 수정 전 systolic 400도 기본 전략은 실패(−0.135)였고, `ExtraTimingOpt` +0.015, `ExplorePostRoutePhysOpt` +0.012에서만 통과했다. 그래서 남은 전략으로 다시 돌렸다.
- WNS 여유가 1 ps라 아주 얇다. 보드로 `bit/evta_sys_alu1_261103/`에 옮겼다(md5 확인, `.bin`은 `/lib/firmware/`에도).

**14:06 [PC] "보드에서 pull 해서 새 문서들을 가지게 해줘. 이전 구조와 수정한 구조를 같이 그려볼까? 뭘 그리면 좋을지 리스트를."**
- 보드 pull: main `c50dd86`, `vta/vtalib` `37552ab`.
- 그림 후보 15개(A 전체 흐름, B 층 사이·안, C runtime, D 하드웨어, E 수치)를 제시했다. 권하는 순서 1 → 12 → 4 → 5 → 14.

**14:07 [PC] "일지들도 커밋, push 하고 보드에서 pull 해줘."**
- 일지 14개(`vta_synth` 10-20~10-28 9개, `vta_systolic` 10-29~11-02 5개)를 commit·push하고 보드에서 pull했다(`72755603`). 보드의 일지 md는 모두 18개다.

**14:16 [PC] "1 → 12 → 4 → 5 → 14 순서로 그리고 나머지도 다. svg로, 자세히, 제목도."**
- 그림 15장 `docs/figs/evta_fig01~15_*.svg`를 그려 commit·push, 보드 pull(`65aad4c0`). 생성 스크립트 `docs/figs/src/make_evta_figs.py`, `svglib.py`. PNG로 렌더링해 겹침과 수치를 확인했다.
- `docs/evta_overview.md` 끝에 "그림: 이전 구조와 지금 구조" 절을 넣었다.

  | # | 내용 |
  |---|---|
  | 1 | Resnet18 한 장: 종류별 시간 막대(148.9 → 28.83 ms, 같은 축)와 이전·지금의 CPU / EVTA 흐름 |
  | 2 | 바꾼 위치 지도: 컴파일러 앞단은 그대로, `VTASave.cpp` pass와 옵션, vtalib 파일별 변경, 하드웨어 |
  | 3 | NHWC와 EVTA layout의 주소 순서, 층마다 transpose → layout 유지 |
  | 4 | residual block: conv → CPU elemadd → CPU relu ⇒ EVTA 호출 하나 (8단계 ALU/GEMM 순서) |
  | 5 | conv0: 16채널 0 채움 7×7 → packed 4×1 (채널 접는 방법, 49 → 12 단계) |
  | 6 | conv0 + maxpool: 17×17 tile과 halo, 두 단계 MAX, ALU 명령 19 → 6개 |
  | 7 | 입력 전처리: CPU 4단계 → NEON 한 번 (`[C][256]` 표, `vqtbl4q`) |
  | 8 | fc: 띄엄띄엄 읽기 → NEON → EVTA GEMM |
  | 9 | EVTA 호출의 CPU 쪽: buffer pool, record / replay |
  | 10 | multi4 멈춤: flush와 MMIO 시작의 시간축, `dsb sy`, 원인을 가른 실험 표 |
  | 11 | EVTA 블록도: HLS GEMM·클럭 하나 → gemm_sa, 400 / 333 MHz |
  | 12 | GEMM: HLS broadcast(256 곱 + 덧셈 트리) → systolic 어레이(PCIN, skew, RMW) |
  | 13 | ALU 포트 사용: II = 2 → imm II = 1, 다음 단계 bias reset (원소당 8 → 5 → 3 clk) |
  | 14 | 단계별 시간 감소 막대 (예상 두 단계는 점선) |
  | 15 | 지금 EVTA 26.47 ms 구성: 전체와 GEMM 밖 확대, ALU II = 1과 bias reset 뒤의 예상 |
- 그림 13·14·15의 예상치(ALU II=1 −1.5 ms, bias reset −0.9 ms)는 점선과 "(예상)"으로 표시했다. 그림 15의 "기타"에 같은 기록을 두 번 쓰는 run 몫 0.25 ms가 들어 있다.

**15:28 [PC] "compute → acc_mem 쓰기 enable과 데이터 경로에 register를 하나 더 넣는 건 어느 파일을 바꾸는 거야?"**
- 모두 `vta-hw-zcu102-systolic/hardware/xilinx/src/vta.cc` 한 파일이다.
- 중요: 가장 나쁜 경로(−0.126)는 고친 ALU 루프가 아니라 **acc LOAD의 padding 쓰기**(`load_pad_2d` / `reset_mem`, `vta.cc:64`를 `:720`에서 호출)에서 시작해 acc_mem `WEA`에서 끝난다. ALU 루프가 acc 포트에 쓰는 쪽을 늘려 mux와 배치가 빡빡해진 것으로 본다.

  | 위치 | 파일 | 무엇을 | 특징 |
  |---|---|---|---|
  | ① acc LOAD의 padding 쓰기 | `vta.cc` 47~64행 `load_pad_2d` / `reset_mem` (acc는 720행에서 부름) | pad를 0으로 쓰는 루프의 주소와 enable 앞에 register | 지금 가장 나쁜 경로. inp LOAD(207행)도 같은 함수를 쓰므로 acc 호출에만 적용되게 해야 함 |
  | ② 새 imm ALU 루프의 acc 쓰기 | `vta.cc` `alu()`의 `ALU_IMM` 루프 | 쓰기 데이터 앞에 `alu_reg` 하나 더 | 실패 묶음 중 ALU 쪽(`reg_reg`, `p_Result_*` → acc_mem) |
  | ③ HLS와 RTL이 acc 포트를 나눠 쓰는 mux | `rtl/gemm_sa_bd.v` (`rtl/gen_gemm_sa_bd.py`가 생성) | mux 출력에 register | 모든 쓰는 쪽에 듣지만 HLS가 보는 acc 지연이 1 늘어남. `RAM_T2P_BRAM latency = 2`도 바꿔야 해서 위험이 큼 |
- 권고: ①+②, ③은 마지막 수단.

**15:52 [PC] "register를 넣으면 쓰는 시간이 1 clk 밀리는데 write addr도 1 clk 밀어야 하지?"**
- HLS(①②)는 store 연산 하나라 주소·데이터·enable이 함께 밀린다(주소 register 때도 손대지 않았고 csim 통과). 손으로 같이 밀어야 하는 것은 RTL(③)이고, 그때는 읽기 지연 2 → 3과 RMW forwarding도 바뀐다.
- 시간 영향: II는 그대로, 명령마다 1 clk(Resnet18 ALU 명령 약 1,600개 ≈ 4 µs). Resnet18 덤프로 imm 명령은 같은 주소를 16 반복 안에 다시 쓰지 않아 깊이 10까지 안전하다.

**15:55 [PC] ALU II=1 single bitstream 보드 통과 (`bc8a4434`)**
- 새 bitstream `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_400mhz_alu1_epr.bin`: `build_systimed` golden 30 PASS. 분류 결과 Resnet18 281 / 0.655530, Resnet50 281 / 0.581379, Partition 281 / 0.530282, VGG16 0 / 0.453709로 전과 같다. 층 출력 byte 비교 Resnet18 40개, Resnet50 106개 모두 같다.
- 같은 실행 파일, 같은 날, systolic 400 MHz에서 기존과 새 bitstream을 비교했다. runbench는 HW 루프 반복 시험, Resnet18은 25회다.

  | | 기존 `..._sa_400mhz` | 새 `..._alu1_epr` |
  |---|---|---|
  | runbench ALU imm 반복 | 2.0 clk (169.7 µs) | **1.0 clk (87.4 µs)** |
  | runbench ALU reg, GEMM | 169.7 / 46.1 µs | 169.5 / 46.4 µs (그대로) |
  | Resnet18 EVTA 시간 (`_sysprof`, 25회 평균) | 26.42 ms | **24.89 ms (−1.53 ms)** |
  | Resnet18 single OMP 1 (`_systimed`, run 2·3·25) | 28.98 ms | **27.32 ms** |

  예상한 −1.5 ms와 같다. 기존 28.98 ms는 어제 28.83 ms와 측정 시점 차이로 조금 다르다.
- 보드 dir을 `bit/evta_sys_alu1_400MHz_pass_261103/`로 바꾸고 README를 넣었다. `evta_runtime_compare.md` §7.15, §1.1, `CLAUDE.md`, overview, 그림 14(실측값)를 고쳤다.

**16:17 [PC] "ALU 1clk multi4도 했어? 이것도 해야 timing이 괜찮은지 확신. 그 다음 bias reset. SYS_BIN 기본값을 새 bitstream으로."**
- single `SYS_BIN`을 `..._sa_400mhz_alu1_epr.bin`으로 바꿨다(`vtabundles_xcmpl/hw/script/run_all.sh`와 생성기 `collect_hw_bundles.py`, `CLAUDE.md`; `3f10dce3`, 보드 pull 확인).
- multi4 `vta.cc`에 같은 ALU 변경(`996c5baa`)을 넣고 `vta-hw-zcu102-multi4/build/m4_alu1.sh`로 빌드 시작: HLS(16/15/18/17) → `make bin SYSTOLIC=1 FREQ=400 JOBS=4 IMPL_STRATEGY=Performance_ExtraTimingOpt BUILD_TAG=alu1` → `run_flow.sh malu1`(Y219).
- bias reset은 multi4 확인 뒤, 그동안 PC 부하를 더 올리지 않는다.

**16:32 [PC] "Resnet18 Mops는 MAC 당 2개로만 잡나? maxpool, elemadd 등 ALU, 데이터 이동은 없는 거로 치는 거야?"**
- `timed_bundle.py`의 `call_ops`는 conv·fc의 MAC × 2만 센다(ALU 연산, LOAD/STORE, DRAM 전송, CPU 복사는 세지 않음). NVDLA, DPU, Gemmini와 같은 관례다. ALU 일은 약 1,500만 연산으로 MAC 36억의 0.4 %지만, 시간으로는 EVTA의 17 % → 11 %다.
- 중요: **지금 Resnet18 Mops에서 conv0+pool과 fc가 빠져 있었다.** 11-02에 새로 만든 호출이 `call_ops` 목록에 없었다. 그래서 11-02 이후 GOPS는 낮게, 그 전 0 채움 conv0 시절은 16채널로 세어 높게 나왔다.

  | 호출 | 실제 MAC × 2 | 지금 세는 값 |
  |---|---|---|
  | conv0 + maxpool (`convolution_wo_tr_pool`, 11-02부터) | 236 Mops (7×7×3 → 64, 112×112) | **0** |
  | fc (`fullyconnected_evta_packed`, 11-02부터) | 1.0 Mops | **0** |
  | 나머지 conv 19개 | 3,391 Mops | 3,391 Mops |

**16:41 [PC] "측정 스크립트 고치자. fc, conv0도 Mops에 들어가는 거야? 가동률은 GOPS와 비슷한 숫자가 나오나?" (16:43, 16:47 결과 통합)**
- `timed_bundle.py`, `prof_summary.py`를 고쳤다(`e0301f75`). 첫 conv는 입력 준비 호출(`preprocess_conv0pack`, `conv0pack_nhwc2vtaio`, 0 채움 함수)에서 원래 채널 수와 커널을 읽어 **원래 모델 모양**으로 센다. `convolution_wo_tr_pool`의 마지막 인자(pool padding)를 EVTA 번호로 읽던 오류도 고쳤다.

  | 호출 | 이전 | 지금 |
  |---|---|---|
  | conv0 + maxpool (`convolution_wo_tr_pool`) | 0 | **236.0 Mops** (원래 7×7×3 → 64, 출력 112×112) |
  | fc (`fullyconnected_evta_packed`) | 0 | **1.0 Mops** |
  | 나머지 conv 19개 | 3,391.1 | 3,391.1 |
  | 합 | 3,391.1 | **3,628.1** |

- GOPS와 가동률의 차이는 버려지는 계산이다. 새 bitstream, EVTA 시간 24.89 ms 기준으로 계산했다.

  | 지표 | 계산 | 값 |
  |---|---|---|
  | GOPS (모델 연산) | 3,628 Mops ÷ 24.89 ms | **145.8 GOPS** = 이론 최대 204.8의 **71 %** |
  | 어레이 가동률 | 실제 GEMM 반복 ÷ 400 MHz ÷ 시간 = 18.85 ms ÷ 24.89 ms | **76 %** |
  | 모델에 필요한 GEMM 시간 | 3,628 M ÷ 2 ÷ 256 MAC ÷ 400 MHz | 17.71 ms |

  차이 1.14 ms(18.85 − 17.71)는 conv0 pack의 0 채널 6개·weight 0인 8번째 행과 conv0+pool halo 재계산이다.
- 보드에서 Resnet18 single, 새 systolic bitstream(ALU II=1), 25회로 다시 쟀다.

  | 측정 | 이전 스크립트 | 지금 |
  |---|---|---|
  | Mops (한 장) | 3,391.1 | **3,628.1** |
  | EVTA 시간 (`_sysprof`, run 2~25 중앙값) | 24.89 ms | 24.888 ms → **145.8 GOPS** (이론 최대 204.8의 71 %) |
  | EVTA 호출 wall time (`_systimed`) | 20.92 ms (162 GOPS) | 25.18 ms → **144.1 GOPS** |
  | 추론 전체 (`_systimed`) | 27.32 ms | 27.33 ms → **132.7 GOPS** |

  중요: **예전 162 GOPS는 과대평가였다**(바른 값 144~146 GOPS). "EVTA calls" 시간이 늘어난 것은 conv0+pool·fc 호출이 그 칸에 들어갔기 때문이고, 추론 시간은 그대로다. 11-02 이후 문서의 GOPS는 conv0·fc가 빠진 값이다.
- multi4 HLS 통과: C simulation 0 errors, imm 루프 II=1, 깊이 9.

**17:25 [PC] "acc 쓰기 전 reg 추가하는 걸 vta.cc 원문과 비교해서 보여줘."**
- ②는 `alu_lane` 뒤에 `dst_tensor[i][b] = alu_reg(...)`, `o_tensor[i][b] = alu_reg(...)` 두 줄. 새 변수 없음, `dst_idx` 그대로, 깊이 9 → 10.
- ①은 `reset_mem` 구조를 바꿔야 한다. 실패한 것은 데이터(상수 0)가 아니라 쓰기 enable(`range`를 넓힌 `zext_ln209_1_reg` → 6단 logic → `WEA`)이다. `PIPELINE II = 1`과 주소 `a = alu_reg(sram_idx + i)`, 증가는 루프 밖에서 한 번. 바깥 `load_pad_2d` `y` 루프의 PIPELINE 때문에 실제 스케줄은 csynth로 확인해야 한다(미확인).

  | | 바꾸는 줄 | 새 변수 | 주소 맞추기 | 위험 |
  |---|---|---|---|---|
  | ② ALU imm 루프 | `alu_lane` 뒤 2줄 | 없음 | HLS가 알아서 | 작음 (깊이 +1) |
  | ① acc LOAD padding | `reset_mem` 루프 구조 | 주소 `a` 하나 | HLS가 알아서 | 중간 (load 루프 스케줄, inp LOAD에도 영향) |

**19:50 [PC] multi4 ALU II=1 timing 실패 → ①②를 `vta.cc`에 넣음 / 20:07 "HLS 하고 있는 거야? 아니면 합성을 하고 있어?"**
- multi4 400 MHz 빌드 결과다. Y219 PIP 0개, stuck cell 경로 0개.

  | | WNS | WHS | WPWS |
  |---|---|---|---|
  | 이전 multi4 systolic 400 MHz (msa400) | +0.002 | +0.009 | +0.001 |
  | ALU II=1, Vivado 배선 후 | −0.181 | +0.002 | +0.001 |
  | ALU II=1, Y219 flow 후 (rs_malu1) | **−0.343** | **−0.031** | +0.001 |

  최악 10개 중 ① `grp_reset_mem` → acc_mem `WEA` 6개, ② `reg_1728`(두 ALU 루프 공용) → LUT 5단 → `DIN` 2개.
- 수정(systolic·multi4 `vta.cc` 동일): ② 두 ALU 루프 쓰기 직전 register(II=2 루프는 `#ifdef VTA_SYSTOLIC` 안만). ① `reset_mem`에 `base`, `a = alu_reg(base + i)`, `PIPELINE II = 1`(`#ifdef VTA_SYSTOLIC`, 원문은 `#else`). 첫 HLS에서 `RESET_MEM`이 파이프라인되지 않아, 원래 "Unable to satisfy"로 적용되지 않던 `y` 루프의 PIPELINE을 systolic 빌드에서만 뺐다.
- HLS 결과: CSim 0 errors, `RESET_MEM` II=1 깊이 2(compute·load 모두), imm 루프 II=1 깊이 10, II=2 루프 깊이 11.
- HLS는 19:42에 끝났는데, 감시 명령이 pgrep으로 자기 자신을 찾아 20분 넘게 아무것도 돌지 않았다. 20:07에 `m4_alu1r.sh`(`make bin` ExtraTimingOpt JOBS=4 → Y219 flow `malu1r`)를 시작했다.
- PC 디스크 여유 38 GB(사용률 96%). 오래된 Vivado 빌드 dir 정리를 사용자가 볼 필요가 있다.
- single `alu1_epr`은 이 수정 전 코드로 만든 것이다.

**22:26 [PC] "다른 컴파일러, 오픈소스 NPU, 런타임, 싼 FPGA 조사를 문서에 정리하자. 계속 update. 목표는 여러 하드웨어를 연결할 컴파일러·런타임·드라이버 설계, 하드웨어 효율화도."**
- 새 `docs/npu_stack_survey.md`(`1cad6dc3`, 보드 pull). 항목마다 `[원문]`, `[측정]`, `[일반 지식]` 표시. §0 한눈에, §1 목표 구조(장치별 backend·opaque executable, 공통 HAL, 커널 driver, 링 버퍼 + doorbell + IRQ), §2 컴파일러(NEST-C, TVM-VTA, Vitis AI, NVDLA, Gemmini, Tensil, XLA), §3 runtime·driver, §4 싼 FPGA(K24, KV260, SCU35, SCU200; 지금 흐름을 거의 그대로 쓸 수 있는 곳은 Kria뿐), §5 하드웨어 효율, §6~§9.
- 빈 곳: MLIR 계열(IREE, torch-mlir), FINN, hls4ml, TVM UMA/Relax는 아직 조사하지 않았다.

**23:41 [PC] "MLIR하고 LLVM 하고 나서 runtime과 driver는 어떻게 구성돼?" / 23:56 "진행하자."**
- MLIR·LLVM은 runtime·driver를 정하지 않는다. LLVM은 프로그래머블 코어 코드에만 쓰이고, EVTA 같은 고정 명령은 dialect에서 serializer로 만든다. runtime 표준 후보는 IREE HAL, PJRT, ONNX Runtime EP, driver는 DRM accel(amdxdna, ivpu, habanalabs, qaic)로 모인다.
- 권하는 순서: (1) HAL 초안(IREE HAL/XRT 모양)과 vtalib 이전, (2) UIO + IRQ driver, (3) EVTA dialect 시제품.
- 공식 문서로 확인해 survey §2.3(새 절), §3.3, §0/§1/§6/§8/§9를 갱신했다(`c0d8d6be`). 새로 확인: IREE HAL 객체와 conformance test(`cts/`), 새 장치 = 컴파일러 target plugin + 외부 HAL driver(iree-amd-aie 사례), AMD NPU firmware mailbox + MSI-X, qaic의 ioctl 세 개 정도, DRM accel `/dev/accel/accel*`.
- 미확인: IREE HAL semaphore·queue 세부, `pjrt_c_api.h` 함수 목록, iree-amd-aie HAL driver가 XRT를 거치는지, Intel 라이브러리 이름.

**11-04 00:57 [PC] "multi4 rebuild는 계속 돌게 두면 될까? 다른 방법이 있어?" / 01:09 "맞게 kill 하고 place를 하고 있는 거야? … m4_*.sh은 opt_design까지만 돌리게 두 방법을 같이 쓰면 어때?"**
- 중요: `run_flow.sh`가 `make bin`에서 쓰는 것은 `vta_wrapper_opt.dcp`(20:54에 이미 생성) 하나뿐인데, `make bin`의 Vivado route가 4시간째 돌고 있었다(Global Iteration 7에서 겹치는 노드 3 → 11만 개). impl_1 route와 부모 Vivado를 kill했고, 스크립트는 `make bin rc=2` 뒤 `run_flow.sh malu1r`로 넘어갔다.
- background job이 계속 도는 것은 watcher가 상위 스크립트 `m4_alu1r.sh`를 보기 때문이다. kill 대상이 맞았다.
- opt_design에서 멈추는 두 방법을 함께 넣었다: multi4 `Makefile`/`vivado.tcl`의 `IMPL_TO_STEP`(기본 `write_bitstream`), 안전장치 `y219route/flow/m4_build.sh <tag> [make vars]`(`Command: place_design`이 보이면 그 impl_1 Vivado만 kill). 아직 실제로 돌려 보지 않았다.

**11-04 02:50 [PC] multi4 ALU II=1 400 MHz P&R·보드 통과, multi4 SYS_BIN 교체 (`f7df508c`)**
- Y219 flow 뒤 hold 한 곳(smc_3 FIFO의 net 하나)이 남아 단계를 더 거쳤다. 단계마다 Y219 행 PIP 0개여야 bit를 쓴다.

  | 판 | 한 일 | WNS | WHS | WPWS | Y219 |
  |---|---|---|---|---|---|
  | `malu1r` flow 직후 | place → RapidWright → route_s | +0.001 | −0.032 | +0.001 | — |
  | step H | `holdnets.tcl`(그 net만 다시 배선) | — | 그대로 (같은 경로를 탐) | — | — |
  | `holdpin.tcl` 400 ps | 끝점에 최소 배선 지연 | — | hold 맞음 | — | **행을 지나가서 버림** |
  | `holdpin.tcl` 310 ps | 같은 방법, 지연 축소 | **+0.001** | **+0.007** | +0.001 | PIP 0, 미배선 0, stuck 0 extra |

- 보드 실험(`bit/evta_multi4_alu1_400MHz_pass_261104/`, README 있음). EVTA 시간은 이전 multi4 bitstream과 비교했다.

  | 항목 | 결과 |
  |---|---|
  | probes (EVTA 0~3) | 40/40 |
  | golden (Conv4, ConvTest, ResConv10) | 3 PASS |
  | Resnet18 4 copy / single | exit 0, 분류 정상 |
  | layer dump 80개 | 이전 multi4 bitstream과 byte 단위로 같음 |
  | EVTA 시간, 4 copy 동시 (EVTA마다) | 26.73~26.85 → **25.24~25.37 ms** |
  | EVTA 시간, single copy | 26.49 → **24.97 ms** |
- multi4 `_sysprof` 실행 파일이 이전 bitstream에서도 exit 139로 죽었다. 11-01 빌드라 지금 bundle과 맞지 않았던 것이라 보드에서 다시 빌드해 쟀다.
- 커밋: vta.cc(systolic·multi4·single 사본 동일, 단 single `alu1_epr`은 이 변경 전 빌드), multi4 SYS_BIN, `IMPL_TO_STEP`·`m4_build.sh`, `holdpin.tcl`, 문서(§7.15, CLAUDE.md, survey §5.3, y219 README).
- 다음은 bias reset(`sa_rmw`·tag, HLS, ISA pad bit, vtalib 명령 순서). multi4는 `m4_build.sh`로 처음 돌린다.

## 결정이 남은 것 (11-04 07:00 기준)
- 이미 결정: ALU II=1이 맞으면 bias reset을 한다(13:39). single·multi4 `SYS_BIN`을 ALU II=1 bitstream으로 바꿨다(16:17, 02:50). Mops는 모델 MAC × 2로 세고 ALU·데이터 이동은 세지 않는다(16:41). opt_design에서 멈추는 두 방법을 함께 쓴다(01:09).
- 다음 작업: bias reset 설계와 구현(02:50 "바로 이어서 시작"). `m4_build.sh`는 처음 쓰는 것이라 동작 확인이 필요하다.
- 사용자 답을 기다리는 것:
  - PC 꺼짐 원인 확인(BIOS 이벤트 기록, PBO·오버클럭, 쿨러·팬, 파워 용량). 원인은 미확정이다.
  - PC 디스크 여유 38 GB(96%): 오래된 Vivado 빌드 dir 정리 여부.
  - single `alu1_epr`(WNS +0.001, ①② 수정 전 코드)를 ①② 들어간 코드로 다시 빌드할지(기록상 결정 없음).
  - `prof_summary.py`가 `EVTA_REPLAY_LOG=2` 로그로 가동률을 자동 출력하게 할지(16:43 제안, 답 없음).
  - survey 다음 조사(§6의 1 HAL 초안, 2 컴파일러 비교 채우기)를 언제 할지.
