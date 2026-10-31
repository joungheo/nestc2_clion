# EVTA 제어 방식(DAE)과 runtime: 다른 가속기와 비교, GEMM을 키우기 전에 볼 것

작성 2026-10-29. 대상: EVTA(ZCU102, systolic 400 MHz / HLS 380 MHz), `vta/vtalib`, NEST-C VTA backend(`lib/Backends/VTA/VTASave.cpp`).
앞선 문서: `docs/evta_runtime_aot.md`(JIT vs AOT, §4에 다른 runtime 표가 있지만 "원문 미확인"이었다. 이 문서가 그 표를 원문으로 다시 확인하고 넓힌다),
`docs/evta_systolic.md` §9(하드웨어 어레이 조사).

질문: systolic GEMM을 32×32, 64×64로 키우면 좋아지겠지만, fetch/load/compute/store가 따로 돌며 dependency token을 주고받는
지금 방식이 다른 방식에 비해 성능 면에서 좋은 편인가? 하드웨어 구조도 같이 비교해야 하나?

## 1. 요약

- **DAE(decoupled access-execute) 구조 자체는 좋은 편이다.** 큰 3×3 conv에서 이론 최대의 93 %(systolic 400, 190 GOPS / 204.8)가 나온다.
  load·store가 GEMM 뒤에 숨는다는 뜻이고, 이 부분은 Gemmini(ROB로 controller 사이 의존 추적)나 TPU v1과 같은 수준이다.
- **성능을 깎는 것은 DAE가 아니라 그 위의 runtime 사용법이다.** Resnet18 한 번(single, OpenMP 4스레드) 73.6 ms 중
  EVTA가 실제로 계산하는 시간은 29.7 ms(40 %)뿐이다. 나머지는 거의 다 runtime과 backend가 만든 CPU 일이다.
  - 층마다 layout을 바꾸는 transpose 22.1 ms(30 %)
  - CPU에서 하는 elemadd, relu, maxpool 10.4 ms(14 %)
  - 층마다 버퍼 할당·해제·flush 5.0 ms(7 %)
  - 층마다 명령어를 그 자리에서 만드는 비용 2.5 ms(3 %)
- 다른 가속기는 이 넷을 대부분 없앴다. 층 사이 데이터를 가속기 layout으로 두고(TVM-VTA, DPU, Gemmini), 명령은 미리 만들거나
  하드웨어가 펼치고(DPU, NVDLA, Gemmini의 loop FSM), conv 뒤의 elementwise·pool을 가속기에서 한다(DPU, NVDLA, Gemmini).
- **GEMM을 32×32로 키우면 EVTA 시간은 약 2.5배 줄지만(29.7 → 약 12 ms), 추론 전체는 73.6 → 약 56 ms(−24 %)에 그친다.**
  큰 층은 weight 대역(HP 포트 하나)에 막히고, CPU 몫 41 ms가 그대로이기 때문이다. 64×64는 ZCU102 DSP(2,520)에 들어가지 않는다.
- **그래서 하드웨어 비교는 해야 하지만, 대상은 어레이 크기보다 "어레이 밖"이다.** ALU로 할 수 있는 연산, STORE/LOAD가 다음 층 layout을
  그대로 쓸 수 있는지, 명령어를 누가 언제 만드는지, 동기화를 어디서 하는지.
- **권장 순서:** ① 층 사이 layout 유지(transpose와 버퍼 할당 제거) → ② elemadd·relu·maxpool을 EVTA ALU로 →
  ③ 명령 AOT·구간 단위 제출 → ④ conv0 입력 → ⑤ 그다음 GEMM 32×32. ①②만으로 추론 73.6 → 약 44 ms, ①~④로 약 36 ms가 기대된다(§7).

## 2. 지금 EVTA는 어떻게 도는가

### 2.1 하드웨어: VTA의 4모듈 DAE (VTA 원논문 §3.1, `docs/ref/arxiv_1807.04188.pdf`)

```
DRAM ── fetch ──┬─▶ load 명령 큐 ──▶ load  ──(inp/wgt SRAM)──▶ compute ──(acc→out SRAM)──▶ store ──▶ DRAM
                ├─▶ compute 명령 큐 ─────────────────────────▶ compute
                └─▶ store 명령 큐 ───────────────────────────────────────────────────────▶ store
                       load ⇄ compute ⇄ store 사이: dependency queue(token)로 RAW/WAR 방지
```

- 원논문: "These modules communicate over command queues and on-chip shared memories (SRAMs)… Accesses to these memories are
  synchronized via dependency queues to prevent data hazards such as Read After Write (RAW) and Write After Read (WAR)."
- 명령은 task 단위(LOAD, GEMM, ALU, STORE)이고, GEMM/ALU는 uop(1×16 · 16×16 하나)를 2중 affine 루프로 돌린다(원논문 Fig. 5).
  uop에는 제어 흐름이 없어 루프를 펼쳐야 하고, 그래서 uop을 runtime이 그때그때 만든다(원논문 §3.2 "micro-kernels have to be unrolled").
- 겹치기(latency hiding)는 "가상 스레드" 2개가 SRAM을 반씩 쓰며 번갈아 load와 GEMM을 하는 방식으로 얻는다
  (원논문 "partition tasks into two mutually-exclusive execution contexts… achieved in TVM using virtual threads").
  우리 vtalib `conv_kernel`도 같다(acc·wgt를 `threadIdx * ENTRY / nVirtualThread`로 나눔, inp는 별도 double buffer).

### 2.2 runtime: conv 하나가 도는 순서 (코드 근거)

NEST-C가 만든 bundle(`VTASave.cpp`)은 **conv마다** 아래를 CPU에서 차례로 한다(`_systimed` 표의 문장 순서 그대로).

```
VTABufferAlloc(입력)  → transpose_nhwc2vtaio(NHWC → EVTA layout, CPU)  → VTABufferFlush
VTABufferAlloc(출력)  → convolution_wo_tr(...):
                          tile마다 conv_kernel: LOAD/GEMM/ALU/STORE 명령과 uop를 CPU가 생성(JIT)
                          VTASynchronize: 명령 큐를 EVTA에 넘기고 끝날 때까지 CPU가 폴링 (VTABundle.cpp:2703)
VTABufferInvalidate   → transpose_vtaio2nhwc(EVTA layout → NHWC, CPU)  → VTABufferFree × 2
elemadd / relu / maxpool (CPU, OpenMP)
```

- 동기화는 conv마다 한 번이고, 완료는 레지스터 폴링으로 본다(`pynq_driver.cc` Run: `done_o_ap_vld` 확인 뒤 `done_o`, 루프마다 `yield`).
  인터럽트는 쓰지 않는다.
- 이 bitstream의 ALU는 `alu_opcode` 2비트라 MIN, MAX, ADD, SHR 네 가지다(`include/zcu102/vta/hw_spec.h:147`). MUL은 안 되지만
  **relu(MAX 0), residual add(ADD), maxpool(MAX)은 이 네 가지로 할 수 있다.** 지금은 conv 끝의 bias add, shift, clip만 EVTA에서 한다.

## 3. 측정: 시간이 어디에 쓰이나 (Resnet18 1회, systolic 400, 2026-10-29)

`run_prof.sh -s _systimed -n 25 -r Resnet18Test`(OpenMP 기본 4스레드)와 OpenMP 1스레드 측정, run 25의 문장 시간을 호출별로 합쳤다.

| 호출 | OMP 4 (73.6 ms) | OMP 1 (148.9 ms) | 어디서 | 비고 |
|---|---|---|---|---|
| `convolution_wo_tr` | **32.1 ms** | 32.2 ms | EVTA + CPU | EVTA 29.7 ms(`_sysprof`) + 명령 생성 2.5 ms |
| `transpose_vtaio2nhwc` | 11.0 | 41.8 | CPU | conv 출력을 NHWC로 |
| `transpose_nhwc2vtaio` | 4.8 | 17.8 | CPU | 다음 conv 입력을 EVTA layout으로 |
| `transpose_nhwc2vtaio_zerofill` | 6.4 | 6.5 | CPU | conv0 입력 3 → 16채널, OpenMP 없음 |
| `elemadd` | 4.7 | 16.1 | CPU | residual add 8개 |
| `maxpool` | 3.3 | 12.8 | CPU | 112×112×64 |
| `relu` | 2.5 | 8.6 | CPU | |
| `VTABufferAlloc` / `Free` / `Flush` | 5.0 | 4.5 | CPU | conv마다 CMA 할당·해제 |
| `fullyconnected` | 1.2 | 4.4 | CPU | 1 Mops |
| quantize, elemsub, elemdiv(전처리) | 2.2 | 3.7 | CPU | |

- **EVTA가 계산하는 시간은 OMP 4에서 40 %, OMP 1(multi4의 각 copy가 이 조건)에서 20 %다.**
- transpose만 OMP 4에서 22.1 ms(30 %), OMP 1에서 66.1 ms(44 %)다. conv보다 크다.
- conv 안의 CPU 몫(명령 생성 + 시작 + 완료 폴링)은 conv당 0.1 ms 안팎, tile이 98개인 conv0만 0.66 ms다(`_sysprof` 대 `_systimed`).

EVTA 쪽만 보면(`_sysprof`, conv별):

| conv | EVTA 시간 | GOPS (최대 204.8) | 무엇에 묶였나 |
|---|---|---|---|
| stage4 3×3 512→512 (7×7) | 1.22 ms | 190 (93 %) | GEMM — DAE가 load를 다 숨김 |
| stage1 3×3 64→64 (56×56) | 1.50 ms | 155 (76 %) | GEMM + tile 경계 |
| stage2~4 1×1 downsample | 0.26~0.33 ms | 39~49 | conv당 고정 비용(시작, bias load, ALU, store) |
| conv0 7×7 (3→16 0 채움) | 9.13 ms | 138 (0 채움 포함), 실효 26 | 0 채움으로 연산 13/16이 버려짐 |

- 1×1 downsample은 데이터가 0.2 MB 안팎이라 대역(4 GB/s면 0.05 ms)이 아니라 conv마다 드는 고정 비용에 묶인다.
  conv마다 따로 시작하고 끝을 기다리는 runtime 구조의 비용이다.
- multi4에서 4개를 동시에 돌려도 EVTA 하나의 시간은 그대로였다(29.8 ms, `docs/evta_runtime_aot.md` 끝). DDR/HP 경쟁은 없다.
  대신 conv마다 하는 폴링 때문에 CPU 스케줄링(4 ms tick)에 흔들렸다(lockstep에서 7~9/25 run, `chrt`로 0~1).

## 4. 다른 가속기의 제어·runtime 방식 (원문 확인)

### 4.1 원래 VTA + TVM (VTA 원논문, TVM `vta/python/vta/top/graphpack.py`)

- **명령 생성:** JIT. 원논문 §3.2 "The JIT runtime generates and manages micro-kernels on the fly." 우리와 같다.
  JIT를 고른 이유로 원논문은 이종 실행(CPU/VTA 분담), 컴파일러 단순화, uop cache 크기 한계, 바이너리 크기를 든다(성능이 이유가 아니다).
- **layout:** 우리와 다르다. `graph_pack`이 그래프의 한 구간(기본 `nn.max_pool2d` 뒤부터 `nn.global_avg_pool2d` 앞까지)의 입구에서 한 번
  NCHW → `NCHW{b}n{c}c`로 바꾸고(`_pack_batch_channel`), 출구에서 한 번 되돌린다(`_unpack_batch_channel`). 구간 안의 conv, add,
  bias_add는 packed layout 그대로 돈다. 즉 **층마다 transpose하지 않는다.** (4D `reshape`은 예외로 중간에 풀었다 다시 묶는다.)
- **연산:** conv 뒤의 elementwise(bias, shift, clip)는 conv schedule에 합쳐 VTA ALU에서 한다. 첫 conv는 CPU에 둔다
  (원논문 §3.2 "the first convolutional layer in most CNNs contains operators with low arithmetic intensity that perform well on CPUs").
- **우리와의 차이 정리:** 같은 하드웨어 계열이고 runtime도 같은 JIT지만, NEST-C backend는 conv 하나를 독립 함수로 감싸 앞뒤로 NHWC를
  유지한다. 그래서 TVM-VTA에 없는 transpose 22 ms와 버퍼 할당 5 ms가 생긴다. **DAE나 JIT의 문제가 아니라 backend의 선택이다.**

### 4.2 Gemmini (UC Berkeley, `docs/ref/arxiv_1911.09925.pdf`, github ucb-bar/gemmini README)

- **하드웨어 제어:** load, store, execute 세 controller가 따로 돈다(DAE, 우리와 같은 계열). 의존성은 명령에 token을 넣는 대신
  **하드웨어 ROB**가 controller 사이 hazard를 잡는다. README: "Gemmini includes an ROB which is meant to detect hazards between
  instructions in different controllers." 같은 controller 안에서는 순서대로 낸다.
- **명령:** RISC-V 코어의 RoCC custom 명령(`mvin`, `mvout`, `matmul.preload`, `matmul.compute.*`, `config_*`). 원래는 DIM×DIM 단위라
  CPU가 tile마다 명령을 내야 한다. 이를 줄이려고 **CISC 루프 명령** `gemmini_loop_ws`, `gemmini_loop_conv_ws`가 있고,
  하드웨어 FSM(`LoopMatmul`, `LoopConv`)이 tile을 펼치고 double buffering한다. 명령 하나가 scratchpad 절반까지의 conv 한 덩어리다.
- **동기화:** CPU 파이프라인 안의 명령이라 별도 드라이버·폴링이 없다(fence). 가상 메모리(TLB)를 공유한다.
- **layout과 연산:** scratchpad에 둔 채 다음 연산으로 넘기고, pooling·ReLU·행렬-스칼라 곱·im2col·transpose 블록을 가속기 안에 둔다.
- **end-to-end에 대한 논문의 관찰:** 하드웨어 im2col이 없으면 host CPU가 im2col을 해야 해서 CPU 종류가 성능을 좌우하고,
  im2col 블록이 있으면 작은 in-order CPU로도 된다(ResNet50 22.8 FPS, Rocket). 산업 평가에서 ML 작업이 가속기가 있어도 시간의
  77 %까지를 CPU에서 쓴다고 인용한다. 우리 측정(EVTA 20~40 %)과 같은 현상이다.

### 4.3 NVDLA (nvdla.org hw/v1 Hardware Architecture)

- **하드웨어 제어:** 명령 스트림이 아니라 **층 단위 레지스터 설정**이다. host(또는 마이크로컨트롤러)가 "configuration of one hardware layer,
  along with an activate command"를 보내고, 끝나면 인터럽트를 받는다.
- **설정 숨기기:** 블록마다 레지스터 그룹이 두 벌(ping-pong)이라, 한 층이 도는 동안 CPU가 다음 층 설정을 써 둔다
  ("The CPU can program the second group in the background").
- **의존성:** 하드웨어가 추적하지 않는다("does not have intrinsic support for dependency tracking"). 층 순서는 소프트웨어 몫이다.
- **연산 합치기:** fused mode에서 convolution core → SDP(bias, BN, activation, elementwise) → PDP(pooling) → CDP가 메모리를 거치지 않고
  FIFO로 이어진다. 즉 **conv + bias + relu + add + pool이 한 "하드웨어 층"**이다.
- 우리와 비교: 층마다 CPU가 관여하는 점은 같지만, 그 일은 레지스터 몇 개 쓰기이고(명령 수천 개 생성이 아님) 다음 층 설정이 겹쳐 숨는다.

### 4.4 Xilinx DPU (DPUCZDX8G, PG338 Hardware Architecture, UG1414)

- **하드웨어 제어:** "After start-up, the DPUCZDX8G fetches instructions from off-chip memory to control the operation of the computing
  engine. The instructions are generated by the AMD Vitis AI compiler"(PG338). 명령 기반이라는 점은 VTA와 같다.
- **명령 생성:** **AOT.** 컴파일러가 subgraph 전체의 명령을 `.xmodel`에 넣는다(UG1414 "netname.xmodel is the compiled xmodel which
  contains instructions"). 컴파일러가 layer fusion 등을 한다(PG338).
- **제출:** VART runner가 DPU subgraph 단위로 비동기 실행(`execute_async` + `wait`)한다. DPU가 지원하지 않는 연산만 CPU subgraph로
  나뉜다(UG1354 graph_runner).
- **데이터:** 중간 feature map을 DPU 형식으로 DDR/온칩 버퍼에 두고 다음 층이 그대로 읽는다(층 사이 CPU transpose 없음). 이 항목은 일반 지식이고 원문으로 확인하지 않았다.
- 우리와 비교: 같은 ZCU102급 장치, 같은 "명령 기반 + 온칩 버퍼" 구조인데 차이는 **명령을 언제 만들고 몇 번 동기화하느냐**다.

### 4.5 참고: Tensil, TPU v1 (원문 미확인, 일반 지식)

- Tensil(오픈 소스 TCU, `docs/ref/tensil`): 모델을 미리 컴파일한 프로그램(명령 + 데이터 배치)을 driver가 한 번 올려 끝까지 돌린다.
  README의 흐름(`tensil compile -a <arch>.tarch -m model.onnx`)은 확인했지만 명령 세트와 동기화 세부는 이번에 원문을 보지 못했다.
- TPU v1: host가 PCIe로 CISC 명령(행렬 단위)을 보내고, weight FIFO와 unified buffer로 load와 행렬 곱을 겹친다. 원논문을 이번에 받지 않았다.

### 4.6 한 표로

| | 하드웨어 제어 | 명령 단위 | 명령 생성 때 | 의존성 | 동기화 | 층 사이 layout | conv 밖 연산 |
|---|---|---|---|---|---|---|---|
| **EVTA + NEST-C (지금)** | DAE 4모듈 | task(LOAD/GEMM/ALU/STORE) + uop | **추론 중 conv마다 CPU(JIT)** | 명령 속 token | **conv마다 폴링** | **conv마다 CPU transpose** | **CPU** |
| VTA + TVM | 같음 | 같음 | JIT | 같음 | 연산 묶음마다 | 구간 입구·출구 한 번 | bias·shift·clip을 ALU |
| Gemmini | DAE 3 controller | RoCC 행렬 명령, CISC loop | 실행 중 CPU, loop은 HW FSM | **HW ROB** | CPU 파이프라인 안 | scratchpad 유지 | pool, relu, im2col, transpose를 HW |
| NVDLA | 층 레지스터 | 하드웨어 층 | 컴파일러가 층 설정 | 소프트웨어 | 층마다 인터럽트, ping-pong | NVDLA 형식 | SDP/PDP/CDP 파이프라인 |
| DPU | 명령 fetch | 컴파일러 명령 | **AOT(xmodel)** | 명령 안(세부 비공개) | subgraph마다 | DPU 형식 | fusion, 대부분 DPU |

## 5. DAE 방식은 성능 면에서 좋은 편인가

**좋은 점(측정으로 확인됨)**
- 큰 conv에서 load·store를 GEMM 뒤로 완전히 숨긴다(stage4 93 %, ResConv10 단독 GEMM 가동률 93.5 %, `evta_multi4_609gops_report.md`).
- 하드웨어가 단순하다. 의존성 판단을 컴파일러/runtime이 token으로 넣으니 ROB(Gemmini)나 복잡한 스케줄러가 필요 없다. 그래서
  400 MHz와 4개 복제(Y219 fence 포함)가 됐다.
- 4개를 동시에 돌려도 EVTA마다 시간이 그대로다(대역 경쟁 없음, lockstep 617 GOPS).

**약한 점(구조에서 오는 것)**
- 명령 단위가 작다. conv 하나에 명령 수백~수천 개(conv0은 tile 98개)라 누군가 만들어야 하고, 그것을 CPU가 추론 중에 한다.
  → AOT(DPU)나 하드웨어 루프(Gemmini `LoopConv`)로 줄일 수 있다.
- conv마다 파이프라인을 비우고 다시 채운다. conv 경계에서 앞 conv의 store와 다음 conv의 load가 겹치지 않고, 1×1 같은 작은 conv는
  고정 비용(0.25 ms 안팎)에 묶인다. → 여러 conv를 한 명령 스트림으로 이어 붙이면(구간 제출) 경계에서도 token으로 겹칠 수 있다.
  DAE라서 가능한 일이다.
- 완료를 CPU가 폴링한다. 4개 동시 실행에서 4 ms tick 흔들림이 생겼다. → 인터럽트, 또는 구간 단위로 동기화 횟수를 줄이기.

**결론:** DAE 자체는 다른 설계와 견줘 손해가 없다(Gemmini도 DAE다). 손해는 "conv 하나 = 함수 하나 = 동기화 한 번 = layout 왕복 한 번"이라는
runtime·backend의 단위에서 난다. 같은 하드웨어로도 TVM-VTA처럼 layout을 유지하거나, DPU처럼 구간을 미리 만든 명령으로 돌리면 된다.

## 6. GEMM을 32×32, 64×64로 키우면

### 6.1 자원 (ZCU102 = XCZU9EG: DSP 2,520, BRAM36 912)

| | MAC/클럭 | DSP (MAC 1개/DSP) | DSP (int8 2개/DSP, WP486) | 비고 |
|---|---|---|---|---|
| 16×16 (지금) | 256 | 268(측정, single) | — | multi4는 4 × 268 = 1,072 |
| 32×32 | 1,024 | 약 1,040 | 약 520 | 들어간다 |
| 64×64 | 4,096 | 4,096 → **안 들어감** | 약 2,050 | DSP 81 %, 다른 로직 여유 거의 없음 |

- weight-stationary systolic은 weight를 tile마다 한 번 채우므로 weight SRAM 폭 부담은 HLS dot-product보다 작다. 대신 입력은 클럭마다
  BLOCK_IN 바이트, 출력 acc는 클럭마다 BLOCK_OUT × 4바이트가 필요하다(32×32: 입력 256비트, acc 1,024비트).
- 클럭: 어레이가 커지면 fanout과 배선이 늘어 400 MHz 유지가 어렵다(지금 16×16도 400에서 BRAM mux가 문제였다, `evta_systolic_design.md` D8).

### 6.2 Resnet18 EVTA 시간 예상 (systolic, 400 MHz 가정)

conv별 `_sysprof` 시간을, 연산은 1/4로 줄이고 DRAM 대역(HP 포트 하나, 실효 4 GB/s 가정)으로 아래를 막아 계산했다.

| 구간 | 16×16 측정 | 32×32 예상 | 막는 것 |
|---|---|---|---|
| conv0 | 9.13 ms | 약 5 ms | BLOCK_IN 32로 0 채움이 3 → 32(연산 2배), 실효 이득 약 2배 |
| stage1 (3×3 ×4) | 5.99 | 약 1.5 | 연산 |
| stage2 (3×3 ×4 + 1×1) | 5.23 | 약 1.5 | 연산, 1×1은 고정 비용 그대로 |
| stage3 | 4.72 | 약 1.4 | 연산 |
| stage4 (512, weight 2.36 MB/conv) | 4.61 | 약 2.4 | **weight 대역**: 2.36 MB / 4 GB/s = 0.6 ms/conv |
| 합 | **29.7 ms** | **약 12 ms (2.5배)** | |

- 큰 층일수록 weight가 커서 32×32에서는 대역에 막힌다. HP 포트를 EVTA 하나에 2개 붙이거나(지금 multi4는 EVTA마다 1개씩 씀),
  batch를 키워 weight를 재사용해야 4배에 가까워진다.
- **추론 전체:** OMP 4에서 73.6 → 약 56 ms(−24 %), OMP 1(multi4 copy)에서 148.9 → 약 131 ms(−12 %). CPU 몫(41 ms / 117 ms)이 그대로다.
- multi4(16×16 × 4)와 비교: DSP는 32×32 하나 ≈ 16×16 넷. 처리량(여러 이미지)은 multi4가 이미 617 GOPS(lockstep)로 비슷하고,
  32×32는 이미지 한 장의 지연을 줄인다. 무엇을 원하느냐(지연 대 처리량)로 고를 일이다.

## 7. 그래서 무엇부터 — 예상 효과 (single, OMP 4, 73.6 ms 기준)

| 순서 | 할 일 | 줄어드는 것 | 예상 추론 | 바뀌는 곳 | 하드웨어 |
|---|---|---|---|---|---|
| ① | **층 사이 EVTA layout 유지**: transpose와 conv마다 `VTABufferAlloc/Free` 없애기(TVM `graph_pack`처럼 구간 입구·출구에서만 변환) | transpose 15.8(conv0 zerofill 제외) + 버퍼 약 4.5 | 약 53 ms | NEST-C VTA backend(`VTASave.cpp`), vtalib CPU 연산이 EVTA layout을 받게 | 없음 |
| ② | **elemadd, relu, maxpool을 EVTA ALU로**(ADD, MAX — 지금 opcode로 됨). conv 뒤에 붙이면 store 전 SRAM에서 처리 | CPU 10.4 ms, 대신 EVTA ALU 시간이 조금 늘어남 | 약 44 ms | vtalib(ALU 명령 생성), backend | 없음(2비트 opcode로 가능) |
| ③ | **명령 AOT + 구간 제출**: 여러 conv를 한 스트림으로, 동기화 한 번 | 명령 생성 2.5, conv 경계 고정 비용 일부, 폴링 흔들림 | 약 41 ms | `evta_runtime_aot.md` §5.2 단계 0~1 | 없음(§5.1) |
| ④ | conv0: 3채널을 16에 바로 쓰는 zerofill(6.4 ms, OpenMP 없음) 개선, 또는 채널을 공간으로 접는 변환 | 6.4 ms 중 약 5 ms + conv0 EVTA 일부 | 약 36 ms | backend, vtalib | 없음 |
| ⑤ | **GEMM 32×32** | EVTA 29.7 → 약 12 | 약 19 ms | HW, vtalib 블록 크기, schedule 표 | 큼 |

- ①~④는 하드웨어를 바꾸지 않는다. ⑤를 먼저 하면 73.6 → 약 56 ms(−24 %), ①~④ 뒤에 하면 36 → 약 19 ms(−47 %)로 같은 작업의 효과가 두 배가 된다.
  ①~④ 뒤에 남는 것: EVTA 29.7 ms + CPU 약 6 ms(fc 1.2, 전처리 2.2, conv0 입력 변환, 구간 입구·출구).
- ①은 TVM-VTA가 같은 계열 하드웨어에서 이미 하는 방식이라 위험이 낮다. 다만 NEST-C 그래프에서 EVTA로 가는 구간을 묶는 패스가 필요하다
  (conv0 뒤 maxpool, 끝의 avgpool·fc가 경계).
- 각 숫자는 위 측정에서 해당 항목을 빼거나 비율로 줄인 추정이다. ②는 EVTA 시간이 조금 늘어난다(ALU 명령). ③의 conv 경계 이득은 측정하지 않았다.

### 7.1 ① 결과 (2026-10-31, `VTASave.cpp` `-vta-keep-layout`, 기본 켜짐)

**어떻게 되나.** EVTA layout은 N{C/16}HW16(NCHWc, c = 16 고정)이다. conv 출력이 이 layout으로 나오고 다음 conv도 이
layout을 읽으므로, 채널 수가 64 → 128처럼 바뀌어도 16채널 블록의 개수만 달라질 뿐 변환이 필요 없다(H, W 변화도
layout 안에 들어 있다). 그래서 `transpose_vtaio2nhwc` → `transpose_nhwc2vtaio` 짝은 항등 변환이었다. CPU 연산은:
- elemadd, relu: 같은 모양끼리 원소별이라 layout과 무관 — 그대로 호출.
- maxpool, avgpool: N{C/16}HW16 = "배치 N·C/16, 채널 16인 NHWC"이므로 `maxpool(.., N*C/16, H, W, 16, ..)`로 그대로 호출.
  avgpool의 1x1 출력은 두 layout이 같아서 fc가 그대로 읽는다.

`findPackedActivations`가 activation(4-D, N = 1, int8, C % 16 = 0, H·W > 1) 중 쓰는 명령이 모두 EVTA conv, elemadd,
relu, maxpool, avgpool인 것만 EVTA layout으로 두고(원소별 연산·pool의 피연산자는 함께), 그 activation을 처음부터
VTA 버퍼(`<name>_vbuf`, `<name>` = 가상 주소)로 잡는다. conv는 그 버퍼를 바로 읽고 쓰며, conv 뒤에 invalidate,
CPU 연산 뒤에 flush를 한다. 그래프 입출력, tensor view, 다른 연산이 닿는 activation은 NHWC 그대로라 다른 모델에서도 안전하다.
Resnet18에서 남은 transpose는 conv0 입력(`transpose_nhwc2vtaio_zerofill`) 하나다.

**측정 (systolic 400 MHz, `-n 25 -r`, 마지막 run).** 결과는 이전과 같다(Result 281, Confidence 0.655530 동일; multi4 4개 copy 모두 281).

| 경우 | 이전 | ① 후 | 변화 |
|---|---|---|---|
| single, OMP 4 (`_systimed`, 1회) | 73.6 ms | **56.4 ms** | −23 % |
| single, HLS 380 (`_timed`, 1회) | - | 57.6 ms | |
| multi4 copy 하나, OMP 1 (`_systimed`) | 153~160 ms | **94.9 ms** | −39 % |
| multi4 4 copy 100회 (`_sys`, OMP 1) | 3909 ms | **2793 ms** (35.8 추론/s) | −29 % |
| single exe 100회 (`_sys`, OMP 1) | 14959 ms | 9227 ms | −38 % |
| single, OMP 1 (`_systimed`, 1회) | 148.7 ms | 91.1 ms | −39 % |

single OMP 4 한 번(56.4 ms)의 내역: conv 호출 32.1(EVTA + JIT), conv0 입력 zerofill 6.2, elemadd 4.7, maxpool 4.1,
relu 2.5, `VTABufferAlloc` 2.2, fc 1.2, 전처리(elemsub/elemdiv/quantize) 2.2, Free 0.4, flush/invalidate 0.4, avgpool 0.2.
- 예상 53 ms보다 3 ms 많은 것은 버퍼 할당이 남았고(activation마다 `VTABufferAlloc` 2.2 ms — 한 번 잡아 두는 pool로 없앨 수 있다)
  maxpool이 채널 16 안쪽 루프로 바뀌어 조금 느려졌기 때문으로 보인다(이전 maxpool 단독 값은 남아 있지 않다).
- 다음은 ②(elemadd/relu/maxpool 11.3 ms를 EVTA ALU로)와 ④(conv0 zerofill 6.2 ms).

### 7.2 ② 결과: conv + elemadd + relu를 EVTA 한 번에 (2026-10-31, `-vta-fuse-residual`, 기본 켜짐)

**어떻게 되나.** EVTA conv는 출력 tile마다 입력 채널 전체의 reduction을 acc SRAM 안에서 끝내고, store 전에 ALU로
bias(ADD), requant(SHR), clip(MAX/MIN)을 한다(`conv_kernel`). Resnet18 v1 block의 `relu(conv(x) + residual)`은 그 자리에 몇 단계를 더 붙여 한 call로 한다
(`convolution_wo_tr_res`, vtalib `VTABundle.cpp` `ConvResidual`; `vtalib-zcu102-systolic`도 같다).
1. conv requant까지는 그대로 한다(conv 출력 int8 값 = 이전에 DDR에 쓰던 값).
2. `acc <<= convShl`(ALU ADD acc, acc).
3. residual tile을 inp SRAM에 출력 tile과 같은 배치로 LOAD한다(store와 같은 주소, conv GEMM이 끝난 inp 자리).
4. `acc += residual × (2^resShl·I)`(마지막 wgt entry의 단위행렬 weight로 GEMM).
5. `acc = (acc + 2^(m-1)) >> m`, `MAX 0`, `MIN 127` 뒤 STORE.

- scale이 2의 거듭제곱이라 곱셈이 필요 없고, 쓰는 opcode는 MIN, MAX, ADD, SHR(2비트로 됨)와 GEMM뿐이다. 하드웨어는 그대로다.
- 반올림은 EVTA가 .5 올림, CPU `std::round`가 0에서 먼 쪽이다. 음수 합에서만 다른데 relu가 0으로 만들어 결과가 같다.
  그래서 relu가 붙은 elemadd만 fusion한다.
- VTASave `findResidualFusions` 조건: conv 출력이 그 elemadd에서만 쓰이고, 피연산자가 모두 EVTA layout(①)이고, scale 비가 2^k이고, 출력이 conv 입력이 아니어야 한다.
  conv → elemadd → relu 사이에는 alloc/dealloc만 있어야 하고, relu 출력의 alloc이 conv 뒤에 있으면 conv 앞으로 옮긴다.
  Resnet18은 8개 elemadd+relu가 모두 fusion되어 CPU elemadd/relu가 없다. `-vta-fuse-residual=false`면 이전 코드가 그대로 나온다.
- 검증: `EVTA_RES_CHECK=1`이면 fused 출력을 일반 conv + CPU elemadd/relu와 원소 단위로 비교한다.
  single과 multi4(4 copy, single-thread exe 포함)에서 8개 모두 0개 불일치였다. Result 281, Confidence 0.655530은 이전과 같다.

**측정 (systolic 400 MHz, `run_prof.sh -r`, 25 run, run 2~25의 median; 같은 날 같은 보드에서 이전 bundle과 이어서 측정).**

| 경우 | 이전 (①만) | ② 후 | 변화 |
|---|---|---|---|
| single, OMP 1, 전체 run (`_systimed`) | 90.93 ms (51.2 GOPS) | **67.13 ms** (69.3 GOPS) | −26.2 % |
| single, OMP 4, 전체 run (`_systimed -t 4`) | 56.64 ms (82.1 GOPS) | **50.23 ms** (92.6 GOPS) | −11.3 % |
| single, EVTA call 합 (CPU에서 본 wall, OMP 1) | 32.07 ms | 33.37 ms | +1.3 ms |
| single, EVTA 하드웨어 시간 (`_sysprof`) | 29.68 ms (156.7 GOPS) | 30.80 ms (151.0 GOPS) | +1.1 ms |
| multi4 copy 하나, 4개 동시, 전체 run (`_systimed`) | 95.6 ms | **70.7 ms** | −26 % |
| multi4 4 copy 합, 전체 run | 194.6 GOPS | **262.7 GOPS** | +35 % |
| multi4 4 copy 합, conv busy union (`_systimed`) | 525.3 GOPS | 509.0 GOPS | −3 % |
| multi4 4 EVTA 동시, lockstep (`_sysprof -l`) | 616.6 GOPS | 593.9 GOPS | −4 % |
| multi4 4 copy 100회 wall (`_sys`) | 2470 ms (40.5 추론/s) | **1851 ms (54.0 추론/s)** | −25 % |
| multi4 비트스트림, single-thread exe 100회 (`_sys`) | 9231 ms | 6818 ms | −26 % |

- GOPS의 ops는 conv MAC만 센다(더하기는 넣지 않았다). 그래서 EVTA 시간이 1.1 ms 늘어난 만큼 EVTA GOPS는 조금 내려간다.
  1.1 ms는 8개 conv의 residual LOAD, 단위행렬 GEMM, ALU 명령에 든 시간이다.
- CPU에서 elemadd와 relu가 빠져 OMP 1에서 23.8 ms, OMP 4에서 6.4 ms가 줄었다. 처리량이 중요한 multi4(OMP 1)에서 효과가 크다.
- 아직 CPU에 남은 것: maxpool(conv0 뒤 하나), conv0 입력 zerofill, `VTABufferAlloc`, 전처리, fc.

## 8. 하드웨어도 같이 비교해야 하나

해야 한다. 다만 §7의 ①~④를 가르는 것은 어레이가 아니라 다음 네 가지다.

1. **ALU 연산 범위:** 지금 MIN/MAX/ADD/SHR(2비트). relu, add, maxpool은 되고, 곱셈(BN 접기 안 된 scale, avgpool 나눗셈)은 안 된다.
   NVDLA SDP/PDP, Gemmini의 pooling·scale 블록과 비교할 항목이다.
2. **STORE/LOAD의 layout 유연성:** 다음 층이 바로 쓸 수 있게 쓰는지(stride, 채널 블록), padding을 LOAD가 넣는지(VTA LOAD는 pad를 넣는다).
   ①의 전제다.
3. **명령을 누가 펼치나:** 지금은 CPU. Gemmini처럼 하드웨어 loop FSM을 두면 명령 수가 줄고 AOT 없이도 CPU 몫이 준다.
4. **완료 알림:** 지금 폴링. HLS 블록의 `ap_done`을 인터럽트로 연결하면 CPU를 쉬게 하고 tick 흔들림이 준다(PS-PL IRQ 배선 필요).

어레이 크기(32×32)는 이 넷이 정리된 뒤에 의미가 커진다(§6.2, §7).

## 9. 확인하지 못한 것

- DPU 명령의 의존성 처리와 내부 모듈 구성(PG338 Hardware Architecture 페이지에 세부가 없다). 인터럽트·레지스터 장은 읽지 않았다.
- TPU v1, Tensil 원문(§4.5는 일반 지식).
- TVM-VTA에서 residual add가 VTA ALU로 가는지 CPU로 가는지(`graphpack.py`는 layout만 다룬다; conv schedule 쪽 코드는 보지 않았다).
- §6.2의 4 GB/s 실효 대역과 32×32의 클럭은 가정이다. §7의 예상은 측정이 아니라 추정이다.

## 10. 출처

- VTA 원논문: T. Moreau et al., "A Hardware-Software Blueprint for Flexible Deep Learning Specialization", arXiv 1807.04188 (`docs/ref/arxiv_1807.04188.pdf`, §3.1, §3.2)
- Gemmini 논문: H. Genc et al., arXiv 1911.09925 (`docs/ref/arxiv_1911.09925.pdf`, §III-B, §IV); README: https://github.com/ucb-bar/gemmini (ISA, ROB, `gemmini_loop_ws`, `gemmini_loop_conv_ws`)
- NVDLA Hardware Architecture: https://nvdla.org/hw/v1/hwarch.html (programming model, ping-pong register groups, fused mode)
- AMD PG338 DPUCZDX8G Hardware Architecture: https://docs.amd.com/r/en-US/pg338-dpu/Hardware-Architecture
- AMD UG1414 Compiling for DPU: https://docs.amd.com/r/2.0-English/ug1414-vitis-ai/Compiling-for-DPU ; UG1354 Graph Runner: https://docs.amd.com/r/2.0-English/ug1354-xilinx-ai-sdk/Developing-with-Vitis-AI-API_3-Graph-Runner
- TVM VTA graph_pack: https://github.com/apache/tvm/blob/v0.10.0/vta/python/vta/top/graphpack.py
- 측정: 보드 `vtabundles_xcmpl/hw/Resnet18Test/systimed.log`(OMP 4, `-n 25 -r`), OMP 1 로그, `sysprof.log`(2026-10-29); `docs/evta_runtime_aot.md`
