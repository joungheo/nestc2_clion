# 2026-10-22 EVTA synth 요약: 보드 멈춤의 원인은 cpuidle과 JTAG 충돌(`nocpuidle.sh`), reset 수정 `_ila3`를 ILA로 확인, `gemmprobe zero` 오류를 load의 wgt 1024비트 read-modify-write로 좁힘(`_ila6`은 GEMM #1에서 deadlock)

- 기간: 2026-10-22 07:00 ~ 10-23 07:00 (KST)
- 세션은 하나다. [PC]는 PC(호스트), 그 위의 docker 컨테이너 `vivado2020-build`(Vivado 2020.1), 호스트의 `hw_server`로 한 작업이다. 보드 명령과 Vivado Hardware Manager GUI 조작은 사용자가 직접 했고, 결과를 붙여 주었다. 커밋은 PC `main`에 했다.

## 시간순

**15:34 [PC] "어제 하던 작업을 이어서 하자. ila3 올려서 테스트 하던 중이었지. hw manager는 docker 안에서 띄워야 하지? 명령어 뭐야?"**
- 밤사이 꺼진 컨테이너와 hw_server(3121 포트)를 다시 켜고, Vivado GUI를 호스트 화면(:1)에 띄워 target `xczu9_0`까지 열었다.
- 직접 띄울 때 쓸 명령(hw_server → `docker start` → `docker exec ... vivado -source hwmgr_open.tcl`)을 정리했다. `hwmgr_open.tcl`은 이날 만든 파일이다(`vta-hw-zcu102-single/build/ila/`).
- 보드 ping을 시각과 함께 계속 기록하기 시작했다. `_ila3.bit`/`.ltx`를 program하고 1~2분 기다려 달라고 했다.

**15:50 [PC] "프로그램하고 2분 지났어."**
- 보드가 또 죽었다. program 끝 15:45:47, ping 끊김 15:48:59(약 3분 뒤, 그동안 PL은 아무 일도 하지 않음).
- JTAG로 죽은 상태를 읽었다.

  | 항목 | 값 | 의미 |
  |---|---|---|
  | A53 #2, #3 | No Power | 리눅스 cpuidle이 코어 전원을 내린 상태 |
  | A53 #0 | PC = `0xfffea120`, 레지스터 쓰레기 값, "APB AP transaction error" | 전원을 다시 올리던 중 멈춤 |
  | A53 #0 RVBAR | `0xfffea120` | ATF `bl31_warm_entrypoint` 첫 명령. 깨어나는 첫 명령도 실행 못 함 |
  | A53 #1 | 커널 안, 인터럽트 막힌 채 돎 | 코어 0을 기다리는 것으로 보임 |
  | PMU 에러, SYSMON 알람, IPI | 모두 0 | 과열·전원 이상 아님 |
  | 커널 로그(JTAG로 추출) | 부팅 157초 이후 기록 없음 | oops/panic 없이 조용히 멈춤 |

- 판단: 비트스트림 문제가 아니라 hw_server가 `arm_dap`으로 A53 디버그 레지스터를 계속 조회하는 상태에서 cpuidle이 코어 전원을 내렸다 올리다 굳은 것으로 보인다. Xilinx 문서로 직접 확인하지는 않았다.
- 어제·그제 보드가 죽은 사례(특히 `plreset.py` 후 레지스터를 읽다 멈춘 것)도 PL 때문이 아니었을 수 있다.
- 확인 방법: 재부팅 직후 cpuidle state1 이상을 끄고(WFI는 유지) 같은 조건으로 5분 대기.

**16:54 [PC] "16:49에 명령을 실행했어."**
- 중요: **cpuidle을 끄자 보드가 살아 있었다.** 16:47:20 부팅, 16:49:43 program 끝, 그 뒤 ping이 한 번도 끊기지 않았다. A53 #0~#3 모두 `Running`.
- 결론: 보드가 죽은 원인은 cpuidle과 JTAG 디버거의 충돌로 보는 게 가장 그럴듯하다. 앞으로 JTAG·ILA를 쓸 때는 부팅 직후 cpuidle을 꺼야 한다.
- 이어서 r2(reset 펄스 캡처), r3(reset 이후 상태) 순서를 안내했다.

**17:00 [PC] (r3: `plreset.py` 후 `dump_regs.py` 출력)**
- 레지스터 읽기가 이제 멈추지 않는다. r3 캡처도 깨끗하다(reset 5개 모두 1, out_mem EN 0, FIFO·AXI 조용).
- r2 파일이 없었다. r3 스크립트가 ILA를 먼저 멈춰 파형을 덮어써서, trigger가 걸렸는지 알 수 없었다. r2를 다시 하게 했다.
- reset 이후 load의 ctrl만 `0xe [done,idle,ready]`(시작하지 않았는데 한 번 끝난 상태)인 점이 눈에 띈다고 했다.

**17:08 [PC] "plreset.py 실행하니 full 상태로 가고 ... 파형도 새로 떴어."**
- 중요: **reset 구조 수정(`_ila3`)을 ILA로 확인했다.** 샘플 64가 trigger(`pl_resetn0` 하강) 시점이다.

  | 신호 | 변화 시점 | 해석 |
  |---|---|---|
  | `processing_system_reset` (pl_resetn0) | 64에서 1→0 | 펄스가 PL까지 도착 |
  | `pll_clk_locked` | 계속 1 | MMCM이 더 이상 reset되지 않음, 333 MHz 유지 |
  | `proc_sys_reset_peripheral_aresetn`, `_interconnect_aresetn` (333 MHz) | 74에서 1→0 | 10 cycle 뒤 333 MHz 도메인 reset |
  | `proc_sys_reset_ps_peripheral_aresetn` (100 MHz) | 100에서 1→0 | 100 MHz 쪽도 reset |
  | fetch `ARREADY`, HP0 `RREADY` | 84~85에서 0 | SmartConnect가 reset 상태로 들어감 |

- `_ila2`에서는 이 펄스가 MMCM까지 reset해 333 MHz 쪽이 클럭을 잃었다. 이제 두 도메인이 함께 reset되고, 그 뒤 레지스터를 읽어도 멈추지 않는다.

**17:15 [PC] (`gemmprobe zero` 출력, g1)**
- 결과는 `out: 0 0 0 1 0 0 0 0 76 0 -35 -1 63 8 0 0`, MISMATCH 6/16. 예전 `41 0 0 54 …` 패턴도, 미리 채운 0x5a도 아니다.
- trigger는 안 걸렸지만 ILA가 보여 준 최신 값에 실행 흔적이 남았다: fetch `ARADDR` `0x79d00000`, gemm_queue 마지막 FINISH, store_queue 마지막 명령이 gemmprobe의 INSTRUCTION 6과 비트까지 일치.
- 명령어 전달 경로는 정상이다. `_ila`에서 store_queue에 들어가던 쓰레기 STORE가 사라졌다(reset 수정 효과로 봄).
- trigger를 놓친 이유를 `ARVALID AND ARREADY` 조건으로 의심하고, 활동이 하나라도 있으면 trigger·캡처하는 `ila4_arm.tcl`을 만들었다.

**17:41 [PC] (`gemmprobe zero` 출력, g2: `-36`, 6/16 틀림)**
- ILA에 실행 전체가 잡혔다.

  | 행 | 동작 | 판정 |
  |---|---|---|
  | 0~21 | fetch가 명령어 10개를 HP0로 읽음, gemmprobe 덤프와 비트 일치 | ✅ |
  | 16~23 | compute가 LOAD UOP, GEMM, LOAD UOP, GEMM 수신 | ✅ |
  | 34, 88 | uop 읽기(`0x77d00000`), `acc=8 inp=8 wgt=8` | ✅ |
  | 130 | GEMM #1(reset): out_mem[8]에 `…107e feb9 0098 … 0200 0000` | 쓰레기여도 됨 |
  | 155, 199 | 두 번째 uop 읽기(`0x77d00008`) | ✅ |
  | 221 | GEMM #2: out_mem[8]에 `…083f ffdc 004c … 0100 0000` | ❌ 0이어야 함 |
  | 223~224 | store가 out_mem[8]을 읽어 DRAM에 씀 | 정상 |
  | — | LOAD INP(`0x77c4a000`)·LOAD WGT(`0x77c4b000`)의 AR이 HP0에 한 번도 안 보임 | ❌ |

- GEMM #2 값은 GEMM #1 값을 1비트 산술 시프트한 것이라 acc reset은 정상이고, inp·wgt가 0이 아닌 게 문제라고 봤다.
- 이때 판단은 "load가 DRAM을 읽지 않고 token만 push한다"였다(18:39에 틀린 것으로 정정).

**17:56 [PC] (`dump_regs.py` → `gemmprobe zero` → `dump_regs.py`)**
- 실행 후 load/compute/store ctrl 모두 `0x83 [start,done,auto_restart]`로 정상 대기, fetch `0x10=0xa`(명령 10개), `0x18=0x79d00000`. 0x20마다 반복은 AXI-lite 주소 미러로 정상.
- 같은 `zero` 재실행에서 lane 10만 `-36`→`-35`로 바뀌었다. SRAM 잔여값만 쓰인다면 매번 같아야 하므로 확인할 점이다.
- 보드 터미널에서 명령이 겹쳐 보이는 것은 셸의 터미널 폭 문제라 `checkwinsize`/`resize` 또는 짧은 `PS1`을 안내했다.
- `_ila4` 빌드 시작(implementation만, 합성 재사용). 새 probe: `load_queue` 출력, load `m_axi_data_port` AR/R, `inp_mem`·`wgt_mem_0/1` 쓰기 포트, compute의 inp·wgt 읽기 포트, `l2g`·`g2l` 양쪽. `nocpuidle.sh`(미커밋)와 `ila4b_arm.tcl`을 미리 만들었다.

**18:09 [PC] "vta-hw/를 분석하면서 아티팩트 evta HW 구조와 합성을 만들어 놓았어. ... 우리가 몰랐던 게 있는지 분석해줘." / 18:17 "반영해줘."**
- 중요: 실제 영향이 있는 새 항목은 **done 경쟁(#1)** 하나다. compute는 첫 명령을 받은 뒤 `l2g`/`s2g` token pop이 끝나야 `done = 0`을 쓴다(`vta.cc:466-482`). 드라이버는 11번 polling 뒤 done이 1이면 끝으로 본다(`pynq_driver.cc:326`). 그래서 첫 compute 명령이 load token을 기다리는 conv 번들에서는 **하드웨어가 도는 중에 `VTASynchronize`가 반환된다.**
  - `gemmprobe`는 첫 compute 명령이 pop 없는 LOAD UOP라 해당 없음.
  - 수정안: 비트스트림 그대로, 드라이버가 시작 전 compute `0x1c`(`done_o_ap_vld`, read-clear)를 비우고 "시작 후 vld를 봤고 `done_o == 1`"일 때만 끝으로 본다. load 버그가 풀린 뒤 번들을 돌리기 전에 넣기를 권했다.
- 문서의 미확인 항목에 답을 달았다: `gemm_from_output`은 번들이 항상 0을 넘겨 무해, #32는 `hw_spec.h`(8/6)로 해결, #23 uop 비트 배치는 ILA로 dst 확인(src·wgt는 `_ila4`로), #18 FIFO는 `$INSN_BYTES`(32B), pl_clk0 100 MHz와 계산 쪽 MMCM 333 MHz, 인스턴스 4개는 multi2/multi4에 반영.
- 18:17에 "EVTA 하드웨어 구조·재합성 노트"에 네 군데를 반영했다(done 항목, "전제 확인 결과 (2026-10-22)", #32·#23·#18 해결 문단, 새 섹션 "보드에서 찾은 것"). reset 수정은 `_ila3`에서만 확인했고 single/multi2/multi4 트리에는 아직 없다고 적었다.

**18:21 [PC] "vta hw 리뷰 가이드도 바뀌었을 수도 있으니 한번 확인해봐." / 18:27 "가이드에 반영해줘."**
- 가이드 내용은 그대로였고, 상태가 바뀐 finding이 여럿이었다: #1 코드로 확인, #2 `plreset.py` 복구 경로 확인, #19 우리 트리 수정(더 큰 MMCM reset 문제 별도), #12~18·#31·#32 single 트리 수정, #22 해소, 첫 #29·#30 사본별 수정 상태, #28 uop 캐시는 두 vtalib 사본 모두 미수정.
- 가이드 오류: #29·#30 번호 중복, 개요의 "HLS 합성 불가" 문구.
- 18:30 반영: 상태 칸 16개에 메모, 두 번째 #29·#30 → #33·#34, 새 행 **#35**(pl_resetn0가 MMCM까지 reset, 확인됨, 트리 미반영)와 **#36**(cpuidle과 JTAG 충돌, 확인됨), 표 위 설명 "36건", 개요에 2026-10-20부터 docker 합성·cosim·PL 시뮬레이션·ILA 사용 추가.

**18:27 [PC] `_ila4` 빌드 완료**
- WNS +0.008 ns로 모든 timing 제약 만족, probe·디버그 경고 없음. 테스트 순서(program → `plreset.py` → `ila4b_arm.tcl` → `gemmprobe zero` → 저장)를 안내했다.

**18:31~18:33 [PC] (`ila4b_arm.tcl` 오류 두 번)**
- 첫 오류: implementation 중 넷 이름이 바뀌어 probe가 `u_ila_0_load_queue_M_AXIS_TVALID`처럼 붙었다. 접두어·접미어를 허용하는 정규식으로 찾게 고쳤다.
- 두 번째 오류: Vivado `-regexp`가 패턴의 `/`를 계층 구분자로 해석했다. probe 목록을 받아 Tcl `regexp`로 직접 비교하게 바꾸고, 컨테이너 Tcl에서 매칭·제외 사례를 시험했다.

**18:35 [PC] (`_ila4`, g3: `out: 0 0 0 80 72 -128 4 0 0 1 0 -96 -128 0 -1 66`, 9/16 틀림)**
- 중요: **데이터가 틀어지는 곳은 load가 wgt SRAM에 쓰는 경로다.** 17:43의 "load가 DRAM을 읽지 않는다"는 판단은 틀렸다(그때는 load 쪽 신호가 캡처 조건에 없었다).

  | 행 | 동작 | 판정 |
  |---|---|---|
  | 20~29 | LOAD INP, LOAD WGT, NOP이 load_queue로 | ✅ |
  | 146~150 | compute가 g2l token push, load가 pop | ✅ |
  | 163~238 | LOAD INP: AR `0x77c4a000`, RDATA `100f0e…0201` (1~16) | ✅ |
  | 243 | inp_mem[8]에 `100f0e…0201` 기록 | ✅ |
  | 262~343 | LOAD WGT: AR `0x77c4b000` len 16, RDATA 16 beat 전부 0 | ✅ |
  | 332~364 | wgt_mem_0[8]에 8번 씀, DIN 하위 128비트가 매번 `20400000…00001006 00000010` | ❌ 0이어야 함 |
  | 456 | compute가 wgt_mem_0[8]에서 `20000010 00000800 00001004 00000000` 읽음 | ❌ |
  | 456 | GEMM 결과 out_mem 틀림 → 출력 9/16 틀림 | 결과 |

- HLS `load.v`는 wgt 한 칸(2048비트)을 1024비트 BRAM 워드 두 뱅크에 담으면서 바이트 WE 대신 **read-modify-write**(II=2, stage1에서 `Din = (Dout & ~mask) | (beat << lane)`, WE 전부 켬, 뱅크는 beat 홀짝)를 한다. inp는 칸 폭 = 버스 폭 128비트라 그냥 쓰고 정상이다.
- RMW의 `Dout`이 틀리면 이렇게 섞인다. 실제로 load가 쓴 값과 compute가 읽은 값이 다르고, 실행마다 비트가 OR로 쌓이는 모양이다. RTL 시뮬레이션은 통과했으므로 보드에서만 다른 부분이 있다.
- 후보: wgt BRAM의 `C_EN_SAFETY_CKT = 1`(읽기 지연 1, WRITE_FIRST, URAM 미사용). 확정은 아니다.
- `_ila5`(wgt_mem 포트 전체 폭, load RDATA) 빌드를 시작했다.

**19:35 [PC] "처음에 vta.h, vta.cc와 어떤 코드들을 넣고, vivado hls 명령어는 뭐고 ... 한 아티팩트로 정리해줘."**
- "EVTA 비트스트림 빌드 따라하기" 문서를 만들었다(https://claude.ai/artifact/TJKdvrtQuMSf8P8TUsDnFf). single no-prof 기준.
  - 준비(트리 경로, docker, 환경), 1단계 소스·설정(`vta.cc`, `vta.h`, `hw_spec.h`, `vta_config.json`), 2단계 `make ip`의 `vivado_hls`와 `hls.tcl`, 3단계 `vivado.tcl`의 블록 디자인(셀, 연결, 주소 맵), 4단계 `make bin`과 출력·체크리스트, 5단계 보드에 올리기와 `gemmprobe`, prof·multi2/multi4 차이 표, 자주 겪는 문제.
- single 트리에 reset 수정(#35)이 아직 없다는 것과 지금 vtahw 비트스트림의 `gemmprobe zero`가 틀린다는 것을 경고로 적었다.

**19:44 [PC] `_ila6` 빌드 완료**
- `_ila5`는 timing을 못 맞춰 쓰지 않았다(20:15 BOARD_TODO 기록). `_ila6`는 ILA 깊이를 1024로 줄여 WNS +0.055 ns로 만족했고, `ila5_arm.tcl`의 probe 12개가 `.ltx`에 모두 있다.
- ILA 빌드를 거친 순서와 이유:

  | 판 | 왜 만들었나 | 새로 본 신호 | timing | 결과 |
  |---|---|---|---|---|
  | `_ila3` | `pl_resetn0`가 MMCM까지 reset하던 구조 수정(전날) | reset, fetch, queue, out_mem | — | reset 정상(r2), 끝까지 돌지만 값 틀림 |
  | `_ila4` | load 쪽이 캡처 조건에 없어 "DRAM 안 읽음"으로 잘못 봄 | load_queue, load AR/R, inp/wgt 쓰기, compute 읽기, l2g/g2l | WNS +0.008 | wgt RMW에서 값이 깨짐 |
  | `_ila5` | RMW의 `Dout`이 실제 메모리 값인지 보려고 wgt_mem 전체 폭 | wgt_mem DIN/DOUT 1024비트, WE 128비트, port B DOUT 등 | timing 미달 | 쓰지 않음 |
  | `_ila6` | `_ila5`를 ILA 깊이 1024로 줄여 다시 | `_ila5`와 같음 | WNS +0.055 | 매번 GEMM #1에서 deadlock |

**19:46~19:47 [PC] (`ila5_arm.tcl` 오류 두 번)**
- 깊이 2048 고정 → `_ila6` 최대 1024라 오류. `MAX_DATA_DEPTH` 속성을 읽게 고쳤는데 그런 속성이 없었다(확인하지 않은 이름을 쓴 실수). 1024를 직접 넣었다.

**19:50 [PC] "여기서 멈추네. 마지막까지 g4 저장은 했어."**
- 캡처 조건에 큐 `TVALID`를 넣어 1024 샘플 중 1016개(load_queue), 1021개(gemm_queue)가 대기 구간으로 찼다. 캡처 조건을 `TREADY`로 바꾸고 `g2l TVALID`는 뺐다.
- 이번 실행은 멈췄다. compute가 명령을 몇 개 꺼냈지만 inp·wgt 읽기, out_mem 쓰기, g2l token push가 없고, load는 LOAD INP 뒤 g2l token을 기다렸다(`_ila4`와 다른 증상).

**19:54 [PC] (멈춘 상태 `dump_regs.py`, 그 뒤 `gemmprobe`가 `fpga_buff_ != nullptr`로 실패)**
- 실패 원인: Ctrl-C로 끝낸 실행의 CMA 버퍼(명령 핸들 하나가 128 MB 중 64 MB)가 해제되지 않았다. `xlnk_reset`이 필요하다.
- 멈춘 상태: load `0x81`(LOAD INP 뒤 g2l token 대기), compute `0x83`이고 done `0x18`=0(FINISH 미도달), store `0x81`. compute가 GEMM #1에서 g2l token을 push하지 않았거나 GEMM #1을 실행하지 않아 생긴 deadlock이다.

**19:57 [PC] "똑같이 멈추는 듯. ^C 하기 전에 g6 저장했어."**
- g4·g5·g6 CSV가 바이트까지 같다(md5 동일). g5·g6에는 새 캡처가 없다. g6은 arm 뒤에도 trigger가 걸리지 않았다.
- 멈춘 상태를 그대로 찍도록(`ila2_snap.tcl`, `dump_regs.py`, `dump_insns.py`) 순서를 안내했다.

**20:07 [PC] "pre trigger 상태야. g7 찍었어. trigger가 안 잡혀." (+ `dump_regs.py`, `dump_insns.py`)**
- trigger가 안 걸린 이유: BASIC 캡처 모드에서 trigger 위치(16) 앞 16 샘플을 먼저 채워야 하는데, 캡처 조건을 짧은 펄스(`TREADY`)로 바꾸자 그 전에 실행이 끝났다. g1도 같은 원인으로 본다. `TRIGGER_POSITION 0`으로 고쳤다.
- 중요: **`_ila6`는 매번 같은 곳에서 deadlock이다.** DRAM 명령 10개 정상(dump_insns), fetch는 다 분배하고 끝남, compute는 LOAD UOP, GEMM #1, LOAD UOP #2를 꺼낸 뒤 l2g token 대기, 그런데 GEMM #1 흔적(inp/wgt 읽기, out_mem 쓰기, g2l push)이 없고 load는 LOAD INP에서 g2l token 대기.
- 같은 RTL인데 `_ila4`는 끝까지 가고(값만 틀림) `_ila6`는 GEMM #1을 건너뛴다. **probe 구성만 다른 구현에 따라 동작이 달라진다.** timing은 둘 다 통과했으므로 timing 분석 밖(reset 안 되는 레지스터 초기값, 비동기 경로, BRAM safety circuit)을 의심한다. wgt 값 섞임과 원인이 같을 가능성이 크다고 봤다.

**20:11 [PC] "Post-Trigger로 바뀌네. pl 시뮬레이션은 내일 돌리자."**
- g8(7 샘플)에서 멈추는 지점이 보였다.

  | 샘플 | 동작 |
  |---|---|
  | 0 | fetch 명령어 읽기(AR) → trigger |
  | 1 | compute가 LOAD UOP #1을 꺼냄 |
  | 3 | load가 LOAD INP를 꺼냄 |
  | 5 | compute가 GEMM #1(reset)을 꺼냄 |
  | 6 이후 | 아무 활동 없음(inp/wgt/out_mem 접근, g2l push 없음) |

- compute가 GEMM #1 안에서 멈춘다. uop_mem 읽기 단계에서 멈췄는지, 앞선 LOAD UOP의 DRAM 읽기가 끝나지 않았는지는 `_ila6`에 uop 포트가 없어 구분할 수 없다.
- 다음 할 일: 보드에 들어간 load·compute 합성 넷리스트와 blk_mem_gen 넷리스트로 PL 시뮬레이션(`build/simbd` 흐름). 결과에 따라 HLS에서 wgt RMW를 없애거나 BRAM 설정을 바꾼다.

**20:14 [PC] "BOARD_TODO.md에 오늘 결과를 정리하고 nocpuidle.sh와 함께 커밋해줘." / 20:15 "PUSH 해줘."**
- `40ae09a0`으로 커밋하고 push했다(`origin/main` = `40ae09a0`).
- BOARD_TODO.md: cpuidle 원인과 부팅마다 `nocpuidle.sh`, reset 구조 버그는 `_ila3` 사본에서만 고침(single/multi2/multi4 `vivado.tcl`은 미수정), `_ila4` wgt RMW 손상과 "load가 DRAM을 안 읽는다"는 판단의 정정, `_ila5` timing 미달로 미사용, `_ila6` GEMM #1 deadlock, 다음 할 일, ILA 주의점(probe 이름 매칭, `TRIGGER_POSITION 0`, `TREADY` 캡처 조건, Ctrl-C 뒤 `xlnk_reset`).
- 같이 커밋: `vtabundles_xcmpl/hwtest/nocpuidle.sh`, `log/host_20261022/`(`ila4_gemmprobe_zero.csv`, `ila6_gemmprobe_zero_deadlock.csv`, reset 관련 열만 남긴 `ila3_plreset_pulse.csv`).
- 보드에서는 `git pull` 뒤 JTAG 쓰기 전에 `sudo ./vtabundles_xcmpl/hwtest/nocpuidle.sh`를 실행한다.

## 결정이 남은 것 (10-23 07:00 기준)
- 이미 결정: PL 시뮬레이션(보드에 들어간 합성 넷리스트 + blk_mem_gen 넷리스트)은 10-23에 돌린다(20:11).
- 그 결과에 따라 HLS에서 wgt RMW를 없앨지, BRAM 설정(`C_EN_SAFETY_CKT` 등)을 바꿀지.
- `_ila6` deadlock이 uop 읽기에서 멈춘 것인지 LOAD UOP가 끝나지 않은 것인지(compute uop 포트 probe가 필요, 미확인).
- reset 수정(#35)을 single/multi2/multi4 트리의 `vivado.tcl`에 넣을지(아직 `_ila3` 사본에만 있음).
- done 경쟁(#1)의 드라이버 수정(`0x1c` vld 확인)을 load 버그가 풀린 뒤 넣을지.
- uop 캐시(#28)는 두 vtalib 사본 모두 미수정.
