# EVTA on ZCU102 사용 설명서 (명령, 스크립트, 옵션)

이 문서는 명령과 스크립트를 **한곳에** 모은 참고서다. 범위는 환경 만들기, bitstream, bundle 생성, 보드 빌드, 실행과 측정, 정확도, 시험이다.
각 명령마다 어느 기계의 어느 dir에서 실행하는지와 옵션의 뜻을 적는다. 장은 하는 일의 종류별로 나눴다.

- 처음부터 그대로 따라 하는 순서: [evta_walkthrough.md](evta_walkthrough.md). 컴파일러 cmake / make, VTASaverTest, profile, model-compiler, 보드 빌드, 실행, 로그 보기 순서다.
- 지금까지 한 일의 요약: [evta_overview.md](evta_overview.md).
- 스크립트를 새로 만들거나 옵션을 더하면 **이 문서의 해당 장에 한 줄을 더한다.**

| 장 | 내용 | 주로 어디서 |
|---|---|---|
| 0 | 기계와 dir | |
| 1 | 환경 만들기, **PC에서 docker 안에서 명령 치는 법** | 호스트 PC |
| 2 | bitstream 만들기와 HLS 시험 | PC docker `vivado2020-build` |
| 3 | bundle 소스 만들기, VTA backend 옵션, `VTA_FUSION_LOG` | PC docker `nestc-build` 또는 보드 |
| 4 | 보드용 실행 파일 빌드 | 보드, 또는 PC cross build |
| 5 | 실행과 측정 | 보드 (sudo) |
| 6 | 정확도 (ImageNetV2) | 보드 + PC |
| 7 | runtime 환경 변수 | 실행할 때 |
| 8 | vtalib 시험 | PC 또는 보드 |
| 9 | 하드웨어 시험 (`hwtest/`) | 보드 |
| 10 | 그 밖의 도구 | |
| 11 | 자주 하는 일의 순서 | |

**읽는 법:**
- 명령 안의 `<이름>`은 **골라서 넣는 값**이다. 각 명령 아래 "넣을 것"에 무엇이 들어가는지와 예를 적었다. 꺾쇠는 쓰지 않는다.
- `[...]`는 **빼도 되는** 인자다. `a|b`는 둘 중 하나를 쓴다는 뜻이다.
- 여러 장에 나오는 값:

| 자리 | 뜻 | 예 |
|---|---|---|
| `<tree>` | nest-compiler 소스 dir | PC `/home/jerry/npuprg/nestc2_clion/nest-compiler`, 보드 `/home/xilinx/JO/npuprg/nestc2_clion/nest-compiler` |
| `<Dir>` | 모델 / 시험 하나의 dir 이름. `vta/bundles/<Dir>`, `vtabundles_xcmpl/hw/<Dir>`가 같은 이름이다. 대소문자를 구분한다 | `Resnet18Test`, `Resnet50Test`, `Vgg16Cifar10Test`, `MobileNetV2Test`, `DeiTTest`, `Conv3Test` |
| `<target>` | 그 `<Dir>`의 make target(= 실행 파일 이름) | `vtaMxnetResnet18Bundle` (표는 §4.1) |
| `<bin>` | 보드 `/lib/firmware/`의 bitstream 파일 이름(경로 없이) | `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_alux3_400mhz_rq.bin` |
| `<suffix>` | 실행 파일 이름 끝 = 어느 build dir에서 빌드했나(§4.1) | `_sysprof`, `_systimed` |

## 0. 기계와 dir

| 기계 | 위치 | 하는 일 |
|---|---|---|
| 호스트 PC `ubuntu2` (Ryzen 9 5950X, user `jerry`) | `/home/jerry/npuprg/nestc2_clion/nest-compiler` (git `main`) | 코드 수정, commit / push, bundle 생성, cross build, bitstream 합성 |
| 첫 호스트 PC (user `ubuntu`) | `/home/ubuntu/npuprg/nestc2_clion/nest-compiler` | 위와 같은 일. 과열로 꺼진 적이 있어 전체 코어를 쓰는 일은 하지 않는다([host_pc_shutdown.md](host_pc_shutdown.md)) |
| PC docker `vivado2020-build` | `$HOME`와 `/tools/Xilinx`가 같은 경로로 mount, user `ubuntu` | Vivado / Vivado HLS 2020.1 (`/tools/Xilinx/v2020.1/`) |
| PC docker `nestc-build` | `$HOME`가 같은 경로로 mount, user `ubuntu` | NEST-C 시뮬레이터 빌드 `Debug2_remote`(model-compiler 등), 보드용 cross build. 보드와 같은 bundle을 바이트까지 같게 만든다 |
| ZCU102 보드 (`xilinx@192.168.20.2`) | `~/JO/npuprg/nestc2_clion/nest-compiler` | 실행과 측정. bundle 생성과 보드 빌드도 되지만 느리다 |

- **PC와 보드의 소스는 git으로만 맞춘다.**
  - PC에서 `git push origin main`, 보드에서 `git pull`한다.
  - `origin`은 GitHub 원격 저장소, `main`은 브랜치 이름이다.
  - submodule `vta/vtalib`는 그 안에서 따로 `git pull origin main`한다.
  - scp로 소스를 덮지 않는다.
- **scp로 보내는 것:** bitstream 파일(§2.3), PC에서 cross build한 실행 파일(§4.2).
- **`Debug2_remote/`와 `vtabundles_xcmpl/build*/`는 git 밖이다**(빌드 결과). 거기에 둔 스크립트는 pull로 오지 않는다.
- **두 PC가 같이 일할 때:** pull한 뒤 [recent_changes.md](recent_changes.md)의 맨 위를 읽고, 다른 쪽이 할 일이 생기는 변경을 push할 때 그 파일 맨 위에 한 항목을 더한다.
  빌드한 것이 어느 commit인지는 파일에 적혀 있다(§4.4).

```bash
# 보드에서 (최신으로)
cd ~/JO/npuprg/nestc2_clion/nest-compiler && git pull && (cd vta/vtalib && git pull origin main)
```

| 소스 dir | 내용 |
|---|---|
| `lib/Backends/VTA/` | backend. `VTASave.cpp`가 bundle 코드를 만든다(옵션은 §3.3) |
| `include/Backends/VTA/VTASchedules.h` | conv tile schedule |
| `vta/vtalib/` (submodule) | EVTA runtime과 bundle 함수(HLS GEMM bitstream용) |
| `vtalib-zcu102-systolic/` | 같은 runtime의 systolic bitstream용 사본. ALU 확장, epilogue 등 새 기능은 여기에만 있다 |
| `vta-hw-zcu102-single/`, `-multi2/`, `-multi4/`, `-systolic/` | bitstream 소스(HLS, RTL, Vivado tcl) |
| `vtabundles_xcmpl/` | 보드 실행 파일을 빌드하는 독립 CMake 프로젝트, bundle 생성 / 실행 / 측정 스크립트 |
| `utils/docker_vivado2020/`, `utils/docker_nestc/` | PC docker 환경(§1) |

## 1. 환경 만들기와 PC에서 docker로 작업하는 법 (호스트 PC)

자세한 배경은 [host_pc_build.md](host_pc_build.md)에 있다.

**무엇을 하나:** PC에 docker 컨테이너 두 개를 만든다. 하나는 bitstream을 만드는 Vivado용이고, 하나는 bundle을 만드는 NEST-C용이다.
보드와 같은 컴파일러와 라이브러리를 넣어서, PC에서 만든 결과가 보드에서 만든 것과 같게 한다. 만드는 것(§1.3)은 **PC마다 한 번만** 하면 된다.
매일 하는 일은 §1.1~§1.2의 "켜기, 들어가기, 명령 치기"다.

### 1.1 컨테이너란, 그리고 어디서 무엇을 치나

- **컨테이너**는 PC 안에서 따로 도는 작은 Linux다. 여기서는 Ubuntu 18.04다. PC는 Ubuntu 20.04라서, 보드(18.04)와 같은 clang-8, cmake 3.10이나 Vivado 2020.1을 PC에 바로 깔 수 없다. 그래서 컨테이너 안에 깔았다.
- **파일은 같다.** PC의 `$HOME`(`/home/jerry`)이 컨테이너 안에도 **같은 경로**로 보인다(mount). 그래서 소스 경로가 안과 밖에서 같다.
  - 안에서 만든 파일은 밖에서 바로 보인다. 밖에서 고친 소스도 안에서 바로 보인다.
  - Vivado 컨테이너에는 `/tools/Xilinx`도 mount돼 있다.
- **사용자:** 컨테이너 안의 사용자 `ubuntu`는 PC의 `jerry`와 번호(uid 1000)가 같다. 그래서 만든 파일의 주인이 `jerry`가 된다.
  - **`-u ubuntu`를 빼면 root로 돈다.** 그러면 만든 파일이 root 소유가 되어, 밖에서 지우거나 고칠 수 없다. 항상 `-u ubuntu`를 붙인다.
- **PC(`ubuntu2`)에서는 `docker`에 `sudo`가 필요하다.** `jerry`가 docker 그룹에 없기 때문이다.
  - `sudo` 없이 치면 `permission denied while trying to connect to the Docker daemon socket`이 나온다.
  - 첫 PC에서 sudo 없이 되면 빼도 된다.

| 어디서 | 무엇을 | 왜 |
|---|---|---|
| PC 셸(컨테이너 밖) | 소스 고치기, `git` commit / push / pull, `ssh` / `scp`로 보드에 보내기, `sync_sysroot.sh`, `docker` 명령 자체 | 컨테이너 안에는 `ssh`가 없고, git 인증도 밖에 있다 |
| `nestc-build` 안 | `make model-compiler`, `build_sim_bundles.sh`, `gen_*_bundle.sh`, `gen_multi4_bundles.sh`, `collect_hw_bundles.py`, PC cross build(§4.2), vtalib 시험(§8) | 보드와 같은 clang-8 / cmake / protobuf가 여기 있다 |
| `vivado2020-build` 안 | `make ip`, `make bin`, `run_cosim.sh`(§2) | Vivado 2020.1이 여기 있다 |

ONNX 도구(`fold_onnx.py`, `float_ref.py`, `fakequant.py`)는 onnx 1.13 이상과 onnxruntime이 필요하다.
지금 `nestc-build`에는 onnx 1.11이 있고, `ubuntu2` 셸에는 onnx가 없다(2026-11-21). 처음 쓸 때 PC 셸에서 venv를 하나 만든다.
아래 명령은 아직 이 PC에서 돌려 보지 않았다.
```bash
python3 -m venv ~/.venvs/onnx && ~/.venvs/onnx/bin/pip install onnx onnxruntime numpy pillow
~/.venvs/onnx/bin/python vtabundles_xcmpl/fold_onnx.py ...      # 이 python으로 부른다
```

### 1.2 켜기, 들어가기, 명령 치기

**1) 켜져 있는지 본다.** PC 셸에서 친다.
```bash
sudo docker ps -a --format '{{.Names}}  {{.Status}}'
#   nestc-build  Up 7 days          <- Up이면 켜져 있다
#   vivado2020-build  Up 7 days
```
- 두 컨테이너는 `--restart unless-stopped`로 만들어서 PC를 다시 켜면 저절로 켜진다.
- `Exited`이면 켠다: `sudo docker start nestc-build vivado2020-build`
- 목록에 없으면 아직 안 만든 것이다(§1.3).

**2) 명령을 치는 방법은 두 가지다.**

**(가) 안으로 들어가서 친다.** 여러 명령을 이어서 칠 때 쓴다.
```bash
sudo docker exec -it -u ubuntu nestc-build bash       # PC 셸에서. 들어가면 프롬프트가 바뀐다
ubuntu@nestc-build:/$ cd /home/jerry/npuprg/nestc2_clion/nest-compiler     # 처음 위치는 / 이다. 먼저 cd
ubuntu@nestc-build:.../nest-compiler$ vtabundles_xcmpl/build_sim_bundles.sh -f vtaMxnetResnet18Bundle
ubuntu@nestc-build:.../nest-compiler$ exit             # 나온다. 컨테이너는 계속 켜져 있다
jerry@ubuntu2:~$                                       # 다시 PC 셸
```
- `-it`는 키보드 입력을 받는 터미널을 연다는 뜻이다. 들어가서 칠 때는 꼭 붙인다.
- **지금 어디에 있는지는 프롬프트의 `@` 앞뒤로 안다.**
  - `ubuntu@nestc-build`: nestc-build 안
  - `jerry@ubuntu2`: PC 셸
  - `ubuntu@ubuntu2`: **vivado2020-build 안.** 이 컨테이너는 PC와 같은 hostname을 써서, 사용자 이름(`ubuntu`)으로만 구분된다.
- Vivado 컨테이너는 들어갈 때마다 먼저 `source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh`를 친다. 이걸 쳐야 `vivado`와 `vivado_hls`를 찾는다.

**(나) 밖에서 명령 하나만 안에서 돌린다.** 한 줄로 끝날 때, 스크립트나 nohup으로 돌릴 때 쓴다.
```bash
sudo docker exec -u ubuntu -w /home/jerry/npuprg/nestc2_clion/nest-compiler nestc-build \
    vtabundles_xcmpl/build_sim_bundles.sh -f vtaMxnetResnet18Bundle
```
- `docker exec ... nestc-build <명령>`은 `<명령>` **하나만** 안에서 돌리고 PC 셸로 돌아온다. 그다음 줄에 치는 것은 다시 **PC에서** 돈다.
  - `sudo docker exec -u ubuntu nestc-build`처럼 명령 없이 치면 아무 데도 들어가지 않는다. 인자가 모자란다는 오류만 난다.
  - 들어가서 계속 치려면 (가)처럼 `-it ... bash`로 들어간다.
- `-w <dir>`: 안에서 그 dir에서 시작한다. 빼면 `/`에서 시작하므로 상대 경로가 맞지 않는다.
- 파이프(`|`), `&&`, `cd`, 환경 변수를 쓰려면 `bash -c '...'`로 감싼다. 감싸지 않으면 `|` 뒤는 PC에서 돈다.
```bash
sudo docker exec -u ubuntu -w /home/jerry/npuprg/nestc2_clion/nest-compiler nestc-build bash -c \
    'cd Debug2_remote && make -j16 model-compiler && cd .. && VTA_FUSION_LOG=1 vtabundles_xcmpl/gen_deit_bundle.sh'
```
- 오래 걸리는 일은 PC 셸에서 `nohup`으로 띄우고 로그를 본다. 터미널을 닫아도 계속 돈다.
```bash
nohup sudo docker exec -u ubuntu vivado2020-build bash -c \
  'source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh && cd /home/jerry/npuprg/nestc2_clion/nest-compiler/vta-hw-zcu102-systolic/hardware/xilinx && make bin SYSTOLIC=1 FREQ=400 JOBS=4' \
  > ~/vivado_bin.log 2>&1 < /dev/null &
tail -f ~/vivado_bin.log            # Ctrl-C는 tail만 끝낸다. make는 계속 돈다
```
  - `ubuntu2`의 `jerry`는 비밀번호 없이 sudo가 된다. 비밀번호를 묻는 PC에서는 nohup 뒤에서 멈추므로, 먼저 `sudo -v`를 한 번 쳐 둔다.

**3) 하루 작업의 예: backend를 고치고, bundle을 만들고, 보드에서 돌리기**
```bash
# --- PC 셸 ---
cd /home/jerry/npuprg/nestc2_clion/nest-compiler
vi lib/Backends/VTA/VTASave.cpp                                   # 소스 고치기(밖에서)
sudo docker exec -it -u ubuntu nestc-build bash                   # 들어가기
# --- nestc-build 안 ---
cd /home/jerry/npuprg/nestc2_clion/nest-compiler/Debug2_remote
make -j16 model-compiler                                          # 바뀐 파일만 다시 컴파일
cd .. && vtabundles_xcmpl/build_sim_bundles.sh -f vtaMxnetResnet18Bundle     # bundle 다시 만들기(§3)
cd vtabundles_xcmpl && ./collect_hw_bundles.py Resnet18Test       # hw/Resnet18Test/로 모으기(§4)
cd build_pc_systimed && make -j16 vtaMxnetResnet18Bundle          # 보드용 cross build(§4.2, 처음엔 build dir부터)
exit
# --- PC 셸 ---
git checkout vtabundles_xcmpl/hw/script/run_all.sh                # PC에서 collect하면 그림 경로가 PC 경로로 바뀐다. 되돌린다
scp vtabundles_xcmpl/hw/Resnet18Test/vtaMxnetResnet18Bundle_pcsystimed \
    vtabundles_xcmpl/hw/Resnet18Test/mxnet_exported_resnet18.weights.bin \
    xilinx@192.168.20.2:JO/npuprg/nestc2_clion/nest-compiler/vtabundles_xcmpl/hw/Resnet18Test/
ssh xilinx@192.168.20.2                                            # 보드로
# --- 보드 ---
cd ~/JO/npuprg/nestc2_clion/nest-compiler/vtabundles_xcmpl
sudo /bin/sh -c "hw/script/run_prof.sh -s _pcsystimed -r Resnet18Test"   # §4.2, §5
```
- **보드로 보내는 것은 실행 파일과 weights뿐이다.** build dir(`build_pc_systimed/`)은 옮기지 않는다(§4.2).
- 소스를 바꿨으면 보드에도 같은 소스가 있도록 PC 셸에서 `git commit`, `git push`하고 보드에서 `git pull`한다(§0).
- **weights 파일도 같이 보낸다.** 실행 파일은 실행할 때 같은 dir의 `<모델>.weights.bin`을 읽는다. bundle을 다시 만들면 이 파일도 바뀔 수 있다.
- `run_all.sh`를 되돌리는 것은 PC에서만 한다. 보드에서는 `git checkout`으로 되돌리지 않는다(§4.1).
- PC cross build를 보드에서 확인한 것은 MobileNetV2뿐이다([host_pc_build.md](host_pc_build.md) §6). 다른 모델은 처음 한 번 보드 빌드와 결과를 비교한다.
- 보드에서 만들고 빌드해도 된다(§3, §4.1). 느리지만 scp와 되돌리기가 필요 없다.

### 1.3 처음 한 번: 컨테이너 만들기

PC 셸(컨테이너 밖)에서 친다. 스크립트 안의 `docker` 명령에 권한이 필요하므로 `sudo`로 부른다.
스크립트가 `SUDO_UID`로 원래 사용자(`jerry`)의 번호와 `$HOME`을 찾아 컨테이너 사용자 `ubuntu`를 만든다.

| 일 | 명령 | 비고 |
|---|---|---|
| Vivado 컨테이너 만들기 | `sudo utils/docker_vivado2020/setup.sh` | ubuntu:18.04 이미지와 `vivado2020-build` 컨테이너. `--init`이 없으면 합성이 `synth_design` 뒤에 멈춘다 |
| Vivado 설치 | 컨테이너 안(§1.2 (가))에서 `utils/docker_vivado2020/install_vivado.sh <Xilinx_Unified_2020.1_0602_1208.tar.gz> [y2k22_patch-1.2.zip]` | MD5 확인, Zynq UltraScale+만 설치, Y2K22 HLS 패치. license `~/.Xilinx/xilinx.lic` |
| NEST-C 컨테이너 만들기 | `sudo utils/docker_nestc/setup.sh` | `nestc-build`. 보드와 같은 clang-8, cmake 3.10, protobuf 3.0 |
| 시뮬레이터 빌드 configure + make | `sudo docker exec -u ubuntu nestc-build <tree>/utils/docker_nestc/configure.sh [targets]` | `Debug2_remote`를 보드와 같은 옵션으로. 기본 target은 model-compiler, image-classifier(-j16, 약 10분). 모델 파일은 보드의 `Debug2_remote`에서 복사 |
| 보드 sysroot 복사 (PC 셸) | `utils/docker_nestc/sync_sysroot.sh [xilinx@192.168.20.2] [~/zcu102_sysroot]` | cross build용(약 2.1 GB). 보드에 라이브러리를 새로 깔면 다시 돌린다 |

넣을 것:
- `<Xilinx_Unified_...tar.gz>`: Xilinx에서 받은 Vivado 2020.1 설치 파일. 예: `~/Downloads/Xilinx_Unified_2020.1_0602_1208.tar.gz`
- `[targets]`(configure.sh): 빌드할 make target. 빼면 `model-compiler image-classifier`. 예: `model-compiler`
- `[xilinx@192.168.20.2] [~/zcu102_sysroot]`(sync_sysroot.sh): 보드 주소, 복사할 곳. 보통 둘 다 뺀다.

예(새 PC에서 처음):
```bash
cd /home/jerry/npuprg/nestc2_clion/nest-compiler
sudo utils/docker_nestc/setup.sh                                                 # 이미지 빌드 + 컨테이너, 수 분
sudo docker exec -u ubuntu nestc-build $PWD/utils/docker_nestc/configure.sh      # 약 10분
utils/docker_nestc/sync_sysroot.sh                                           # cross build를 할 때만
```

## 2. bitstream 만들기 (PC, docker `vivado2020-build`)

**무엇을 하나:** EVTA 하드웨어 소스(HLS C++과 RTL)를 FPGA에 올릴 파일(`.bin`)로 만든다.
- `make ip`: HLS로 EVTA의 네 블록(fetch, load, compute, store)을 회로로 바꾼다.
- `make bin`: Vivado로 배치(place)하고 배선(route)해서 bitstream을 만든다.
- 하드웨어(ISA, ALU, clock)를 바꿀 때만 한다. 소프트웨어만 바꿀 때는 하지 않는다.

### 2.1 명령

PC 셸에서 들어가서(§1.2 (가)) 친다.
```bash
sudo docker exec -it -u ubuntu vivado2020-build bash        # 프롬프트가 ubuntu@ubuntu2로 바뀐다
source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh
cd <tree>/vta-hw-zcu102-systolic/hardware/xilinx
make ip CSIM=1 SYSTOLIC=1                      # HLS: C simulation + 4개 IP 합성 (약 10분)
make bin SYSTOLIC=1 FREQ=400 JOBS=4 IMPL_STRATEGY=Performance_ExtraTimingOpt BUILD_TAG=<tag>   # Vivado (약 40분)
```

넣을 것:
- `<tree>`: 위 표.
- `<tag>`: 같은 설정의 다른 판을 구분하는 짧은 이름. 빼도 된다. 예: `eto`(전략 이름), `try2`
- 옵션 값은 §2.2.

예: ALU 확장 3단계, systolic 400 MHz. 출력 이름은 `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_alux3_400mhz_rq.bin` 꼴이 된다.
```bash
make ip CSIM=1 SYSTOLIC=1 ALUX=3
make bin SYSTOLIC=1 ALUX=3 FREQ=400 JOBS=4 IMPL_STRATEGY=Performance_ExplorePostRoutePhysOpt
```

- 다른 copy도 같은 방법이다(`vta-hw-zcu102-single`, `-multi2`, `-multi4`의 `hardware/xilinx`).
- **첫 PC에서는 `JOBS=4`로 돌린다.** Vivado 전체 부하 중에 PC가 세 번 꺼졌다(10-25, 10-28, 11-02).
  - 돌릴 때 온도를 남기려면 `templog.sh`를 백그라운드로 띄운다(§10).
- 오래 걸리므로 PC 셸에서 `nohup sudo docker exec ...`으로 띄워도 된다. 명령 전체는 §1.2 (나)의 예다.

### 2.2 make 옵션

| 옵션 | 기본 | 뜻 |
|---|---|---|
| `SYSTOLIC=1` | 0 | GEMM을 RTL systolic 어레이 `gemm_sa`로(`-DVTA_SYSTOLIC`, dir 이름에 `_sa`). `vta-hw-zcu102-systolic`, `-multi4`에서 |
| `ALUX=1` / `ALUX=2` / `ALUX=3` | 0 | ALU 확장(`_alux`, `_alux2`, `_alux3`; [evta_isa_ext.md](evta_isa_ext.md) §8). systolic copy에서 |
| `ALUX_NOIMM=1` | 0 | ALU의 immediate 전용 루프를 빼고 slot 루프만 쓴다(`VTA_ALUX_NO_IMM_LOOP`, IP 이름에 `_ni`). compute LUT 49.5k → 38.5k, multi4 ALU 확장 판이 칩에 들어가게 한다. `sim/run_csim.sh NOIMM=1`로 C 시뮬레이션 |
| 환경 `VTA_DATA_IC=1` | 0 | multi4: data master를 SmartConnect 대신 AXI Interconnect로. LUT가 줄지 않고 BRAM이 늘어(580 → 742) 쓰지 않는다 |
| `FREQ=<MHz>` | copy 기본값 | EVTA PLL 클럭(dir 이름에 `_<FREQ>mhz`) |
| 환경 `VTA_HP_FREQ` | 333 | 333 MHz보다 높게 빌드할 때 HP 포트 쪽 클럭(`hp_pll`). PS8 HP 포트는 333 MHz가 한계다 |
| `PROFILE=1` | 0 | compute 카운터와 AXI perf monitor(`_prof`). 보드에서는 vtalib `-DNESTC_EVTA_PROFILE_AXI_MONITOR=ON`과 같이 쓴다 |
| `MEM_OREG=1` | 0 | compute의 inp/wgt BRAM 읽기에 출력 register(`_oreg`) |
| `CSIM=1` | 0 | `make ip` 전에 HLS C simulation(`sim/vta_test.cc`) |
| `JOBS=<n>` | 8 | Vivado 동시 run 수(`launch_runs -jobs`). 첫 PC는 **4** |
| `IMPL_STRATEGY=<이름>` | (기본 전략) | Vivado implementation 전략. systolic 400 MHz는 `Performance_ExtraTimingOpt` 또는 `Performance_ExplorePostRoutePhysOpt` |
| `BUILD_TAG=<tag>` | 없음 | vivado dir과 export 이름 끝에 `_<tag>`. 같은 설정의 다른 판을 구분할 때 |
| `EXPORT_NAME=<이름>` | `vtahw_<dir 이름>` | 출력 파일 이름 |
| 환경 `VTA_BRAM_FENCE` | `y219` | 이 보드의 결함 행(SLICE Y219)을 피하는 방법: `y219`, `upper`, `none`. 이 보드에서는 바꾸지 않는다 |
| 환경 `VTA_RESET_MC=0` | 1 | reset 경로의 multicycle 2(`reset_multicycle.xdc`)를 끈다 |
| 환경 `VTA_QUEUE_RS` | 380 MHz 초과에서 켜짐 | command queue 뒤의 AXIS register slice |

### 2.3 결과 확인과 보드로 옮기기

- **출력:** `<copy>/build/hardware/xilinx/vivado/<dir>/export/vtahw_<dir>.{bin,bit,hwh,xsa,timing.rpt,utilization.rpt}`
- **timing:** `*.timing.rpt`의 Design Timing Summary에서 WNS, WHS, WPWS가 모두 0 이상인지 본다.
  - 실패 path를 묶어 보려면 routed checkpoint(`vta.runs/impl_1/vta_wrapper_routed.dcp`)를 Vivado로 연다.
  - 그다음 `get_timing_paths -max_paths 3000 -nworst 1 -slack_lesser_than 0`의 시작점과 끝점을 센다.
- **보드로 옮기기:** dir 이름 규칙은 `bit/<목적>_<YYMMDD>`이다. 보드에서 통과한 판에는 `<clock>_pass_`를 붙인다.

```bash
# PC에서
ssh xilinx@192.168.20.2 mkdir -p /home/xilinx/JO/npuprg/bit/<목적>_<YYMMDD>
scp <export>/vtahw_<dir>.{bin,bit,hwh,timing.rpt,utilization.rpt} xilinx@192.168.20.2:/home/xilinx/JO/npuprg/bit/<목적>_<YYMMDD>/
# 보드에서
sudo cp /home/xilinx/JO/npuprg/bit/<목적>_<YYMMDD>/vtahw_<dir>.bin /lib/firmware/
```

넣을 것:
- `<export>`: `make bin`의 출력 dir. 예: `vta-hw-zcu102-systolic/build/hardware/xilinx/vivado/zcu102_1x16_i8w8a32_16_16_19_18_sa_alux3_400mhz_rq/export`
- `<dir>`: 그 vivado dir 이름. 예: `zcu102_1x16_i8w8a32_16_16_19_18_sa_alux3_400mhz_rq`
- `<목적>_<YYMMDD>`: 무엇을 위한 판인지와 날짜. 예: `alux3_261115`. 보드에서 통과하면 `400mhz_pass_alux3_261115`

- `.bit`만 있으면 `vtabundles_xcmpl/hwtest/bit2bin.py <x>.bit`로 `.bin`을 만든다.
- JTAG(ILA, hw_server)를 쓰기 전에는 보드가 부팅될 때마다 `sudo vtabundles_xcmpl/hwtest/nocpuidle.sh`를 먼저 실행한다. 안 하면 Linux가 멈춘다.

### 2.4 지금까지 만든 bitstream과 어느 것을 쓰나

- **HLS GEMM:** `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_380mhz.bin`(10-28, `BIN` 기본). `vta/vtalib` 실행 파일(`build`, `build_prof`, `build_timed`)용.
- **systolic, single EVTA:** 가장 최근은 `..._sa_alux3_400mhz_rq.bin`(11-15)이다. 모든 기능(requantize, bias reset, ALU 확장 1~3단계)이 들어 있다.
  - `SYS_BIN` 기본은 2026-11-22부터 이 판이다(그 전 `..._sa_400mhz_rq.bin`, ALU 확장 없음). 옛 판은 `BIN=`으로 준다.
  - `..._sa_amul_400mhz_rq.bin`(11-23): ALU 확장 중 MUL immediate만(`_amul`, avgpool on EVTA). MobileNetV2가 `_alux3`보다 0.26 ms 느리다.
  - `..._sa_amul_bc_400mhz_rq2.bin`(11-24): `_amul` + broadcast vector 연산 II 1(`_amul_bc`). CNN 다섯 모델은 `_alux3`와 같은 시간, LUT 45.0k(`_alux3` 69.8k). DeiT 확장 연산은 없다.
    `rq2`는 고쳐서 다시 합성한 두 번째 판이다. 첫 판 `..._sa_amul_bc_400mhz_rq.bin`은 depthwise가 틀려 쓰지 않는다.
- **systolic, EVTA 4개:** `SYS_BIN` 기본은 2026-11-26부터 `..._16_15_18_17_4evta_sa_amul_dw_400mhz_hp304_rq.bin`(11-25, `_amul` + depthwise 창 장치, HP 포트 304.3 MHz)이다.
  - 그 전 기본(11-24 ~ 25) `..._4evta_sa_amul_400mhz_hp322_rq.bin`(11-23, MUL immediate, HP 포트 321.7 MHz).
  - 그 전 기본 `..._4evta_sa_400mhz_rq.bin`(11-04, ALU 확장 없음, avgpool CPU). per copy MobileNetV2 19.23 → 18.69, Resnet50 78.47 → 77.99 ms.
  - `..._4evta_sa_amul_bc_400mhz_hp282_rq.bin`(11-24): 보드는 통과했지만 CNN 이득이 없고 HP 클럭이 낮아(281.7 MHz) Resnet50이 1.3 ms 느리다. 쓰지 않는다.
  - `..._4evta_sa_amul_dw_400mhz_hp304_rq.bin`(11-25): `_amul` + depthwise 창 장치(`_dw`, HP 304.3 MHz). MobileNetV2Tf 4개 동시 260.1 → 287.4 fps,
    혼자 13.54 → 12.09 ms. 11-26부터 기본(위). 만든 과정과 명령: [evta_dw_unit.md](evta_dw_unit.md) §9.
    multi4 빌드는 이 판부터 `VTA_CONTAIN=1`(`run_flow.sh` / `m4_build.sh` 앞 환경 변수; pblock `CONTAIN_ROUTING`)을 쓴다.
  - single `..._16_16_19_18_sa_alux3_dw_375mhz_rq.bin`(11-25): `_alux3` + `_dw`, 375 MHz. MobileNetV2Tf 1장 13.90 ms.
  - single `..._16_16_19_18_sa_alux3_dw_387mhz_rq.bin`(11-26): `_alux3` + `_dw`(sa_dw 입력 등록 고친 판), 387.5 MHz. MobileNetV2 / Tf 13.05 → 11.7 ms, 다른 모델은
    클럭 때문에 1–3 % 느려 single 기본은 `_alux3` 그대로. [evta_perf_latest.md](evta_perf_latest.md) §3.
  - 이름 조각(`_amul`, `_amul_bc`, `_rq2`, `_hp322` …)의 뜻: [evta_isa_ext.md](evta_isa_ext.md) §5 "bitstream 이름 읽는 법".
- **최종판은 아니다.** 다음 판의 후보(RQ ALU 연산, multi4 ALU 확장, KV260)가 있다.
- **짝이 되는 vtalib:** `_alux3`의 기능은 `vtalib-zcu102-systolic` commit `8191320`(11-15) 뒤로 빌드한 실행 파일만 쓴다. 보드의 `_sys*`는 그 전 판이다(§4.3).
- 판마다 날짜, 클럭, 더한 기능, WNS, 지금 소스와의 관계: [evta_isa_ext.md](evta_isa_ext.md) §5.1.
  판마다 하드웨어 / vtalib / 컴파일러에서 더한 것: §5.2. 판별 성능 표(bitstream만 바꾼 효과, 판마다 소프트웨어가 줄인 시간, `_alux3`에서 한 일): §5.3.

### 2.5 HLS 시험 (bitstream 없이)

| 명령 (PC, `vta-hw-zcu102-systolic/hardware/xilinx`) | 하는 일 | 시간 |
|---|---|---|
| `sim/run_csim.sh [ALUX=1\|0] [SYSTOLIC=1\|0]` | HLS C simulation을 g++로(vivado_hls 없이). 임의 ALU 프로그램(`VTA_ALUX_SEEDS`, 기본 20)을 참조 모델과 비교 | 수 초 ~ 수 분 |
| `sim/run_csim.sh PROG=1 [rows] [seed]` | layer norm / softmax ALU 프로그램을 HLS ALU 위에서, bundle CPU 함수와 비교 | |
| `sim/run_cosim.sh [seeds] [insns]` | `compute()`의 C/RTL co-simulation(vivado_hls, 컨테이너). 결과 `build/hardware/xilinx/cosim_alux/cosim.log` | 수십 분 |
| `rtl/tb/run_xsim.sh <vector dir>` | systolic RTL 단위 시험([evta_systolic_design.md](evta_systolic_design.md)) | |

`run_cosim.sh`의 변형은 이렇다. 자세한 것은 스크립트 머리말에 있다.
- `VTA_ALUX_WINDOW=16`: 16 step 안에서는 같은 자리를 다시 읽지 않는 프로그램만 쓴다.
- `VTA_ALUX_DIST=14,15,16,17,18`: 재사용 거리 d를 하나씩 시험한다.
- `VTA_ALUX_HAZARD_OK=1`: 17-step 규칙을 어기는 프로그램을 일부러 넣는다. 이때는 FAIL이 나와야 맞다.

## 3. bundle 소스 만들기 (시뮬레이터 빌드 `Debug2_remote`)

**무엇을 하나:** 모델(ONNX, Caffe2)을 EVTA에서 도는 C++ 코드로 바꾼다. 이 코드를 **bundle**이라고 한다.
- 순서: `image-classifier`가 이미지 몇 장을 돌려 양자화 범위(profile yaml)를 잰다.
- 그다음 `model-compiler`가 그 profile로 int8 bundle을 만든다. 만드는 파일은 `<모델>.cpp`, `<모델>.weights.bin`, main이다.
- 이 장의 스크립트가 두 도구를 순서대로 불러 준다.
- 결과 `.cpp`는 §4에서 보드 실행 파일로 빌드한다.

PC 컨테이너 `nestc-build`에서 만드는 것을 권한다. 결과는 보드와 바이트까지 같고 훨씬 빠르다(bundle target 하나에 PC 4~7초, 보드 약 2분).
보드에서도 같은 명령이 돈다.

```bash
# PC에서 (컨테이너 밖에서 한 줄로, §1.2 (나))
sudo docker exec -u ubuntu -w /home/jerry/npuprg/nestc2_clion/nest-compiler nestc-build \
    vtabundles_xcmpl/build_sim_bundles.sh -f vtaMxnetResnet18Bundle
# 보드에서
cd ~/JO/npuprg/nestc2_clion/nest-compiler && vtabundles_xcmpl/build_sim_bundles.sh -f vtaMxnetResnet18Bundle
```

출력 예(끝부분):
```
vtaMxnetResnet18Bundle: -f: deleting vta/bundles/Resnet18Test/mxnet_exported_resnet18.cpp (made again)
vtaMxnetResnet18Bundle: make -j2 (log: .../Debug2_remote/bundle_logs/vtaMxnetResnet18Bundle.log)
vtaMxnetResnet18Bundle                   OK     6s
  20 EVTA producers, 9 with a tail
  0 GEMMs (conv / fc / matmul) on the CPU
```

### 3.1 명령

| 무엇 | 명령 (어디서 실행해도 됨) | 출력 |
|---|---|---|
| NEST-C CMake에 있는 bundle (시험 번들, Resnet18 / 50, VGG16) | `vtabundles_xcmpl/build_sim_bundles.sh [-f] [target ...]` | `Debug2_remote/vta/bundles/<Dir>/`, 로그 `Debug2_remote/bundle_logs/<target>.log`, `summary.txt` |
| MobileNetV2 | `vtabundles_xcmpl/gen_mobilenetv2_bundle.sh [model-compiler 옵션]` | `Debug2_remote/vta/bundles/MobileNetV2Test/` (`gen.log`, `prof.log`) |
| DeiT-tiny | `vtabundles_xcmpl/gen_deit_bundle.sh [model-compiler 옵션]` | `Debug2_remote/vta/bundles/DeiTTest/` (`gen.log`) |
| CNN 네 모델을 여러 그림의 profile로 | `vtabundles_xcmpl/gen_calib_bundle.sh <Dir> <그림 목록 \| profile.yaml[.gz]> [model-compiler 옵션]` (`BINS=2048` histogram 칸; `OUT=<dir>` bundle을 그 dir에; DPU 비교 dir `Resnet50TvTest`, `MobileNetV2TfTest`, `MobileNetV2ValTest`는 KL + `-quantization-pow2-mse=2`가 기본) | `Debug2_remote/vta/bundles/<Dir>/` (`prof.log`, `gen.log`). [evta_accuracy.md](evta_accuracy.md) §6, §7 |
| torchvision ResNet-50 ONNX | `~/.venvs/onnxeval/bin/python vtabundles_xcmpl/export_tv_resnet50.py Debug2_remote/vta/bundles/Resnet50TvTest/resnet50.onnx` (torch, torchvision 필요) | Caffe2 resnet50.onnx와 입력 / 출력이 같은 ONNX (`Resnet50TvTest`) |
| TF-slim float MobileNetV2 ONNX | `vta/bundles/MobileNetV2TfTest/CMakeLists.txt` 머리의 curl / `tfl2onnx.py` 두 줄 (tflite, onnx 필요) | `Debug2_remote/vta/bundles/MobileNetV2TfTest/mobilenetv2_tf_f32.onnx` (logits, 1001 클래스) |
| multi4 copy 4개 | `[MODEL=<onnx>] [PROF=<yaml>] [MCX="<옵션>"] vtabundles_xcmpl/gen_multi4_bundles.sh <Dir> ...` (`MultiEVTAResnet18Test`, `MultiEVTAResnet50Test`, `MultiEVTAVgg16Cifar10Test`, `MultiEVTADeiTTest`, `MultiEVTAMobileNetV2Test`, DPU 비교 `MultiEVTAResnet50TvTest`, `MultiEVTAMobileNetV2TfTest`) | `Debug2_remote/vta/multievta_bundles/<Dir>/` (`gen_<i>.log`). single bundle이 먼저 있어야 한다(모델과 profile을 거기서 읽음) |
| model-compiler 다시 빌드 | `Debug2_remote`에서 `make -j16 model-compiler`(PC) / `make -j2 model-compiler`(보드) | VTASave.cpp 등을 바꾼 뒤 |

넣을 것:
- `[target ...]`(build_sim_bundles.sh): 하나 이상. 빼면 전부. 예:
  - 모델: `vtaMxnetResnet18Bundle`, `vtaCaffe2Resnet50Bundle`, `vtaVGG16Cifar10Bundle`, `vtaMxnetResnet18PartitionBundle`
  - 시험 번들: `vtaConvTestBundle`, `vtaConv2TestBundle` ~ `vtaConv5TestBundle`, `vtaResConv1TestBundle` ~ `vtaResConv10TestBundle`, `vtaFCTestBundle`, `vtaMaxPoolTestBundle`, `vtaElemAddTestBundle` 등. 전체 목록은 스크립트 맨 위 `TARGETS=`
- `[model-compiler 옵션]`(gen_*_bundle.sh): §3.3의 옵션. 스크립트의 기본 옵션 뒤에 붙는다. 예: `-vta-alu-softmax=0`, `-vta-fuse-elemadd-mul`
- `<Dir>`(gen_multi4_bundles.sh): `MultiEVTAResnet18Test`, `MultiEVTAResnet50Test`, `MultiEVTAVgg16Cifar10Test`, `MultiEVTADeiTTest`, `MultiEVTAMobileNetV2Test`
- model-compiler `-quantization-pow2-mse=<k>`(NEST-C `lib/Quantization/Quantization.cpp`): 거듭제곱 scale을 s, s/2, …, s/2^k 가운데 histogram 오차가 가장 작은 것으로. `-quantization-calibration=KL`과 함께 쓰고, profile은 `-num-histogram-bins=2048`로 만든다(evta_accuracy.md §7)

예:
```bash
vtabundles_xcmpl/build_sim_bundles.sh -f vtaVGG16Cifar10Bundle vtaCaffe2Resnet50Bundle   # 두 개를 다시
vtabundles_xcmpl/gen_deit_bundle.sh -vta-alu-softmax=0                                  # float softmax로 DeiT
vtabundles_xcmpl/gen_multi4_bundles.sh MultiEVTAResnet18Test                            # multi4 copy 4개
```

**`build_sim_bundles.sh`:**
- target마다 시작할 때 로그 경로를 찍는다. make가 끝날 때까지는 화면이 조용하다. 진행은 `tail -f <로그>`로 본다.
- 끝나면 `OK / FAIL`, 걸린 시간, 융합 요약(§3.4)을 찍는다.
- **`-f`:** 그 target이 만든 번들 `.cpp`를 지우고 다시 만든다.
  - make는 번들이 최신이면 model-compiler를 다시 돌리지 않는다.
  - 그래서 옵션이나 `VTA_FUSION_LOG`만 바꾸고 다시 만들려면 `-f`가 필요하다.
- target을 주지 않으면 시험 번들과 Resnet18 / 50 / VGG16 전부를 만든다(보드에서 수십 분).
- `NESTC_SIM_BUILD=<dir>`로 `Debug2_remote` 대신 다른 빌드 dir을 쓸 수 있다.

**`gen_*_bundle.sh`:**
- MobileNetV2와 DeiT는 NEST-C CMake에 target이 없다. 그래서 make 없이 도구를 직접 부르고, 매번 model-compiler를 돌린다.
  - target을 더하면 Debug2_remote의 cmake가 다시 돌아 Glow 전체를 다시 빌드한다(-j2로 약 1시간).
- DeiT에 필요한 것: `Debug2_remote/vta/bundles/DeiTTest/`에 `deit_fold.onnx`가 있어야 한다.
  - PC에서 `vtabundles_xcmpl/fold_onnx.py model.onnx deit_fold.onnx 1,3,224,224`로 만든다. 보드의 onnx 1.11에는 `onnx.reference`가 없다.
- DeiT의 Glow graph hash는 실행마다 달라진다. 그래서 스크립트가 profile 사본의 hash를 고쳐 가며 model-compiler를 4번까지 돌린다.
- DeiT의 환경 변수:
  - `PRUNE_ROWS=1`(기본): `prune_rows_onnx.py`로 마지막 블록을 출력이 읽는 2행만 계산하게 바꾼다([deit_optimization.md](deit_optimization.md) §6.7).
  - `QSCHEMA=symmetric`(기본).

**주의:**
- **보드에서는 새 build dir을 만들지 않는다.** 메모리가 3.9 GB라 처음부터 빌드하면 메모리가 모자란다. `make -j2`까지만 쓴다.
- cmake가 다시 돌면 Glow 거의 전체가 다시 빌드된다(보드 -j2로 약 1시간). CMakeLists를 바꿨거나 날짜가 바뀐 뒤 첫 빌드일 때 그렇다.
  - 로그에 `Re-running CMake`가 보이면 멈추고 PC에서 하는 것이 낫다.
- Resnet18 conv0을 EVTA에서 돌리려면 `cmake -DVTA_RESNET18_WITH_SKIPQUANT0=OFF .`로 한 번 설정한다(지금 기본).

### 3.2 ONNX 도구 (PC)

| 명령 | 하는 일 |
|---|---|
| `vtabundles_xcmpl/fold_onnx.py in.onnx out.onnx N,C,H,W` | 입력 모양을 고정하고, 알 수 있는 노드를 모두 상수로 접는다(DeiT 1261 → 608 노드). onnx ≥ 1.13 필요 |
| `vtabundles_xcmpl/prune_rows_onnx.py in.onnx out.onnx [in.yaml out.yaml [extra.yaml]]` | 출력이 읽는 행만 계산하게 Slice를 넣는다. yaml을 주면 profile도 맞춰 복사한다. `gen_deit_bundle.sh`가 부른다 |

**무엇을 하나:** model-compiler에 넣기 전에 ONNX 모델을 손본다. 모양이 정해지지 않은 입력, 실행 중에 모양을 계산하는 노드처럼 Glow가 받지 못하는 부분을 미리 없앤다.

넣을 것:
- `in.onnx`, `out.onnx`: 원래 모델과 새로 쓸 파일
- `N,C,H,W`: 입력 모양. 예: `1,3,224,224`(배치 1, RGB, 224 × 224)
- `in.yaml out.yaml`: 원래 모델의 profile(§3 첫 단계 결과)과 새 profile

예:
```bash
python3 vtabundles_xcmpl/fold_onnx.py deit_tiny.onnx deit_fold.onnx 1,3,224,224
```

### 3.3 VTA backend 옵션 (`model-compiler`, `VTA.cpp` / `VTASave.cpp`)

- 표에 "꺼짐"이라고 적은 것만 기본으로 꺼져 있고, 나머지는 켜져 있다.
- `=0`을 주면 그 단계 이전의 경로로 bundle을 만든다. 비교하거나 문제를 가를 때 쓴다.
- 실행 때 줄 것은 없다. 옵션의 결과는 생성된 코드에 들어간다.
- 모델마다 다른 옵션은 그 모델의 생성 규칙에 적혀 있다.
  - MobileNetV2: `vta/bundles/MobileNetV2Test/CMakeLists.txt`, `gen_mobilenetv2_bundle.sh`.
  - DeiT: `gen_deit_bundle.sh`(`-vta-round-shift`).
- 조건이 맞지 않는 층은 옵션이 켜져 있어도 이전 경로(CPU 함수나 따로 부르는 EVTA 호출)로 간다. 채널 수, offset, scale 비, 모양이 그런 조건이다.
- 무엇이 왜 그렇게 됐는지는 §3.4의 요약에 나온다.

생성된 bundle `.cpp` 맨 앞에는 아래 옵션 값이 기본값까지 주석으로 적힌다. 그 뒤에 융합 요약이 이어진다(§3.4).
그래서 bundle을 어떤 옵션으로 만들었는지 `head -40 hw/MobileNetV2Test/*.cpp`로 확인할 수 있다.

| 옵션 | 켜면 하는 일 | 자세히 |
|---|---|---|
| **층 사이 layout** | | |
| `-vta-keep-layout` | 층 사이 activation을 EVTA layout N{C/16}HW16으로 둔다(층마다 transpose 없음) | compare §7.1 |
| `-vta-pack-rows` | DeiT의 행 × C int8 텐서도 EVTA layout으로 | [evta_isa_ext.md](evta_isa_ext.md) §8.16 |
| `-vta-attn-keep-layout` | Q K^T와 A × V를 query / key / value conv 출력에서 바로 읽기(`matmul_evta_qk`, `matmul_evta_av`) | compare §7.27, deit §6.8 |
| `-vta-pad-channels` | 채널 수가 16의 배수가 아닌 conv / add activation을 0 채널로 넓혀 그 conv를 EVTA로(MobileNetV2 24채널) | §7.20 |
| **conv와 꼬리 (융합)** | | [evta_fusion_rules.md](evta_fusion_rules.md) |
| `-vta-fuse-residual` | conv 뒤의 elementadd + relu를 같은 EVTA 호출에서 | §7.2 |
| `-vta-fuse-residual-norelu` | relu 없는 residual add도(MobileNetV2) | isa_ext §8.12 |
| `-vta-conv-epilogue` | conv 뒤 elementwise chain을 epilogue 프로그램으로(`VTASetConvEpilogue`) | [evta_alux_epilogue.md](evta_alux_epilogue.md) |
| `-vta-fuse-pool` | EVTA conv(relu) 뒤의 max pool 2x2/2/0, 3x3/2/1을 conv 호출 안에서(`convolution_wo_tr_pool`) | §7.13, [evta_conv_pool_guide.md](evta_conv_pool_guide.md) |
| `-vta-alu-output-lut` | conv 뒤의 표 구간(GELU), attention `Div` 표를 conv / Q K^T의 출력 LUT로 | isa_ext §8.10, §8.22 |
| `-vta-fuse-embed` | patch embedding의 position-embedding add를 patch conv의 epilogue로 | deit §6.9 |
| `-vta-fuse-elemadd-mul` (**꺼짐**) | 2의 거듭제곱이 아닌 scale 비의 add를 conv epilogue로(MUL / ADD / RQ). 결과는 같지만 `elemadd_evta`가 더 빠름 | isa_ext §8.18 |
| **첫 conv** | | |
| `-vta-fuse-preprocess` | 입력 quantize, 채널별 상수 elemsub/elemdiv, NCHW→NHWC를 conv0 입력 복사 하나로 | §7.9 |
| `-vta-conv0-pack` | 채널이 적은 첫 conv를 packed 입력의 ceil(KH/s)×1 conv로(Resnet18: 7×7/2 → 4×1, 48채널) | §7.12 |
| **EVTA에서 돌릴 연산** | | |
| `-vta-evta-maxpool` | EVTA layout max pool을 EVTA에서(`maxpool_evta`) | §7.3 |
| `-vta-evta-fc` | fc를 EVTA에서. weight와 bias는 compile time에 EVTA layout으로(`fullyconnected_evta_packed`) | §7.8 |
| `-vta-evta-depthwise` | depthwise channelwise conv를 EVTA에서(`dw_convolution_evta`). `-enable-channelwise-depthwise`에만 걸림 | §7.21 |
| `-vta-evta-avgpool` | global avgpool을 EVTA ALU에서(`avgpool_evta`, `_alux`만) | isa_ext §8.12 |
| `-vta-skip-evta-invalidate` | EVTA 출력을 free할 때까지 EVTA 호출만 읽으면 그 출력의 `VTABufferInvalidate`를 뺌(bundle 본문을 줄 단위로 보고 정함; MobileNetV2 0.5 ms) | dpu_compare §6.9 |
| `-vta-evta-elemadd` | packed int8 add를 정수 a A + b B >> N으로 EVTA ALU에서(`elemadd_evta`) | isa_ext §8.17 |
| `-vta-elemadd-precise` (**꺼짐**) | 위를 22비트 곱수로(약 1.8배 ALU 시간) | isa_ext §8.17 |
| `-vta-matmul-conv` | MatMul을 int8로, 상수 오른쪽 MatMul(Linear)을 EVTA 1x1 conv로 | §7.23 |
| `-vta-fold-matmul-bias` | Linear 뒤의 상수 bias Add를 conv bias로 | §7.26 |
| `-vta-evta-matmul` | 오른쪽도 activation인 int8 MatMul을 EVTA에서(`matmul_evta`) | §7.24 |
| `-vta-matmul-heads` | 같은 모양의 연속 matmul(attention head)을 EVTA 호출 하나로 | §7.27 |
| `-vta-alu-layernorm` | 융합한 layer norm을 EVTA ALU에서(`layernorm_evta`) | isa_ext §8.10 |
| `-vta-alu-softmax` | softmax + quantize를 정수 softmax로, ALU 확장이 있으면 EVTA에서. `=0`은 float softmax | deit §6.10 |
| **CPU 쪽** | | |
| `-vta-lut-elementwise` | int8 → int8 elementwise 연산 구간을 256칸 표 하나로 | §7.25 |
| `-vta-fuse-layernorm` | int8 layer norm chain을 함수 하나로 | §7.26 |
| `-vta-cpu-approx` | softmax / layer norm의 나눗셈을 역수 곱으로(값이 조금 바뀔 수 있음) | §7.27 |
| **양자화 / 반올림** | | |
| `-vta-pow2-ratio` | int8 conv / fc / matmul의 출력 scale을 올려 in × w / out을 2의 거듭제곱으로(`-quantization-schema=symmetric`에서) | deit §6.6 |
| `-vta-round-shift` (**꺼짐**) | EVTA conv 출력 shift를 반올림(bias에 2^(shift−1)). 켜면 모든 EVTA conv 출력이 바뀐다. MobileNetV2, DeiT만 켬 | §7.20 |
| `-enable-channelwise-depthwise` (꺼짐, quantizer) | depthwise conv만 채널별 scale로 양자화 | §7.20 |
| **multi-EVTA** | | |
| `-index-multi-evta=<mask>` | bundle을 EVTA 하나에 고정(1, 2, 4, 8 → EVTA 0..3) | README |

"§"만 적은 것은 [evta_runtime_compare.md](evta_runtime_compare.md)의 절이다.

**옵션 하나만 바꿔 번들을 만들 때:**
- 모델의 CMakeLists에 옵션을 붙이면 Debug2_remote의 cmake가 다시 돌아 Glow가 다시 빌드된다.
- 그래서 `gen_*_bundle.sh <옵션>`처럼 model-compiler를 직접 부르거나, 빈 작업 dir에서 부른다.
- 결과를 `hw/<Dir>/`에 복사해 빌드하고, 끝나면 `collect_hw_bundles.py <Dir>`로 되돌린다([resnet18_optimization.md](resnet18_optimization.md) §9.3).

### 3.4 무엇이 EVTA / CPU에서 도나: `VTA_FUSION_LOG`

model-compiler가 bundle을 만들 때 읽는 환경 변수다. 번들 코드는 이 값과 상관없이 같다.

| 값 | 출력 (stderr, 스크립트가 로그로 보냄) |
|---|---|
| 없음 또는 빈 값 (기본) | 요약만 |
| `0` | 아무것도 안 찍음 |
| 그 밖의 값 (`1` 등) | 요약 + 명령마다 자세히(생산자와 꼬리, CPU로 간 conv / fc / matmul과 그 이유) |

요약은 bundle `.cpp` 맨 위 주석(`// EVTA fusion summary`)에도 **늘** 들어간다. 스크립트는 끝에 그것을 화면에 찍는다.

```
  73 EVTA producers, 13 with a tail, 24 read by a tail op not fused
    not fused: 24 x an add: -vta-fuse-elemadd-mul is off (elemadd_evta is faster)
  6 GEMMs (conv / fc / matmul) on the CPU
    on the CPU: 3 x matmul 2 x 198 x 64: 2 rows: no H x W with W >= 7 (largest W 1)
```

```bash
VTA_FUSION_LOG=1 vtabundles_xcmpl/build_sim_bundles.sh -f vtaVGG16Cifar10Bundle   # -f: 최신이어도 다시 만듦
VTA_FUSION_LOG=1 vtabundles_xcmpl/gen_deit_bundle.sh
```

규칙과 읽는 법: [evta_fusion_rules.md](evta_fusion_rules.md), 코드와 그림: [evta_fusion_rules_guide.md](evta_fusion_rules_guide.md).

## 4. 보드용 실행 파일 빌드 (`vtabundles_xcmpl/`)

**무엇을 하나:** §3의 bundle `.cpp`를 vtalib(EVTA runtime 라이브러리)와 묶어 보드에서 도는 실행 파일로 만든다.
- `collect_hw_bundles.py`: `Debug2_remote`의 bundle 소스를 `hw/<Dir>/`로 모은다.
- `make <target>`: 그 소스를 빌드한다. 실행 파일은 `hw/<Dir>/<target><suffix>`에 생긴다.
- build dir마다 같은 bundle을 다른 vtalib나 측정 방식으로 빌드한다(아래 표). 같은 bundle을 여러 build dir에서 빌드해도 된다.

`<Dir>`와 `<target>`의 짝(실행할 때 입력은 `run_all.sh`의 run 줄이 정한다):

| `<Dir>` | `<target>` | 입력 |
|---|---|---|
| `Resnet18Test` | `vtaMxnetResnet18Bundle` | `glow/tests/images/imagenet/cat_285.png` |
| `Resnet18PartitionTest` | `vtaMxnetResnet18PartitionBundle` | 같은 그림 |
| `Resnet50Test` | `vtaCaffe2Resnet50Bundle` | 같은 그림 |
| `Vgg16Cifar10Test` | `vtaVGG16Cifar10Bundle` | `examples/bundles/vgg16_cifar10/cifar10/test/airplane/0001.png` |
| `MobileNetV2Test` | `vtaMobileNetV2Bundle` | `cat_285.png` |
| `DeiTTest` | `vtaDeiTBundle` | `cat_285.png` |
| `Conv3Test` 등 시험 | `vtaConv3TestBundle` 등(`vta<Dir에서 Test 앞>TestBundle`) | `<...>Input.bin`, golden `<...>Golden.bin` |
| `MultiEVTAResnet18Test` (hw_multievtax4/) | `multivtaMxnetResnet18Bundle`, `singlevtaMxnetResnet18Bundle` | |
| `MultiEVTAResnet50Test`, `MultiEVTAVgg16Cifar10Test`, `MultiEVTADeiTTest` | `multivta<모델>Bundle`, `singlevta<모델>Bundle` | |

### 4.1 보드에서

```bash
cd ~/JO/npuprg/nestc2_clion/nest-compiler/vtabundles_xcmpl
./collect_hw_bundles.py [--set multievtax2|multievtax4] [Dir ...]   # Debug2_remote의 소스를 hw[_<set>]/<Dir>/로 복사, run_all.sh 다시 생성
cd build_sysprof && make -j2 vtaMxnetResnet18Bundle                   # 또는 make -j2 hw_bundles (전부)
```

넣을 것:
- `[--set ...]`: 빼면 single EVTA(`hw/`). `multievtax4`면 `vta/multievta_bundles` → `hw_multievtax4/`
- `[Dir ...]`: 모을 `<Dir>`. 빼면 전부. 예: `DeiTTest MobileNetV2Test`
- `build_sysprof`: 빌드할 build dir(아래 표). 예: `build_sys`, `build_systimed`
- `vtaMxnetResnet18Bundle`: 위 표의 `<target>`

예: DeiT를 다시 만든 뒤 systolic 문장별 시간 측정용으로 빌드. 결과는 `hw/DeiTTest/vtaDeiTBundle_systimed`다.
```bash
./collect_hw_bundles.py DeiTTest
cd build_systimed && make -j2 vtaDeiTBundle
```

- `collect_hw_bundles.py`는 `hw/script/run_all.sh`를 다시 만든다.
  - git의 파일과 달라지면 보드에서 `git stash` 후 pull한다(보드에서는 `git checkout`으로 되돌리지 않는다).
- MobileNetV2 / DeiT 등 처음 더한 target은 그 build dir에서 `cmake .`를 한 번 한다.

build dir마다 cmake 옵션이 정해져 있다. 처음 한 번만 `mkdir <dir> && cd <dir> && cmake <옵션> ..`으로 만든다(git에 없음).

| build dir | cmake 옵션 | 실행 파일 끝 | 무엇을 재나 |
|---|---|---|---|
| `build/` | (없음) | — | 기본 HLS bitstream, golden 확인 |
| `build_prof/` | `-DNESTC_EVTA_PROFILE=ON -DBUNDLE_EXE_SUFFIX=_prof` | `_prof` | EVTA run마다 시간과 GOPS |
| `build_timed/` | `-DBUNDLE_TIMED=ON -DBUNDLE_EXE_SUFFIX=_timed` | `_timed` | bundle 문장마다 wall time(`timed_bundle.py`가 .cpp를 감쌈) |
| `build_sys/` | `-DVTALIB=<root>/vtalib-zcu102-systolic -DBUNDLE_EXE_SUFFIX=_sys` | `_sys` | systolic bitstream, golden / 정확도 |
| `build_sysprof/` | 위 `_prof` + systolic vtalib, 끝 `_sysprof` | `_sysprof` | systolic bitstream에서 run마다 |
| `build_systimed/` | 위 `_timed` + systolic vtalib, 끝 `_systimed` | `_systimed` | systolic bitstream에서 문장마다 |
| `build_multievtax2[_prof\|_timed]/` | `-DBUNDLE_SET=multievtax2` (+ 위 옵션) | | EVTA 2개 |
| `build_multievtax4[_prof\|_timed\|_sys\|_sysprof\|_systimed]/` | `-DBUNDLE_SET=multievtax4 -DNESTC_EVTA_MULTI=ON -DNESTC_EVTA_MAX_XFER=4194304` (+ 위 옵션) | | EVTA 4개 |

- `BUNDLE_SET`은 buffer 크기와 queue 크기를 정한다(single 16/16/19/18, multi 16/15/18/17).
- `VTALIB`는 vtalib 소스 dir이다. 기본은 `vta/vtalib`이고, systolic bitstream에는 `vtalib-zcu102-systolic`을 쓴다.
- vtalib의 헤더(`cmd_queue.h` 등)만 바꾸면 의존성이 잡히지 않을 수 있다. 결과가 이상하면 그 build dir의 vtalib을 다시 빌드한다.

### 4.2 PC에서 cross build

**무엇을 하나:** 보드용(aarch64) 실행 파일을 PC에서 빌드한다. 보드의 `make -j2`보다 훨씬 빠르다. 결과는 보드에서 빌드한 것과 기계어까지 같다.
먼저 §1의 `sync_sysroot.sh`로 보드의 헤더와 라이브러리를 PC에 복사해 둔다.

**build dir과 보드에 보내는 것:**
- `vtabundles_xcmpl/build*/`는 git 밖이다. 그래서 보드에 있는 `build`, `build_sys`, `build_sysprof`, … 는 PC에 없고, PC의 `build_pc_systimed`는 보드에 없다. 각 기계가 자기 build dir을 따로 만든다.
- **build dir은 옮기지 않는다.**
  - `CMakeCache.txt`에 그 기계의 절대 경로와 컴파일러(PC는 toolchain file)가 적혀 있어서, 다른 기계에서는 쓸 수 없다.
  - 안의 object 파일도 실행에는 필요 없다.
- **보드에 보내는 것은 `hw/<Dir>/`의 실행 파일 하나와, bundle을 다시 만들었으면 `<모델>.weights.bin`이다.**
  - vtalib는 실행 파일 안에 정적으로 들어간다. 나머지 라이브러리(libpng, glog, libcma, libgomp 등)는 보드에 이미 있는 것을 쓴다(`readelf -d`의 NEEDED).
  - 실행 파일은 build dir이 아니라 `hw/<Dir>/<target><suffix>`에 생긴다.
- **이름 끝은 `_pcsystimed`처럼 보드와 다르게 한다.** PC 실행 파일이 보드에서 빌드한 `_systimed`를 덮지 않게 하려는 것이다.
  - 지금 PC의 `build_pc_systimed`도 `-DBUNDLE_EXE_SUFFIX=_pcsystimed`다.
  - 판을 여럿 비교할 때는 `_pcsys2`, `_pcsys3`, … 처럼 번호를 붙였다(`docs/evta_isa_ext.md` §8.15).
- **이름은 `_pcsys`로 시작하게 한다.** `run_prof.sh`는 `-s`가 `_sys` 또는 `_pcsys`로 시작하면 systolic bitstream(`SYS_BIN`)을 올린다(2026-11-21부터).
  - 그래서 `BIN=`을 따로 주지 않아도 된다. 다른 systolic bitstream으로 돌릴 때만 `BIN=<file>`을 준다.
  - 그 전에는 `_pcsys…`에 HLS bitstream(`BIN`)을 올렸다. 그 위에서 systolic vtalib가 돌면 결과가 틀린다.
  - 시간 표(`_timed` 계열)는 이름과 상관없이 정리된다.

```bash
# PC, nestc-build 컨테이너 안 (처음 한 번: build dir 만들기)
cd <tree>/vtabundles_xcmpl && mkdir build_pc_systimed && cd build_pc_systimed
cmake -DCMAKE_TOOLCHAIN_FILE=<tree>/utils/docker_nestc/zcu102_toolchain.cmake \
      -DBUNDLE_TIMED=ON -DBUNDLE_EXE_SUFFIX=_pcsystimed -DVTALIB=$PWD/../../vtalib-zcu102-systolic ..
# 그다음부터 (bundle은 §3, §4.1의 collect_hw_bundles.py로 hw/<Dir>/에 모아 둔 뒤)
make -j16 vtaMobileNetV2Bundle                                   # -> ../hw/MobileNetV2Test/vtaMobileNetV2Bundle_pcsystimed
exit
# PC 셸
cd <tree>/vtabundles_xcmpl
scp hw/MobileNetV2Test/vtaMobileNetV2Bundle_pcsystimed hw/MobileNetV2Test/mobilenetv2.weights.bin \
    xilinx@192.168.20.2:JO/npuprg/nestc2_clion/nest-compiler/vtabundles_xcmpl/hw/MobileNetV2Test/
# 보드, vtabundles_xcmpl/
sudo /bin/sh -c "hw/script/run_prof.sh -s _pcsystimed -r MobileNetV2Test"      # SYS_BIN을 올린다
```

넣을 것:
- `<tree>`: §0 표의 PC 경로
- `_pcsystimed`: `_pcsys`로 시작하고 보드 실행 파일과 겹치지 않는 이름. 측정 방식은 `-DBUNDLE_TIMED=ON`(문장마다 시간) 또는 `-DNESTC_EVTA_PROFILE=ON`(EVTA 호출마다 시간)이 정한다. 예: `_pcsysprof`
- `BIN=<file>`(빼도 됨): `SYS_BIN`이 아닌 다른 systolic bitstream으로 돌릴 때. 이름에 `_sa_`가 든 것(§5.2). 예: `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_alux3_400mhz_rq.bin`

- toolchain file을 바꾸면 **새 build dir**에서 configure한다. `cmake .`는 처음의 `*_INIT` 플래그를 그대로 쓴다.
- 맞춘 내용(libgomp, hash style 등)은 [host_pc_build.md](host_pc_build.md) §4에 있다.

### 4.3 vtalib 판과 지금까지 빌드한 실행 파일 (2026-11-21에 정리)

**vtalib 소스(두 벌)**

| 소스 | 쓰는 bitstream | 빌드하는 build dir | 비고 |
|---|---|---|---|
| `vta/vtalib` (submodule) | HLS GEMM(`BIN`, 이름에 `_sa_` 없음) | `build`, `build_prof`, `build_timed`, `build_multievtax2*`, `build_multievtax4`, `_prof`, `_timed` | 원래 EVTA runtime. 공통 수정만 들어간다 |
| `vtalib-zcu102-systolic` | systolic(`SYS_BIN`, 이름에 `_sa_`) | `build_sys*`, `build_multievtax4_sys*`, PC `build_pc_systimed` | uop 순서가 systolic GEMM용이다. ALU 확장, epilogue, LOAD_AHEAD, layer norm / softmax / elemadd / matmul 등 10월 말 이후 기능은 여기에만 있다 |
| (지움) `vtalib-zcu102-single`, `-multi2`, `-multi4` | | | 2026-10-29에 지웠다. 지금은 `BUNDLE_SET`(buffer, queue 크기)으로 나눈다 |

- **vtalib의 "판"은 따로 번호가 없다. git commit이 판이다.** 같은 build dir에서 다시 빌드하면 실행 파일이 덮인다.
- 어느 vtalib로 빌드한 실행 파일인지는 이름 끝(`<suffix>`)으로 안다.

**보드의 build dir (`vtabundles_xcmpl/`, git 밖, 보드에서 빌드)**

| build dir | vtalib | `BUNDLE_SET` | 측정 | 실행 파일 끝 |
|---|---|---|---|---|
| `build` | `vta/vtalib` | single | 없음 | (없음) |
| `build_prof` / `build_timed` | `vta/vtalib` | single | EVTA 호출마다 / 문장마다 | `_prof` / `_timed` |
| `build_sys` / `build_sysprof` / `build_systimed` | systolic | single | 없음 / 호출마다 / 문장마다 | `_sys` / `_sysprof` / `_systimed` |
| `build_multievtax2`, `_prof`, `_timed` | `vta/vtalib` | multievtax2 (`MAX_XFER` 8 MB) | 위와 같음 | |
| `build_multievtax4`, `_prof`, `_timed` | `vta/vtalib` | multievtax4 (`MAX_XFER` 4 MB) | 위와 같음 | |
| `build_multievtax4_sys`, `_sysprof`, `_systimed` | systolic | multievtax4 | 위와 같음 | `_sys` 등 |

**PC의 build dir:** `build_pc_systimed` 하나(systolic vtalib, single, 문장마다 시간, 끝 `_pcsystimed`).

**PC에서 빌드해 보드에 보낸 실행 파일 (`hw/<Dir>/<target>_pc…`)**
- 실험할 때마다 `_pcsystimed`를 빌드했다. 그것을 `_pcsys<번호>`로 이름을 바꿔 보드에 보냈다. 이전 판과 나란히 두고 A/B 비교를 하려는 것이다.
- 번호는 만든 순서다. 뜻은 보드 파일의 시각을 commit 시각과 맞춰 적었다. 문서에 이름이 나오지 않는 것은 **(추정)** 이라고 적었다.
- 모델 칸이 "5개"면 DeiT, MobileNetV2, Resnet18, Resnet50, VGG16이다.
- 이 파일들은 git 밖이고 보드에만 있다. 비교가 끝난 판은 지워도 된다. 다시 필요하면 그 commit에서 다시 빌드한다.

| 이름 끝 | 보드에 놓은 때 | 모델 | 무엇을 비교하려고 | 자세히 |
|---|---|---|---|---|
| `_pcsystimed` | 11-14 ~ | 5개 | PC build dir의 최신 출력. 빌드할 때마다 덮인다 | §4.2 |
| `_pclntime` | 11-15 00:43 | DeiT | `layernorm_evta` replay + NEON 입력, `EVTA_LN_TIME` 시간 분해 | isa_ext §8.13 끝 |
| `_pcsys2` | 11-15 01:14 | 5개 | `_alux2` vtalib(결합 layer norm 프로그램). softmax는 CPU float | isa_ext §8.14, §8.15 표 |
| `_pcsys3` | 11-15 08:40 | 5개 | `-vta-alu-softmax`, EVTA softmax(int32 입력) | isa_ext §8.15 |
| `_pcsys4` | 11-15 08:45 | 5개 | + int8 입력 경로(`AluInp`), `_alux3` vtalib | isa_ext §8.15 |
| `_pcsys5` | 11-15 10:18 | DeiT | `-vta-pack-rows`(행 텐서를 EVTA 배치로). elemadd는 CPU float | isa_ext §8.16, §8.17 |
| `_pcsys6` | 11-15 12:29 | DeiT | `-vta-evta-elemadd` 15비트 정수 | isa_ext §8.17 |
| `_pcsys7` | 11-15 12:38 | DeiT | `-vta-elemadd-precise` 22비트 | isa_ext §8.17 |
| `_pcsys8` | 11-15 13:34 | DeiT | elemadd_evta 기본(결과는 `_pcsys6`와 같다) | isa_ext §8.17, §8.20 |
| `_pcsys9` | 11-15 16:43 | DeiT | `-vta-fuse-elemadd-mul` 첫 판 **(추정)** | isa_ext §8.18 |
| `_pcsys10` | 11-15 16:52 | DeiT | `-vta-fuse-elemadd-mul`: epilogue, residual을 acc 위쪽에 | isa_ext §8.18 |
| `_pcsys11` | 11-15 17:50 | DeiT, MobileNetV2, Resnet18 / 50 | `EVTA_RES_PREFETCH` **(추정)** | isa_ext §8.19 |
| `_pcsys12` | 11-15 21:20 | DeiT | fused add + `EVTA_TIME_NO_CONV_CLIP` 시간 측정 | isa_ext §8.20 |
| `_pcsys13`, `_pcsys14` | 11-16 01:00 | DeiT | LOAD_AHEAD 전의 기준: 13 = 그때까지의 실행 파일, 14 = 새 vtalib + 기본 번들 **(추정)** | isa_ext §8.21 |
| `_pcsys15`, `_pcsys16`, `_pcsys17` | 11-16 01:17 | 16은 5개, 15·17은 DeiT | `EVTA_LOAD_AHEAD`. 17은 fused add 없이 EVTA softmax인 DeiT **(15는 추정)** | isa_ext §8.21 |
| `_pcsys18` | 11-16 10:26 | 5개 | Q K^T 출력 LUT(vtalib: epilogue가 conv batch 전체에 적용) **(추정)** | isa_ext §8.22 |
| `_pcsys19` ~ `_pcsys21` | 11-16 11:29 ~ 11:44 | DeiT | 마지막 블록 2행만 계산(`PRUNE_ROWS`): 19 첫 판, 20 mixed layout add 수정, 21 반올림 수정 | deit §6.7 |
| `_pcsys22`, `_pcsys22r` | 11-16 11:49 | DeiT | 2행 판과 전체 행 판을 `EVAL_HASH`로 비교(어느 쪽이 `r`인지는 **추정**: `r` = 전체 행 기준) | deit §6.7 |
| `_pcsys23` | 11-16 19:41 | DeiT | A x V에서 V와 출력을 EVTA 배치로 | deit §6.8 |
| `_pcsys24` | 11-16 22:54 | DeiT | patch embedding add를 patch conv의 epilogue로 | deit §6.9 |
| `_pcsys25` | 11-17 00:34 | DeiT | packed 텐서의 행 slice 복사 **(추정)** | deit §6.10 |
| `_pcsys26` | 11-17 18:51 | Resnet18 / 50, VGG16 | conv + 2x2 max pool 한 호출 | [vgg16_optimization.md](vgg16_optimization.md) §3.7 |
| `_pcsys27` | 11-21 23:45 | 5개 | 11-21의 최신 소스 전부(vtalib `186e53d`). `_rq`와 `_alux3`에서 나란히 잼 | isa_ext §5.3 표 D |

- `isa_ext`는 [evta_isa_ext.md](evta_isa_ext.md), `deit`는 [deit_optimization.md](deit_optimization.md)다.
- 지금 기본으로 쓰는 것은 보드 build dir의 `_sys*`, `_sysprof`, `_systimed`와 PC의 `_pcsystimed`다. 번호 판은 비교 기록용이다.
- 보드의 `build_systimed`(single)와 `build_multievtax4_systimed`(multi4)는 11-23 / 11-24에 `vtalib-zcu102-systolic` `398f358`로 다시 빌드했다(모델 실행 파일 전부).
  그 전(11-14 ~ 11-22)에는 보드의 `_sys*`가 옛 vtalib라 `_alux*` 기능을 쓰지 않았고, 그때 측정은 PC의 `_pcsys27`(11-21)로 했다.
  build dir별로 지금 어느 vtalib인지는 §4.4의 빌드 흔적으로 본다. `_sysprof`, `_sys` 등 다른 build dir은 다시 빌드하지 않았다.
  bitstream과 vtalib commit의 짝, 판마다 더한 것과 성능 표: [evta_isa_ext.md](evta_isa_ext.md) §5, §5.2, §5.3.
- bitstream 판은 [evta_isa_ext.md](evta_isa_ext.md) §5.1에 있다. 실행 파일은 vtalib가 bitstream 이름을 보고 기능을 고르므로, 같은 실행 파일이 여러 bitstream에서 돈다.

### 4.4 빌드 흔적: 어느 commit으로 만든 bundle / 실행 파일인가 (2026-11-24)

빌드한 것은 git에 없어서, 다른 PC가 만든 것이나 오래된 것을 구별하기 어려웠다(11-24: 보드 model-compiler가 11-22 수정 전이었고,
multi4 Resnet18 / VGG16 bundle이 11-06 것이었다). 이제 파일 자체에 남긴다.

| 무엇 | 어디에 | 내용 | 만드는 곳 |
|---|---|---|---|
| bundle `.cpp` | 둘째 줄(옵션 목록 다음) `// model-compiler: ...` | NEST-C 컴파일러 소스(`lib include tools cmake`)의 마지막 commit과 날짜, 커밋 안 된 수정 여부, model-compiler binary를 만든 날짜와 host. binary가 그 commit보다 오래됐으면 `WARNING: ... rebuild model-compiler` | `VTASave.cpp` `compilerStamp` (생성할 때 `git -C <소스 트리>`) |
| 실행 파일 | 문자열 `NESTC_BUILD vtalib=<dir> <commit> <날짜>[ dirty] host=<host> dir=<build dir>` | 링크한 vtalib dir의 마지막 commit, 커밋 안 된 수정 여부, 빌드한 host와 build dir | `vtabundles_xcmpl/build_stamp.cmake` (make마다 돌고, 내용이 바뀔 때만 다시 써서 relink) |

```bash
head -40 hw/Resnet18Test/mxnet_exported_resnet18.cpp | grep '^// model-compiler:'
grep -a -o 'NESTC_BUILD [ -~]*' hw/Resnet18Test/vtaMxnetResnet18Bundle_systimed | head -1
cd vtabundles_xcmpl/hw && bash -c '. ../bininfo.sh; exe_info Resnet18Test/vtaMxnetResnet18Bundle_systimed <bitstream 이름>'
```

- `run_prof.sh`는 실행 파일마다 `exe :`(만든 날짜, 흔적), `bundle :`(Dir의 bundle `.cpp`의 model-compiler 줄)을 찍고, summary에도 남긴다.
- **경고:** bitstream 이름의 기능을 vtalib가 쓰기 시작한 commit(§5의 짝 표)보다 실행 파일의 vtalib가 오래됐으면 `WARNING : vtalib <commit> predates ...`를 찍는다.
  그 실행 파일은 돌지만 그 기능을 쓰지 않는다. 검사하는 commit: `_sa_` 3982d46, bias / `_rq` 76aa60e, `_rq` d26c3e3, `_alux` e1a7924, `_alux2` c50e968, `_alux3` 8191320, `_amul` 398f358.
  새 기능을 vtalib에 넣으면 `bininfo.sh` `exe_info`에 한 줄 더한다.
- 흔적이 없는 실행 파일은 11-24 전에 링크한 것이고, `// model-compiler:` 줄이 없는 bundle은 11-24 전에 만든 것이다.
- bundle 두 개를 바이트 비교할 때는 이 줄을 빼고 비교한다(binary를 만든 날짜가 들어 있다):
  `diff <(grep -v '^// model-compiler:' a.cpp) <(grep -v '^// model-compiler:' b.cpp)`.
- PC에서 cross build한 실행 파일도 흔적이 같이 간다(scp한 파일 안에 있다). 흔적의 commit을 보드에서 `git log -1 <commit>`으로 찾으려면 그 PC가 push해 두어야 한다.

## 5. 실행과 측정 (보드, sudo)

**무엇을 하나:** §4의 실행 파일을 보드의 EVTA에서 돌린다.
- `run_all.sh`: 결과가 맞는지 본다. 출력을 golden(정답 파일)과 비교하고 PASS / MISMATCH를 낸다.
- `run_prof.sh`: 몇 번 돌려 시간을 잰다.
- 둘 다 실행 전에 bitstream을 다시 올리고, 한 번에 하나만 돌도록 lock을 잡는다.
- 실행 파일을 직접 부를 수도 있지만, bitstream 올리기와 lock 때문에 스크립트로 돌린다.

### 5.1 실행 전에

- **다른 사용자의 EVTA 실행을 확인한다:** `ps aux | grep -E "[r]un_prof|[r]un_all|Bundle"`.
  - run 스크립트는 `/tmp/evta_run.lock`으로 한 번에 하나만 돈다.
  - 다른 사람의 시간 측정 중에는 빌드나 실행으로 끼어들지 않는다.
- **sudo는 `sudo -n /bin/sh -c "..."` 형태로** 쓴다. `sudo VAR=... cmd`는 비밀번호를 묻는다.
- **실행마다 bitstream을 다시 올린다.** 멈춘 run이 있으면 EVTA가 deadlock 상태로 남기 때문이다.
  - run 스크립트는 자동으로 올린다. 손으로는 `sudo vtabundles_xcmpl/hw/script/load_bit.sh <bin>`이다.
  - 올린 이름은 `/tmp/evta_bitstream`에 남고, vtalib이 그 이름으로 기능을 고른다.
- **CMA:** 비정상 종료한 프로세스의 CMA(128 MB 중)는 `xlnk_reset` 전까지 돌아오지 않는다.
  - 손으로 할 때: `sudo -n /bin/sh -c "cd /tmp && python3 -c 'from pynq import Xlnk; Xlnk().xlnk_reset()'"`.
  - **다른 사람이 EVTA를 쓰는 중에는 하지 않는다.** 그 사람의 CMA도 지운다.
  - 남은 양은 `grep CmaFree /proc/meminfo`로 본다.
- **root 디스크가 100 %가 되면 run이 조용히 끝난다.** 로그가 잘리고 rc 3이 된다. dump는 쓴 뒤에 지운다.
- ssh로 오래 걸리는 일을 걸 때는 `nohup <cmd> > <log> 2>&1 < /dev/null &`를 쓴다. `< /dev/null`이 없으면 ssh가 끝나지 않는다.

### 5.2 golden 확인: `run_all.sh`

```bash
cd ~/JO/npuprg/nestc2_clion/nest-compiler/vtabundles_xcmpl
sudo hw/script/run_all.sh [Dir ...]                     # single, build/의 실행 파일, 결과 hw/result/results.txt
sudo hw_multievtax4/script/run_all.sh                    # multi4 (multi2도 같은 형태)
sudo /bin/sh -c "BIN=<file in /lib/firmware> hw/script/run_all.sh Resnet18Test"   # 다른 bitstream으로
```

넣을 것:
- `[Dir ...]`: §4의 `<Dir>`. 빼면 run 줄 전부(시험 번들 포함, 수 분). 이름이 틀리면 비슷한 이름을 알려 주고 아무것도 돌리지 않는다.
- `<file in /lib/firmware>`: `<bin>`. 보드에서 `ls /lib/firmware | grep vtahw`로 본다. 예:
  - `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_380mhz.bin`: HLS GEMM 380 MHz(`BIN` 기본)
  - `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_400mhz_rq.bin`: systolic 400 MHz, requantize(11-21까지 `SYS_BIN` 기본)
  - `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_alux3_400mhz_rq.bin`: 위 + ALU 확장 3단계(DeiT layer norm / softmax on EVTA). 11-22부터 `SYS_BIN` 기본
  - `vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta_sa_400mhz_rq.bin`: EVTA 4개(hw_multievtax4/)

출력 예: `hw/result/results.txt`에 Dir마다 한 줄씩 남는다.
```
Resnet18Test                   vtaMxnetResnet18Bundle             RAN (no golden; see Resnet18Test/run.log)
Conv3Test                      vtaConv3TestBundle                 PASS
```
모델(golden 없음)은 `hw/<Dir>/run.log`의 끝에서 분류 결과를 본다. 예: Resnet18 `Result: 281`(cat_285.png), `Inference time: 29.925ms`.

- 기본 bitstream은 `run_all.sh` 맨 위의 `BIN`(HLS 380 MHz)이다. `SYS_BIN`(systolic 400 MHz)은 `run_prof.sh -s _sys*`가 쓴다.
- `OMP_NUM_THREADS` 기본값은 1이다.

### 5.3 측정: `run_prof.sh`

```bash
sudo hw/script/run_prof.sh [-s <suffix>] [-n <runs>] [-l] [-r] [-t <threads>] Dir [Dir ...]
# 예
sudo /bin/sh -c "hw/script/run_prof.sh -s _sysprof -n 25 -r Resnet18Test"
sudo /bin/sh -c "hw/script/run_prof.sh -s _systimed -r -t 4 Resnet18Test"
sudo /bin/sh -c "hw_multievtax4/script/run_prof.sh -s _sysprof -n 25 -l -r MultiEVTAResnet18Test"
```

| 옵션 | 뜻 |
|---|---|
| `-s _prof` (기본) / `_timed` / `_sysprof` / `_systimed` / `_sys` / `_pcsys…` | 어느 build dir의 실행 파일인지. `_sys*`와 PC cross build `_pcsys*`(§4.2)는 `SYS_BIN`(systolic)을 올린다 |
| `-n <runs>` | 추론 횟수(기본 25). multi는 copy당. `hw/`의 범용 main 실행 파일(`MULTI_SINGLE`: MobileNetV2, DeiT, 11-26부터 systolic vtalib의 Resnet18 / 50 / VGG16)은 timed / prof 빌드가 아니면 4 × n장(`MULTI_RUNS`; 11-26 전에는 `_sys`에서 1장만 쟀다) |
| `-l` | multi-EVTA lockstep: 모든 EVTA 호출 앞에서 copy들이 barrier로 만나 같은 conv를 동시에 돈다 |
| `-r` | `chrt -f 10`(SCHED_FIFO). 4 ms scheduler tick 때문에 흔들리는 run이 없어진다. 시간 측정에는 붙인다 |
| `-t <n>` | `OMP_NUM_THREADS`(기본 1). `-t 4`면 4코어 |
| 환경 `BIN=<file>` | bitstream 바꾸기 |
| `-h`, `--help` | 옵션 설명을 출력하고 끝난다(sudo 없이 된다). `run_all.sh --help`, `load_bit.sh --help`도 있다 |

**모델 7개의 최신 시간을 한 번에(`perf_record.sh`, 2026-11-26):** int8 입력 만들기, `EVTA_RES_CHECK`, multi4와 single을 bitstream을 번갈아 가며 3번씩.
결과와 방법은 [evta_perf_latest.md](evta_perf_latest.md).
```bash
sudo /bin/sh -c "vtabundles_xcmpl/perf_record.sh -r 3 [i8] [check] [m4] [single]"     # 기본 bitstream = 각 run_all.sh의 SYS_BIN
sudo /bin/sh -c "M4_BITS='<a>.bin <b>.bin' S1_BITS='<c>.bin <d>.bin' vtabundles_xcmpl/perf_record.sh -r 3 m4 single"
```
- 다른 사용자의 EVTA 실행이 `/tmp/evta_run.lock`을 잡고 있으면 30초마다 다시 시도한다(`LOCK_WAIT=<초>`, 기본 1800). 그때까지 못 잡으면 `ERROR: ... not run`.
  시간은 그 실행이 새로 쓴 로그에서만 읽는다(없으면 `no new log`). 11-26 오후까지는 거절된 실행 뒤에 남아 있던 옛 로그를 읽었다.

**기능 하나씩 켜기 / 끄기(`ablation_mnv2.sh`, MobileNetV2Tf, EVTA 1개, 2026-11-27):** 컴파일러 옵션을 바꾼 bundle 18개(`hw/AblMnv2_<tag>`),
런타임 변수, bitstream을 구성마다 바꿔 목록 전체를 R바퀴 번갈아 잰다. 결과와 해석은 [mobilenetv2_evta_paper.md](mobilenetv2_evta_paper.md).
```bash
cd vtabundles_xcmpl
./ablation_mnv2.sh gen [<tag> ...]      # c0 (모두 끔) .. c8 (= 지금 bundle), all, no_<옵션>; Debug2_remote의 model-compiler, 하나 18 s
./ablation_mnv2.sh build                # build_sysabl (systolic vtalib, -s _sysabl), build_abl (vta/vtalib, -s _abl)
sudo /bin/sh -c "R=3 ./ablation_mnv2.sh run ladder|loo|hls"   # 하나씩 켜기 / 하나씩 끄기 / HLS bitstream; OUT=<log dir>, N=<장 수>
./ablation_mnv2.sh list ladder          # 구성마다 bundle / bitstream / 변수
./ablation_mnv2.sh opts c5              # 그 bundle의 model-compiler 옵션
```
- `gen_calib_bundle.sh`의 `OUT=<dir>`(bundle을 그 dir에)와 `run_prof.sh`의 `RUN_LIST=<file>`(run 줄을 그 파일에서; `BIN=`을 함께 준다)을 쓴다.

넣을 것:
- `<suffix>`: 실행 파일을 빌드한 build dir(§4.1 표). systolic bitstream이면 `_sysprof`(EVTA 호출마다 시간) 또는 `_systimed`(문장마다 시간)
- `<runs>`: 추론 횟수. 예: `25`, 빨리 볼 때 `5`
- `<threads>`: CPU 코어 수. `1` 또는 `4`
- `Dir`: §4의 `<Dir>`. 여러 개 가능

예: ALU 확장 bitstream에서 DeiT를 4코어로 문장별 시간 측정.
```bash
sudo /bin/sh -c "BIN=vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_alux3_400mhz_rq.bin hw/script/run_prof.sh -s _systimed -r -t 4 DeiTTest"
```
끝에 `hw/result/_systimed_summary.txt`의 줄과, 로그 `hw/DeiTTest/systimed.log`의 요약(추론마다 ms, 중앙값)이 나온다.

- **실행 시작 때 출력:** 어느 bitstream으로 도는지와 그 설정을 먼저 출력하고, summary 파일 맨 앞에도 넣는다(`vtabundles_xcmpl/bininfo.sh`). 나오는 항목은 다음과 같다.
  - bitstream: 파일 이름, 어디서 왔는지(`BIN` / `SYS_BIN` / 환경 `BIN=`), 날짜, md5 앞 8자리.
  - 이름에서 읽은 것: EVTA 수, clock, GEMM 종류(HLS / systolic), 기능(ALU imm II = 1, bias reset, requantize, ALU 확장, AXI perf monitor).
  - 실행 설정: `EVTA_*` / `MULTI_*` / `BUNDLE_*` 환경 변수, `-s`의 build dir과 무엇을 재는지, 횟수, lockstep, `chrt`, `OMP_NUM_THREADS`, Dir 목록.
  - 경고: `-s`와 bitstream이 맞지 않으면 `WARNING`을 출력한다.
- **결과:**
  - 로그는 `hw/<Dir>/<suffix에서 앞의 _를 뺀 이름>.log`다. 예: `-s _systimed` → `hw/DeiTTest/systimed.log`. 한 Dir에 실행 파일이 여럿이면 `systimed_<실행 파일>.log`.
  - `_prof` 로그에는 `prof_summary.py` 요약(`<log>.sum`, conv마다 시간과 GOPS)이 붙는다.
  - `_timed` 로그에는 `run_times.py` 요약(추론마다 시간, 중앙값, GOPS)이 붙고, 문장별 시간은 `timing.csv`에 남는다.
  - Dir마다 한 줄씩 `hw/result/<suffix>_summary.txt`에 남는다(exit, golden, 전체 시간).
  - `_timed` / `_systimed`의 요약 줄(2026-11-22부터):
    - 1장의 연산량과 fps: `whole run median 22.537 ms, 164.19 GOPS (3.700 GOP), 44.4 fps`
      - fps = 1000 / 2–25회 중앙값(ms)이다. 1초 동안 돌릴 필요는 없다. 첫 회는 준비 시간이 섞여 빼고 잰다.
      - copy k개를 동시에 돌리면 fps는 k × 1000 / 가장 느린 copy의 시간이다.
      - GOPS = GOP × fps다.
    - 그 실행의 분류: `top-1 281 (0.655530)`
    - 데이터셋 정확도 기록: `script/accuracy.txt`의 그 Dir 줄(§6). 그림 한 장으로는 정확도를 잴 수 없어, 따로 잰 값을 보여 준다.
- multi-EVTA 실행 파일은 copy마다 표를 따로 출력한다.
  - `multi4conv4Bundle <mask> [iters] [conv]` 같은 GOPS 측정은 [vtabundles_xcmpl/README.md](../vtabundles_xcmpl/README.md)를 본다.

## 6. 정확도 (ImageNetV2 / CIFAR-10, `vtabundles_xcmpl/imagenet_eval/`)

**무엇을 하나:** 그림 한 장의 분류(§5)가 아니라 ImageNetV2 수천 장의 top-1 정확도를 잰다.
- 양자화나 반올림을 바꾸는 단계(`-vta-round-shift`, 정수 softmax 등)가 정확도를 얼마나 떨어뜨리는지 본다.
- 보드 결과는 PC의 float 기준(`float_ref.py`, CNN은 `float_ref_cnn.py`)과 비교한다.
- 지금의 기록(다섯 모델, 2026-11-22)과 잰 방법: [evta_accuracy.md](evta_accuracy.md). Dir마다의 값은 `hw/script/accuracy.txt`에 두고 `run_prof.sh`가 보여 준다.
- 실행 파일: DeiT / MobileNetV2는 `vtaDeiTBundle_sys` / `vtaMobileNetV2Bundle_sys`. Resnet18 / Resnet50 / VGG16은 eval 실행 파일
  `vtaMxnetResnet18Eval_sys` / `vtaCaffe2Resnet50Eval_sys` / `vtaVGG16Cifar10Eval_sys`(공통 main, 입력은 원래 main과 같음; `build_sys`에서 `make <이름>`).

넣을 것:
- `<exe>`: `vtabundles_xcmpl/`에서 본 실행 파일 경로. 실행 파일은 `hw/<Dir>/`에 있다. 예: `hw/DeiTTest/vtaDeiTBundle_sys`, `hw/DeiTTest/vtaDeiTBundle_sysprof`
- `<name>`: 결과 로그 이름(아무 이름). 예: `base`, `softmax_float`
- `[n]`: 그림 수(빼면 전부). 예: `2000`(빠른 비교), `9000`
- `<model.onnx>`, `<png dir>`(PC): 예: `Debug2_remote/vta/bundles/DeiTTest/deit_fold.onnx`, `~/imagenetv2_png224`

예:
```bash
# 보드, vtabundles_xcmpl/
sudo /bin/sh -c "BIN=vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_alux3_400mhz_rq.bin imagenet_eval/eval_board.sh hw/DeiTTest/vtaDeiTBundle_sys base 2000"
tail -1 imagenet_eval/result/base.log      # 마지막 EVAL 줄에 top-1
```

| 일 | 어디서 | 명령 |
|---|---|---|
| 이미지 준비 (10,000장 → 224 × 224 PNG, `list.txt`) | PC | `prep_imagenetv2.py <imagenetv2-matched-frequency-format-val> <out>`. 보드 `~xilinx/JO/imagenetv2_png224`로 복사 |
| bundle top-1 | 보드 | `sudo /bin/sh -c "imagenet_eval/eval_board.sh <exe> <name> [n]"`. 예: `hw/DeiTTest/vtaDeiTBundle_sys base 10000`. 결과 `imagenet_eval/result/<name>.log`. 이미지는 실행 파일 Dir의 셋(`imagenet_eval/eval_sets.txt`; `PNG=<dir>`가 우선), `n` 기본은 셋 전체 |
| ImageNet val 준비 (DPU 비교, 50,000장) | PC | HF `ILSVRC/imagenet-1k`의 `data/val_images.tar.gz`(로그인과 약관 동의 필요: `~/.venvs/onnxeval/bin/hf auth login`)를 풀고 `prep_imagenet_val.py <tv\|tf> <JPEG dir> <out>` (`tv` torchvision 전처리 → 보드 `~xilinx/JO/imagenet_val_tv224`, `tf` TF-slim 전처리 → `~xilinx/JO/imagenet_val_tf224`) |
| float 기준 | PC | `float_ref.py <model.onnx> <png dir> [n] [out]` (DeiT), `float_ref_cnn.py <resnet18\|resnet50\|vgg16\|mobilenetv2\|deit> <model.onnx> <png dir> [n] [out]` (bundle main과 같은 입력; venv `~/.venvs/onnxeval`) |
| CIFAR-10 준비 (VGG16) | PC | `prep_cifar10.py <cifar-10-python.tar.gz> <out>`. 보드 `~xilinx/JO/cifar10_png`로 복사 (`Vgg16Cifar10Test`의 셋이라 `PNG=`는 필요 없다) |
| int8 흉내 (보드 없이 양자화 시도) | PC | `fakequant.py <model.onnx> <png dir> [--n 9000] [--pow2] [--ln-float] [--gelu-float] [--softmax-float] [--skip s]` |

- `eval_board.sh`는 bitstream을 올리고 lock을 잡는다(`BIN=`, 기본 `SYS_BIN`).
- 한 Dir에는 테스트 셋이 하나다(`eval_sets.txt`). 같은 모델을 다른 셋으로 재려면 Dir을 새로 만든다(예: `MobileNetV2Test` ImageNetV2, `MobileNetV2ValTest` ImageNet val). 새로 받은 모델도 새 Dir이다(`Resnet50TvTest`, `MobileNetV2TfTest`).
  - 환경 `PNG=<dir>`, `OMP_NUM_THREADS`(기본 4), 그리고 `EVTA_*` 같은 실행 스위치를 그대로 넘긴다.
- DeiT main의 환경 변수:
  - `EVAL_LIST=<file>`("<png> <class>" 줄들): 정확도 모드.
  - `EVAL_HASH=1`: 이미지마다 출력 바이트의 hash를 찍는다. 두 bundle이 같은 출력을 내는지 비교할 때 쓴다.

## 7. runtime 환경 변수 (vtalib, 실행할 때)

**무엇을 하나:** 이미 빌드한 실행 파일의 동작을 **다시 빌드하지 않고** 바꾼다.
- 이 변수들은 vtalib(EVTA runtime)과 bundle main이 실행을 시작할 때 읽는다. model-compiler 옵션(§3.3)이나 cmake 옵션(§4)과 다르다.
- 쓰는 곳:
  - 새 기능을 끄고 켜서 시간을 비교한다(`EVTA_LOAD_AHEAD=0` 등).
  - EVTA 결과를 CPU 계산과 비교해 맞는지 본다(`_CHECK`).
  - 층 출력을 파일로 남긴다(`EVTA_DUMP_CONV`).
  - bitstream 이름이 켜는 기능을 강제로 끄거나 켠다(`EVTA_REQUANT` 등).
- 변수를 주지 않으면 표의 "기본"대로 돈다. 보통은 아무것도 주지 않는다.

**어떻게 주나:**
- 스크립트 앞에 `이름=값`을 붙인다. **sudo를 쓰므로 `sudo /bin/sh -c "..."`의 따옴표 안에** 넣는다. `sudo VAR=1 cmd`는 비밀번호를 묻는다.
- 여러 개는 빈칸으로 이어 쓴다.
- `run_all.sh`, `run_prof.sh`, `eval_board.sh`는 받은 변수를 실행 파일에 그대로 넘긴다. 실행 시작 때 출력에 어떤 `EVTA_*`가 설정됐는지도 찍는다(§5.3).

예 1: LOAD를 앞당기는 기능(`EVTA_LOAD_AHEAD`)이 얼마나 빨라지게 했나 보기. 같은 실행 파일을 두 번 돌려 시간을 비교한다.
```bash
cd ~/JO/npuprg/nestc2_clion/nest-compiler/vtabundles_xcmpl
sudo /bin/sh -c "hw/script/run_prof.sh -s _systimed -r Resnet50Test"                     # 기본(2)
sudo /bin/sh -c "EVTA_LOAD_AHEAD=0 hw/script/run_prof.sh -s _systimed -r Resnet50Test"   # 끔
```

예 2: fused conv(residual, pool, depthwise)의 EVTA 결과가 CPU 계산과 같은지 실행 중에 확인.
```bash
sudo /bin/sh -c "EVTA_RES_CHECK=1 hw/script/run_prof.sh -s _sysprof -n 1 MobileNetV2Test"
grep EVTA_RES_CHECK hw/MobileNetV2Test/sysprof.log | head -3
#   EVTA_RES_CHECK <H>x<W>x<C> -> <KN>, shl ... shr ...: 0 of <개수> differ    <- 0 of 이면 EVTA와 CPU가 같다
```
`_CHECK`는 CPU 계산을 더 하므로 시간 측정에는 쓰지 않는다.

예 3: 변경 전후 층 출력이 바이트까지 같은지 보기. 실행 파일 두 개를 각각 돌려 dump하고 비교한다.
```bash
sudo /bin/sh -c "EVTA_DUMP_CONV=/tmp/dumpA hw/script/run_prof.sh -s _sysprof -n 1 Resnet18Test"   # 바꾸기 전 실행 파일
# ... 실행 파일을 다시 빌드한 뒤
sudo /bin/sh -c "EVTA_DUMP_CONV=/tmp/dumpB hw/script/run_prof.sh -s _sysprof -n 1 Resnet18Test"
diff -r /tmp/dumpA /tmp/dumpB && echo same; sudo rm -rf /tmp/dumpA /tmp/dumpB              # root 디스크가 차지 않게 지운다
```

넣을 것(아래 표의 `<...>`):
- `<dir>`: 쓸 수 있는 빈 dir. 예: `/tmp/dumpA`
- `<n>`(`EVTA_MIN_TILE`): conv 출력 tile의 가장 작은 높이 / 폭. 예: `7`
- `<mask>`(`EVTA_MEMEXP`): 비트 0 inp, 1 wgt, 2 acc LOAD, 3 STORE. 예: `1`(inp만), `15`(전부)
- `<bits>`(`MULTI_MASK`): 돌릴 copy의 비트. 예: `1`(copy 0만), `5`(copy 0, 2)
- `EVTA_SCHED`의 값: `<H>,<W>,<C>,<KN>,<K>,<stride>`는 conv 모양(입력 H, W, 입력 채널, 출력 채널, kernel, stride)이다. `<vt>,<tileH>,<tileW>`는 virtual thread 수와 tile 크기다. `EVTA_SCHED_LOG=1`로 먼저 모양을 찍어 보고 쓴다. 예: `EVTA_SCHED=56,56,64,64,3,1:2,14,14`

"systolic"이라고 적은 것은 `vtalib-zcu102-systolic`에만 있다. 이 vtalib로 빌드한 실행 파일(`_sys*`)에서만 듣는다.

**동작 바꾸기**

| 변수 | 기본 | 뜻 |
|---|---|---|
| `EVTA_REPLAY` | 1 | EVTA 명령 record / replay. `0`이면 매번 명령을 새로 만든다 |
| `EVTA_REPLAY_MB` | 16 | EVTA마다 replay 사본에 쓰는 메모리 상한 |
| `EVTA_BUF_POOL_MB` | 32 | buffer pool 상한. `0`이면 pool을 끈다 |
| `EVTA_RUN_TIMEOUT_MS` | 5000 | run 하나가 이 시간을 넘으면 모든 EVTA 상태와 명령을 `/tmp/evta_hang_*`에 남기고 exit 5. `0`은 무제한 |
| `EVTA_NO_BIT_CHECK=1` | 0 | 올라간 bitstream의 EVTA 수 검사를 건너뛴다(위험: EVTA 수가 모자라면 SError) |
| `EVTA_BIAS_RESET=0\|1` | bitstream 이름 | bias reset 강제(이름에 `bias`나 `_rq`가 있으면 켜짐). [evta_isa_ext.md](evta_isa_ext.md) |
| `EVTA_REQUANT=0\|1` | bitstream 이름 | out 쓰기 requantize 강제(이름에 `_rq`). 기능이 없는 bitstream에서 1이면 결과가 틀린다 |
| `EVTA_DWU=0\|1` | bitstream 이름(`_dw`) | systolic: depthwise 3 x 3를 창 장치로(`0`이면 대각 GEMM). 시뮬레이터는 늘 1 ([evta_dw_unit.md](evta_dw_unit.md)) |
| `EVTA_ALUX=0\|1`, `EVTA_ALUX2`, `EVTA_ALUX3` | bitstream 이름 | systolic: ALU 확장 사용(`_alux<k>`). `0`이면 같은 실행 파일이 CPU 대체 경로로 |
| `EVTA_ALU_INP=0` | 1 | systolic: layer norm / softmax의 int8 입력을 inp + identity GEMM 대신 int32로 acc에 |
| `EVTA_AVGPOOL=0` | 1 | systolic: `avgpool_evta`를 끄고 CPU avgpool |
| `EVTA_LOAD_AHEAD` | 2 | systolic: 다음 filter group / tile의 LOAD를 앞당김. `0` 끔, `2`는 1x1 conv 상주 weight 재LOAD도 생략([evta_load_ahead_guide.md](evta_load_ahead_guide.md)) |
| `EVTA_CONV_BURST=0` | 1 | systolic: conv DMA를 길게 묶는 것을 끔 |
| `EVTA_INPUT_STREAM=0` | 1 | systolic: conv0 입력을 conv 전에 한 번에 채움. 기본은 tile 행마다 run을 나눠 겹침 |
| `EVTA_MIN_TILE=<n>` | bitstream 이름 | systolic: 가장 작은 conv 출력 tile. 이름에 `_sa_`면 1, 아니면 7 |
| `EVTA_SCHED=<H>,<W>,<C>,<KN>,<K>,<stride>:<vt>,<tileH>,<tileW>[;...]`, `EVTA_SCHED_LOG=1` | 없음 | systolic: conv 모양의 schedule을 실행 중에 바꾸기, conv마다 모양과 schedule 출력(compare §7.19) |
| `EVTA_RES_PREFETCH=1` | 0 | systolic: epilogue residual을 첫 입력과 같이 LOAD(이득 없음, isa_ext §8.19) |
| `EVTA_QUANT_EXACT=1` | 0 | quantize를 나눗셈으로(기본은 곱 + 확인) |
| `EVTA_CONST_SHARE=0` | 1 | load_module의 상수 VTA buffer를 copy마다 따로. 기본은 같은 내용이면 process 안에서 공유 |
| `NESTC_LN_INT=0` | 1 | DeiT layer norm을 int8 chain으로(기본 정수 layer norm) |
| `NESTC_AVGPOOL_NEON=1` | 0 | 비교용 NEON CPU avgpool |
| `MULTI_MASK=<bits>` | 모두 | multi4 Resnet50 / VGG16: 이 copy만 돌린다(copy i = EVTA i) |
| `MULTI_RUNS`, `MULTI_LOCKSTEP` | 25, 0 | multi-EVTA main의 copy당 추론 수, lockstep(`run_prof.sh -n`, `-l`이 설정) |
| `MULTI_FREE`, `MULTI_STAGGER_US` | 0, 0 | multi-EVTA main: 1이면 추론마다 barrier 없이 각자 돈다, copy i의 출발을 i × us 늦춘다. 2번째 run부터의 처리량을 `THROUGHPUT` 줄로 찍는다(`_sys`로 잴 것; 2026-11-24) |
| `PROF_EXE` | (없음) | `run_prof.sh`: Dir의 run 줄 가운데 그 실행 파일만 돌린다(multi4 Dir의 multivta / singlevta 중 하나) |
| `PROF_INPUT=<file>` | (없음) | `run_prof.sh`: run 줄의 그림 대신 이 입력을 모든 실행 파일에 준다(예: int8 입력 `<name>.i8`). golden 비교는 하지 않는다(2026-11-25) |
| `RUN_LIST=<file>` | (없음) | `run_prof.sh`: run 줄(과 bitstream 줄)을 `script/run_all.sh` 대신 이 파일에서 읽는다. 파일에 bitstream 줄이 없으면 `BIN=`을 함께 준다(`ablation_mnv2.sh`, 2026-11-27) |
| `INPUT_INT8`, `INPUT_INT8_DIR=<dir>` | 1, (없음) | 공통 main(systolic vtalib): 그림 `<dir>/<이름>.png`의 int8 입력 `<이름>.i8`을 `$INPUT_INT8_DIR`, 그림의 dir, 실행 Dir의 `i8/` 차례로 찾아 있으면 그것으로 돈다(기본, 2026-11-25 저녁부터). 없으면 float. `INPUT_INT8=0`이면 늘 float. float 그림도 함께 넘겨서, `.i8`의 quantize가 bundle과 다르면 경고 한 번 찍고 float로 돈다. main은 `Input: int8 ...` / `Input: float ...` 줄을 찍고 `run_prof.sh` summary에 `input int8 <file>` / `input float <png>`가 붙는다 |
| `INPUT_PIPELINE` | 0 | 공통 main(systolic vtalib): `.i8`을 읽지 않고 float 그림을 run k가 도는 동안 run k+1 몫으로 quantize해 둔다(이중 buffer, `VTAInputQuantize`; 카메라 / 디코드에서 그림이 줄지어 오는 경우). `1` = copy마다 둘째 thread, `2` = 둘째 thread 없이 copy가 EVTA run을 기다리는 polling마다 한 줄씩(vtalib-systolic `VTASetWaitWork`; core를 copy가 다 쓰는 multi4에 맞다). run 0은 float(quantize 값 기록), run 1 입력은 시간 재기 전에 만든다. 기다린 시간 / 남은 줄은 시간에 들고 끝에 `INPUT_PIPELINE copy <i>: ...` 줄로 찍는다. 출력 바이트는 float / `.i8`과 같다(2026-11-27, [mobilenetv2_optimize_sw_hw_allmodels.md](mobilenetv2_optimize_sw_hw_allmodels.md) §10) |
| `EVTA_REPLAY_VARIANTS` | 8 | systolic(보드): replay 하나에 buffer 주소 조합마다 둘 명령 사본 수. `0`이면 호출마다 주소를 고치고 flush(전 방식). [mobilenetv2_optimize_sw_hw_allmodels.md](mobilenetv2_optimize_sw_hw_allmodels.md) §6 |
| `EVTA_WGT_FIT=0\|1` | 1 | systolic: 1×1 conv의 thread weight가 wgt 몫에 안 들어가면 filter 묶음을 줄여 한 LOAD로, residual이 있어도 identity entry 아래면 같은 길로(`0`이면 전 방식). [mobilenetv2_optimize_sw_hw_allmodels.md](mobilenetv2_optimize_sw_hw_allmodels.md) §7 |
| `EVTA_BUF_POOL_SHARED=1` | 0 | systolic(보드): buffer pool을 thread 모두에 하나로(전 방식). 기본은 thread마다 크기별 free list |
| `EVTA_CALL_TIME=1` | 0 | systolic(보드): replay된 EVTA 호출마다 CPU 시간을 나눠(bundle, pre, find, start, hook, wait) thread가 끝날 때 replay id별로 찍는다([mobilenetv2_optimize_sw_hw_allmodels.md](mobilenetv2_optimize_sw_hw_allmodels.md) §6) |
| `INPUT_INT8_SAVE=<dir>` | (없음) | 공통 main(`multi_threads_main.cpp`, systolic vtalib): copy 0이 첫 그림을 float로 한 번 돌려 bundle의 quantize(scale / offset / table)를 얻고, 인자의 그림(또는 `EVAL_LIST`의 그림)마다 `<dir>/<이름>.i8`을 쓰고 끝낸다. `EVAL_LIST`이면 `<dir>/eval_list.txt`도 쓴다. 그림 인자나 `EVAL_LIST` 줄이 `.i8`이면 main은 그 바이트를 입력 자리에 넣고 conv0 묶기가 int8로 읽는다(quantize가 다르면 exit 6). [evta_dpu_compare.md](evta_dpu_compare.md) §6.10 |
| `BUNDLE_RUNS` | | `_prof` / `_timed` 실행 파일의 추론 수(`run_prof.sh -n`이 설정) |

**확인 (EVTA 결과를 CPU와 실행 중에 비교, 불일치 수 출력)**

| 변수 | 무엇을 |
|---|---|
| `EVTA_RES_CHECK=1` | fused residual, conv + pool, avgpool, depthwise |
| `EVTA_EPI_CHECK=1` | conv epilogue(어떻게 내렸는지도 출력) |
| `EVTA_EA_CHECK=1` | `elemadd_evta` |
| `EVTA_LN_CHECK=1`, `EVTA_SM_CHECK=1` | layer norm, softmax |
| `EVTA_MATMUL_CHECK=1` | `matmul_evta*`(Q K^T는 출력 LUT까지) |

**기록과 시간**

| 변수 | 뜻 |
|---|---|
| `EVTA_DUMP_CONV=<dir>` | EVTA conv와 maxpool 출력을 모두 `conv_<n>.bin`으로. 변경 전후 byte 비교용(`_CHECK` 변수 없이 비교한다) |
| `EVTA_REPLAY_LOG=1\|2` | `1`: 기록 목록. `2`: replay된 run마다 `EVTA_RUN` 줄(GEMM/ALU 반복, DRAM 바이트, µs) |
| `EVTA_REPLAY_DUMP=<dir>` | 기록된 run마다 명령과 uop을 텍스트로(`rec_<id>_<seg>.txt`). `hazard_dump.py`의 입력(§8) |
| `EVTA_LN_TIME=1`, `EVTA_SM_TIME=1`, `EVTA_MATMUL_TIME=1` | layer norm / softmax / matmul 시간 분해 |

**실험과 멈춤 추적 (결과가 틀릴 수 있음)**

| 변수 | 뜻 |
|---|---|
| `EVTA_MEMEXP=<mask>` | replay의 inp/wgt/acc LOAD와 STORE(bit 0~3)를 1 element로. **결과는 틀린다** |
| `EVTA_TIME_NO_CONV_CLIP=1` | conv clip의 MAX / MIN을 뺀다(시간 측정용, 결과 틀림) |
| `EVTA_FC_PROBE=1` | fc EVTA 경로 bring-up 시험(결과 틀림) |
| `EVTA_NO_DSB=1` | cache flush 뒤 `dsb sy`를 뺀다(multi4 멈춤 재현용) |
| `EVTA_PRE_RUN_US`, `EVTA_PRE_RUN_MASK`, `EVTA_PRE_RUN_AT` | run 앞 대기 시간(µs), 대기할 EVTA mask, 대기 위치 |
| `EVTA_STORE_SENTINEL`, `EVTA_DOUBLE_QUEUE` | STORE 영역 표식, 명령 buffer 번갈아 쓰기 |
| `EVTA_SIM_TOKENS=1`, `EVTA_ALU_HAZARD=17`, `EVTA_ALU_SLOTS=34` | 시뮬레이터 전용: run이 남긴 token 출력, ALU hazard 흉내(step / slot 수) |

## 8. vtalib 시험 (`vtalib-zcu102-systolic/tests/`)

**무엇을 하나:** 모델 전체가 아니라 vtalib 함수 하나(conv epilogue, layer norm, softmax, 출력 LUT)를 작은 입력으로 돌린다. 그 결과를 같은 계산의 CPU 판과 바이트 단위로 비교한다.
- vtalib를 고친 뒤 보드에 가기 전에 PC에서 먼저 돌린다. PC에서는 vtalib의 **시뮬레이터**(EVTA를 C++로 흉내 내는 판)로 돈다.
- 마지막 줄에 통과 수가 나온다(예: `39/39`). 하나라도 틀리면 그 경우의 모양과 다른 칸 수를 찍는다.

넣을 것:
- `[rows] [seed]`: 시험할 행 수와 난수 seed. 빼면 스크립트 기본값. 예: `197 1`
- `[build|run|all]`(보드): `build`는 빌드만, `run`은 실행만, `all`(기본)은 둘 다
- `<EVTA_REPLAY_DUMP dir>`: §7의 `EVTA_REPLAY_DUMP=<dir>`로 남긴 dir. 예: `/tmp/rd`

예:
```bash
# PC
cd /home/jerry/npuprg/nestc2_clion/nest-compiler/vtalib-zcu102-systolic/tests
./run_epi_test.sh                    # conv epilogue 세 방식
# 보드 (먼저 _alux bitstream: sudo vtabundles_xcmpl/hw/script/load_bit.sh vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_alux3_400mhz_rq.bin)
sudo -n /bin/sh -c "./run_alux_board_tests.sh all"
# ALU hazard 검사: 먼저 replay dump를 남기고 검사
sudo /bin/sh -c "EVTA_REPLAY_DUMP=/tmp/rd ../../vtabundles_xcmpl/hw/script/run_prof.sh -s _sysprof -n 1 DeiTTest"
WS=7,17 python3 hazard_dump.py /tmp/rd
```

| 명령 | 어디서 | 하는 일 |
|---|---|---|
| `run_alu_ext_test.sh [rows] [seed]` | g++ 있는 아무 기계 | 정수 layer norm ALU 프로그램 vs bundle CPU 함수(수 초) |
| `run_lut_conv_test.sh` | PC (llvm-8 헤더) | 시뮬레이터 vtalib을 빌드해 conv 출력 LUT, `layernorm_evta`, `softmax_evta`를 CPU와 비교(약 1분) |
| `run_dw_test.sh` | PC | 시뮬레이터 vtalib으로 `dw_convolution_evta` 15모양을 창 모드와 대각 판(`EVTA_DWU=0`) 둘 다 CPU와 비교 |
| `run_epi_test.sh` | PC | conv epilogue를 세 방식(rq 끔, `EVTA_REQUANT=1`, `EVTA_ALUX=0`)으로 CPU와 비교 |
| `sudo -n /bin/sh -c "run_alux_board_tests.sh [build\|run\|all]"` | 보드 (`_alux` bitstream 올린 뒤) | 위 시험들을 보드 EVTA에서. `EVTA_ALUX=0`이면 CPU 대체 경로. `CROSS=1`이면 PC에서 빌드 |
| `WS=7,17 hazard_dump.py <EVTA_REPLAY_DUMP dir>` | 아무 데 | ALU hazard 검사: vector ALU 명령이 최근 W step 안에 쓴 acc 자리를 읽는지 |

## 9. 하드웨어 시험 (보드, `vtabundles_xcmpl/hwtest/`)

**무엇을 하나:** NEST-C나 bundle 없이 EVTA 하드웨어 자체를 시험한다.
- 새 bitstream이 명령을 제대로 읽는지(ISA 비트폭), GEMM / LOAD / STORE가 맞는지, 명령 하나에 시간이 얼마나 드는지 본다.
- 실행이 멈췄을 때 하드웨어 레지스터를 읽는 도구도 여기 있다.
- 새 bitstream을 처음 올렸을 때, 모델이 0이나 쓰레기를 낼 때 먼저 돌린다.

```bash
cd ~/JO/npuprg/nestc2_clion/nest-compiler/vtabundles_xcmpl/hwtest
SUFFIX=_rb ./build.sh runbench                 # vtalib ZCU102 -D 세트로 빌드 (BUFF, UOP, ISA 등은 build.sh 머리말)
sudo ./runbench_rb base 0 1 aluimm 16 2048 load 16 4096 ...   # 여러 case를 한 프로세스에서 (REPS=200)
sudo ./gemmprobe ident                         # 새 bitstream의 GEMM 기본 확인 (reset, zero, ident, perm, ones)
```

넣을 것:
- `SUFFIX=<이름>`, `[test ...]`(build.sh): 실행 파일 이름 끝과 빌드할 시험. 시험을 빼면 `add_test gemm`. 예: `SUFFIX=_rb ./build.sh runbench` → `runbench_rb`
- build.sh의 환경: 다른 bitstream이면 `BUFF="15 18 17"`(EVTA 2 / 4개용 buffer), `ISA="3 3"` 등. 전부 머리말에 있다
- runbench case: `<case> <N> <K>`를 이어 쓴다. `N`은 명령 수, `K`는 명령당 반복 수다(`load` / `store`는 `N`이 LOAD / STORE 수, `K`가 block 수). `base`만 `N K` 없이 `base 0 1`로 쓴다. 예: `gemm 16 256 aluimm 16 2048`. 결과는 case마다 run 시간 중앙값이다
- `<bin>`(probes.sh): 시험할 bitstream. 예: `vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta_sa_400mhz_rq.bin`

| 명령 | 하는 일 |
|---|---|
| `runbench` | run 하나의 고정 비용(약 4.7 µs), 명령 하나와 반복 하나의 비용. case `base`, `gemm N K`, `gemmu N K`, `aluimm N K`, `alureg N K`, `load N K`, `store N K`. 한 프로세스에서 여러 case를 돌린다(프로세스를 여러 번 띄우면 CMA가 샌다) |
| `sudo -n /bin/sh -c 'NUM_EVTA=4 /bin/sh probes.sh <bin> [...]'` | bitstream 프로브 묶음: gemmprobe, sramscan, donerace를 case마다 bitstream을 다시 올려 가며. 통과 기준은 머리말 |
| `sudo ./run_rawtests.sh`, `run_rawtests2.sh` | ISA 비트폭 프로브 `rawtest*`. 다른 bitstream을 쓰기 전에 먼저([vta_synth_notes.md](vta_synth_notes.md) §2.4). `BIN=`으로 bitstream |
| `sudo ./run_convvar.sh` | vtalib `convolution()` EVTA vs CPU, conv 모양 여러 개 |
| `sudo python3 dump_regs.py` | 시간 초과 직후 EVTA 네 HLS 블록의 AXI-lite 레지스터 |
| `sudo python3 dump_insns.py [evta] [count]` | 멈춘 run의 fetch 레지스터와 DRAM의 명령(runtime이 쓴 것과 비교) |
| `sudo python3 plreset.py [n]` | PS→PL reset `pl_resetn<n>` 펄스(`xlnk_reset`은 PL을 건드리지 않음) |
| `sudo python3 plclk.py` / `plclkset.py <MHz>\|stop\|start\|show` | `pl_clk0` 보기 / 바꾸기(기본 100 MHz로 되돌린다) |
| `sudo ./nocpuidle.sh` | JTAG 전에 cpuidle 끄기(부팅마다) |
| `bit2bin.py <x>.bit` | `.bit` → fpga_manager용 `.bin` |
| `brambist/brambist.py all\|columns\|load <s>` | BRAM self test(결함 행 찾기). `stuckchk.py`, `cfgdiff.py`, `sramscan_analyse.py`, `regtest.py`, `regpat.py`는 같은 조사용 |

결함 행(Y219) 조사와 multi4 route는 `log/BOARD_TODO.md`, `vta-hw-zcu102-multi4/hardware/xilinx/scripts/y219route/README.md`에 있다.

## 10. 그 밖의 도구

- **conv schedule 다시 찾기:** `vtabundles_xcmpl/sched_search.py`(`cands` → 보드 `run` → `pick` / `final`). 모든 모양에 후보를 하나씩 동시에 끼워
  run 수를 줄이고, 고른 값은 dump 비교 후 `VTASchedules.h`에 넣는다. 방법과 2026-11-22 결과: [evta_sched_search.md](evta_sched_search.md).
  4개 동시 실행에서 찾기: `SCHED_SET=hw_multievtax4 SCHED_EXE=<multivta…Bundle>`(depthwise도 후보; §5).

| 일 | 어디서 | 명령 |
|---|---|---|
| 날짜별 일지 생성 | PC | `docs/dailyreport/tools/daily_report.sh [YYMMDD ...]` (cron 매일 07:00, 07:00 경계, 로그 `~/.cache/daily_report.log`) |
| 문서 그림 다시 만들기 | PC | `python3 docs/figs/gen_evta_<이름>_figs.py docs/figs` |
| bundle 문장마다 시간 재는 사본 | (CMake가 부름) | `vtabundles_xcmpl/timed_bundle.py <in.cpp> <out.cpp> [csv]` |
| `_timed` 로그 요약 | (run_prof.sh가 부름) | `vtabundles_xcmpl/run_times.py <Dir> <log>` |
| PC 온도 기록 | 첫 PC | `nohup /home/ubuntu/npuprg/templog/templog.sh &` → `templog/temp_<날짜_시각>.log` |

넣을 것:
- `[YYMMDD ...]`(daily_report.sh): 다시 만들 날짜. 빼면 가장 최근 일지 다음 날부터 어제까지. 예: `261120`
- `<이름>`(그림): `docs/figs/gen_evta_<이름>_figs.py`의 이름. 예: `rules`(`gen_evta_rules_figs.py`, 퓨전 규칙 그림)

## 11. 자주 하는 일의 순서

**backend(`VTASave.cpp`, `VTA.cpp`)를 바꿨을 때**
1. PC 컨테이너 `nestc-build`의 `Debug2_remote`에서 `make -j16 model-compiler`.
2. bundle을 만든다(§3.1). `build_sim_bundles.sh -f <target>`, `gen_*_bundle.sh`, multi4는 `gen_multi4_bundles.sh`.
3. 바꾸기 전의 `.cpp`와 비교한다. 바이트가 같으면 보드 실행은 바뀌지 않는다.
4. 바뀌었으면:
   - commit, push.
   - 보드에서 pull, `Debug2_remote`에서 `make -j2 model-compiler`, 다시 생성.
   - 또는 PC에서 cross build해서 scp(§4.2).
   - 그다음 `collect_hw_bundles.py`, 빌드, 측정.
5. 결과가 같은지 `EVTA_DUMP_CONV`나 `_CHECK` 변수로 확인한다.

**vtalib(runtime)만 바꿨을 때**
1. PC에서 고친다. 새 기능은 `vtalib-zcu102-systolic`, HLS용 공통 수정은 `vta/vtalib`에도 한다.
2. PC에서 시험을 돌린다(§8).
3. `vta/vtalib`(submodule)을 commit하고 push한다. main에서 submodule 포인터와 systolic 사본을 commit하고 push한다.
4. 보드에서 pull한다(main, 그다음 `vta/vtalib` 안).
5. `vtabundles_xcmpl/build_sysprof` 등에서 `make -j2 <target>`으로 빌드한다.
6. `run_prof.sh -s _sysprof`로 측정한다. golden 확인은 `run_all.sh`로 한다.
7. 결과가 같은지는 `EVTA_DUMP_CONV`로 변경 전후 출력을 비교한다.

**bitstream을 바꿨을 때**
1. §2로 빌드하고 timing을 확인한 뒤 보드로 옮긴다.
2. `probes.sh`(또는 `gemmprobe`, `runbench`)로 기본 동작을 확인한다.
3. `BIN=<새 파일>` 또는 `SYS_BIN=<새 파일>`로 golden 확인과 측정을 한다.
4. 통과하면 bit dir 이름에 `<clock>_pass_`를 붙이고 README를 쓴다. README에는 맞춰야 하는 software(vtalib commit, cmake 옵션, buffer 크기)를 적는다.

**새 모델을 더할 때**
1. ONNX를 준비한다(`fold_onnx.py`로 모양 고정).
2. `gen_mobilenetv2_bundle.sh`나 `gen_deit_bundle.sh`를 본떠 스크립트를 만든다. profile과 model-compiler를 직접 부르는 방식이다.
3. 요약(§3.4)에서 CPU로 간 GEMM과 합쳐지지 않은 꼬리를 본다. `VTA_FUSION_LOG=1`로 자세히 보고 하나씩 줄인다.
4. `vta/bundles/<Dir>`의 main, `vtabundles_xcmpl/CMakeLists.txt`에 target을 더하고, 이 문서 §3.1에 한 줄을 더한다.
