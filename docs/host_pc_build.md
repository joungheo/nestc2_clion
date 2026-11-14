# 호스트 PC에서 NEST-C 빌드: 시뮬레이터 빌드와 보드 실행 파일 cross build (docker, 2026-11-14)

보드(ZCU102)에서 하던 컴파일러 빌드, 번들 생성, 번들 실행 파일 빌드를 x86 호스트 PC(`ubuntu2`)의 docker 컨테이너에서 한다.
보드에서는 실행과 시간 측정만 한다. 무엇을 어떻게 만들었는지, 어떻게 확인했는지, 무엇이 남았는지를 적는다.
bitstream 합성용 Vivado 컨테이너(`vivado2020-build`, [utils/docker_vivado2020/](../utils/docker_vivado2020/))는 따로 있다.

## 1. 왜 / 무엇이 빨라지나

| 일 | 보드 (4코어, 3.9 GB, swap 없음) | PC 컨테이너 (5950X 32스레드) |
|---|---|---|
| 컴파일러 빌드 (`model-compiler`, `image-classifier`, 처음부터) | 새 빌드 디렉터리는 메모리 부족, cmake 재실행 시 Glow 전체 ~1 h (`-j2`) | 약 10분 (`-j16`) |
| 번들 target 하나 (`make vtaMobileNetV2Bundle` 등) | 106 s | 4–7 s |
| 번들 실행 파일 (`vtabundles_xcmpl`) | 보드에서 빌드 | PC에서 cross build → scp |
| vtalib 시뮬레이터 시험 (`run_lut_conv_test.sh` 등) | – | 컨테이너에서 |
| EVTA 시간 측정 | 보드 | 안 됨 (보드만) |

## 2. 왜 Ubuntu 18.04 컨테이너인가

보드에 직접 들어가 확인했다(2026-11-14). 문서보다 보드가 기준이다.

- `/etc/os-release`: Ubuntu 18.04 (bionic), PYNQ 2.4, 커널 4.14.0-xilinx-v2018.3
- glibc 2.27, python 3.6.5, apt 소스가 bionic
- clang-8 8.0.0-3~ubuntu18.04.2, cmake 3.10.2, protobuf 3.0.0, glog 0.3.5, gflags 2.2.1, fmt 7.0.3 (`/usr/local`, 소스 빌드)
- pip: numpy 1.19.5, onnx 1.11.0, protobuf 3.19.6

호스트 `ubuntu2`는 Ubuntu 20.04.6이라 컴파일러와 라이브러리 버전이 다르다. 같은 버전을 쓰려고 `ubuntu:18.04` 이미지에 위 버전을 깐다.
그래서 PC에서 만든 번들(cpp, weights)이 보드에서 만든 것과 바이트 단위로 같다(§5).

## 3. 만든 것 ([utils/docker_nestc/](../utils/docker_nestc/))

| 파일 | 하는 일 |
|---|---|
| `Dockerfile` | ubuntu:18.04 + build-essential, binutils-aarch64-linux-gnu, rsync, clang-8, llvm-8(-dev), cmake, ninja, protobuf / glog / gflags / png / boost …, fmt 7.0.3 (clang-8, PIC), pip 버전 고정. 사용자 `ubuntu` (UID/GID는 build arg) |
| `setup.sh` | 이미지 `nestc:18.04` 빌드, 컨테이너 `nestc-build` 생성 (`--init`, `$HOME`을 같은 경로로 마운트, TZ KST). sudo로 돌려도 `SUDO_UID`/`SUDO_GID`로 원래 사용자 |
| `configure.sh` | `<tree>/Debug2_remote`를 보드 `Debug2_remote`의 옵션으로 configure (Release, clang-8, `LLVM_DIR=/usr/lib/llvm-8/...`, EVTA, `NESTC_USE_VTASIM=ON`, GENERIC_BUNDLE ON, precompiled OFF, `VTA_RESNET18_WITH_SKIPQUANT0=OFF`) 후 `make -j${JOBS:-16}` |
| `sync_sysroot.sh` | 보드 rootfs 일부를 `~/zcu102_sysroot`로 rsync (§4), 절대 symlink를 상대로 |
| `zcu102_toolchain.cmake` | cross build용 CMake toolchain file (§4) |

```bash
# 호스트 (한 번)
sudo utils/docker_nestc/setup.sh
# 컴파일러 빌드
docker exec -u ubuntu nestc-build <tree>/utils/docker_nestc/configure.sh
# 컨테이너 안에서 작업
docker exec -it -u ubuntu nestc-build bash
cd <tree>/Debug2_remote && make vtaMobileNetV2Bundle        # 번들 cpp / weights 생성
```

- 호스트에서 `docker`는 `sudo -n docker ...`로 돌린다 (jerry는 docker 그룹이 아님).
- 컨테이너에는 `$HOME`만 마운트된다. `/tmp`(Claude 스크래치 포함)는 안 보이므로 중간 파일은 `$HOME` 아래에 둔다.
- 모델 파일은 S3(`nestc-data-pub`)가 없어지고 `aws`도 없어서, 보드 `Debug2_remote`의 모델 파일을 `scp -p`로 가져왔다
  (타임스탬프를 지켜야 make가 다시 내려받으려 하지 않는다). §6 남은 일 참고.

## 4. 보드 실행 파일 cross build

보드의 컴파일러와 같은 clang-8을 `--target aarch64-linux-gnu`로 쓰고, 보드 rootfs 복사본(sysroot)에 대고 링크한다.
aarch64 gcc/g++를 따로 깔 필요는 없다. clang-8이 컴파일러이고, 링커(`ld`)와 `as`만 binutils-aarch64-linux-gnu에서 온다.
C++ 표준 라이브러리, crt 파일, libgcc는 sysroot의 gcc 7.3 설치(보드와 같은 것)에서 온다.

### 4.1 sysroot

```bash
utils/docker_nestc/sync_sysroot.sh [xilinx@192.168.20.2] [~/zcu102_sysroot]   # 호스트에서, 약 2.1 GB
```

- 복사하는 곳: `/usr/include`, `/usr/lib/aarch64-linux-gnu`, `/usr/lib/llvm-8`, `/lib/aarch64-linux-gnu`, `/usr/local/lib`,
  `/usr/local/include`, `/usr/lib/gcc/aarch64-linux-gnu`, `/lib/ld-linux-aarch64.so.1`, `/usr/lib/libcma.so`
- 보드의 절대 symlink(예 `libz.so -> /lib/aarch64-linux-gnu/libz.so.1`)는 PC의 x86 라이브러리를 가리키게 되므로 sysroot 안의
  상대 경로로 바꾼다 (44개). 대상이 없는 16개는 blas / mpi / python 것뿐이라 번들 빌드와 무관.
- 보드에 새 라이브러리를 깔거나 올리면 다시 돌린다.

### 4.2 toolchain file

```bash
cd vtabundles_xcmpl && mkdir build_pc_systimed && cd build_pc_systimed
cmake -DCMAKE_TOOLCHAIN_FILE=<tree>/utils/docker_nestc/zcu102_toolchain.cmake \
      -DBUNDLE_TIMED=ON -DBUNDLE_EXE_SUFFIX=_systimed -DVTALIB=$PWD/../../vtalib-zcu102-systolic ..
make -j16 vtaMobileNetV2Bundle
scp ../hw/MobileNetV2Test/vtaMobileNetV2Bundle_systimed xilinx@192.168.20.2:<tree>/vtabundles_xcmpl/hw/MobileNetV2Test/
# 보드: sudo -n /bin/sh -c "hw/script/run_prof.sh -s _systimed MobileNetV2Test"
```

보드 빌드와 같은 결과를 내려고 맞춘 것:

| 문제 | 원인 | 고친 것 |
|---|---|---|
| vtalib configure에서 ACL 라이브러리 `NOTFOUND` | `find_library`가 sysroot 안에서만 찾음 (`lib/acl`은 소스 트리) | `CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH` |
| `-lgomp`이 LLVM libomp에 링크 | `/usr/lib/llvm-8/lib/libgomp.so`는 libomp의 alias. `-L`로 주면 시스템 디렉터리보다 먼저 찾음 | `-Wl,-L<sysroot>/usr/lib/llvm-8/lib`: driver의 `-L` 뒤에 붙어 보드처럼 GNU libgomp.so.1 |
| 실행 파일 크기 0x8d8 바이트 차이 | 보드의 ld 기본값은 `.hash` + `.gnu.hash`, binutils-aarch64 기본은 `.gnu.hash`만 | `-Wl,--hash-style=both` |
| `vtabundles_xcmpl` CMakeLists의 `/usr/lib/llvm-8/include` | PC의 x86 llvm 헤더를 봄 | `${CMAKE_SYSROOT}/usr/lib/llvm-8/include` (sysroot 없으면 그대로 `/usr/...`) |
| `timed_bundle.py`의 python3 | sysroot에서 찾으면 aarch64 python | `CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER` |

- toolchain file을 바꾸면 **새 빌드 디렉터리**에서 configure한다. `cmake .`는 처음의 `*_INIT` 플래그를 캐시에 둔 채로 쓴다.

## 5. 확인한 것 (2026-11-14)

- 번들 생성: Resnet18 / Resnet50 / VGG16 / MobileNetV2의 번들 cpp와 weights가 보드에서 만든 것과 바이트 단위로 같다.
  calibration yaml은 마지막 float 자릿수만 다르다 (x86 / ARM의 float 출력, 그리고 보드 profile은 10-29에 만든 것).
- vtalib 시뮬레이터 시험 `vtalib-zcu102-systolic/tests/run_lut_conv_test.sh` 통과.
- cross build (MobileNetV2 `_systimed`):

| 항목 | 보드에서 빌드 | PC에서 cross build |
|---|---|---|
| object `.text` (md5) | – | 같음 |
| `NEEDED` 공유 라이브러리 | – | 같음 |
| `EVTA_RES_CHECK=1` | 2,900 비교, 0 differ | 2,900 비교, 0 differ |
| 결과 (cat_285.png) | 282 / 0.944157 | 282 / 0.944157 |
| 시간 (`_systimed`, OMP 1, 24회 중앙값) | 18.00 ms | 18.00 ms |

## 6. 남은 일

1. **모델 파일 받는 곳.** S3가 없어서 지금은 보드에서 복사했다. 앞으로는 onnx/models (GitHub, LFS) 등 공개된 곳에서 받는다:
   `vtabundles_xcmpl/models.md`(파일, md5, 출처)와 받는 스크립트를 만들고, CMake의 `aws` 다운로드 대신 쓰게 한다.
   새 모델(KV260 대상 포함)은 이 경로로만 들인다.
2. **cross build는 MobileNetV2 하나만 보드에서 확인했다.** Resnet18 / 50, VGG16, DeiT, multi4 (`build_multievtax4_*`)를
   PC에서 빌드해 보드에서 같은 결과와 시간인지 본다. 같으면 보드 빌드 디렉터리를 쓰지 않는다.
3. **`collect_hw_bundles.py`를 PC에서 돌리면** `hw/script/run_all.sh`의 이미지 경로가 PC 경로(`/home/jerry/...`)로 바뀐다
   (한 번 그렇게 되어 되돌렸다). 보드 경로를 기본으로 두는 옵션을 넣거나, PC에서는 그 파일을 고치지 않게 한다.
4. **PC → 보드 동기화.** 지금은 실행 파일을 하나씩 scp한다. `hw/<Dir>/`의 실행 파일과 `/tmp/evta_bitstream`을 보는 실행
   스크립트를 한 번에 보내고 돌리는 스크립트(`push_and_run.sh` 같은 것)가 있으면 편하다. 보드 트리도 git pull이 필요하다
   (`94b27a8`, `2190da7`은 문서 / 도구만 바뀌어 아직 보드에 안 받았다).
5. **KV260.** 같은 방식(보드 rootfs → sysroot, toolchain file)으로 된다. KV260의 OS / 라이브러리 버전을 보드에서 직접 확인해
   컨테이너 이미지와 sysroot를 따로 만든다.
6. **시뮬레이터에서 시간 보기.** EVTA 시간은 보드에서만 잰다. `EVTA_REPLAY_LOG=2`가 내는 GEMM / ALU 반복 수, DRAM 바이트를
   시뮬레이터에서 내면 PC에서도 대략의 비교는 된다 (아직 안 함).
7. Vivado 컨테이너와 이 컨테이너는 따로다. 합성은 호스트 온도 문제([host_pc_shutdown.md](host_pc_shutdown.md))가 남아 있다.
