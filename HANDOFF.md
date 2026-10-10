# HANDOFF — vta-hw 리뷰 (EVTA/ZCU102 관점)

최종 갱신: 2026-10-10

## 목적

ZCU102 EVTA 비트스트림(`zcu102_1x16_i8w8a32_16_16_19_18.bit`)과 NEST-C `vta/vtalib`이
upstream VTA(`tvm/3rdparty/vta-hw`, HEAD `dfe9f57`)와 어떻게 갈라졌는지 이해하고 검증한다.

- 진행 방식: 채팅에서 파일 하나씩, 실제 코드를 보며 천천히, 매번 EVTA와 비교.
- **코드는 수정하지 않는다.** vta-hw는 NEST-C가 추적하지 않는 서브모듈이라 발견 사항만 기록한다.
- 리뷰 문서(Claude Docs, 발견 사항 표 포함):
  https://claude.ai/code/artifact/15ef05d9-70a6-4f8a-bc0c-e79955a51904

## 진행 상황

| 단계 | 내용 | 상태 |
|---|---|---|
| 1 | ISA 기준선: config(json/pkg_config/vta_config) → hw_spec_const.h → hw_spec.h | config 완료, 헤더 줄 단위 읽기 남음 |
| 2 | HLS: `hardware/xilinx/src/vta.cc`, `scripts/hls.tcl`, `scripts/vivado.tcl` | hls.tcl, vivado.tcl, pragma 개관 완료. `compute()` 상세 남음 |
| 3 | 드라이버/런타임: `driver.h`, `src/pynq`, `src/sim` | 미착수 (문서에 지도만 있음) |
| 4 | NEST-C vtalib fork와의 diff | 미착수 (차이 표 초안만 있음) |

## 이번 세션에서 확인한 것

### config 흐름
- `vta_config.py`가 `pkg_config.py`를 `exec`로 불러 `PkgConfig(cfg)`를 만든다. 대부분의 옵션은 stdout 출력만 한다.
- 파일을 만드는 옵션은 `--save-cfg-json`, `--export-tcl` 둘뿐이다.
- 헤더(`hw_spec.h`, `hw_spec_const.h`)는 생성되지 않는 고정 파일이다. `VTA_LOG_*`는 컴파일러 `-D` 인자로 들어온다.
  - TVM: `VTA.cmake:48-66` (`--defs` → `target_compile_definitions`)
  - HLS: `--export-tcl`의 `CFLAGS` → `hls.tcl:92 add_files -cflags`
  - Python 컴파일러: 같은 JSON을 `environment.py`가 직접 읽음
- `-D` 없이 `hw_spec.h`를 include하면 컴파일 에러가 난다. `gcc -E -dM`으로 `-D`가 내장 매크로와 같은 자리에 들어감을 확인했다.
- `vta_define(s).h`는 존재하지 않는다(`tests/hardware/common/test_lib.h:65` 주석에만 옛 이름이 남음).
- `HW_VER=0.0.1`은 `zcu104_sample.json` 값이며, `bitstream.py`의 다운로드 경로에만 쓰인다. ISA와 무관하다(이전 설명 정정).

### Xilinx Makefile (vivado 없이 실행 가능한 범위)
- `hardware/xilinx/Makefile`은 upstream 소스 파일이다. NEST-C CMake와 관계없다.
- `BUILD_DIR`, `VTA_CONFIG`(`--use-cfg` 끼워넣기), `VIVADO_HLS=echo`를 커맨드라인으로 덮어써서 저장소를 건드리지 않고 TCL 생성까지 실행했다.
- `hls.tcl`, `vivado.tcl`은 생성물이 아니라 소스 스크립트다. `verilog.tcl`은 없다. Verilog는 `vivado_hls`의 `csynth_design`이 만든다.
- EVTA 값을 넣은 JSON으로 `vta_config.py`를 돌리면 `--cfg-str`이 `zcu104_1x16_i8w8a32_16_16_19_18`이 된다.
  매크로는 EVTA CMake와 거의 일치하며, 다른 것은 COHERENT(true vs false), TARGET 이름, 멀티 디바이스 주소뿐이다.

재현용 `evta_like.json`:
```json
{ "TARGET": "zcu104", "HW_VER": "0.0.1",
  "LOG_INP_WIDTH": 3, "LOG_WGT_WIDTH": 3, "LOG_ACC_WIDTH": 5,
  "LOG_BATCH": 0, "LOG_BLOCK": 4, "LOG_UOP_BUFF_SIZE": 16,
  "LOG_INP_BUFF_SIZE": 16, "LOG_WGT_BUFF_SIZE": 19, "LOG_ACC_BUFF_SIZE": 18 }
```
```bash
make -C hardware/xilinx BUILD_DIR=<scratch>/xbuild_evta \
  "VTA_CONFIG=$PWD/config/vta_config.py --use-cfg=<scratch>/evta_like.json" \
  <scratch>/xbuild_evta/include/vta_config.tcl
```

### EVTA 하드웨어가 upstream 스크립트로 재현되지 않는 근거
- 명령어 256비트(`zcu102/vta/hw_spec.h:40 LOG_INS_WIDTH 8`) vs upstream 128비트. `vivado.tcl:208`의 명령 FIFO 폭 16B 하드코딩과 맞지 않는다.
- upstream 헤더에 EVTA 크기를 넣으면 `sizeof(VTAGemInsn)=24`(192비트)가 되어 128비트를 넘는다. EVTA가 256비트로 늘린 이유로 보인다.
- `memory_type`/`alu_opcode` 2비트는 `-D`가 아니라 헤더 상수(`VTA_MEMOP_ID_BIT_WIDTH`)다.
- VTA 4대(+0x10000), APM(0xA0050000), compute 카운터 레지스터(0x50/0x60/0x70)가 upstream `vivado.tcl`/`vta.cc`에 없다.
- `pkg_config.py`에 zcu102 타깃이 없다(모르는 TARGET은 PYNQ 값으로 떨어짐).

## 발견 사항 (문서 표가 원본)

| # | 위치 | 요약 | 상태 |
|---|---|---|---|
| 1 | pynq_driver `Run` | 이전 `done=1`을 읽는 경쟁 조건 | 후보 |
| 2 | pynq_driver `Run` | 타임아웃 후 복구 없음 | 후보 |
| 3 | vta.cc:338-347 | ALU `src_offset_*`가 `inp_idx_T` | 후보 |
| 4 | vta.cc:300-303 | `reset_reg` 때 `o_tensor`에 리셋 전 값 | 후보 |
| 5 | sim_driver.cc:489 | fsim만 MUL 구현 | 후보 |
| 6 | `VTAMapRegister` | MAP_FAILED 미검사 등 | 후보 |
| 7 | vtalib cmd_queue.h:443-560 | 명령어 필드 상한 검사 없음 | 확인됨 |
| 8 | vtalib cmd_queue.h:49-52 | MEM_ID_MOVE 뒤 break 없음 | 확인됨 |
| 9 | vtalib hw_spec.h:439-441,471 | ALU src가 INP 깊이로 선언 | 확인됨 |
| 10 | vtalib CMakeLists.txt:49 | `LOG_OUT_BUFF_SIZE=16` 하드코딩(MULTI/BNN에서 불일치) | 후보 |
| 11 | pkg_config.py:52-63 | `cfg_keys` 미사용, 키 검증 없음 | 후보 |
| 12 | pkg_config.py:199-202 | `coherent=True` 하드코딩 | 후보 |
| 13 | Makefile:55-56 | TCL이 JSON에 의존하지 않음 | 후보 |
| 14 | pkg_config.py:135-197 | zcu102 타깃 없음, PYNQ로 폴백 | 후보 |
| 15 | Makefile:41,58 | IP_PATH가 compute zip만 확인 | 후보 |
| 16 | vivado.tcl:446-449 | `vta.bit` vs Makefile의 `$(CONF).bit` | 후보 |
| 17 | vivado.tcl:446-450 | `.hwh` export 없음 | 후보 |
| 18 | vivado.tcl:206-208 | 명령 FIFO 16B 하드코딩 | 후보 |
| 19 | vivado.tcl:154 | `USE_LOCKED false` | 후보 |
| 20 | vivado.tcl:440 | strategy 주석과 코드 불일치 | 후보 |
| 21 | vta.cc:450 | `DEPENDENCE acc_mem inter false` hazard (fsim은 재현 못함) | 후보, 높음(잠재) |
| 22 | include/vta/hw_spec.h | 명령어 크기 static_assert 없음(EVTA 크기면 192비트) | 확인됨 |

## 남은 문제 / 열린 질문

- EVTA의 HLS 소스(`vta.cc`), `vivado.tcl`, `.hwh`를 ETRI에서 구할 수 있는가. 문서에 댓글로 질문해 둠.
  `.hwh`가 있으면 HP vs HPC 포트(coherent hang 원인), 칩(xczu9eg), FIFO 폭을 확정할 수 있다.
- docker의 Vivado 2020.1 확인 필요: `vivado_hls` 포함 여부, `zynquplus` 디바이스 설치 여부, xczu9eg 라이선스.
  (`vivado.tcl:19`가 정확히 2020.1을 요구한다. Ubuntu 20.04는 2020.1 공식 지원 OS가 아니라 docker가 낫다.)
- #21: NEST-C VTA 백엔드의 uop 생성이 같은 acc 주소 연속 쓰기를 피하는지 확인해야 한다.

## 세션 설정 관련

- `nest-compiler/.claude/settings.json`의 SessionStart/SessionEnd hook과 `commands/wrapup.md`는
  nest-compiler에서 시작한 세션에만 적용된다. 이 세션(vta-hw에서 시작)에는 적용되지 않는다.
- 이 세션의 산출물은 Claude Docs 문서, 스크래치패드(`/tmp/claude-1000/...`, 임시), 이 HANDOFF.md다.
- vta-hw에 맞는 hook/wrapup과 별도 저장소를 만들지 논의 중이다.

## 다음 할 일

1. `include/vta/hw_spec_const.h`를 줄 단위로 읽는다: `-D` 값과 고정 상수를 구분하고, #22에서 128비트를 넘긴 필드를 비트 단위로 확인한다.
2. `include/vta/hw_spec.h` 비트필드 → EVTA `zcu102/vta/hw_spec.h`와 대조.
3. `vta.cc` `compute()`(GEMM/ALU 루프, pragma 적용 지점) 상세 읽기.
4. 3단계(드라이버), 4단계(fork diff).
