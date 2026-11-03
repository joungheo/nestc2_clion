# 2026-10-20 EVTA synth 요약: Vivado 2020.1 docker 설치 경위 정정, 보드 gemmprobe 오류는 설계가 아니라 보드 쪽으로 좁힘, 200 MHz 비트스트림 실험 요청

- 기간: 2026-10-20 07:00 ~ 10-21 07:00 (KST)
- 세션은 하나다. [PC]는 PC(호스트)와 그 위의 docker 컨테이너 `vivado2020-build`에서 한 작업이다. 보드 실험은 `log/BOARD_TODO.md`로 보드 세션에 요청만 했다.

## 시간순

**16:43 [PC] "vivado hls 등이 다 설치된 게 맞아? 시간이 1시간 이상 걸리는데 어떻게 3분도 안 되서 다 설치됐지? …"**
- 설치는 제대로 됐다고 답했다. 근거: 컨테이너의 `vivado_hls`와 `vivado`로 오늘 비트스트림 3종을 HLS IP 생성부터 `.bin` 출력까지 빌드했다.
- `/tools/Xilinx/v2020.1/Vivado/2020.1`이 33 GB이고 `bin/`에 `vivado`, `vivado_hls`가 있다. Y2K22 HLS 패치도 적용돼 있다(`y2k22_patch.log`).
- 화면의 작은 창은 설치 프로그램이 아니라 Xilinx Information Center(XIC)다. 설치가 끝날 때 자동으로 뜬 업데이트 알림 프로그램이라 닫아도 된다.
- 몇 분 만에 끝난 이유를 "설치 파일이 `Downloads`에 미리 받아져 있어서"라고 설명했는데, 다음 턴에서 정정했다.

**16:51 [PC] "Downloads에 받은 적이 없는데. … docker 안에서 ubuntu 18.04 안에 설치되지 않아? 왜 밖에 설치되어 있어? …"**
- 중요: 앞의 설명이 틀렸다고 정정했다. 3분은 두 번째 설치만 본 결과였다. 대화 기록으로 확인한 10-19 설치 순서다(시각은 UTC, KST = +9시간).

  | 시각 (UTC) | 단계 | 결과 |
  |---|---|---|
  | 11:21 | 사용자가 받은 수백 MB `Xilinx_Unified_2020.1_0602_1208_Lin64.bin`을 컨테이너 안에서 batch 모드로 실행 | 설치 프로그램이 13.83 GB를 설치 경로 아래 `Downloads/`로 자동 다운로드. 다운로드와 압축 풀기 약 17분 |
  | 11:39 | 첫 설치 (경로 `/tools/Xilinx`) | 다른 버전들과 함께 쓰는 `/tools/Xilinx/xic`를 덮어쓰다 압축 풀기 오류 → 설치 프로그램이 스스로 되돌림 |
  | 11:41 | 받은 파일을 `/tools/Xilinx/v2020.1/Downloads`로 옮기고 경로를 `/tools/Xilinx/v2020.1`로 바꿔 재실행 | 다운로드 없이 3분 만에 완료 |

  처음부터 끝까지 약 23분이었다. 1시간이 안 걸린 것은 다운로드가 빨랐기 때문이다.
- docker 안에 설치된 게 맞다. 설치도 빌드도 모두 컨테이너(ubuntu 18.04) 안에서 실행됐고, 아래 폴더가 bind mount라서 파일이 호스트 디스크에 있다.

  | 호스트 경로 | 컨테이너 경로 | 용도 |
  |---|---|---|
  | `/tools/Xilinx` | `/tools/Xilinx` | Vivado 설치 위치 |
  | `/home/ubuntu/npuprg/nestc2_clion/nest-compiler` | 같은 경로 | 소스와 빌드 결과 |
  | `/home/ubuntu/Downloads` | 같은 경로 | 설치 프로그램 |
  | `/tmp/.X11-unix` | 같은 경로 | 화면 출력 (XIC 창이 호스트 화면에 뜬 이유) |

- mount를 쓴 이유: 컨테이너를 지워도 남고, 다른 Xilinx 버전(2018.3, 2020.2)과 같은 배치이며, 복사 없이 호스트에서 고친 코드를 바로 빌드한다. 구조를 그대로 두길 권했다.
- 정리 후보: `/tools/Xilinx/v2020.1/Downloads`(15 GB, 재설치용 캐시)와 XIC 창. 사용자가 말하면 지우거나 닫겠다고 했다.

**17:53 [PC] "vivado 2020.1이 호스트 ubuntu 20.04에는 안 깔리는데 … 실행은 항상 docker 안에 들어가서 해야하지? … 메모리는 호스트 걸 같이 쓰는 거네."**
- 그렇다고 답했다. Vivado 2020.1은 18.04 라이브러리(`libtinfo5` 등)를 전제로 하므로 항상 컨테이너 안에서 실행한다. 호스트에서는 실행한 적이 없다.
  - 실행: `docker exec -it -u ubuntu vivado2020-build bash` → `source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh`
- 결과는 mount된 호스트의 `vta-hw-zcu102-*/build/...`에 바로 생긴다. 18.04인 것은 사용자 영역뿐이고 커널(5.15), CPU 32코어, 메모리 약 125 GB는 호스트 것을 제한 없이 쓴다(빌드 3개 동시 실행이 가능했던 이유).
- 컨테이너는 restart policy `no`라서 호스트 재부팅 뒤에는 `docker start vivado2020-build`를 한 번 해야 한다.

**19:01 [PC] git pull 후 "log/BOARD_TODO.md의 Results를 보고 원인을 찾아 보자"**
- 원인은 아직 못 찾았다. 설계 자체(HLS 코드, 모듈 RTL, 블록 디자인 연결)는 아니라는 것까지 확인했고, 다음 보드 실험을 정리해 push했다(`12f880b9`).
- 호스트 검증: 보드에 올린 비트스트림과 같은 IP, 같은 연결로 단계별로 확인했다.

  | 단계 | 결과 |
  |---|---|
  | 레지스터 맵, 명령어 큐 폭, 명령어 비트 배치 | 런타임(GCC)과 하드웨어(HLS)가 일치 |
  | `vta.cc` C 시뮬레이션 (gemmprobe 5가지) | 모두 정답 |
  | compute·load 모듈 단위 RTL 시뮬레이션 (prof·no-prof) | PASS |
  | PS를 테스트용 모델로 바꾼 PL 전체 시뮬레이션 | alu·zero·ident·perm·ones 모두 OK. prof도 멈추지 않고 끝남 |
  | 실제 빌드 결과물의 BRAM 주소 비트, AXI 속성, 주소 맵 | 의도대로 |

- 중요: 비트스트림을 새로 올린 직후에는 모든 메모리가 0이라 `gemmprobe reset`은 0이어야 하는데, 보드에서는 garbage가 나왔다. 입력이 전부 0이어도 GEMM 결과가 틀리므로 원인은 설계가 아니라 보드 쪽이다. 후보는 세 가지다.
  - 실제 칩에서의 timing이나 구현 문제
  - PS 쪽 포트 설정(HP0 데이터 폭 등)
  - 런타임이 실제로 보내는 명령어 스트림의 차이
  - pl_clk0는 보드 세션이 100 MHz로 확인했으므로 클럭 설정 문제는 아니다.
- 보드 세션에 요청한 실험(`log/BOARD_TODO.md` 끝부분):
  1. 새로 추가한 `hwtest/plclk.py`로 HP0 읽기·쓰기 폭이 128비트인지 확인.
  2. 같은 IP로 클럭만 200 MHz로 낮춘 비트스트림(WNS +0.265 ns)으로 gemmprobe 재실행. 되면 333 MHz timing 문제, 똑같이 실패하면 timing이 아니다. 이 파일은 git에 없어 scp로 보드 `bit/`에 옮겨야 한다(`.../build/clk200/.../vtahw_zcu102_1x16_i8w8a32_16_16_19_18_clk200.bin`).
  3. `gemmprobe_zero.out`, `gemmprobe_ident.out`(명령어 dump) 커밋. 받으면 런타임의 실제 명령어 스트림을 PL 시뮬레이션에 넣어 볼 예정.
- 중요: 앞서 문서에 적은 HP 포트 폭 레지스터(AFIFM) 주소가 두 칸씩 밀려 있었다. HP0는 `0xFD380000`이고 `BOARD_TODO.md`에서 고쳤다.
- load와 compute IP에 같은 이름의 `reset_mem` 모듈이 있어 시뮬레이션에서 이름이 충돌했다. 실제 빌드는 IP마다 따로 합성하므로 하드웨어에는 영향이 없다.
- PL 시뮬레이션 환경(명령어 생성기, BD 스크립트, 테스트벤치)은 git 밖의 `vta-hw-zcu102-single/build/hardware/xilinx/cosim/`에 있다. 원하면 트리 안으로 옮겨 커밋하겠다고 했다.

## 결정이 남은 것 (10-21 07:00 기준)
- 보드 실험 결과 대기: HP0 폭 확인(`plclk.py`), 200 MHz 비트스트림 gemmprobe, 명령어 dump(`gemmprobe_*.out`) 커밋. 200 MHz `.bin`은 사용자가 보드로 옮겨야 한다.
- PL 시뮬레이션 환경(`build/.../cosim/`)을 트리 안으로 옮겨 커밋할지.
- `/tools/Xilinx/v2020.1/Downloads`(15 GB) 삭제와 XIC 창 종료를 할지.
- 이미 정해진 것: Vivado 2020.1은 호스트 디스크에 mount로 둔 채 항상 `vivado2020-build` 컨테이너 안에서 실행한다.
