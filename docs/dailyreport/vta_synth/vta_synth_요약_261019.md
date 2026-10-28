# 2026-10-19 EVTA synth 요약: Vivado 2020.1 docker 구축, 첫 자체 EVTA 비트스트림 333 MHz 통과(WNS +0.027), profile 비트스트림 single/multi2/multi4 모두 timing 통과

- 기간: 2026-10-19 07:00 ~ 10-20 07:00 (KST)
- 세션은 하나다. [PC]는 PC의 `main` branch에서 한 작업이다. 보드 실험은 이날 하지 않았다.

## 시간순

**12:47 [PC] "main을 git pull 해야겠어. … joungheo/nestc2_clion으로 바꿔서 해야 해." / 12:56 "진행하자."**
- `main`은 이미 `github/main`(joungheo/nestc2_clion)을 추적하고 있었다. "이미 update 된 상태"는 IDE가 `origin`(etri)을 골라서 나온 것으로 보인다.
- 중요: 실제 문제는 브랜치가 갈라진 것이었다. SessionEnd hook(`git add -A && commit && push`)이 보드에서 먼저 push한 `5087e117` 뒤에 `auto:` 커밋을 만들었고, push는 거부됐다.
- `main`을 `github/main`으로 reset했다(버린 커밋은 `backup/auto-2026-10-19`). `.claude/` 파일 2개는 `1ceaddfc`로 다시 커밋해 push했다.
- `vta/vtalib`을 `cd5590c`로 맞췄다. 이 커밋이 `github`(joungheo/nestc2_vtalib)에만 있어 거기서 fetch했다.

**13:03 [PC] "vtalib는 github/joungheo/nestc2_vtalib에서 pull 해서 똑같이 맞춰야지." / 13:06 "바꿔줘"**
- vtalib의 `main`도 joungheo 쪽을 추적하게 했다. 두 저장소의 remote 이름을 바꿨다.

  | 저장소 | `origin` | `upstream` |
  |---|---|---|
  | nest-compiler | joungheo/nestc2_clion | etri/nest-compiler |
  | vta/vtalib | joungheo/nestc2_vtalib | gitlab ones-ai/vta_lib |
- SessionStart hook을 remote 이름 없이 `git pull --rebase --autostash`로 고쳤다(`d32216c9`).

**14:48 [PC] "docker 안의 Vivado 2020.1로 evta_hw에서 make ip → make bin NUM_EVTA=1을 진행해 줘."**
- 환경을 조회하는 명령이 거부되어 실행하지 못했다. docker 이름, 마운트 경로 등을 물었다.

**14:57 [PC] "nest-compiler/vta-hw-zcu102-single 을 만들어서 tvm/3rdparty/vta-hw/를 복사해 오자." / 14:59 "claude 관련 md 파일 등은 복사할 필요 없지 않을까?"**
- `tvm/3rdparty/vta-hw`(`dfe9f57`)를 `vta-hw-zcu102-single/`로 복사했다. `.claude/`, `CLAUDE.local.md`, `.git`은 뺐다.

**15:02 [PC] "아티팩트 vta-hw review를 확인해 줘. … 손볼 곳을 정리해 줘."**
- 리뷰 문서(515줄)와 이전 세션의 `vtabundles_xcmpl/evta_hw/`를 대조해 손볼 곳을 A(ISA·설정)/B(Vivado·HLS 스크립트)/C(`vta.cc`, 1단계에서는 안 건드림)/D(연결·검증)로 정리했다.
- `evta_hw/`는 vta-hw를 고치지 않고 vtalib 헤더를 `-I`로 끼워 우회하는 방식이다. 그래서 `vta.cc` 자체는 고치지 않아도 합성된다.

**17:22 [PC] "nest-compiler/vtalib-zcu102-single/을 만들고 … 헤드를 통째 복사해서 검증하자. … evta_hw는 어디 있는 거야?"**
- `evta_hw`는 `vtabundles_xcmpl/evta_hw/`이고, 보드에서 push한 `5087e117`로 들어온 것이다.
- `vta/vtalib`(`cd5590c`)을 `vtalib-zcu102-single/`로 복사하고, 그 `include/zcu102/vta/hw_spec.h`를 `vta-hw-zcu102-single/include/vta/hw_spec.h`에 덮어썼다.
- 헤더 검증: 비트필드 프로브를 (가) vta-hw 복사본 헤더 + `pkg_config.py`의 `-D`, (나) vtalib 헤더 + vtalib CMake `-D`로 컴파일해 비교했다. **두 출력이 완전히 같다.** 명령어 3종 32 B, uop 8 B, 버퍼 깊이 uop 8192 / inp 4096 / wgt 2048 / acc 4096이다. 리뷰 1단계 표의 EVTA 열과도 모두 맞다. 다른 `-D`는 `VTA_COHERENT_ACCESSES`(true/false)와 `VTA_TARGET`(zcu104/zcu102) 둘뿐이었다.

**17:51 [PC] "vtalib-zcu102-single에서 lib만 있으면 되지? … vta-hw-zcu102-single에서도 xilinx 외에 … 다 지워죠." / 18:00 "나중에 보드에서 빌드할 때 쓰자."**
- vta-hw 복사본에서 xilinx가 아닌 하드웨어와 드라이버 소스를 지웠다(1.5 MB → 340 KB). 작업 대상은 `vta-hw-zcu102-single`로 정했다.
- `vtalib-zcu102-single`은 `lib`만 남기면 빌드가 안 되므로 보드 빌드용으로 그대로 두었다.
- 정리 중 `tests/`를 지웠다가, `CMakeLists.txt`가 `tests/simtests`·`tests/zcu102`를 조건 없이 불러서 원본에서 다시 복사해 되돌렸다.

**18:02 [PC] "zcu102 타킷을 추가해 줘." / 18:10 "… 왜 TVM 위치가 어긋났어?" / 18:12 "안 쓰니 그대로 두자. B1-B7을 고치자."**
- `config/pkg_config.py`에 `zcu102` 분기를 추가했다: `xczu9eg-ffvb1156-2-e`, 보드 rev 3.3, 333 MHz, AXI 128비트, 레지스터 `0xA0000000~0xA0003000`. coherent는 타깃별로 정하게 해서 zcu102만 `false`, cache bits `0011`이다. `zcu102_sample.json`과 `vta_config.json`은 16/16/19/18이다.
- 이제 `vta_config.py --defs`가 vtalib CMake의 `-D`와 `TARGET`·`COHERENT`까지 같다.
- `AXI_PROT_BITS`는 upstream zcu104와 같은 `010`으로 했다(`evta_hw`는 `000`). 지금 비트스트림의 값은 확인할 수 없었다.
- TVM 경로가 어긋난 이유: `get_tvm_path()`가 `config/../../..`를 TVM으로 보는데, 복사본은 깊이가 한 단계 얕다. 합성에 쓰이지 않아 그대로 두었다.
- B1~B7을 `hardware/xilinx`에 반영했다. 이 시점에는 `make -n`과 생성된 config tcl까지만 확인했다.
  - B1 명령 FIFO 폭을 `VTA_LOG_INS_WIDTH`에서 계산(32 B), B2 `S_AXI_HP0_FPD` non-coherent 128비트, B3 `.hwh`/`.xsa`/timing·utilization 보고서 export, B4 export 이름 인자화, B5 `clk_wiz locked` → `dcm_locked`와 `validate_bd_design`, B6 csim 기본 끔, B7 Makefile(CONF별 config, IP 4개 확인, `make bin`).
  - 보드의 etri 비트스트림을 덮어쓰지 않도록 결과물 이름을 `vtahw_` 접두어로 했다.

**18:40 [PC] "docker 이름이 pynq24-build 인데 …" / 19:25 "pynq27_docker도 있어." / 19:29 "… xilinx 버전들 사이즈를 알려줘."**
- 중요: **Vivado 2020.1은 컨테이너에도 호스트에도 없었다.** `pynq24-build`는 2018.3, `pynq27-build`는 2020.2를 호스트에서 마운트해서 쓴다. 호스트의 `vivado_hls`는 2018.3과 2017.4뿐이다.
- 디스크 여유는 114 GB였다(916 GB 중 756 GB 사용). 2018.3 설치 파일 2개(약 39 GB)를 먼저 지우기를 권했다.

**19:51 [PC] "2020.1 *.bin을 받았는데 sudo apt install …?" / 19:54 "ubuntu20.04.6에서는 … 설치가 안 돼. docker 안에서 해야 해." / 20:11 "Not Supported Linux OS …" / 20:12 "아무것도 안 뜨는데 다시 시작해 줘." / 20:21 "했어."**
- 2018.3 설치 파일 삭제 후 여유는 153 GB다. `.bin`은 웹 설치 프로그램이라 apt로는 설치할 수 없다.
- 새 컨테이너 `vivado2020-build`(ubuntu:18.04, `/tools/Xilinx`·`~/Downloads`·nest-compiler 마운트, 사용자 `ubuntu`)를 만들었다.
- 18.04.6 경고는 OK로 넘겨도 된다고 안내했다. GUI 창이 1×1로만 떠서 배치 설치로 바꿨다. 사용자가 AuthTokenGen으로 인증한 뒤 배치 설치를 시작했다.

**20:41 [PC] 설치 재시도 (별도 루트 `/tools/Xilinx/v2020.1`)**
- 중요: 첫 설치는 root 소유의 공용 `/tools/Xilinx/xic`를 갱신하지 못해 실패했다. 되돌리기 과정에서 `xic/tps/boost_1_64_0` 헤더 12,423개가 지워졌고, 설치 캐시 사본에서 복구해 차이가 없음을 확인했다. 다른 버전 디렉터리는 건드려지지 않았다.
- 재발 방지로 별도 루트 `v2020.1`에 설치했다. 설치 결과는 `/tools/Xilinx/v2020.1/Vivado/2020.1`(33 GB), Y2K22 패치 적용, ZCU102 board file 3.1~3.3이다. 다운로드 캐시 15 GB가 남아 있다.

**21:13 [PC] 첫 EVTA 비트스트림 완성 (single, profile 없음)**
- `make ip` → `make bin`이 27분 걸렸다(HLS 2.5분, Vivado 25분). fetch의 `raw_insn`과 명령 FIFO가 256비트로 잡혔다.
- `vtahw_zcu102_1x16_i8w8a32_16_16_19_18`의 타이밍과 자원이다.

  | 항목 | 결과 |
  |---|---|
  | 클럭 | 333.01 MHz |
  | WNS / WHS | +0.027 ns / +0.010 ns, 실패 경로 0 (여유 0.03 ns로 빠듯) |
  | LUT / FF | 9.4 % / 5.8 % |
  | BRAM / DSP | 253/912 (27.7 %) / 268 (10.6 %) |
- `.hwh`로 레지스터 맵이 vtalib과 같은 것을 확인했다. AXI 마스터 5개가 모두 `S_AXI_HP0_FPD`(128비트), cache `0011`, prot `010`이다.
- BRAM `MASTER_TYPE` CRITICAL WARNING 8건은 upstream에서도 나오는 메타데이터 차이로 판단했다.
- 이 비트스트림에는 compute 카운터(`0x50/0x60/0x70`)와 AXI Perf Monitor가 없어 프로파일 GOPS는 의미가 없다.

**10-20 00:24 [PC] "두 트리 커밋해주고, 문서 경로도 반영해줘. 프로파일링 켜고 합성을 하고 싶어." / 00:56 "push까지 하자."**
- 두 트리를 `44b33307`로 커밋해 push했다. `*.json`이 `.gitignore`에 걸려 `vta_config.json`과 `zcu102_sample.json`은 `git add -f`로 넣었다.
- 프로파일 기능: compute 카운터 3개(0x50 load 원소 수, 0x60 GEMM 반복, 0x70 ALU 반복, FINISH마다 리셋). `PROFILE=1`이면 `0xA0050000`에 APM(슬롯 3개)을 넣고 `_prof` 디렉터리에 따로 만든다.
- 중요: `vtalib-zcu102-single` 드라이버는 APM reset/enable 쓰기가 주석 처리돼 있어 모든 값이 0으로 읽혔을 것이다. 실행 전에 reset → enable을 쓰게 고치고, CMake 옵션 `NESTC_EVTA_PROFILE_AXI_MONITOR`를 추가했다.
- `vtabundles_xcmpl/CMakeLists.txt`에 `-DVTALIB=...`을 추가했다. `CLAUDE.md`, `evta_hw/README.md`, `WORKLOG.md`에 새 경로를 반영했다.

**10-20 00:46 [PC] "vta-hw-zcu102-multi2, vtalib-zcu102-multi2 만들어서 … 합성해줘. … multi4에 대해서도 똑같이 … axi bus width가 128이야? … 1024bit 까지 늘릴 수 있잖아."**
- multi2/multi4 트리를 만들었다. EVTA i의 레지스터는 `0xA0000000 + i×0x10000`, 메모리는 `S_AXI_HP<i>_FPD`, APM은 `0xA0050000 + i×0x10000`이다. 버퍼는 vtalib `NESTC_EVTA_MULTI`와 `BUNDLE_SET=multievtax2`에 맞춰 16/15/18/17이다.
- AXI 폭: ZynqMP의 PL→DDR 포트는 모두 **최대 128비트**라 1024비트로 넓혀도 SmartConnect가 다시 쪼갠다(포트당 128 b × 333 MHz ≈ 5.3 GB/s). 효과가 있는 방법은 여러 HP 포트를 병렬로 쓰는 것이다. 먼저 `_prof` 비트스트림의 slot1로 load가 대역폭에 묶이는지 재기를 권했다.

**10-20 01:02~02:41 [PC] profile 비트스트림 3종 합성**
- single `_prof`는 두 번 다시 빌드한 끝에 통과했다. 첫 판은 레지스터 위치가 틀렸고, 둘째 판은 timing이 모자랐다.

  | 판 | 결과 | 원인 → 수정 |
  |---|---|---|
  | 1차 | WNS +0.031 통과, 카운터가 0x58/0x68/0x78에 나옴 | 카운터를 `volatile` 참조로 선언해 HLS가 입출력 쌍으로 만듦 → `volatile` 제거, 출력 전용 포트. 비트스트림은 버림 |
  | 2차 | WNS −0.039 ns, 경로 3개 | PS 레지스터 포트 `M_AXI_HPM0` 쪽 응답 경로가 333 MHz에서 모자람 → PS 레지스터 포트와 제어 interconnect 입력만 `pl_clk0` 100 MHz로, 리셋 블록 `proc_sys_reset_ps` 추가 |
  | 3차 | WNS +0.054 ns 통과 (`0acdc20a`) | — |
- 같은 버그를 안고 시작했던 multi2/multi4 빌드는 중단하고 두 수정을 넣어 다시 돌렸다(그때 온 실패 알림 2건은 중단 때문이다).
- 최종 결과(모두 333 MHz, `PROFILE=1`). 보드에서는 아직 시험하지 않았다.

  | 비트스트림 (`_prof`) | EVTA | 버퍼 | WNS | LUT | BRAM | DSP | 커밋 |
  |---|---|---|---|---|---|---|---|
  | `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_prof` | 1 | 16/16/19/18 | +0.054 ns | 10.8 % | 254 | 271 | `0acdc20a` |
  | `vtahw_zcu102_1x16_i8w8a32_16_15_18_17_2evta_prof` | 2 | 16/15/18/17 | +0.004 ns | 20.0 % | 278 | 542 | `1bd29b78` |
  | `vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta_prof` | 4 | 16/15/18/17 | +0.003 ns | 39.5 % | 556 | 1084 | `97d2146e` |
- `.hwh`로 주소 맵(EVTA `0xA0000000~0xA0030000`, APM `0xA0050000~0xA0080000`, EVTA마다 HP0~HP3)과 카운터 0x50/0x60/0x70을 확인했다.
- multi2/multi4는 여유가 0.003~0.004 ns뿐이다. 결과가 불안정하면 300 MHz로 낮춰 다시 합성한다(`pkg_config.py`의 `fpga_freq`/`fpga_per`).
- multi에서 결과가 0이면 HP1~3 폭(AFIFM `0xFD370000`/`0xFD380000`/`0xFD390000`)부터 본다. 이 값은 부트 이미지가 정한다.
- multi4는 4-EVTA 번들 세트가 없어(`BUNDLE_SET`은 `multievtax2`까지) rawtest와 vtalib 테스트로만 확인할 수 있다.

**10-20 04:44 [PC] "이 세션에서 작업한 내용을 보드에서 연 세션에 전달하기 가장 좋은 방법은 뭐야?" / 04:52 "/nest-compiler/log/BOARD_TODO.md 에 넣어서 push 해줘."**
- git에 인수인계 문서를 push하는 방법을 권했다. 보드 세션은 SessionStart hook으로 pull하고 `CLAUDE.md`를 읽는다.
- `log/BOARD_TODO.md`를 커밋해 push했다(`fc03b172`). 비트스트림 3종, 레지스터·APM 맵, 시험 순서(single rawtest → profile 번들 → load 대역폭 판단 → multi2 → multi4), 위험 요소, 빈 Results 절을 넣었다.
- `.bin`은 git에 없으므로 scp로 보드 `/home/xilinx/JO/npuprg/bit/`에 옮겨야 한다.

## 결정이 남은 것 (10-20 07:00 기준)
- 비트스트림 3종의 보드 시험(`log/BOARD_TODO.md`). 아직 하지 않았다. `.bin`을 scp로 옮기는 것이 먼저다.
- load가 대역폭에 묶이는지 APM으로 잰 뒤, single에서도 load/compute/store를 여러 HP 포트로 나눌지.
- multi2/multi4의 timing 여유(+0.004/+0.003)가 보드에서 문제가 되면 300 MHz로 다시 합성할지.
- multi4용 4-EVTA 번들 세트(와 CMA 128 MB에 맞춘 `VTA_MAX_XFER`)를 만들지.
- `AXI_PROT_BITS` `010`(새 판)과 `000`(`evta_hw`) 중 어느 것이 맞는지 미확인. DDR 접근이 실패하면 먼저 의심한다.
- 정리 거리: 다운로드 캐시 `/tools/Xilinx/v2020.1/Downloads`(15 GB) 삭제, 역할이 겹친 `vtabundles_xcmpl/evta_hw/` 정리, SessionEnd hook에 커밋 전 pull 넣기(권고만 함).
- 이미 결정된 것: remote 이름 변경(`origin`=joungheo), 작업 트리는 `vta-hw-zcu102-single`(스크립트도 그 안), `vtalib-zcu102-single`은 보드 빌드용으로 유지, TVM include 경로는 쓰지 않아 그대로 둠, 합성은 `vivado2020-build` 컨테이너의 `/tools/Xilinx/v2020.1`, profiling은 계속 켬.
