# vta_synth_notes — EVTA 비트스트림 합성/보드 디버깅 핸드오프

최종 갱신: 2026-10-27 (§8 systolic 검토, Phase A). 이 문서는 다음 세션이 바로 이어서 작업할 수 있도록, 명령·경로·함정·코드 위치를 모은 것이다.

- 실험 경위와 상세 근거는 `log/BOARD_TODO.md`(시간순 기록)에 있다.
- 디버깅 보고서는 https://claude.ai/artifact/4KvtghuqpzV1GKunEyTRdf (Claude Docs)에 있다.
- 속도 측정 보고서(EVTA 4개 609.7 GOPS, ResConv10 구조와 동작): `docs/evta_multi4_609gops_report.md`. 요약은 §6.
- 이 문서는 "지금 무엇이 되고, 어떻게 돌리며, 어디가 위험한가"만 다룬다.

## 0. 현재 상태 한눈에

| 구성 | 소스 트리 | 상태 | 보드 파일 |
|---|---|---|---|
| single (EVTA 1개, 16/16/19/18) | `vta-hw-zcu102-single` + `vtalib-zcu102-single` | **동작**: 30개 번들 golden PASS. 커밋 `6fa74ee1` | `bit/evta_single_y219_333MHz_pass_261023/` |
| multi2 (EVTA 2개, 16/15/18/17) | `vta-hw-zcu102-multi2` + `vtalib-zcu102-multi2` | **동작**: 전 테스트와 Resnet18 x2 PASS. 커밋 `50201ac5` | `bit/evta_multi2_y219_333MHz_pass_261024/`, `/lib/firmware/vtahw_multi2_y219.bin` |
| multi4 (EVTA 4개, 16/15/18/17) | `vta-hw-zcu102-multi4` + `vtalib-zcu102-multi4` | **333 MHz로 동작** (2026-10-26): Y219 안의 PIP를 하나도 쓰지 않게 재배선, HP 포트 앞 register slice, EVTA별 proc_sys_reset(§3.3). hwtest 전부 PASS, 번들 run_all 전부 PASS. **ResConv10 4-EVTA 동시 609.7 GOPS**(§6), Conv4 4-EVTA 491.6 GOPS. 커밋 `dfec80f7` | `bit/evta_multi4_y219_333MHz_pass_261026/vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta.bin` (309.6 MHz판 `bit/evta_multi4_y219_261025/rs_hpm.bin`) |

- 기본 비트스트림(2026-10-26): single `vtahw_single_y219.bin`, multi2 `vtahw_multi2_y219.bin`, multi4 `vtahw_multi4_y219.bin`(= rs_rst).
  `vtabundles_xcmpl/collect_hw_bundles.py`의 `SETS`와 각 `run_all.sh`의 `BIN=${BIN:-...}` 기본값이다.
- 비트스트림 dir마다 `README.md`가 있다(보드에만 있고 git에는 없음): 파일, md5, WNS, 소스 커밋, 런타임 설정.

남은 일의 순서:
1. ~~multi4 333 MHz: EVTA마다 리셋 블록(`proc_sys_reset_<i>`)~~ 완료(rs_rst, §3.3)
2. `_prof` 비트스트림(PROFILE=1: compute 카운터, AXI perf monitor)을 새 fence로 다시 생성. 필요해질 때까지 미룬다
   (VTA_SYNC의 EVTA 시간은 SW 오버헤드가 약 1%뿐이어서 지금은 그것으로 충분하다, §6.1).
3. 보드 소유자에게 PL 결함 통보
4. 성능: conv 사이 CPU 간격(66 µs, 4.4%)을 명령 버퍼 두 개로 숨기기, 실제 네트워크를 EVTA 여러 개에 나누기(§6.5)
5. 설계 검토 중: GEMM을 systolic(weight-stationary)으로 바꾸기. 영향 범위와 순서는 §8(다른 세션에서 여기서 시작)

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
  - `run_all.sh`/`run_prof.sh`는 `BIN=${BIN:-<set 기본값>}`이다. 다른 비트스트림은 `sudo BIN=<x>.bin ./run_all.sh`.
    `run_prof.sh`는 `run_all.sh`에서 BIN 기본값을 읽고, `/lib/firmware/$BIN`이 없으면 멈춘다(2026-10-26 `a37e3e0b`, §3.4 #3).
  - 두 스크립트 모두 `/tmp/evta_run.lock`을 `flock -n`으로 잡는다. 다른 EVTA 실행이 있으면 바로 끝난다(§3.4 #4).
    직접 스크립트를 쓸 때도 `exec 9>/tmp/evta_run.lock; flock -n 9 || exit 1`을 넣는다.
  - set: `single`(`hw/`, `build*/`), `multievtax2`(`hw_multievtax2/`, `cmake -DBUNDLE_SET=multievtax2`),
    `multievtax4`(`hw_multievtax4/`, `-DBUNDLE_SET=multievtax4 -DVTALIB=<root>/vtalib-zcu102-multi4`).
    `_prof`/`_timed` 빌드는 `build_<set>_prof`, `build_<set>_timed`.
  - 새 비트스트림에는 `-DVTALIB=$PWD/../../vtalib-zcu102-<single|multi2|multi4>`로 짝이 맞는 vtalib 복사본을 쓴다.
    지금은 원본 `vta/vtalib`와 복사본 3개가 주석까지 같다(`config.mk` 제외). 복사본은 런타임 실험용으로 남겨 둔다.
  - `VTA_MAX_XFER`는 vtalib CMake의 `NESTC_EVTA_MAX_XFER`다(2026-10-26 `e119fac9`, vtalib `f716ed3`).
    비우면 `NESTC_EVTA_MULTI`일 때 4 MB, 아니면 `driver.h` 기본 32 MB. xcmpl은 multievtax2 8 MB, multievtax4 4 MB를 넘긴다.
  - PROFILE=1 비트스트림에서는 `-DNESTC_EVTA_PROFILE=ON -DNESTC_EVTA_PROFILE_AXI_MONITOR=ON`을 켠다. APM이 없는 비트스트림에서 이것을 켜면 SIGBUS가 난다.
    `NESTC_EVTA_PROFILE=ON`만 켜면(APM 없음) 일반 비트스트림에서도 VTA_SYNC(EVTA run 시작부터 done까지)가 기록된다.
  - 생성기가 만드는 multi dir(`MultiEVTAConv4Test`, `MultiEVTAResConv10Test`)은 `hw_multievtax4/MultiEVTAConv4TestSrc/make_multi4conv4.py`가
    `hw/<Name>Test`에서 만든다. single set을 먼저 collect해야 한다.
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

### 3.3 multi4: Y219를 넘는 배선 (해결, 309.6 MHz → 333 MHz)

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

#### 333 MHz까지 간 과정 (rs_hpm → rs_rst, 2026-10-25~26)

1. **rs_z**(Y219 PIP 0개)의 333 MHz 위반은 대부분 위쪽 smartconnect `smc_2/3`(Y221-225)에서 아래쪽 PS `SAXIGP4/5`(HP2/HP3)
   WDATA로 가는 경로였다. 이 경로는 Y219를 반드시 건너고, 건널 때 행 안에서 방향을 바꿀 수 없어 길게 돈다.
   - 수정: `vivado.tcl`에서 smc와 `S_AXI_HP<i>_FPD` 사이에 `axi_register_slice`(`hpslice<i>`, 모든 채널 REG=1)를 넣었다.
   - 이것을 fence 아래쪽 pblock `pb_hpslice`에 두었다. 건너는 구간이 레지스터 사이 한 단계가 된다.
   - 함정: fence xdc의 `-regexp {vta_i/hpslice\d}`가 아무것도 잡지 못했다. glob으로 바꾸고, `place_d.tcl`이 다시 추가한다.
   - 결과 **rs_hp**: 333 MHz WNS −0.199.
2. rs_hp의 남은 위반은 전역 `proc_sys_reset` 하나에서 compute_3(위쪽 맨 끝)으로 가는 리셋 fan-out이었다.
   지연의 97%가 배선이었다.
   - 우선 MMCM을 309.6 MHz(MULT 44.5, `flow/mmcm_set.tcl`)로 내린 **rs_hpm**으로 보드를 확인했다(WNS +0.019).
3. **rs_rst**: `vivado.tcl` `add_evta`가 EVTA마다 `proc_sys_reset_<i>`를 만들고 그 EVTA의 pblock 안에 둔다.
   - 리셋 net이 Y219를 건너지 않게 됐다.
   - `flow/run_flow.sh rst`(place_d → Y219RWRoute → route_s) 결과: WNS +0.008, WHS +0.010, Y219 PIP 0, stuckchk 194(0 extra).
   - 보드: gemmprobe 4/4, sramscan 0 wrong, donerace 16/16(EVTA 0-3), mg4 3회 Golden Matched.
   - 번들(`hw_multievtax4/run_all.sh`)은 MultiEVTAConvTest와 MultiEVTAConv4Test가 PASS, Resnet18 4 copy와 single이 281.
     MultiEVTAResConv10Test는 직접 실행(§6)에서 4개 출력 모두 golden이었다(`run_all.sh`로는 아직 안 돌림).
   - 비트스트림: `bit/evta_multi4_y219_333MHz_pass_261026/`, 커밋 `dfec80f7`.
   - 309.6 → 333 MHz(+7.7%)로 Conv4 EVTA 시간은 1.909 → 1.864 ms(−2.4%)만 줄었다. load/store가 GEMM 뒤에 숨지 않는 비율이 크기 때문이다.
     ResConv10은 GEMM 가동률 93.5%라 클럭에 거의 비례한다(§6.2).

### 3.4 2026-10-26: 런타임·번들 흐름에서 찾은 문제 (비트스트림과 별개)

| # | 증상 | 원인 | 수정 |
|---|---|---|---|
| 1 | 보드의 `vtalib-zcu102-single/multi2`에 done race 수정(§3.2 #4)이 없었다. 그 전의 일부 결과는 수정 없는 런타임으로 잰 것이다 | PC에서 고치고 push했지만 보드가 pull하지 않았다. 반대 방향으로 파일을 옮겨 맞추다 엇갈렸다 | 보드에서 `git pull --ff-only`. vtalib 서브모듈은 보드에서 init되지 않았으므로 `vta/vtalib` 안에서 pull한다. 바뀐 런타임으로 다시 실험해 single Conv4/ResConv1, multi2/multi4 run_all 모두 PASS, Resnet18 281. 이후 규칙: 보드에서 고치고 보드에서 push, PC는 pull. 복사본을 맞추기 전에는 (1) 보드의 원본과 복사본, (2) 보드와 PC(주석 포함)를 비교해 어느 쪽이 새것인지 먼저 정한다 |
| 2 | `build_*_timed`에서 cmake 실패: 같은 `mxnet_exported_resnet18.cpp`에 custom command가 두 개 | multi set에서 한 Dir의 실행 파일 두 개(single/multi thread)가 같은 .cpp를 쓰는데, timed 사본 경로가 같았다 | timed 사본 경로를 `timed/<Dir>/<target>/<file>`로 바꿨다. 실행 파일 하나에 timed .cpp가 여러 개면 `timing_<stem>.csv`로 따로 쓴다(`8002aeab`) |
| 3 | `run_prof.sh`가 비트스트림을 다시 로드하지 않았다(이전 실행의 PL에 그대로 돎) | `run_all.sh`의 `BIN=${BIN:-x.bin}` 줄을 그대로 읽어 `BIN`이 리터럴 `${BIN:-x.bin}`이 됐다. firmware 쓰기가 실패했지만 계속 진행했다 | sed로 기본값만 뽑고, `/lib/firmware/$BIN`이 없으면 멈춘다(`a37e3e0b`) |
| 4 | 사용자의 `run_prof.sh -s _timed`(multi4)와 내 single `_prof` 실행이 겹쳤다: `Internal error: Oops: 9600014e`(SError), bus error(exit 135), MISMATCH, 중간에 "writing ... to Xilinx ZynqMP FPGA Manager", EVTA별 시간 표가 안 나옴 | 실행마다 PL을 다시 로드하므로, 한 실행이 다른 실행이 쓰는 중인 PL을 바꿔 넣었다 | 혼자 다시 돌려 모두 PASS. `run_all.sh`/`run_prof.sh`가 `/tmp/evta_run.lock`을 잡는다(`68a51f05`). 공유 보드이므로 실행 전 `ps`/`dmesg`도 확인한다 |
| 5 | `build_multievtax2`에 새 타깃(`multi4conv4Bundle`)이 없음 | `add_bundle_if_present`는 configure할 때 hw dir에 소스가 있어야 타깃을 만든다. collect 전에 configure했다 | collect 뒤 `cmake .` 다시 실행 |
| 6 | multi set의 `VTA_MAX_XFER`가 xcmpl `CMakeLists.txt`에만 있었다. vtalib만 가져가는 사람은 32 MB 기본값으로 CMA가 모자란다 | — | vtalib CMake 옵션 `NESTC_EVTA_MAX_XFER`(`e119fac9`, vtalib `f716ed3`) |
| 7 | 4-EVTA conv 측정이 412~579 GOPS로 실행마다 달랐다 | 스레드 4개가 각자 OpenMP 4개(기본값)를 띄워 4코어에 16개가 됐다. conv 사이 CPU 명령어 생성이 늦어지고, 가장 늦은 EVTA가 wall을 정한다 | conv 반복 측정은 `OMP_NUM_THREADS=1`. ResConv10 609.7, Conv4 491.6~493.4로 3회 모두 같다(§6) |
| 8 | 측정 도중 보드 ssh가 몇 분씩 끊겼다. 보드에서 `git push`가 github.com:22 타임아웃 | 보드 쪽 네트워크(재부팅은 없었음, `uptime -s` 그대로). 인터넷은 되고 22번 포트만 막혔다 | 긴 측정은 `nohup`으로 보드에서 돌리고 결과 파일을 나중에 읽는다. push는 연결이 돌아온 뒤 다시 한다. `pkill -f "git push"`를 ssh 명령 안에서 쓰면 그 ssh 셸 자신도 죽는다 |

- single 기본 비트스트림을 etri `zcu102_1x16_i8w8a32_16_16_19_18.bin`에서 `vtahw_single_y219.bin`으로 바꿨다(`50cdfe85`).
  - 둘 다 35개 번들이 돌고 golden 30개가 PASS다. stuckchk도 같다(194, 0 extra).
  - EVTA 시간 비율(etri/Y219)은 큰 conv와 Resnet18 1.01, Resnet50 1.07, ResConv5/8·Vgg16Cifar10·Conv_inception_5b_5x5 1.10~1.21이다.
    Y219판이 그만큼 빠르다.

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
- CMA 풀은 128 MB이고 런타임은 핸들당 2 x `VTA_MAX_XFER`(기본 32 MB → 64 MB)를 미리 잡는다.
  - multi에서는 vtalib CMake `NESTC_EVTA_MAX_XFER`(multi2 8 MB, multi4 4 MB)로 줄인다. hwtest는 `XFER=`.
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

## 6. 속도 측정

자세한 내용(ResConv10 구조, 명령 흐름, 측정 프로그램, 원자료)은 `docs/evta_multi4_609gops_report.md`에 있다.

### 6.1 측정 방법 (무엇을 재는가)

| 방법 | 빌드 / 명령 | 재는 것 | 주의 |
|---|---|---|---|
| 번들 전체 시간 | `build*/`, `run_all.sh` | 번들 main의 2번째 실행 시간(`CLOCK_MONOTONIC`) | CPU 연산 포함 |
| VTA_SYNC | `build*_prof/`(`-DNESTC_EVTA_PROFILE=ON -DBUNDLE_EXE_SUFFIX=_prof`), `run_prof.sh` → `prof.log` | `VTASynchronize`마다 EVTA run 시작부터 done까지(`exe_time`)와 타임스탬프, `total_vta_time` | SW 오버헤드 약 1%. PROFILE=1 비트스트림 없이도 됨. compute 카운터 값(`gemm_count` 등)은 PROFILE=1 비트스트림에서만 의미가 있고, 사이클이 아니라 명령에서 센 수다 |
| 문장별 시간 | `build*_timed/`(`-DBUNDLE_TIMED=ON -DBUNDLE_EXE_SUFFIX=_timed`), `run_prof.sh -s _timed` → `timed.log`, `timing*.csv` | `timed_bundle.py`가 번들 .cpp의 문장마다 시간 측정을 넣는다. conv/FC는 GOPS도 | `convolution_wo_tr` 시간 = CPU 명령어 생성 + EVTA + 완료 대기 |
| 여러 EVTA 동시 | `hw_multievtax{2,4}/MultiEVTA<Name>Test/multi4<name>Bundle <mask> <iters> [conv]` | barrier로 같이 출발해 마지막 EVTA가 끝날 때까지의 wall, EVTA별 시간, golden 비교 | `conv`는 `convolution_wo_tr`만 반복. **`OMP_NUM_THREADS=1`로 잰다** |

### 6.2 결과 (2026-10-26, 333 MHz, 모두 golden 일치)

| 측정 | EVTA 1개 | 2개 | 4개 |
|---|---|---|---|
| ResConv10(7x7x512→512) conv 반복, multi4 | 152.6 GOPS | — | **609.7 GOPS** (3회 609.6~609.7) |
| ResConv10, multi2 | 152.8 | 305.5 | — |
| Conv4(56x56x64→64) conv 반복, multi4 | 123.5 | — | 491.6~493.4 |
| Conv4, multi2 | 123.6 | 247.1 | — |
| ResConv10 EVTA 시간(VTA_SYNC) | 1.4473 ms | — | 1.4491 ms |
| ResConv10 번들 그대로 반복(CPU transpose 포함) | — | — | 296.6 GOPS |

- 이론 최대는 EVTA당 256 MAC x 2 x 333.3 MHz = 170.7 GOPS(4개 682.7)다. 609.7은 그 89.3%다.
- ResConv10 GEMM 451,584회 = 1.3548 ms, EVTA 시간 1.449 ms → GEMM 가동률 93.5%. Conv4는 1.864 ms로 73%다.
- 4개를 동시에 돌려도 EVTA 시간이 0.12%만 늘었다. EVTA당 1.66 GB/s, 4개 6.65 GB/s를 써도 HP 포트·DDR 경쟁은 없다.
- conv 사이 CPU 간격은 66 µs(4.4%)다. 측정 구간의 82.8%에서 4개가 모두 돌고 있었다.
- single `_timed` 전체 순위(보고서 §5.1): 상위는 모두 231.2 Mops 3x3 conv(7x7x512, 14x14x256)로 146~152.5 GOPS,
  stage1 모양(56x56x64)은 122~124 GOPS. 출력이 tile 하나에 들어가고 입력 채널 반복이 긴 conv가 빠르다.

### 6.3 함정

- 기본 OpenMP 설정(스레드마다 4개)에서는 4-EVTA conv가 558~579(ResConv10), 412~476(Conv4) GOPS로 낮고 실행마다 달랐다.
- MultiEVTAResnet18의 layer별 GOPS(149)는 동시 값이 아니다. copy 4개의 같은 layer가 최대 120 ms 어긋나 돌았다.
- 실행마다 비트스트림 재로드, 실행 lock(§2.4). 다른 사람의 실행과 겹치면 SError가 나고 결과도 틀린다(§3.4 #4).

### 6.4 이전 측정과의 관계

- 309.6 MHz rs_hpm: Conv4 EVTA 1.909 ms, conv 반복 457.3 GOPS(4개).
- 333 MHz rs_rst: Conv4 EVTA 1.864 ms, 489.6~493.4 GOPS(4개).
- single 비트스트림(16/16/19/18)의 ResConv10 EVTA 시간도 1.449 ms다. 커널 상수(`ACC/INP/WGT_MEM_ENTRY`)가 multi 깊이라
  타일링이 같다(§5.4 #5).

### 6.5 다음에 할 수 있는 것

- conv 사이 CPU 간격 숨기기: 명령 버퍼를 둘로 나눠 EVTA가 도는 동안 다음 conv의 명령을 만든다. 4개 합계 약 638 GOPS까지 가능.
- 실제 네트워크에서 EVTA 여러 개 쓰기: layer 안에서 출력 채널을 나누거나(intra-layer split), 여러 입력을 EVTA마다 나눈다.
  Resnet18 4 copy 동시 실행은 CPU(conv0, transpose)가 병목이다(WORKLOG, README To do).

## 7. 유용한 경로 모음

- 기록:
  - `log/BOARD_TODO.md`: 시간순 결과. 새 결과는 여기에 절을 추가한다.
  - `log/board_20261020/`, `log/host_2026102{1,2}/`: 덤프, ILA csv
- 속도 측정: `docs/evta_multi4_609gops_report.md`, 측정 프로그램 `vtabundles_xcmpl/hw_multievtax4/MultiEVTAConv4TestSrc/`,
  결과 `vtabundles_xcmpl/hw*/*/timing*.csv`, `prof.log`, `*_summary.txt`(git 제외)
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

## 8. 설계 검토: GEMM을 systolic(weight-stationary) 구조로 (2026-10-27, 시작 전)

다른 세션이 여기서 이어 시작할 수 있게 정리한 검토다. 아직 코드는 하나도 바꾸지 않았다.

### 8.1 제안된 구조

사용자 제안(16x16 어레이, PE(ic, oc)):
- **입력**: 1x16 입력 벡터를 길이가 다른 shift register로 넣는다. 위 행부터 한 클럭씩 늦게 들어간다(skew).
- **weight**: double register를 쓴다. 다음 16x16 블록을 위에서부터 한 행(16 x int8 = 128비트)씩 shift해 16클럭에 채운다.
  채워지면 한 클럭에 전체가 계산용 레지스터로 넘어간다.
- **출력**: 열(oc)마다 결과가 나오는 시각이 다르다. 열마다 다른 acc bank로 들어가게 한다.

### 8.2 결론 요약

- **ISA와 컴파일러(NEST-C)는 바꿀 필요가 없다.** GEMM 명령의 의미("uop 하나 = `acc[dst] += inp[src](1x16) · wgt[wgt](16x16)ᵀ`",
  `vta-hw-zcu102-*/hardware/xilinx/src/vta.cc` `gemm()` 236행)를 HW가 그대로 지키면 된다.
  - 바뀌지 않는 것: `hw_spec.h`, 명령·uop 포맷, `VTASave.cpp`/`VTA.cpp`(NEST-C는 `convolution_wo_tr(...)` 호출과 tile 인자만 만든다),
    vtalib 시뮬레이터(golden 기준으로 그대로 씀).
  - tile 스케줄(`include/Backends/VTA/VTASchedules.h`)은 성능 때문에 다시 맞출 수 있다(필수 아님).
- **런타임은 uop 순서를 반드시 바꿔야 한다**(성능). vtalib `lib/Bundle/VTABundle.cpp`의 `gemmop_mxn`(132행), `gemmop_1x1`(156행) 두 함수다.
- **HW가 큰 일이다**: deskew와 acc bank, acc hazard, 명령 경계의 파이프라인 비우기, HLS가 아닌 RTL.

### 8.3 지금 HW가 GEMM을 하는 방식

- `vta.cc` `gemm()`: `iter_out` x `iter_in` x uop(`uop_bgn..uop_end`) 루프에 `#pragma HLS PIPELINE II = 1`.
  - 사이클마다 `wgt_mem`에서 16x16 블록 전체(2048비트), `inp_mem`에서 1x16, `acc_mem`에서 1x16 int32를 읽는다.
  - 256개 곱과 열마다 16입력 합을 계산해 acc와 out에 쓴다.
  - 주소는 uop의 dst/src/wgt + 루프 factor(`dst/src/wgt_factor_out/in`)다.
- 그래서 사이클마다 weight 블록이 바뀌어도 속도가 같다. 지금 ResConv10의 GEMM 가동률은 93.5%다(§6.2).
- zcu102 SRAM bank(`pkg_config.py`): 모든 bank가 128비트다. wgt 원소 하나 = 16 bank x 128비트, acc = 4 x 128비트.

### 8.4 런타임: uop 순서 (vtalib `gemmop_mxn`, `gemmop_1x1`)

**지금 순서** (`gemmop_mxn`, 빠르게 도는 것이 아래):

```
it_out (VTAUopLoopBegin(tile_w, dst 1, src stride_w, wgt 0)) : 출력 x
  it_in (VTAUopLoopBegin(KW, dst 0, src 1, wgt 1))           : 커널 kx
    uop: for ky(KH) for kernelIdx(n_kernel_) for y(tile_h)    : 출력 y가 가장 안쪽
         dst = out_base + (kernelIdx·tile_h + y)·tile_w
         src = in_base + (y·stride + ky)·in_tile_w
         wgt = wgt_base + ky·KW + kernelIdx·KH·KW
```

- 같은 weight가 연속으로 쓰이는 사이클 수 = `tile_h`다. ResConv10은 7, Conv4는 8이다.
  - weight 블록 하나는 conv 전체에서 49번(7x7 출력) 쓰이지만 7사이클씩 끊겨 7번 나뉜다.
- weight-stationary에서는 weight를 채우는 데 16클럭이 든다. 그래서 **같은 weight로 16사이클 이상 계산해야 다음 weight 채우기가 숨겨진다.**
  지금 순서 그대로면 7사이클 계산 후 9사이클을 기다리므로 가동률이 약 7/16 = 44%다.
- `gemmop_1x1`도 같다: `it_out` = x, `it_in` = n_kernel(wgt +1, dst +tile_w·tile_h), uop = y. 연속 = `tile_h`.

**바꿀 순서** (weight 고정, 출력 픽셀 전체가 안쪽):

```
it_out (VTAUopLoopBegin(n_kernel_, dst tile_h·tile_w, src 0, wgt KH·KW)) : 출력블록(kernelIdx)
  it_in (VTAUopLoopBegin(KW, dst 0, src 1, wgt 1))                       : 커널 kx
    uop: for ky(KH) for y(tile_h) for x(tile_w)                           : 픽셀이 가장 안쪽
         dst = out_base + y·tile_w + x
         src = in_base + (y·stride + ky)·in_tile_w + x·stride
         wgt = wgt_base + ky·KW
```

- 같은 weight가 `tile_h·tile_w` 사이클 연속으로 쓰인다. ResConv10은 49, Conv4(8x56)는 448이다.
- uop 수는 KH·tile_h·tile_w다. ResConv10은 147, Conv4는 1,344로 uop 버퍼(8192)에 충분히 들어간다.
  - 다만 uop을 DRAM에서 읽어 오는 양이 늘어난다. `VTAPushGEMMOp`의 uop 캐시(`vGemmUOpHandle`)로 두 번째 호출부터 다시 만들지는 않지만,
    uop SRAM으로 load되는 양이 tile마다 늘어나는지는 확인해야 한다.
- 같은 `dst`가 다시 오는 간격은 `tile_h·tile_w` 사이클(≥ 49, 최소 tile 7x7)이다. 이것이 §8.5 hazard 조건과 맞물린다.
- **정확성 검증은 지금 비트스트림에서 먼저 할 수 있다.** 정수 누산의 순서만 바뀌므로 출력이 비트 단위로 같아야 한다.
  - 시뮬레이터 빌드 → 지금 보드(single, multi4)에서 `run_all.sh` → ResConv10/Conv4 시간이 거의 같은지 본다.
  - 지금 HW는 순서와 상관없이 사이클당 1 GEMM이므로 시간이 거의 같아야 한다.
- 같은 함수를 쓰는 곳: `conv_kernel`(548/550행)과 `conv_kernel_scale`(933/935행)이 `gemmop_mxn`/`gemmop_1x1`을 쓴다.
  - `reset_acc`(89행, GEMM `reset_reg`)는 weight를 쓰지 않으므로 영향이 없다. HW에서 reset 경로는 따로 두면 된다.
  - XP-GEMM/BNN 경로는 이 비트스트림에서 이미 쓸 수 없다(§3.2 #6).

### 8.5 HW 세부 설계 (2026-10-27 개정, §8.8 Phase A·§8.9 조사 반영)

처음 안(열별 acc bank, BCIN shift로 16클럭 채우기)을 조사 뒤 아래처럼 바꿨다. 바뀐 이유는 항목마다 적었다.

**배치** — PE(i, j): i = ic(행), j = oc(열). 열 j = DSP48E2 16개를 PCIN/PCOUT로 cascade한 한 DSP 열.
- 부분합 Σ_i x[i]·w[j][i]는 열 안에서 P cascade로 흐른다. 지금 HLS의 16입력 덧셈 트리(400 MHz에서 최악, §8.8)가 없어진다.
- ZU9EG의 DSP cascade는 한 DSP 열 안에서 세로로만 된다(ACIN/BCIN/PCIN 모두 세로). y219 fence 안에 DSP48E2_X0..X17 x Y0..Y85가 있어 16개 cascade 열 16개가 들어간다.
- 곱셈 int8 x int8, P는 48비트. 16항 합은 int21이면 충분하다. INT8 2-MAC packing(DSP 반)은 나중 선택.

**입력 x (A 포트)** — fanout 1 경로로 바꾸는 것이 핵심(§8.6, §8.8).
- inp_mem 한 행(128비트) → 행 i는 i클럭 지연(skew FF) → 행 i 안에서 열 0→15로 FF 한 칸씩(가로, fabric FF, fanout 1).
  열 j의 행 i는 픽셀을 t+i+j에 받는다. A cascade는 세로라 가로 전달에 쓸 수 없다.
- FF: skew 8x(0+…+15)=960 + 가로 16x16x8=2048. AREG=1이면 가로 한 칸을 DSP A 레지스터가 대신한다.

**weight (B 포트)** — UG579로 확인(§8.9 #2): BREG=2, BCASCREG=1이면 BCOUT은 B1에서 나가고 곱셈기는 B2를 쓴다(INMODE[4]=0). CEB1/CEB2는 따로다.
- B1 = prefetch chain(BCIN으로 위 행에서 한 칸씩), B2 = 계산 중 고정. 2409.03508의 "DSP-Fetch"가 정확히 이 구조다(ZU3EG 14x14, 666 MHz).
- **CE는 wavefront로 늦춘다.** PE(i, j)는 픽셀을 t+i+j에 받으므로 weight 교체(CEB2)도 PE(i, j)에서 T+i+j에 일어나야 한다.
  - CEB1을 한꺼번에 켜면 아래 행이 B2로 넘기기 전에 B1이 덮인다. 그러면 다음 채우기를 T+15 이후에야 시작할 수 있어 주기가 31클럭이 된다.
  - CEB1을 **PE(i, j)에서 T+i+j부터 T+15+j까지** 켠다. 시작은 wavefront로 늦추고, 끝은 열 안의 모든 행이 같다.
    PISO는 T+j부터 깊은 행의 값부터 넣는다. 그러면 **weight 주기 16클럭**이 된다.
    - 시작만 늦추고 끝도 늦추면 모든 행이 같은 값을 받는다.
    - 확인: 한 열의 비트 정확 모델로 주기 16, 17, 20은 오류 0/128, 15는 7/128이었다(2026-10-27).
  - CE 신호는 대각선 shift register(행 +1, 열 +1)로 만든다. Gemmini가 propagate 비트를 데이터와 같이 skew하는 것과 같다.
- chain 입구: 열 j 맨 위에 128비트 PISO(병렬 로드, 8비트씩 16클럭 shift). wgt_mem bank j(128비트) = w[j][0..15]다.
  2048비트 wgt 읽기는 weight마다 한 번(≥16클럭에 한 번)이다. BRAM→PISO는 fanout 1이라 레지스터를 더 넣어도 된다.
- 대안(B 포트 병렬 로드, chain 없음)도 주기 16이 되지만 BRAM→DSP 256개 거리 경로(지금 wgt→DSP 0.06~0.16 ns)가 남는다. chain을 기본으로 한다.
- weight 주소 비교: RTL이 uop의 wgt 주소를 지금 채운 것과 비교한다. 바뀌었는데 16클럭이 안 지났으면 uop 스트림을 멈춘다(AXIS backpressure).

**출력 deskew와 acc** — 열별 acc bank 안을 버리고 Gemmini 방식(출력 쪽 RMW)으로 바꿨다.
- 열 j의 합은 열 위쪽에서 t+15+j+(DSP 지연)에 나온다. 열 j를 15−j클럭 늦추는 deskew FF(약 120x24비트)로 행을 맞춘다.
- 맞춰진 16 x int32 행 + dst 태그(같은 지연의 태그 pipe) → **RMW 단계**: acc[dst] 읽기 → 16 덧셈 → acc[dst], out_mem[dst](하위 8비트) 쓰기.
- 이유:
  1. acc를 512비트 한 행 그대로 둘 수 있다. ALU·STORE·bias load의 접근 방식이 지금과 같다.
  2. hazard 창이 systolic 깊이(~50)가 아니라 RMW 깊이(BRAM 2 + 덧셈 1 + 쓰기, 약 4클럭)다.
- hazard: RMW 안 in-flight dst(≤4개)와 비교해 forwarding한다. 어레이는 멈출 수 없으므로 Gemmini처럼 stall하지 않는다.
  §8.4 순서면 같은 dst 간격이 ≥49라 forwarding이 쓰일 일은 없지만 정확성을 순서에 맡기지 않는다.
- `reset_reg` GEMM: 태그에 reset 비트를 싣고 같은 pipe로 보낸다. RMW가 0을 쓴다. 순서는 자동으로 지켜진다.

**HLS와 RTL의 경계** (§8.9 #1)
- 지금 `acc_mem`은 HLS compute 안의 4096 x 512 true dual-port RAM(BRAM36 114개, 두 포트 모두 HLS가 씀)이다. 이것을 compute 밖으로 꺼낸다.
  - BD에 512비트 2포트 RAM. 포트 0, 1 모두 `gemm_busy`로 RTL(RMW 읽기/쓰기)과 HLS(ALU, bias load, 지금 그대로) 사이를 mux한다.
  - GEMM과 ALU는 compute 안에서 명령 단위로 순서대로 돌므로 동시 접근은 없다. HLS는 GEMM 명령을 내고 done을 받을 때까지 acc를 건드리지 않는다.
  - HLS 쪽은 `INTERFACE bram port = acc_mem` + `RESOURCE core = RAM_T2P latency = 2`. 모양은 inp_mem과 같다. 확인할 것: 2포트 외부 bram 인터페이스와 `DEPENDENCE` pragma가 HLS 2020.1에서 같이 되는지.
- out_mem 포트 A: RTL(GEMM 결과)과 HLS(ALU 결과)를 같은 `gemm_busy`로 mux한다.
- inp_mem·wgt_mem 포트 B: RTL만 쓴다(ALU는 inp/wgt를 읽지 않음).
- HLS → RTL: 지금 GEMM 루프(II=1)가 계산 대신 uop 스트림 {dst, src, wgt, reset, last}를 AXIS로 내보낸다. RTL → HLS: done 1개.
- 명령 경계: RMW가 순서대로라 연속한 GEMM 명령 사이에는 drain이 필요 없다.
  - 처음 구현은 단순하게 GEMM 명령마다 done을 기다린다(약 50클럭 x 64명령 = 0.6%, §8.5 이전 추정과 같음).
  - 그다음 ALU/STORE 토큰 앞에서만 기다리게 줄인다.

**HLS vs RTL**: 어레이, skew/deskew, CE wavefront, PISO, RMW, mux는 RTL. 큐, uop decode, ALU, 토큰은 HLS 그대로.
**검증**: §8.7 3단계 그대로(C 사이클 모델 → RTL 단위 TB → cosim → `gemmprobe`/`rawtest*` → `run_all.sh`).

### 8.6 얻는 것과 잃는 것

- **MAC 수는 그대로다.** 사이클당 256 MAC(16x16)은 지금과 같다. ResConv10은 이미 GEMM 가동률 93.5%다.
  그러니 이득은 거의 **클럭(타이밍)**에서만 나온다.
  - 입력 broadcast(1x16을 16열 DSP 모두로)와 16입력 덧셈 트리가 이웃 PE 연결로 바뀐다. 배선이 짧아진다.
  - 어레이를 32x32로 키울 때(MAC 4배) 확장이 훨씬 쉽다. 그때는 ISA의 BLOCK 크기, `hw_spec.h`, 레이아웃, 컴파일러까지 바뀐다(별도 작업).
- **타이밍 근거 (확인한 것)**: multi4 rs_rst(333 MHz, `vta-hw-zcu102-multi4/build/y219/rs_rst.timing.rpt`)의 worst setup 경로다.
  - `inp_mem_3_0` BRAM → `compute_3/inst/add_ln700_42_reg` DSP 입력
  - slack +0.008 ns, 데이터 경로 2.500 ns(배선 65%), logic level 0
  - 즉 **입력 SRAM에서 GEMM DSP로 가는 broadcast 경로가 critical**이다. systolic의 skew shift register가 정확히 이 경로를 끊는다.
  - 이 리포트는 그룹마다 최악 경로 하나만 담고 있다. 할 일: `report_timing -max_paths 100 -group clk_out1_vta_pll_clk_0`로
    상위 경로 중 GEMM(compute 안 DSP/덧셈 트리) 비율을 본다. single(`vta-hw-zcu102-single/build/.../export/*.timing.rpt`)도 본다.
- **잃는 것**: weight 재사용이 적은 연산은 느려진다.
  - weight 블록마다 픽셀이 16개보다 적은 연산(GEMV, 출력이 아주 작은 1x1 conv)은 16클럭을 채우고 몇 사이클만 계산한다. 최악이면 1/16이다.
  - 지금 번들에서 FC는 VTA를 쓰지 않는다. `fullyconnected()`는 CPU 함수다(`CPUBundle_aarch64.cpp` 4098행).
    vtalib에서 GEMM을 내는 곳은 `conv_kernel`/`conv_kernel_scale`뿐이다(`VTAPushGEMMOp` 6곳).
    conv는 최소 tile 7x7(`MIN_H_SIZE/MIN_W_SIZE`, 작으면 더미로 채움)이므로 weight당 ≥ 49사이클이 보장된다.
  - 나중에 FC를 VTA로 보낼 때는 broadcast 경로를 남기거나 batch를 모아야 한다.

### 8.7 다음 세션의 시작 순서

1. **타이밍 근거 확인 (HW 작업 전에)** — 2026-10-27 완료, Phase A 클럭 sweep과 360 MHz 보드 검증까지 §8.8
   - rs_rst DCP(`build/y219/`)를 열어 `report_timing -max_paths 100`을 본다.
   - 333 MHz에서 GEMM 경로가 상위를 차지하는지, systolic으로 몇 MHz를 기대할 수 있는지 판단한다.
   - GEMM이 병목이 아니면 클럭 이득이 작다. 그때는 이 작업의 가치를 다시 판단한다.
2. **런타임 uop 순서 변경**
   - `gemmop_mxn`/`gemmop_1x1`을 §8.4 순서로 바꾼다. `#### JO` 표시를 달고 원래 코드는 주석으로 남긴다.
     원본 `vta/vtalib`와 복사본 3개를 같이 맞춘다(§3.4 #1의 비교 규칙).
   - 지금 비트스트림에서 golden과 시간을 확인한다: `run_all.sh`(single, multi2, multi4)와 ResConv10/Conv4 `_prof` VTA_SYNC.
   - 이 단계만으로도 HW 교체 전에 런타임 쪽 검증이 끝난다.
3. **HW GEMM 교체**
   - HLS C 모델(`vta.cc` `gemm()`과 같은 결과를 내는 사이클 모델)을 먼저 만든다.
   - 그다음 RTL과 cosim, single 비트스트림(fence `y219`)에서 시작해 multi2, multi4(재배선 흐름) 순서로 간다.
4. 성능 비교
   - 같은 측정(§6.1, `OMP_NUM_THREADS=1`, conv 반복, VTA_SYNC)으로 클럭과 GOPS를 비교한다.

### 8.8 Phase A: 타이밍 근거와 파이프라인만으로 갈 수 있는 클럭 (2026-10-27)

작업 위치: 브랜치 `systolic`, `vta-hw-zcu102-systolic`(single 복사본). 스크립트와 결과: `vta-hw-zcu102-systolic/hardware/xilinx/scripts/timing/`(`top_paths.tcl`, `y219pips.tcl`), 결과 `build/timing/*.paths.csv`, 빌드 로그 `build/phaseA/`.

**1) 지금 333 MHz의 critical path** (`report_timing -max_paths 1000 -nworst 1`, routed DCP)
- single(10-23 y219 pass)과 multi4(rs_rst) 모두 상위 1000 경로가 slack 0.23 ns 안에 몰려 있다(slack wall).
- 상위 100개: single 100/100, multi4 96/100이 `inp_mem` BRAM → GEMM DSP A 입력이다.
  - logic 0, BRAM 출력 레지스터 없음, **fanout 346**, 데이터 경로 2.35 ns(배선 61%). §8.6의 가설이 맞다.
- 그 밖의 경로 종류별 최악 slack:

| 경로 | single | multi4 |
|---|---|---|
| inp_mem → DSP | 0.004 | 0.008 |
| load → inp/wgt BRAM 쓰기 | 0.150 | 0.017 |
| wgt_mem → DSP B | 0.161 | — |
| compute 덧셈 트리 (8 level) | 0.172 | 0.119 |
| reset fanout | — | 0.097 |

**2) 빌드 옵션** (커밋 `ab32e110`, 기본값이면 빌드는 이전과 같다)
- `make ... MEM_OREG=1`: inp/wgt BRAM 포트 B(compute 읽기)에 출력 레지스터를 켜고, HLS에 read latency 2를 알린다(`vta.cc` `#ifdef VTA_MEM_OREG`).
  - GEMM 루프는 II=1, 깊이 12다. 기준은 깊이 11이다(보드의 10-23 single IP `hls/<CONF>`와 오늘 base `_prof` IP 모두 wgt 16 bank x 128비트).
    acc read→write 거리는 약 3사이클 그대로라(ST_45 load, ST_47~48 store) 새 hazard는 없다.
    - 주의: single `hls/<CONF>_prof`(10-20)는 wgt 2 x 1024비트인 옛 구성이라 비교 기준이 아니다.
- `make ... FREQ=<MHz>`: PLL만 바꾼다. IP는 그대로 쓴다. 빌드 dir 이름은 `<CONF>_prof[_oreg][_<FREQ>mhz]`.

**3) 클럭 sweep** (single, y219 fence, `PROFILE=1`, 기본 implementation strategy)

| 변형 | 333 | 360 | 380 | 400 |
|---|---|---|---|---|
| base (지금 구조) | +0.004 (10-23판) | **+0.011** | **+0.007** | −0.106 (852 실패) |
| oreg | +0.026 | **+0.041** | −0.007 (12 실패) | −0.162 (653 실패) |

- **base도 360, 380 MHz를 통과한다.** slack wall은 placer가 목표에 맞춰 멈춘 모양이지 구조적 한계가 아니다.
- 이 범위에서는 oreg 효과보다 P&R 편차(±0.05 ns 정도)가 크다. 380에서 base +0.007, oreg −0.007이다. 출력 레지스터만으로는 이득이 없다.
- base 400 실패(상위 200): wgt_mem → DSP B 99, 덧셈 트리(DSP_OUTPUT → FDRE) 45, inp_mem → DSP A 25, compute 내부 FF 17, load → inp_mem 5.
  - **GEMM 어레이 경로가 169/200이고, 셋 다 §8.5 구조에서 없어진다**(wgt는 PISO + B1 chain, inp는 fanout 1, 합은 P cascade).
  - 그룹 최악: load → inp_mem −0.106(6 level), compute FF −0.102, wgt → DSP −0.098, axi_xbar → store −0.083, g2l_queue → compute −0.080, reset −0.068.
- oreg 380 실패: reset fanout(`proc_sys_reset` → fetch regslice) 73/200, inp broadcast 71/200, acc_mem 쓰기, load → inp_mem.
- oreg 400 실패의 종류(상위 200 중 개수, 그룹 최악 slack):

| 경로 | 개수 | 최악 slack | systolic으로 없어지나 |
|---|---|---|---|
| inp_mem → DSP (출력 레지스터를 켜도 배선 88%, 1.88 ns) | 80 | −0.130 | **예** (fanout 1) |
| compute 덧셈 트리 (DSP_OUTPUT → FDRE, 8 level) | 42 | −0.162 | **예** (P cascade) |
| compute → out_mem 쓰기 | 10 | −0.151 | 아니오 (RTL RMW로 옮기면 손볼 수 있음) |
| load → wgt/inp BRAM 쓰기 | 7 | −0.146 | 아니오 |
| fetch → axi_xbar FIFO, axi_xbar 내부 | — | −0.137 | 아니오 |
| reset fanout | 17 | — | 아니오 |

- 해석:
  - BRAM 출력 레지스터는 BRAM Tco만 줄인다. fanout 346 배선은 그대로라 inp broadcast가 400 MHz에서도 상위에 남는다. 구조(fanout)를 바꿔야 끊긴다.
  - systolic은 400 MHz 실패 경로 중 가장 많은 두 종류(상위 200 중 122개, 최악 −0.162)를 없앤다.
  - 400 MHz를 맞추려면 load 쓰기, out_mem 쓰기, fetch/AXI FIFO, reset 복제도 같이 고쳐야 한다(HLS pipeline/regslice, `proc_sys_reset` 복제).
- 기대 이득: 지금 구조로 360~380 MHz(333 대비 +8~14%, 360은 보드 확인). systolic과 주변 정리(load 쓰기, out_mem, 큐/AXI, reset)로 400 MHz(+20%). 그 위는 어레이만 2배 클럭(§8.9 #4).
**4) 360 MHz 보드 검증** (2026-10-27, `bit/evta_single_phaseA_261027/`, `/lib/firmware`에 같은 이름)
- 비트스트림: `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_prof_360mhz`(base, md5 `15768d7c…`), `..._prof_oreg_360mhz`(oreg, md5 `b7a69d41…`).
- 호스트(§2.2): 두 판 모두 stuckchk 194, 0 extra. Y219 행 PIP net 0(`scripts/timing/y219pips.tcl`).
- HW 프로브(`hwtest/*_nc`, 케이스마다 bitstream 재로드): 기준 `vtahw_single_y219.bin`과 두 360 판 모두 통과.
  gemmprobe reset/zero/ident/perm/ones 0/16 wrong, sramscan wgt/inp(+invert) 0 bytes wrong, donerace 16/16.
- 번들 golden(`hw/run_all.sh`, `BIN=<위>.bin`): 두 판 모두 30 PASS + golden 없는 5개 정상 실행(Resnet18/Partition/50 → 281, Vgg16 → 0, 이전과 같음).
- 속도(`hw/run_prof.sh`, `_prof` 번들 = `vta/vtalib` + `NESTC_EVTA_PROFILE`, total_vta_time, 2회 반복 편차 0.1% 이내):

| 번들 | 333 (`vtahw_single_y219`) | base 360 | oreg 360 |
|---|---|---|---|
| ResConv10 | 2.895 ms | 2.686 ms (−7.2%) | 2.683~2.686 ms (−7.3%) |
| Conv4 | 3.547 ms | 3.292~3.295 ms (−7.2%) | 3.293~3.294 ms (−7.2%) |

  - 클럭 비율 기대치는 −7.5%다. 시간이 클럭에 비례한다(DDR 병목 아님). oreg의 latency +1은 차이가 없다.
  - **360 MHz는 지금 구조(base)로 바로 쓸 수 있다.** single 기본 비트스트림으로 바꿀지는 multi2/multi4 360도 본 뒤 정한다.
- 보드 sudo: `xilinx`는 `/bin/sh`, `hwtest/*`, `hw/*`, `cp * /lib/firmware/*`만 비밀번호 없이 된다.
  환경 변수를 넘기려면 `sudo -n /bin/sh -c 'BIN=... ./run_all.sh'`.

### 8.9 조사: Gemmini, Tensil, UG579, AutoSA 등 (2026-10-27)

**#1 acc를 compute 밖으로 꺼내는 경계 — Gemmini**
- Gemmini의 accumulator는 별도 모듈이다(`AccumulatorMem.scala`).
  - 어레이 쓰기는 주소 비트로 accumulate/overwrite를 고른다. 읽기(mvout)에 scale/activation을 붙인다.
  - accumulate는 메모리 쪽 RMW다. 쓰기 요청이 `acc_latency` 깊이 pipe에 들어가고 같은 주소의 옛 값을 읽어 더한다.
- hazard는 **forwarding 없이 stall**한다.
  - 같은 주소가 pipe에 있으면 `io.write.ready`를 내린다. 같은 주소의 pending 쓰기가 있으면 읽기도 받지 않는다.
  - 파일 안에 작은 행렬의 read-after-write가 낡은 값을 읽을 수 있다는 TODO가 남아 있다.
- 어레이 출력(`MeshWithDelays.scala`)은 deskew shift register로 행을 맞춘 뒤 accumulator로 간다. 어레이는 acc 주소를 모르고, 호출자가 준 태그만 데이터와 같이 흘린다.
- 컨트롤러 사이의 hazard는 ROB가 막는다. 같은 컨트롤러 안의 hazard는 각 컨트롤러가 책임진다.
- 우리 설계에 반영(§8.5):
  - 출력 쪽 RMW, 태그 pipe, deskew FF로 바꿨다. 열별 acc bank 안은 버렸다.
  - 우리 어레이는 멈출 수 없으므로 stall 대신 forwarding으로 한다.
  - GEMM/ALU 순서는 compute 명령 순서가 지켜 준다(ROB 역할).

**#2 B1/B2와 BCIN으로 weight 채우기 — UG579 (v1.11)**
- BREG=2이면 BCASCREG는 1이나 2일 수 있다. BREG=1이면 B2를 쓴다. INMODE[4]=0이면 곱셈기는 B2(BREG에 따름)를 쓴다.
  CEB1/CEB2는 따로 있다(UG579 Table 3-2 핀, Table 3-3 속성, Figure 2-6 dual B register).
- BCASCREG=1이면 BCOUT은 B1 뒤에서 나온다. 그래서 B1 = prefetch chain, B2 = 계산 중 고정이 된다.
  arXiv 2409.03508 "DSP-Fetch"가 이 구조를 쓴다. 문장도 같다: "B1 registers serve as the operand prefetching path, while B2 registers remain static".
- 겹치기 조건(우리가 확인): CEB2는 PE(i, j)에서 T+i+j, CEB1은 [T+i+j, T+15+j]. 주기 16에서 비트 정확 모델 오류 0이었다(§8.5).

**#3 PE 사이 입력 레지스터(fanout 1)**
- §8.8 결과로 근거가 강해졌다. 출력 레지스터를 켜도 inp broadcast는 400 MHz에서 −0.130 ns(배선 88%)다. 문제는 fanout이지 BRAM Tco가 아니다.
- 2409.03508: TPUv1형 어레이에서 activation을 가로 CLB FF로 전달하고(DSP cascade는 세로뿐), 부분합은 세로 P cascade를 쓴다. 우리 배치와 같다.

**#4 ZU+에서 현실적인 클럭**
- arXiv 2409.03508 (XCZU3EG, Vivado OOC, speed grade 미기재):
  - INT8 14x14 TPUv1형: tinyTPU 400 MHz, Libano/CLB-Fetch/DSP-Fetch 666 MHz.
  - DPU B1024 재현 666 MHz.
  - 어레이만 단독(OOC) 측정한 값이다. 큐, AXI, HLS 제어가 붙은 전체 설계 값이 아니다.
- Xilinx DPU (PG338): 데이터/제어 325 MHz, DSP 계산 `dpu_2x_clk` 650 MHz(Matched Routing 2클럭).
  상용 설계도 fabric 제어는 ~325 MHz, DSP 어레이만 2배다.
- arXiv 2510.08137 (Alveo U50): 시스템 300 MHz, SA와 on-chip 메모리 600 MHz. activation은 DSP 입력 cascade로 세로, 부분합은 C 포트로 가로.
- Tensil: ZU+ 아키텍처 파일만 확인했다(ultra96v2 16x16, zcu104 32x32, FP16BP8). 공개 자료에서 클럭 수치는 찾지 못했다(hackster 글은 403).
- AutoSA (UCLA, FPGA'21): polyhedral 컴파일러로 HLS C systolic을 생성한다. I/O 모듈 daisy-chain(L1/L2/L3), double buffering, SIMD를 지원한다.
  - U250에서 MM int8 6.95 TOPs, int16 3.41 TOPs, fp32 934 GFLOPs.
  - 클럭은 논문 본문을 받지 못해 확인하지 못했다(par.nsf.gov 접속 실패).
  - HLS 생성물이라 우리 경우(HLS가 333 MHz에서 막힘)에 RTL보다 나을 근거는 없다.
- SPCL `gemm_hls`(HLS systolic, VCU1525): half/single/double 462/301/132 GFLOP/s. 클럭 목표가 낮은 HLS 설계라 참고만 한다.
- 결론:
  - fabric 전체(HLS 큐/AXI 포함)는 ~400 MHz가 현실적인 상한이다.
  - DSP 어레이만은 650~666 MHz까지 사례가 있다.
  - 큰 이득은 "어레이를 2배 클럭(666 MHz), 나머지는 333 MHz"(DPU 방식)에서 나온다. 같은 256 MAC으로 2배다.
  - §8.5 구조(fanout 1, 모두 레지스터 경계)는 그 방향과 맞는다. 2배 클럭은 어레이 입출력에 2:1 폭 변환만 더하면 되므로 다음 단계로 둔다.

출처: [Gemmini](https://github.com/ucb-bar/gemmini) (`PE.scala`, `AccumulatorMem.scala`, `MeshWithDelays.scala`),
UG579 v1.11 (UltraScale DSP48E2), [arXiv 2409.03508](https://arxiv.org/abs/2409.03508), [arXiv 2510.08137](https://arxiv.org/abs/2510.08137),
[PG338 Clocking](https://docs.amd.com/r/3.4-English/pg338-dpu/Clocking), [Tensil arch](https://github.com/tensil-ai/tensil/tree/main/arch),
[AutoSA](https://github.com/UCLA-VAST/AutoSA), [spcl/gemm_hls](https://github.com/spcl/gemm_hls).
