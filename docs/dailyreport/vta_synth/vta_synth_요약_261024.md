# 2026-10-24 EVTA synth 요약: Y219 행 전체가 아니라 고장 비트를 켜는 PIP 606종만 문제임을 확인(U-Ray DB와 stuckchk 85비트 일치), multi4 첫 stuckchk 0 extra 비트스트림(`rs_h`, WNS −0.092 ns로 아직 미달), 행 전체 회피 방식(RapidWright 재배선·더미 blockage·창 pblock)은 모두 실패

- 기간: 2026-10-24 07:00 ~ 10-25 07:00 (KST)
- 세션은 하나다. [PC]는 PC(호스트)와 그 위의 docker 컨테이너 `vivado2020-build`(Vivado 2020.1), 호스트에 받은 RapidWright로 한 작업이다. 이날은 보드 시험을 하지 않았다. 새 파일과 변경은 커밋하지 않았다.

## 시간순

**09:04 [PC] "1번이지."** (10-23의 multi4 선택지 중 1번: RapidWright로 Y219 행을 피해 다시 배선)
- 두 작업을 백그라운드로 돌렸다.
  - Vivado step A: 배치가 끝난 multi4 DCP를 하위 pblock의 CONTAIN_ROUTING 없이 다시 배선(`routed_a.dcp`).
  - RapidWright 재배선 시험: 2차 빌드의 routed DCP에서 Y219 행의 PIP를 쓰는 118개 net을 풀고 다시 잇기.
- RapidWright는 Vivado 2020.1과 같은 세대인 v2020.1.7을 `~/tools/rapidwright`에 받았다. 이 버전에는 배선기(RWRoute)가 없어 `scripts/y219route/Y219Route.java`를 직접 작성했다. Y219 행에서 구동되는 노드만 막고, 행을 지나가기만 하는 긴 배선은 허용한다.
- `write_edif`로 EDIF를 쓰면 RapidWright가 multi4 DCP를 읽는다(암호화된 IP 없음). 문제 PIP는 전부 Y219 INT 타일에 있고, net은 smartconnect exit 레지스터, PS `saxigp4` rdata, EVTA 3 제어 레지스터 등이다.

**18:02 [PC] "너무 오래 도는 거 아냐? … 다른 방법이 있을까?"**
- 재배선을 멈췄다. 5시간 동안 1003번 배선했고, 처음 528개였던 Y219 net 일부를 15~19번씩 반복 배선했다. 다른 net의 노드를 빼앗는 비율이 200회당 7 → 11 → 53 → 172 → 173으로 올랐다. "no path"는 없었고 문제는 혼잡이었다.
- 중요: 원인은 Y219 바로 옆 행이다. Y218·Y220 행 입력 핀(IMUX)은 들어오는 길의 절반이 Y219 타일을 지나서, Y219를 막으면 남은 3개 정도의 입구를 여러 net이 나눠 써야 한다.
- 배치 금지를 Y218~Y220 세 행으로 넓혀 opt DCP부터 다시 배치했다(`build/y219/place_d.tcl`). PROHIBIT: SLICE Y218~Y220, RAMB36 Y43·Y44, RAMB18 Y86~89, DSP Y86~89. BRAM은 한 행 정도만 더 잃는다(556/912 사용 중).
- 대안: PathFinder 방식 재배선기, multi4 버퍼 축소(가장 확실하지만 성능이 바뀜).

**18:16 [PC] "현재 세션에 핸드오프 문서를 작성해줘. `vta_synth_notes.md`에 …" / 18:17 "docs/vta_synth_notes.md 에"**
- `docs/vta_synth_notes.md`를 썼다(커밋 안 함).
  - §1–2 빌드·시뮬레이션·합성 명령과 환경(docker Vivado 2020.1, RapidWright와 Java 11, 보드 sudo·비트스트림 로드·`nocpuidle.sh`, 보드 전 확인 3가지 WNS / `stuckchk` 0 extra / Y219 PIP 0).
  - §3 버그·원인·해결: Y219 configuration 셀 결함과 fence, 별개 버그 7개(PLL 리셋, wgt RMW, DONT_TOUCH, done race, sramscan CHUNK, ISA 2비트 폭, coherent 설정), multi4 현재 상태.
  - §4 함정, §5 GEMM HW ↔ 컴파일러 인터페이스(파일 지도, 256비트 명령어·uop 필드, SRAM·DRAM 레이아웃, 파라미터 변경 시 맞출 7곳).
- 중요: 새로 찾은 함정. vtalib conv 커널(`VTABundle.cpp`)에 SRAM 크기가 multi 기준(acc 2048 / inp 2048 / wgt 1024)으로 하드코딩되어 single 비트스트림에서도 버퍼를 절반만 쓴다. multi4 버퍼 축소를 택하면 반드시 고쳐야 한다(§5.4).

**19:06~19:38 [PC] Y218~Y220 비운 재배치(`routed_d`)와 RapidWright 재배선(`rr_d`)**
- `routed_d`: 배선 에러 0, Y219 PIP net 528 → 239(드라이버는 Y218~Y220에 없음), 333 MHz WNS −0.100 ns(실패 endpoint 63, TNS −1.2 ns), WHS +0.010 ns, RAMB36 572/886.
- 239개 net 재배선(`rr_d`)은 다른 net을 뜯은 경우 1번으로 몇 분 만에 끝났다(3시간 타임아웃, 재배선 상한 3000).
- 그러나 `route_c.tcl` 확인 결과 Y219 PIP 0, 배선 에러 0이지만 WNS −2.827 ns(endpoint 13,897), WHS −0.031 ns(2개)여서 보드에 올리지 않았다.
- 원인은 재배선기: net 전체를 뜯고 타이밍 고려 없이 sink를 붙여, 리셋 net `peripheral_aresetn`(PIP 약 3만 개)이 긴 사슬이 됐다. Vivado 배선 트리를 두고 Y219 가지만 떼어 다시 붙이도록 고쳐 `rr_e`를 돌렸다.

**19:46 [PC] "타이밍이 문제면 셀 경계를 못 넘게 제약을 걸었던 걸 풀어줘도 … 진짜 문제는 아니었잖아." / "지금 돌리고 있는 건 끝내고 다음에 적용할 지 생각해봐."**
- DONT_TOUCH 해제 스크립트 `build/y219/place_f.tcl`만 준비했다(opt DCP에서 HLS 블록과 큐의 DONT_TOUCH를 끄고 `opt_design`부터, Y218~Y220 금지 유지).
- 20:02 `rr_e` 결과: Y219 PIP 0, 배선 에러 0, WNS −1.748 ns(endpoint 457), WHS −0.028 ns(2개). 최악 경로는 PS 쪽 xbar(Y194) → compute_3(Y231) `WDATA[23]` net으로 행을 건너는 데 2.9 ns(Vivado라면 0.5 ns 안팎).
- 깨진 1.6 ns는 재배선기 탓이라 DONT_TOUCH 해제는 미뤘다. 끊긴 지점마다 원래 가지로 돌아가는 짧은 우회만 만들도록 고쳐 `rr_g`를 돌렸다.

**10-25 00:38~01:01 [PC] dummy 점유 + Vivado 재배선(`blk_e`)**
- `rr_g`는 더 나빴다(WNS −1.994 ns): 경로를 지연이 아니라 hop 수로 골라 최악 net이 12타일 wire로 고리를 돌았고(0.76 → 3.18 ns), 끊긴 PIP 조각으로 DRC에도 걸렸다. RapidWright 2025.2 RWRoute도 clock 제약을 몰라 WNS −1.578 ns, sink 78~156개 미배선이었다.
- 새 방식: Y219 PIP가 구동하는 노드를 dummy LUT 1,137개의 고정 배선(PIP 54,603개)으로 차지하고 Vivado가 timing-driven으로 배선(`Y219Block.java`, `build/y219/y219block.tcl`). 행 노드 49,285개 중 48,063개를 덮었다.
- 결과: 333 MHz WNS 0.000 ns, WHS +0.006 ns, 미배선 0으로 타이밍은 맞았지만, dummy 제거 후 Y219 PIP를 쓰는 net이 169개 남아 bitstream을 쓰지 않았다. 표본 노드는 모두 거대 dummy net `vta_y219blk_SLICE_X0Y219_A_o` 소속이었다(가설, 미확인).

**10-25 01:04 [PC] "지금 routing 하는 게 결과가 안 좋으면 … y219가 칩에서 어디에 있는지 그림으로 … pblock을 어디에 잡았어? … 아래쪽에 vta 3개가 들어가도 RAMB 갯수는 충분할 것 같은데."**
- 그림 두 장을 `docs/vta_synth_notes.md` §3.3에 넣었다(`docs/figs/zcu102_y219.svg`, `docs/figs/multi4_placement.svg`, 생성 스크립트 `FloorDump.java`, `floorplan_svg.py`).
- 정정: 지금은 CONTAIN_ROUTING을 끄고 PROHIBIT를 쓴다. split 모드에서 CONTAIN_ROUTING을 켜면 배선 실패(conflict 18), 끄면 528개 net이 Y219 PIP를 썼다. PROHIBIT는 배치만 막는다.
- Y219는 다이 높이 거의 가운데를 가로지른다. PS는 왼쪽 아래(Y0~179), HP·레지스터 포트는 Y99~118. `pb_evta_0/_1`은 아래 반쪽 전체, `_2/_3`은 위 반쪽 전체이고 배치는 아래부터 EVTA 0 → 1 → (Y219) → 2 → 3이다.
- 아래에 EVTA 3개는 들어가지 않는다:

  | Y219 아래 | 있는 것 | EVTA 3개 + 공용 |
  |---|---|---|
  | RAMB36 | 379 | 3×143 + 9 = 438 (**59개 부족**) |
  | SLICE | 14,666 | 약 15,100 (100% 초과) |
  | DSP | 1,080 | 804 (충분) |

- 후보: 1) dummy net에 sink를 달아 다시, 2) EVTA 2/3에 사각형 하나짜리 창 pblock + CONTAIN_ROUTING.

**10-25 01:12 [PC] "2 창 방식으로 해보자."**
- `place_w.tcl`로 창 방식 재배치·재배선. 네 pblock 모두 CONTAIN_ROUTING, 공용 로직(xbar, reset)은 pblock 밖, Y218~220 PROHIBIT, EVTA마다 블록 26개.

  | 대상 | 창(Y 범위) | 창 안 RAMB36 | 필요한 RAMB36 |
  |---|---|---|---|
  | EVTA 0·1 (함께) | Y0~217 | 379 | 2 × 143 |
  | EVTA 2 | Y221~319 | 247 | 143 |
  | EVTA 3 | Y320~419 | 260 | 143 |

- 04:31 결과: 창 방식은 실패했다. `routed_d`와 비교:

  | 항목 | 이전 (`routed_d`) | 창 방식 (`routed_w`) |
  |---|---|---|
  | WNS | −0.100 ns | **−2.294 ns** |
  | 위반 endpoint | 63 | **46,373** |
  | WHS | +0.010 | −0.147 |
  | 배선 충돌 | 0 | **2** (INT_X43Y217, 아래 창의 위쪽 끝) |
  | Y219 PIP을 쓰는 net | 239 | 89 |

- 남은 89개는 행을 건너야만 하는 net이다(51개 EVTA2/3 CONTROL_BUS RDATA·AXI, 38개 공용 xbar). 배치 직후 −0.47, 배선 중 −4.6까지 갔다가 −2.29. 최악 경로는 EVTA0 `inp_mem` BRAM → `compute` DSP로, 아래 창 위쪽 끝(Y217 부근) 혼잡으로 본다.

**10-25 04:31~05:17 [PC] dummy 트리 크기 제한(`blk_b`, `blk_c` → `blk_f`)**
- dummy 트리가 뻗는 범위를 출발 LUT 기준 열 수로 제한(`-Dxw`)해 다시 만들었다.

  | | 제한 없음 (`blk_e`) | ±3열 (`blk_b`) | ±6열 (`blk_c`) |
  |---|---|---|---|
  | 더미 LUT 수 | 1,137 | 1,365 | 1,252 |
  | 가장 큰 트리 | 행 대부분을 덮는 트리 1개 | 3,131 PIP | 5,935 PIP |
  | 덮인 노드 (전체 49,285) | 48,063 | 47,471 | 47,895 |
  | 덮이지 않은 INT 노드 | 1 | 715 | 169 |

  ±3열은 SLICE 없는 열(BRAM/DSP) 근처에 구멍이 4열 간격으로 생겨 쓰지 않았다.
- `blk_c`로 배선한 `blk_f`: WNS −0.092 ns, WHS +0.006 ns, 배선 오류 0, 그러나 dummy 제거 후 Y219 PIP net 168개. 새어 나간 PIP 460개 중 458개가 dummy가 잡고 있던 노드였고 Vivado 겹침 노드 수는 0.
- 중요: Vivado는 sink 없는(antenna) dummy 배선을 점유된 것으로 보지 않는다. "거대 트리 때문"이라는 가설은 틀렸고, dummy blockage는 크기와 상관없이 효과가 없다.

**10-25 05:29 [PC] 중요: 고장 비트를 켜는 PIP만 문제**
- 고장 셀은 Y219 타일 비트의 일부(48비트 중 8비트, 슬롯 offset 2, 16–19, 28–30)뿐이다. Project U-Ray `prjuray-db`의 `segbits_int.db`로 INT PIP별 설정 비트를 계산했다.
- `blk_f`의 Y219 PIP 460개 중 고장 셀을 켜는 것은 85개(46개 net)이고, `stuckchk`가 `blk_f.bit`에서 찾은 extra 85비트와 개수·8종 비트별 분포가 모두 일치했다.
- 금지할 PIP은 INT PIP 3,778종 중 606종(`stuckpips.py` → `stuck_int_pips.txt`). `Y219RWRoute.java`에 `-Dpips=` 옵션을 넣어 목록 PIP만 금지한다. 재배선 대상이 239개 net에서 46개로 줄었다.
- 두 bitstream의 `stuckchk` 결과:

  | bitstream | 설명 | WNS | extra stuck 비트 |
  |---|---|---|---|
  | `routed_d` | Y219 대책 없음 | −0.100 | 135 |
  | `blk_f` | 더미 blockage 후 | −0.092 | 85 |

- 필요 없어진 누수 집계 `leak.tcl`(노드당 약 38초)은 중단했다.

**10-25 05:44~06:16 [PC] multi4 첫 stuckchk 0 extra 비트스트림, 타이밍 −0.092 ns 남음**
- `blk_f`에서 46개 net의 문제 가지만 RapidWright로 다시 배선하고 Vivado로 확인했다. `rs_f`는 delay를 무시해 −0.909, `rs_g`는 `-Dcrit`로 연결마다 criticality 0.95를 강제했다(남은 미배선 핀 1개는 Vivado가 배선). 이어 post-route `phys_opt_design` 3회(`rs_h`).

  | 단계 | WNS | 고장 셀을 쓰는 net | stuckchk extra |
  |---|---|---|---|
  | `blk_f` (Vivado 배선) | −0.092 | 46 | 85 |
  | `rs_f` (RapidWright로 46개 재배선) | −0.909 | 0 | 배선 미완료로 bitstream 없음 |
  | `rs_g` (재배선 + criticality 0.95 강제) | −0.206 | 0 | **0** |
  | `rs_h` (post-route `phys_opt` 3회) | **−0.092** | 0 | (bitstream 작성됨) |

- `rs_g`의 최악 경로는 smc → PS HP 포트(SAXIGP5) 행 횡단 net이었다. `rs_h`의 남은 위반(TNS −0.673 ns, endpoint 51)은 EVTA3 안 `load_queue_3` FIFO36 두 개 사이(EMPTY → LUT 2단 → 8개 FIFO의 RDEN, RAMB36 X6Y72 → X5Y70)로, Y219·재배선과 무관하게 `routed_d`부터 있던 위반이다.
- 배치를 건드리는 phys_opt(`rs_i`): 1차는 `-bram_enable_opt`가 post-route 미지원이라 오류로 끝났고, 그 옵션을 빼고(`-placement_opt -critical_cell_opt -rewire -critical_pin_opt -clock_opt`) 다시 돌렸지만 바뀌지 않았다.
- 06:16 1번 방법 시작: 배치 directive `ExtraTimingOpt`로 다시 배치·자유 배선 후 고장 PIP net만 RapidWright로 고침(`place_x.tcl`, 1시간 이상, 0 이상 보장 없음). 결과는 아직 없다.
- 2번 방법(결정 필요): 배선이 끝난 `rs_h`에서 MMCM 분주값만 바꿔 333 → 약 325 MHz. 10분 정도, −0.092 ns는 거의 확실히 해소되지만 multi4만 성능 약 2.5% 낮고 클럭 사양이 single/multi2와 달라진다.

## multi4 Y219 회피 시도 정리

행 전체를 피하는 방법이 차례로 막혀서, 마지막에 고장 비트를 켜는 PIP만 피하는 방법으로 바꿨다(모두 333 MHz).

| 시도 | 방법 | WNS / WHS (ns) | Y219(고장) net | 실패·전환 이유 |
|---|---|---|---|---|
| 2차 빌드 재배선 | 직접 만든 재배선기, Y219 행 전체 금지 | — | 528 → 수렴 안 함 | Y218/Y220 IMUX 입구 혼잡, 5시간 후 중단 |
| `routed_d` | Y218~Y220 PROHIBIT 재배치 | −0.100 / +0.010 | 239 | 기준점 |
| `rr_d` | net 전체 재배선 | −2.827 / −0.031 | 0 | 리셋 net이 긴 사슬 |
| `rr_e` | Y219 가지만 재연결 | −1.748 / −0.028 | 0 | 행 횡단 2.9 ns |
| `rr_g` | 원래 가지로 짧은 우회 | −1.994 | — | hop 수 기준 경로, DRC |
| RWRoute 2025.2 | — | −1.578 | sink 78~156 미배선 | clock 제약 모름 |
| `blk_e` | dummy 점유 + Vivado 배선 | 0.000 / +0.006 | 169 | antenna dummy를 Vivado가 무시 |
| `routed_w` | 창 pblock + CONTAIN_ROUTING | −2.294 / −0.147 | 89 | 창 끝 혼잡, 충돌 2 |
| `blk_f` | dummy ±6열 | −0.092 / +0.006 | 168 (고장 46) | 같은 이유, extra 85 |
| `rs_g` → `rs_h` | 고장 PIP 606종만 금지 + criticality + phys_opt | −0.206 → −0.092 | 0 | extra 0, 원래 FIFO 위반만 남음 |

## 결정이 남은 것 (10-25 07:00 기준)
- 사용자 답 대기: `place_x`(1번)가 실패하면 2번(약 325 MHz로 낮춤)으로 갈지, 1번이 끝나기 전에 2번도 함께 만들지.
- `place_x` 결과는 아직 확인하지 않았다. WNS ≥ 0이 나오면 `stuckchk` → 보드에서 EVTA 4개 각각 sramscan, gemmprobe, donerace, 그리고 `multi_gemm_gemm_4evta_m`.
- DONT_TOUCH 해제(`place_f.tcl`)는 보류. Y219 문제가 풀린 뒤에도 WNS가 음수로 남을 때만 쓴다. 해제한 bitstream은 보드에서 gemmprobe와 donerace를 반복해 안정성을 확인해야 한다.
- vtalib `VTABundle.cpp`의 SRAM 크기 하드코딩(multi 기준)을 고칠지.
- 커밋 안 된 것: `docs/vta_synth_notes.md`, 그림 2장, `Y219Route.java`, `Y219Block.java`, `Y219RWRoute.java`, `y219block.tcl`, `FloorDump.java`, `floorplan_svg.py`, `stuckpips.py` 등. `vta_synth_notes.md` §3.3 표는 이후 결과로 갱신하기로 했다.
- 이미 결정된 것:
  - multi4는 RapidWright 재배선(1번)으로 간다(09:04).
  - 창 pblock 방식은 시도(01:12) 후 실패로 접었다.
  - Y219 회피 기준은 "행 전체 PIP 0"에서 "고장 비트를 켜는 PIP 606종 0 + `stuckchk` 0 extra"로 바뀌었다.
