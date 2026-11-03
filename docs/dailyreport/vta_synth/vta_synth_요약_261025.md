# 2026-10-25 EVTA synth 요약: Y219 결함은 행 안의 routing PIP(관통 wire는 정상), "Y219 PIP 0개" multi4 보드 첫 통과(280 → 309.6 → 333 MHz), Conv4 4-EVTA 489.6 GOPS

- 기간: 2026-10-25 07:00 ~ 10-26 07:00 (KST)
- 세션은 하나다. [PC]는 PC에서 multi4 비트스트림(Y219 결함 행 회피)을 만든 세션이고, 보드 실험은 이 PC에서 ssh로 돌렸다.
- 보드는 이날 여러 번 멈췄다(AXI 응답 없음 2회, 레지스터 테스트 1회, JTAG 사용 2회). 보드 멈춤의 원인은 아래 항목마다 적었다.

## 시간순

**07:21 [PC] 배치부터 다시 한 결과 (전날 요청의 1번)**
- 배치를 다시 해도 333 MHz에서 WNS가 0 이상이 되지 않았다. 가장 나은 후보는 여전히 `rs_h`다.

  전날부터 이어진 multi4(Y219 위아래로 EVTA를 나눈 fence) 333 MHz 타이밍 비교다.

  | 설계 | WNS | TNS | 위반 endpoint | 가장 나쁜 경로 | 고장 셀 사용 |
  |---|---|---|---|---|---|
  | `rs_h` (`routed_d` 기반, 고장 PIP 수정 후) | −0.092 | −0.673 | 51 | EVTA3 `load_queue_3` FIFO36 사이 | 0 net, stuckchk extra 0 |
  | `routed_x` (ExtraTimingOpt로 다시 배치) | −0.076 | −1.257 | 50 | `proc_sys_reset` → EVTA3 `store_queue_3` FIFO | 아직 수정 전 (Y219 PIP net 209개) |

- 판단: 배치를 바꿀 때마다 −0.08~−0.09 ns 위반이 다른 곳에서 나온다. 제안: 1) MMCM 분주만 바꿔 약 325 MHz로 낮춤(추천), 2) seed/directive를 바꿔 333 MHz 재시도.

**09:38 [PC] "1번으로 해보자"**
- 클럭을 낮춘 `rs_m`(322 MHz)과 클럭 변경 전 `rs_h`(333 MHz)로 보드에서 `gemmprobe ident`를 돌렸다. 중요: **보드가 멈췄다**(ssh, ping 무응답).

  | bitstream | EVTA0 | EVTA1 | EVTA2 | EVTA3 |
  |---|---|---|---|---|
  | `rs_m` (322 MHz) | 멈춤 (sramscan, gemmprobe, donerace 모두 결과 없음) | (중단해서 못 봄) | | |
  | `rs_h` (333 MHz, 클록 변경 전) | 멈춤 | **정상 (0/16 wrong)** | 시작하자마자 보드 전체가 멈춤 | 못 봄 |

- 클록 변경 탓은 아니다(333 MHz판도 EVTA0이 멈춤). 로드와 레지스터 idle 읽기는 정상인데, EVTA0은 FINISH가 오지 않는다.
- 가설(미확인): RapidWright가 다시 깐 배선 일부(46 net, EVTA2 CONTROL_BUS 포함)가 Vivado 검사는 통과하지만 보드에서는 동작하지 않는다.

**12:55 [PC] "새로 부팅했어. 해봐."**
- `rs_m`: EVTA1 통과, EVTA3은 `gemmprobe` 시작 시 보드 전체가 다시 멈췄다(두 번째).
- 부팅 직후 레지스터 읽기에서 EVTA0 compute ctrl bit 28이 1, EVTA1 store ctrl idle bit가 0으로 틀렸다.
- JTAG readback으로 PL 설정은 원래 있던 193비트 외에는 bitstream과 같았다.
- 틀린 두 비트의 net(`compute_0` RDATA[28], `store_1` RDATA[2])은 **둘 다 Y219 행을 지난다.** RapidWright가 다시 깐 net이 아니고 순수 Vivado 배선이며, 고장 셀 PIP도 쓰지 않고, 여유는 setup +1.4 ns, hold +0.3 ns다. read-data net 384개 중 Y219를 지나는 것이 95개라서 우연일 확률은 약 6%다.
- 다음 방향으로 A(버퍼를 줄여 4개를 모두 Y219 아래에, 추천), B(불량 배선 찾기), C(다른 ZCU102)를 제안했다.

**13:34 [PC] "A로 가자. … 그 전에 보드를 다시 켰으니 한번 더 실험 해보자. 정말 그런지."**
- EVTA를 시작하지 않고 주소 레지스터에 패턴(0, 0xFFFFFFFF, 0xAAAAAAAA, 0x55555555)을 써서 다시 읽었다(`regpat.py`). EVTA3 load 레지스터 단계에서 보드가 다시 멈췄다(세 번째).

  | block | 틀린 비트 | 그 비트의 net이 Y219를 지나는가 |
  |---|---|---|
  | EVTA0 fetch / load / store | 없음 | |
  | EVTA0 compute | bit 28 (1로 읽힘) | 예 (RDATA[28]) |
  | EVTA1 fetch / load / compute | 없음 | |
  | EVTA1 store | bit 2, 4, 9, 22, 26, 30 (0으로 읽힘) | 예, 6개 모두 |
  | EVTA3 fetch | bit 14 | 예 (WDATA[14]가 Y219를 넘어 위로 가는 가지) |

- 틀린 비트 8개가 모두 Y219를 지나는 net이었고, 아래에만 있는 net은 하나도 틀리지 않았다.
- A안 빌드를 시작했다: 버퍼 uop/inp/wgt/acc = 15/15/17/16(기존 16/15/18/17), 333 MHz, CONF `zcu102_1x16_i8w8a32_15_15_17_16_4evta`. vtalib `NESTC_EVTA_Y219_SMALL`, hwtest `UOP=15 BUFF="15 17 16"`도 맞췄다.

**14:01 [PC] "위쪽도 쓰면서 Y219를 안 지나게 하는 방법은 없어? EVTA2, 3를 실험하다가 멈추는 이유는 뭐지? …"**
- Y219는 소자 전체 폭을 가로지르고 PS 포트는 모두 그 아래에 있어서, 위쪽 EVTA는 어떤 신호든 행을 건너야 한다. 대신 "검증된 wire로만 건너기"를 제안했다(wire 전수 검사 bitstream → good 목록 → RapidWright 배선).
- 보드가 멈추는 이유는 없는 주소 접근(DECERR 응답)이 아니라 **AXI 응답(BVALID/RVALID)이 오지 않는 것**으로 추정했다. ZynqMP는 기본으로 timeout이 없다. `rs_m`에서 `load_3` BVALID/RVALID, `compute_2/3` AWREADY/WREADY/BVALID, HP2/3 handshake 등 응답 신호가 Y219를 지난다. 보드를 멈추지 않으려면 AXI Timeout Block을 켜는 방법이 있다(주소는 UG1087에서 확인 필요).
- 파일 창 dir 문제는 세션이 `cd`로 하위 dir에 들어갔기 때문이었다. 앞으로 절대 경로만 쓴다.

**14:17 [PC] "A안 빌드를 하다가 PC가 꺼졌어. … wire 전수 검사용 bitstream을 만들어서 검사부터 해보는 게 어때. …"**
- PC 꺼짐: 로그가 14:05에 끊겼고 종료 기록, 커널 오류가 없다. 하드웨어 보호(THERMTRIP 또는 PSU 과부하)로 추정한다. Vivado가 job 8개로 16코어를 거의 다 쓰던 IP synthesis 중이었다. 조치로 docker를 `--cpus 8`로 제한했다.
- 검사 대상: Y219를 위아래로 관통하는 INT node 20,328개(행 안에서 시작·끝나는 20,064개는 제외). 생성기 `WireTest.java`, `WireTestBit.tcl`을 만들었다.
- JTAG readback `-capture`가 이 소자에서 지원되지 않아, 도착 쪽을 FF 대신 SRL로 바꿨다(LUT config 셀이라 일반 readback으로 읽힘).
- JTAG로 PL을 프로그램한 직후 보드가 또 멈췄다(네 번째).

**14:53 [PC] "보드가 켜져 있는데. … 앞으로는 꺼지고 나서 좀 있다가 다시 켜졌는지 한번 더 검사해봐." / 15:08, 15:20 "뭘 하고 있어?"**
- 보드는 멈춘 뒤 스스로 재부팅됐다. 보드 로그가 05:32:07에 끊기고(`last`에 `crash`) 05:36:18에 새로 부팅됐다. PS watchdog(`cdns-wdt`)으로 약 4분 뒤 재부팅된 것으로 추정한다(메모리에 저장).
- 보드 sshd 로그에 외부 IP(8.130.162.112)가 root로 비밀번호 로그인을 계속 시도한 기록이 있다. 보드 관리자에게 공유기·방화벽 확인을 권했다.
- SRLC32E 도착 쪽 생성기, `wtb_small_0/1.bit`(node 200개), readback 해석 `wiretest.py`를 만들었다.
- 15:16~15:30에는 auto 모드 권한 검사기 오류로 Bash가 모두 거부되어 실행하지 못했다.

**15:25 / 15:29 [PC] (사용자가 PC 터미널에서 직접 실행)**
- `!` 접두어 안내가 틀렸다(일반 bash에서는 결과를 뒤집음). `!` 없이 실행하니 가짜 readback 검증은 `bad nodes: 0 of 200`으로 통과했다.
- 보드 검사 `wiretest_run.sh`는 로드(`operating`)와 JTAG readback까지 하고, 그 뒤 보드가 무응답이 되었다.

**15:35 [PC] "보드가 안 살아나네. 껐다 켰어. …"**
- 중요: JTAG 프로그램·readback 후 보드가 멈춘 원인은 **`nocpuidle.sh`를 실행하지 않은 것**이었다(10-22에 밝혀 BOARD_TODO에 적어 둔 문제). 검사 설계 탓이 아니었다. `wiretest_run.sh`가 항상 먼저 실행하게 고쳤고 메모리에 저장했다. 그 뒤 보드는 멈추지 않았다.
- 작은 검사(X30~X36 열, NN12/SS12 node 200개): 출발값 0/1 모두 불량 0개.
- 전체 정적 검사 설계 A(11,704)와 B(8,334)를 빌드했다. SRL pin 어긋남 24개와 미배선 290개는 빠졌다.

**16:17 [PC] 정적 검사 결과: 관통 node 20,010개 모두 정상**
- A와 B를 합쳐 20,010개 node가 0/1 고정값 검사를 모두 통과했다.
- `rs_m`에서 틀렸던 `store_1` 비트의 관통 node도 통과했다. 즉 wire가 끊겨 0/1에 고정된 것은 아니다.

  | 틀렸던 비트 | 그 net이 쓰던 관통 node | 이번 검사 |
  |---|---|---|
  | store_1 RDATA[2] | X34Y216 NN4_W_BEG6 | B, 정상 |
  | store_1 RDATA[4], [9], [26] | X34Y215 NN12_BEG5 / 3 / 1 | A, 정상 |
  | store_1 RDATA[30] | X35Y218 NN4_E_BEG2 | B, 정상 |
  | store_1 RDATA[22], compute_0 RDATA[28] | Y219 안에서 시작하거나 끝나는 node | 검사 대상 아님 |

- 남은 가능성으로 속도 결함과 이웃 신호 영향을 들고, 실제 속도 검사를 제안했다.

**16:19 [PC] "속도 검사부터 해보자."**
- 출발 SRL이 32비트 패턴을 순환시키고 도착 SRL이 받는다. 늦으면 1비트 회전된 값이 된다. `pl_clk0`는 `plclkset.py`로 바꾸고 멈춘다.
- 작은 검사(node 300개, X30~X36의 NN12/SS12): 100 / 150 / 187.5 / 250 / 300 / 375 / 500 MHz 모두 회전 0. 데이터 지연은 Vivado 계산 0.7~1.0 ns다.
- 전체 속도 검사(설계 4개 × 주파수 4개)를 보드에서 돌렸다.

**16:50 [PC] "다음 항목들을 다 해 본거야? … xdc 등에 옵션들 잘 넣으면 해결될 수도 있지 않을까" (고장 비트를 쓰는 PIP만 막기, 비트 역추적, `IS_ROUTE_FIXED`)**
- 1번(고장 비트에 1을 쓰는 PIP만 막기)은 이미 했고, 그 결과가 보드에서 실패한 `rs_m`이다(stuckchk 0 extra).
- 2번은 prjuray `zynqusp/segbits_int.db`로 했다. 고장 offset {2, 16~19, 28~30}에 1을 쓰는 PIP가 INT PIP 3,778종 중 606종이다. 이 목록이 `blk_f`의 extra 85비트를 위치별 분포까지 정확히 예측했다.
- `IS_ROUTE_FIXED`는 확인하지 않았다(dummy 차단 방식은 그만둠).
- 결론: 같은 wire가 빈 검사 설계에서는 정상이고 multi4에서는 틀리므로 xdc·배선 옵션 문제가 아니다.
- 16:55: 틀린 값을 받은 FF가 모두 Y220~Y232에 있어 leaf clock이 Y219를 관통한다는 가설을 추가했다(미확인).

**17:05 [PC] 전체 속도 검사 결과와 `rs_m` 재시험**
- 관통 wire는 750 MHz까지 모두 통과했다. 레지스터 테스트를 다시 했다(보드 정상).

  | bitstream | 틀린 비트 |
  |---|---|
  | **rs_m (이번)** | `compute_0` bit 28만 틀림. **`store_1`은 이번에는 정상** |
  | rs_m (오전) | `compute_0` bit 28, `store_1` bit 2/4/9/22/26/30 |
  | routed_d (순수 Vivado, 고장 비트 135개 남음) | 거의 모든 블록에서 bit 2가 1로 고정, EVTA1 fetch/compute는 대부분 비트가 틀림 |

- `compute_0` bit 28은 매번 틀린다. 이 net은 고장 목록에 없는 Y219 PIP 4개(`INT_X33Y219`, `INT_X34Y219`)를 쓴다. 즉 "고장 비트를 안 쓰는 Y219 PIP는 안전하다"는 전제가 틀렸을 가능성이 크다.
- `store_1` 실패는 재현되지 않는 간헐적 현상이었다.
- 제안: E1(행 안 node/PIP 검사), E2(Y219 PIP 0개 multi4).

**17:13 [PC] "진행하자." (E1, E2)**
- **E1 작은 설계(300개):** 고장 목록 밖의 Y219 PIP를 거친 net 300개 중 28개(9%)가 실패했다. 100~750 MHz 모두 같은 net이 실패해서 정적 고장이다. readback은 bitstream과 같다(원래 있던 194비트 제외).
  - 중요: Y219 결함은 config 셀 8개만이 아니다. **셀 값은 정상으로 읽히는데 그 셀이 제어하는 routing mux가 동작하지 않는 경우**가 있어서 stuckchk와 readback으로 잡히지 않는다.
  - 규칙: **Y219 행 안의 PIP는 하나도 쓰지 않고, 행을 지나가는 wire만 쓴다.**
- **E2:** `blk_f`에서 `Y219RWRoute`를 `-Dpips` 없이 돌려(crit 0.95, CPU 8개) Y219 PIP를 쓰던 net 168개를 다시 배선했다(`rs_z`). Y219 PIP 0, 미배선 0, 고장 셀 0. 333 MHz에서 WNS −0.488 ns라 MMCM을 약 280 MHz로 낮춘 `rs_zm`으로 시험했다. phys_opt는 Y219 안으로 다시 배선할 수 있어서 쓰지 않았다.

**17:36 [PC] 중요: 4-EVTA multi4 보드 첫 통과 (`rs_zm`, 280 MHz, 버퍼 16/15/18/17 그대로)**

`rs_zm`의 보드 하드웨어 시험이다(레지스터 패턴은 EVTA0/1을 3회 로드). 보드는 한 번도 멈추지 않았다.

| 테스트 | EVTA0 | EVTA1 | EVTA2 | EVTA3 |
|---|---|---|---|---|
| 레지스터 패턴 | 정상 | 정상 | 정상 | 정상 |
| gemmprobe (4종) | 4/4 | 4/4 | 4/4 | 4/4 |
| sramscan wgt/inp (+반전) | 0 bytes wrong | 0 | 0 | 0 |
| donerace | 16/16 | 16/16 | 16/16 | 16/16 |

- `multi_gemm_gemm_4evta_m`(EVTA 4개 동시)의 모든 결과가 Golden Matched였다.
- 버퍼를 줄일 필요가 없어졌다.

**17:47 [PC] E1 전체 결과 (행 안의 PIP를 쓰는 net 6,850개)**
- 설계 3개(a 5,258 / b 1,193 / c 399)로 행 안에서 출발하는 node 10,041개 중 6,850개를 덮었다.
- 382개(5.6%)가 실패했다. 100 MHz와 500 MHz 결과가 같아서 정적 고장이다.
- 고장은 열(X)마다 다르다. 같은 config 비트(예: `22_03`)도 열에 따라 되고 안 된다. (열, 비트) 조합 631개로 382건을 설명하지만 대부분 한두 번만 관찰된 것이라 규칙이라기보다 관찰 목록이다.
- 실패 offset은 0~3, 14~15, 20~27, 31로, 고장 offset 주변에 몰려 있다. 32 이상은 실패가 없다.
- 결론: 안전 PIP 목록은 확신하기 어렵다. "Y219 안의 PIP는 전부 금지"가 실용 규칙이다. 분석 스크립트와 `bad_col_bits.txt`는 `build/wiretest/`에 있다.

**18:37 [PC] "커밋하고 A안은 중단하자."**
- 커밋 `ce24454c`(push 안 함): `y219route/`(RapidWright 도구, wire 검사, `flow/` tcl, `README.md`), `vivado.tcl`/`pkg_config.py`(multi4 Y219 fence, 128비트 뱅크), hwtest `regpat.py`/`plclkset.py`/`UOP=`, 문서(`vta_synth_notes.md`, `BOARD_TODO.md`, `CLAUDE.md`).
- A안의 vtalib `NESTC_EVTA_Y219_SMALL`과 Makefile `VTA_CFG_JSON`을 되돌렸다. git 밖의 `config/vta_config_y219.json`과 A안 빌드 dir는 남아 있다.

**18:40 [PC] "진행해" (333 MHz 시도)**
- `rs_z`의 실패 경로 대부분은 행 위 `smc_2/3`(Y221~225)에서 PS `S_AXI_HP2/3`(Y99~118)로 가는 WDATA 등이었다(최대 −0.488 ns). 나머지는 EVTA3 내부 FIFO, BRAM→DSP, 행을 넘는 reset 경로(−0.01~−0.09 ns)다.
- `vivado.tcl`: EVTA마다 `smc_i`와 HP 포트 사이에 AXI register slice(`hpslice<i>`)를 넣고 행 아래 pblock `pb_hpslice`에 고정했다. 빌드는 `JOBS=2`.
- Y219 flow를 `y219route/flow/run_flow.sh <tag>`로 묶었다(재배치 → RapidWright 재배선 → Vivado 마무리, Y219 PIP 0과 타이밍 확인).

**18:51 [PC] "VTABundlle.cpp 안에 하드코딩된 버퍼 사이즈 숫자들은 바꿔야지 않아?" / 18:54 "일단은 문서만 보강하자. … vta bit 파일 만들 때도 같이 바꿔야 해?"**
- `VTABundle.cpp`(원본과 사본 4개 동일)의 conv 커널 4곳에 `ACC_MEM_ENTRY = 2048`, `INP_MEM_ENTRY = 2048`, `WGT_MEM_ENTRY = 1024`가 하드코딩돼 있다.

  | 구성 (uop/inp/wgt/acc) | INP | WGT | ACC | 하드코딩 값과 비교 |
  |---|---|---|---|---|
  | multi2, **multi4** (16/15/18/17) | 2048 | 1024 | 2048 | **정확히 같음** |
  | single (16/16/19/18) | 4096 | 2048 | 4096 | 실제 버퍼의 절반만 사용. 동작은 정확하고, 타일이 작아 성능만 손해 |
  | 중단한 A안 (15/15/17/16) | 2048 | **512** | **1024** | WGT, ACC가 실제보다 커서 버퍼 넘침. **바꿔야 했음** |

- 중요: A안이었다면 버퍼가 넘쳤다. A안 준비에서 이 파일을 빠뜨렸다.
- 매크로화는 런타임만의 변경이고 비트스트림은 다시 만들 필요가 없다. 버퍼 크기 자체를 바꿀 때는 `vta_config.json`(비트스트림)과 vtalib CMake·hwtest·`VTABundle.cpp`(런타임)를 모두 바꿔야 한다.
- `docs/vta_synth_notes.md` §5.4를 보강했다(비트스트림/런타임 표, 런타임이 버퍼 크기를 확인하지 않는다는 점, 매크로화 계획). 매크로화는 multi4 작업 뒤에 한다.

**23:03 [PC] multi4 309.6 MHz 보드 통과 (`rs_hpm`)**
- 첫 `make bin`은 라우팅 혼잡으로 bitstream을 쓰지 못했다(이 flow는 그 결과를 쓰지 않음). fence xdc의 `-regexp {vta_i/hpslice\d}`가 셀을 찾지 못해(원인 미확인) glob `vta_i/hpslice*`로 고치고, `place_d.tcl`에서 hpslice를 `pb_hpslice`에 다시 넣었다.
- 333 MHz WNS가 −0.488 → −0.199 ns로 줄었다. 남은 위반은 리셋 → compute_3 경로(지연의 97%가 배선)다. MMCM을 309.6 MHz(MULT 44.5)로 낮춰 WNS +0.019, WHS +0.008.
- Y219 PIP net 0개. stuckchk는 multi2와 같은 194개, extra 0.

  `rs_hpm`의 보드 시험이다. 같은 묶음을 두 번 돌렸고 보드는 멈추지 않았다.

  | 테스트 | 결과 |
  |---|---|
  | regpat (3회 로드) | 모든 비트 정상 |
  | gemmprobe, EVTA 0-3 | 4/4 |
  | sramscan | 0 bytes wrong |
  | donerace | 16/16 |
  | `multi_gemm_gemm_4evta_m` (3회) | Golden Matched |

- 파일: 보드 `bit/evta_multi4_y219_261025/rs_hpm.bin`(280 MHz `rs_zm.bin`도 있음), PC `vta-hw-zcu102-multi4/build/y219/rs_hpm.{bit,dcp}`.

**10-26 00:20 [PC] "번들 검증을 먼저 하자. 309.6MHz bit 파일이지? … 그 다음에 333MHz를 더 시도해보자."**
- `rs_hpm`(309.6 MHz)을 `/lib/firmware/vtahw_multi4_y219.bin`으로 설치해 번들을 검증했다.

  | 번들 | 결과 |
  |---|---|
  | MultiEVTAConvTest (EVTA 4개 동시, 각 10000회) | EVTA 0-3 출력 4개 모두 golden과 일치 |
  | multivtaMxnetResnet18Bundle (4개 사본이 4 스레드로 100회를 나눠 실행) | 100회 모두 정답 281 |
  | singlevtaMxnetResnet18Bundle (단일 EVTA, 100회) | 100회 모두 정답 281 |

  Resnet18 100회 실행 시간을 `OMP_NUM_THREADS`별로 비교했다. 4-EVTA는 독립된 사본 4개를 4 스레드로 돌린 것이다.

  | 100회 실행 시간 | OMP=1 | OMP=2 | OMP=4 |
  |---|---|---|---|
  | 4-EVTA (4 스레드) | **11.0 s** | 12.1 s | 14.1 s |
  | 단일 EVTA | 43.3 s | 23.5 s | 13.5 s |

- EVTA 밖의 CPU 작업이 대부분이고, 스레드마다 OpenMP를 쓰면 코어 4개를 다툰다.
- vtabundles_xcmpl에 `multievtax4` 세트를 추가했다(`vtalib-zcu102-multi4`, `VTA_MAX_XFER` 4 MB). Resnet18 2·3번 사본은 보드의 `model-compiler`로 생성했다. 보드의 `vtalib-zcu102-multi4`가 10-23 done race 수정이 빠진 옛 버전이라 동기화했다.
- 333 MHz용으로 EVTA마다 리셋 블록(`proc_sys_reset_<i>`)을 두고 그 EVTA 영역에 배치하는 빌드를 시작했다.

**10-26 00:49 [PC] "하나의 번들을 4개 evta에 넣는 방식은 뭐야? … 필터 그룹을 나눠서 넣든지 채널 그룹을 나눠서 넣든지"**
- 지금은 번들 하나가 EVTA 하나에 고정된다(`-index-multi-evta` 마스크, 핸들 하나). 레이어 안의 병렬화는 구현돼 있지 않다.

  | 나누는 축 | 각 EVTA가 하는 일 | 합치기 | 평가 |
  |---|---|---|---|
  | **출력 채널(필터 그룹, KN)** | 입력 전체 × 필터 KN/4개 | **필요 없음.** 출력 채널 블록이 서로 겹치지 않아 각 EVTA가 출력 버퍼의 자기 영역에 바로 씀 | 가장 적합. 입력은 4개가 같이 읽지만 HP 포트가 EVTA마다 따로 있음 |
  | 입력 채널(C) | 입력 C/4 × 필터 전체의 부분합 | int32 부분합을 더한 뒤 shift·relu를 해야 함 | 불리함. int32 출력 경로와 덧셈 단계가 새로 필요하고, CPU가 더해야 함 |
  | 공간(출력 행 H) | 출력 행 1/4 + 경계 행(halo) 입력 | 필요 없음 | 채널이 적고 이미지가 큰 앞쪽 레이어에 적합 |

- 출력 채널 분할 구현: `convolution_wo_tr`이 핸들 배열을 받고, 시작과 대기를 나누는 API가 필요하다.
- Resnet18에서는 효과가 작다. 보드의 기존 단계별 측정(single 비트스트림, 1회 135.8 ms):

  | 항목 | 시간 | 비율 |
  |---|---|---|
  | EVTA conv (`convolution_wo_tr`) | 26.1 ms | 19% |
  | conv0, CPU float (`convolutionFloat`) | 76.4 ms | 56% |
  | 나머지 CPU (transpose, elemadd, maxpool 등) | 약 33 ms | 25% |

- conv를 4개로 나눠도 약 136 → 118 ms(−14%)다. 순서 제안: 1) conv0 입력 채널을 16으로 채워 EVTA로, 2) 출력 채널 분할.

**10-26 00:54 [PC] "Conv4Test 4개를 evta 4개에서 동시에 돌릴 수 있겠네. … 600GPS가 나오면 좋겠는데."**
- Conv4Test(3×3, 56×56×64 → 64, 231.2 Mops)를 EVTA 0-3용 사본으로 만들어(`vtabundles_xcmpl/multi4conv4/`) 동시에 돌렸다. 비트스트림은 `rs_hpm`(309.6 MHz), 모든 출력이 golden과 일치했다.

  | 측정 | 1개 | 2개 | 4개 동시 |
  |---|---|---|---|
  | EVTA 하드웨어 시간 (`VTA_SYNC`) | 1.909 ms/회 = 121.1 GOPS | 같음 | 같음 → 합계 **484 GOPS** |
  | conv만 반복 (명령 생성 포함, OMP=1) | 114.6 GOPS | 229.0 GOPS | **457.3 GOPS** |
  | 번들 전체 (CPU transpose, 버퍼 할당 포함) | 64 GOPS | 91 GOPS | 108-117 GOPS |

- EVTA 4개 동시에도 EVTA당 시간이 단독과 같다(HP/DDR 경합 없음). 번들 전체는 CPU transpose와 버퍼 할당 때문에 느리다.
- 순수 GEMM 시간(이론값, GEMM 451,584회 × 1 cycle)은 1.459 ms. 차이 0.45 ms(24%)는 GEMM 뒤에 숨지 못한 load/store다. 지금 비트스트림에는 카운터가 없어 계산값이다. 효율은 4개 합계 피크 634 GOPS의 76%다.

**10-26 01:06 [PC] "지금 돌리고 있는 multi4가 로직 utility는 얼마정도 돼? BRAM은 여유가 있어? 1, 2개 evta를 더 넣을 수 있는지?"**

`rs_hpm`의 자원 사용량이다. BRAM 가용 886은 Y219 행을 금지한 뒤의 수치다(ZU9EG 전체 912).

| 자원 | 사용 | 가용 | 비율 |
|---|---|---|---|
| CLB LUT | 102,018 | 271,752 | 37.5% |
| CLB Register | 136,879 | 543,504 | 25.2% |
| CLB (slice) | 20,784 | 33,969 | 61.2% |
| **BRAM tile** | **580** | **886** | **65.5%** |
| DSP | 1,072 | 2,448 | 43.8% |

EVTA 1개당 LUT 약 25k, FF 약 33.5k, BRAM 145 tile, DSP 268이다. 이걸로 추정한 5·6개 구성이다.

| 구성 | BRAM | DSP | LUT |
|---|---|---|---|
| 5개 | 725 / 886 (82%) | 1,340 (55%) | 약 127k (47%) |
| 6개 | 870 / 886 (**98%**) | 1,608 (66%) | 약 152k (56%) |

- 5번째는 Y219 위쪽(남은 약 227 tile)에 들어간다. 6번째는 버퍼가 결함 행을 넘어 나뉘어야 해서 어렵다.
- 그 밖에 필요한 것: HP0-3을 다 써서 HPC0/1 또는 포트 공유, EVTA5 주소가 APM(`0xA0050000`)과 겹침, 런타임·컴파일러의 4개 제한(`vtaCmdH`–`vtaCmdH3`, `-index-multi-evta`), CMA, fence 분할.
- 5개는 해 볼 만하고 6개는 버퍼 축소 없이 어렵다. 333 MHz 결과를 보고 정하기를 권했다.

**10-26 03:52 [PC] 중요: multi4 333 MHz 보드 통과 (`rs_rst`), Conv4 4-EVTA 489.6 GOPS**
- EVTA별 리셋으로 공용 리셋 → compute_3 위반이 사라졌다. `make bin`은 route 혼잡 8 net으로 bitstream을 못 만들었지만(예상), Y219 flow는 `vta_wrapper_opt.dcp`부터 다시 하므로 문제없다.
- Y219 PIP 0, WNS +0.008 ns, WHS +0.010 ns. stuckchk는 multi2와 같은 194비트, extra 0.
- 보드: EVTA 0-3 gemmprobe, sramscan, donerace 16/16 통과. `multi_gemm_gemm_4evta_m` 3회 Golden Matched. regpat은 출력이 섞여 잘려서 **이번에 확인하지 못했다**.

multi4가 333 MHz에 이르기까지의 단계다. Y219 결함 행 때문에 Vivado 기본 구현을 쓸 수 없어서 재배선 flow로 만들고, 남은 위반을 단계별로 고쳤다.

| 판 | 한 일 | 333 MHz WNS | 쓴 클럭 | 보드 |
|---|---|---|---|---|
| `rs_h` | 고장 비트를 쓰는 PIP만 피함 (606종) | −0.092 | 333 | EVTA0 멈춤, EVTA2 접근 시 보드 멈춤 |
| `rs_m` | `rs_h`에서 MMCM만 낮춤 | — | 322 | EVTA0 멈춤, EVTA3 접근 시 보드 멈춤, 레지스터 비트 틀림 |
| `rs_z` / `rs_zm` | Y219 행 PIP 전부 금지 (E2) | −0.488 (HP2/3 경로) | 280 | 모든 시험 통과 |
| `rs_hpm` | HP 포트 앞 register slice `hpslice<i>`를 행 아래에 | −0.199 (리셋 → compute_3) | 309.6 (WNS +0.019) | 모든 시험, 번들 통과 |
| `rs_rst` | EVTA마다 리셋 블록 | **+0.008** | 333 | 통과 (regpat 미확인) |

Conv4 4-EVTA를 309.6 MHz(`rs_hpm`)와 333 MHz(`rs_rst`)에서 같은 방법으로 비교했다. 모든 출력이 golden과 같다.

| | 309.6 MHz | 333 MHz |
|---|---|---|
| conv 1회당 EVTA 실행 시간 | 1.909 ms | 1.864 ms |
| EVTA 1개 (HW 시간 기준) | 121.1 GOPS | 124.0 GOPS |
| conv only, EVTA 1 / 2 / 4개 (OMP=1) | 114.6 / 229.0 / 457.3 | 122.6 / 245.1 / **489.6** |

- 클럭은 7.7% 올랐지만 EVTA 시간은 2.4%만 줄었다. 순수 GEMM 시간 1.355 ms는 실행 시간의 73%이고, 나머지는 GEMM 뒤에 숨지 못한 load/store다. 600 GOPS를 넘으려면 tile을 키우거나 double buffering 등으로 load/store와 GEMM을 겹쳐야 하고, 클럭은 이제 거의 효과가 없다.
- 보드 `/lib/firmware/vtahw_multi4_y219_333.bin`이 333 MHz판이다(원본 `bit/evta_multi4_y219_261026/rs_rst.bin`). 번들 기본 `vtahw_multi4_y219.bin`은 아직 309.6 MHz판이다.

## 결정이 남은 것 (10-26 07:00 기준)
- 번들이 기본으로 읽는 `vtahw_multi4_y219.bin`을 333 MHz판(`rs_rst`)으로 바꿀지.
- 커밋: `ce24454c` 이후 변경(hpslice, EVTA별 리셋, `run_flow.sh`, multi4conv4, 문서)은 커밋하지 않았다. 묶어서 커밋할지.
- `rs_rst`의 regpat 재확인(이번에 출력이 잘려 미확인).
- 레이어 안 4-EVTA 병렬화(출력 채널 분할)와 conv0의 EVTA 이전 중 무엇을 먼저 할지.
- 5-EVTA 구성으로 갈지(333 MHz 결과를 보고 정하기로 함).
- git 밖에 남은 `config/vta_config_y219.json`과 A안 빌드 dir를 지울지.
- Conv4 600 GOPS를 위한 스케줄 변경과 `PROFILE=1` multi4 비트스트림.
- 이미 결정됨: A안(버퍼 축소) 중단, 커밋 `ce24454c`, Y219 행 안의 PIP 전부 금지 규칙, 번들 검증 후 333 MHz 시도(완료), `VTABundle.cpp` 버퍼 상수 매크로화는 multi4 작업 뒤에(문서만 먼저 보강).
- 보드 관련 알림: 보드 SSH에 외부 IP의 root 로그인 시도가 있다. 보드 관리자에게 확인을 권했다.
