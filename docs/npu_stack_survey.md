# 여러 NPU를 잇는 컴파일러·runtime·driver: 조사 정리와 방향

처음 쓴 날: 2026-11-03. **계속 고쳐 쓰는 문서다.** 새 조사나 측정이 나오면 해당 절을 고치고 맨 아래 §9 변경 이력에 한 줄을 남긴다.

**목표**
- 여러 하드웨어(EVTA, 다른 오픈 소스 NPU, 다른 FPGA/ASIC 가속기)를 붙일 수 있는 컴파일러, runtime, driver를 설계한다.
- 하드웨어 자체도 더 효율적으로 설계한다.

**이 문서의 범위**
- 지금까지 EVTA 작업 중에 한 조사(2026-10-26 ~ 11-03)를 한곳에 모은다.
- 자세한 근거는 원래 문서에 있으므로, 여기서는 결론과 위치만 적는다.

**확인 수준 표시** (각 항목 끝)
- `[원문]`: 논문, 공식 문서, 소스를 직접 읽어 확인했다.
- `[측정]`: 우리 보드에서 쟀다.
- `[일반 지식]`: 원문을 다시 확인하지 않았다. 설계 결정에 쓰기 전에 원문으로 확인해야 한다.

## 0. 한눈에

| 축 | 지금 (EVTA + NEST-C) | 조사에서 얻은 방향 | 상태 |
|---|---|---|---|
| 컴파일러 | Glow 위 NEST-C VTA backend. 합치기와 layout은 bundle 생성 단계(`VTASave.cpp`)에서 IR 패턴으로 정한다 | 가속기마다 다른 부분은 backend와 **opaque executable**(명령 blob)에 가두고, 앞 단계는 공통으로 | 비교만 함 (§2) |
| runtime | vtalib. conv마다 CPU가 명령을 만들고(JIT) 폴링으로 기다린다. record/replay로 두 번째부터 복사만 한다 | AOT executable, 여러 연산을 한 번에 제출, interrupt와 fence | 0단계(record/replay) 구현 (§3) |
| driver | 사용자 공간 `/dev/mem` + libcma, sudo 필요 | 커널 driver(UIO 또는 XRT zocl → Linux DRM accel). 장치를 열거하고 장치마다 queue를 둔다 | 미착수 (§3) |
| 공통 API | 없음 (Glow Backend/DeviceManager만) | IREE HAL이나 PJRT 모양의 C API: buffer, executable, dispatch, fence, reset, perf. AMD/Intel NPU는 MLIR 컴파일러 → UMD → DRM accel 구성 | 원문 개요 확인, 초안 미착수 (§2.3, §3.3) |
| 하드웨어 | VTA DAE 4모듈, systolic 16×16, 400 MHz, ALU MIN/MAX/ADD/SHR | 후처리 경로, 하드웨어 루프, 2배 클럭 어레이, 공통 제어 규약(링 버퍼 + doorbell + IRQ) | ALU II=1 완료, 나머지 후보 (§5) |
| 싼 FPGA | ZCU102 (ZU9EG) | Kria K24(ZU3급)가 가장 가깝다. Spartan US+는 CPU가 없어 runtime이 새로 필요 | 문서 조사만 (§4) |

## 1. 목표 구조 (2026-10-28 제안, 아직 그대로)

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

- **공통 API의 핵심:** 가속기마다 다른 것을 opaque executable과 그것을 만드는 backend에 몰아넣는다.
  - 그러면 다음은 하드웨어와 무관해진다: buffer 할당·import·sync, executable 적재, dispatch(executable, bindings), 제출, fence 대기, reset, perf counter 읽기.
  - IREE HAL과 PJRT가 이 방식이라, 새로 만들기보다 둘 중 하나의 모양을 따르는 것이 좋다. `[원문]` 개요 수준 (§3.3)
- **하드웨어 규약:** 가속기들이 같은 제어 규약을 따르면 driver는 하나로 된다.
  - VTA의 fetch 모듈은 이미 DRAM의 명령 스트림을 읽는다. 그래서 EVTA는 interrupt와 링 버퍼만 더하면 이 규약에 가깝다.
- **단계 (제안 당시 순서):**
  1. vtalib driver 층(`driver.h` 계열)을 HAL 모양으로 감싼다.
  2. 명령 생성을 AOT로 옮긴다. 0단계 record/replay는 구현했다(§3.2).
  3. 커널 driver를 붙인다.

근거: `docs/evta_runtime_aot.md` §4, 2026-10-28 대화(이 문서로 옮김).

계획 전체(10-28 층 구조, 11-02 AOT 단계, 11-05 하드웨어 5단계, 11-10까지 구현한 것, HAL API 초안, 합친 순서)는 `docs/unified_runtime_plan.md`에 모았다.

## 2. 컴파일러

### 2.1 비교 (지금까지 본 것)

| | 입력 | 가속기 쪽 출력 | 합치기·layout을 정하는 곳 | 다른 장치와 나누기 | 확인 |
|---|---|---|---|---|---|
| **NEST-C (Glow 기반, 지금)** | ONNX, Caffe2 | C++ bundle + weights, 실행 중 vtalib가 명령 생성 | backend 마지막 단계 `VTASave.cpp`가 저수준 IR 명령 목록을 훑으며 정함 (layout 유지, conv+add+relu, maxpool, fc, conv0 pack, conv0+pool) | `NestPartitioner` + 장치 설명 YAML | `[원문]` 코드 |
| TVM + VTA | Relay | JIT runtime이 명령 생성 | `graph_pack`이 구간 입구·출구에서만 layout을 바꾸고, bias·shift·clip은 conv schedule에 합침. 첫 conv는 CPU | Relay 분할 | `[원문]` VTA 논문, `graphpack.py` |
| Vitis AI (DPU) | 양자화된 모델 | `.xmodel` 안에 subgraph 전체 명령 (AOT) | 컴파일러가 layer fusion | DPU subgraph / CPU subgraph | `[원문]` UG1414, PG338 |
| NVDLA (오픈 소스 `nvdla/sw`) | Caffe만 | loadable (층 설정, AOT) | 하드웨어 fused mode에 맞춰 | 없음. 유지보수가 사실상 멈춤 | `[일반 지식]` |
| Gemmini | C 매크로, ONNX 일부 | RoCC 명령, CISC loop 명령 | 프로그래머/라이브러리 | RISC-V 코어와 한 몸 | `[원문]` README |
| Tensil | ONNX, TF | 미리 컴파일한 프로그램 (`tensil compile -a <arch>.tarch`) | 컴파일러 | — | README만 `[원문]` |
| TPU (XLA) | TF, JAX | AOT 프로그램 | XLA | PJRT plugin | `[일반 지식]` |

**가속기를 붙이는 공식 경로 (후보):** TVM UMA, IREE (HAL + 컴파일러 plugin), OpenXLA PJRT, ONNX Runtime Execution Provider, Glow Backend(지금 쓰는 것). `[일반 지식]`

### 2.2 우리 컴파일러에서 본 한계 (2026-11-01)

- **런타임 준비 일이 CPU에 남는다.** 예: fc weight의 EVTA layout 변환을 첫 호출 때 CPU가 했다. 나중에 `-vta-evta-fc`에서 compile time으로 옮겼다(`docs/evta_runtime_compare.md` §7.8).
  CPU가 약한 보드(Kria, MicroBlaze)에서는 이런 일을 모두 compile time으로 옮겨야 한다.
- **앞 단계가 backend의 결정을 모른다.** graph 단계와 partitioner는 무엇이 EVTA로 갈지, 무엇이 합쳐지는지 모른다.
  - backend 전용 노드(`VTASpecificNodes.h`)와 `transformPostLowering`으로 옮기면 비용 모델과 분할에 반영된다. 작업이 크다.
- **블록 16이 backend 코드에 고정돼 있다**(`VTA.cpp:509`, `VTASave.cpp:805` 등). 작은 FPGA용으로 블록을 줄이거나 32×32로 키우려면 backend를 고쳐야 한다.
- **명령은 아직 실행 중에 만든다.** record/replay로 두 번째 실행부터는 복사만 하지만, 진짜 AOT(컴파일러가 명령 blob을 내는 것)는 아니다.

### 2.3 MLIR 계열: 컴파일러 아래에 runtime과 driver가 어떻게 붙는가 (2026-11-03)

**MLIR과 LLVM은 컴파일러를 만드는 틀이다. runtime과 driver는 정해 주지 않는다.** MLIR로 만든 프로젝트마다 runtime과 driver를 따로 둔다.

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

- **LLVM은 (a)에만 쓰인다.** EVTA처럼 명령 형식이 고정된 NPU의 명령은 LLVM을 거치지 않는다. MLIR dialect에서 바로 serializer로 간다.
  EVTA에서 LLVM 몫은 CPU host 코드, 그리고 나중에 제어용 RISC-V 코어를 넣으면 그 펌웨어뿐이다.

| 프로젝트 | 컴파일러 | 출력 | runtime | driver | 확인 |
|---|---|---|---|---|---|
| **IREE** | 입력 dialect → Flow(스케줄·실행 영역 분할, dispatch) → Stream → **HAL dialect**("거의 1:1로 compute 전용 Vulkan API") → target backend | VM bytecode(스케줄) + 장치별 executable. 정적 링크용으로 VM을 LLVM IR로 내릴 수도 있다 | IREE VM + HAL C API | HAL driver(local-task, vulkan, cuda, hip, metal). 외부 HAL driver를 plugin으로 붙인다 | `[원문]` iree.dev design roadmap, HAL dialect 문서. vmfb 형식은 `[일반 지식]` |
| **iree-amd-aie** | IREE 컴파일러 plugin(`compiler/plugins`). AIE 코어 코드는 Peano(LLVM-AIE) | — | IREE runtime + 외부 HAL driver `amdxdna`(`runtime/src`) | `xdna-driver` 커널 모듈, XRT submodule | `[원문]` README. HAL driver가 XRT를 거치는지는 미확인 |
| **MLIR-AIE / IRON** (AMD Ryzen AI) | MLIR 기반 AIE toolchain, Python IRON. 코어 코드는 Peano(LLVM에 AIE target 추가) | **xclbin + 명령 스트림**(배열 설정 overlay와 제어 코드) | XRT | **amdxdna** (DRM accel) | `[원문]` mlir-aie README, kernel amdxdna 문서 |
| **Intel NPU** | OpenVINO IR → MLIR 기반 NPU 컴파일러(compiler-in-driver, `libnpu_driver_compiler.so`) → blob | blob | OpenVINO NPU plugin → **Level Zero** UMD(`libze_intel_npu.so`, Graph extension) | **ivpu** (`drivers/accel/ivpu`) + firmware | 구조는 `[원문]`(linux-npu-driver 저장소), 라이브러리 이름은 패키징 페이지(제3자) |
| **OpenXLA** | StableHLO → 장치별 컴파일러 | 장치별 | **PJRT C API**: 장치 vendor가 plugin으로 구현, framework에는 opaque | 장치별 | `[원문]` openxla.org/xla/pjrt. plugin이 구현하는 함수 목록은 `pjrt_c_api.h`(아직 안 읽음) |
| Triton | Triton → TritonGPU dialect → LLVM → PTX/AMDGCN | cubin/hsaco | 얇은 launcher | CUDA/ROCm driver | `[일반 지식]` |
| torch-mlir | front end만 | MLIR | 없음 (IREE 등에 넘김) | — | `[일반 지식]` |

**정리**
- 새 장치를 IREE에 붙이는 단위는 **컴파일러 target plugin + 외부 HAL driver 한 쌍**이다(iree-amd-aie가 실례).
- AMD와 Intel NPU는 둘 다 "MLIR 컴파일러 → 전용 UMD(XRT / Level Zero) → DRM accel KMD + firmware" 구성이다. 우리 목표 구조(§1)와 가장 가깝다.
- AMD는 계산 코어 코드(LLVM)와 **제어 코드(명령 스트림)**를 따로 낸다. 제어 코드는 NPU 안의 microcontroller firmware가 실행한다(§3.3). EVTA로 치면 GEMM/ALU 명령 스트림을 fetch 모듈이 읽는 것과 같은 자리다.

**EVTA에 대응시키면**

| 층 | 지금 | MLIR식으로 바꾸면 |
|---|---|---|
| front end | Glow importer, NEST-C 그래프 최적화 | torch-mlir / ONNX → linalg (공통) |
| backend | `VTASave.cpp`가 vtalib 호출 C++를 생성 | **EVTA dialect**에서 tiling, fusion. serializer가 256-bit 명령과 uop를 컴파일 때 생성 |
| 출력 | bundle 실행 파일 (명령은 실행 중에 생성) | 명령과 uop가 들어 있는 module. 지금 record/replay가 실행 중에 하는 일을 컴파일 때로 옮긴 것 |
| runtime | vtalib (`cmd_queue`, replay) | HAL 구현 하나 (IREE 외부 HAL driver "evta" 또는 같은 모양의 자체 API) |
| UMD / KMD | `pynq_driver.cc` + `/dev/mem` + polling | UIO → DRM accel. CMA buffer는 GEM, 완료는 IRQ + fence |

- 판단: 여러 하드웨어를 잇는 데는 MLIR로 갈아타는 것보다 **runtime/HAL 경계를 먼저 정하는 것**이 중요하다. 컴파일러는 Glow든 MLIR이든 그 경계에 맞는 executable만 내면 된다.
  순서 제안: (1) HAL 초안(IREE HAL 모양) 뒤로 vtalib 옮기기, (2) UIO + IRQ, (3) EVTA dialect 시제품(MLIR-AIE 구조 참고).

### 2.4 아직 조사하지 않은 것

- MLIR로 만든 가속기 컴파일러(예: Allo, Buddy-MLIR), IREE 컴파일러 plugin API의 실제 모양.
- FPGA용 데이터플로 컴파일러: FINN(Xilinx, 양자화 데이터플로), hls4ml.
- TVM의 최신 경로(Relax, UMA)로 VTA 계열을 붙인 사례.
- ExecuTorch, TFLite delegate, ONNX Runtime EP의 실제 장치 연결 API.
- 위를 같은 기준으로 비교하기: 입력 형식, IR 단계, 가속기 backend를 붙이는 비용, AOT 여부, 여러 장치 분할, 양자화 처리.

## 3. Runtime과 driver

### 3.1 다른 가속기의 제어와 runtime (원문 확인, `docs/evta_runtime_compare.md` §4)

| | 하드웨어 제어 | 명령 단위 | 명령 생성 때 | 의존성 | 동기화 | 층 사이 layout | conv 밖 연산 |
|---|---|---|---|---|---|---|---|
| EVTA + NEST-C (10-29 당시) | DAE 4모듈 | task + uop | 추론 중 conv마다 CPU (JIT) | 명령 속 token | conv마다 폴링 | conv마다 CPU transpose | CPU |
| EVTA + NEST-C (11-03) | 같음 | 같음 | 첫 호출만 JIT, 이후 replay | 같음 | conv마다 폴링 | **EVTA layout 유지** | **ALU (relu, add, maxpool, fc)** |
| VTA + TVM | 같음 | 같음 | JIT | 같음 | 연산 묶음마다 | 구간 입구·출구 한 번 | bias·shift·clip을 ALU |
| Gemmini | DAE 3 controller | RoCC 행렬 명령, CISC loop | 실행 중 CPU, loop는 HW FSM | HW ROB | CPU 파이프라인 안 | scratchpad 유지 | pool, relu, im2col, transpose를 HW |
| NVDLA | 층 레지스터 (ping-pong 두 벌) | 하드웨어 층 | 컴파일러가 층 설정 | 소프트웨어 | 층마다 interrupt | NVDLA 형식 | SDP/PDP/CDP 파이프라인 |
| Xilinx DPU | 명령 fetch | 컴파일러 명령 | AOT (xmodel) | 명령 안 (세부 비공개) | subgraph마다 | DPU 형식 | fusion, 대부분 DPU |

**driver·runtime 구조 (`[일반 지식]`, `docs/evta_runtime_aot.md` §4):**

| | 제출·동기화 | 메모리 | 커널 driver | 다른 하드웨어 붙이기 |
|---|---|---|---|---|
| VTA / 우리 vtalib | 연산 묶음마다 Synchronize, polling | 사용자 공간 `/dev/mem`, CMA, 수동 flush | 없음 | driver 층이 얇아 포팅은 쉬움 |
| NVDLA | 그래프 통째 제출, KMD가 interrupt로 엔진 간 의존 처리 | 커널 GEM / dma-buf | KMD + UMD | NVDLA 레지스터 맵에 묶임 |
| Gemmini | CPU 파이프라인 안, fence | 가상 메모리(공유 TLB) | 필요 없음 | RISC-V Rocket에 묶임 |
| TPU (XLA) | 프로그램 단위 비동기 | runtime 관리 | 있음(비공개) | PJRT plugin API |
| Xilinx XRT | `run.start/wait`, interrupt | buffer object + sync | `zocl` (MPSoC) | FPGA 범용, 우리 보드에 바로 맞음 |

### 3.2 무엇이 나은가, 그리고 우리가 한 것

| 항목 | 나은 쪽 | 우리 상태 |
|---|---|---|
| 명령 생성 | AOT (DPU, NVDLA, TPU) | record/replay(AOT 0단계, 11-02). Resnet18 단일 OMP 1에서 37.1 → 34.5 ms `[측정]` |
| 제출 단위 | command buffer 하나를 제출 (IREE/Vulkan) | conv(또는 합친 호출)마다 제출 |
| 완료 알림 | interrupt + fence | 폴링. multi4에서 4 ms tick 흔들림, `OMP_NUM_THREADS=1` 필요 |
| 메모리 | 커널이 관리하는 dma-buf | `/dev/mem` + libcma. sudo, 다른 사용자와 충돌, hang 뒤 bitstream 재적재 필요 |
| 여러 장치 | 장치 N개 열거, 장치마다 queue | EVTA i = 주소 + index, bundle이 EVTA 하나에 고정 |
| 층 사이 layout | 가속기 형식 유지 | 유지 (11-01, `-vta-keep-layout`) |
| 후처리 | 가속기에서 | relu, residual add, maxpool, fc를 EVTA로 (10-31 ~ 11-02) |

**결론 (10-29, 이후 측정으로 맞음):**
- DAE 구조 자체는 다른 설계와 견줘 손해가 없다(Gemmini도 DAE다).
- 손해는 "conv 하나 = 함수 하나 = 동기화 한 번 = layout 왕복 한 번"이라는 runtime·backend의 단위에서 났다.
- 실제로 그 단위를 고쳐 Resnet18 single이 73.6 ms(10-29, OMP 4)에서 27.3 ms(11-03, OMP 1)가 됐다 `[측정]`.

### 3.3 공통 API 후보 (2026-11-03 원문 일부 확인)

| 후보 | 모양 | 우리에게 | 확인 |
|---|---|---|---|
| IREE HAL | "Vulkan 같은 현대 compute API 위의 저수준 추상화(CPU 포함)". 할 일: 장치 열거와 능력 조회, 장치에서 돌 executable 정의, 메모리 할당과 cache 제어, 지연 제출용 작업 묶음(command buffer), 제출 순서를 위한 명시적 동기화. HAL dialect의 객체: allocator, buffer, buffer_view, command_buffer, device(`queue.execute`), executable, fence, event, channel. 구현 검증용 conformance test(`cts/`)가 있다 | 1순위 참고. executable이 opaque라 EVTA 명령 blob을 그대로 담을 수 있다. CTS로 우리 구현을 검증할 수 있다 | `[원문]` HAL README, HAL dialect 문서. semaphore 모델 세부는 미확인 |
| OpenXLA PJRT | 장치 vendor가 PJRT C API를 plugin으로 구현하고 framework(TF, JAX)는 그 뒤를 모른다. IREE에도 PJRT plugin이 있다 | 컴파일까지 묶인 단위라 무겁다. IREE를 쓰면 PJRT는 따라온다 | `[원문]` openxla.org. 함수 목록은 미확인 |
| TVM UMA | TVM에 가속기를 붙이는 공식 경로 | 컴파일러 쪽 연결 | `[일반 지식]` |
| Linux DRM accel | 장치 `/dev/accel/accel*`(major 261). DRM의 새 장치 종류로 DRM 코드를 재사용하지만, GPU 스택이 가속기를 GPU로 오인하지 않게 노드를 따로 둔다. driver는 `drivers/accel/`에 두고 `DRIVER_COMPUTE_ACCEL` 플래그를 쓴다. 문서는 대상으로 embedded FPGA/ASIC의 edge AI도 든다 | driver 최종 목표 | `[원문]` kernel accel 문서 |
| ↳ amdxdna (AMD NPU) | microcontroller firmware가 명령 처리. 작업 context마다 mailbox 채널과 64 MB 명령 buffer(사용자 공간에도 매핑). 흐름: context 생성 → 사용자가 제어 코드를 명령 buffer에 복사 → command buffer 제출 → driver가 mailbox로 전달 → firmware가 제어 코드 실행(DMA 구동) → 완료 MSI-X | 우리가 할 "명령 링 + doorbell + IRQ"의 실제 예. firmware 대신 EVTA fetch 모듈이 명령을 읽는다 | `[원문]` kernel amdxdna 문서 |
| ↳ qaic (Qualcomm AI100) | 작업마다 DBC 채널: host 메모리의 request/response FIFO, tail pointer를 올려 제출, 완료는 response FIFO + MSI. uAPI: `CREATE_BO`(GEM handle), `ATTACH_SLICE_BO`(BO의 어느 부분을 어디로), `EXECUTE_BO`(큐에 넣기만, 비동기), `WAIT_BO`(완료 또는 timeout) | 우리 KMD uAPI 모양의 참고: 할당, 제출, 대기 세 가지면 된다 | `[원문]` kernel qaic 문서 |
| Xilinx XRT / zocl | buffer object, run start/wait, interrupt. AMD NPU도 XRT를 UMD로 쓴다(xdna-driver의 XRT shim) | 지금 보드(ZynqMP)에 바로 맞는 중간 단계 | zocl은 `[일반 지식]`, NPU shim은 `[원문]` |
| Intel Level Zero | NPU UMD가 Level Zero API를 구현, ioctl로 ivpu에 제출. 컴파일러는 UMD가 불러 쓴다(compiler-in-driver) | 컴파일러를 UMD에 넣는 방식의 예 | `[원문]` 일부, §2.3 |
| Glow Backend / DeviceManager | 이미 사용 중 | 컴파일러와 runtime이 한 몸이라 다른 컴파일러와 나누기 어렵다 | `[원문]` 코드 |

### 3.4 아직 하지 않은 것

- HAL API 초안 문서(IREE HAL, PJRT, XRT와 비교).
- 커널 driver: UIO로 interrupt와 DMA 버퍼부터. 하드웨어에서 `ap_done`을 PS-PL IRQ로 연결해야 한다.
- 여러 conv를 한 명령 스트림으로 이어 제출하기(구간 제출). DAE token으로 conv 경계도 겹칠 수 있다.
- multi-EVTA로 한 층을 나누기(2026-10-25 검토): 출력 채널(KN) 분할이 가장 단순하다. 합치기가 필요 없고, 각 EVTA가 출력 버퍼의 자기 영역에 쓴다.
  `convolution_wo_tr`이 핸들 배열을 받고, 시작과 대기를 나누는 API가 필요하다.

## 4. 싼 FPGA로 옮기기 (`docs/evta_board_compare.md`, 2026-10-29~30)

EVTA 쪽 수치는 저장소 utilization report에서, 보드 쪽은 공식 페이지에서 가져왔다. **합성해 보지는 않았다.**

| | ZCU102 (지금) | Kria K24 (ZU3급) | Kria K26 (KV260) | Spartan US+ SCU35 | Spartan US+ SCU200 |
|---|---|---|---|---|---|
| LUT / DSP / BRAM36 | 274K / 2,520 / 912 | 70.5K / 360 / 216 | 117K / 1,248 / 144 (+URAM 64) | 16K / 48 / 48 | 100K / 384 / 192 (+URAM 64) |
| hard CPU | A53 ×4 | A53 ×4 | A53 ×4 | 없음 | 없음 |
| 외부 메모리 | DDR4 | LPDDR4 (용량 미확인) | DDR4 4 GB | HyperRAM 8 MB | LPDDR4X/5 hard MC 2개 |
| EVTA 16×16 | 4개 | 1개 (버퍼 16_15_18_17) | (계산 안 함) | 안 들어감 | 1개 (16_15_18_17) |
| Resnet18 | 됨 | 됨 | 됨 | 메모리 부족 | 됨 |
| runtime | 지금 그대로 | 메모리 할당(xlnk → u-dma-buf / dma-heap)과 bitstream 적재 교체 | K24와 같음 | baremetal soft core로 새로 | PCIe host driver 또는 MicroBlaze V Linux |
| 가격 | — | SOM $250, KD240 kit $399 | 미확인 | $229 | 미확인 |

- EVTA 하나 = LUT 약 26K, FF 약 36K, DSP 약 268, BRAM36 258(single 버퍼) 또는 약 139(multi 버퍼).
- **지금 흐름을 거의 그대로 쓰는 곳은 PS8이 있는 Zynq US+(Kria)뿐이다.** Spartan US+는 CPU가 없어 runtime과 driver가 새 포팅이다.
- 성능 예상: K24에서 300 MHz면 약 120 GOPS(ZCU102 380 MHz의 약 80 %). speed grade를 확인해야 한다.
- 다음 확인: Vivado 2020.1에서 XCZU3EG part로 16_15_18_17 EVTA를 합성해 들어가는지와 timing을 본다.
- **이 목표에 대한 의미:** 싼 보드일수록 CPU가 약하거나 없다. 그래서 §2.2의 "준비 일은 compile time으로", §3의 AOT와 커널 driver, 하드웨어 쪽 후처리가 필수가 된다.
  작은 보드용으로 EVTA를 줄이려면(SCU35) GEMM과 ALU를 시간 분할하고 버퍼를 줄여야 하며, 블록 16을 바꾸려면 backend도 고쳐야 한다.

## 5. 하드웨어를 더 효율적으로

### 5.1 다른 NPU의 하드웨어와 비교

**GEMM 어레이** (`docs/evta_systolic.md` §9.12, 2026-10-27, `[원문]`)

| 설계 | 어레이 | dataflow | 클럭 | 우리에게 준 것 |
|---|---|---|---|---|
| EVTA 지금 | 16×16 int8 | WS systolic, DSP 안 B1/B2 | 400 MHz `[측정]` | — |
| Gemmini | 16×16 int8 | WS/OS | 1.89 GHz (ASIC) | 출력 쪽 RMW와 stall, 태그 |
| DSP-Fetch (2409.03508) | 14×14 | WS, B1 chain + B2 | 666 MHz (ZU3EG, OOC) | weight 경로를 DSP 안에 |
| FireFly v2 | 16×16×(4~8)×4 | OS | 300/600 (ZU3EG 보드) | DSP만 2배 클럭 |
| Xilinx DPU | B512~B4096 | OS | 325/650 | 2배 클럭, edge-aligned |
| tinyTPU | 14×14 | broadcast | 400 (OOC) | broadcast가 클럭을 막는다 |
| gemm_hls, AutoSA | HLS | — | 145~186 (gemm_hls) | HLS systolic은 택하지 않음 |
| TPU v1 | 256×256 | WS systolic | 700 MHz | 같은 구조, 크기만 다름 `[일반 지식]` |
| NVDLA nv_full | 16 cell × 64 = 1,024 MAC (int8 2,048) | broadcast + 덧셈 트리 | ASIC ~1 GHz | 빠른 이유는 MAC 수와 고정 기능 파이프라인 `[일반 지식]` |

**후처리** (2026-10-31, `[일반 지식]` 위주)

| | 방식 | 특징 |
|---|---|---|
| VTA/EVTA | 범용 vector ALU 하나 (int32 16 lane, MIN/MAX/ADD/SHR), acc SRAM 위에서 명령으로 | 작고 조합이 자유롭다. GEMM과 compute 모듈을 나눠 써서 ALU 시간만큼 GEMM이 쉰다 |
| Gemmini | residual은 `mvin` 때 accumulator에 누적, `mvout` 때 scale, ReLU, max-pool engine | 중간 |
| NVDLA | conv 뒤 SDP(bias, BN, eltwise, ReLU, LUT) 파이프라인, PDP(pool), CDP | 빠르지만 유닛과 설정 레지스터가 많고 연산이 고정 |
| Xilinx DPU | conv engine과 별도 misc engine(pool, eltwise) | NVDLA와 비슷 |

### 5.2 우리 측정이 가리키는 곳 (Resnet18, 11-03)

- EVTA 24.89 ms: GEMM 18.85, ALU 약 3.1(II=1 뒤), 나머지 약 3 ms(conv0 tile, 작은 run 고정비, 1×1 conv) `[측정]`.
- 실행 하나의 고정 비용 4.7 µs, GEMM 명령 약 25 clk, ALU 명령 약 35 clk `[측정]`.
- 모델 GOPS 145.8 / 이론 204.8 = 71 %. 어레이가 일한 시간은 76 %이고, 차이는 쓸모없는 GEMM(conv0의 0 채널, halo)이다 `[측정]`.

### 5.3 후보 (효과와 비용)

| 후보 | 내용 | 예상 효과 | 비용 | 상태 |
|---|---|---|---|---|
| ALU imm II=1 | imm 연산은 acc 읽기·쓰기 한 번씩 | ALU 4.63 → 약 3.1 ms | 작음 | **완료** (single 11-03, multi4 11-04 보드 통과) |
| RMW reset에 bias | GEMM 첫 누산을 0 대신 bias로 | bias ADD 0.91 ms 제거 | 약 2일 (RTL tag, ISA pad bit, vtalib) | **완료** single 11-04 (EVTA 24.89 → 24.15 ms), multi4 11-04 |
| 후처리를 출력 경로로 | Gemmini/NVDLA처럼 SHR, clip, relu를 STORE 경로에 | ALU 시간 대부분 | 중간~큼, ISA | **완료** requantize(out 쓰기 길, `rq_*`), single 23.05 ms, multi4 400 MHz (11-04, `evta_isa_ext.md`) |
| ALU opcode 폭 | 지금 2비트라 MUL(4), MOVE(5)를 못 씀 | avgpool, 곱 scale, BNN | ISA, 컴파일러 | 검토 |
| 하드웨어 루프 FSM | Gemmini `LoopConv`처럼 tile을 하드웨어가 펼침 | 명령 수와 CPU 일 감소 | 큼 | 검토 |
| interrupt + 명령 링 버퍼 + doorbell | §1의 공통 제어 규약 | 폴링 제거, driver 공통화 | 중간 (PS-PL IRQ) | 검토 |
| 어레이만 2배 클럭 (C안) | DSP와 FF만 ~600 MHz, 나머지 300~333 | GEMM 처리량 최대 2배 | 큼, 클럭 경계 | 조사 완료, 미착수 |
| INT8 2-MAC packing (WP486) | DSP 하나에 곱 2개 | DSP 절반 | 7항 누산 제한, ISA | 보류 |
| 32×32 어레이 | MAC 4배 | Resnet18 EVTA 약 2.5배 (weight 대역에 막힘) | ISA, SRAM, backend | 보류 |
| im2col / transpose를 하드웨어로 | Gemmini처럼 | CPU 몫 감소 (지금은 이미 작음) | 중간 | 낮은 우선순위 |

## 6. 다음에 할 조사 (우선순위 순)

1. **HAL 초안:** 개요 수준 원문 확인은 했다(11-03, §3.3). 남은 것: IREE HAL 헤더(`iree/hal/*.h`)의 semaphore·queue 함수, `pjrt_c_api.h`, amdxdna·qaic uAPI 헤더를 읽고 EVTA용 HAL 초안을 쓴다.
2. **컴파일러 비교 채우기:** MLIR 계열은 구조만 봤다(§2.3). FINN, hls4ml, TVM UMA/Relax, IREE 컴파일러 plugin API를 §2.1과 같은 기준으로. (§2.4)
3. **다른 오픈 소스 NPU:** Coral NPU(Google, RISC-V 기반, 2025), OpenTPU, PULP 계열 가속기의 제어 방식과 SW 스택. `[일반 지식]`으로만 알고 있다.
4. **싼 FPGA 합성:** XCZU3EG part로 EVTA 합성. K24 LPDDR4 용량, speed grade, PYNQ 지원.
5. **확인 못 한 수치들:** DPU 의존성 처리, Gemmini FPGA 클럭, Tensil·AutoSA 클럭, ZU9EG -2 DSP Fmax(DS925).

## 7. 관련 문서

| 문서 | 이 문서와의 관계 |
|---|---|
| [evta_runtime_compare.md](evta_runtime_compare.md) | §4 다른 가속기의 제어·runtime (원문), §5 DAE 평가, §6 32×32, §8 하드웨어 비교 항목 |
| [evta_runtime_aot.md](evta_runtime_aot.md) | §4 driver/runtime 비교와 공통 API 후보, §5 AOT 단계 |
| [evta_systolic.md](evta_systolic.md) | §0 하드웨어 방향 결정(A~E안), §9 외부 설계·논문 조사 |
| [evta_board_compare.md](evta_board_compare.md) | 다른 보드 자원과 출처 |
| [evta_overview.md](evta_overview.md) | EVTA 작업의 시간순 요약 |
| [ref/README.md](ref/README.md) | 받아 둔 논문과 소스 |

## 8. 출처

이 문서의 원문 근거는 위 문서들의 출처 절에 있다.
- `evta_runtime_compare.md` §10: VTA, Gemmini, NVDLA, DPU(PG338, UG1414, UG1354), TVM `graphpack.py`.
- `evta_systolic.md` §9: Gemmini, UG579, arXiv 2409.03508, 2309.16158, 2510.08137, DPU, WP486, Tensil, AutoSA, gemm_hls.
- `evta_board_compare.md` §6: AMD SCU35, SCU200, Kria K24, DS891, DS985.
- §2.3, §3.3 (2026-11-03에 읽음):
  - IREE: https://iree.dev/ , https://iree.dev/developers/design-docs/design-roadmap/ , https://iree.dev/reference/mlir-dialects/HAL/ ,
    https://github.com/iree-org/iree/blob/main/runtime/src/iree/hal/README.md
  - iree-amd-aie: https://github.com/nod-ai/iree-amd-aie ; MLIR-AIE: https://github.com/Xilinx/mlir-aie
  - PJRT: https://openxla.org/xla/pjrt
  - Linux accel: https://docs.kernel.org/accel/introduction.html , https://docs.kernel.org/accel/amdxdna/amdnpu.html ,
    https://docs.kernel.org/accel/qaic/aic100.html , https://docs.kernel.org/accel/qaic/qaic.html
  - Intel NPU: https://github.com/intel/linux-npu-driver (구성 요소 이름 일부는 Fedora COPR, snapcraft 등 패키징 페이지)

## 9. 변경 이력

- 2026-11-03: 처음 씀. 10-26 ~ 11-03의 조사(런타임 비교 10-28/29, 다른 NPU 10-28/31, 싼 보드 10-29/30, 하드웨어 조사 10-27)와 11-03까지의 측정을 모았다.
- 2026-11-03: §2.3 MLIR 계열(IREE, iree-amd-aie, MLIR-AIE, Intel NPU, PJRT)의 컴파일러 → runtime → driver 구성과 EVTA 대응을 추가. §3.3을 원문(IREE HAL, DRM accel, amdxdna, qaic) 확인으로 고침. 옛 §2.3은 §2.4.
- 2026-11-04: §5.3 ALU imm II=1이 multi4에서도 보드 통과(Resnet18 copy당 EVTA 26.8 → 25.3 ms).
- 2026-11-05: §5.3 bias reset, requantize 완료로. 다른 모델 측정(`evta_runtime_compare.md` §7.19): Resnet50은 1×1 conv schedule과 합쳐지지 않은 residual 하나, VGG16 CIFAR-10은 weight LOAD가 병목.
- 2026-11-05: 위 VGG16 원인 정정 — weight LOAD가 아니라 7×7 미만 출력을 7×7로 늘려 계산한 것(HLS GEMM의 acc hazard 회피용 최소 tile; Gemmini는 stall, 우리 systolic `sa_rmw`는 forwarding이라 필요 없음). 더미 tile STORE의 채널 블록 버그(vt 2에서 틀림)도 고침. 결과: Resnet50 147.9 → 85.7 ms, VGG16 15.5 → 7.3 ms(§7.19 ①~③).
- 2026-11-05: fc chunk 조건(≥ 8 블록)도 HLS GEMM hazard용이라 systolic에서 풂(TVM VTA는 batch 1 마지막 dense를 CPU에 둠): Resnet50 83.4 ms. bias entry가 virtual thread 1의 acc를 덮던 vtalib 버그(HLS·simulator만 틀림) 고침(§7.19 ④⑤).
- 2026-11-06: conv0 입력 복사(A53 한 코어에서 NEON 명령 수가 한계, 1.4~1.65 ms)를 conv와 겹침: driver run을 start / wait로 나누고 replay가 run마다 hook으로 다음 타일 행 입력을 채움(double buffering, CPU–NPU overlap의 첫 단계). Resnet50 83.4 → 82.2 ms, Resnet18 25.4 → 24.1 ms(§7.19 ⑥).
- 2026-11-06: multi4에 Resnet50, VGG16 추가(copy마다 같은 weight는 CMA buffer 하나를 공유: 여러 NPU가 한 model을 나눠 쓰는 runtime의 기본 요구), conv0 stream은 multi4에서도 copy당 −1.2~1.3 ms. multi buffer에서만 드러난 acc 범위 버그 고침(§7.19 ⑦).
- 2026-11-06: multi4가 느린 원인(§7.19 ⑧): HP 포트 앞 register slice(load latency, 혼자 +1.1 %)와 DDR 공유(EVTA 쌍마다 +0.9 ms, HP1+HP2는 같은 DDR port라 +1.25 ms). 대역이 아니라 latency: 여러 NPU를 한 SoC에 둘 때 포트 배치가 성능 요소.
- 2026-11-06: MobileNetV2(TFLite QAT 모델을 float ONNX로) 첫 실행: 1110 ms 중 97 %가 CPU(depthwise 657, 24채널 conv 262, 비대칭 pad conv0 154 ms), EVTA 1×1 conv는 13.7 ms. power-of-two per-tensor quantization은 depthwise weight에서 깨짐(per-channel이면 맞음). depthwise 하드웨어 방식 조사(대각 GEMM / systolic array의 depthwise 모드(BiRD, CMSA) / 전용 엔진(DPU의 ALU) / ALU MAC)와 EVTA 추정치는 결정 보류.
- 2026-11-07: 다른 runtime과 비교하기 위한 설명서 [evta_runtime_arch.md](evta_runtime_arch.md)(EVTA runtime / driver 구조와 동작, 그림 6장, §10 비교 관점 표). backend, compiler 편은 이어서.
- 2026-11-07: 컴파일러 편 [evta_compiler_frontend.md](evta_compiler_frontend.md)(importer, Glow overlay, optimizeFunction 순서, lowering, power-of-two 양자화와 그 한계, IR)와 [evta_compiler_backend.md](evta_compiler_backend.md)(VTA backend, VTASave 분석 pass, conv 경로, conv0 pack, schedule, 옵션), 그림 5장(`make_evta_compiler_figs.py`). 입구 문서 [evta_stack_arch.md](evta_stack_arch.md)(그림 11장 순서대로 + 다른 스택 비교용 질문 10개). 확인 중 발견: importer의 `-npu-conv-fitting=16`(채널 0 채움, MobileNetV2 24채널 conv에 쓸 수 있음), power-of-two 지수 하한 −7(작은 범위 텐서의 정밀도 손실).
- 2026-11-07: MobileNetV2 1048 → 84.2 ms(OMP 1; `-t 4` 44.4 ms), 분류 293 → 282(evta_runtime_compare.md §7.20). (1) depthwise만 per-channel 양자화하고 NEON CPU kernel로(정확도 문제의 원인은 depthwise weight의 per-tensor scale; 위 −7 하한은 이 빌드에서 쓰이지 않음). (2) 24채널 activation을 0 채널로 32채널로 넓혀 EVTA로. (3) EVTA shift를 반올림(compile time에 bias로; 잘라내면 −0.5 LSB 편향이 쌓임). (4) TF SAME 비대칭 pad conv0를 pack으로 EVTA에. 남은 것은 depthwise 43 ms(A53 NEON이 한계) → EVTA 대각 GEMM이냐 ISA 확장이냐의 결정.
- 2026-11-08: MobileNetV2 84.2 → 43.2 ms(OMP 1; `-t 4` 22.5 ms; §7.20 ⑤~⑦): depthwise가 NPU layout을 직접 읽고 써서 CPU↔NPU 사이 layout 변환 34개가 없어짐(CPU fallback 연산도 NPU layout을 알아야 한다는 예), 1×1 conv를 fc로 바꿔 NPU로, 모양별 schedule 탐색(AutoTVM 방식) 20개.
- 2026-11-08: depthwise conv를 ISA 변경 없이 EVTA로(§7.21): tap마다 대각 weight로 GEMM(1/16 활용), 채널별 shift는 ALU vector SHR. MobileNetV2 43.2 → 28.6 ms(OMP 1); 4코어 NEON보다는 느림(OMP 4 22.5 → 24.7 ms). 작은 DMA가 병목이라 depthwise용 하드웨어(채널별 MAC)나 큰 burst LOAD가 다음 후보.
- 2026-11-08: bundle `.cpp` 맨 앞에 생성 옵션 값(기본값 포함)을 주석으로 기록. 모델별 컴파일 옵션(MobileNetV2만 켜는 반올림, EVTA depthwise)이 산출물에 남아 재현과 비교가 쉬워짐(TVM의 build config 기록과 같은 목적). EVTA depthwise(`-vta-evta-depthwise`)는 해당 layer가 있을 때만 걸리므로 기본으로 켬(모델별 판단을 사람이 아니라 컴파일러가).
- 2026-11-09: 첫 Transformer(DeiT-tiny)를 보드에서 끝까지(evta_runtime_compare.md §7.22): cat / dog / zebra 3장 모두 정답, 1장 3.77 s(OMP 1). 대부분 CPU float이다. 고정된 연산 목록 밖의 모델에서 드러난 것: (1) frontend가 동적 shape를 못 받아 ONNX를 미리 접어야 했다(TVM / ONNX Runtime은 상수 접기를 import에서 함). (2) backend의 CPU fallback이 없는 연산은 segfault였다. generic CPU 연산(matmul, copy, slice, LayerNorm / GELU 조각)을 넣어 "NPU 미지원 = CPU에서 느리게"가 되게 했다(DPU / Hailo의 CPU subgraph와 같은 역할). (3) 기존 conv0 pack이 patch embedding(16×16/16)을 1×1 conv로 바꾸는 데 그대로 쓰였다. 다음은 Linear를 EVTA로 보내는 것이다(MAC의 약 85%).
- 2026-11-09: DeiT-tiny의 Linear 72개를 EVTA로 보냈다(evta_runtime_compare.md §7.23). MatMul을 int8로 양자화하고, 상수 weight의 MatMul은 1×1 conv로 바꿨다(M행을 H × W 이미지로 펼침). 별도 GEMM 경로 없이 conv 경로를 재사용했다. CPU 쪽(int8 attention matmul NEON, 양자화 입력의 erf / exp 표, vtalib elementwise NEON)도 bit 단위로 같게 줄여 3.77 s → 0.475 s(OMP 1)가 됐다. 이제 EVTA가 56 ms이고, 나머지는 attention과 LayerNorm / GELU / softmax의 CPU 시간이다.
- 2026-11-09: DeiT-tiny attention의 Q·Kᵀ / A·V 72개도 EVTA로 보냈다(evta_runtime_compare.md §7.24, `-vta-evta-matmul`). 새 하드웨어 없이 같은 GEMM을 쓰는 1×1 conv이고, 실행 중에 만들어지는 두 입력을 CPU가 EVTA 배치로 옮긴다(vtalib `matmul_evta`). 146 → 30 ms, 1장 475 → 359 ms(OMP 1), 0 differ다. TPU / GPU / Gemmini / ANE도 attention 곱을 같은 행렬 연산기로 계산하고, 따로 붙이는 것은 전치 경로와 softmax / LayerNorm용 벡터 장치다. 이제 남은 시간은 대부분 그 연산들(CPU 약 250 ms)이다.
- 2026-11-09: DeiT-tiny의 GELU / softmax / LayerNorm CPU 시간을 줄였다(evta_runtime_compare.md §7.25). 결과는 bit 단위로 같다. 원소별 연산 구간(int8 → int8, 상수는 한 값)을 처음 실행할 때 같은 코드로 만든 256칸 표로 바꿨다(`-vta-lut-elementwise`). TFLite / ONNX Runtime의 int8 LUT 연산(GELU, sigmoid 등)과 같은 생각이지만, 표를 원래 연산 코드로 만들어서 결과가 같다는 것이 보장된다. softmax는 int8을 직접 읽고, LayerNorm은 pow / reduceAdd / elemsub을 NEON으로 바꿨다. 359 → 206 ms(OMP 1)다. 이제 EVTA conv 56 ms 다음으로 큰 것은 matmul_evta의 CPU 배치 변환과 Linear bias 덧셈이다.- 2026-11-10: DeiT-tiny 137 ms(systolic 400 MHz, OMP 1; evta_runtime_compare.md §7.26). Linear bias를 conv bias에 접었고(TVM / TensorRT의 bias fusion과 같음), head slice / concat을 int8로 지원해 attention 전치를 int8로 바꿨다. backend가 데이터 이동 연산을 int8로 지원한다고 알리지 않으면 quantizer가 그 주변을 float로 남긴다. ONNX Runtime의 QDQ 전파와 같은 문제다. LayerNorm 21개 명령도 한 함수로 합쳤다. 다음은 softmax / LayerNorm의 남은 float 계산과 matmul_evta 호출 수다.
- 2026-11-10: DeiT-tiny 137 ms(systolic 400 MHz, OMP 1; evta_runtime_compare.md §7.26). Linear bias를 conv bias에 접었고(TVM / TensorRT의 bias fusion과 같음), head slice / concat을 int8로 지원해 attention 전치를 int8로 바꿨다. backend가 데이터 이동 연산을 int8로 지원한다고 알리지 않으면 quantizer가 그 주변을 float로 남긴다. ONNX Runtime의 QDQ 전파와 같은 문제다. LayerNorm 21개 명령도 한 함수로 합쳤다.
- 2026-11-10: DeiT-tiny 123.5 ms(§7.27). Q / K를 Linear conv의 EVTA 출력 배치 그대로 attention conv의 입력과 weight로 썼다(버퍼 view와 256 byte 블록 순서 바꾸기만). 3 head는 한 command queue에 쌓아 EVTA 실행 한 번으로 돌렸다. TVM VTA의 "layout을 연산 사이에 유지"와 TensorRT의 multi-head 묶음과 같은 방향이다. 하드웨어 변경 없이 남은 데이터 이동은 V의 16×16 전치뿐이다.
- 2026-11-10: DeiT-tiny 107.4 ms / `-t 4` 75.2 ms(§7.28). EVTA 55.8 → 38.3 ms. Linear와 attention matmul을 가상 스레드 2개 schedule로 돌려 LOAD와 GEMM을 겹쳤다(이전에는 전송 36.6 ms가 하나도 겹치지 않았다). 출력 블록 수가 홀수일 때 vt 2가 조용히 아무것도 계산하지 않던 vtalib 결함도 고쳤다. TVM의 AutoTVM / Ansor처럼 모양별 schedule 탐색이 성능을 좌우한다는 같은 결론이다. 남은 것은 1×1 경로의 DMA 묶음이다.
- 2026-11-10: conv 타일 DMA를 길게(§7.29): 1×1 conv의 weight를 스레드마다 한 번에 LOAD하고, 연속인 행은 한 행으로 합쳤다. DeiT EVTA 38.3 → 19.4 ms(130 GOPS, DRAM 없는 하한 수준), 1장 `-t 4` 55.7 ms. Resnet50 82.2 → 69.6 ms, MobileNetV2 24.3 → 18.8 ms. VTA 계열의 2D DMA 명령도 전송 길이가 가동률을 좌우한다(Gemmini / NVDLA의 burst 묶음과 같은 이유).
- 2026-11-10: KV260에 모델 하나를 올려 공급할 때의 비교를 `kv260_product_compare.md`로 정리했다(11-08 ~ 11-09 검색: Pi 5 + Hailo-8, Jetson Orin Nano Super, KV260 DPU, RK3588, Coral의 가격·전력·크기·인터페이스·공개 성능; 정정한 장점; Hailo-8이 Transformer를 CNN보다 약 30배 느리게 돌려 DeiT-tiny를 고른 이유; DeiT-tiny 3.77 s → 88.5 ms 진행).
- 2026-11-10: DeiT-tiny를 줄여 온 과정을 `deit_optimization.md`로 정리했다(3.77 s → 88.5 ms, 단계별 효과, 남은 시간, 소프트웨어 후보와 하드웨어 후보 H1–H8). 하드웨어 순서는 이 문서의 계획과 맞췄다: ISA 개정 한 번에 ALU opcode 확장 + 출력 경로 LUT + STORE 배치 옵션, 다음 명령 링 + interrupt, vector unit, 그 뒤에 배열 확대.
- 2026-11-10: unified runtime / driver 계획을 `unified_runtime_plan.md`로 정리했다. 10-28 층 구조, IREE / MLIR / PJRT / DRM accel(amdxdna, qaic) 비교, AOT 0–2단계, 하드웨어 5단계(11-05)를 한곳에 모았다. HAL API 초안(IREE HAL 모양, qaic식 최소 uAPI)과 11-10까지 구현한 runtime 기능의 대응을 붙였다. 합친 순서: HAL 초안 → vtalib을 HAL로 감싸기 → ISA 개정 → IRQ + 링 → AOT 1단계 → DRM accel → 두 번째 가속기.
- 2026-11-10: Resnet18을 줄여 온 과정을 `resnet18_optimization.md`로 정리했다(134.8 → 23.3 ms; 10-29 분석의 결론 "DAE가 아니라 runtime 단위가 문제"대로 CPU 일 제거로 148.7 → 28.8 ms, 하드웨어 세 단계(ALU II = 1, bias reset, requantize)로 EVTA 26.4 → 23.05 ms, 그 뒤 입력 겹침과 DMA 묶기).
- 2026-11-10: Resnet50(160.3 → 69.6 ms), VGG16 CIFAR-10(16.1 → 7.0 ms), MobileNetV2(1048 → 18.7 ms)도 같은 형식으로 정리했다(`resnet50_optimization.md`, `vgg16_optimization.md`, `mobilenetv2_optimization.md`). 세 모델 모두 Resnet18에 없던 모양(1×1 bottleneck, 작은 공간의 큰 weight, depthwise / 24채널 / 비대칭 pad)이 병목이었고, schedule 탐색과 DMA 묶기가 공통으로 컸다.
- 2026-11-10: VTA → ETRI EVTA → 이번 작업을 코드로 비교했다(`vta_to_evta_diff.md`). ETRI는 256-bit 명령, XP_GEMM(binary), output → input 경로(LOAD MOVE, `gemm_from_output`, BNN에서만 사용), Glow 기반 컴파일러와 bundle 라이브러리를 더했다. NEST-C 논문의 MOV를 지금 하드웨어에 넣을 때의 상한을 쟀다(EVTA_MEMEXP): Resnet18 −6 %, Resnet50 −13 %, VGG16 −5 %. compute에 MOV opcode를 두는 방식이 맞고, 배열 확대나 tile 단위 층 fusion 때 같이 하는 것이 좋다.
- 2026-11-10: 다음 ISA 개정의 ALU 확장을 설계했다(`evta_isa_ext.md` §8, 구현 전). `alu_opcode` 2비트는 그대로 두고 pad에 `alu_op_ext` 2비트를 두어 이전 실행 파일과 호환한다. MUL, SUB, CPY, LUT / LUTSET, RSUM / RMAX, CLZ, 출력 경로 LUT를 넣는다. matmul은 GEMM에서 하고, 필요한 하드웨어는 데이터 이동(STORE 옵션, MOV)이다. 하드웨어와 runtime 3–4주, 컴파일러를 합하면 6–7주로 본다.
- 2026-11-10: DeiT-tiny 정확도를 ImageNetV2(9,000장)로 쟀다(`deit_optimization.md` §6.6). float 62.34 %, 보드 int8 56.17 %. 손실은 int8 −1.7, power-of-2 scale −2.3, 번들 구현 −1.7, LayerNorm 체인 −0.4 %p다. ALU 연산만으로 하는 정수 LayerNorm / softmax(`evta_isa_ext.md` §8.6)는 56.49 %로 정확도를 잃지 않는다. power-of-2를 푸는 `rq_mul`이 다음 정확도 후보다.
- 2026-11-11: DeiT-tiny ImageNetV2 top-1 56.17 → 60.47 %(float 62.34 %), 1코어 88.8 → 75.7 ms, 4코어 55.7 → 52.9 ms(`deit_optimization.md` §6.6.3). activation scale은 자유롭게 두고 EVTA 연산의 비율만 2의 거듭제곱으로 맞춘다(`-vta-pow2-ratio`). 그래서 하드웨어 `rq_mul`가 필요 없다. 정수 LayerNorm은 bias를 shift 전에 더해 float과 같은 정확도가 되었고 기본이 되었다. 채널별 weight scale은 효과가 없었다.
- 2026-11-11: ALU 확장 2단계(`evta_isa_ext.md` §8.7, §8.8). ISA 헤더에 `alu_op_ext` / `lut_en`을 넣고, 기준 모델(`alu_ext_ref.h`)을 vtalib 시뮬레이터에 연결했다. 정수 LayerNorm(61 명령) / softmax(29 명령)를 ALU 명령열로 짜서 bundle CPU 함수와 바이트 단위로 같음을 확인했다. 하드웨어보다 기준 모델과 명령열을 먼저 정한 것은 IREE / TVM이 연산의 의미를 먼저 고정하고 backend를 맞추는 것과 같은 순서다. 하드웨어 전 상태는 tag `alux-step2-261111`로 남겼다. 3단계(HLS ALU, CSim)는 설계만 했다. 호스트 PC가 수랭 펌프 고장으로 보이는 과열로 꺼져서(`host_pc_shutdown.md`) 합성은 미뤘다.
- 2026-11-11: ALU 확장 3단계(`evta_isa_ext.md` §8.8). HLS ALU에 연산 4–11과 출력 LUT를 넣었다(`ALUX=1`). 무작위 명령열과 LayerNorm / softmax 명령열을 CSim에서 기준 모델 / CPU 함수와 바이트 단위로 맞췄다. 버그를 일부러 넣어 시험이 잡는지도 확인했다. C 합성으로는 LUT +20k, FF +36k, DSP +16이다. pipeline이 깊어져(17 step) forwarding 없는 ALU의 규칙이 바뀌었다. 그래서 보드에서 기존 다섯 모델의 명령을 모두 뽑아 검사했고, 문제는 0건이었다. 하드웨어를 바꿀 때 기존 실행 파일의 명령을 실제로 모아 새 제약에 비춰 보는 것은, 컴파일러 쪽 회귀 시험(IREE / TVM의 lit 시험)에 대응하는 하드웨어 쪽 회귀 시험이다. GEMM 출력 LUT는 ISA 비트만 예약하고 ALU pass로 대신한다. DeiT에서 0.6 ms이고, 출력이 큰 YOLO류에서 다시 본다. 비트스트림 합성은 호스트 PC 냉각을 고친 뒤로 미뤘다.
- 2026-11-11: conv 출력 표(GELU 등)를 GEMM 출력 하드웨어 대신 ALU pass로 하는 vtalib 경로를 넣었다(`evta_isa_ext.md` §8.9). 비트스트림 없이 vtalib 시뮬레이터에서 지금 방식(CPU 표)과 바이트 단위로 같음을 확인했다. 하드웨어 기능을 늘리기 전에 같은 결과를 내는 명령열을 먼저 정하고 시뮬레이터로 맞춘다. 그러면 하드웨어는 그 명령을 빠르게 하는 문제만 남는다. IREE가 HAL 아래 backend마다 같은 dispatch 결과를 요구하는 것과 같은 방식이다.
- 2026-11-12: ALU 확장 6·7단계 중 소프트웨어로 되는 것을 했다(`evta_isa_ext.md` §8.10). DeiT의 GELU 표, LayerNorm, softmax(선택)를 EVTA ALU 명령열로 내는 경로를 컴파일러와 vtalib에 넣었다. 실행할 때 비트스트림 이름으로 확장이 있는지 보고, 없으면 예전 CPU 함수로 간다. 그래서 한 실행 파일이 두 비트스트림에서 다 돈다. 이것은 IREE / ONNX Runtime이 장치 기능(capability)을 실행 시 조회해 커널을 고르는 방식과 같고, 통합 runtime(`unified_runtime_plan.md`)의 HAL에서 장치 기능 플래그로 둘 항목이다. DeiT bundle 전체를 PC의 vtalib 시뮬레이터에 링크해 ALU 경로와 CPU 경로의 결과가 같음을 확인했다. 보드의 시간 측정 빌드에서만 드러난 버그(코드 생성기가 낸 `if` 줄이 계측 도구와 충돌)도 있었다. 생성 코드는 계측·후처리 도구가 줄 단위로 다룰 수 있게 한 줄 한 문장으로 낸다.
