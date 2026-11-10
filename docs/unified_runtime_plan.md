# 여러 하드웨어를 붙이는 unified runtime / driver 계획 (2026-10-28 ~ 11-10)

EVTA 말고도 다른 오픈소스 NPU / FPGA 가속기를 같은 runtime과 driver로 붙이기 위한 계획을 정리했다. 계획을 세운 날짜는 이렇다.

| 날짜 | 한 일 |
|---|---|
| 10-28 | 층 구조와 unified HAL을 처음 제안했다 |
| 11-02 | AOT 단계를 나눴다 |
| 11-03 | IREE, MLIR 계열, DRM accel을 원문으로 확인했다 |
| 11-05 | 하드웨어 5단계 계획을 세웠다 |

그 뒤 11-10까지 실제로 구현한 것과 DeiT 작업에서 새로 나온 요구도 넣었다.

- **근거 문서:** `docs/npu_stack_survey.md` §1–§3(조사 정리, 계속 고쳐 씀), `docs/evta_runtime_aot.md` §4–§5(AOT 단계), `docs/evta_runtime_compare.md` §4(다른 가속기의 제어 방식), §8(하드웨어 비교 항목)
- **확인 수준 표시:** survey와 같다. `[원문]`은 공식 문서나 소스로 확인한 것, `[측정]`은 보드에서 잰 것, `[일반 지식]`은 원문을 다시 확인하지 않은 것이다.
- **§5의 API 초안:** 아직 코드가 아닌 제안이다.

## 1. 왜 필요한가

- **목표**(11-03): 여러 하드웨어(EVTA, 다른 오픈소스 NPU, 다른 FPGA / ASIC 가속기)를 붙일 수 있는 컴파일러, runtime, driver를 만든다. 하드웨어 자체도 더 효율적으로 만든다.
- **지금 EVTA 스택:** 하드웨어 하나에 단단히 묶여 있다.

| 층 | 지금 | 문제 |
|---|---|---|
| 컴파일러 | Glow + NEST-C VTA backend. `VTASave.cpp`가 vtalib 호출 C++를 생성 | 장치가 바뀌면 backend와 bundle 형식이 통째로 바뀐다 |
| runtime | vtalib. conv마다 CPU가 명령을 만들고(JIT) record / replay로 두 번째부터 복사 | API가 conv 같은 연산 단위라 다른 가속기에 쓸 수 없다 |
| driver | 사용자 공간 `/dev/mem` + libcma, 완료는 polling | sudo가 필요하다. 여러 사용자가 충돌한다. hang이 나면 bitstream을 다시 올려야 한다. interrupt가 없다 |
| 하드웨어 제어 | AXI-Lite 레지스터에 명령 주소 / 개수를 쓰고 done을 polling | 장치마다 레지스터 맵이 다르다 |

- **결론**(10-28): 가속기마다 다른 것은 **opaque executable**(명령 blob)과 그것을 만드는 **컴파일러 backend**에 몰아넣는다. 나머지는 공통으로 한다. 공통으로 둘 것은 buffer, executable 적재, dispatch, 제출, fence, reset, perf다.

## 2. 다른 프로젝트 비교

### 2.1 가속기의 제어와 runtime (`evta_runtime_compare.md` §4, 원문 확인)

| | 하드웨어 제어 | 명령 단위 | 명령 생성 때 | 의존성 | 동기화 | 층 사이 layout | conv 밖 연산 |
|---|---|---|---|---|---|---|---|
| EVTA + NEST-C (10-29 당시) | DAE 4모듈 | task + uop | 추론 중 conv마다 CPU (JIT) | 명령 속 token | conv마다 polling | conv마다 CPU transpose | CPU |
| EVTA + NEST-C (11-10) | 같음 | 같음 | 첫 호출만 JIT, 이후 replay | 같음 | 호출마다 polling(여러 conv를 한 호출로 묶기도 함) | **EVTA layout 유지** | **ALU(relu, add, maxpool, fc, depthwise)**, 비선형은 CPU |
| VTA + TVM | 같음 | 같음 | JIT | 같음 | 연산 묶음마다 | 구간 입구·출구 한 번 | bias·shift·clip을 ALU |
| Gemmini | DAE 3 controller | RoCC 행렬 명령, CISC loop | 실행 중 CPU, loop는 HW FSM | HW ROB | CPU 파이프라인 안 | scratchpad 유지 | pool, relu, im2col, transpose를 HW |
| NVDLA | 층 레지스터 (ping-pong 두 벌) | 하드웨어 층 | 컴파일러가 층 설정 | 소프트웨어 | 층마다 interrupt | NVDLA 형식 | SDP / PDP / CDP 파이프라인 |
| Xilinx DPU | 명령 fetch | 컴파일러 명령 | AOT (xmodel) | 명령 안 (세부 비공개) | subgraph마다 | DPU 형식 | fusion, 대부분 DPU |

### 2.2 driver와 runtime 구조 (`[일반 지식]`, `evta_runtime_aot.md` §4)

| | 명령 생성 | 제출·동기화 | 메모리 | 커널 driver | 다른 하드웨어 붙이기 |
|---|---|---|---|---|---|
| VTA / 우리 vtalib | 실행 중 CPU(JIT), 지금은 replay | 연산 묶음마다 Synchronize, polling | 사용자 공간 `/dev/mem`, CMA, 수동 flush | 없음 | driver 층이 얇아 포팅은 쉬움 |
| NVDLA | 오프라인 컴파일러 → loadable(AOT) | 레이어 그래프 통째 제출, KMD가 interrupt로 엔진 간 의존 처리 | 커널 GEM / dma-buf | KMD + UMD | NVDLA 레지스터 맵에 묶임 |
| Gemmini | C 매크로·컴파일러가 RoCC 명령 | CPU 파이프라인 안, fence | 가상 메모리(공유 TLB) | 필요 없음 | RISC-V Rocket에 묶임 |
| TPU (XLA) | AOT 프로그램 | 프로그램 단위 비동기 | runtime 관리 | 있음(비공개) | PJRT plugin API |
| Xilinx XRT | 사용자 커널(xclbin) | `run.start/wait`, interrupt | buffer object + sync | `zocl`(MPSoC) | FPGA 범용, 우리 보드에 바로 맞음 |

**무엇이 나은가** (10-28 판단, 이후 측정으로 맞음):

| 항목 | 나은 쪽 | 우리 상태 (11-10) |
|---|---|---|
| 명령 생성 | AOT (DPU, NVDLA, TPU) | record / replay(AOT 0단계, 11-02). Resnet18 OMP 1 37.1 → 34.5 ms `[측정]` |
| 제출 단위 | command buffer 하나를 제출 (IREE / Vulkan) | 호출마다 제출. attention head 3개는 한 호출(`gConvBatch`, 11-10) |
| 완료 알림 | interrupt + fence | polling. `RunStart` / `RunWait`로 나눠 run 사이에 CPU 일을 겹침(11-06) |
| 메모리 | 커널이 관리하는 dma-buf | `/dev/mem` + libcma, buffer pool(11-01), 상수 buffer 공유(11-06), `VTABufferView`(11-10) |
| 여러 장치 | 장치 N개 열거, 장치마다 queue | EVTA i = 주소 + index. bundle이 EVTA 하나에 고정, `CheckLoadedBitstream`으로 EVTA 수 확인(10-31) |
| 층 사이 layout | 가속기 형식 유지 | 유지(11-01, `-vta-keep-layout`) |
| 후처리 | 가속기에서 | relu, residual add, maxpool, fc, depthwise, requantize를 EVTA로 |

DAE 구조(fetch / load / compute / store가 token으로 맞물림) 자체는 손해가 없다. Gemmini도 DAE다. 손해는 "conv 하나 = 함수 하나 = 동기화 한 번 = layout 왕복 한 번"이라는 runtime과 backend의 단위에서 났다. 그 단위를 고쳐 Resnet18 single이 73.6 ms(10-29, OMP 4)에서 23.3 ms(11-10, OMP 1)가 됐다 `[측정]`.

### 2.3 MLIR 계열: 컴파일러 아래에 runtime과 driver가 붙는 방식 (11-03, survey §2.3)

**MLIR과 LLVM은 컴파일러를 만드는 틀이다. runtime과 driver는 정해 주지 않는다.**

```
PyTorch / ONNX / TF
   │  front end (torch-mlir, StableHLO, TOSA)
MLIR 상위 dialect → linalg / tensor / memref (장치 무관 최적화: fusion, tiling, layout)
   │  장치별 dialect로 (IREE HAL dialect, AIE dialect, Intel VPU dialect)
   ├─(a) 프로그래머블 코어 코드 → LLVM IR → ELF        (CPU, GPU, NPU 안 VLIW/RISC-V 코어)
   └─(b) 고정 기능 명령, 제어 코드 → 전용 serializer → 명령 binary
   │
module 하나 (IREE .vmfb, AMD xclbin + 명령 스트림, Intel blob)
   │
runtime (user space): module 적재, buffer, command buffer, 제출, fence
UMD: Vulkan ICD, CUDA driver, XRT, Level Zero
KMD: DRM accel (/dev/accel), GEM buffer, 제출 ioctl, 완료 IRQ, firmware
HW : 명령 FIFO / 링 버퍼 + doorbell + IRQ
```

| 프로젝트 | 컴파일러 | 출력 | runtime | driver | 확인 |
|---|---|---|---|---|---|
| **IREE** | 입력 dialect → Flow(실행 영역 분할, dispatch) → Stream → **HAL dialect**("거의 1:1로 compute 전용 Vulkan API") → target backend | VM bytecode(스케줄) + 장치별 executable | IREE VM + HAL C API | HAL driver(local-task, vulkan, cuda, hip, metal). 외부 HAL driver를 plugin으로 | `[원문]` |
| **iree-amd-aie** | IREE 컴파일러 plugin. AIE 코어 코드는 Peano(LLVM-AIE) | — | IREE runtime + 외부 HAL driver `amdxdna` | `xdna-driver` 커널 모듈, XRT | `[원문]` README |
| **MLIR-AIE / IRON** (AMD Ryzen AI) | MLIR 기반 AIE toolchain, Python IRON | **xclbin + 명령 스트림**(배열 설정과 제어 코드) | XRT | **amdxdna** (DRM accel) | `[원문]` |
| **Intel NPU** | OpenVINO IR → MLIR 기반 NPU 컴파일러(compiler-in-driver) → blob | blob | OpenVINO NPU plugin → **Level Zero** UMD | **ivpu** + firmware | 구조는 `[원문]` |
| **OpenXLA** | StableHLO → 장치별 컴파일러 | 장치별 | **PJRT C API**: vendor가 plugin으로 구현, framework에는 opaque | 장치별 | `[원문]` 개요 |
| Triton | Triton → TritonGPU → LLVM → PTX / AMDGCN | cubin / hsaco | 얇은 launcher | CUDA / ROCm driver | `[일반 지식]` |
| torch-mlir | front end만 | MLIR | 없음 (IREE 등에 넘김) | — | `[일반 지식]` |

- **IREE에 붙이는 단위:** 새 장치는 **컴파일러 target plugin + 외부 HAL driver 한 쌍**으로 붙인다. iree-amd-aie가 실제 예다.
- **AMD와 Intel NPU의 구성:** 둘 다 "MLIR 컴파일러 → 전용 UMD(XRT / Level Zero) → DRM accel KMD + firmware"다. 우리 목표 구조와 가장 가깝다.
- **AMD의 명령 분리:** 계산 코어 코드(LLVM)와 **제어 코드(명령 스트림)**를 따로 낸다. 제어 코드는 NPU 안 microcontroller firmware가 실행한다. EVTA로 치면 fetch 모듈이 GEMM / ALU 명령 스트림을 읽는 자리다.
- **LLVM이 쓰이는 곳:** (a)뿐이다. EVTA처럼 명령 형식이 고정된 NPU의 명령은 LLVM을 거치지 않고 dialect에서 바로 serializer로 간다. EVTA에서 LLVM 몫은 CPU host 코드와, 나중에 제어용 RISC-V 코어를 넣으면 그 펌웨어뿐이다.

### 2.4 공통 API 후보 (11-03, survey §3.3)

| 후보 | 모양 | 우리에게 | 확인 |
|---|---|---|---|
| **IREE HAL** | "Vulkan 같은 compute API 위의 저수준 추상화(CPU 포함)". 장치 열거와 능력 조회, executable 정의, 메모리 할당과 cache 제어, 지연 제출용 command buffer, 명시적 동기화. 객체: allocator, buffer, buffer_view, command_buffer, device(`queue.execute`), executable, fence, event, channel. conformance test(`cts/`)가 있다 | **1순위 참고.** executable이 opaque라 EVTA 명령 blob을 그대로 담는다. CTS로 우리 구현을 검증할 수 있다 | `[원문]` HAL README, HAL dialect. semaphore 세부는 미확인 |
| OpenXLA PJRT | vendor가 PJRT C API를 plugin으로 구현하고 framework(TF, JAX)는 그 뒤를 모름. IREE에도 PJRT plugin이 있다 | 컴파일까지 묶인 단위라 무겁다. IREE를 쓰면 따라온다 | `[원문]` 개요. 함수 목록 미확인 |
| TVM UMA | TVM에 가속기를 붙이는 공식 경로 | 컴파일러 쪽 연결 | `[일반 지식]` |
| ONNX Runtime EP | ORT에 장치별 실행 provider | 컴파일러 + runtime 연결, 배포가 쉬움 | `[일반 지식]` |
| **Linux DRM accel** | `/dev/accel/accel*`(major 261). DRM 코드를 재사용하되 GPU로 오인하지 않게 노드를 따로 둠. `drivers/accel/`, `DRIVER_COMPUTE_ACCEL`. 대상에 embedded FPGA / ASIC의 edge AI도 명시 | **driver 최종 목표** | `[원문]` kernel accel 문서 |
| ↳ amdxdna (AMD NPU) | context마다 mailbox 채널과 64 MB 명령 buffer. 흐름: context 생성 → 사용자가 제어 코드를 명령 buffer에 복사 → command buffer 제출 → driver가 mailbox로 전달 → firmware가 실행(DMA 구동) → 완료 MSI-X | "명령 링 + doorbell + IRQ"의 실제 예. firmware 대신 EVTA fetch 모듈이 명령을 읽는다 | `[원문]` |
| ↳ qaic (Qualcomm AI100) | 작업마다 DBC 채널: host 메모리의 request / response FIFO, tail pointer로 제출, 완료는 response FIFO + MSI. uAPI: `CREATE_BO`, `ATTACH_SLICE_BO`, `EXECUTE_BO`(비동기), `WAIT_BO` | **우리 KMD uAPI 모양의 참고:** 할당, 제출, 대기 세 가지면 된다 | `[원문]` |
| Xilinx XRT / zocl | buffer object, run start / wait, interrupt. AMD NPU도 XRT를 UMD로 쓴다 | 지금 보드(ZynqMP)에 바로 맞는 중간 단계 | zocl `[일반 지식]`, NPU shim `[원문]` |
| Intel Level Zero | NPU UMD가 Level Zero를 구현하고 ioctl로 ivpu에 제출. 컴파일러는 UMD가 불러 씀 | 컴파일러를 UMD에 넣는 방식의 예 | `[원문]` 일부 |
| Glow Backend / DeviceManager | 이미 사용 중 | 컴파일러와 runtime이 한 몸이라 다른 컴파일러와 나누기 어렵다 | `[원문]` 코드 |

**판단(11-03):** 여러 하드웨어를 잇는 데는 MLIR로 갈아타는 것보다 **runtime / HAL 경계를 먼저 정하는 것**이 중요하다. 컴파일러는 Glow든 MLIR이든 그 경계에 맞는 executable만 내면 된다.

## 3. 목표 구조 (10-28 제안, 지금도 그대로)

```
모델 (ONNX 등)
   │
컴파일러 앞단: import, 그래프 최적화, 양자화, 분할(어느 장치에서 돌지)        ← 장치 독립
   │
컴파일러 backend (장치마다 하나): 명령 + uop + weight 배치 → AOT executable
   │
Runtime: 그래프 스케줄, 장치 간 분할(multi-EVTA, CPU), 메모리 계획           ← 장치 독립
   │
Unified HAL (C API, IREE HAL 모양)                                         ← 장치마다 backend 하나
  buffer / executable / dispatch / submit / fence / reset / perf
   │
Kernel driver: UIO 또는 XRT zocl → 나중에 Linux DRM accel (/dev/accel)
  DMA 버퍼, MMIO, interrupt, 장치 열거
   │
Hardware 공통 제어 규약: AXI-Lite start/done/irq, DRAM 명령 링 버퍼 + doorbell, 상태·perf 레지스터
```

**EVTA를 MLIR식 구조에 대응시키면** (11-03):

| 층 | 지금 | 목표 |
|---|---|---|
| front end | Glow importer, NEST-C 그래프 최적화 | 그대로, 또는 torch-mlir / ONNX → linalg |
| backend | `VTASave.cpp`가 vtalib 호출 C++를 생성 | EVTA backend가 256-bit 명령과 uop를 컴파일 때 생성(AOT). MLIR이면 EVTA dialect + serializer |
| 출력 | bundle 실행 파일(명령은 실행 중 생성) | 명령과 uop, relocation 표가 든 executable. 지금 record / replay가 실행 중에 하는 일을 컴파일 때로 |
| runtime | vtalib (`cmd_queue`, replay) | HAL 구현 하나(자체 HAL 또는 IREE 외부 HAL driver "evta") |
| UMD / KMD | `pynq_driver.cc` + `/dev/mem` + polling | UIO → DRM accel. CMA buffer는 GEM, 완료는 IRQ + fence |
| HW | 레지스터에 쓰고 polling | 명령 링 버퍼 + doorbell + 완료 IRQ |

**하드웨어 공통 제어 규약:** 가속기들이 같은 규약을 따르면 driver가 하나로 된다.
1. DRAM 명령 링 버퍼의 주소와 크기 레지스터
2. doorbell 레지스터(tail pointer)
3. 완료 interrupt(+ head pointer)
4. 상태, 오류, perf counter 레지스터
5. reset

VTA의 fetch 모듈은 이미 DRAM의 명령 스트림을 읽는다. 그래서 EVTA는 interrupt와 링 버퍼만 더하면 이 규약에 가깝다.

## 4. 단계 계획

### 4.1 runtime / driver 단계 (10-28 제안)

1. vtalib driver 층(`driver.h` 계열)을 HAL 모양으로 감싼다.
2. 명령 생성을 AOT로 옮긴다.
3. 커널 driver를 붙인다.

### 4.2 AOT 단계 (11-02, `evta_runtime_aot.md` §5.2)

| 단계 | 내용 | 규모 (추정) | 상태 |
|---|---|---|---|
| **0. record / replay** | conv 호출마다 첫 실행의 명령·uop와 relocation을 기록하고, 다음부터 복사해 주소만 고침 | 300–500줄, 3–5일 | **완료** (11-02). Resnet18 37.1 → 34.5 ms. 그 뒤 start / wait 분리와 run hook(11-06), 여러 conv를 한 replay key로(`gConvBatch`, 11-10) |
| 1. 컴파일 때 생성 (진짜 AOT) | NEST-C가 x86에서 vtalib 명령 생성기를 돌려 conv마다 `.insn` blob + relocation을 bundle에 넣음 | 1,000–2,000줄, 2–4주 | 미착수 |
| 2. HAL + 커널 driver + 구간 제출 | C HAL, UIO / zocl driver(dma-buf, interrupt, reset), 여러 layer를 command buffer 하나로 | 수개월 | 미착수 |

- **하드웨어 조건은 이미 된다.** HLS load / store / compute의 메모리 주소는 "base register + `dram_base` × 원소 크기"다. 그래서 arena base나 relocation 표로 명령을 위치 독립으로 만들 수 있다.
- base가 0이 아닐 때의 동작은 보드에서 아직 확인하지 않았다.

### 4.3 하드웨어 5단계 (11-05 제안, 11-10 갱신)

단계마다 다른 오픈소스를 먼저 보고 시작한다.

| 단계 | 내용 | 먼저 볼 것 | 상태 (11-10) |
|---|---|---|---|
| **1. 모델 병목부터** (하드웨어 그대로) | Resnet50 / VGG16 / MobileNetV2 / DeiT에서 막히는 곳 | TVM VTA 1×1 schedule, Gemmini 1×1 | **대부분 완료**: res2_0 residual, 1×1 schedule, depthwise EVTA, DeiT 전체, DMA 묶기(§7.29) |
| **2. ISA를 한 번에 개정** (ALU 연산 범위 + LOAD / STORE layout) | 모델들이 실제로 필요로 하는 것을 모아 bitstream을 한 번에 다시 만듦: MUL, avgpool 나눗셈, depthwise, concat / stride, **출력 경로 LUT, STORE 전치**(DeiT에서 추가) | NVDLA SDP / PDP, Gemmini mvin / mvout stride·pooling·scale, 최신 TVM VTA ISA, DPU misc engine | 요구 목록이 모임(`deit_optimization.md` §6.2 H1–H3) |
| **3. 완료 interrupt + 명령 링 버퍼 / doorbell** | §3의 공통 제어 규약 | amdxdna, qaic | 소프트웨어 쪽 바탕만 있음(`RunStart` / `RunWait`, run hook) |
| **4. HAL 경계 → UIO → DRM accel driver** | 3단계와 함께 설계 | IREE HAL 헤더, amdxdna·qaic uAPI, Rockchip NPU(Rocket)·Ethos-U accel driver, XRT zocl | 원문 개요만 확인, 초안 미착수 |
| **5. 하드웨어 loop FSM** | tile을 하드웨어가 펼침. 명령 형식이 바뀌므로 2단계 뒤 | Gemmini `LoopConv` | 효과를 다시 재고 판단(replay로 CPU 몫이 이미 작음) |

**11-10의 DeiT 작업에서 더해진 요구** (`deit_optimization.md` §6.3):
- 비선형 연산(softmax / LayerNorm)용 **vector unit**(행 reduction, 정수 exp / rsqrt)이 필요하다. 2단계 ISA 개정과 3단계 뒤에 둔다.
- CPU와 EVTA를 겹치려면 3단계의 interrupt / 링이 기반이다. 지금 DeiT 1장은 EVTA 약 25 ms와 CPU 약 32 ms가 겹치지 않는다.
- 배열 확대(32×32, KV260)는 위의 것들 뒤에 둔다. CPU 몫이 남은 채로 배열을 키우면 효과가 작다.

### 4.4 합친 순서 (11-10 기준 제안)

runtime / driver 단계, AOT 단계, 하드웨어 단계를 한 줄로 놓으면 이렇다. 서로 의존이 있어서 이 순서가 덜 돌아간다.

| 순서 | 할 일 | 의존 | 얻는 것 |
|---|---|---|---|
| A | **HAL API 초안**(§5) 문서. IREE HAL 헤더, `pjrt_c_api.h`, amdxdna / qaic uAPI를 읽고 확정 | 없음 | 모든 다음 단계의 경계 |
| B | vtalib의 driver 층(`pynq_driver.cc`)과 replay를 HAL 구현 `evta`로 감쌈. bundle은 HAL만 부름 | A | 다른 장치를 같은 bundle / runtime에 붙일 자리. CPU 장치(`cpu`)도 같은 HAL로 |
| C | 하드웨어 2단계 ISA 개정(MUL, 출력 LUT, STORE 전치 등) | (B와 병행 가능) | DeiT / CNN 성능, ISA가 안정됨 |
| D | 하드웨어 3단계: 완료 IRQ + 명령 링 + doorbell. UIO driver로 IRQ 받기 | ISA 안정(C) | polling 제거, CPU와 EVTA 겹치기, 공통 제어 규약의 첫 구현 |
| E | AOT 1단계: 컴파일 때 명령 blob + relocation 생성 → HAL executable 형식 | A, C | 실행 중 명령 생성 없음, executable이 장치 독립 runtime에 그대로 들어감 |
| F | 커널 driver를 DRM accel로(GEM buffer, 제출 / 대기 ioctl, reset), 여러 장치 열거 | D | sudo / `/dev/mem` 제거, 여러 사용자, hang 복구, multi-EVTA를 장치 N개로 |
| G | 두 번째 가속기를 붙여 검증(§6) | B, D(가능하면 F) | "unified"의 실제 증명 |
| H | 하드웨어 4–5단계 나머지(vector unit, loop FSM, 배열 확대) | C, D | 성능 |

- **A와 B를 먼저 하는 이유:** 하드웨어 변경 없이 바로 할 수 있다. 이후 단계는 모두 이 경계 아래에서 일어난다. driver(UIO → DRM accel)와 명령 생성(JIT → AOT)이 바뀌어도 bundle과 runtime은 그대로 둘 수 있다.
- **D를 C 뒤에 두는 이유:** 링과 IRQ는 fetch 모듈과 명령 형식을 건드린다. ISA 개정과 함께 bitstream을 한 번에 바꾸는 편이 짝(bitstream ↔ vtalib)을 덜 바꾼다(`evta_isa_ext.md` §5의 짝 관리).

## 5. HAL API 초안 (제안, 코드 아님)

IREE HAL의 객체 모양과 qaic의 최소 uAPI를 따른다. 확정은 A단계(원문 헤더 확인) 뒤에 한다.

| 객체 / 함수 | 역할 | EVTA 구현 (지금 코드와 대응) | CPU 장치 구현 |
|---|---|---|---|
| `hal_device_enumerate / open(index)` | 장치 열거와 능력 조회(배열 크기, buffer 크기, ISA 버전, 지원 연산) | `VTADeviceAlloc(index)`, `CheckLoadedBitstream`(bitstream 이름에서 EVTA 수), 지금은 bitstream 이름으로 기능 판단(`_rq`, `bias`) | 코어 수 |
| `hal_buffer_alloc / free / map / import` | DMA 가능한 buffer, dma-buf import | `VTABufferAlloc`(CMA, pool), `VTABufferView`, `VTAConstBufferAlloc`(copy 간 공유) | malloc |
| `hal_buffer_flush / invalidate` | cache 관리(EVTA는 coherent가 아님) | `VTABufferFlush / Invalidate` (`dsb sy` 포함) | 없음 |
| `hal_executable_load(blob)` | 장치별 opaque 명령 blob + relocation 표 적재 | 지금 replay 기록(명령, uop, relocation)이 그 형태. AOT 1단계 뒤에는 컴파일 산출물 | 함수 포인터 표 |
| `hal_command_buffer_begin / dispatch(executable, entry, bindings) / end` | 여러 dispatch를 묶어 지연 제출 | replay 여러 개를 한 run으로 잇기(`gConvBatch`가 첫 형태) | 함수 호출 목록 |
| `hal_queue_submit(cmdbuf, wait_fence, signal_fence)` | 비동기 제출 | 지금 `VTADeviceRunStart`; D단계 뒤에는 링 tail 갱신 + doorbell | thread pool |
| `hal_fence_wait(fence, timeout)` | 완료 대기 | 지금 `VTADeviceRunWait`(polling, `EVTA_RUN_TIMEOUT_MS`); D단계 뒤 IRQ | join |
| `hal_device_reset` | hang 복구 | 지금은 bitstream 재적재 + `xlnk_reset`(run 스크립트); F단계에서 driver reset | – |
| `hal_perf_read` | perf counter | AXI perf monitor(`_prof`), `EVTA_REPLAY_LOG` | 시간 |

**커널 uAPI** (F단계, qaic 모양):
- `CREATE_BO`(GEM, CMA): buffer를 만든다.
- `SUBMIT`(BO 목록 + 명령 BO, 비동기): 링에 넣고 doorbell을 친다.
- `WAIT`(fence 또는 timeout): 완료를 기다린다.
- 그 밖에 `RESET`과 `QUERY`(능력, perf)를 둔다.

## 6. 붙일 수 있는 다른 하드웨어

`[일반 지식]`이고, 확인과 조사는 survey §6에 남아 있다. "붙이기"는 해당 가속기의 executable을 만드는 컴파일러 backend와 HAL 구현 한 쌍을 만든다는 뜻이다.

| 하드웨어 | 제어 방식 | HAL에 붙이는 방법 | 난이도 |
|---|---|---|---|
| **EVTA** (우리) | DAE, DRAM 명령 스트림 | 기준 구현 | – |
| VTA 원본 (TVM) | 같은 DAE | ISA 차이(필드 폭)만 다른 HAL 구현, executable은 TVM이 만듦 | 낮음 |
| NVDLA | 층 레지스터, KMD가 interrupt로 의존 처리 | loadable을 executable로, 제출은 KMD 경유 | 중간 (레지스터 맵 고정) |
| Gemmini | RoCC(CPU 명령 확장) | 별도 장치가 아니라 CPU 장치의 한 종류(local-task 같은 HAL) | 다른 범주 |
| Xilinx DPU | 명령 fetch, XRT | xmodel을 executable로, XRT를 UMD로 | 중간 (형식 비공개) |
| Google Coral NPU (2025, RISC-V 기반) | 미조사 | 미조사 | survey §6 |
| PULP 계열, OpenTPU, Tensil | 미조사 / 일부만 | 미조사 | survey §6 |

**두 번째 하드웨어로 검증하는 방법** (G단계): 가장 싼 후보는 VTA 원본이나 우리 EVTA의 다른 구성이다. multi-EVTA, 다른 버퍼 크기, 32×32를 서로 다른 "장치"로 열거해 같은 runtime에서 돌린다. 진짜 다른 구조(NVDLA나 DPU)는 그 다음이다.

## 7. 지금까지 한 것과 아직 안 한 것

**한 것** (runtime 쪽, `evta_runtime_compare.md` §7, `evta_runtime_aot.md` §5.4):

| 날짜 | 내용 | 계획에서의 위치 |
|---|---|---|
| 10-31 | 층 사이 EVTA layout 유지, conv + add + relu 한 호출, maxpool EVTA | 제출 단위 키우기 |
| 10-31 | `CheckLoadedBitstream`(EVTA 수 확인) | 장치 능력 조회의 원시 형태 |
| 11-01 | VTA buffer pool | allocator |
| 11-02 | `dsb sy`(flush 뒤 순서 보장), run timeout과 상태 dump | 동기화, hang 진단 |
| 11-02 | **record / replay (AOT 0단계)**, `EVTA_REPLAY_DUMP` | executable 형식의 시제품 |
| 11-06 | driver run을 `RunStart` / `RunWait`로 나눔, run마다 hook(CPU 일 겹치기) | 비동기 제출의 원시 형태 |
| 11-06 | 상수 buffer를 copy 사이에 공유(`VTAConstBufferAlloc`) | allocator, 여러 장치 |
| 11-10 | 여러 conv를 한 queue / 한 replay key로(`gConvBatch`), `VTABufferView` | command buffer, buffer view |
| 11-10 | `EVTA_CONV_BURST`(DMA 명령 묶기) | executable 품질 |

**안 한 것:**
- HAL 초안 문서. IREE HAL 헤더(semaphore, queue), `pjrt_c_api.h`, amdxdna / qaic uAPI 헤더를 아직 읽지 않았다.
- 커널 driver(UIO 또는 zocl)와 PS-PL IRQ 배선(`ap_done` → GIC).
- 여러 호출을 한 명령 스트림으로 이어 제출하기(구간 제출).
- AOT 1단계(컴파일 때 명령 생성)와 host / 보드 명령의 동일성 검증.
- vtalib 사본 정리(`vta/vtalib`, `vtalib-zcu102-systolic`) 결정.
- 다른 가속기 하나를 실제로 붙이기.
- 컴파일러 비교 채우기: FINN, hls4ml, TVM UMA / Relax, IREE 컴파일러 plugin API, ExecuTorch, ONNX Runtime EP.

## 8. 정할 것

1. **HAL을 직접 만들지, IREE의 외부 HAL driver로 들어갈지.**
   - 직접 만들면 가볍고 Glow bundle에 바로 붙는다.
   - IREE로 들어가면 컴파일러(MLIR)와 CTS, PJRT를 얻는다. 대신 IREE 빌드를 보드나 cross 환경에 올려야 한다.
   - 추천: IREE HAL과 같은 모양으로 직접 만들어 시작한다. 경계가 같으면 나중에 IREE 외부 driver로 옮기기 쉽다.
2. **커널 driver의 중간 단계로 UIO와 XRT zocl 중 무엇을 쓸지.** UIO는 가장 단순하다. zocl은 Xilinx 도구와 맞지만 의존이 크다.
3. **vtalib 사본 두 개를 하나로 합칠지.** HLS와 systolic은 uop 순서와 일부 기능이 다르다. B단계 전에 합치면 HAL 구현이 하나로 끝난다.
4. **컴파일러를 MLIR로 옮길지.** 지금 판단은 runtime 경계가 먼저이고, 컴파일러는 그 경계에 맞는 executable만 내면 된다는 것이다(§2.4). 옮기는 시점은 AOT 1단계(E)를 할 때 다시 본다.

## 9. 출처

- IREE: [iree.dev](https://iree.dev/), [design roadmap](https://iree.dev/developers/design-docs/design-roadmap/), [HAL dialect](https://iree.dev/reference/mlir-dialects/HAL/), [HAL README](https://github.com/iree-org/iree/blob/main/runtime/src/iree/hal/README.md)
- [iree-amd-aie](https://github.com/nod-ai/iree-amd-aie), [mlir-aie](https://github.com/Xilinx/mlir-aie)
- [OpenXLA PJRT](https://openxla.org/xla/pjrt)
- Linux: [accel 소개](https://docs.kernel.org/accel/introduction.html), [amdxdna](https://docs.kernel.org/accel/amdxdna/amdnpu.html), [qaic AIC100](https://docs.kernel.org/accel/qaic/aic100.html), [qaic uAPI](https://docs.kernel.org/accel/qaic/qaic.html)
- [intel/linux-npu-driver](https://github.com/intel/linux-npu-driver)
- 하드웨어 비교 원문: `docs/evta_runtime_compare.md` §4 (VTA 논문, Gemmini arXiv 1911.09925, NVDLA hw/v1, PG338 / UG1414)
- 우리 문서: `docs/npu_stack_survey.md`, `docs/evta_runtime_aot.md`, `docs/evta_runtime_arch.md`, `docs/evta_isa_ext.md`, `docs/deit_optimization.md`
