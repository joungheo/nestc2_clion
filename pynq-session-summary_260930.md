# PYNQ(ZCU102) NEST-C / EVTA 세션 요약

- 원본 세션: `session_01CNuXvXDeZMJ1x1gyBnj4hw` (Remote Control, 보드 `pynq`에서 실행)
- 기간: 2026-09-29 03:15 ~ 2026-09-30 10:32 (UTC). 마지막 응답은 사용량 한도("session limit")에 걸려 끊김.
- 이 문서는 세션 기록(사용자 메시지, Claude 응답, 도구 호출)을 처음부터 끝까지 읽고 정리한 것입니다. 사용자가 붙여 넣은 긴 로그는 앞뒤만 확인했습니다.
- 스크립트 원문은 기록에 남은 Write/Edit/sed 호출을 순서대로 재적용해서 복원했습니다 → `recovered-scripts/` 폴더. 보드의 실제 파일과 다를 가능성은 낮지만, 보드에서 직접 확인한 것은 아닙니다.

---

## 0. 보드 파일은 남아 있을까? (추정, 미확인)

**거의 확실히 남아 있습니다.** 터미널을 닫으면 끝나는 것은 Claude 세션(대화 프로세스)이고, 보드 디스크의 파일은 그대로입니다. 클라우드에서 "환경이 삭제됨(environment_deleted)"으로 보이는 것은 Remote Control 연결 정보가 사라졌다는 뜻이지 보드 파일이 지워졌다는 뜻이 아닙니다. 보드를 재부팅·재이미징하지 않았다면 아래 파일은 모두 있을 것입니다(직접 확인하지는 못했습니다).

| 남아 있을 것 (디스크) | 사라졌을 수 있는 것 |
|---|---|
| `nest-compiler/` 소스 수정 전부, `Debug2_remote/` 빌드 결과 | `/tmp/claude-1000/...` 스크래치(재부팅 시 삭제): `conv3/`, `conv3_after/`, `VTASave.cpp.before_flush`, `hw_spec.h.before_reformat` 백업 |
| `vtabundles_xcmpl/` 스크립트·번들·`hw/` | 대화 문맥 자체 (이 문서로 대체) |
| tar 백업 2개, `~/.claude/projects/...` 메모리 파일 | |

확인 명령 (보드에서):
```bash
ls -la /home/xilinx/JO/npuprg/nestc2_clion/vtabundles_xcmpl/{build_sim_bundles.sh,build_hw_bundles.py} \
       /home/xilinx/JO/npuprg/nestc2_clion/vtabundles_xcmpl/hw/results.txt \
       /home/xilinx/JO/npuprg/nestc2_clion/*.tar.gz
grep -rn "#### JO" /home/xilinx/JO/npuprg/nestc2_clion/nest-compiler/vta/vtalib/include/zcu102/vta/hw_spec.h
```

---

## 1. 목표

ZCU102 보드(PYNQ 2.4)에서 NEST-C(ETRI의 Glow 기반 NPU 컴파일러)로 EVTA 번들을 만들고, **실제 FPGA(EVTA 비트스트림)에서 실행해 시뮬레이터 golden과 일치시키는 것.** 최종적으로 vta/bundles 아래 모든 번들(Conv, Pool, Quantize, ElemAdd, Resnet18/50, VGG16 등)을 시뮬레이터용·하드웨어용으로 빌드하고 보드에서 돌림.

## 2. 환경

| 항목 | 값 |
|---|---|
| 보드 | ZynqMP ZCU102 Rev1.0, 호스트명 `pynq`, aarch64 4코어, RAM 3.9G, **swap 없음**, 디스크 59G(여유 27G) |
| OS | PYNQ Linux 2.4 (Ubuntu 18.04), 커널 4.14.0-xilinx-v2018.3 |
| 사용자 | `xilinx` (sudo 비밀번호 필요 → Claude는 sudo 실행 불가, 하드웨어 실행은 사용자가 직접) |
| 소스 | `/home/xilinx/JO/npuprg/nestc2_clion/nest-compiler` (**git 저장소 아님**. CLion이 호스트 PC에서 원격 업로드하는 구조) |
| 빌드 디렉터리 | `nest-compiler/Debug2_remote` (Unix Makefiles, Release, **시뮬레이터 설정**: `NESTC_USE_VTASIM=ON`, `NESTC_EVTA_RUN_ON_ZCU102=OFF`) |
| cmake 옵션 (캐시에서 확인) | `-DCMAKE_C_COMPILER=clang-8 -DCMAKE_CXX_COMPILER=clang++-8 -DLLVM_DIR=/usr/lib/llvm-8/lib/cmake/llvm -DNESTC_EVTA_BUNDLE_TEST=ON`, `NESTC_WITH_EVTA=ON`, `GLOW_WITH_BUNDLES=ON`, `VTA_RESNET18_WITH_SKIPQUANT0=ON` |
| 하드웨어 작업 폴더 (새로 만듦) | `/home/xilinx/JO/npuprg/nestc2_clion/vtabundles_xcmpl/` |
| 툴체인 | clang-8/clang++-8 (`clang` 심볼릭 링크 없음), LLVM `/usr/lib/llvm-8` (문서의 `llvm-8.0` 경로는 없음), CMake 3.22.6(pip), Python 3.6.5 |
| 비트스트림 | `/lib/firmware/zcu102_1x16_i8w8a32_16_16_19_18.bin` (fpga_manager로 로드). 이 보드에서 처음 사용 |
| 기타 | aws CLI 없음, ETRI S3 버킷 `nestc-data-pub` 삭제됨(NoSuchBucket). CMA 128MB. 다른 계정(songmi, suhyung, jinhu)도 보드 공유 |

## 3. 진행 순서 (시간순)

### 9/29 오전: 분석
1. 보드 접속 확인, 프로젝트 구조와 ZCU102 빌드 조건 분석(Glow 오버레이 구조, 백엔드, 툴체인 상태).
2. `CLAUDE.md` 수정(파트너 백엔드 제외, 보드 빌드 주의점, `make -j2`, LLVM 경로, sudo). 이때 Claude가 `Debug2_remote`를 실수로 ZCU102로 재설정했다가 백업 `CMakeCache.txt`로 **원복**함.
3. `Debug2_remote`의 cmake 옵션 역추적(위 표).
4. 결정: **번들 생성은 시뮬레이터 빌드(VTASaverTest)에서, 하드웨어 실행 파일은 cmake 없이 vtalib ZCU102 소스와 직접 컴파일 (Option B).** libVTA.a는 시뮬레이터/ZCU102 설정과 무관(조건부 컴파일 없음)함을 코드로 확인.
   - 근거: `cmake/glow/CMakeLists.txt`는 ZCU102=ON이면 folly, gtest/TestMain을 빼서 VTASaverTest가 링크 안 됨. 사용자가 08:44에 `add_subdirectory(lib)`/`tools`를 ZCU102=ON에서 다시 빼도록 직접 수정함.

### 9/29: VTASaverTest.convTest3 실패 디버깅 (시뮬레이터 버그 2개)
- 증상: VTA 출력 6272개가 전부 0, VTAInterpreter는 정상 → 테스트 FAIL.
- 원인 1: `include/simulator/hw_spec.h` 명령어 구조체 패딩이 주석 처리되어 Generic/Mem 16B, Gem/Alu 24B로 섞임(256bit=32B여야 함). → 패딩 주석 해제, 모두 32B.
- 원인 2: `sim_driver.cc` `RunStore()`가 `VTA_MEM_ID_OUT`(4)를 무시. memory_type을 2→3bit로 넓히면서 생긴 문제. → OUT 허용 추가.
- 결과: convTest3 **PASSED**, golden 정상. (VTASaverTest 첫 빌드 63분)
- 백업: `/home/xilinx/JO/npuprg/nestc2_clion/vtalib_instruction_width_256bit.tar.gz` (8.6MB, 2bit 수정 **이전** vtalib 전체)

### 9/29: 보드 실행 디버깅 (Conv3Test)
| 단계 | 증상 | 원인/조치 |
|---|---|---|
| 1 | `dat_queue.h:39 fpga_buff_ != nullptr` | CMA 여유 30MB(런타임이 64MB 선할당). `sudo python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()"`로 해결 |
| 2 | 입력 복사 후 무한 대기 | 하드웨어 DONE 미도착. add_test/gemm도 timeout |
| 3 | 점검: fclk0 100MHz ✅, EVTA 레지스터 응답 ✅, HP/HPC/HPM 모두 128bit ✅, coherent 설정 무관, vtalib ZCU102 소스는 원본과 동일, S3 사전빌드 런타임은 x86용(+vta.tar.gz 0바이트) | |
| 4 | `rawtest`(명령어 직접 투입): FINISH ✅, 256bit 간격 ✅, 128bit 간격 ❌ | **하드웨어 = 256bit 명령어** |
| 5 | `rawtest2`: 새로 비트스트림을 올리면 모두 DONE이지만 STORE가 DRAM에 안 씀 | 앞서 멈춘 건 이전 교착 상태가 PL에 남아서. **실행 전 매번 비트스트림 재로드 필요** |
| 6 | `store2b`/`add2b`: memory_type·alu_opcode를 2bit로 인코딩하니 정확 | **근본 원인: 비트스트림은 memory_type 2bit, alu_opcode 2bit. 헤더는 3/3** → zcu102·simulator `hw_spec.h` 모두 2bit로 수정 |
| 7 | add_test_nc 16개 정확, gemm_nc `Golden Matched`, conv13_nc `TEST COMPLETE`. Conv3 번들만 timeout | `VTA_COHERENT_ACCESSES=true`(캐시 flush 안 함)가 원인 → `false`로 빌드하니 실행 완료 |
| 8 | 1947/6272 불일치 (포화 안 된 채널만) | `convvar` 6개 조건 모두 PASS → 하드웨어 연산 정상. 번들이 입력/출력 버퍼를 CPU가 직접 쓰고 읽으며 flush/invalidate 안 함 |
| 9 | 번들 사본에 `FlushCache`/`InvalidateCache` 추가 | **Conv3Test RESULT: PASS** (11:17) |

### 9/30: 근본 수정과 전체 빌드
1. **vtalib 공개 API 추가**: `VTABufferFlush(buffer, offset, size)`, `VTABufferInvalidate(...)` (zcu102·simulator `runtime.h`, `lib/zcu102/api.cc`, `lib/simulator/runtime.cc`).
2. **VTASave.cpp 코드 생성기**: CPU가 `VTABufferGetVirtAddr`로 직접 접근하는 6곳에 호출 생성(입력 transpose 4곳 뒤 Flush, 출력 transpose 2곳 앞 Invalidate). 백업: 스크래치 `VTASave.cpp.before_flush`. Conv3 새 번들 시뮬레이터·보드 모두 **PASS**(07:20).
3. **vtalib CMake**: 새 옵션 `NESTC_EVTA_COHERENT_ACCESSES`(기본 OFF → `-DVTA_COHERENT_ACCESSES=false`). 스크래치 CMake로 configure·`vta_runtime` 컴파일 확인.
4. `CLAUDE.md`에 "EVTA on this board" 절 추가.
5. 백업: `/home/xilinx/JO/npuprg/nestc2_clion/nestc2_nest-compiler_vtabundles_20260930.tar.gz` (398MB, 15,504파일, `Debug2_remote` 제외, 07:46 생성). ⚠️ 이후 만든 `build_sim_bundles.sh`, `build_hw_bundles.py`, `hw/`는 **이 tar에 없음**.
6. 전체 재빌드:
   - 도구 6개 (`make -j2 image-classifier model-compiler model-partition-tuner ccode-generator nestos-partitioner VTASaverTest`, Debug2_remote): ✅ 73분
   - `.onnx` 배치: `Debug2_remote/models/mxnet_exported_resnet18.onnx`, `Debug2_remote/vta/bundles/Resnet50Test/resnet50.onnx`, `Debug2_remote/vta/bundles/Vgg16Cifar10Test/vgg16_cifar10.onnx` (+ 파티션용 보정 YAML)
   - 시뮬레이터 번들 35/35 빌드, ctest **65/65 통과**. 모델 분류: Resnet18/Resnet18Partition/Resnet50 → `cat_285.png` = 281(tabby cat), Vgg16 → 0(airplane)
   - 하드웨어 번들 35/35 빌드 (`build_hw_bundles.py`)
   - 제외: `Resnet18RelayEVTAPartitionTest`(TVM 필요), BNN(`BinaryNet`, `BiRealNet`, 이 비트스트림에서 MUL/MOVE 불가)

### 9/30 10:31 보드 실행 결과 (`sudo ./run_all.sh`)
- **PASS 30개**: AvgPool, Conv/Conv2~5, Conv_inception_5b_5x5_1__1, Dequantize, ElemAdd/Sub/Div, FC, FloatConv, MaxPool, NonVTAConv, Quantize, Relu, ResConv1~10, SoftMax, SplatMax, Transpose
- **RAN (golden 없음) 5개**: Resnet18PartitionTest, Resnet18Test, Resnet50Test, Resnetv10S1C0Test, Vgg16Cifar10Test → `hw/<번들>/run.log`의 `Result:` 값 확인 필요(정상: Resnet 계열 281, Vgg16 0)
- MISMATCH/TIMEOUT/FAIL: 없음
- 이 결과를 받은 직후 세션이 사용량 한도로 끊겨, Claude는 분석하지 못함.

## 4. 소스 수정 목록 (모두 `//#### JO ...` 주석 + 원본 주석 보존)

| 파일 | 변경 |
|---|---|
| `vta/vtalib/include/simulator/hw_spec.h` | Generic/Mem/Gem/Alu 구조체 패딩 활성화(32B), `VTA_ALU_OPCODE_BIT_WIDTH` 3→2, `VTA_MEMOP_ID_BIT_WIDTH` 3→2 |
| `vta/vtalib/include/zcu102/vta/hw_spec.h` | `VTA_ALU_OPCODE_BIT_WIDTH` 3→2, `VTA_MEMOP_ID_BIT_WIDTH` 3→2 |
| `vta/vtalib/lib/simulator/sim_driver.cc` | `RunStore()`에 `VTA_MEM_ID_OUT` 허용 (2bit에서는 불필요하지만 남겨 둠) |
| `vta/vtalib/include/{zcu102,simulator}/vta/runtime.h` | `VTABufferFlush`, `VTABufferInvalidate` 선언 |
| `vta/vtalib/lib/zcu102/api.cc`, `lib/simulator/runtime.cc` | 위 API 구현 (`DataBuffer::FlushCache/InvalidateCache`) |
| `vta/vtalib/CMakeLists.txt` | 옵션 `NESTC_EVTA_COHERENT_ACCESSES` (기본 OFF), ZCU102 분기의 `-DVTA_COHERENT_ACCESSES=true` 주석 처리 |
| `lib/Backends/VTA/VTASave.cpp` | transpose 6곳에 Flush/Invalidate 생성 코드 |
| `CLAUDE.md` | 보드 빌드 절, "EVTA on this board" 절 |
| (사용자 직접) `cmake/glow/CMakeLists.txt` | ZCU102=ON이면 `lib`/`tools` 다시 제외 |
| (번들 사본) `vtabundles_xcmpl/Conv3Test/vtaConv3TestBundle.cpp.handfix` | 손으로 넣었던 flush 버전 (현재는 새 API 버전으로 대체) |

주요 diff (기록에서 그대로):
```c
// zcu102/vta/hw_spec.h, simulator/hw_spec.h
//#### JO changed to match EVTA bitstream zcu102_1x16_i8w8a32_16_16_19_18 (alu opcode 2bit)
//#define VTA_ALU_OPCODE_BIT_WIDTH 3
#define VTA_ALU_OPCODE_BIT_WIDTH 2
//#### JO changed to match EVTA bitstream zcu102_1x16_i8w8a32_16_16_19_18 (memop id 2bit)
//#define VTA_MEMOP_ID_BIT_WIDTH 3
#define VTA_MEMOP_ID_BIT_WIDTH 2

// lib/zcu102/api.cc (simulator/runtime.cc 동일)
//#### JO added: explicit cache maintenance (DataBuffer skips it when kBufferCoherent)
void VTABufferFlush(void* buffer, size_t offset, size_t size) {
  vta::DataBuffer::FromHandle(buffer)->FlushCache(offset, size);
}
void VTABufferInvalidate(void* buffer, size_t offset, size_t size) {
  vta::DataBuffer::FromHandle(buffer)->InvalidateCache(offset, size);
}
```
```cmake
# vta/vtalib/CMakeLists.txt
option(NESTC_EVTA_COHERENT_ACCESSES "EVTA DMA is cache-coherent with the CPU (ZCU102 only)" OFF)
...
            #### JO changed: coherence now selected by NESTC_EVTA_COHERENT_ACCESSES (default OFF)
            #-DVTA_COHERENT_ACCESSES=true
            -Dvta_EXPORTS)
    #### JO added
    if(NESTC_EVTA_COHERENT_ACCESSES)
        add_definitions(-DVTA_COHERENT_ACCESSES=true)
    else()
        add_definitions(-DVTA_COHERENT_ACCESSES=false)
    endif()
```

생성된 번들 코드 모양:
```cpp
transpose_nhwc2vtaio(inputP, VTABufferGetVirtAddr(conv_input_transpose), ...);
VTABufferFlush(conv_input_transpose, 0, 3136);
convolution_wo_tr(...);
VTABufferInvalidate(conv_output_bef_transpose, 0, 6272);
transpose_vtaio2nhwc(VTABufferGetVirtAddr(conv_output_bef_transpose), outP, ...);
```

## 5. 보드 사용 규칙 (검증된 사실)
- 비트스트림 `zcu102_1x16_i8w8a32_16_16_19_18`: **256bit 명령어, memory_type 2bit, alu_opcode 2bit.** ALU MUL(4), MEM_ID_MOVE(5) 표현 불가 → BNN 경로 불가. 다른 비트스트림을 쓸 땐 `hwtest/rawtest*`로 먼저 확인.
- **캐시 일관성 없음** → `VTA_COHERENT_ACCESSES=false` 필수.
- **EVTA 실행 전마다 비트스트림 재로드 + `xlnk_reset`.** 한 번 교착되면 이후 실행이 모두 멈추고 `xlnk_reset`만으론 안 풀림. (`run.sh`, `run_*.sh`, `hw/run_all.sh`가 자동 처리)
  ```bash
  sudo sh -c 'echo 0 > /sys/class/fpga_manager/fpga0/flags; echo zcu102_1x16_i8w8a32_16_16_19_18.bin > /sys/class/fpga_manager/fpga0/firmware'
  sudo python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()"
  ```
- 런타임이 명령 핸들당 CMA 64MB 선할당(CMA 전체 128MB). 공유 보드라 다른 사용자 작업과 겹치면 서로 깨질 수 있음. `run.sh`에 잠금/사용 중 확인을 넣는 제안은 사용자가 **보류**함.
- EVTA 레지스터: fetch 0xA0000000, load 0xA0001000, compute 0xA0002000(done=0x18, valid=0x1c), store 0xA0003000. HLS ctrl 비트: 0=start, 1=done, 2=idle, 7=auto_restart.
- cmake 재실행 시 날짜가 바뀌었으면 `GLOW_BUILD_DATE` 때문에 Glow 전체 재컴파일(-j2로 약 1시간).

## 6. 사용자가 정한 작업 규칙
- 코드 수정 시 원본은 주석으로 남기고 새 코드는 그 아래, 위에 `//#### JO <이유>` 한 줄 (CMake는 `#### JO`). 문서·새 파일에는 적용 안 함. (보드 메모리 `code-edit-comment-out-original.md`에 저장됨)
- CLAUDE.md만 고치라고 하면 다른 것은 건드리지 않기. 다른 사용자 홈 디렉터리는 조사하지 않기.
- CLion은 호스트 PC → 보드 업로드 방식이라, **보드에서만 고친 파일은 CLion을 켜기 전에 호스트로 가져가야 덮어쓰이지 않음.**

## 7. 남은 일
1. **모델 번들 5개(RAN) 결과 확인**: `hw/<번들>/run.log`의 `Result:` 값 (Resnet18/Partition/Resnet50 = 281, Vgg16 = 0이 정상, Resnetv10S1C0은 기준값 확인 필요). 하드웨어 결과를 시뮬레이터 출력과 비교하는 golden 추가도 고려.
2. 최신 백업: 07:46 tar 이후 만든 `vtabundles_xcmpl/build_sim_bundles.sh`, `build_hw_bundles.py`, `hw/`, `logs/`는 백업 tar에 없음. 새 tar 생성 권장.
3. 보드에서 고친 파일들을 호스트 PC(CLion)로 옮기기.
4. BNN 경로(`xp_convolution_wo_tr`의 `mul_buf` flush 누락, VTASave BNN 4곳 미검증)는 BNN용 비트스트림이 생길 때.
5. `Resnet18RelayEVTAPartitionTest` 등 Relay/TVM 번들은 libtvm 빌드가 필요해서 제외됨.
6. `run.sh` 공유 보드 보호(잠금, `fuser /dev/xlnk`, CMA 여유 확인)는 보류 상태.
7. `VTAXPGemInsn`(BNN)과 `gemm_from_output` 필드는 시뮬레이터/zcu102 헤더 간 차이가 남아 있음.

## 8. 파일 위치 (보드)
`/home/xilinx/JO/npuprg/nestc2_clion/vtabundles_xcmpl/`
- `build_sim_bundles.sh`: Debug2_remote에서 번들 35개 생성+빌드, 로그 `logs/<target>.log`, `logs/summary.txt`
- `build_hw_bundles.py`: 하드웨어 번들 빌드 → `hw/<Dir>/`, `hw/common/`, `hw/run_all.sh`, `hw/build_report.txt`. **`Conv3Test/build.sh`에서 컴파일 플래그를 읽어 오므로 그 파일이 있어야 동작.**
- `hw/run_all.sh`: `sudo ./run_all.sh [Dir ...]`, 결과 `hw/results.txt`, 각 `run.log`
- `Conv3Test/`: `build.sh`(cmake 없이 ZCU102 빌드), `run.sh`(재로드→실행→golden cmp), 번들 소스·bin
- `hwtest/`: `build.sh`, `add_test(_nc)`, `gemm(_nc)`, `conv13(_nc)`, `rawtest.cpp`, `rawtest2.cpp`, `convvar.cpp`, `run_rawtests.sh`, `run_rawtests2.sh`, `run_convvar.sh`, `dump_regs.py`

백업: `/home/xilinx/JO/npuprg/nestc2_clion/vtalib_instruction_width_256bit.tar.gz`(구버전), `/home/xilinx/JO/npuprg/nestc2_clion/nestc2_nest-compiler_vtabundles_20260930.tar.gz`(최신 소스)

보드의 Claude 메모리: `/home/xilinx/.claude/projects/-home-xilinx-JO-npuprg-nestc2-clion-nest-compiler/memory/` (`code-edit-comment-out-original.md`, `zcu102-evta-bitstream-facts.md`, `MEMORY.md`)

---

## 부록 A. `build_sim_bundles.sh` 전문 (복원)
```bash
#!/bin/bash
# Build (generate + compile) every vta/bundles target in the simulator build Debug2_remote,
# one target at a time so a failure does not stop the rest. Relay/TVM and BNN bundles excluded.
# Log per target: logs/<target>.log ; summary: logs/summary.txt
# usage: ./build_sim_bundles.sh [target ...]
set -u
B=/home/xilinx/JO/npuprg/nestc2_clion/nest-compiler/Debug2_remote
HERE=$(cd "$(dirname "$0")" && pwd)
LOG=$HERE/logs; mkdir -p $LOG
export TVM_HOME=$B/../tvm PYTHONPATH=$B/../tvm/python TVM_LIBRARY_PATH=$B/tvm

TARGETS=${@:-
vtaConvTestBundle vtaConv2TestBundle vtaConv3TestBundle vtaConv4TestBundle vtaConv5TestBundle
vtaResConv1TestBundle vtaResConv2TestBundle vtaResConv3TestBundle vtaResConv4TestBundle
vtaResConv5TestBundle vtaResConv6TestBundle vtaResConv7TestBundle vtaResConv8TestBundle
vtaResConv9TestBundle vtaResConv10TestBundle Conv_inception_5b_5x5_1__1Bundle
vtaFloatConvTestBundle vtaResnetv10S1C0TestBundle vtaAvgPoolTestBundle
vtaElemAddTestBundle vtaElemSubTestBundle vtaElemDivTestBundle vtaFCTestBundle
vtaMaxPoolTestBundle vtaReluTestBundle vtaSoftMaxTestBundle vtaSplatMaxTestBundle
vtaTransposeTestBundle vtaQuantizeTestBundle vtaDequantizeTestBundle vtaNonVTAConvTestBundle
vtaMxnetResnet18Bundle vtaMxnetResnet18PartitionBundle vtaVGG16Cifar10Bundle vtaCaffe2Resnet50Bundle}

cd $B
for t in $TARGETS; do
  # The S3 calibration download for the partition bundle is gone: reuse Resnet18Test's profile.
  if [ $t = vtaMxnetResnet18PartitionBundle ]; then
    src=$B/vta/bundles/Resnet18Test/mxnet_exported_resnet18_calib_1.yaml
    dst=$B/vta/bundles/Resnet18PartitionTest/mxnet_exported_resnet18_calib_1.yaml
    [ -f $src ] && [ ! -f $dst ] && cp -p $src $dst
  fi
  s=$(date +%s)
  make -j2 $t > $LOG/$t.log 2>&1; rc=$?
  printf "%-40s %s %5ds\n" $t $([ $rc = 0 ] && echo OK || echo "FAIL(rc=$rc)") $(( $(date +%s) - s )) | tee -a $LOG/summary.txt
done
echo "== done $(date)" >> $LOG/summary.txt
```

## 부록 B. `build_hw_bundles.py` 전문 (복원, Python 3.6 수정 반영)
```python
#!/usr/bin/env python3
"""Build every vta/bundles bundle for the ZCU102 EVTA, outside CMake.

Bundle sources/data are taken from the simulator build (Debug2_remote/vta/bundles/<Dir>, produced by
build_sim_bundles.sh) and from the source tree (vta/bundles/<Dir>). The EVTA runtime (vtalib ZCU102
driver + VTABundle) is compiled once into hw/common with the same -D macros / flags as
Conv3Test/build.sh (= NESTC_EVTA_RUN_ON_ZCU102=ON branch of vta/vtalib/CMakeLists.txt, non-coherent).

Output: hw/<Dir>/<exe> plus its data files, and hw/run_all.sh (sudo) which reloads the bitstream,
runs each bundle and compares with the golden where the CMake test does (USE_DIFF).

usage: ./build_hw_bundles.py [Dir ...]
"""
import os, re, shlex, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
R = '/home/xilinx/JO/npuprg/nestc2_clion/nest-compiler'
SRC = f'{R}/vta/bundles'
BLD = f'{R}/Debug2_remote/vta/bundles'
HW = f'{HERE}/hw'
SKIP = {'BinaryNet', 'BiRealNet', 'Resnet18RelayEVTAPartitionTest'}   # BNN / Relay(TVM)
DATA_EXT = ('.cpp', '.h', '.bin', '.o')


def flags():
    """Reuse the flag/define/include set of Conv3Test/build.sh (single source of truth)."""
    sh = open(f'{HERE}/Conv3Test/build.sh').read()
    block = sh[sh.index('\nR='):sh.index('\nmkdir -p obj')]
    out = subprocess.run(['bash', '-c', block + '\necho "$CXX $FLAGS $DEFS $INCS"'],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, check=True).stdout
    return ' '.join(out.split())


def parse(d):
    s = re.sub(r'#.*', '', open(f'{SRC}/{d}/CMakeLists.txt').read())
    exes = {n: srcs.split() for n, srcs in re.findall(r'add_executable\(\s*(\S+)\s+([^)]*)\)', s)}
    tests = {}
    for t in re.findall(r'add_nestc_test\(([^)]*)\)', s):
        a = t.split()
        name = a[a.index('NAME') + 1]
        params = a[a.index('PARAMS') + 1] if 'PARAMS' in a else ''
        golden = a[a.index('DIFF_TARGET') + 1] if 'DIFF_TARGET' in a else ''
        tests[name] = (params, golden)
    return exes, tests


def resolve(p, d):
    """Map a CMake path to (kind, value): 'local' file name in hw/<d> or an absolute path."""
    p = p.replace('${GLOW_SOURCE_DIR}', f'{R}/glow').replace('${NESTC_ROOT_DIR}', R)
    for pre in ('${CMAKE_CURRENT_BINARY_DIR}/', '${CMAKE_CURRENT_SOURCE_DIR}/'):
        if p.startswith(pre):
            return os.path.basename(p)
    return p


def main():
    cc = flags()
    os.makedirs(f'{HW}/common', exist_ok=True)
    V = f'{R}/vta/vtalib'
    common = []
    for f in (f'{V}/lib/zcu102/api.cc', f'{V}/lib/zcu102/pynq_driver.cc',
              f'{V}/lib/Bundle/VTABundle.cpp', f'{V}/lib/Bundle/CPUBundle_generic.cpp'):
        o = f'{HW}/common/{os.path.basename(f)}.o'
        print('  CXX common', os.path.basename(f), flush=True)
        subprocess.run(f'{cc} -c {f} -o {o}', shell=True, check=True)
        common.append(o)

    dirs = sys.argv[1:] or sorted(x for x in os.listdir(SRC)
                                  if os.path.isfile(f'{SRC}/{x}/CMakeLists.txt') and x not in SKIP)
    runs, report = [], []
    for d in dirs:
        exes, tests = parse(d)
        # the bundle executable is the one a test runs (skips the *Profiling variants)
        cand = [n for n in exes if n in tests] or list(exes)
        if not cand:
            continue
        exe = cand[0]
        out = f'{HW}/{d}'
        shutil.rmtree(out, ignore_errors=True)
        os.makedirs(out)
        # data + generated sources: build dir first, then source-dir .bin/.h/.cpp not yet present
        for base in (f'{BLD}/{d}', f'{SRC}/{d}'):
            if os.path.isdir(base):
                for f in os.listdir(base):
                    if f.endswith(DATA_EXT) and os.path.isfile(f'{base}/{f}') and not os.path.exists(f'{out}/{f}'):
                        shutil.copy2(f'{base}/{f}', out)
        srcs = []
        for s in exes[exe]:
            name = os.path.basename(s)
            if s.startswith('${CMAKE_CURRENT_BINARY_DIR}/') or not os.path.exists(f'{SRC}/{d}/{s}'):
                srcs.append(f'{out}/{name}')          # generated (copied from the build dir)
            else:
                srcs.append(f'{SRC}/{d}/{s}')          # checked-in main / bundle source
        missing = [s for s in srcs if not os.path.exists(s)]
        if missing:
            report.append(f'{d:30s} SKIP  missing {", ".join(os.path.basename(m) for m in missing)}')
            continue
        objs = []
        ok = True
        for s in srcs:
            if s.endswith('.o'):
                objs.append(s)
                continue
            o = f'{out}/{os.path.basename(s)}.obj.o'
            r = subprocess.run(f'{cc} -I{out} -I{SRC}/{d} -c {s} -o {o}', shell=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
            if r.returncode:
                open(f'{out}/build.err', 'a').write(r.stderr)
                ok = False
                break
            objs.append(o)
        if ok:
            r = subprocess.run(f'{cc} {" ".join(objs + common)} -o {out}/{exe} -L/usr/lib/llvm-8/lib '
                               '-lLLVMSupport -lpng -lcma -lglog -lgflags -lgomp -lpthread -ltinfo -ldl -lz',
                               shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
            if r.returncode:
                open(f'{out}/build.err', 'a').write(r.stderr)
                ok = False
        report.append(f'{d:30s} {"OK   " if ok else "FAIL "} {exe}' + ('' if ok else f'  (see hw/{d}/build.err)'))
        print(report[-1], flush=True)
        if ok:
            params, golden = tests.get(exe, ('', ''))
            runs.append((d, exe, resolve(params, d) if params else '', resolve(golden, d) if golden else ''))

    with open(f'{HW}/run_all.sh', 'w') as f:
        f.write('#!/bin/bash\n# Run all ZCU102 bundles (generated by build_hw_bundles.py). usage: sudo ./run_all.sh [Dir ...]\n'
                'set -u\ncd "$(dirname "$0")"\nBIN=zcu102_1x16_i8w8a32_16_16_19_18.bin\n'
                '[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }\nONLY="$*"\n: > results.txt\n'
                'run() {  # dir exe input golden\n'
                '  [ -n "$ONLY" ] && [[ " $ONLY " != *" $1 "* ]] && return\n'
                '  echo "================ $1 ($2)"\n'
                '  echo 0 > /sys/class/fpga_manager/fpga0/flags; echo $BIN > /sys/class/fpga_manager/fpga0/firmware\n'
                '  python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()" 2>/dev/null\n'
                '  ( cd $1 && rm -f output.bin && timeout 300 ./$2 $3 > run.log 2>&1; echo $? > rc )\n'
                '  rc=$(cat $1/rc); chown -R --reference=. $1\n'
                '  if [ $rc = 124 ]; then res=TIMEOUT\n'
                '  elif [ $rc != 0 ]; then res="FAIL(rc=$rc)"\n'
                '  elif [ -n "$4" ]; then cmp -s $1/output.bin $1/$4 && res=PASS || res=MISMATCH\n'
                '  else res="RAN (no golden; see $1/run.log)"; fi\n'
                '  tail -3 $1/run.log; printf "%-30s %s\\n" $1 "$res" | tee -a results.txt\n}\n')
        for d, exe, inp, gold in runs:
            f.write(f'run {d} {exe} {shlex.quote(inp)} {shlex.quote(gold)}\n')
        f.write('echo; echo "== summary"; cat results.txt\n')
    os.chmod(f'{HW}/run_all.sh', 0o755)
    open(f'{HW}/build_report.txt', 'w').write('\n'.join(report) + '\n')
    print('\n== build report'); print('\n'.join(report))


if __name__ == '__main__':
    main()
```

## 부록 C. `Conv3Test/build.sh` 전문 (복원, build_hw_bundles.py가 의존)
```bash
#!/bin/bash
# Build vtaConv3TestBundle for the ZCU102 EVTA hardware, outside the NEST-C CMake tree.
#
# Bundle sources (vtaConv3TestBundle.cpp, *.bin, *MainEntry.h, VTARuntime.h) were generated by
# Debug2_remote/tests/VTASaverTest --gtest_filter=VTASaverTest.convTest3 (simulator build).
# The EVTA runtime is compiled here with the same -D macros / flags as the
# NESTC_EVTA_RUN_ON_ZCU102=ON branch of vta/vtalib/CMakeLists.txt
# (single EVTA, non-BNN, NESTC_EVTA_RUN_WITH_GENERIC_BUNDLE=ON, Release), except
# VTA_COHERENT_ACCESSES defaults to false here: this board/bitstream is not cache-coherent
# (coherent=true hangs), so the runtime must flush/invalidate caches explicitly.
#
# usage: ./build.sh            then: sudo ./vtaConv3TestBundle vtaConv3TestInput.bin
set -e
cd "$(dirname "$0")"

R=/home/xilinx/JO/npuprg/nestc2_clion/nest-compiler
V=$R/vta/vtalib
CXX=clang++-8
JOBS=${JOBS:-2}

DEFS="-DNESTC_USE_VTASIM=0 -DDMLC_USE_FOPEN64=0 -DVTA_TARGET=zcu102 -DVTA_HW_VER=0.0.1
 -DVTA_LOG_INP_WIDTH=3 -DVTA_LOG_WGT_WIDTH=3 -DVTA_LOG_ACC_WIDTH=5 -DVTA_LOG_BATCH=0
 -DVTA_LOG_BLOCK=4 -DVTA_LOG_UOP_BUFF_SIZE=16 -DVTA_LOG_BLOCK_IN=4 -DVTA_LOG_BLOCK_OUT=4
 -DVTA_LOG_OUT_WIDTH=3 -DVTA_LOG_OUT_BUFF_SIZE=16 -DVTA_LOG_BUS_WIDTH=7 -DVTA_DEVICE_NUM=4
 -DVTA_IP_REG_MAP_RANGE=0x1000
 -DVTA_FETCH_ADDR=0xA0000000 -DVTA_LOAD_ADDR=0xA0001000 -DVTA_COMPUTE_ADDR=0xA0002000 -DVTA_STORE_ADDR=0xA0003000
 -DVTA_FETCH_ADDR_1=0xA0010000 -DVTA_LOAD_ADDR_1=0xA0011000 -DVTA_COMPUTE_ADDR_1=0xA0012000 -DVTA_STORE_ADDR_1=0xA0013000
 -DVTA_FETCH_ADDR_2=0xA0020000 -DVTA_LOAD_ADDR_2=0xA0021000 -DVTA_COMPUTE_ADDR_2=0xA0022000 -DVTA_STORE_ADDR_2=0xA0023000
 -DVTA_FETCH_ADDR_3=0xA0030000 -DVTA_LOAD_ADDR_3=0xA0031000 -DVTA_COMPUTE_ADDR_3=0xA0032000 -DVTA_STORE_ADDR_3=0xA0033000
 -DVTA_FETCH_INSN_COUNT_OFFSET=16 -DVTA_FETCH_INSN_ADDR_OFFSET=24 -DVTA_LOAD_INP_ADDR_OFFSET=16
 -DVTA_LOAD_WGT_ADDR_OFFSET=24 -DVTA_COMPUTE_DONE_WR_OFFSET=16 -DVTA_COMPUTE_DONE_RD_OFFSET=24
 -DVTA_COMPUTE_UOP_ADDR_OFFSET=32 -DVTA_COMPUTE_BIAS_ADDR_OFFSET=40 -DVTA_STORE_OUT_ADDR_OFFSET=16
 -DVTA_COHERENT_ACCESSES=${VTA_COHERENT:-false} -Dvta_EXPORTS
 -DVTA_LOG_INP_BUFF_SIZE=16 -DVTA_LOG_WGT_BUFF_SIZE=19 -DVTA_LOG_ACC_BUFF_SIZE=18
 -DPROFILE_ENABLE=0"
INCS="-I. -I$R -I$V/include -I$V/include/zcu102 -I$V/include/Bundle
 -I$V/3rdparty/dlpack/include -I$V/3rdparty/dmlc-core/include -isystem /usr/lib/llvm-8/include"
FLAGS="-std=c++14 -faligned-new -O3 -fPIC -fopenmp=libgomp -Wno-psabi -mtune=cortex-a53 -march=armv8-a+crc -ftree-vectorize -w"

mkdir -p obj
SRCS="$V/lib/zcu102/api.cc $V/lib/zcu102/pynq_driver.cc $V/lib/Bundle/VTABundle.cpp
 $V/lib/Bundle/CPUBundle_generic.cpp $R/vta/bundles/Conv3Test/vtaConv3TestBundleMain.cpp
 vtaConv3TestBundle.cpp"
export CC_LINE="$(echo $CXX $FLAGS $DEFS $INCS)"   # flatten newlines
echo $SRCS | tr ' ' '\n' | xargs -P "$JOBS" -I{} sh -c \
  'echo "  CXX $(basename {})"; $CC_LINE -c {} -o obj/$(basename {}).o'

echo "  LINK vtaConv3TestBundle"
$CXX $FLAGS obj/*.o -o vtaConv3TestBundle \
  -L/usr/lib/llvm-8/lib -lLLVMSupport -lcma -lglog -lgflags -lgomp -lpthread -ltinfo -ldl -lz
echo "built $(pwd)/vtaConv3TestBundle"
```

## 부록 D. `Conv3Test/run.sh` 전문 (복원)
```bash
#!/bin/bash
# Run vtaConv3TestBundle on the ZCU102 EVTA and compare with the simulator golden.
# Reloads the bitstream first: a previous hung run leaves the auto-restart EVTA blocks
# deadlocked, and every later run on that PL state hangs too.
# usage: sudo ./run.sh
set -u
cd "$(dirname "$0")"
BIN=zcu102_1x16_i8w8a32_16_16_19_18.bin   # in /lib/firmware

if [ "$(id -u)" != 0 ]; then echo "run with sudo"; exit 1; fi

echo 0 > /sys/class/fpga_manager/fpga0/flags
echo $BIN > /sys/class/fpga_manager/fpga0/firmware
python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()" 2>/dev/null
rm -f output.bin

timeout 60 ./vtaConv3TestBundle vtaConv3TestInput.bin; rc=$?
echo "exit=$rc"
[ $rc = 124 ] && { echo "RESULT: TIMEOUT"; exit 1; }
[ -f output.bin ] || { echo "RESULT: no output.bin"; exit 1; }
chown --reference=. output.bin

if cmp -s output.bin vtaConv3TestGolden.bin; then
  echo "RESULT: PASS (output.bin == vtaConv3TestGolden.bin)"
else
  python3 - <<'EOF'
o = open('output.bin', 'rb').read(); g = open('vtaConv3TestGolden.bin', 'rb').read()
n = min(len(o), len(g))
bad = [i for i in range(n) if o[i] != g[i]]
print(f"RESULT: FAIL  sizes out={len(o)} golden={len(g)}  mismatches={len(bad)}/{n}  zeros(out)={o.count(0)}")
for i in bad[:8]:
    print(f"  [{i}] out={o[i]-256 if o[i]>127 else o[i]} golden={g[i]-256 if g[i]>127 else g[i]}")
EOF
fi
```

나머지 `hwtest/` 파일(rawtest.cpp, rawtest2.cpp, convvar.cpp, dump_regs.py, 실행 스크립트)은 같은 폴더의 `recovered-scripts/`에 있습니다.
