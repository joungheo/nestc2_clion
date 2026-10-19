# ZCU102 EVTA 작업 기록 (2026-09-29 ~ 2026-10-19)

보드(ZCU102, PYNQ 2.4)에서 NEST-C EVTA 번들을 빌드·실행·측정하고, CPU 연산을 최적화하고, multi-EVTA를
시도한 기록. 수정한 소스에는 원래 줄을 주석으로 남기고 `#### JO` 표시를 달았다.

## 1. 보드용 빌드 흐름 (`vtabundles_xcmpl/`)

```
Debug2_remote/build_sim_bundles.sh <target>       # 1차: 시뮬레이터 빌드에서 번들 소스 생성
vtabundles_xcmpl/collect_hw_bundles.py [--set S]  # hw/<Dir>로 복사 + run_all.sh 생성
vtabundles_xcmpl/build*/  make -j2 hw_bundles     # 2차: 보드용 컴파일 (CMake, vtalib은 빌드 디렉터리당 1번)
vtabundles_xcmpl/hw/run_all.sh, run_prof.sh       # sudo, 실행마다 비트스트림 재로드
```
- 위치: `nestc2_clion/vtabundles_xcmpl` → `nest-compiler/vtabundles_xcmpl` (2026-10-17, 절대경로 제거).
- 빌드 디렉터리: `build`(일반), `build_timed`(`-DBUNDLE_TIMED=ON`, 문장별 시간), `build_prof`(`-DNESTC_EVTA_PROFILE=ON`),
  `build_multievtax2`(`-DBUNDLE_SET=multievtax2`). 모두 git 제외.
- `timed_bundle.py`: 번들 `.cpp`의 문장마다 시간 측정 코드를 넣은 사본을 `build_timed/timed/`에 생성
  (실행 순서대로, 실행별 열, conv/FC GOPS, `timing.csv`).
- `run_prof.sh [-s _timed] [Dir…]`: Dir 없으면 전체, 요약 `<suffix>_summary.txt`. `BIN=`으로 비트스트림 지정.
- `collect_hw_bundles.py`: Dir 지정 시 `run_all.sh`의 다른 줄 보존, 첫 번째 `add_executable` 사용,
  `--set multievtax2`, 실행 파일별 로그(multi 세트).
- 시뮬레이터 빌드 cmake 재실행 시 Glow 전체 재빌드를 피하는 법: `SOURCE_DATE_EPOCH` 고정 +
  생성 헤더 5개(`{Node,Instr}GenIncludes.h`, `ONNXModelWriterIncludes.h`, `cpuObjectRegistry.h`, `FunctionPasses.def`)
  의 수정 시각 복원 (`CLAUDE.md` 참고).

## 2. CPU 연산 최적화 (`vta/vtalib/lib/Bundle/CPUBundle_generic.cpp`)

OpenMP(`-fopenmp`, libomp. clang-8의 `-fopenmp=libgomp`는 pragma를 무시함)와 메모리 배치 개선. 결과는 원본과 비트 단위로 동일.

| 함수 | 변경 |
|---|---|
| `convolutionFloat` | 커널 재배치([fx][fy][fd][KN]), 행 단위 병렬 — Resnet18 conv0 1547 → 76 ms |
| `cpuFullyConnected`, `maxpool`, `relu`, `quantize`, `elemadd`, `elemsub_f32`, `elemdiv_f32`, `transpose_*` | 병렬화·연속 접근 |
| 작은 크기 처리 | `relu/quantize/elemadd/elemsub_f32/elemdiv_f32`는 4096 이하이면 OpenMP 없이 실행 (`elemmax`가 원소마다 `quantize`를 불러 SplatMax가 15~100 s 걸리던 문제 → 40 ms) |

## 3. 측정 결과 (보드, 측정 실행 = 2회 중 두 번째)

| 번들 | 처음 | 현재 | 비고 |
|---|---:|---:|---|
| Resnet18Test | 1678.9 ms | **135.9 ms** | top-1 281 / 0.587504 (불변) |
| Resnet18PartitionTest | 567.9 ms | **168.3 ms** | p0을 VTA 파티션으로 (§5), 281 / 0.530282 |
| Resnet50Test | — | 1452.8 ms | conv1(`nonvtaconvolution`)이 1156 ms — 할 일 |
| Vgg16Cifar10Test | 733.7 ms | **47.6 ms** | `-instrument-debug` 끔 |
| Conv4Test (3×3, 231 Mops) | — | 3.6 ms | VTA conv 122 GOPS (피크 170.5 GOPS @333 MHz) |
| SplatMax | 15~100 s | 40 ms | §2 |

골든이 있는 테스트 29개 모두 PASS.

## 4. 번들 main / 생성 설정 (`vta/bundles/`)

- main 33개: 2회 실행(워밍업 + 측정), `CLOCK_MONOTONIC`으로 측정. Resnet50·VGG16·Partition·FloatConvTest는
  `clock()`(전 스레드 CPU 시간 합)을 벽시계 시간으로 교체.
- `Vgg16Cifar10Test/CMakeLists.txt`: `-instrument-debug` 주석 처리.
- `Resnet18PartitionTest/main.cpp`: 파티션별 시간 출력, `p0_load_module/destroy_module`.

## 5. Resnet18PartitionTest 계획 변경

- 새 계획 `tests/runtime_test/partition_perform_profile/NESTOptimalFusedPlan_p0VTA.yaml` (p0을 CPU → VTA).
  원래 계획은 Resnet18RelayEVTAPartitionTest도 써서 그대로 둠.
- `Resnet18PartitionTest/CMakeLists.txt`: 새 계획, `../Resnet18Test/VTASkipQuantizeNodes.txt` 복사(conv0 float 유지
  → `convolutionFloat`), `p0.o` → `p0.cpp`.
- conv0(입력 채널 3)은 VTA 조건(채널 16의 배수, `VTASave.cpp` ~3098행)을 못 맞춰 VTA 파티션 안에서도 CPU 함수.

## 6. Multi-EVTA

- `vta/multievtax2_bundles/` (새): 2-EVTA용 ConvTest(번들 0/1 동시), Resnet18(2벌 스레드 / 단일 비교용).
  시뮬레이터 빌드 옵션 `NESTC_EVTA_MULTI_BUNDLES=ON`(`vta/CMakeLists.txt`)으로 생성.
- 보드: `collect --set multievtax2` → `hw_multievtax2/`, `build_multievtax2/`(vtalib `NESTC_EVTA_MULTI` + `VTA_MAX_XFER` 8 MB:
  명령 핸들마다 CMA 2×`VTA_MAX_XFER`, CMA 128 MB).
- `-index-multi-evta`는 비트마스크(1/2/4/8 → EVTA 0~3).
- **결과**: etri/nest-data의 15/18/17 계열 비트스트림(1×, 2×, 4×)은 지금 vtalib으로 결과가 모두 0
  (명령어 폭 256비트는 같음, `memory_type`/`alu_opcode` 2·3비트 조합 모두 실패). nest-data의 옛 런타임
  (`lib/VTA/zcu102`)은 시간 초과, 지금 비트스트림에서는 SError 커널 오류 → 사용 금지(`hwtest/*_old*`).
- `vta/vtalib/include/zcu102/vta/hw_spec.h`: 두 필드 폭을 `-D`로 바꿀 수 있게 함(기본 2, 결과 불변 확인).
- `hwtest/`: `build.sh`에 `BUFF/XFER/ISA/EVTA/NUM_EVTA/OLDRT` 옵션, `run_rawtests*.sh`에 `BIN/RT/TMO`,
  `dump_regs.py <evta…>`, `bit2bin.py`(.bit → /lib/firmware용 .bin, md5 검증), `old_api_shim.h`.

## 7. EVTA 비트스트림 직접 생성 (`vtabundles_xcmpl/evta_hw/`, 준비만 함)

vta-hw HLS를 vtalib의 256비트 `hw_spec.h`로 합성, ZCU102·HP 포트·EVTA 1~4개·레지스터 `0xA0000000 + i×0x10000`.
Vivado/HLS 2020.1(docker, PC). 1단계: `make ip && make bin NUM_EVTA=1` → 지금 비트스트림과 같은 결과인지 확인.
자세한 내용은 `evta_hw/README.md`.

2026-10-20: EVTA 1개는 `vta-hw-zcu102-single/hardware/xilinx`(vta-hw Makefile에 zcu102 타깃 추가)로 합성 완료.
PC docker `vivado2020-build`, Vivado `/tools/Xilinx/v2020.1`. `vtahw_zcu102_1x16_i8w8a32_16_16_19_18.bin`,
333 MHz WNS +0.027 ns, BRAM 253/912. `PROFILE=1`이면 compute 카운터(0x50/0x60/0x70)와 AXI Perf Monitor
(0xA0050000, slot0 HP0 전체/slot1 load/slot2 store)가 들어간 `..._prof.bin`. 보드 시험 전.

## 8. 할 일

1. `vtahw_..._16_16_19_18(.bin, _prof.bin)` 보드 검증 → evta_hw 2개 → 4개(작은 버퍼).
2. `nonvtaconvolution`/`cpunonvtaconvolution` 최적화 (Resnet50 conv1 1156 ms).
3. conv0(3채널): `convolutionFloat` NEON 최적화 또는 채널 16 패딩으로 VTA 실행 (`README.md` To do).
4. VTA 1×1 conv(19~54 GOPS), 출력 transpose, 버퍼 alloc/free 비용.

## 9. git

보드에서 직접 커밋·push (2026-10-19부터, 브랜치 `main`). 보드 디렉터리가 git 작업 사본이므로 CLion 자동 업로드는 끌 것.
- `nest-compiler` → github.com/joungheo/nestc2_clion, `vta/vtalib` → github.com/joungheo/nestc2_vtalib
  (서브모듈이 먼저 push되어야 상위 저장소의 서브모듈 포인터가 유효).
- 2026-10-02 커밋에 이미 들어간 것: `lib/Backends/VTA`, `cmake/glow/CMakeLists.txt`, vtalib `CMakeLists.txt`·simulator 쪽 등
  (보드 최초 이식). 2026-10-19 커밋: vtalib 6개(§2, §6 hw_spec, profiling/flush), nest-compiler 71개(§1, §4~§7).
- push: `git -C vta/vtalib push` → `git push` (HTTPS, 사용자 이름 joungheo + 토큰. 토큰은 저장하지 않음).
