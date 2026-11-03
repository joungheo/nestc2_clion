# 2026-10-23 EVTA synth 요약: 보드 FPGA의 configuration 셀 불량(SLICE Y219 한 행 stuck-at-0) 발견, Y219를 피한 single 333 MHz 비트스트림이 처음으로 보드 통과(번들 35개), 드라이버 done 경쟁 수정

- 기간: 2026-10-23 07:00 ~ 10-24 07:00 (KST)
- 세션은 하나다. [PC]는 PC(호스트), 그 위의 docker 컨테이너 `vivado2020-build`(Vivado 2020.1), 호스트의 `hw_server`로 한 작업이다. 10-24 00:16까지는 보드 명령을 사용자가 직접 실행하고 결과를 붙여 주었다. 그 뒤로는 ssh 키와 sudo 설정이 생겨 PC에서 ssh로 보드 시험을 돌렸다. 커밋은 PC `main`에 했다.

## 시간순

**11:02 [PC] "PL 시뮬레이션 해줘."**
- 앞서 6개 케이스가 모두 멈춘 것은 시뮬레이션 연결 문제였다. 구현 단계가 상수 비트를 IP 경계 밖으로 옮겨서, 그 비트들이 Z로 떠 있었다.
- routed 넷리스트를 넣기 위해 세 가지를 고쳤다: phys_opt가 추가한 포트(reset 복제, FIFO EMPTY) 연결, BRAM 클럭 출력과 GND/VCC 출력 비트 구동, 사라진 상수 비트를 weak 드라이버로 넣기.
- 시뮬레이션 결과다. 보드에서 쓰는 load, compute, BRAM 로직을 단계별 넷리스트로 바꿔 6개 케이스를 돌렸다.

  | 시뮬레이션 | 대체한 부분 | 결과 |
  |---|---|---|
  | RTL (10/20) | 없음 | 6개 케이스 정상 |
  | 합성 넷리스트 (오늘) | load, compute, inp/wgt×2/out BRAM (보드 설계 ila3의 OOC 넷리스트) | 6개 케이스 정상, RTL과 같음 |
  | post-route 넷리스트 (오늘) | 같은 셀을 배선이 끝난 `_ila6` 설계에서 추출 | 출력값은 6개 모두 정상. 다만 실행이 4~7배 느림 |

- post-route에서 compute가 g2s token을 약 1000 cycle 동안 계속 내보냈다. 시뮬레이션 FIFO가 `lopt`(FIFO 원시 셀의 `EMPTY`)를 `~TVALID`로 근사한 탓일 수 있어 실제 동작이라고 단정하지 않았다. 보드 `_ila6`의 GEMM #1 deadlock은 재현되지 않았다.
- 남은 후보: 1) 명령·token FIFO(FIFO36E2/18E2, 구현이 `EMPTY`를 소비자 안으로 끌어들임), 2) PS 쪽 AXI 타이밍, 3) 물리적 요인.

**12:52 [PC] "셀 경계를 안 넘어가게 옵션을 주고 합성하면 안 돼? … DONT_TOUCH는 좋은 듯. queue를 BRAM으로 만들면 순서가 안 될텐데. … ILA를 잘 넣어서 봐야할텐데."**
- 셀은 넷리스트의 모든 인스턴스다(계층 셀 = HLS 4개·fifo_generator·blk_mem_gen 등, 원시 셀 = LUT·FF·RAMB36E2·FIFO36E2). BD의 IP는 OOC로 합성하므로 경계를 넘는 것은 **구현 단계**(opt_design의 `lopt`, phys_opt의 reset 복제, 상수 이동)다. BRAM 기반 FIFO도 순서는 지킨다.
- FIFO 구조는 그대로 두고 `_ila7`을 빌드했다: HLS 셀 4개와 큐 7개에 `DONT_TOUCH`, probe 52개(큐 pop/push, gemm_queue TDATA, compute uop AR/R, load의 wgt 쓰기 등). arm 스크립트 `ila/ila7_arm.tcl`.

**13:07 [PC] "picocon으로 접속하려는데 멈추네. 보드를 껐다 켜니 되는데 왜?" / 13:12 `nocpuidle.sh` command not found**
- 원인은 단정할 수 없다. 후보: 보드 리눅스가 이미 멈춤, 콘솔이 대기 상태, 포트를 잘못 고름(`ttyUSB4`는 JTAG 쪽). 다음에는 전원을 끄기 전에 ping/ssh를 먼저 확인하기로 했다.
- `nocpuidle.sh`는 보드에서 `git pull`을 하지 않아 없었던 것이다.

**13:20 [PC] `_ila7` 빌드 완료 / 13:49 "Hardware Manager를 어디서 실행해? 처음부터 자세히"**
- `_ila7`: WNS +0.023 ns. DONT_TOUCH로 경계 포트(`repN`/`lopt`)가 0개.
- "EVTA 비트스트림 빌드 따라하기" 문서의 JTAG 절을 다시 썼다. 단계마다 실행 위치(보드 / 호스트 / 컨테이너 / Vivado GUI) 표, 1~9단계 명령, 증상별 해결 표. Hardware Manager는 컨테이너 Vivado GUI에서 열고 `172.17.0.1:3121`의 호스트 `hw_server`에 붙는다.

**14:04 [PC] `_ila7`에서 `gemmprobe zero` 결과 (MISMATCH 12/16)**
- 중요: **DONT_TOUCH로 deadlock이 사라졌다.** 모든 명령이 순서대로 실행되고 token도 한 번씩만 오갔다. 그래서 `_ila6`의 deadlock은 구현 단계의 경계를 넘는 최적화 때문이라고 판단했다.
- ILA 캡처로 본 실행 단계별 판정이다.

  | 샘플 | 동작 | 판정 |
  |---|---|---|
  | 10~12 | GEMM #1: inp[8], wgt[8]을 읽고 out_mem[8]에 씀, g2l push | ✅ |
  | 15~16 | LOAD INP: `77c4a000`에서 `100f…0201` 읽음 | ✅ |
  | 17~34 | LOAD WGT: `77c4b000`, RDATA 16 beat 전부 0 | ✅ |
  | 24 | wgt_mem_0[8] 첫 쓰기: lane 0~1 = 0 | ✅ |
  | 28~52 | 나머지 7번 쓰기: lane 1에 `00000400 40(44)0000c3`가 계속 남음. 쓸 때마다 40과 44가 번갈아 나옴 | ❌ |
  | 64 | GEMM #2가 wgt[8]을 읽음: lane 1 = `00000400 000000c3` → 출력 12/16 틀림 | ❌ |

- 남은 문제는 wgt read-modify-write로 보고, 원인을 쫓는 대신 **wgt_mem을 128비트 × 16뱅크로 나눠 RMW를 없애자**고 제안했다.

**14:07 [PC] "진행해."**
- single 트리에 넣은 변경: `pkg_config.py`(zcu102에서 SRAM 뱅크 폭 = AXI 128비트 → wgt 128비트 × 16뱅크), `vivado.tcl` reset 수정 #35(clk_wiz `USE_RESET false`, `pll_clk/resetn`을 pl_resetn0에서 뺌), 구현 전용 XDC로 DONT_TOUCH.
- HLS에서 load의 wgt RMW가 없어졌고, RTL 시뮬레이션 alu/zero/ident/perm/ones OK(reset 케이스 MISMATCH는 의도된 동작).
- 비트스트림: WNS +0.026 ns, BRAM 258/912, `.bin` md5 `3911d84c…`. 이전 결과는 `..._old_rmw`로 옮겨 두었다.

**14:54~15:00 [PC] wgt16 보드 시험: CMA 할당 실패, 그 뒤 커널 패닉**
- 첫 실행은 모두 `fpga_buff_ != nullptr` 실패였다. CmaFree가 64876 kB로, Ctrl-C로 끝낸 실행이 CMA를 남긴 것이다. `xlnk_reset` 뒤 129728 kB로 돌아왔다.
- 다시 돌린 `gemmprobe zero`에서 보드가 **커널 패닉**으로 재부팅됐다(`kernel write to read-only memory at ffffffc000200000`, `gem_rx → __swiotlb_unmap_page → __dma_inv_area`). 이더넷 수신 버퍼 주소가 덮어써진 것이고, 그 시점에 DRAM에 쓸 수 있는 것은 EVTA다.
- 올린 방법별 결과를 비교했다. 올리는 방법이 원인일 수 있다고 보고(미확인) JTAG로 올려 보기로 했다.

  | 비트스트림 | 올린 방법 | 결과 |
  |---|---|---|
  | `_ila` | fpga_manager (.bin) | 쓰레기 STORE, 멈춤, 보드 죽음 |
  | `_ila2`~`_ila7` | JTAG (.bit) | 명령어 정상(값 문제나 deadlock은 별개) |
  | 이번 wgt16 | fpga_manager (.bin) | 멈춤, 커널 패닉 |

**15:02 [PC] "hw manager에 7.bit 올려둔 게 켜져 있는데 충돌했나?" / 15:35 JTAG로 올리고 JTAG를 끊은 뒤 실행해도 같은 패닉**
- Hardware Manager 연결이 주원인일 가능성은 낮다고 답했다(JTAG 조회는 읽기만 함).
- JTAG로 올려도 같은 주소·같은 경로의 패닉이 났다. 올리는 방법이나 Hardware Manager 때문이 아니다.
- 지금까지 빌드별 결과다.

  | 빌드 | 합성 | 결과 |
  |---|---|---|
  | 10/20 no-ILA | 전체 | 값이 틀림 |
  | `_ila` | 전체 | 쓰레기 STORE, 보드 죽음 |
  | `_ila4` | ila3 합성 + 구현 | 값이 틀림(wgt RMW) |
  | `_ila6` | ila3 합성 + 구현 | GEMM #1에서 deadlock |
  | `_ila7` (+DONT_TOUCH) | ila3 합성 + 구현 | 끝까지 돌고 wgt만 틀림 |
  | wgt16 (+DONT_TOUCH, +reset) | 새 합성 | 메모리를 덮어써서 커널 패닉 |

- 모든 빌드의 WNS가 +0.008~+0.055 ns라 timing 마진을 가장 강한 후보로 보고, 같은 설계를 200 MHz로 구현했다(WNS +0.281 ns, md5 `92c3bbfa…`).

**16:07~16:09 [PC] 200 MHz 결과: 패닉은 없고 값은 틀림, 틀린 lane이 항상 같음**
- `zero`를 두 번, `ident`, `perm`, `ones`를 돌렸다. 모든 케이스가 MISMATCH 6/16이고, 두 번 돌린 `zero`는 같은 값이다.
- 중요: **16 lane 중 0, 2, 3, 7, 8, 14번만 틀리고 나머지 10개는 정확하다.** 결정적이고 클럭을 낮춰도 같으므로 timing이 아니라고 보았다. 아래는 `ident` 결과다.

  | lane | 0 | 1 | 2 | 3 | 4~6 | 7 | 8 | 9~13 | 14 | 15 |
  |---|---|---|---|---|---|---|---|---|---|---|
  | ident 기대값 | 2 | 4 | 6 | 8 | 10~14 | 16 | 18 | 20~28 | 30 | 32 |
  | ident 결과 | **40** | 4 | **109** | **97** | 10~14 | **0** | **-75** | 20~28 | **-66** | 32 |

- 16뱅크에서는 lane `oc` = wgt 뱅크 `oc`다. 틀린 6개 뱅크는 모두 BRAM Y39 이상을 포함하고, 맞는 10개는 Y38 이하에만 놓였다. 가설: 이 보드 FPGA의 일부 BRAM(또는 영역) 고장. Y36 이상 BRAM을 금지한 200 MHz 빌드(`_lowbram`)를 시작했다.

**16:15 [PC] "1024bit으로 쓸 때도 같은 위치에서 에러가 났나?"**
- `_ila7`(1024비트 RMW)의 wgt_mem_0 BRAM 셀(셀당 16비트)과 캡처 결과를 맞춰 보았다.

  | 비트 | BRAM 위치 | 캡처 결과 |
  |---|---|---|
  | 0~31, 48~127 (lane 0) | X5Y24~28, X6Y27~30 | ✅ 0 |
  | 32~47 (lane 0) | **X6Y39** | ✅ 0 |
  | 128~191 (lane 1) | X7Y31, X7Y33~35 | ✅ 0 |
  | 192~207 (lane 1) | **X7Y39** | ❌ `00c3` |
  | 208~223 (lane 1) | **X7Y45** | ❌ `4000`/`4400` |
  | 224~239 (lane 1) | **X8Y36** | ❌ `0400` |
  | 240~255 (lane 1) | X8Y33 | ✅ 0 |

- 깨진 셀은 모두 Y36~45에 있었고, X8Y36과 X7Y45는 두 빌드 모두에서 깨진 쪽이었다. 다만 경계가 깔끔하지는 않았다(X6Y39는 정상).

**16:21 [PC] "BRAM이 불량이라고는 믿기 힘드네. BRAM만 읽고 쓰는 실험을 해 보자."**
- 기존 비트스트림으로 돌리는 `hwtest/sramscan.cpp`를 만들었다. `wgt` 모드는 wgt 2048칸 전체에 패턴을 쓰고 GEMM으로 읽는다. `inp` 모드는 단위 wgt로 inp 4096칸을 꺼낸다. `invert` 옵션이 있다.
- `_lowbram`(200 MHz): RAMB36 256개 모두 Y35 이하, WNS +0.386 ns, md5 `6e796082…`.
- **200 MHz wgt16(위쪽 BRAM 사용)**: 뱅크 0, 2, 3, 7, 8, 14만 칸 2048개 전부에서 바이트의 97% 이상이 틀렸다(뱅크당 31966~32730 / 32768, 뒤집힌 비트 OR 0xff). 나머지 10개 뱅크는 오류 0, 합계 194241바이트. 셀 하나의 고장이 아니라 뱅크 전체가 틀렸다(뱅크 3은 Y35 셀까지 틀림).
- 중요: **`_lowbram`에서는 모두 통과했다.** `sramscan wgt` 0 / 524288바이트, `inp` 0 / 4096행, `gemmprobe` 4개 OK. 바뀐 것은 Y36 이상 BRAM 금지 하나뿐이다.
- `_lowbram`의 로직 배치도 확인했다. 설계가 아래쪽 클럭 영역 0~2행에 모였기 때문에, 이 빌드만으로는 BRAM만의 문제인지 영역 전체의 문제인지 가를 수 없다.

  | 클럭 영역 행 | SLICE Y 범위 | LUT/FF 수 | BRAM |
  |---|---|---|---|
  | 0 | 0~59 | 18047 | 있음 |
  | 1 | 60~119 | 17908 | 있음 |
  | 2 | 120~179 | 26371 | 있음 |
  | 3 | 180~239 | 1180 | 없음(금지) |
  | 4~6 | 240~419 | 0 | 없음 |

**16:45 [PC] "Y36 이상에서 클럭이나 reset이 제대로 연결되고 동작하는지 확인할 수 있어?"**
- 333 MHz wgt16 빌드의 배선 결과로 정적 검사를 했다. 이상은 없었다.

  | 항목 | 결과 |
  |---|---|
  | 배선 상태 | 배선 가능한 넷 64471개 모두 배선 완료, 오류 0 |
  | EVTA 클럭 | MMCM `MMCM_X0Y3` → `BUFGCE_X0Y74` → 클럭 루트 `X1Y2`. 위쪽 영역(X0~X3, Y3~Y5)까지 같은 전역 클럭으로 분배됨 |
  | 클럭 skew | 아래쪽 BRAM(X6Y31, 뱅크 8): skew -0.180 ns, slack +0.422 ns. 위쪽 BRAM(X3Y50, 뱅크 13): skew -0.035 ns, slack +0.713 ns. 위쪽이 오히려 여유가 더 큼 |
  | BRAM reset 넷 | 16개 뱅크 전부 배선 완료(ROUTED) |
  | PS 위치 | `PS8_X0Y0`(왼쪽 아래). PS에서 온 클럭은 `BUFG_PS_X0Y65`를 거쳐 MMCM으로 들어감 |

**17:11 [PC] "지금 bit 파일에 있는 로직 중에 위쪽 영역 reset이 잘 되는지 확인할 수 있는 로직이 있을까?"**
- HLS 블록 4개의 AXI-lite 레지스터(동기 reset으로 0)를 쓰는 `hwtest/regtest.py`를 만들었다. 333 MHz wgt16(레지스터 FF가 클럭 영역 3행, SLICE Y180~190)에 올려 EVTA를 시작하지 않고 시험했다.
- 결과: 레지스터 7개 × 패턴 6종의 쓰기/읽기 OK, `plreset` 뒤 모두 0, ctrl `0x4`. 위쪽 영역에서도 클럭, reset, 일반 FF는 동작한다.

**17:21 [PC] "BRAM 전수 시험 설계를 만들고 single 333MHz 빌드도 시작하자."**
- single 트리 `vivado.tcl`에 zcu102 기본으로 RAMB36 Y36 이상과 RAMB18 Y72 이상 금지를 넣었다(`VTA_ALLOW_UPPER_BRAM=1`로 끔). 333 MHz 빌드 결과: RAMB36 256개 모두 Y35 이하, WNS +0.018 ns, md5 `2fa610a9…`.
- BRAM 전수 시험 `brambist`(`hwtest/brambist/`): BRAM36 912개를 각자 `LOC`에 고정하고 패턴 4종을 port A로 쓰고 port B로 읽는다. 100 MHz, WNS +0.489 ns, md5 `2b1def62…`.

**17:47 [PC] "위쪽 BRAM 읽고 쓰는 로직에 문제가 있는지 확인해 보는 방법이 있을까? 한 칸 shift 된 경우도 있었잖아."**
- `sramscan dump`와 `sramscan_analyse.py`를 만들었다. 틀린 값이 주소 밀림, 바이트 밀림, 뱅크 섞임, 이전 값 남음, 비트 고장 중 무엇과 맞는지 검사한다.
- 200 MHz wgt16 결과는 다섯 가설 어디에도 맞지 않았다. 처음에는 "기대값 + 칸마다 다른 상수"로 보고 acc가 0이 되지 않는다는 가설을 세웠다.
- 정정: 덤프 전체로 검사하니 "다음 구간 상수 = 이전 상수 + 이전 W"가 맞는 비율이 28672개 중 약 110개였다. 우연 수준(≈112)이라 **acc 누적 가설은 틀렸다.** 틀린 lane의 값은 거의 무작위다.

**18:03 [PC] BRAM 전수 시험 v1 결과와 PL 전압**
- v1은 5회 모두 912개 중 607개 BRAM에 오류가 났고, 아래쪽(Y35 이하) X5~X12 열도 대부분 오류였다. 여러 BRAM에서 같은 비트(`0x802`)가 틀렸다. `_lowbram` EVTA 결과와 맞지 않아, 시험 회로 문제나 전원 문제를 의심했다.
- 부하 없는 상태에서 SYSMON으로 PL 전압을 읽었다. 모두 정상이다(값 = raw × scale).

  | 전원 | 측정값 | ZU9EG 규격(-2, 공칭) |
  |---|---|---|
  | VCCINT (PL 로직) | 848 mV | 850 mV (825~876) |
  | VCCBRAM | 847 mV | 850 mV (825~876) |
  | VCCAUX | 1794 mV | 1800 mV |

- v2를 빌드했다. `EN_LO`/`EN_HI`로 BRAM 범위를 고르고, 반복 모드를 넣었다. `brambist.py`에는 `all`/`columns`/`load` 모드가 있다. 100 MHz, WNS +0.423 ns, md5 `833aa1a8…`.

**18:17 [PC] "single 빌드가 끝났잖아. 해보자." / 18:24 "1 push 까지 해줘."**
- 333 MHz single lowbram 비트스트림의 보드 시험이다. 직접 빌드한 EVTA 비트스트림이 이 보드에서 처음으로 동작했다.

  | 시험 | 결과 |
  |---|---|
  | `sramscan` wgt / wgt invert / inp / inp invert | 오류 0 |
  | `gemmprobe` zero, ident, perm, ones | 모두 OK |
  | `gemm_nc` (vtalib 기본 GEMM 테스트) | Golden Matched |
  | `add_test_nc` (ALU) | 16/16 일치 |

  처음 awk로 센 "17 pairs, 1 mismatches"는 디버그 출력 한 줄이 섞인 것이다. 마지막 16줄로 다시 세니 0 mismatches였다.
- 커밋 `e8864207`을 push했다. 내용은 single `vivado.tcl`/`pkg_config.py`, `sramscan`, `sramscan_analyse.py`, `regtest.py`, `brambist/`, `log/BOARD_TODO.md` 10-23 절이다.

**18:28 [PC] "done 경쟁 드라이버 수정하자. 내용도 comment에. 원 소스가 틀렸다는 거지?"**
- 중요: **원 소스의 드라이버가 틀렸다.** compute는 설계대로 동작한다. `done_o`는 마지막 값을 유지하는데, `Run()`은 시작하자마자 `done_o == 1`이면 끝났다고 본다. ETRI의 `t > 10`은 판정을 10번 읽는 동안 미룰 뿐이다. upstream TVM 드라이버와는 대조하지 않았다.
- 수정: 시작 전에 `done_o_ap_vld`(0x1c, 읽으면 지워짐)를 비운다. vld가 켜진 뒤에만 `done_o == 1`을 끝으로 인정한다. `dump_regs.py`는 0x1c를 읽지 않게 했고, 경쟁을 일부러 일으키는 `hwtest/donerace.cpp`를 만들었다.
- 옛 드라이버와 새 드라이버를 같은 lowbram 비트스트림에서 비교했다.

  | | 옛 드라이버(`_old`) | 새 드라이버(`_nc`) |
  |---|---|---|
  | `donerace` | **16회 중 13회 틀림** | **16/16 OK** |
  | 실행 1회 시간 | 0.022 ms | 0.105 ms |
  | `sramscan` wgt/inp | — | 오류 0 |

  0.105 ms는 weight 512 KB load 시간과 맞는다. 5.0 GB/s로, 333 MHz × 16 B의 이론값 5.3 GB/s에 가깝다. 이전 보드 실행 중 conv 번들처럼 첫 compute가 긴 load를 기다리는 경우의 결과는 다시 확인이 필요하다.
- 18:37 "커밋과 push도 진행해줘.": submodule `vta/vtalib` `163b3b0`(push `cd5590c..163b3b0`), 메인 `d07c292e`(vtalib 복사본 3개, `donerace.cpp`, `dump_regs.py`, submodule 포인터, BOARD_TODO).

**18:41~18:59 [PC] BRAM 시험 v2: ID 레지스터가 틀림**
- `brambist.py`가 "v2 bitstream not loaded"로 멈췄다. 10000번씩 읽어도 매번 같은 값이었다.

  | 레지스터 | 기대값 | 읽은 값 |
  |---|---|---|
  | ID | `b1a57002` | `b1a5f042` |
  | N | `00000390` | `000083d0` |
  | EN_LO | `0` | `00008040` |
  | EN_HI | `00000390` | `000083c0` (비트 4도 틀림) |
  | CTRL | `0` | `00008040` |

- 비트 6과 15가 고정적으로 1이다. 그 비트의 SRL은 `SLICE_X41Y179`와 `SLICE_X34Y183`에 있고, 같은 슬라이스의 다른 비트는 정상이다. v1에서는 같은 회로가 ID를 정상으로 읽었다. **같은 RTL인데 배치마다 다른 곳이 고정적으로 고장 난다.** 그래서 v1의 "607개 불량"은 믿을 수 없다고 판단했다.
- 가장 의심되는 원인으로 FPGA에 올라간 configuration이 비트스트림과 다를 가능성을 꼽고, JTAG readback으로 비교하기로 했다.

**19:02~19:24 [PC] JTAG readback: configuration 셀 불량 확정**
- 처음 `verify_hw_devices`는 `MASK_FILE` 속성 설정이 막혀 실패했다("No maskfile assigned"). 이후 readback 데이터를 직접 비트스트림과 비교했다.
- 중요: **readback으로 이 보드 FPGA의 configuration 메모리 셀 불량을 찾았다.** 실행 중 바뀌는 비트를 mask로 뺀 뒤 비교했다. 아래는 확인 단계별 결과다.

  | 확인 | 결과 |
  |---|---|
  | BRAM 시험 v2, fpga_manager로 로드 | **5000비트 차이, 모두 1→0.** clock region row 3 프레임의 word 61~62 = **타일 한 행, SLICE Y219**(RAMB36 Y43). 그 행에서 1이어야 할 82570비트 중 약 5000개(6%), 프레임 2689개(장치 가로 거의 전체, 프레임 20586~27350) |
  | 몇 분 뒤 다시 읽기 | 같은 5000비트 |
  | fpga_manager로 다시 로드 | 같은 5000비트, 빠진 것·새로 생긴 것 0 |
  | JTAG로 프로그램 후 readback | 같은 비트가 틀림 → `.bin` 변환·로드 경로 문제 아님 |
  | 다른 설계(EVTA lowbram) | 194비트 차이, 모두 1→0, 같은 행. v2에서 0이던 셀 중 EVTA가 1을 쓰는 178개가 하나도 빠짐없이 0. 반대 경우 0개 |
  | 위치 정리 | 프레임마다 같은 8개 비트(word 61 bit 18, word 62 bit 0~3·12~14), stuck-at-0 |

- 이로써 그동안의 증상이 설명된다. 같은 RTL인데 빌드마다 증상이 다르고, 시뮬레이션과 timing은 깨끗한데 보드에서만 고정적으로 틀렸다. BRAM 시험 v1에서 Y43 행은 13개 열 전부가 오류였다. "위쪽 BRAM 전체"가 이 한 행만으로 다 설명되는지는 이 시점에 미확인이었다.
- readback 도구 `readback.tcl`, `progrb.tcl`, `cfgdiff.py`를 `hwtest/brambist/`에 넣었다.

**19:27 [PC] "커밋하고 push 해줘. single이 제대로 동작하는지 Conv4Test 해 보자. Slice Y219를 피하는 빌드도 시작해 줘."**
- 커밋 `5f068573`(readback 도구와 configuration 불량 기록)을 push했다.
- Conv4Test는 lowbram 333 MHz 비트스트림과 done을 고친 드라이버로 돌렸다(`pynq_driver.cc.o` 시각 10:32 UTC = 19:32 KST로 재컴파일 확인). 이전 기록과 비교했다.

  | | 이번 (vtahw lowbram + done 수정 드라이버) | 이전 기록 (etri 비트스트림 + 옛 드라이버) |
  |---|---|---|
  | 결과 | PASS (골든과 일치) | — |
  | 측정 실행 (run 2) | 3.507 ms, 65.94 GOPS | 3.501 ms, 66.05 GOPS |
  | 워밍업 (run 1) | 23.3 ms | 5.7 ms |

  워밍업 23 ms는 비트스트림을 올린 직후 첫 실행이라 그런 것으로 추정했고, 확인하지는 않았다.
- `vivado.tcl`에 `VTA_BRAM_FENCE=y219`를 넣었다. PS를 뺀 PL 전체를 SLICE Y218 이하 pblock에 넣고 CONTAIN_ROUTING을 켜며, MMCM을 row 3~6에서 금지한다.

**20:04~20:35 [PC] Y219 회피 빌드 (2회)**
- single 비트스트림이 보드를 통과하기까지 거친 빌드 순서다. 단계마다 그때의 의심 원인을 하나씩 바꿨다.

  | 판 | 바꾼 것 (이유) | 클럭 / WNS | 보드 결과 |
  |---|---|---|---|
  | `_ila7` | HLS 셀·큐 DONT_TOUCH (`_ila6` deadlock이 경계를 넘는 최적화 때문으로 의심) | 333 / +0.023 | 끝까지 돌고 wgt만 틀림 |
  | wgt16 | wgt 128비트 × 16뱅크, reset #35 (RMW 제거) | 333 / +0.026 | 커널 패닉 |
  | wgt16_clk200 | 클럭만 200 MHz (timing 마진 의심) | 200 / +0.281 | 패닉 없음, lane 0·2·3·7·8·14 틀림 |
  | wgt16_clk200_lowbram | RAMB36 Y36 이상 금지 (틀린 뱅크가 위쪽에 놓임) | 200 / +0.386 | 모두 통과 |
  | single lowbram | 같은 금지를 실제 트리에 넣음 | 333 / +0.018 | 모두 통과, Conv4Test PASS |
  | `_y219try1` | `VTA_BRAM_FENCE=y219` pblock (불량은 Y219 한 행) | 333 / +0.047 | 쓰지 않음: cascade wgt BRAM 쌍 2개가 경계를 넘어 RAMB36 Y43에 놓이고 배선 26개가 Y219 PIP 사용 |
  | Y219 | pblock + Y219 행 사이트 PROHIBIT(RAMB36 Y43, RAMB18 Y86~87, DSP Y86~87) | 333 / +0.004 | 모두 통과 (아래) |

- 최종 Y219 빌드다. 이전에 금지했던 위쪽(Y36~42)을 다시 쓰면서 Y219 행만 피했다.

  | 항목 | 결과 |
  |---|---|
  | timing (333 MHz) | WNS +0.004 ns, 통과(여유는 작음) |
  | RAMB36 배치 | 244개, 최대 **Y42**. Y43(Y219 줄)은 사용 안 함 |
  | **Y219 줄의 배선 스위치를 쓰는 넷** | **0개** |
  | 이전 "위쪽 불량"으로 금지했던 Y36~42 사용 | RAMB36 42개, 그중 **wgt 39개** |
  | `.bin` md5 | `62c3f9e3…` |

**20:36 [PC] "네가 직접 할 수 있어?" / 10-24 00:08 `ssh-copy-id` "No identities found" / 00:16 "둘 다 했는데 되나 해봐."**
- 호스트에 키가 없어서 `~/.ssh/id_ed25519`를 패스프레이즈 없이 만들었다. 사용자가 키를 등록하고 보드에 `/etc/sudoers.d/evta-test`를 설정했다. python3와 sh가 들어 있어 사실상 root 전체 권한이라는 점을 알렸다.
- 이후 보드 시험은 PC에서 ssh로 돌렸다. Y219 single 비트스트림의 하드웨어 시험 결과다.

  | 시험 | 결과 |
  |---|---|
  | sramscan wgt / wgt invert / inp / inp invert | 4개 모두 0 bytes wrong |
  | gemmprobe zero / ident / perm / ones | 4개 모두 0/16 wrong |
  | donerace 16회 | 16회 모두 정상 |
  | gemm_nc | Golden Matched |
  | Conv4Test | PASS, 3.504 ms, 65.98 GOPS (이전 lowbram 빌드는 65.94 GOPS) |

- `run_all.sh`로 번들 35개를 돌렸다. 기준 출력이 있는 30개(Conv, ResConv1~10, FC, Elem, Pool, Quantize, SoftMax, Transpose 등)는 모두 PASS였다. 나머지 모델 5개의 결과다.

  | 모델 | 결과 | 추론 시간 |
  |---|---|---|
  | Resnet18Test | 281 (cat_285 → tabby cat) | 134.4 ms |
  | Resnet18PartitionTest | 281 | VTA partitions 142.5 ms |
  | Resnet50Test | 281 | 1439.2 ms |
  | Vgg16Cifar10Test | 0 (airplane) | 44.5 ms |
  | Resnetv10S1C0Test | 정상 실행 | 3.54 ms |

- 중요: wgt BRAM 39개가 Y36~42에 있어도 오류가 0이다. 그래서 **"위쪽 BRAM 불량"은 실제로는 SLICE Y219 한 행 때문이었다**고 결론지었다. 앞으로는 위쪽 영역 전체가 아니라 Y219 행만 피한다.

**10-24 00:21 [PC] "진행해줘." (커밋, multi2/multi4 적용)**
- 커밋 `6fa74ee1`을 push했다. single `vivado.tcl`의 zcu102 기본값을 `VTA_BRAM_FENCE=y219`로 했고(다른 값 `upper`, `none`), `BOARD_TODO.md`와 `CLAUDE.md`에 적었다.
- 판정 도구 `hwtest/brambist/stuckchk.py`를 만들었다. 비트스트림이 stuck 셀 자리에 넣는 1의 개수를 센다. 정상 동작한 두 빌드가 공통으로 넣는 194비트(쓰지 않는 tile 설정)를 기준선으로 삼아 "추가 비트 0"이면 합격으로 본다. 기존 비트스트림 9개로 확인했다.

  | bitstream | 기준선 대비 추가 비트 | 보드 결과 |
  |---|---|---|
  | single lowbram, single Y219 | 0 | 정상 |
  | y219try1 | +37 | 실패 |
  | old_rmw / wgt16_upper / prof | +561 / +2132 / +3088 | 실패 |
  | 기존 multi2_prof / multi4_prof | +2037 / +5183 | 문제 있음 |

- multi2/multi4(333 MHz, PROFILE 없음)에 single의 수정 3가지를 옮겼다. multi4는 Y219 아래 RAMB36이 379개뿐이라, 처음에는 위·아래 두 영역으로 나눈 `split` fence로 빌드했다.
- `sramscan`과 `donerace`가 버퍼 깊이를 `hw_spec.h`에서 가져오게 고쳤다. EVTA 0~3용 시험 프로그램과 `multi_gemm_gemm`을 빌드했고, `build_multievtax2` 번들은 done 수정 드라이버로 다시 빌드했다.

**10-24 01:01 [PC] "지금까지 디버깅 한 거 정리해서 문서로 … 아티팩트로 만들자. 나중에 ppt로도 만들거야."**
- 보고서 아티팩트 "EVTA vtahw 비트스트림 보드 디버깅 보고서"(https://claude.ai/artifact/4KvtghuqpzV1GKunEyTRdf)를 만들었다. 9개 섹션, 흐름 그림 1개, 표 8개, 실험 22개를 담았다.
- 두 가지는 판단으로 표시했다. wgt RMW와 DONT_TOUCH가 단독으로 얼마나 문제를 일으켰는지는 보드 결함 때문에 가려지지 않았다. "위쪽 BRAM 불량"을 배선이 Y219 행을 쓴 탓으로 보는 설명은 추정이다.

**10-24 01:48~03:44 [PC] multi2 보드 통과, multi4 미해결**
- 첫 빌드는 둘 다 `stuckchk`에 걸렸다.

  | 빌드 | timing | RAMB36 | 기준선보다 많이 쓴 비트 | Y219 행 PIP를 쓰는 net |
  |---|---|---|---|---|
  | multi2 | 통과 (WNS +0.055 ns) | 286 | +34 | 30개 |
  | multi4 | 실패 (WNS −0.119 ns, 435 endpoint) | 572 | +26 | 69개 |

  원인은 세 가지였다.
  1. `get_sites -regexp`에 사이트 종류를 묶어 주면 첫 종류(SLICE)만 돌려준다. 그래서 RAMB36 PROHIBIT이 빠졌다.
  2. 떨어진 직사각형 두 개로 만든 pblock은 배선을 막지 못했다.
  3. multi4에서 Y219 행을 쓴 69개 net은 전부 PS HP2/HP3와 위쪽 EVTA를 잇는 경로라 pblock으로 가둘 수 없다.

  PROHIBIT을 종류마다 한 줄로 나눴고, EVTA 2개 이하면 기본을 `y219`로 했다.
- **multi2 재빌드:** WNS +0.049 ns, RAMB36 286개 전부 Y219 아래, `stuckchk` 추가 비트 0.
- multi2 보드 시험이다. EVTA별 시험과 동시 실행을 확인했다.

  | 시험 | 결과 |
  |---|---|
  | EVTA 0, 1: `sramscan` wgt/inp/각 invert | 0 byte wrong |
  | EVTA 0, 1: `gemmprobe` / `donerace` | 4/4 / 16/16 |
  | `multi_gemm_gemm_2evta_m` | 두 EVTA 모두 Golden Matched |
  | MultiEVTAx2ConvTest | PASS |
  | Resnet18 2 EVTA × 100회 | 매번 281, 15.2 s |
  | Resnet18 1 EVTA × 100회 | 매번 281, 13.6 s |

- 처음 돌린 multi `sramscan` wgt는 거의 모든 바이트가 틀렸다. 시험 프로그램 버그였다(acc 4096행을 쓰는데 multi acc는 2048행). `CHUNK = ACC_DEPTH/16`으로 고쳤다.
- 커밋 `3091bd8f`(`stuckchk`, `sramscan`/`donerace` 수정)와 `50201ac5`(multi2 fence, BOARD_TODO, CLAUDE.md)를 push했다. 비트스트림은 보드 `bit/evta_multi2_y219_261024/`와 `/lib/firmware/vtahw_multi2_y219.bin`에 있다.
- **multi4:** 하위 pblock에 CONTAIN_ROUTING을 걸었더니 배선 충돌(DRC RTSTAT-6)로 비트스트림이 나오지 않았다. 필요한 RAMB36은 572개인데 Y219 아래에는 379개뿐이다. PS에서 EVTA 2·3으로 가는 net은 그 행을 가로지른다. multi4의 `vivado.tcl`/`pkg_config.py` 수정은 커밋하지 않고 작업 트리에 두었다.

## 결정이 남은 것 (10-24 07:00 기준)
- **multi4를 이 보드에서 어떻게 할지 (사용자 답 대기).** 선택지는 네 가지다.
  1. RapidWright로 Y219 행의 배선 노드를 막고 다시 배선한다. 가장 확실하지만 도구를 새로 들여와야 한다.
  2. 문제 net만 반복 재배선하고 매번 `stuckchk`로 확인한다. 성공 보장이 없다.
  3. 버퍼를 줄여 RAMB36 379개 안에 넣는다. vtalib와 번들도 바꿔야 한다.
  4. 다른 ZCU102를 쓴다.
- 보드 `/etc/sudoers.d/evta-test`를 언제 지울지. multi 시험을 PC에서 계속 돌리려면 그때까지 남겨 둬야 한다. 호스트 `~/.ssh/id_ed25519`는 패스프레이즈가 없다.
- 보드 담당자에게 PL configuration 불량(SLICE Y219 행)을 알리는 일.
- 옛 드라이버로 돌렸던 이전 보드 결과(conv 번들처럼 첫 compute가 긴 load를 기다리는 경우)는 다시 확인해야 한다.
- 이미 결정된 것:
  - single과 multi2는 zcu102 기본으로 `VTA_BRAM_FENCE=y219`를 쓴다. wgt 128비트 × 16뱅크, clk_wiz reset 제거, DONT_TOUCH는 유지한다.
  - done 경쟁 드라이버 수정은 submodule과 복사본 3개에 커밋했다.
  - 앞으로 비트스트림은 보드에 올리기 전에 `stuckchk`로 판정한다.
