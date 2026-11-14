# 2026-11-13 EVTA systolic 요약: 두 번째 호스트 PC(ubuntu2)에 Vivado 2020.1 docker 설치, ALUX compute C/RTL cosim 20/20 PASS, `_alux` 400 MHz 빌드 timing 실패(WNS −0.288)

- 기간: 2026-11-13 07:00 ~ 11-14 07:00 (KST)
- 세션은 둘이고, 모두 새 호스트 PC `ubuntu2`의 `main`에서 했다. 보드 실험은 이 PC에서 ssh로 돌렸다.
  - [PC clone]: 새로 clone한 저장소의 submodule 받기와 Remote Control 연결 문의.
  - [PC]: Docker 안 Vivado 2020.1 설치, ALU 확장(ALUX) 비트스트림 빌드와 C/RTL cosim, 보드 접속 준비, 네 CNN 모델에 ALU 확장을 쓸 곳 정리.
- 이날 작업은 19:23에 시작했다. 앞 PC의 냉각 문제(`docs/host_pc_shutdown.md`) 때문에 미뤄 둔 ALUX 비트스트림 빌드(`docs/evta_isa_ext.md` §8 4단계)를 새 PC에서 이어 가는 것이 주된 흐름이다.

## 시간순

**19:23 [PC clone] "git clone 했는데 여기 연결되 dir 들을 다 update 해야하는데"**
- `git submodule update --init --recursive --jobs 4`로 submodule 4개(`glow`, `tvm`, `vta/vtalib`, `onnx_yaml-parser`)를 받았다. 그 안에 중첩된 submodule(glow의 folly, onnx, googletest 등, tvm의 dlpack, dmlc-core, vta-hw 등, vtalib의 3rdparty)도 함께 받았다.
- `git submodule status --recursive`로 확인했다. 초기화 안 된 것(`-`)이나 커밋이 어긋난 것(`+`)은 없고, 모두 main 저장소가 지정한 커밋에 있다.
- 알려 준 것:
  - 원격 최신으로 올리려면 `git submodule update --remote <path>`를 써야 하고, 그러면 main 저장소에 diff가 생긴다.
  - 앞으로는 `git pull --recurse-submodules`를 쓴다.
  - cmake가 `cmake/glow/`를 `glow/` 위에 복사하므로 `glow`가 dirty가 되지만, `ignore = dirty` 설정 때문에 정상이다.

**19:26 [PC clone] "같은 이름으로 claude 했는데 왜 브라우저에서 연결이 안 되지?"**
- 세션에 Remote Control이 켜져 있다. 다만 서버 쪽 상태를 볼 수 없어서 원인은 확정하지 못했다.
- 확인한 것: `ANTHROPIC_API_KEY`는 없다(API 키 로그인 문제는 아니다). 로그인 정보는 19:08에 갱신됐다. 세션 시작 때 claude.ai MCP가 `credential validation failed`로 실패했다.
- 가능성 있는 원인 세 가지를 안내했다.
  1. 이름이 같아도 새 세션이므로 목록을 새로고침해 최신 항목을 연다.
  2. 인증 문제일 수 있으니 `/login` 후 `/remote-control`을 다시 실행한다.
  3. 새 디렉터리의 trust 확인을 아직 안 했을 수 있다.

**19:33 [PC] "vivado 20.01을 설치해야 하는데 이 버전 ubuntu에서는 안 될거야. docker를 설치하고 내부에 ubuntu18.04를 설치하고 vivado를 깔자"**
- 새 PC(ubuntu2)에는 Docker도 `/tools/Xilinx`도 없었다. sudo에 비밀번호가 필요해서, 우선 파일만 `utils/docker_vivado2020/`에 만들었다.
  - `Dockerfile`: ubuntu:18.04에 Vivado 2020.1용 라이브러리를 넣는다. 사용자 `ubuntu`의 uid/gid는 호스트 jerry와 같은 1000이다. 그래서 문서의 `docker exec -u ubuntu vivado2020-build ...`를 그대로 쓸 수 있다.
  - `setup.sh`: 이미지를 빌드하고 컨테이너 `vivado2020-build`를 만든다. `/tools/Xilinx`와 `$HOME`을 같은 경로로 마운트하고, 노드락 라이선스가 호스트 MAC을 보도록 `--network host`로 띄운다.
  - `install_config.txt`: Zynq UltraScale+만 `/tools/Xilinx/v2020.1`에 설치하는 배치 설정이다.
  - `install_vivado.sh`: 압축 해제, `xsetup --batch`, Y2K22 HLS 패치, `vivado -version` / `vivado_hls -version` 확인을 한다.
- 사용자에게 부탁한 일: Docker 설치, 설치 파일(`Xilinx_Unified_2020.1_0602_1208.tar.gz`, 약 35 GB)과 `y2k22_patch-1.2.zip`(AMD AR 76960) 다운로드, 라이선스 파일 가져오기(xczu9eg는 WebPACK에 없다).
- `install_config.txt`의 모듈 이름은 기억으로 적어서 틀릴 수 있다고 미리 알렸다. 이 우려는 20:34에 실제로 맞았다.

**19:58 [PC] "이 PC display setting을 하려는데 100% 200% 사이값은 없네."**
- Ubuntu 20.04는 분수 배율이 기본으로 꺼져 있다. 이 PC는 X11 세션이고 그래픽 카드는 NVIDIA다.
- 방법 1: 설정 → 디스플레이의 Fractional Scaling을 켠다. 또는 `gsettings set org.gnome.mutter experimental-features "['x11-randr-fractional-scaling']"`를 실행한다. X11 + NVIDIA에서는 흐려지거나 부하가 늘 수 있다.
- 방법 2: 글자만 키운다(`text-scaling-factor 1.25`). 되돌릴 때는 `gsettings reset ...`를 쓴다.

**20:02 [PC] "b018f7b331ab0446137756156ff944d9 checksum 값이야 vivado 2020.01" / 20:16 "외부에서 이 ubuntu2에 접속할 때 어떻게 쳐야해?"**
- 체크섬을 `install_vivado.sh`에 넣었다. 압축이 풀리기 전에만 MD5를 확인하고, 값이 다르면 멈춘다. 이 값이 AMD 공식 값과 같은지는 확인할 방법이 없어 받은 값을 그대로 썼다.
- 외부 접속: 지금은 SSH 서버가 꺼져 있다(`ssh` inactive, 22번 포트 없음). 내부 IP는 `192.168.20.20`, 공인 IP는 공유기 주소 `121.138.215.185`다. `docker0`이 보여 Docker는 설치된 것으로 봤다.
  - 안내한 것: `openssh-server` 설치, 같은 네트워크에서는 `ssh jerry@192.168.20.20`, 외부에서는 공유기 포트포워딩(예: 2222 → 22)과 DHCP 고정, DDNS.
  - 보안: 공개키만 등록하고 `PasswordAuthentication no`를 쓴다. 대안으로 Tailscale도 있다.

**20:19 [PC] "다 down 받았어." / 20:22 "claude를 다시 켰어 이제 돼?"**
- 받은 파일 세 개를 확인했다.
  - 설치 파일(38 GB)의 MD5가 `b018f7b3...`과 일치한다.
  - `y2k22_patch-1.2.zip` 안에 `patch.py`가 있다.
  - 라이선스 `~/.Xilinx/xilinx.lic`는 `HOSTID=ANY`이고, System Edition과 HLS가 영구 라이선스로 들어 있다. 파일 이름이 소문자라서, 컨테이너가 파일이 아니라 `~/.Xilinx` 폴더를 보도록 `setup.sh`를 고쳤다.
- jerry가 `docker` 그룹에 없어서 막혔다. 사용자가 그룹을 추가하고 Claude를 다시 켠 뒤에는 `sg docker`로 이미지 `vivado2020:18.04` 빌드까지 끝냈다.
- 설치 파일 압축 해제(`~/xilinx_installer_2020.1/`)도 백그라운드로 끝냈다. 남은 것은 `/tools/Xilinx` 폴더였다.

**20:29 [PC] "실행했어. 네가 sudo 를 실행할 수 있게 하자." / 20:32 "했어 확인해봐"**
- `/tools/Xilinx`는 생겼지만 소유자가 root였다. 비밀번호 없는 sudo(`/etc/sudoers.d/jerry-nopasswd`, `visudo -c`로 문법 검사)를 안내했다. 외부에 SSH를 열 거면 키 인증만 쓰라고 다시 경고했다.
- 사용자가 설정한 뒤 진행한 것:
  - sudo가 비밀번호 없이 되는지 확인하고, `/tools/Xilinx` 소유자를 jerry로 바꿨다.
  - 컨테이너가 `sg docker` 때문에 gid 136(docker)으로 잡혀 있었다. `setup.sh`가 jerry의 기본 그룹(gid 1000)을 쓰도록 고쳤다.
  - 기억으로 적은 모듈 이름은 설치 파일이 거부했다. `xsetup -b ConfigGen`이 만든 실제 설정에서 Zynq UltraScale+ MPSoC만 남기도록 `install_config.txt`를 다시 썼다.

**20:39 [PC] Vivado 2020.1 설치 완료, HLS와 합성으로 동작 확인**
- 컨테이너 이름(`vivado2020-build`), 사용자(`ubuntu`), 경로(`/tools/Xilinx/v2020.1/`)가 예전 호스트와 같다. 그래서 문서 명령을 그대로 쓸 수 있다.
- 확인한 것:

  | 확인 | 결과 |
  |---|---|
  | `vivado -version` / `vivado_hls -version` | Vivado v2020.1 / Vivado HLS v2020.1 |
  | 작은 HLS 함수를 xczu9eg용으로 합성하고 IP export | 성공. Y2K22 패치가 없으면 이 단계가 실패하므로, 패치가 적용된 것이다 |
  | 작은 Verilog를 xczu9eg로 `synth_design` | 성공. 로그에 `Got license for feature 'Synthesis' and/or device 'xczu9eg'` |
  | 설치 크기 | 34 GB (Zynq UltraScale+만) |

- 진행 중 고친 것:
  - Vivado 2020.1에는 Y2K22 패치를 돌릴 python 3가 없다. 그래서 컨테이너의 `python3`로 돌리도록 `install_vivado.sh`를 고쳤다.
  - 컨테이너 시간대를 UTC에서 `TZ=Asia/Seoul`로 바꾸고 컨테이너를 다시 만들었다. 설치물은 `/tools/Xilinx`에 있어서 영향이 없다.
  - 컨테이너는 `--restart unless-stopped`라서 재부팅 뒤에도 다시 뜬다.
- 정리할 것: `~/xilinx_installer_2020.1/`(약 38 GB)는 지워도 된다. 다운로드한 `tar.gz`는 재설치용으로 남겨 둔다.

**23:18 [PC] "utils/docker_vivado2020/ ... 와 CLAUDE.md 수정해서 커밋하고 푸쉬해줘. 이전 npu_systolic 세션에서 alu extend 를 하고 있었어. ... 계속 이어 나가보자."**
- 커밋 `9796737`: `utils/docker_vivado2020/` 파일 4개와 `CLAUDE.md` Vivado 절의 두 번째 호스트 `ubuntu2` 설명이다.
  - 이 PC에 git 사용자 설정이 없어서, 이 저장소에 한해 이전 커밋과 같은 이름(joungheo)으로 설정했다.
  - GitHub 인증이 없어 push는 못 했다. SSH 키(`jerry@ubuntu2`)를 만들고, 공개키 등록을 부탁했다.
- ALU 확장의 지금 위치(`docs/evta_isa_ext.md` §8):
  - 끝난 단계: 1단계 정수 LayerNorm / softmax 식, 2단계 ISA 헤더와 기준 모델, 3단계 HLS ALU와 CSim, 6·7단계의 소프트웨어 부분(DeiT의 GELU 출력 표, LayerNorm, softmax를 시뮬레이터에서 CPU 경로와 바이트 단위로 맞춤).
  - 남은 것: 4단계 `_alux` 비트스트림. 앞 PC의 냉각 문제로 미뤄 둔 일이라 바로 시작했다.
- 시작한 빌드(컨테이너 안, 백그라운드): 지금 single `_rq` 비트스트림을 만든 설정(11-04 daily report에서 찾음)에 `ALUX=1`만 더했다.
  ```
  make ip SYSTOLIC=1 ALUX=1
  make bin SYSTOLIC=1 ALUX=1 FREQ=400 JOBS=8 IMPL_STRATEGY=Performance_ExplorePostRoutePhysOpt BUILD_TAG=rq
  ```
  - 결과 이름은 `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_alux_400mhz_rq.bin`이다. `_sa_`, `_rq`, `_alux`가 모두 들어 있어서 vtalib이 requantize와 ALU 확장을 함께 켠다.
  - 로그는 `vta-hw-zcu102-systolic/build/logs/alux_400_rq.log`이다.
- 빌드 뒤 보드에서 할 일(§8.8 4단계 계획):
  1. 기존 golden과 layer dump가 `_rq`와 바이트 단위로 같은지 본다.
  2. 확장 ALU 명령 probe(무작위 명령열, LayerNorm / softmax 명령열)를 돌린다.
  3. DeiT를 다시 빌드하지 않고 `EVTA_LN_CHECK`, `EVTA_SM_CHECK`, `EVTA_DUMP_CONV`로 비교하고 시간을 잰다.

**23:31 [PC] "공개키를 github에 add ssh 했어. 이게 등록 맞아? alux 비트스트림 만드는 건 이미 저번 PC에서 vta.cc 등 소스를 alu 추가하게 고쳐놨었어? cpu에서 돌게 만들어놓은 simulation과 같은 지까지 확인했었어?"**
- `ssh -T git@github.com`에 `Hi joungheo! You've successfully authenticated`가 나왔다. remote를 `git@github.com:joungheo/nestc2_clion.git`으로 바꾸고 push했다(`8371a64..9796737 main -> main`).
- 소스는 이전 PC에서 이미 고쳐져 있었다. 지금 빌드는 그 소스를 그대로 합성한다.
  - `vta-hw-zcu102-systolic/hardware/xilinx/src/vta.cc`에 `#ifdef VTA_ALUX` 구간이 19곳 있다. 연산 4–11(MUL, SUB, CPY, LUT, LUTSET, RSUM, RMAX, CLZ), ALU `lut_en`, LUT 표가 들어 있다(커밋 `e6df7fa7`, 11-11).
  - Makefile의 `ALUX=1`이 `-DVTA_ALUX`를 켜고 이름에 `_alux`를 붙인다. RTL(`gemm_sa`, `sa_rmw.v`)은 바꾸지 않았다.
- 비트스트림 없이 PC에서 확인된 것(`evta_isa_ext.md` §8.8–8.10)을 정리했다:

  | 비교 | 결과 |
  |---|---|
  | HLS `vta.cc` CSim ↔ 기준 모델 `alu_ext_ref.h` | 무작위 명령열 200개 모두 바이트 단위로 같음. 버그를 일부러 넣은 사본 5개는 모두 잡힘 |
  | HLS CSim으로 돌린 LayerNorm / softmax 명령열 ↔ bundle의 CPU 함수 | 같음 |
  | vtalib 시뮬레이터(같은 기준 모델로 ALU 실행) ↔ CPU 경로 | conv 출력 표, LayerNorm, softmax 시험이 0바이트 다름. DeiT 전체도 281, 0.788097로 같음 |
  | 기존 모델 명령이 새 pipeline(17 step 겹침)에서 깨지는지 | 다섯 모델 모두 겹침 0 |
  | C 합성(compute만) | 기존 imm 루프 II 1 유지, LUT +19.9k, DSP +16 |

  HLS C 코드, 기준 모델, 시뮬레이터, CPU 경로 네 가지가 서로 같다는 데까지 확인됐다.
- 아직 확인 안 된 것:
  - HLS가 만든 RTL 자체. ALUX로 C/RTL cosim을 돌린 적이 없다.
  - GEMM `rq_en = 0` + ALU CPY(`rq_*` + `lut_en`) 경로. 시뮬레이터 GEMM에는 out buffer가 없어서 보드에서만 볼 수 있다.
  - 보드 실행 전체(4단계).

**23:41 [PC] "ALUX compute의 C/RTL cosim(무작위 명령열)을 같이 돌려보자."**
- 이전 PC의 cosim 스크립트는 git 밖에 있어서 새로 만들었다.
  - HLS 프로젝트의 top은 `compute`이고, cflags는 비트스트림 빌드와 같은 `SYSTOLIC=1 ALUX=1`이다.
  - `compute()`는 호출 한 번에 명령 하나를 처리한다. 그래서 testbench가 기존 `alux_test.cc`의 무작위 명령열을 명령마다 하나씩 넣는다. STORE는 testbench가 직접 해서 기준 모델과 비교한다.
  - "17 step 안에 쓴 entry를 다시 읽는" 명령은 cosim 명령열에서 뺀다(`hazards()`). 실제 하드웨어에서는 컴파일러와 vtalib이 이 규칙을 지킨다. 규칙을 어기면 RTL에서 틀리는 것이 정상이다.
- 새 파일은 `sim/alux_cosim_tb.cc`, `sim/cosim_compute.tcl`, `sim/run_cosim.sh`이다. `sim/alux_test.cc`는 `-DALUX_COSIM`일 때만 `compute()`를 직접 부르고 hazard 명령을 뺀다. `VTA_ALUX_HAZARD_OK=1`이면 hazard 명령을 남긴다(대조 시험용). 기존 `run_csim.sh`는 바뀌지 않는다.
- 첫 결과(seed 1개, 약 4분): 무작위 ALU 명령 54개(연산 0–11이 고루 섞임), 110,592바이트를 비교해 모두 같았다. `C/RTL co-simulation finished: PASS`가 나왔다. 명령 하나의 latency는 73–1,113 cycle이다.

**11-14 00:26 [PC] ALUX compute C/RTL cosim 통과 (`b4efc66`), 비트스트림 첫 빌드는 중간에 멈춤**
- 중요: HLS가 만든 ALUX compute RTL이 기준 모델과 같다는 것을 확인했다. 단, 17 step 겹침 규칙을 지키는 명령에 한해서다.
- 아래는 cosim 결과다. seed 20개의 무작위 명령열을 C 실행과 RTL에서 돌려 기준 모델과 비교했다. 대조 시험은 hazard 필터를 끈 명령열이다. testbench가 C 결과가 아니라 정말 RTL 결과를 비교하는지 보려고 넣었다. 그렇다면 C에서는 통과하고 RTL에서만 틀려야 한다.

  | 시험 | C 실행 | RTL |
  |---|---|---|
  | 무작위 명령열 seed 20개 (seed마다 ALU 명령 54개, 110,592바이트 비교) | 20/20 같음 | **20/20 같음**, `C/RTL co-simulation finished: PASS` |
  | 대조: hazard 필터를 끈 명령열, seed 3개 | 3/3 통과 | 3/3 실패 (216 / 463 / 676바이트 다름) |

  - 대조 시험이 예상대로 RTL에서만 틀렸다. testbench가 RTL 결과를 비교하고 있다는 뜻이다.
  - 아직 확인하지 않은 것은 GEMM(`gemm_sa`)과 함께 도는 경로와 보드다. 둘 다 4단계에서 본다.
  - 방법과 결과는 `docs/evta_isa_ext.md` §8.11에 적었다.
- 비트스트림 첫 빌드는 compute IP 합성 뒤 체크포인트를 쓰는 단계에서 멈췄다. 합성 보고는 23:34에 끝났지만, 그 뒤 40분 동안 CPU를 전혀 쓰지 않았고 오류도 없었다.
  - 그때 cosim이 같은 컨테이너에서 같이 돌고 있었다. 이것이 원인인지는 확실하지 않았다.
  - 멈춘 Vivado를 끄고 빌드 디렉터리를 지운 뒤 `make bin`만 단독으로 다시 시작했다(HLS IP는 재사용). 첫 실행 로그는 `build/logs/alux_400_rq_hung1.log`에 남겼다.

**11-14 00:57 [PC] Vivado 합성 멈춤의 원인: 컨테이너 PID 1이 좀비를 정리하지 않음 (`9e025d8`)**
- 중요: 00:26에는 cosim과 동시에 돈 것을 원인으로 의심했다. 그런데 단독으로 다시 돌려도 같은 단계에서 멈췄다. 진짜 원인은 컨테이너 설정이었다.
- 원인을 찾아 고친 순서:

  | 단계 | 한 일 | 결과 |
  |---|---|---|
  | 1차 빌드 | cosim과 함께 실행 | compute IP 합성 직후 체크포인트 단계에서 40분 정지, CPU 0, 오류 없음 |
  | 2차 빌드 | cosim을 빼고 `make bin` 단독 실행 | 같은 단계에서 다시 정지 → 동시 실행은 원인이 아님 |
  | gdb | 멈춘 Vivado를 조사 | 병렬 합성용 하위 프로세스를 기다리는 중(`HARTSJobManager::thisThreadWaitForChildError`). 하위 프로세스는 이미 끝나서 좀비로 남아 있었음 |
  | 원인 | 컨테이너 PID 1이 `sleep infinity` | 부모를 잃은 프로세스를 아무도 reap하지 않음 |
  | 수정 | `docker run --init`으로 컨테이너 재생성 (tini가 PID 1) | 같은 합성이 `vta_compute_0_0.dcp`까지 끝나고, 좀비 0개 |

- `setup.sh`와 `CLAUDE.md`를 고쳐 커밋하고 push했다(`9e025d8`).
- 그 뒤 비트스트림 빌드(`..._sa_alux_400mhz_rq`)는 implementation(Post Commit Optimization)까지 진행했다.

**11-14 01:03 [PC] "ssh 로 접속하면 될거야. 같은 네트워크에 묶여 있으니. 비번이 없어도 접속할 수 있게 key를 만들어놔야 해?" / 01:07 "키를 등록했어."**
- 보드(`192.168.20.2`)는 응답하고 SSH도 열려 있다. 키가 없어 `Permission denied (publickey,password)`가 났다. 사용자 이름은 문서 경로(`/home/xilinx/JO/...`)를 보고 `xilinx`로 가정했고, `ssh-copy-id xilinx@192.168.20.2`를 안내했다.
- 사용자가 키를 등록한 뒤:
  - 키 로그인이 되고, `sudo -n /bin/sh -c "..."`로 root 명령도 실행된다. 보드 저장소는 최신 커밋으로 pull했다.
  - 새 스크립트 `vtalib-zcu102-systolic/tests/run_alux_board_tests.sh`를 만들었다. PC 시뮬레이터에서만 돌던 시험 세 가지(conv 출력 표, LayerNorm, softmax)를 보드용으로 빌드하고, 실제 EVTA에서 돌려 CPU 결과와 바이트 단위로 비교한다.
    - 컴파일러와 vtalib은 `vtabundles_xcmpl/build_sys`와 같은 설정(clang++-8, 같은 링크 옵션)을 쓴다.
    - 시험 세 파일은 보드에서 `zcu102` runtime 헤더를 쓰도록 `#include` 한 줄만 고쳤다.
    - 이 시점에는 커밋 전이었고, 링크 줄을 고친 최신 스크립트는 보드에 직접 복사했다.
- 지금 비트스트림(`_rq`, 확장 없음)에서 이 스크립트를 미리 돌렸다. 확장이 없을 때 대체 경로가 맞게 도는지와 스크립트가 보드에서 돌아가는지를 본 것이다.

  | 시험 | 결과 |
  |---|---|
  | conv 출력 표 4가지 | 0바이트 다름. 확장이 없어서 CPU 대체 경로로 돌았고, 맞게 동작함 |
  | LayerNorm | `NOT RUN`. 확장이 없으면 `layernorm_evta`가 0을 돌려주는 설계라 예상한 결과이고, 시험이 EVTA 경로를 기대해서 실패로 셈 |
  | softmax | LayerNorm에서 멈춰 실행 안 됨 |

- `_alux`가 나오면 할 일: `.bin`을 보드 `/lib/firmware`에 복사해 `load_bit.sh`로 올리고, 세 시험을 실제 EVTA ALU 경로로 돌린 뒤, golden / layer dump 비교와 DeiT 시간 측정을 한다.

**11-14 01:27 [PC] "overview.md 등을 보면 ... resnet18, resnet50, vgg16, mobilenet v2에 대해 지금 한 alu extension 을 사용할 수 있는 게 있는지 정리해보자. ... 연산 시간을 얼마나 줄일 수 있는지도 확인해봐야지."**
- 중요: ALU 확장을 쓸 수 있는 곳은 네 모델 모두에서 **avgpool 하나뿐**이고, 줄어드는 시간은 모델당 1 ms 이하다. MobileNetV2 residual add는 EVTA로 옮기면 약 1 ms 줄지만, 이것은 지금 비트스트림의 기존 ALU로도 된다.
- 아래는 avgpool을 EVTA로 옮겼을 때의 추정이다. CPU 시간은 11-11 보드의 single, OMP 1 측정값이고, EVTA 시간과 줄어드는 시간은 추정이다(보드에서 재지 않았다). 7×7 평균은 ÷49라서 shift만으로는 안 되고, 이번 확장의 MUL이 필요하다.

  | 모델 | CPU avgpool | 계산 방식 | EVTA에서 예상 | 줄어드는 시간(추정) |
  |---|---|---|---|---|
  | Resnet18 | 0.24 ms | 합 → MUL 2번 + rq | 약 0.07 ms | −0.17 ms (전체의 0.7 %) |
  | Resnet50 | 0.95 ms | 합 → MUL 2번 + rq | 약 0.12 ms | −0.8 ms (1.2 %) |
  | MobileNetV2 | 0.59 ms | 합 → MUL 1번 + rq | 약 0.08 ms | −0.5 ms (2.7 %) |
  | VGG16 | 없음 | – | – | 0 |

  - 가능한 모든 합에 대해 CPU와 바이트 단위로 같은지 확인했다. MobileNetV2(비율 1/49)는 MUL 한 번으로 정확하다. Resnet18/50(비율 4/49)은 int16 곱셈 한 번으로는 정밀도가 모자라 맞는 값이 없고, MUL 두 번(28533 × 3, shift 20)이면 정확하다.
  - 중요: CPU avgpool이 느린 진짜 이유는 코드다. 채널을 바깥 루프로 돌며 C바이트 간격으로 읽어서 캐시를 못 쓴다. NEON으로 연속해서 읽게 고치면 100 KB 남짓이라 수십 µs면 된다(추정). 그러면 확장 없이 모든 비트스트림에서 비슷하게 줄어든다. 즉 avgpool만 보면 ALU 확장이 꼭 있어야 얻는 이득은 아니다.
- 확장 없이 지금 ALU로 되는 것: MobileNetV2 residual add.
  - `elemadd` 10개, 1.03 ms가 CPU에서 돈다.
  - bundle의 scale이 모두 2의 거듭제곱(1/2, 1/4, 1/8)이라, MUL 없이 기존 ADD / SHR / clip으로 conv에 합칠 수 있다.
  - 지금 fusion은 relu가 붙은 add만 받는다. relu 없는 경우를 받도록 컴파일러와 vtalib을 고치면 된다.
  - 예상 −0.9 ms 안팎(약 −5 %)이고, 앞뒤의 cache invalidate도 일부 줄어든다.
- 이 네 모델에서 시간과 상관없는 것:
  - LUT, RSUM / RMAX, CLZ는 softmax / GELU / LayerNorm용이라 이 CNN들에는 쓸 곳이 없다. ReLU6는 이미 clip으로 처리하고, Resnet50 softmax는 0.06 ms다.
  - depthwise를 ALU로 하면 탭마다 MUL과 ADD를 II 2로 돌려야 해서 지금의 대각 GEMM보다 느리다.
  - MUL로 2의 거듭제곱이 아닌 scale을 쓰는 것은 시간이 아니라 정확도 쪽 이점이다(예: MobileNetV2 confidence).
- 합친 예상이다. "지금"은 각 모델 최적화 문서의 최종값이고, 나머지 열은 추정이다.

  | 모델 | 지금 | avgpool EVTA | MobileNetV2 residual 융합까지 |
  |---|---|---|---|
  | Resnet18 | 23.3 ms | 약 23.1 ms | – |
  | Resnet50 | 69.6 ms | 약 68.8 ms | – |
  | VGG16 | 7.0 ms | 7.0 ms | – |
  | MobileNetV2 | 18.7 ms | 약 18.2 ms | 약 17.3 ms |

- **`_alux` 비트스트림: 400 MHz 빌드가 timing을 못 맞춰 나오지 않았다(WNS −0.288 ns).**

  | 빌드 | 전략 | WNS | 결과 |
  |---|---|---|---|
  | `_sa_alux_400mhz_rq` | `Performance_ExplorePostRoutePhysOpt` (지금 `_rq`와 같은 설정) | −0.288 ns | 실패, 비트스트림 없음 |

  - 실패한 경로는 ALU 연산 자체가 아니다. reset이나 루프 카운터 같은 제어 신호가 너무 많은 곳으로 퍼지는 경로이고, 지연의 약 80 %가 배선이다. 커진 compute(LUT +19.9k) 때문에 배치가 빡빡해진 것으로 추정한다.
  - 두 가지를 동시에 다시 빌드하기 시작했다: 400 MHz에 `Performance_ExtraTimingOpt` 전략, 그리고 380 MHz에 기존 전략. 380은 보드 기능 시험용으로 더 빨리 나올 것으로 본다. 결과는 이 기간 안에 나오지 않았다.
- 제안한 순서:
  1. vtalib-systolic에 `avgpool_evta`를 만든다. `maxpool_evta`처럼 identity GEMM으로 acc에 올리고, 창 위치마다 ADD를 한 뒤 MUL과 rq를 한다. VTASave는 `if (!avgpool_evta(...)) avgpool(...)` 형태로 낸다. 비교용으로 NEON CPU avgpool도 만든다.
  2. MobileNetV2 residual add를 relu 없이 conv에 합친다(기존 ALU).
  3. 시뮬레이터에서 바이트 단위로 맞춘 뒤, `_alux` 비트스트림이 나오면 보드에서 네 모델을 `_systimed`로 잰다.
  - 권장: avgpool은 확장보다 NEON CPU 쪽이 더 간단하고 이득도 비슷하다. 그래서 둘 다 구현해 보드에서 비교한 뒤 기본값을 정한다.
  - 이 PC 컨테이너에 시뮬레이터 시험용 llvm-8 헤더가 없어서 apt로 설치하겠다고 했다.

## 결정이 남은 것 (11-14 07:00 기준)
- 사용자 답을 기다리는 것:
  - 01:34에 제안한 순서(①`avgpool_evta` + NEON CPU avgpool, ②MobileNetV2 relu 없는 residual 융합, ③시뮬레이터 확인 뒤 보드 `_systimed` 측정)로 진행할지. avgpool 기본값은 둘을 보드에서 비교한 뒤 정하자는 것이 권장이다.
  - `_alux` 재빌드 두 개(400 MHz `Performance_ExtraTimingOpt`, 380 MHz 기존 전략)의 결과. 400이 계속 실패하면 380판을 기능 시험용으로 쓸지, 400을 위해 제어 신호 fanout을 줄이는 수정을 할지 정해야 한다.
  - ubuntu2의 외부 SSH 공개(포트포워딩 또는 Tailscale) 여부. 비밀번호 없는 sudo가 켜져 있으므로, 연다면 키 인증만 쓰는 것이 전제다.
- 이날 결정되거나 끝난 것:
  - Vivado 2020.1은 ubuntu2의 docker 컨테이너 `vivado2020-build`(ubuntu:18.04, `--init`, TZ KST)에서 쓴다. 예전 호스트와 이름·경로가 같다(`9796737`, `9e025d8`).
  - 보드 작업은 ubuntu2에서 `xilinx@192.168.20.2`로 키 ssh로 한다. 저장소 push는 SSH remote로 한다.
  - ALUX compute RTL은 cosim으로 검증됐다(`b4efc66`). 남은 검증은 `gemm_sa`와 함께 도는 경로와 보드(4단계)다.
  - `run_alux_board_tests.sh`와 보드용으로 고친 시험 세 파일은 01:09 기준으로 커밋 전이었다.
  - `~/xilinx_installer_2020.1/`(약 38 GB)은 지워도 된다고 안내했다. 지웠는지는 기록에 없다.
