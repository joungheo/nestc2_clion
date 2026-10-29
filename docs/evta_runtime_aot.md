# EVTA driver/runtime 구조 비교와 AOT 전환 검토

작성 2026-10-28. 대상: `vtalib-zcu102-*`(zcu102 runtime), NEST-C VTA backend(`lib/Backends/VTA/VTASave.cpp`), ZCU102 보드.

## 1. 요약

- **지금 구조는 JIT다.** bundle이 conv마다 `convolution_wo_tr(...)`를 부르면 vtalib가 그 자리에서 CPU로 명령과 uop를 만들고,
  끝에서 `VTASynchronize` 한 번으로 EVTA를 돌린 뒤 polling으로 기다린다. 명령 생성과 EVTA 실행은 겹치지 않는다.
- **측정(Resnet18, 보드, 333 MHz HLS 비트스트림):** 명령 생성과 실행 시작에 드는 CPU 시간은 conv당 0.056~0.111 ms,
  19개 합계 **1.62 ms**다. 추론 134.8 ms의 **1.2 %**, conv 호출 시간의 6.3 %다.
  같은 추론에서 CPU float conv0이 76.7 ms(57 %), transpose가 12.3 ms(9 %), CPU 연산(elemadd, relu, pool 등)이 15.9 ms다.
- **결론:** AOT로 바꾸는 일 자체로 얻는 것은 지금 1~2 %다. end-to-end에는 아래 두 가지가 훨씬 크다.
  - conv0을 EVTA로 옮기기
  - layer 사이 activation을 VTA layout으로 두어 transpose와 `VTABufferAlloc`을 없애기

  AOT는 그다음에, 혹은 그 작업과 같이 할 때 의미가 커진다.
  - EVTA가 빨라질수록(systolic 400 MHz, 더 큰 GEMM) 고정된 CPU 비용의 비율이 커진다.
  - 그래프 구간을 하나로 제출하려면 명령을 미리 만들어 두어야 한다.
- **AOT 노력(추정):**
  - 0단계: vtalib 안의 record/replay. **3~5일.** 컴파일러는 바꾸지 않는다.
  - 1단계: 컴파일 때 명령 blob을 만든다. **2~4주.**
  - 2단계: HAL, 커널 드라이버, 그래프 단위 제출. **수개월.**

  하드웨어는 바꿀 필요가 없다. load/store의 DRAM 주소는 base register 기준 상대값이라 위치 독립 명령을 만들 수 있다(§5.1).

## 2. 현재 구조 (코드 근거)

```
bundle .cpp (VTASave.cpp가 생성)
  VTABufferAlloc → transpose_nhwc2vtaio → VTABufferFlush → convolution_wo_tr(shape, tile, vtaCmdH, nThread, tileH, tileW, idEVTA)
  → VTABufferInvalidate → transpose_vtaio2nhwc → VTABufferFree                 (conv 하나마다 이 순서)
vtalib  lib/Bundle/VTABundle.cpp        conv_kernel(): tile 루프마다 VTALoadBuffer2D / VTAPushGEMMOp / VTAPushALUOp / VTADepPush·Pop
        include/zcu102/cmd_queue.h      CommandQueue: insn_queue_, uop_queue_ 에 256비트 명령을 쌓음, Synchronize()에서 FINISH 추가 후 실행
        include/zcu102/uop_kernel.h     uop kernel 캐시(context 구조체 비교로 찾음)
driver  lib/zcu102/pynq_driver.cc       cma_alloc(libcma, /dev/mem), VTAWriteMappedReg, VTADeviceRun(start 후 done polling)
```

- 명령 생성은 실행할 때마다 다시 한다.
  - `VTABundle.cpp:342` `conv_kernel`이 tile, 입력 채널, 가상 스레드마다 명령을 push한다.
  - `VTABundle.cpp:2547` `convolution_wo_tr`은 끝에서 `VTASynchronize(vtaCmdH, 1 << 31)`를 부른다.
- uop는 캐시된다.
  - `PushGEMMOp`(`cmd_queue.h:310`)은 signature로 `UopKernel`을 찾고, 없을 때만 `finit`(예: `gemmop_mxn`)으로 uop를 만든다.
  - 찾는 방법은 `vGemmUOpHandle[idEVTA]` 벡터의 선형 탐색이다.
- 명령에는 **절대 물리 주소**가 들어간다.
  - `cmd_queue.h:80` `insn->dram_base = src->phy_addr() / elem_bytes + offset`
  - 버퍼가 바뀌면 명령도 바뀌는 구조다.
- 실행: `pynq_driver.cc:299-323`이 명령 주소와 개수, 그리고 base register(`VTA_LOAD_INP_ADDR_OFFSET` 등, 모두 0)를 쓴 뒤 `VTA_START`를 쓰고 done을 polling한다.
  - interrupt는 없다.
  - 사용자 공간 `/dev/mem`이라 sudo가 필요하다.
  - 동시 실행은 `/tmp/evta_run.lock`으로만 막는다.
- activation은 conv마다 `VTABufferAlloc`/`Free`하고 NHWC ↔ VTA layout transpose를 두 번 한다. weight는 init에서 한 번 할당한다.
- vtalib 사본이 4개다: `vta/vtalib`, `vtalib-zcu102-single`, `-multi2`, `-multi4`. runtime을 고치면 넷 다 고치거나 하나로 합쳐야 한다.

## 3. 측정

### 3.1 Resnet18 추론 한 번의 시간 분해

`hw/Resnet18Test`(single EVTA, `vtahw_single_y219.bin`, 333 MHz HLS GEMM)의 측정 두 개를 비교했다.

- `_timed`: statement별 wall 시간, 10-26 측정, 두 번째 run
- `_prof`: `VTA_SYNC`, 즉 EVTA run 시작부터 done까지. 10-27 측정, 두 번째 run

두 측정은 날짜가 달라 ±수 % 오차가 있을 수 있다.

| 항목 | 시간 (ms) | 비율 | 비고 |
|---|---:|---:|---|
| conv0 (`convolutionFloat`, CPU) | 76.65 | 56.9 % | 첫 conv(입력 3채널)는 EVTA로 안 감 |
| CPU 연산 (elemadd 4.63, maxpool 3.29, quantize 2.94, relu 2.51, fc 1.21, 기타 1.30) | 15.88 | 11.8 % | |
| transpose (`nhwc2vtaio` 4.70 + `vtaio2nhwc` 7.60) | 12.31 | 9.1 % | conv마다 두 번 |
| `VTABufferAlloc`/`Free` (각 38회) | 3.61 | 2.7 % | conv마다 CMA 할당 |
| **EVTA 하드웨어 시간 (VTA_SYNC 19회)** | 24.16 | 17.9 % | |
| **명령 생성 + 실행 시작 (conv 호출 − VTA_SYNC)** | **1.62** | **1.2 %** | JIT 비용 |
| 합계 / 측정한 추론 시간 | 134.2 / 134.8 | | |

### 3.2 conv별 JIT 비용

conv 호출 wall 시간에서 같은 conv의 EVTA 시간을 뺐다(두 번째 run, ms).

| conv | 호출 | EVTA | 차이 |
|---|---:|---:|---:|
| stage1 3x3 (56x56x64), 4개 | 1.866~1.884 | 1.773~1.774 | 0.093~0.111 |
| stage2 3x3 (28x28x128), 3개 | 1.677~1.681 | 1.587~1.588 | 0.090~0.093 |
| stage3 3x3 (14x14x256), 3개 | 1.580~1.581 | 1.487~1.488 | 0.092~0.094 |
| stage4 3x3 (7x7x512), 3개 | 1.544~1.547 | 1.445~1.446 | 0.098~0.102 |
| 1x1 downsample, 3개 | 0.324~0.424 | 0.267~0.356 | 0.057~0.068 |
| stride 2 3x3, 3개 | 0.824~1.078 | 0.768~1.008 | 0.056~0.070 |

- 이 차이에는 명령·uop 생성, 명령 버퍼 flush, 레지스터 쓰기, polling 간격이 모두 들어 있다. 따로 나눠 재지는 않았다.
- 첫 run의 conv 합계는 26.95 ms로 두 번째보다 1.17 ms 길다. uop kernel을 처음 만들고 캐시가 비어 있는 탓으로 추정한다.
- multi4 ResConv10(`docs/evta_multi4_609gops_report.md`)에서는 conv 1.516 ms, EVTA 1.447 ms로 차이가 0.069 ms(4.6 %)다.
  systolic 400 MHz로 EVTA 시간이 줄면 같은 0.07 ms의 비율이 5~6 %로 커진다(추정).

### 3.3 해석

- 지금 크기의 GEMM(1x16·16x16)과 tile에서는 conv 하나에 명령이 수백 개라 CPU가 0.1 ms 안에 만든다. JIT 자체는 병목이 아니다.
- 병목은 EVTA 밖의 CPU 일이다(합계 80 % 이상). runtime 구조에서 문제가 되는 부분은 명령 생성이 아니라 다음 셋이다.
  - **layer마다 layout을 바꿈:** transpose
  - **layer마다 할당:** `VTABufferAlloc`
  - **CPU와 EVTA가 번갈아 기다림:** 겹치지 않음
- EVTA가 빨라질수록 고정 CPU 비용의 비율이 커진다. 예를 들어 GEMM을 4배로 키우면 EVTA 시간은 줄지만 명령 수는 비슷하다.

## 4. 다른 프로젝트의 driver/runtime과 공통 API

일반 지식으로 쓴 것이다. 원문은 이번에 다시 확인하지 않았다.

| | 명령 생성 | 제출·동기화 | 메모리 | 커널 드라이버 | 다른 하드웨어 붙이기 |
|---|---|---|---|---|---|
| VTA / 우리 vtalib | 실행 중 CPU(JIT), uop만 캐시 | 연산 묶음마다 Synchronize, polling | 사용자 공간 `/dev/mem`, CMA, 수동 flush | 없음 | driver 층이 얇아 포팅은 쉬움 |
| NVDLA | 오프라인 컴파일러 → loadable(AOT) | 레이어 그래프 통째 제출, KMD가 interrupt로 엔진 간 의존 처리 | 커널 GEM/dma-buf | KMD + UMD | NVDLA 레지스터 맵에 묶임 |
| Gemmini | C 매크로·컴파일러가 RoCC 명령 | CPU 파이프라인 안, fence | 가상 메모리(공유 TLB) | 필요 없음 | RISC-V Rocket에 묶임 |
| TPU (XLA) | AOT 프로그램 | 프로그램 단위 비동기 | runtime 관리 | 있음(비공개) | PJRT plugin API |
| Xilinx XRT | 사용자 커널(xclbin) | `run.start/wait`, interrupt | buffer object + sync | `zocl`(MPSoC) | FPGA 범용 |

공통 API 후보:

- **IREE HAL:** Vulkan과 비슷한 구조다.
  - allocator, buffer
  - command buffer, executable
  - queue, semaphore
- **OpenXLA PJRT:** plugin마다 compile과 execute를 제공한다.
- **TVM UMA:** TVM에 가속기를 붙이는 공식 경로다.
- **Linux DRM accel:** `/dev/accel`, 2023년부터 커널에 들어 있다.
- **Glow Backend/DeviceManager:** 이미 쓰고 있다.

공통 API의 핵심은 가속기마다 다른 것을 **opaque executable**(명령 blob)과 그것을 만드는 컴파일러 backend에 몰아넣는 것이다. 그러면 나머지는 하드웨어와 무관해진다.

- buffer 할당과 sync
- executable 적재
- dispatch(executable, bindings)
- 제출과 fence 대기
- reset
- perf counter

## 5. AOT 전환

### 5.1 하드웨어 조건: 이미 된다

- HLS `load`/`store`/`compute`는 `m_axi ... offset = slave`이다(`vta-hw-zcu102-systolic/hardware/xilinx/src/vta.cc:177`).
  - 메모리 주소는 "base register + `insn.dram_base` × 원소 크기"로 계산한다.
  - 지금은 base를 0으로 두고 `dram_base`에 절대 주소를 넣는다(`pynq_driver.cc:300`, `cmd_queue.h:80`).
- 그러므로 다음 둘 중 하나로 명령을 위치 독립으로 만들 수 있다.
  - activation, weight, bias, uop를 각각 한 arena에 두고 base register에 arena 주소를 쓴다.
  - 명령 blob에 relocation 표(어느 명령의 `dram_base`에 어느 버퍼의 주소를 더할지)를 붙인다.
- 그래서 하드웨어 수정은 필요 없다. 다만 base register가 0이 아닐 때의 동작은 보드에서 확인해야 한다(미확인).

### 5.2 단계별 수정 범위와 노력

| 단계 | 내용 | 바뀌는 곳 | 규모·기간(추정) | 얻는 것 | 위험 |
|---|---|---|---|---|---|
| **0. record/replay (vtalib 안)** | conv 호출 위치마다 첫 실행의 명령·uop 스트림과 relocation(입력·weight·bias·출력 4개)을 기록한다. 다음부터는 복사하고 주소만 고쳐 flush·실행한다 | `cmd_queue.h`(기록 모드, Load/Store에 relocation 표시), `VTABundle.cpp` conv 진입 함수 6개(캐시 키 = 인자), uop 버퍼 고정 | 300~500줄, **3~5일**(보드 검증 포함). vtalib 사본 4개 반영은 별도 | JIT 비용 1.6 ms → 수십 µs(추정), 첫 run 1.2 ms. 1단계 형식의 시험대 | 낮음. 키가 틀리면 결과가 틀리는데, golden 비교로 잡힘 |
| **1. 컴파일 때 생성 (진짜 AOT)** | NEST-C가 bundle을 만들 때 x86에서 vtalib 명령 생성기를 돌려 conv마다 `.insn` blob과 relocation을 bundle에 넣는다. bundle은 `VTARunProgram(blob, bufs)`만 부른다 | zcu102 `cmd_queue.h`/`uop_kernel.h`를 host에서도 빌드(가짜 allocator), `VTASave.cpp`의 conv 생성 4곳(`:2144, :2285, :2420, :2567`), bundle `CMakeLists`/weights 형식, multi-EVTA `idEVTA`, sim/hw 검증 경로 | 1,000~2,000줄, **2~4주** | 실행 중 생성 없음, 명령 검사·덤프를 컴파일 때 함, 그래프 단위 제출의 바탕 | 중간. host와 보드의 명령이 같아야 함(시뮬레이터 `lib/simulator/runtime.cc`는 다른 구현) |
| **2. HAL + 커널 드라이버 + 구간 제출** | §4 형태의 C HAL, UIO 또는 XRT `zocl` 드라이버(dma-buf, interrupt, reset), 여러 layer를 command buffer 하나로 제출 | 새 HAL 층, 드라이버, runtime 스케줄러, 컴파일러(구간 묶기) | **수개월** | sudo와 `/dev/mem` 제거, 동시 사용, hang 복구, CPU·EVTA 겹치기, 다른 오픈 하드웨어 붙이기 | 높음. 구간을 길게 하려면 transpose·elemadd 등도 EVTA에서 돌아야 효과가 있음 |

### 5.3 AOT보다 먼저 볼 것 (같은 측정 기준)

| 작업 | 줄어드는 시간 (Resnet18, 추정) | 성격 |
|---|---:|---|
| conv0을 EVTA로 (입력 3채널 → 16 padding) | 76.7 ms 중 대부분 | 컴파일러(VTA backend 지원 조건, quantization) |
| layer 사이 activation을 VTA layout으로 유지 | transpose 12.3 ms + alloc 3.6 ms | 컴파일러(layout 전파) + runtime(정적 메모리 계획) |
| elemadd, relu를 EVTA ALU로 | 7.1 ms 중 일부 | 컴파일러 + vtalib |
| AOT 0단계 | 1.6 ms | vtalib |

- layout 유지와 정적 메모리 계획은 AOT 1·2단계의 전제이기도 하다. 버퍼 위치가 고정되어야 명령을 미리 만들 수 있다.
- 순서는 conv0 → layout/메모리 계획 → AOT 1단계(필요하면 0단계를 시험대로) → HAL을 권한다.

**conv0 양자화 측정 (2026-10-28, 보드 single 333 MHz, `vtahw_single_y219`)**
- 지금 Resnet18 번들은 `VTA_RESNET18_WITH_SKIPQUANT0=ON`이라 `VTASkipQuantizeNodes.txt`가 전처리와 conv0, relu0을 양자화에서 뺀다 → `convolutionFloat`.
- skip 파일 없이 보드 `Debug2_remote/glow/bin/model-compiler`로 번들을 다시 만들었다(cmake 옵션을 바꾸면 Glow 전체가 다시 빌드되므로).
  결과는 `Debug2_remote/vta/bundles/Resnet18Test_q0/`, 보드 실행은 `vtabundles_xcmpl/hw/Resnet18Q0Test/`(빌드 명령은 build_timed, build_prof의 Resnet18 명령에서 경로만 바꿈).
- conv0은 int8이 되지만 입력 3채널이라 EVTA 조건에 안 맞아 CPU `nonvtaconvolution`으로 간다(`VTA_BNN`이 꺼져 있으면 `VTASave.cpp:3108`).

| | SKIPQUANT0=ON | SKIPQUANT0=OFF |
|---|---|---|
| 분류 (cat_285) | 281 | 281 |
| conv0 | `convolutionFloat` 76.2 ms | `nonvtaconvolution` 1153.7 ms |
| 추론 전체 (`_timed`, run 2) | 134.3 ms | 1210.3 ms |
| EVTA 시간 (`_prof`) | 48.37 ms | 48.38 ms |

- 양자화 자체는 이 이미지에서 결과를 바꾸지 않았다. 느려진 것은 CPU int8 conv가 최적화되지 않았기 때문이다(float 쪽은 1547 → 76 ms로 고쳐 둠).
  conv0을 EVTA로 보내야(입력 0 채움 3 → 16채널, `saveFirstBNNVTAConvolutionInst`와 같은 방식) 의미가 있다. 예상 약 8 ms.

**conv0을 EVTA로 (2026-10-29, 보드 single 380 MHz, `..._16_16_19_18_380mhz.bin`)**
- `VTASave.cpp` `saveConvolutionInst`: 3채널 int8 conv가 EVTA 조건(2의 거듭제곱 scale, offset 0, 출력 채널 16의 배수)을 만족하면
  `VTA_BNN` 없이도 `saveFirstBNNVTAConvolutionInst`로 보낸다(입력은 `transpose_nhwc2vtaio_zerofill`, 가중치는 16채널로 0 채움해 저장, `convolution_wo_tr`).
- 보드 `Debug2_remote`는 `make VTA/fast model-compiler/fast`로 다시 빌드했다(그냥 `make`는 `vta/vtalib/CMakeLists.txt`가 캐시보다 새로워 cmake와 Glow 전체를 다시 돌린다).
  번들 `Debug2_remote/vta/bundles/Resnet18Test_q0e/`, 보드 실행 `vtabundles_xcmpl/hw/Resnet18Q0ETest/`. weights.bin은 64×7×7×13 B(0 채움분) 커졌다.

| | conv0 float (기본) | conv0 int8 CPU (Q0) | conv0 int8 EVTA (Q0E) |
|---|---|---|---|
| 분류 (cat_285) / confidence | 281 / 0.588 | 281 / 0.533 | 281 / 0.656 |
| conv0 | `convolutionFloat` 76.3 ms | `nonvtaconvolution` 1156 ms | zerofill 6.7 + `convolution_wo_tr` 13.5 + 출력 transpose 3.5 ms |
| 추론 전체 (`_timed`, run 2) | 131.8 ms | 1209.8 ms | **78.4 ms** |
| EVTA 시간 (`_prof`, 추론 2회분) | 42.94 ms | 42.95 ms | 68.57 ms (conv0 1회 12.8 ms) |

- 추론 전체가 기본보다 40 % 짧아졌다. conv0 EVTA는 16채널로 계산해 13/16이 0이라 실효 236 Mops / 13.5 ms = 17.5 GOPS(예상 8 ms보다 느림).
  남은 CPU 몫인 zerofill(6.7 ms)은 3채널 NHWC를 바로 16채널 블록으로 쓰면 줄일 수 있다(지금은 NCHW를 거쳐 malloc 두 번).
- Q0와 Q0E는 conv0만 다른데 confidence가 다르다(0.533 / 0.656). conv0 출력(relu 후, int8 scale 1/32)을 덤프해 비교했다
  (`hw/<dir>/dump_*`: 번들 .cpp에 fwrite 한 줄을 넣은 `_prof` 빌드, 기준은 기본 번들의 float 출력 × 32):

  | | float × 32 와의 차 평균 | 평균 \|차\| | 최대 \|차\| | 일치 |
  |---|---|---|---|---|
  | Q0 (CPU `nonvtaconvolution`) | +0.015 | 0.315 | 3.77 | round(float×32)와 75.8 % |
  | Q0E (EVTA) | −0.340 | 0.402 | 3.93 | floor(float×32)와 77.2 % |

  Q0E − Q0는 0(64.5 %) 아니면 −1(35.5 %)뿐이다. EVTA는 shift로 내림(산술 시프트), CPU는 반올림한다. 계산 오류가 아니라 반올림 방식 차이이며,
  conv0 뒤 층은 세 번들 모두 같은 EVTA(내림)라 confidence 차이는 conv0 반올림 방식에서 온다. 어느 쪽이 더 정확한지는 이미지 하나로는 말할 수 없다.

**기본 번들도 conv0을 EVTA로 (2026-10-29, `f126a2b6`)**
- 소스의 `VTA_RESNET18_WITH_SKIPQUANT0` 기본값은 원래 OFF였고, 보드 `Debug2_remote` 캐시만 ON이었다. OFF로 바꿨다.
- model-compiler는 skip 목록을 cwd의 `./VTASkipQuantizeNodes.txt`에서 읽는다. OFF는 복사만 안 해서 예전 파일이 남으면 conv0이 계속 float였다
  → OFF면 그 파일을 지우게 했다(`vta/bundles/Resnet18Test`). multi-EVTA Resnet18(x2, x4)은 옵션과 상관없이 복사하던 것을 옵션을 따르게 했다.
  `Resnet18PartitionTest`는 그대로 skip 목록을 쓴다(분할 계획이 conv0 float를 전제로 한다).
- x4(`MultiEVTAResnet18Test`)는 sim 빌드의 target이 아니다(`NESTC_EVTA_MULTI=OFF`). copy 0/1은 x2에서 복사하고, 2/3은 model-compiler를
  `-index-multi-evta=4|8 -network-name=mxnet_exported_resnet18_2|_3`으로 직접 돌려 만들었다(skip 파일 없는 dir에서).
  예전 번들 소스는 보드 `/home/xilinx/JO/skipq0_backup/`.
- `vtabundles_xcmpl/build_multievtax4{,_prof,_timed}`의 캐시에 지운 `vtalib-zcu102-multi4/lib/acl`의 `EXTERN_ACL_*` 경로가 남아 링크가 깨졌다.
  `cmake -UEXTERN_ACL_* -Uvta_runtime_LIB_DEPENDS .`로 고쳤다(나머지 build dir은 해당 없음).
- 보드 결과(380 MHz 기본 비트스트림): golden single 30 PASS(Resnet18 281 / 0.656, Resnet18Partition·Resnet50 281), multi2·multi4 PASS,
  Resnet18 multi/single 100회 모두 281(rc 0, "maxIdx" 0줄).

| 번들 | EVTA 시간 전 (10-28, 8)) | 후 | 추론 전체 |
|---|---|---|---|
| single Resnet18 (추론 2회) | 42.94 ms | 68.56 ms | `_timed` run 2: 131.8 → **78.6 ms** |
| multi2 Resnet18 x2 | 1053.49 ms | 1719.76 ms | 100회 6.60 s(`OMP_NUM_THREADS=1`) |
| multi4 Resnet18 multi | 516.57 ms | 937.76 ms | 100회 5.77 s |
| multi4 Resnet18 single | 1157.39 ms | 1779.75 ms | 100회 4.08 s |

  EVTA 시간은 conv0이 더해진 만큼 늘었다. multi 번들의 추론 전체는 예전 값이 기록에 없어 비교하지 못했다.
- systolic 400 MHz(`SYS_BIN`, `_sys*` 실행 파일): `build_sysprof`, `build_multievtax4_sys`의 `VTALIB`를 git 밖 사본
  `/home/xilinx/JO/npuprg/systolic/vtalib-zcu102-systolic`(주석만 다름)에서 저장소의 `vtalib-zcu102-systolic`으로 바꿔 다시 빌드했다.
  single `run_prof.sh -s _sysprof all` 30 PASS, Resnet18 281 / 0.656(HLS와 같음), multi4 `-s _sys all` PASS.

  | | HLS 380 | systolic 400 | 변화 |
  |---|---|---|---|
  | single Resnet18 EVTA 시간(추론 2회) | 68.56 ms | 65.99 ms | −3.8 % |
  | multi4 Resnet18 multi, 100회(`OMP_NUM_THREADS=1`, 2회 번갈아) | 4049 / 4033 ms | 4001 / 4004 ms | −1.0 % |
  | multi4 Resnet18 single, 100회 | 15367 / 15367 ms | 15276 / 15283 ms | −0.6 % |

  추론 전체는 CPU 몫(transpose, pool, fc 등)이 대부분이라 EVTA 클럭 이득이 거의 보이지 않는다.
- `_systimed`(문장별 시간, systolic 400): `build_systimed`, `build_multievtax4_systimed`를 새로 만들었다(`vtabundles_xcmpl/README.md`).
  `run_prof.sh -s _systimed`가 `SYS_BIN`으로 돈다. 같은 날 `_timed`(HLS 380)와:

  | | HLS 380 `_timed` | systolic 400 `_systimed` | 변화 |
  |---|---|---|---|
  | single Resnet18 run 2 전체 | 78.35 ms | 77.27 ms | −1.4 % |
  | 그중 `convolution_wo_tr` 합 | 36.64 ms | 35.49 ms | −3.1 % |
  | 그 밖 (CPU) | 41.67 ms | 41.72 ms | 0 |
  | multi4 Resnet18 multi, 마지막 run copy 0~3 (`OMP_NUM_THREADS=1`) | 161.8 / 159.3 / 157.1 / 158.8 ms | 156.9 / 156.4 / 159.0 / 155.4 ms | −1.4 % (평균 159.2 → 156.9) |
  | multi4 Resnet18 single, run 100 | 153.33 ms | 153.05 ms | −0.2 % |

  multi4 번들은 `OMP_NUM_THREADS=1`이라 CPU 몫이 single 셋(기본 OpenMP 4스레드)보다 커서 한 번에 약 150 ms다. 결과는 모두 281.

## 6. 정할 것

1. AOT 0단계를 시험으로 먼저 할지, 아니면 conv0과 layout 작업을 먼저 할지.
2. vtalib 사본 4개를 하나로 합칠지. runtime을 고치기 전에 합치면 이후 수정이 한 번으로 끝난다.
3. 명령 생성 시간을 더 정확히 나눠 잴지. `Synchronize` 진입 시각을 찍는 계측 빌드가 필요하고, 0.5일 정도 걸린다.
