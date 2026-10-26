# vta_synth_notes — EVTA 비트스트림 합성/보드 디버깅 핸드오프

최종 갱신: 2026-10-25. 이 문서는 다음 세션이 바로 이어서 작업할 수 있도록, 명령·경로·함정·코드 위치를 모은 것이다.

- 실험 경위와 상세 근거는 `log/BOARD_TODO.md`(시간순 기록)에 있다.
- 디버깅 보고서는 https://claude.ai/artifact/4KvtghuqpzV1GKunEyTRdf (Claude Docs)에 있다.
- 이 문서는 "지금 무엇이 되고, 어떻게 돌리며, 어디가 위험한가"만 다룬다.

## 0. 현재 상태 한눈에

| 구성 | 소스 트리 | 상태 | 보드 파일 |
|---|---|---|---|
| single (EVTA 1개, 16/16/19/18) | `vta-hw-zcu102-single` + `vtalib-zcu102-single` | **동작**: 30개 번들 golden PASS. 커밋 `6fa74ee1` | `bit/evta_single_y219_333MHz_pass_261023/` |
| multi2 (EVTA 2개, 16/15/18/17) | `vta-hw-zcu102-multi2` + `vtalib-zcu102-multi2` | **동작**: 전 테스트와 Resnet18 x2 PASS. 커밋 `50201ac5` | `bit/evta_multi2_y219_333MHz_pass_261024/`, `/lib/firmware/vtahw_multi2_y219.bin` |
| multi4 (EVTA 4개, 16/15/18/17) | `vta-hw-zcu102-multi4` + `vtalib-zcu102-multi4` | **333 MHz로 동작** (2026-10-26): Y219 안의 PIP를 하나도 쓰지 않게 재배선, HP 포트 앞 register slice, EVTA별 proc_sys_reset(§3.3). hwtest 전부 PASS, Conv4 4-EVTA 489.6 GOPS. 커밋 `dfec80f7` | `bit/evta_multi4_y219_333MHz_pass_261026/vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta.bin` (309.6 MHz판 `bit/evta_multi4_y219_261025/rs_hpm.bin`) |

남은 일의 순서:
1. multi4 333 MHz: EVTA마다 리셋 블록(`proc_sys_reset_<i>`)
2. `_prof` 빌드(single/multi2/multi4)를 새 fence로 다시 생성
3. 보드 소유자에게 PL 결함 통보

## 1. 환경

### 1.1 호스트 PC (x86, Ubuntu 20.04) — 합성

- **Vivado/HLS 2020.1은 docker 컨테이너 `vivado2020-build` 안에만 있다**(ubuntu 18.04, user `ubuntu`).
  - `/tools/Xilinx`와 nest-compiler 트리가 같은 경로로 마운트되어 있다.
  - 설치 경로는 `/tools/Xilinx/v2020.1/`이며 Y2K22 HLS 패치가 적용되어 있다.
  - 호스트의 `/tools/Xilinx/Vivado/2018.3`과 `2020.2`는 다른 버전이다. 2020.2에는 `vivado_hls`가 없다.
  - `pynq24-build`/`pynq27-build` 컨테이너에는 2020.1이 없다.
  - `vivado.tcl`은 정확히 2020.1을 요구한다.
- 대화형 사용:
  ```bash
  docker exec -it -u ubuntu vivado2020-build bash
  source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh
  ```
- 배치(batch)로 tcl 하나를 돌릴 때:
  ```bash
  D=<작업 dir>
  docker exec -u ubuntu vivado2020-build bash -c \
    "source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh >/dev/null; cd $D && \
     vivado -mode batch -nojournal -log X.log -source X.tcl > X.out 2>&1"
  ```
- **RapidWright**(multi4 재배선용):
  - jar: `~/tools/rapidwright/rapidwright-2020.1.7-standalone-lin64.jar`
    - Vivado 2020.1과 짝이 맞는 버전이다. RWRoute는 없다.
    - 2020.2.7 jar도 받아 두었지만 쓰지 않는다.
  - 디바이스 데이터: `RAPIDWRIGHT_PATH=$HOME/tools/rapidwright/d2020.1`(xczu9eg 포함, 약 2.1 GB).
  - 호스트에는 Java 11 **JRE만** 있고 javac가 없다. 소스 파일 모드로 실행한다: `java -cp <jar> File.java`.
    - 클래스 이름이 jar 안의 난독화 클래스와 겹치면 안 된다(예: `M`).
  - `Design.readCheckpoint(x.dcp)`는 옆에 **암호화되지 않은 `x.edf`**가 있어야 한다. Vivado에서 `write_edif`로 만든다.
  - RapidWright가 쓴 DCP는 Vivado 2020.1에서 그대로 열린다.

### 1.2 보드 (ZCU102, PYNQ 2.4 / Ubuntu 18.04, aarch64)

- 접속은 `ssh zcu102`(= `xilinx@192.168.20.2`)로 한다.
  - 트리: `/home/xilinx/JO/npuprg/nestc2_clion/nest-compiler`
  - 비트스트림 보관: `/home/xilinx/JO/npuprg/bit/<목적>_<YYMMDD>/`
- **공유 보드다.** 작업 전에 `who`/`uptime`을 확인한다.
- **sudo는 `/etc/sudoers.d/evta-test`에 등록된 명령만 비밀번호 없이 된다.**
  - 등록된 명령: `/usr/bin/python3`, `/bin/cp * /lib/firmware/*`, `/bin/sh`, `hwtest/*`, `hw/*`
  - 스크립트는 `sudo -n /bin/sh -c "bash /tmp/x.sh"`처럼 통째로 sudo로 돌리고, 안에서는 sudo를 쓰지 않는다.
  - `sudo ./prog`는 실패한다("a password is required").
- **비트스트림 로드 순서(매 실행 전)**:
  ```bash
  cp <x>.bin /lib/firmware/
  echo 0 > /sys/class/fpga_manager/fpga0/flags
  echo <x>.bin > /sys/class/fpga_manager/fpga0/firmware
  python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()"
  python3 vtabundles_xcmpl/hwtest/plreset.py     # pl_resetn0 펄스 (EMIO GPIO 95)
  ```
  - 멈춘 실행은 auto-restart 블록을 deadlock 상태로 남긴다. 그래서 매번 다시 로드한다.
  - Ctrl-C로 중단한 뒤에는 `xlnk_reset`이 필요하다. CMA 누수가 생기면 "fpga_buff_ != nullptr" 에러가 난다.
- **JTAG/ILA를 쓸 때**: 부팅 후 `sudo vtabundles_xcmpl/hwtest/nocpuidle.sh`를 먼저 실행한다.
  - 하지 않으면 hw_server 연결 몇 분 뒤 Linux가 조용히 죽는다(cpuidle 문제).
  - JTAG 케이블은 호스트 PC의 hw_server에 붙어 있다.

## 2. 빌드 / 시뮬레이션 / 합성 명령

### 2.1 비트스트림 (호스트, docker 안)

```bash
cd <nest-compiler>/vta-hw-zcu102-{single,multi2,multi4}/hardware/xilinx
make ip            # HLS 4블록 (fetch/load/compute/store) → build/hardware/xilinx/hls/<CONF>[_prof]/.done
make bin           # Vivado 블록디자인+구현 → .bit → bit2bin.py → .bin   (전체 약 30분~1시간+)
```

Makefile 옵션(모두 `#### JO`):

| 변수 | 의미 |
|---|---|
| `PROFILE=1` | compute 카운터(0x50/0x60/0x70)와 AXI perf monitor를 넣는다. 빌드 dir은 `<CONF>_prof`, 출력은 `vtahw_<CONF>_prof.*` |
| `CSIM=1` | HLS C 시뮬레이션을 먼저 돌린다 |
| `JOBS=8` | Vivado 구현 스레드 수 |
| `EXPORT_NAME` | 출력 이름. 기본값 `vtahw_<BUILD_NAME>` |
| 환경변수 `VTA_BRAM_FENCE` | zcu102 전용. `y219`(기본, EVTA 2개 이하) / `split`(EVTA 3개 이상 기본값, 현재 실패) / `upper` / `none` |

- 출력: `build/hardware/xilinx/vivado/<CONF>[_prof]/export/vtahw_<CONF>[_prof].{bit,bin,hwh,xsa}`와 timing/utilization 리포트.
- `<CONF>` 문자열은 `python3 config/vta_config.py --cfg-str`로 확인한다.
  - single은 `zcu102_1x16_i8w8a32_16_16_19_18`이다. 숫자 순서는 uop/inp/wgt/acc log2 바이트다.
  - multi는 뒤에 `_<N>evta`가 붙는다.
- 다른 설정으로 빌드할 때는 트리를 복사해 별도 build dir을 쓴 예가 있다(`build/clk200`, `build/wgt16_clk200` 등).

### 2.2 비트스트림 검증 (호스트) — 보드에 올리기 전 필수

```bash
# 1) 타이밍: *.timing.rpt의 WNS >= 0
# 2) 결함 행 사용 여부: 0 extra여야 함
python3 vtabundles_xcmpl/hwtest/brambist/stuckchk.py <design.bit> \
  -b vta-hw-zcu102-single/build/wgt16_clk200/export/vtahw_zcu102_1x16_i8w8a32_16_16_19_18_wgt16_clk200_lowbram.bit
# 3) (Vivado) Y219 INT 타일에 PIP가 있는 net 수 = 0 : y219route/flow/route_s.tcl 이 출력
#    stuckchk 0 extra만으로는 부족하다: 결함 셀을 안 쓰는 Y219 PIP도 5% 정도 동작하지 않는다(§3.1)
```

### 2.3 시뮬레이션

- **HLS C sim**: `make ip CSIM=1`. 테스트벤치는 `hardware/xilinx/sim/vta_test.cc`다.
- **C/RTL cosim, PL 전체 시뮬레이션**: 스크립트가 git-ignore된 scratch dir에만 있다.
  - cosim: `vta-hw-zcu102-single/build/hardware/xilinx/cosim/`(`cosim.tcl`, `cosim_load.tcl`, `compute_tb.cc`)
  - PL 블록디자인 시뮬레이션(PS 대신 AXI VIP): `build/simbd`
  - 결과 요약은 BOARD_TODO 2026-10-20 "host PC" 절에 있다.
- **설계는 시뮬레이션에서 모두 맞았다.** 보드 문제는 전부 실리콘 결함 때문이었다(§3.1). 시뮬레이션이 PASS라고 보드가 맞는 것은 아니다.

### 2.4 런타임 / 테스트 (보드)

- **ISA 프로브와 HW 테스트**(`vtabundles_xcmpl/hwtest/`, NEST-C/Glow 없이 vtalib만 링크):
  ```bash
  cd vtabundles_xcmpl/hwtest
  VTA_COHERENT=false SUFFIX=_nc ./build.sh gemmprobe sramscan donerace          # single
  VTA_COHERENT=false SUFFIX=_m BUFF="15 18 17" XFER=8388608 EVTA=$e ./build.sh sramscan   # multi, EVTA e
  ```
  - `sramscan wgt|inp [invert] [dump]`: SRAM 전체에 패턴을 쓰고 GEMM+STORE로 읽어 비교한다. 0 bytes wrong이어야 한다.
  - `gemmprobe_<reset|zero|ident|perm|ones>`: 10개 명령짜리 GEMM. `GP_IDX=n`으로 SRAM 인덱스를 바꾼다.
  - `donerace`: 드라이버 done race 회귀 테스트. 16/16이어야 한다.
  - `gemm_nc`, `add_test_nc`, `rawtest*`: vtalib 기본 테스트와 raw ISA 프로브.
  - `multi_gemm_gemm` (+`NUM_EVTA=2`): multi-EVTA 동시 실행.
  - 보조 도구:
    - `dump_regs.py`: 블록 레지스터 덤프
    - `dump_insns.py`: fetch가 가리키는 DRAM 명령 디코드
    - `plclk.py`: pl_clk, HP 포트 폭(AFIFM)
    - `regtest.py`
- **번들**: 순서는 `vtabundles_xcmpl/README.md`를 따른다.
  1. Debug2_remote에서 `build_sim_bundles.sh`
  2. `collect_hw_bundles.py`
  3. `build/`에서 `make -j2 hw_bundles`
  4. `sudo hw/run_all.sh`
  - `hw/run_all.sh`의 `BIN=`은 하드코딩되어 있다. 새 비트스트림 이름으로 바꿔야 한다.
  - multi2: `cmake -DBUNDLE_SET=multievtax2`, 실행 dir은 `hw_multievtax2/`
  - 새 비트스트림에는 `-DVTALIB=$PWD/../../vtalib-zcu102-<single|multi2|multi4>`로 짝이 맞는 vtalib 복사본을 쓴다.
  - PROFILE=1 비트스트림에서는 `-DNESTC_EVTA_PROFILE=ON -DNESTC_EVTA_PROFILE_AXI_MONITOR=ON`을 켠다. APM이 없는 비트스트림에서 이것을 켜면 SIGBUS가 난다.
- **보드 위 NEST-C 빌드**(필요할 때만): `Debug2_remote/`에서 `make -j2`. 새 build dir을 만들면 메모리가 부족하다(CLAUDE.md 참고).

## 3. 찾은 버그, 원인, 해결

### 3.1 [근본 원인] 보드 PL의 configuration 셀 stuck-at-0 (tile row SLICE Y219)

- **증상**: 같은 RTL이 구현할 때마다 다르게 망가졌다.
  - 333 MHz no-prof: GEMM 출력이 일정한 쓰레기 값
  - prof: compute done이 오지 않음
  - 200 MHz: ALU까지 hang
  - ILA 빌드: fetch가 쓰레기 명령을 냄, wgt 손상, deadlock
  - 이 모든 경우에 타이밍과 시뮬레이션은 깨끗했다.
- **찾은 방법**:
  - `hwtest/sramscan`으로 위쪽 BRAM 뱅크만 틀린다는 것을 확인했다.
  - JTAG readback(`hwtest/brambist/readback.tcl`)을 `cfgdiff.py`로 비트스트림·mask와 비교했다.
  - 다른 비트는 모두 **1→0**이었다.
  - 위치: clock-region row 3, type-0 frame 20586..27350, frame word 61 bit 18과 word 62 bit 0-3·12-14
  - 이것은 tile row **SLICE Y219**에 해당한다(RAMB36 Y43 / RAMB18 Y86-87 / DSP Y86-87).
  - 다시 readback, fpga_manager 재로드, JTAG 프로그래밍 모두 같은 결과였다. 장치 자체의 결함이다.
- **해결**: 설계를 Y219 행 밖에 두는 pblock fence(`vivado.tcl`의 `VTA_BRAM_FENCE`).
  - `y219` 모드에서는 PS를 뺀 모든 셀을 pblock `SLICE_X0Y0:SLICE_X96Y218`(+ RAMB36 ..Y42, RAMB18 ..Y85, DSP ..Y85)에 넣는다.
  - 이 pblock에 CONTAIN_ROUTING을 건다.
  - Y219 행의 사이트와 MMCM row 3..6에는 PROHIBIT를 건다.
  - 검증은 `stuckchk.py`로 0 extra를 확인하는 것이다.
  - 모든 설계가 공통으로 쓰는 194비트(word 62 bit 0, 미사용 타일 설정)는 기준선이라 제외한다.
- **2026-10-25 추가: 행 안의 PIP 자체도 일부 고장이다.** wire 검사 설계(`y219route/WireTest.java`, README 참고)로 확인했다.
  - 행 위를 지나가기만 하는 wire(NN/SS 2·4·12): 20,207개 모두 정상, 750 MHz까지.
  - 결함 셀을 쓰지 않는 Y219 PIP를 거친 net: 6,850개 중 382개(5.6%)가 정적으로 실패. readback은 bitstream과 같다. 열마다 고장 PIP가 다르다.
  - 그래서 규칙은 "Y219 INT 타일의 PIP 0개"다. stuckchk 0 extra인 rs_m은 레지스터 비트가 틀리고 EVTA 접근 시 보드가 멈췄다.
- 처음에는 "위쪽 BRAM(Y≥36)이 나쁘다"고 결론 냈었지만 **틀렸다**. Y36..42 BRAM은 정상이고 Y219 행만 문제다.
- 할 일: 보드 소유자에게 알린다.

### 3.2 설계/런타임 버그 (보드 결함과 별개, 모두 수정됨)

| # | 위치 | 버그 | 원인 | 수정 |
|---|---|---|---|---|
| 1 | `vivado.tcl` `pll_clk` | `pl_resetn0`가 clk_wiz까지 리셋해서, 리셋 펄스 동안 333 MHz 클럭이 멈추고 HLS 블록과 FIFO가 리셋되지 않음 | clk_wiz `RESET_PORT` | `CONFIG.USE_RESET {false}`. locked → proc_sys_reset `dcm_locked`. 로드 후 `plreset.py` |
| 2 | `pkg_config.py` (zcu102) | 1024비트 wgt 뱅크에 128비트 beat를 read-modify-write로 씀(II=2) | HLS load의 wide BRAM 쓰기 | zcu102에서는 모든 SRAM 뱅크를 AXI beat 폭(128비트)으로. wgt는 16 x 128비트. 결함 행이 주원인이었지만 이 수정은 유지 |
| 3 | `vivado.tcl` | 구현 중 HLS 블록과 큐가 재최적화되어 구현마다 달라짐 | — | fetch/load/compute/store와 7개 큐에 DONT_TOUCH |
| 4 | `pynq_driver.cc` `Run()` (vta/vtalib와 vtalib-zcu102-* 3개) | **done race**: compute `done_o`(0x18)에 이전 FINISH의 1이 남아 있어 바로 리턴함 | HLS s_axilite 출력 레지스터의 유지 동작 | 시작 전에 `done_o_ap_vld`(0x1c, clear-on-read)를 비우고, vld를 본 뒤에만 done을 인정. `hwtest/donerace` 16/16. **이전의 보드 결과(특히 conv 번들)는 의심해야 한다** |
| 5 | `hwtest/sramscan.cpp` | multi 비트스트림에서 바이트 전부 틀림 | 테스트 버그: CHUNK=256 → acc 4096행을 썼는데 multi의 acc 깊이는 2048 | `CHUNK = ACC_DEPTH / 16`, 깊이는 hw_spec.h에서 가져옴 (`3091bd8f`) |
| 6 | `hw_spec.h` (vtalib + vta-hw) | 3비트 `memory_type`/`alu_opcode`로 모든 LOAD/STORE 주소가 1비트 어긋남 | 보드 비트스트림은 2비트 | `VTA_MEMOP_ID_BIT_WIDTH 2`, `VTA_ALU_OPCODE_BIT_WIDTH 2` (`//#### JO`). 그 결과 ALU `MUL`(4)과 `MEM_ID_MOVE`(5)는 인코딩할 수 없다(BNN/XP-GEMM 경로 불가) |
| 7 | vtalib coherent | `VTA_COHERENT_ACCESSES=true`면 보드에서 hang | HP 포트가 non-coherent | `NESTC_EVTA_COHERENT_ACCESSES` 기본 OFF. 번들이 `VTABufferFlush/Invalidate`를 호출(`VTASave.cpp`가 생성) |

### 3.3 multi4: Y219를 넘는 배선 (해결, 309.6 MHz)

#### Y219 행과 pblock의 위치

![ZCU102(XCZU9EG) 다이에서 Y219 행과 multi4의 pblock](figs/zcu102_y219.svg)

- 그림은 site 열(CLB 회색, BRAM 빨강, DSP 파랑)마다 막대 하나씩, 세로축은 SLICE Y(아래 Y0, 위 Y419)다. 열 사이의 배선(INT) 열은 생략했다.
- **Y219 행(빨간 선)은 다이 높이의 정확히 가운데쯤**에 다이 폭 전체를 가로지른다. 이 행의 모든 타일(INT, CLE, BRAM Y43, DSP Y86-87)에 결함 셀이 있다.
- PS(Zynq MPSoC)는 왼쪽 아래(Y0..~179)에 있다. HP0-3 데이터 포트와 M_AXI_HPM0(레지스터)는 모두 PS의 오른쪽 경계(INT_INTF_LEFT_TERM_PSS, Y99-118)로 나온다.
- multi4의 pblock(`vivado.tcl`, `VTA_BRAM_FENCE=split`):
  - `pb_fence` = 아래 사각형(Y0..218) + 위 사각형(Y220..419). PS·MMCM을 뺀 모든 셀.
  - `pb_evta_0`, `pb_evta_1` = 아래 사각형 전체, `pb_evta_2`, `pb_evta_3` = 위 사각형 전체 (`pb_fence`의 자식).
  - **즉 EVTA마다 작은 창을 준 것이 아니라, 반쪽 다이 전체를 두 EVTA가 같이 쓴다.** pblock은 "이 셀들은 이 사각형(들) 안에만 배치하라"는 배치 제약이고, 사각형을 작게 잡으면 EVTA 하나의 위치를 창 하나로 한정할 수 있다. 여기서는 위/아래만 정하려고 반쪽 전체를 줬다.

**PROHIBIT과 CONTAIN_ROUTING은 다른 것이다** (헷갈리기 쉬움)

| 제약 | 대상 | 효과 | multi4에서 |
|---|---|---|---|
| `PROHIBIT` (site 속성) | 배치 | 그 site에 셀을 놓지 않는다. **배선은 막지 않는다** | Y219 행 site(`vivado.tcl`), `place_d.tcl`부터는 Y218-220 행 site까지 |
| pblock (`resize_pblock`) | 배치 | 셀을 사각형 안에만 놓는다 | 위 그림의 두 사각형 |
| `CONTAIN_ROUTING` (pblock 속성) | 배선 | pblock 안의 셀끼리만 연결된 net을 pblock 영역 안에서만 배선한다 | `vivado.tcl` split에서는 켬. **`place_d.tcl`부터는 끔** |

- CONTAIN_ROUTING이 Y219를 지켜 주지 못한 이유:
  - pblock이 두 사각형으로 되어 있으면 두 사각형 사이의 행(Y219)도 "pblock 안"으로 취급되는지, 자식 pblock의 net이 Y219 PIP를 썼다.
  - PS와 EVTA 2/3을 잇는 net(HP2/HP3 ↔ smc_2/3, M_AXI_HPM0 ↔ xbar ↔ EVTA 2/3 레지스터)은 어느 pblock에도 속하지 않아 제약이 없다. 이 net들은 반드시 Y219를 가로질러야 한다.
  - 게다가 CONTAIN_ROUTING을 켜면 배선이 실패했다(RTSTAT-6, 18 conflicts).
- 그래서 `place_d.tcl`(현재 배치)은 CONTAIN_ROUTING을 끄고, Y218-220 행 site를 PROHIBIT해서 배치만 비운 뒤, 배선에서 Y219 PIP를 빼는 일은 별도 도구로 한다(아래 표). Vivado에는 배선 금지 영역(routing blockage) 제약이 없다.
- 배선이 Y219 행을 "지나가는" 것(긴 wire가 행 위를 통과)은 괜찮다. 문제는 그 행의 타일 안에서 **PIP(스위치)를 켜는** 것뿐이다. 그 PIP의 설정 비트가 결함 셀에 있다.

#### 4개 EVTA가 배치된 곳

![multi4 배치: EVTA별로 쓴 site](figs/multi4_placement.svg)

- EVTA 0·1은 아래(0이 맨 아래, 1이 그 위), EVTA 2·3은 위(2가 Y219 바로 위, 3이 맨 위)다. 다이 오른쪽 끝 몇 열은 비어 있다.
- EVTA 하나 = SLICE 약 4,800~5,300, **RAMB36 143**, DSP 268. 공용(AXI interconnect, reset) = SLICE 456, RAMB36 16.

**아래쪽에 EVTA 3개를 넣을 수 있나? → 들어가지 않는다.**

| 자원 (Y219 아래, Y0..218) | 있는 것 | EVTA 3개 + 공용 | |
|---|---|---|---|
| RAMB36 (Y0..42) | 379 | 3 × 143 + 9 = 438 | **59개 부족** |
| SLICE | 14,666 | 약 14,700 + 443 | 100% 넘음 |
| DSP48E2 | 1,080 | 804 | 충분 |

- PS가 아래 왼쪽을 차지해서 아래 반쪽(392 RAMB36, Y43 행 13개는 결함 행이라 제외 → 379)이 위 반쪽(520)보다 작다.
- 버퍼(16/15/18/17: uop 64 KB, inp 32 KB, wgt 256 KB, acc 128 KB = 480 KB)는 RAMB36 약 107개 분량인데 EVTA당 143개를 쓴다. 뱅크를 128비트 폭으로 나누면서(§3.2 #2) 생기는 손실로 보인다(확인 안 함). 버퍼를 줄이면 들어갈 수 있지만 vtalib 상수와 성능이 바뀐다(§5.4).
- 어떻게 나누든 위쪽에 EVTA가 하나라도 있으면 PS ↔ EVTA net이 Y219를 넘어야 한다. 그래서 배선 단계에서 Y219 PIP를 빼는 방법이 필요하다.

#### 시도한 것

| 시도 | 결과 |
|---|---|
| `split` + 자식 pblock CONTAIN_ROUTING | RTSTAT-6 배선 실패(18 conflicts) |
| `split`, CONTAIN_ROUTING 끔(`build/y219/route_a2.tcl`) | 배선은 깨끗함. WNS −0.126 ns. **528개 net이 Y219 PIP 사용** |
| RapidWright로 Y219 PIP 금지 후 재배선(`Y219Route.java`), 하드 블록 | 29 net 실패. 대부분 Y218/Y220 sink |
| 같은 도구, PathFinder식(노드 빼앗기 + history cost) | **수렴하지 않음**. 5시간에 1003회 재배선, 같은 net을 15~19번 왕복. 중단함 |
| Y218·Y219·Y220 세 행 배치 금지 후 재배치(`build/y219/place_d.tcl`) → `routed_d.dcp` | WNS −0.100 ns, 배선 OK. Y219 PIP net 528 → 239 |
| `routed_d`에 `Y219Route.java` v1(net 전체 뜯기) → `rr_d` | Y219 PIP 0, bitstream까지 됨. **WNS −2.83 ns** (reset net이 사슬로 배선됨) |
| v2(끊긴 sink만 다시) → `rr_e` | WNS −1.75 ns |
| v3(끊긴 가지를 우회로 다시 잇기) → `rr_g` | WNS −1.99 ns, antenna DRC로 bitstream 실패. 직접 짠 A*는 지연 모델이 없어 긴 wire로 고리를 돈다 |
| RapidWright 2025.2 RWRoute partial(`Y219RWRoute.java`) → `rw_a`/`rw_b` | WNS −1.58 ns, sink 78~156개 미배선. RWRoute는 설계의 clock 제약을 몰라 criticality가 0 |
| **dummy로 Y219 노드 점유 + Vivado 재배선**(`Y219Block.java` + `build/y219/y219block.tcl`) → `blk_e` | **WNS 0.000, WHS +0.006**, 미배선 0. 그러나 dummy를 지운 뒤 **169 net이 Y219 PIP 사용** — Vivado가 sink 없는(ANTENNAS) dummy net의 노드를 막힌 자원으로 보지 않는 것으로 보임(조사 중) |

- 원인 분석(RapidWright, `~/tools/rapidwright/t/Up.java`):
  - Y218 행 IMUX 입력의 uphill 노드 절반은 Y219 타일의 INODE(_BLS)에서 온다. Y220 행은 BLN에서 같은 구조다.
  - Y219 PIP를 막으면 Y218/Y220 sink는 로컬 INT_NODE_IMUX 3개만 남는다. 그래서 혼잡해진다.
  - 그래서 바로 옆 행까지 비우면 net들은 그 행을 "지나가기만" 하게 된다.
- RapidWright 메모: 2020.1.7은 재배선기 v1-v3, 2025.2.2는 RWRoute와 `Y219Block`(2023.2는 이 설계의 clock leaf 노드에서 예외). 2025.2의 data dir에는 jar 안의 `timing/ultrascaleplus/*.txt`를 풀어 둬야 한다. Vivado 2020.1은 2025.2가 쓴 DCP를 읽는다.
- 그림 다시 만들기: `FloorDump.java <dcp> <csv>`(RapidWright 2025.2, `.edf` 필요) → `floorplan_svg.py <csv> docs/figs` (둘 다 `vta-hw-zcu102-multi4/hardware/xilinx/scripts/y219route/`).
- 2026-10-25 이후:

| 시도 | 결과 |
|---|---|
| prjuray `segbits_int.db`로 결함 셀을 쓰는 PIP 606종만 금지(`stuckpips.py`) → `Y219RWRoute.java -Dpips -Dcrit=0.95` → rs_m(321.7 MHz) | stuckchk 0 extra, 타이밍 OK. **보드 실패**: 레지스터 비트 틀림, EVTA0 hang, EVTA2/3 접근 시 보드 멈춤 |
| wire 검사 설계(정적/속도/행 안 PIP) | 지나가는 wire 정상, 행 안 PIP 5.6% 고장(§3.1) |
| **Y219 PIP 전부 금지**: `Y219RWRoute.java -Dcrit=0.95`(`-Dpips` 없음) → `flow/route_s.tcl` → rs_z | Y219 PIP net 0, 333 MHz WNS −0.488 |
| rs_z + `flow/mmcm_set.tcl` MULT 40.25 → **rs_zm (280 MHz)** | WNS +0.057, WHS −0.002(1 path). 보드: EVTA 0-3 regpat, gemmprobe, sramscan, donerace, `multi_gemm_gemm_4evta_m` 모두 PASS |
| rs_z의 333 MHz 위반은 대부분 smc_2/3(Y221-225) → PS SAXIGP4/5 WDATA. `vivado.tcl`에서 smc와 `S_AXI_HP<i>_FPD` 사이에 `axi_register_slice`(`hpslice<i>`, 전 채널 REG=1)를 넣고 `pb_hpslice`(fence 아래쪽)에 둠 → `flow/run_flow.sh hp`(place_d → Y219RWRoute → route_s) → rs_hp | Y219 PIP net 0, 333 MHz WNS −0.199. 남은 위반은 `proc_sys_reset` → compute_3 리셋 fan-out(지연의 97%가 배선) |
| rs_hp + `mmcm_set.tcl` MULT 44.5 → **rs_hpm (309.6 MHz)** | WNS +0.019, WHS +0.008. stuckchk: multi2와 같은 194개, 0 extra. 보드: regpat(3회 로드), gemmprobe 4/4, sramscan, donerace 16/16(EVTA 0-3), `multi_gemm_gemm_4evta_m` 3회 모두 Golden Matched. 같은 묶음을 두 번 돌려 모두 PASS |
| + EVTA별 `proc_sys_reset_<i>` (각 EVTA pblock 안) → **rs_rst (333 MHz)** | WNS +0.008, WHS +0.010, Y219 PIP 0. stuckchk 194, 0 extra. 보드: gemmprobe, sramscan, donerace, mg4 3회 PASS. Conv4 EVTA 시간 1.864 ms(124 GOPS/EVTA) |

- 재현 순서는 `vta-hw-zcu102-multi4/hardware/xilinx/scripts/y219route/README.md`에 있다(작업 dir `build/y219`).
- 버퍼를 줄여 4개를 모두 아래에 넣는 안은 시작했다가 중단했다.
- 타이밍: 333 MHz에서 배치 단계부터 −0.1 ns 수준이다. DONT_TOUCH를 풀고 다시 배치하는 `build/y219/place_f.tcl`이 준비되어 있다. 300 MHz로 내리려면 `pkg_config.py`의 zcu102 `fpga_freq`/`fpga_per`.

## 4. 빠지기 쉬운 함정

**툴과 버전**
- Vivado는 docker 안의 2020.1만 쓴다. 호스트의 2020.2에는 HLS가 없고, 2018.3은 버전이 다르다.
- RapidWright 버전은 Vivado와 맞춘다(2020.1.x).
  - 데이터 zip은 첫 실행 때 자동으로 받지 못한다. 릴리스의 `rapidwright_data.zip`을 직접 풀고 `RAPIDWRIGHT_PATH`를 지정했다.
- 보드에는 `clang`/`python` 심볼릭 링크가 없다. `clang++-8`, `python3`를 쓴다(Makefile의 `PYTHON ?= python3`).

**Vivado 제약**
- `get_sites -regexp {A|B}`처럼 두 사이트 타입을 한 번에 넘기면 **첫 타입만** 반환한다. PROHIBIT는 타입별로 한 줄씩 쓴다.
- CONTAIN_ROUTING은 두 사각형으로 된 pblock의 **사이 행**을 막지 못한다.
  - PS에서 나오는 net(HP rdata/rvalid, smartconnect 출구)은 어느 pblock에도 속하지 않는다.
  - Vivado에는 "이 행 배선 금지" 같은 routing blockage가 없다. 그래서 RapidWright를 쓴다.
- fence 첫 시도에서 Y219 행 PROHIBIT를 빼먹었더니 cascade된 wgt BRAM 쌍이 Y42/Y43에 걸쳐 배치되었다.
- `make`는 `vivado.tcl`이 바뀌면 bit을 다시 만든다. `hw_spec.h`, `hls.tcl`, `src/*`가 바뀌면 HLS IP까지 다시 만든다.

**타이밍**
- 333 MHz에서 multi는 슬랙이 0.003~0.05 ns뿐이다.
- PS 레지스터 포트 `M_AXI_HPM0`는 `pl_clk0` 100 MHz에서 돈다. 333 MHz에 묶었을 때는 타이밍을 놓쳤다. 클럭 컨버터는 `axi_xbar`에 있다.
- `pl_clk1..3`은 보드에서 꺼져 있다. 설계에서 쓰면 안 된다.
- **WNS ≥ 0이어도 보드에서 틀릴 수 있다**(§3.1). 반드시 stuckchk를 돌린다.

**보드와 런타임**
- `.bin` 이름은 `vtahw_` 접두사를 붙인다. etri 비트스트림(`zcu102_1x16_i8w8a32_16_16_19_18.bin`)과 CONF 문자열이 같아서 덮어쓸 위험이 있다.
- HP1-3 포트 폭은 비트스트림이 아니라 **부트 이미지**(AFIFM 레지스터)가 정한다. multi에서 0이 나오면 `plclk.py`로 확인한다. 현재 보드는 모두 128비트다.
- CMA 풀은 128 MB이고 런타임은 핸들당 64 MB를 미리 잡는다.
  - multi에서는 `VTA_MAX_XFER=8388608`(`XFER=`)로 줄인다.
  - 4-EVTA 테스트를 2-EVTA 비트스트림에서 돌리면 안 된다. EVTA2/3 레지스터가 없다.
- 레지스터 맵:
  - EVTA i: `0xA0000000 + i*0x10000`. fetch/load/compute/store는 +0x0000/0x1000/0x2000/0x3000.
  - 데이터 포트: `S_AXI_HP<i>_FPD`
  - APM(prof): `0xA0050000 + i*0x10000`
  - 자세한 내용은 BOARD_TODO "Register map".
- `dump_regs.py`로 compute 0x1c를 읽으면 안 된다. 실행 중에 읽으면 vld가 지워진다.
- vta-hw의 `reset_reg` GEMM은 out에 **이전 acc+곱** 값을 쓴다. etri HW는 0을 쓴다. 의미가 다르다(`gemmprobe reset`).

**ILA**
- 프로브 이름은 opt 이후 `vta_i/<net>` 또는 `u_ila_0_<net>`가 된다. Tcl regexp로 매칭한다.
- BASIC capture + 펄스 qualifier를 쓸 때는 `TRIGGER_POSITION 0`으로 둔다.
- qualifier는 TVALID가 아니라 TREADY에 건다.
- 넓은 프로브(1024비트 wgt)를 달면 타이밍이 깨진다(`_ila5` WNS −0.9).

**git / 트리**
- vta-hw 소스는 이 저장소의 `vta-hw-zcu102-*`(upstream 복사본에 `#### JO` 수정)다. `tvm/3rdparty/vta-hw` 서브모듈은 손대지 않는다.
- `build/`는 git-ignore 대상이다. ILA/cosim 스크립트와 `.bin`은 git에 없다.

## 5. GEMM: HW ↔ 컴파일러 인터페이스

### 5.1 파일 지도

| 층 | 파일 | 내용 |
|---|---|---|
| HW config | `vta-hw-zcu102-*/config/vta_config.json` | LOG_* 크기 (single 16/16/19/18, multi 16/15/18/17) |
| | `vta-hw-zcu102-*/config/pkg_config.py` | zcu102 타깃(xczu9eg, 333 MHz, non-coherent `axi_cache 0011`, beat 폭 뱅크) |
| HW ISA | `vta-hw-zcu102-*/include/vta/hw_spec.h` | 명령어와 uop 비트필드. **vtalib `include/zcu102/vta/hw_spec.h`와 같은 파일이다(diff 없음)** |
| HLS | `vta-hw-zcu102-*/hardware/xilinx/src/vta.cc` | `fetch`(132), `load`(169), **`gemm`(236)**, `alu`(327), `compute`(415, prof 카운터 438-440), `store`(561) |
| | `.../src/vta.h` | HLS 타입(`insn_T`, `uop_T`, `bus_T`, idx 타입) |
| | `.../scripts/hls.tcl` | HLS 합성(CSIM/PROFILE 인자) |
| | `.../scripts/vivado.tcl` | 블록디자인: 큐 FIFO 32B, SRAM 뱅크, HP 포트, APM, fence |
| 런타임 | `vtalib-zcu102-*/include/zcu102/cmd_queue.h`, `lib/zcu102/api.cc` | `VTALoadBuffer2D`, `VTAStoreBuffer2D`, `VTAUopLoopBegin/End`, `VTAUopPush`, `VTAPushGEMMOp`, `VTADepPush/Pop`, `VTASynchronize` |
| | `vtalib-zcu102-*/lib/zcu102/pynq_driver.cc` | 레지스터 쓰기와 `Run()`(done race 수정, APM) |
| | `vtalib-zcu102-*/CMakeLists.txt` | `-D` 매크로(크기, 주소, 오프셋, `NESTC_EVTA_MULTI` → 15/18/17) |
| 커널 | `vtalib-zcu102-*/lib/Bundle/VTABundle.cpp` | `reset_acc`(89), `gemmop_mxn`(132), `gemmop_1x1`(156), **`conv_kernel`(342)**, `convolution_wo_tr`(2547) 등 |
| | `vtalib-zcu102-*/lib/Bundle/CPUBundle_aarch64.cpp` | `transpose_nhwc2vtaio`(4990), `transpose_nhwc2vtak`(5029): DRAM 레이아웃 변환 |
| 컴파일러 | `lib/Backends/VTA/VTA.cpp` | 지원 op 판정, VTAConv 그래프 최적화(채널 /group % 16 검사 509-515, 필터 6차원 재배열 566, `createVTAConv` 591) |
| | `lib/Backends/VTA/VTASave.cpp` | 번들 C++ 생성. `saveConvolutionInst`(3066)가 VTA/non-VTA를 고르고, `generateVTAConvolutionCall`(2084)/`generateEVTAConvolutionCall`(2219)이 `convolution_wo_tr(...)`를 출력. 입력 transpose와 `VTABufferFlush`(1654~) |
| | `lib/Backends/VTA/ClassGen/VTASpecific{Nodes,Instrs}.h` | `VTAConvolution` 노드와 명령 |
| | `include/Backends/VTA/` | 백엔드 헤더 |
| 테스트 | `vtabundles_xcmpl/hwtest/{gemmprobe,sramscan,donerace}.cpp` | GEMM 경로를 직접 두드리는 프로브 |

### 5.2 명령어 포맷 (256비트, `hw_spec.h`)

- 명령어 하나 = 32바이트(`VTA_LOG_INS_WIDTH 8`). upstream VTA는 128비트다.
  - fetch→load/gemm/store 큐 FIFO도 32바이트 폭이다(`vivado.tcl`이 hw_spec.h의 INSN_BYTES를 따름).
- opcode 3비트: LOAD 0, STORE 1, GEMM 2, FINISH 3, ALU 4, XP_GEMM 5.
- 의존성 토큰 4비트: `pop_prev_dep`, `pop_next_dep`, `push_prev_dep`, `push_next_dep`.
  - load↔compute(l2g/g2l)와 compute↔store(g2s/s2g) 큐로 오간다.
  - 런타임 `VTADepPush/Pop(from, to)`에서 1=load, 2=compute, 3=store.
- **`VTAMemInsn`**(LOAD/STORE):
  - `memory_type`는 **2비트**: UOP 0, WGT 1, INP 2, ACC 3. OUT(4)/MOVE(5)는 2비트로 표현할 수 없다.
  - 그 밖의 필드: `sram_base` 16, `dram_base` 32(원소 단위 주소), `y_size`/`x_size`/`x_stride` 각 16, `y_pad_0/1`·`x_pad_0/1` 각 4.
- **`VTAGemInsn`**:
  - 앞부분: `reset_reg` 1, `uop_bgn` (LOG_UOP_DEPTH=13), `uop_end` 14, `iter_out`/`iter_in` 각 14
  - factor 필드:
    - `dst_factor_out/in`: LOG_ACC_DEPTH
    - `src_factor_out/in`: LOG_INP_DEPTH
    - `wgt_factor_out/in`: LOG_WGT_DEPTH
  - 마지막: `gemm_from_output` 1
  - 실행 의미(`vta.cc gemm`):
    ```
    for it_out < iter_out: for it_in < iter_in: for upc in [uop_bgn, uop_end):
      dst = uop.dst + it_out*dst_factor_out + it_in*dst_factor_in   (src, wgt 동일)
      acc[dst] += inp[src] (1x16 int8) · wgt[wgt]^T (16x16 int8)   → int32 1x16
      out[dst] = acc[dst] 하위 8비트 ; reset_reg면 acc[dst]=0 (out은 위 값)
    ```
- **`VTAAluInsn`**: `alu_opcode` **2비트**(MIN 0, MAX 1, ADD 2, SHR 3; MUL 불가), `use_imm`, `imm` 16비트 signed.
- **비트 배치는 GCC 비트필드 규칙을 따른다**: 64비트 워드에 들어가지 않는 필드는 다음 워드로 넘어간다.
  - 확인한 예: GEMM `wgt_factor_in`은 bits 138:128, ALU `imm`은 143:128.
  - HLS가 같은 방식으로 디코드하는 것을 생성된 `compute.v`에서 확인했다.
  - 필드 폭을 바꾸면 HW와 SW 양쪽에서 다시 확인해야 한다. upstream 헤더에는 static_assert가 없다.
- **uop**(`VTAUop`, 64비트): `dst_idx`(LOG_ACC_DEPTH), `src_idx`(LOG_INP_DEPTH), `wgt_idx`(LOG_WGT_DEPTH).

### 5.3 버퍼 레이아웃과 크기

| 버퍼 | 원소 | 원소 크기 | single 깊이 (log2 B) | multi 깊이 (log2 B) |
|---|---|---|---|---|
| uop | 1 uop | 8 B | 8192 (16) | 8192 (16) |
| inp | 1x16 int8 | 16 B | 4096 (16) | 2048 (15) |
| wgt | 16x16 int8 | 256 B | 2048 (19) | 1024 (18) |
| acc | 1x16 int32 | 64 B | 4096 (18) | 2048 (17) |
| out | 1x16 int8 | 16 B | acc와 같은 인덱스 | |

- HW 뱅크 구성(zcu102, `pkg_config.py`): 모든 뱅크가 AXI beat 폭 128비트다.
  - wgt 원소 하나는 16개 뱅크 x 128비트다.
  - acc는 4 x 128비트다.
  - 깊이 공식은 `hw_spec.h` 56-122줄.
- **DRAM 레이아웃**(번들이 CPU에서 변환하고 `VTABufferFlush`한다):
  - 입력/출력 활성값: NHWC → **N, C/16, H, W, 16** (`transpose_nhwc2vtaio`). 역변환은 `transpose_vtaio2nhwc`.
  - 필터: KN,KH,KW,C(NHWC) → **KN/16, C/16, KH, KW, 16(n), 16(c)** (`transpose_nhwc2vtak`). wgt 원소 = 16x16 블록.
  - 컴파일러 쪽 `VTA.cpp:566`의 6차원 재배열(`F8H.at({c0/16, c3/16, c1, c2, c0%16, c3%16})`)도 같은 레이아웃이다.
  - bias: int32, acc 원소 단위로 LOAD ACC.
- **conv 타일링**(`VTABundle.cpp conv_kernel`):
  - acc reset(uop 루프 1개) 뒤 입력 채널 16개 단위로 반복한다: LOAD INP(패딩 포함 2D), LOAD WGT(`KH*KW` x `nFilterinLoop`), GEMM.
  - 입력 SRAM은 `INP_MEM_ENTRY/2` 두 영역을 번갈아 쓰는 double buffer다.
  - 이어서 ALU(bias ADD, SHR, relu MAX/MIN)와 STORE.
  - `nVirtualThread`로 acc/wgt SRAM을 나눈다.
  - 최소 타일 7x7(`MIN_H_SIZE/MIN_W_SIZE`)보다 작으면 더미 행/열로 채운다.

### 5.4 파라미터 config가 맞아야 하는 곳 (하나를 바꾸면 전부 바꾼다)

1. `vta-hw-zcu102-*/config/vta_config.json`: HW 크기. CONF 문자열과 HLS `-D`가 여기서 나온다.
2. `vtalib-zcu102-*/CMakeLists.txt`(또는 `vta/vtalib/CMakeLists.txt`): `-DVTA_LOG_*_BUFF_SIZE`. `NESTC_EVTA_MULTI`이면 15/18/17.
3. `vtabundles_xcmpl/hwtest/build.sh`: `BUFF="inp wgt acc"`(기본 `16 19 18`).
4. `hw_spec.h`(두 복사본): ISA 폭(memory_type/alu_opcode 2비트, 256비트 insn).
5. **`VTABundle.cpp conv_kernel`의 하드코딩 상수**: `ACC_MEM_ENTRY=2048`, `INP_MEM_ENTRY=2048`, `WGT_MEM_ENTRY=1024`(379-381, 783-785, 1193-1195, 1589-1591).
   - 이 값은 multi 깊이다. **single 비트스트림에서도 커널은 버퍼의 절반만 쓴다.**
   - 버퍼를 multi보다 더 줄이면 여기를 반드시 고쳐야 한다. 그렇지 않으면 SRAM 범위를 넘는다.
     (2026-10-25: 15/15/17/16 축소안을 준비하면서 2·3번만 맞추고 이곳을 빠뜨렸다. 그 안은 중단했다.)
   - 할 일(multi4 작업 뒤): `hw_spec.h`의 `VTA_INP_BUFF_DEPTH` / `VTA_WGT_BUFF_DEPTH` / `VTA_ACC_BUFF_DEPTH`로 바꾼다.
     wgt 깊이 매크로에는 `VTA_BUFFER_NTIMES`가 곱해져 있으니 값을 먼저 확인한다. single에서는 타일이 2배가 되므로
     single 번들을 다시 검증해야 한다.
6. `hw_spec.h` `VTA_LOG_OUT_BUFF_SIZE`는 vtalib CMake에 16으로 하드코딩되어 있다(HANDOFF.md 발견 #10).
7. uop 버퍼(`VTA_LOG_UOP_BUFF_SIZE`): vtalib CMake에 16으로 하드코딩, hwtest는 `UOP=`(기본 16).
8. 컴파일러가 VTA conv를 고르는 조건(`VTASave.cpp:3093`): group==1, 입력 C % 16 == 0, KN % 16 == 0, 모든 offset 0, `scale > 1`이고 2의 거듭제곱 shift ≥ 0, `biasScale == inScale*filterScale`. 하나라도 어기면 CPU(`nonvtaconvolution`)로 간다.

**비트스트림과 런타임 중 어디에 들어가나**

| 항목 | 비트스트림(HLS/Vivado) | 런타임(vtalib, 번들, hwtest) |
|---|---|---|
| 1 `vta_config.json` | **여기서 결정**: HLS `-D`, BRAM 뱅크 크기, CONF 이름 | — |
| 2 vtalib CMake `-DVTA_LOG_*_BUFF_SIZE` | — | 런타임과 번들이 쓰는 `hw_spec.h` 매크로 값 |
| 3 hwtest `BUFF=` / `UOP=` | — | hwtest 바이너리 |
| 4 `hw_spec.h` ISA 폭 | **양쪽 모두**(vta-hw 복사본은 HLS, vtalib 복사본은 런타임) | 〃 |
| 5 `VTABundle.cpp` 상수 | — | 번들의 conv 타일링 |

- 5번을 매크로로 바꾸는 것은 **런타임만의 변경이라 비트스트림을 다시 만들 필요가 없다.** 다만 매크로 값은 2번에서 오므로,
  2번이 그 비트스트림의 1번과 같아야 한다.
- 버퍼 크기 자체를 바꿀 때는 1번(비트스트림 재빌드)과 2·3·5번(런타임 재빌드)을 함께 바꾼다.
- 런타임이 비트스트림의 크기를 확인하는 장치는 없다. 맞지 않으면 SRAM 범위를 넘거나 결과가 틀릴 뿐 오류는 나지 않는다.

## 6. 유용한 경로 모음

- 기록:
  - `log/BOARD_TODO.md`: 시간순 결과. 새 결과는 여기에 절을 추가한다.
  - `log/board_20261020/`, `log/host_2026102{1,2}/`: 덤프, ILA csv
- 이전 리뷰 핸드오프: `HANDOFF.md`(vta-hw vs upstream 비교, 발견 사항 #1-#22)
- 결함 도구: `vtabundles_xcmpl/hwtest/brambist/`(`readback.tcl`, `cfgdiff.py`, `stuckchk.py`, `progrb.tcl`, brambist v1/v2)
- multi4 작업 dir: `vta-hw-zcu102-multi4/build/y219/`
  - 스크립트: `route_a2.tcl`, `place_d.tcl`, `route_c.tcl`, `chk.tcl`/`chkbody.tcl`
  - 로그: `rr_*.log`
- 재배선기: `vta-hw-zcu102-multi4/hardware/xilinx/scripts/y219route/Y219Route.java`
- RapidWright 프로브: `~/tools/rapidwright/t/`(`Up.java`: 노드 uphill 분석)
- 커밋 규칙:
  - `git commit -s`(DCO)
  - 커밋은 요청이 있을 때만 한다. 지금까지는 보드 검증 뒤에 커밋했다.
