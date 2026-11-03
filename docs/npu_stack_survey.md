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
| 공통 API | 없음 (Glow Backend/DeviceManager만) | IREE HAL이나 PJRT 모양의 C API: buffer, executable, dispatch, fence, reset, perf | 미착수 (§3.3) |
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
  - IREE HAL과 PJRT가 이 방식이라, 새로 만들기보다 둘 중 하나의 모양을 따르는 것이 좋다. `[일반 지식]`
- **하드웨어 규약:** 가속기들이 같은 제어 규약을 따르면 driver는 하나로 된다.
  - VTA의 fetch 모듈은 이미 DRAM의 명령 스트림을 읽는다. 그래서 EVTA는 interrupt와 링 버퍼만 더하면 이 규약에 가깝다.
- **단계 (제안 당시 순서):**
  1. vtalib driver 층(`driver.h` 계열)을 HAL 모양으로 감싼다.
  2. 명령 생성을 AOT로 옮긴다. 0단계 record/replay는 구현했다(§3.2).
  3. 커널 driver를 붙인다.

근거: `docs/evta_runtime_aot.md` §4, 2026-10-28 대화(이 문서로 옮김).

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

### 2.3 아직 조사하지 않은 것

- MLIR 기반 흐름: IREE의 컴파일러 쪽, torch-mlir, MLIR로 만든 가속기 컴파일러(예: Allo, Buddy-MLIR).
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

### 3.3 공통 API 후보 (`[일반 지식]`, 원문으로 아직 확인 안 함)

| 후보 | 모양 | 우리에게 |
|---|---|---|
| IREE HAL | Vulkan을 본뜸: allocator, buffer, command buffer, executable, queue, semaphore | 1순위 참고. executable이 opaque라 EVTA 명령 blob을 그대로 담을 수 있다 |
| OpenXLA PJRT | plugin마다 compile과 execute | 컴파일러까지 묶인 단위라 무겁다 |
| TVM UMA | TVM에 가속기를 붙이는 공식 경로 | 컴파일러 쪽 연결 |
| Linux DRM accel | `/dev/accel/*`, 2023년 커널. Intel VPU, AMD XDNA, Qualcomm AI100이 씀 | driver 최종 목표 |
| Xilinx XRT / zocl | buffer object, run start/wait, interrupt | 지금 보드(ZynqMP)에 바로 맞는 중간 단계 |
| Glow Backend / DeviceManager | 이미 사용 중 | 컴파일러와 runtime이 한 몸이라 다른 컴파일러와 나누기 어렵다 |

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
| ALU imm II=1 | imm 연산은 acc 읽기·쓰기 한 번씩 | ALU 4.63 → 약 3.1 ms | 작음 | **완료** (single 보드 통과, multi4 timing 재빌드 중) |
| RMW reset에 bias | GEMM 첫 누산을 0 대신 bias로 | bias ADD 0.91 ms 제거 | 약 2일 (RTL tag, ISA pad bit, vtalib) | 다음 |
| 후처리를 출력 경로로 | Gemmini/NVDLA처럼 SHR, clip, relu를 STORE 경로에 | ALU 시간 대부분 | 중간~큼, ISA | 검토 |
| ALU opcode 폭 | 지금 2비트라 MUL(4), MOVE(5)를 못 씀 | avgpool, 곱 scale, BNN | ISA, 컴파일러 | 검토 |
| 하드웨어 루프 FSM | Gemmini `LoopConv`처럼 tile을 하드웨어가 펼침 | 명령 수와 CPU 일 감소 | 큼 | 검토 |
| interrupt + 명령 링 버퍼 + doorbell | §1의 공통 제어 규약 | 폴링 제거, driver 공통화 | 중간 (PS-PL IRQ) | 검토 |
| 어레이만 2배 클럭 (C안) | DSP와 FF만 ~600 MHz, 나머지 300~333 | GEMM 처리량 최대 2배 | 큼, 클럭 경계 | 조사 완료, 미착수 |
| INT8 2-MAC packing (WP486) | DSP 하나에 곱 2개 | DSP 절반 | 7항 누산 제한, ISA | 보류 |
| 32×32 어레이 | MAC 4배 | Resnet18 EVTA 약 2.5배 (weight 대역에 막힘) | ISA, SRAM, backend | 보류 |
| im2col / transpose를 하드웨어로 | Gemmini처럼 | CPU 몫 감소 (지금은 이미 작음) | 중간 | 낮은 우선순위 |

## 6. 다음에 할 조사 (우선순위 순)

1. **공통 API 원문 확인:** IREE HAL, PJRT, Linux DRM accel, XRT의 API를 원문으로 읽고 HAL 초안을 쓴다. (§3.3)
2. **컴파일러 비교 채우기:** MLIR 계열(IREE 컴파일러, torch-mlir), FINN, hls4ml, TVM UMA/Relax를 §2.1과 같은 기준으로. (§2.3)
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

## 9. 변경 이력

- 2026-11-03: 처음 씀. 10-26 ~ 11-03의 조사(런타임 비교 10-28/29, 다른 NPU 10-28/31, 싼 보드 10-29/30, 하드웨어 조사 10-27)와 11-03까지의 측정을 모았다.
