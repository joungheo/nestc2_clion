# 2026-10-21 EVTA synth 요약: ILA로 fetch가 엉터리 명령을 내보내고 compute가 혼자 도는 것 확인, 원인을 PL reset(MMCM이 `pl_resetn0`에 연결)으로 좁힘, `_ila3`는 올린 직후 PL idle 정상

- 기간: 2026-10-21 07:00 ~ 10-22 07:00 (KST)
- 세션은 하나다. [PC]는 PC(호스트), 그 위의 docker 컨테이너 `vivado2020-build`(Vivado 2020.1), 호스트의 `hw_server` 2020.2로 한 작업이다. 보드 명령과 Vivado Hardware Manager GUI 조작은 사용자가 직접 했고, 결과를 붙여 주었다.

## 시간순

**11:55 [PC] "HP 포트 쪽 레지스터 주소가 두 칸씩 밀린 건 지금 에러가 나고 있는 것과 상관 없어?"**
- 상관없다고 답했다. 틀린 주소는 `log/BOARD_TODO.md`의 안내 문장 한 곳에만 있었고, 코드·비트스트림·드라이버 어디에서도 쓰지 않는다(저장소 전체 검색).
- 주소를 고친 `plclk.py`로 보드 세션이 읽은 결과(`a23de542`): vtahw와 etri 비트스트림 모두 HPC0/1, HP0~3의 읽기·쓰기 폭이 128비트이고, pl_clk0는 99.999 MHz다. 포트 설정은 원인이 아니다.
- 보드에서 올라온 새 결과(전날 요청한 실험)다.

  | 실험 | 결과 |
  |---|---|
  | 200 MHz 비트스트림 | 더 나쁨. no-prof 333 MHz에서는 통과하던 ALU(`add_test_nc`)까지 모두 멈춤 |
  | 333 MHz no-prof GEMM garbage | 실행마다, SRAM 인덱스(0/1/8/100)·입력·weight와 무관하게 항상 같은 16바이트 패턴 → STORE가 고정된 값을 내보냄 |
  | prof | compute가 첫 시작 신호 전부터 done 상태(`0x0e`) → reset 직후 idle로 시작하지 못하는 것으로 보임 |
- 모두 PL 시뮬레이션에서는 정상이던 동작이고, 같은 IP인데 구현 run마다 증상이 다르다. 다음으로 reset 경로와 구현 결과를 보기로 했다.

**11:58 [PC] "`log/BOARD_TODO.md`의 2026-10-20 board 결과를 보고 ILA 디버그 빌드를 만들어 줘. …" / 12:00 "보드를 켰으니 JTAG 확인해줘. vivado 화면이 호스트 화면에 떠?"**
- 333 MHz no-prof 설정(보드에서 고정 garbage가 나던 설정)으로 ILA 빌드를 시작했다. 요청한 3개(`gemm_queue`, compute out_mem 쓰기, store out_mem 읽기)에 `store_queue`, `g2s_queue`를 더했다.
- JTAG 케이블 드라이버 설치는 sudo가 필요해 사용자가 했다. 첫 시도는 하위 스크립트를 상대 경로로 불러 실패했고, 폴더 안에서 다시 실행해 성공했다(12:09~12:11).
- 컨테이너의 Vivado 2020.1에서 호스트 `hw_server` 2020.2(`172.17.0.1:3121`)로 체인을 읽었다: `xczu9_0`(PL), `arm_dap_1`. GUI 창은 호스트 화면에 뜬다.
- 구현을 다시 돌리며 probe를 15개(856비트)로 직접 지정했다(`build/ila/ila_impl.tcl`, git 밖).

**12:40 [PC] ILA 디버그 비트스트림 완성 / 12:48 보드 주소 / 12:50 "dst dir를 bit/evta_ila_261021 로 해줘. 앞으로도 …"**
- `_ila` 빌드 결과는 아래 ILA 빌드 표에 있다. `.bin` md5 `ff614e35…`.
- 보드 주소를 192.168.20.2로, 보드로 보내는 파일은 `bit/<목적>_<YYMMDD>` 폴더에 넣기로 하고 둘 다 메모리에 저장했다.

**12:54 [PC] "보드에서 bit 올렸어." → 13:15~13:17 "멈춰버려." / "트리거는 안 잡히고 계속 기다리고 있고."**
- fpga_manager로 올린 `_ila`를 arm하고(trigger: `gemm_queue` 첫 handshake) `gemmprobe zero`를 돌렸는데, gemmprobe가 멈추고 trigger가 끝내 안 걸렸다. compute가 첫 명령(LOAD UOP)도 받지 못했다는 뜻이다.
- Trigger Immediate로 멈춘 상태를 찍은 결과(13:24, 사용자가 GUI에서 읽음):

  | 신호 | 값 | 의미 |
  |---|---|---|
  | `gemm_queue` TVALID / TREADY | 0 / 0 | compute로 가는 FIFO가 비어 있음 |
  | `store_queue` TVALID / TREADY | **1** / 0 | store FIFO에 명령이 있는데 store가 꺼내지 않음 |
  | `g2s_queue` TVALID | 0 | compute → store 토큰 없음 |

  명령 순서상 STORE는 compute 명령 뒤라서, fetch가 명령을 잘못 읽는다고 추정했다.

**13:30 [PC] "저장했어. iladata.csv"**
- 중요: store FIFO 맨 앞 명령은 STORE `dram=0x04020002 sram=2 x=10240 y=13312`(dependency 비트 모두 0)로, 런타임이 보낸 STORE(`dram=0x077c4f00 sram=8 x=y=1`)와 전혀 다른 엉터리 명령이었다. store가 이 거대한 STORE를 처리하느라 TREADY 0으로 전체가 멈춘다.
- 고정 garbage, prof·200 MHz 멈춤을 "fetch가 보는 명령 스트림이 깨져 있고, 깨지는 방식이 run마다 다르다"로 설명했다.
- DRAM의 명령 버퍼를 덤프하는 `hwtest/dump_insns.py`를 추가했다. 캡처 CSV와 분석을 `log/BOARD_TODO.md`에 적어 push했다(`e578a163`).

**14:04 [PC] "gemmprobe 실행하던 창은 멈춰있고, 새로 연 창에서는 접속이 안 되네." / 14:18 "PL Power Status OFF … End of startup status: LOW … 여기서 올려도 되는 거긴 하지?"**
- 보드가 멈춰 전원을 껐다 켜기로 했다. 그동안 fetch·HP0의 AR/R 채널을 더한 `_ila2` 빌드를 시작했다.
- JTAG로 Hardware Manager에서 직접 올려도 된다고 답했다. `.bin`이 아니라 `.bit`(`.bin`은 fpga_manager용으로 바이트 순서를 뒤집은 파일이라 `End of startup status: LOW`의 원인일 가능성이 큼)를 쓰고, 리눅스 부팅이 끝난 뒤 Refresh → Program 한다. probe 파일은 `.ltx`.

**14:28 [PC] `_ila2` 완성 / 14:51~14:53 arm 스크립트 오류 두 번**
- `ila2_arm.tcl`(trigger: fetch의 첫 명령 읽기 요청)과 `ila2_save.tcl`을 만들었다.
- 스크립트 실수 두 개를 고쳤다: X 비교값을 probe 폭만큼 쓰지 않음, `CONTROL.TRIGGER_MODE`는 이 ILA(`C_ADV_TRIGGER false`)에서 read-only.

**15:02 [PC] "저장했어. 근데 다 0 인 듯."**
- 이 CSV는 gemmprobe 실행 장면이 아니었다. arm 후 4분간 trigger가 없어 사용자가 중지했고, 그 전에 표시돼 있던 즉시 캡처가 저장된 것으로 보인다.
- 중요: 그래도 두 가지가 보였다.
  - fetch가 DRAM 읽기 요청을 전혀 하지 않았다(캡처 전체에서 ARVALID=0).
  - compute가 명령 없이 혼자 돌고 있었다. out_mem 쓰기 포트가 EN=1, WE=ffff로 계속 켜져 있고, 주소 0에 4사이클마다 1씩 증가하는 값을 쓴다.
- 가설: **비트스트림을 올린 뒤 PL reset이 제대로 걸리지 않는다.** `Xlnk().xlnk_reset()`은 CMA 해제 함수이고 pl_resetn0를 움직이지 않는 것으로 알고 있으며, JTAG로 올리면 reset 펄스가 없다. FIFO는 구성 직후 reset이 필요하다. PL 시뮬레이션은 reset을 제대로 걸어서 정상이었다고 보면 앞뒤가 맞는다.
- 즉시 캡처 `ila2_snap.tcl`을 추가했다.

**15:09 [PC] `dump_regs.py` 출력 / 15:12 `plreset.py` 실행 → 15:22 레지스터 reset 테스트에서 다시 멈춤**
- `dump_regs.py`: fetch/load/compute/store 모두 ctrl=0x4 [idle].
- pl_resetn0를 EMIO GPIO로 펄스하는 `hwtest/plreset.py`를 push했다(`9c4d5afc`). 보드에서 `gpio 511, chip base 338`로 동작했다.
- 보드 비트스트림에 실제로 들어간 compute 합성 netlist(DSP·BRAM 프리미티브 344개)로 PL 시뮬레이션: reset 후 idle에서 out_mem EN이 2000사이클 중 0번, alu·zero·ident·perm·ones 모두 OK. 합성 단계의 로직 변형은 원인이 아니다.
- 레지스터 reset 테스트(compute ctrl `0xA0002000`):

  | 단계 | 결과 |
  |---|---|
  | `0x80` 쓰고 읽기 | `0x84` (정상) |
  | `plreset.py` | `pulsed low, now 1` |
  | 다시 읽기 | AXI 읽기가 끝나지 않아 보드 멈춤, Hardware Manager의 ILA가 사라졌다 다시 뜸, trigger 시작이 무조건 잡힘 |

- 중요: BD를 다시 보니 reset 구조에 문제가 있었다. `pl_resetn0`가 클럭을 만드는 MMCM(clk_wiz)의 reset에도 연결돼 있어서, 펄스 때 EVTA 333 MHz 클럭이 멈췄다 다시 lock된다(ILA가 사라진 이유). 333 MHz 쪽 `proc_sys_reset`은 그 클럭으로 동작하고, 레지스터 포트 쪽 `proc_sys_reset_ps`(100 MHz, timing 때문에 분리했던 것)는 정상 reset된다. AXI interconnect 클럭 변환기 양쪽 reset이 어긋나 읽기가 멈춘 것으로 보인다. 비트스트림을 올린 직후에도 333 MHz 쪽 블록·FIFO가 reset되지 않았을 수 있고, 지금까지의 증상과 맞는다.

**15:23~16:00 [PC] `_ila3` 빌드 (MMCM reset 분리 + reset probe)**
- `pll_clk`(MMCM)를 `pl_resetn0`에서 뗐다(`USE_RESET false`). reset 신호 probe 5개(`pl_resetn0`, MMCM `locked`, 333 MHz 쪽 `peripheral_aresetn`·`interconnect_aresetn`, 100 MHz 쪽 `peripheral_aresetn`)를 더했다.
- ILA 삽입 단계에서 `debug.xdc` 폴더를 만들지 않아 한 번 멈췄고, 고쳐서 구현만 다시 돌렸다.
- 확인 순서를 정했다: r1 올린 직후 즉시 캡처 → r2 `pl_resetn0` 하강 trigger(`ila3_resettrig.tcl`)로 `plreset.py` 캡처 → r3 reset 이후 캡처와 `dump_regs.py` → `gemmprobe zero`.

**ILA 디버그 빌드 (모두 333 MHz no-prof 기반, 단계마다 관찰 범위를 넓히거나 가설을 확인하려고 다시 빌드)**

| 판 | 왜 만들었나 | 바뀐 점 | timing | 자원 |
|---|---|---|---|---|
| `_ila` | 보드 garbage가 compute 쓰기·store 읽기 중 어디서 생기는지 | probe 15개(856비트), depth 2048, storage qualification | WNS +0.030 ns, WHS +0.011 ns | LUT 11.0 %, BRAM 304/912 (ILA 약 50) |
| `_ila2` | store FIFO의 엉터리 명령이 DRAM 쪽인지 fetch 읽기 경로인지 | fetch AR/R(RDATA 256)과 HP0 AR/R(RDATA 128) 추가 | WNS +0.003 ns | BRAM 332/912 |
| `_ila3` | reset이 333 MHz 블록까지 가는지, MMCM reset 분리로 고쳐지는지 | MMCM을 `pl_resetn0`에서 분리, reset probe 5개 추가 (합성부터 다시) | 한 경로만 −2.059 ns: 100 MHz `proc_sys_reset_ps` 출력을 333 MHz ILA가 관찰용으로 샘플링하는 경로라 설계 동작과 무관. 나머지 만족 | — |

**17:51 [PC] "보드에 접속이 안 돼" / 17:54 "PL Power Status OFF … ssh는 되는데 program하려니 에러"**
- 보드 ping 100% 손실, ARP `INCOMPLETE`. `plreset.py` 뒤 멈춘 상태로 추정하고 전원을 다시 켜기로 했다. USB-UART(J83)를 호스트에 꽂아 두면 다음 멈춤 때 커널 메시지를 볼 수 있다고 제안했다.
- program 에러는 Vivado가 전원이 꺼졌던 때의 장치 상태를 들고 있어서로 보고, `close_hw_target` → `open_hw_target` → refresh 순서를 안내했다.

**18:00 [PC] "r1 캡처 했는데 갑자기 … target이 죽어버리네."**
- 캡처는 17:58에 끝났고, 17:59:08에 호스트에서 JTAG USB(Digilent, SN 210308B3B575)가 끊겨 `usb1-port6: Cannot enable … unable to enumerate`만 반복됐다. ping도 응답이 없었다. 지난번처럼 리눅스만 멈춘 게 아니라 보드 전원이 내려갔거나 컨트롤러가 셧다운했을 가능성이 크다고 봤다(과열·전원 레일 이상은 후보, 미확인).
- 중요: r1 캡처는 `_ila3`가 올린 직후 깨끗한 idle임을 보여 준다.

  | 신호 | `_ila2` 올린 직후 | `_ila3` 올린 직후(r1) |
  |---|---|---|
  | compute out_mem EN / WE | 1 / ffff, DIN이 계속 바뀜 | **0 / 0** |
  | store out_mem EN | 쓰레기 값을 읽음 | **0** |
  | reset 5개 (`processing_system_reset`, `pll_clk_locked`, `*_aresetn` 3개) | (관측 안 함) | 모두 1, 정상 해제 |
  | fetch AR, HP0 AR/R, 각 FIFO | 0 | 0 (조용함) |

  MMCM reset 분리가 적어도 초기 상태는 바로잡은 것으로 보인다. 보드가 꺼진 것과의 관계는 아직 모른다(r1 구간에서 PL은 아무 일도 하지 않았다).

**18:10 [PC] "LED 여러 군데 들어와 있고 팬도 돌고, 빨간·주황 LED 없음. … 다시 켜서 open target 해도 안 열려." / 18:20 lsusb 결과**
- 보드는 다시 살아났지만(ping 0.1 ms) JTAG USB가 port6에서 계속 enumeration에 실패했다. 케이블이나 호스트 포트 문제로 보고, 다른 포트를 권했다.
- 다른 포트(bus 3)에 꽂은 뒤 체인에 `xczu9`, `arm_dap`과 `PS TAP`, `PMU`, `PL` target이 정상으로 보였다.
- 보드는 18:07에 켠 뒤 program 없이 계속 살아 있어, 저절로 꺼지는 문제는 아닌 것으로 본다. 호스트에서 보드 ping을 1초 간격으로 시각과 함께 기록하기 시작했다.
- 다음 순서: hw_server 재연결 → `_ila3` program → 1~2분 대기(꺼지면 `_ila3` 비트스트림 자체가 원인) → 살아 있으면 r2.

## 결정이 남은 것 (10-22 07:00 기준)
- `_ila3` program 후 1~2분 대기 결과 대기. 그다음 r2(reset 펄스 캡처), r3(reset 이후 캡처 + `dump_regs.py`), `gemmprobe zero` 캡처가 남았다.
- 17:59 보드가 꺼진 원인은 미확인(전원 글리치 추정). USB-UART(J83)를 호스트에 연결할지 사용자 답 대기.
- `dump_insns.py`(DRAM 명령 버퍼 덤프)는 아직 보드에서 돌리지 못했다.
- ILA 스크립트(`ila_impl.tcl`, `ila2_arm.tcl`, `ila2_snap.tcl`, `ila3_resettrig.tcl` 등)는 git 밖 `vta-hw-zcu102-single/build/ila/`에 있다.
- 이미 정해진 것: 보드 주소 192.168.20.2, 보드로 보내는 파일은 `bit/<목적>_<YYMMDD>` 폴더. JTAG로 올릴 때는 `.bit` + `.ltx`를 리눅스 부팅 뒤에 쓴다. JTAG 케이블은 호스트 bus 3 포트에 꽂는다.
