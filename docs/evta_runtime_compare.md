# EVTA 제어 방식(DAE)과 runtime: 다른 가속기와 비교, GEMM을 키우기 전에 볼 것

작성 2026-10-29. 대상: EVTA(ZCU102, systolic 400 MHz / HLS 380 MHz), `vta/vtalib`, NEST-C VTA backend(`lib/Backends/VTA/VTASave.cpp`).
앞선 문서: `docs/evta_runtime_aot.md`(JIT vs AOT, §4에 다른 runtime 표가 있지만 "원문 미확인"이었다. 이 문서가 그 표를 원문으로 다시 확인하고 넓힌다),
`docs/evta_systolic.md` §9(하드웨어 어레이 조사).

질문: systolic GEMM을 32×32, 64×64로 키우면 좋아지겠지만, fetch/load/compute/store가 따로 돌며 dependency token을 주고받는
지금 방식이 다른 방식에 비해 성능 면에서 좋은 편인가? 하드웨어 구조도 같이 비교해야 하나?

## 1. 요약

### 1.1 지금 상태 (2026-11-03)

10-29의 분석(§1.2)과 계획(§7)대로 CPU 일을 거의 다 없앴다. **Resnet18 single 추론이 73.6 ms(OMP 4)에서 27.71 ms(OMP 4),
OMP 1로는 91.1 ms에서 28.83 ms가 됐다.** multi4 100회(4 copy 동시)는 3.9 s에서 0.820 s다. 모든 단계는 EVTA 출력이 이전과 byte 단위로 같거나 CPU
경로와 실행 중 비교해 같고, golden(single 30, multi2 3, multi4 3)을 통과했다. 전체 경과는 [evta_overview.md](evta_overview.md),
명령과 옵션은 [evta_user_manual.md](evta_user_manual.md)에 있다.

| 단계 | 절 | Resnet18 single, OMP 1 (systolic 400 MHz) | 끄는 방법 |
|---|---|---|---|
| 시작 (10-31 아침, keep-layout 직후) | §7.1 | 91.1 ms (OMP 4: 73.6 → 56.4) | `-vta-keep-layout=false` |
| conv + elemadd + relu 합치기 | §7.2 | 90.9 → 67.1 ms | `-vta-fuse-residual=false` |
| maxpool EVTA | §7.3 | 67.1 → 52.5 ms | `-vta-evta-maxpool=false` |
| transpose 16바이트 복사 | §7.4 | 52.5 → 46.9 ms | — |
| buffer pool | §7.5 | 46.9 → 44.3 ms | `EVTA_BUF_POOL_MB=0` |
| CPU fc NEON | §7.6 | 44.3 → 40.6 ms | — |
| multi4 멈춤 수정 (`dsb sy`) | §7.7 | (multi4 copy당 4000회 무멈춤) | `EVTA_NO_DSB=1` |
| fc EVTA | §7.8 | 40.6 → 39.9 ms | `-vta-evta-fc=false` |
| 입력 전처리 합치기 | §7.9 | 39.9 → 37.1 ms | `-vta-fuse-preprocess=false` |
| 명령 record / replay | §7.10 | 37.1 → 34.5 ms | `EVTA_REPLAY=0` |
| conv0 packed 입력 | §7.12 | 34.5 → 29.2 ms | `-vta-conv0-pack=false` |
| conv0 + maxpool 합치기 | §7.13 | 29.2 → **28.83 ms** (OMP 4 27.71) | `-vta-fuse-pool=false` |
| ALU imm 연산 II = 1 (bitstream) | §7.15 | 28.98 → **27.32 ms** (같은 날 측정) | 이전 bitstream `..._sa_400mhz` |
| RMW reset에 bias (bitstream) | §7.16 | 27.30 → **26.59 ms** (같은 날 측정) | `EVTA_BIAS_RESET=0` 또는 이전 bitstream |

- **남은 시간(§7.14):** EVTA 26.47 ms = GEMM 18.85(71 %) + ALU 4.63(17.5 %) + 그 밖 2.99. CPU와 호출은 약 2.4 ms다.
  - ALU가 큰 것은 HLS ALU 루프가 반복마다 acc를 두 번 읽어 II = 2이기 때문이다.
  - run 하나의 고정 비용은 4.7 µs뿐이다. 층 사이 DRAM 왕복을 모두 없애도 −2.36 ms(§7.11)라 하드웨어 경로는 만들지 않는다.
- **ALU imm 연산 II = 1 (§7.15, 11-03):** 새 systolic bitstream `..._sa_400mhz_alu1_epr`로 EVTA 26.42 → 24.89 ms, Resnet18 single OMP 1
  28.98 → 27.32 ms(같은 날 측정). bundle과 vtalib은 그대로다.
- **RMW reset에 bias (§7.16, 11-04):** bitstream `..._sa_400mhz_bias`로 EVTA 24.89 → 24.15 ms, Resnet18 single OMP 1 27.30 → 26.59 ms.
- **다음** 후보: conv0 tile 키우기, 1×1 downsample conv 합치기, 한 장을 EVTA 4개로, 다른 모델.

### 1.2 10-29 분석 당시의 요약

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
- 아직 CPU에 남은 것: maxpool(conv0 뒤 하나, → 7.3에서 EVTA로), conv0 입력 zerofill, `VTABufferAlloc`, 전처리, fc.

### 7.3 ② 나머지: maxpool을 EVTA로 (2026-10-31, `-vta-evta-maxpool`, 기본 켜짐)

**어떻게 되나** (`maxpool_evta`, vtalib `VTABundle.cpp`).
- 출력을 16채널 블록 하나의 oh × ow tile로 나눈다. ACC/INP의 반(1024 entry)에 들어가는 가장 큰 tile이고, Resnet18 pool0(112×112 → 56×56)은 3행 × 56열이다.
- tile마다 다음 순서로 한다.
  1. 입력 창 ((oh-1)·s+K) × ((ow-1)·s+K)를 LOAD한다.
  2. 단위행렬 GEMM으로 acc에 옮긴다.
  3. 출력 자리를 0으로 둔다.
  4. 창의 K×K 위치마다 ALU MAX(acc, acc) 명령을 하나씩 낸다. 명령 하나는 출력 pixel 전체를 도는 2단 loop이고 uop는 1개다.
  5. STORE한다.
- 같은 dst를 연달아 쓰는 uop 순서는 ALU pipeline에서 위험할 수 있어 피했다.
- tile 두 개를 inp/acc의 두 반쪽에서 동시에 처리해 load, compute, store가 겹친다.
- **tile 경계**: 출력 tile은 서로 겹치지 않는다. 입력은 tile마다 halo까지 DDR에서 다시 읽는다(3×3 s2이면 이웃 tile과 입력 1행이 겹친다).
  입력이 DDR에 있으므로 tile 사이에 넘길 상태가 없다.
- **이미지 가장자리**: LOAD의 padding 0으로 채운다. 입력이 0 이상일 때만 CPU(창 안의 유효한 값만으로 구하는 max)와 같다.
  그래서 입력을 마지막으로 쓴 명령이 relu가 붙은 EVTA conv(zero-fill conv0 포함)나 relu일 때만 EVTA로 보낸다(`findEvtaMaxPools`).
- conv0 안에 붙이면 DDR 왕복(0.8 MB 쓰기와 읽기)이 없어지지만, conv tile에 halo를 다시 계산해야 해서 하지 않았다.
- 검증: `EVTA_RES_CHECK=1`이면 CPU maxpool과 비교한다. single과 multi4에서 0개 불일치였고, Result 281과 Confidence 0.655530도 같다.

**측정** (7.2와 같은 조건; 왼쪽부터 ①만, ①+fusion, ①+fusion+EVTA maxpool)

| 경우 | ①만 | + conv/elemadd/relu fusion | + EVTA maxpool | ① 대비 |
|---|---|---|---|---|
| single, OMP 1, 전체 run | 90.93 ms | 67.13 ms | **52.48 ms** (88.6 GOPS) | −42 % |
| single, OMP 4, 전체 run | 56.64 ms | 50.23 ms | **48.71 ms** (95.5 GOPS) | −14 % |
| single, EVTA 하드웨어 시간 (`_sysprof`) | 29.68 ms | 30.80 ms | 31.98 ms (145.4 GOPS) | +2.3 ms |
| multi4 copy 하나, 4개 동시, 전체 run median | 95.6 ms | 70.7 ms | **56.1~56.5 ms** | −41 % |
| multi4 4 EVTA 동시, lockstep (`_sysprof -l`, conv MAC 기준) | 616.6 GOPS | 593.9 GOPS | 571.5 GOPS | −7 % |
| multi4 4 copy 100회 wall (`_sys`) | 2470 ms (40.5/s) | 1851 ms (54.0/s) | **1471 ms (68.0/s)** | −40 % |
| multi4 비트스트림, single-thread exe 100회 (`_sys`) | 9231 ms | 6818 ms | **5393 ms** | −42 % |

- maxpool은 OMP 1의 CPU에서 약 15 ms, OMP 4에서 약 3 ms였다. EVTA에서는 1.2 ms(21번째 EVTA call)다.
- multi4 `_systimed`(lockstep 없음)의 "4 copy 합"(run마다 가장 느린 copy)은 78.0 ms, 238.5 GOPS로 나왔다.
  copy마다의 median(56 ms)보다 크다. run마다 한 copy씩 늦는 run이 섞이기 때문이다(마지막 run에서 copy 1이 92 ms). 원인은 아직 보지 않았다.
  처리량은 `_sys` 100회 wall(1471 ms = copy당 58.8 ms/run)로 보는 것이 맞다.
- 이제 CPU에 남은 것(OMP 1, single 52.5 ms − EVTA call 33.4 ms 중): conv0 입력 zerofill, fc 4.4 ms, 전처리, `VTABufferAlloc`/Free, avgpool.

### 7.4 CPU transpose를 16바이트 복사로 (2026-11-01, vtalib `CPUBundle_generic.cpp`)

NHWC와 EVTA layout N{C/16}HW16 사이 변환은 픽셀마다 연속된 16바이트를 옮기는 것뿐인데, vtalib은 바이트 단위
(`transpose_nhwc2vtaio_zerofill`은 malloc한 NCHW 버퍼를 거치는 2단계)로 옮기고 있었다. 보드 빌드는
`NESTC_EVTA_RUN_WITH_GENERIC_BUNDLE=ON`이라 `CPUBundle_generic.cpp`를 쓴다(`CPUBundle_aarch64.cpp`도 같이 고침).
결과는 예전 코드와 같다(N = 1; `_zerofill`의 예전 코드는 N > 1에서 batch를 섞었다). systolic 복사본도 같다.

| transpose (run당) | 예전 | 16바이트 복사 |
|---|---|---|
| `transpose_nhwc2vtaio` 56×56×64, OMP 1 | 1.75 ms | 0.20 ms |
| `transpose_vtaio2nhwc` 56×56×64, OMP 1 | 3.31 ms | 0.26 ms |
| `transpose_nhwc2vtaio_zerofill` Resnet18 conv0 224×224×3 → 16, OMP 4 | 6.8 ms | 0.2 ms |

CMA 버퍼는 cache가 켜져 있어 malloc 메모리와 속도가 같다. 느렸던 것은 코드였다.

| 경우 (systolic 400 MHz, `-r`, run 2~25 median) | 7.3 후 | + 16바이트 transpose | 변화 |
|---|---|---|---|
| Resnet18 single, OMP 1, 전체 run | 52.48 ms | **46.90 ms** (99.2 GOPS) | −10.6 % |
| Resnet18 single, OMP 4, 전체 run | 48.71 ms | **41.81 ms** (111.3 GOPS) | −14.2 % |
| Resnet18 multi4 copy 하나, 4개 동시 | ~56.3 ms | **49.5 ms** | −12 % |
| Resnet18 multi4 4 copy 100회 wall (`_sys`) | 1471 ms (68.0/s) | **1314 ms (76.1/s)** | −10.7 % |
| Resnet18 multi4 비트스트림, single-thread exe 100회 | 5393 ms | **4724 ms** | −12.4 % |
| Conv4Test (56×56×64) 4 EVTA 100회 전체 (380 MHz, OMP 1) | 757 ms (122 GOPS) | **316 ms (293 GOPS)** | −58 % |
| Conv4Test 1 EVTA 100회 전체 | 703 ms | **243 ms** | −65 % |
| ResConv10Test 4 EVTA 100회 전체 | – | 161 ms (574 GOPS) | |

- EVTA 하드웨어 시간(`_sysprof` 31.98 ms, 4 EVTA lockstep 571.5 GOPS)은 그대로다. Result 281, Confidence 0.655530.
- Conv4Test는 conv만(`conv` 인자) 1.66 ms/run에 alloc/free와 transpose 0.75 ms가 남는다. 4 EVTA 전체(316 ms)가
  1 EVTA(243 ms)보다 느린 이유(CPU 쪽이 동시에 돌 때)는 아직 보지 않았다.
- multi4 `_systimed`의 "4 copy 합"(run마다 가장 느린 copy)이 49.7 ms로 copy마다의 median(49.5 ms)과 거의 같아졌다(7.3에서는 78.0 ms).

### 7.5 VTA buffer pool (2026-11-01, vtalib `lib/zcu102/api.cc`)

bundle은 run마다 activation buffer를 `VTABufferAlloc`/`VTABufferFree`로 잡고 놓는다(Resnet18 23개). 매번 `cma_alloc`/`cma_free`
ioctl이라 run당 alloc 2.3 ms + free 0.4 ms였다. 이제 `VTABufferFree`는 buffer를 크기별 free list에 남기고 같은 크기의
`VTABufferAlloc`이 꺼내 쓴다(mutex, multi4의 thread 4개가 같은 list를 쓴다). list에 남기는 양은 `EVTA_BUF_POOL_MB`(기본 32) MB까지,
`EVTA_BUF_POOL_MB=0`이면 예전처럼 바로 해제한다. 남은 buffer는 process가 끝날 때 해제된다. 재사용 buffer에는 이전 내용이 남아
있다(`cma_alloc`도 지우지 않는다). bundle 재생성 없이 vtalib만 바뀐다. systolic 복사본도 같다.

| 경우 (systolic 400 MHz, `-r`, run 2~25 median) | pool 끔 | pool 켬 | 변화 |
|---|---|---|---|
| Resnet18 single, OMP 1, 전체 run | 46.97 ms | **44.27 ms** (105.1 GOPS) | −5.7 % |
| Resnet18 single, OMP 4, 전체 run | 41.89 ms | **39.07 ms** (119.0 GOPS) | −6.7 % |
| run당 `VTABufferAlloc` + `Free` (23 + 23) | 2.7 ms | 0.05 ms | |
| Resnet18 multi4 4 copy 100회 wall (`_sys`) | 1286 ms | **1189 ms (84.1/s)** | −7.5 % |
| Resnet18 multi4 비트스트림, single-thread exe 100회 | 4740 ms | **4460 ms** | −5.9 % |
| Conv4Test 4 EVTA 100회 전체 (380 MHz, OMP 1, 3번) | 322~325 ms | **243~254 ms** (~375 GOPS) | −23 % |
| ResConv10Test 4 EVTA 100회 전체 (3번) | 172~189 ms | 147~176 ms | 흔들림이 더 크다 |

- OMP 4에서 첫 run의 alloc이 200 ms 걸린다(OMP 1은 1.5 ms). pool이 없으면 run 2에도 생겼다. pool로는 run 1만 남는다.
- run_all.sh hw 전부 PASS, multi4 전부 PASS. Result 281, Confidence 0.655530 그대로.

### 7.6 CPU fc (2026-11-01, vtalib `CPUBundle_generic.cpp` `cpuFullyConnected`)

Resnet18 dense(512 → 1000, weight int8 512 KB)는 OMP 1에서 4.4 ms였다. 출력 64개씩 나눠 k마다 weight 행의 64바이트를 읽어서,
conv들 뒤 cache에 없는 weight를 1000바이트 간격으로 건너뛰며 DRAM에서 읽었다(prefetch가 안 된다). 이제 thread마다 출력의 연속 구간을
맡고, weight 행을 순서대로 8행씩 읽으며 출력 64개의 합을 NEON register 16개에 둔다(int8 × int8 → int16 → int32, offset이 0일 때;
아니면 scalar). 다음 8행은 prefetch한다. 정수 합이라 결과는 bit 단위로 같다(보드 bench 576가지 크기·offset·thread 수 비교, FCTest PASS).

| 경우 (systolic 400 MHz, `-r`, run 2~25 median) | 7.5 후 | + fc | 변화 |
|---|---|---|---|
| fc, OMP 1 | 4.43 ms | **0.74 ms** | −83 % |
| fc, OMP 4 | 1.20 ms | **0.77 ms** | −36 % |
| Resnet18 single, OMP 1, 전체 run | 44.27 ms | **40.56 ms** (114.7 GOPS) | −8.4 % |
| Resnet18 single, OMP 4, 전체 run | 39.07 ms | **38.68 ms** (120.3 GOPS) | −1.0 % |
| Resnet18 multi4 copy 하나, 4개 동시(`-l` 없이) | – | 41.2 ms (4 copy 451 GOPS) | |
| Resnet18 multi4 4 copy 100회 wall (`_sys`) | 1189 ms | **1089 / 1131 ms** (~90/s) | −6 ~ −8 % |
| Resnet18 multi4 비트스트림, single-thread exe 100회 | 4460 ms | **4094 ms** | −8.2 % |

- bench(cache를 밀어낸 상태)에서는 OMP 4가 0.23 ms인데 bundle 안에서는 0.77 ms로 OMP 1과 같다. 이유는 아직 보지 않았다(thread를 깨우는 비용?).
- multi4 `_sys` 100회가 4번 중 2번 멈췄다(68번째 run 근처, `-l` 없이도; 7.5 전후로 `-l`에서도 1번). 조사는 §7.7. timeout으로 죽은 뒤
  CMA가 ~60 s 동안 돌아오지 않아 다음 bitstream load가 ENOMEM으로 실패한다: run 스크립트는 load를 5 s 간격으로 120 s까지 다시 시도하고 `timeout -k 10`을 쓴다.

### 7.7 multi4 멈춤: run 시작 직전에 짧게 기다리면 사라진다 (2026-11-01~02, vtalib `cmd_queue.h`)

- 멈추는 곳은 늘 1×1 downsample conv **바로 다음** 3×3 stride-2 conv다(Resnet18 13번째 `stage3_conv0`, 18번째 `stage4_conv0`).
  HLS 380 MHz, systolic 400 MHz 둘 다 멈추고, 4 EVTA가 같이 돌 때만 멈춘다(single exe는 수천 번 0).
- 멈춘 순간: fetch는 명령어를 다 내보냈고(`0x06`), load/compute/store는 auto-restart로 대기, compute의 FINISH가 없다.
  명령어·uop 버퍼는 DRAM과 cache가 같다(멈춘 뒤 5 s에 비교). 의존성 FIFO는 깊이 1024, 명령어 FIFO는 512라 가득 찰 수 없다.
- systolic multi4 Resnet18, copy당 100회씩 반복:

| run 앞에서 하는 일 | copy당 추론 | 멈춤 |
|---|---|---|
| 없음 | 약 400회 | 1번 (EVTA2, 13번째 호출) |
| STORE 영역을 `/dev/mem`으로 0xA5로 채움 (`EVTA_STORE_SENTINEL=1`) | 2000회 | 0번 |
| 바쁜 대기 40 / 10 / 5 µs (`EVTA_PRE_RUN_US`) | 각 2000회 | 0번 |
| EVTA0에만 40 µs (`EVTA_PRE_RUN_MASK=1`) | 약 500회 | 1번 (대기 없는 EVTA1, 18번째 호출) |

- 원인을 가르는 실험(2026-11-02, 같은 조건, copy당 100회씩 반복, 멈추면 중단):

| run 앞에서 하는 일 | 멈춤 |
|---|---|
| 없음 (대조) | 7번째 100회에서 (EVTA1) |
| 5 µs 대기를 uop/명령어 복사와 cache flush **앞**에 (`EVTA_PRE_RUN_AT=1`) | 2번째 100회에서 |
| 대기 없음, uop/명령어 버퍼를 반쪽씩 번갈아 씀 (`EVTA_DOUBLE_QUEUE=1`) | 4번째 100회에서 |
| 대기 없음, cache flush 뒤 `dsb sy` | **4000회 0번** |

- 원인: PYNQ 2.4 `libcma.so`의 `cma_flush_cache`(와 `cma_invalidate_cache`)는 `dc civac` loop뿐이고 `dsb`가 없다(objdump).
  Arm에서 cache 정리는 `dsb` 뒤에야 끝났다고 보장되므로, EVTA를 시작하는 MMIO 쓰기가 정리된 명령어/uop가 DRAM에 닿기 전에
  나갈 수 있다. 4 EVTA가 DRAM을 같이 쓸 때 그 틈이 길어져 fetch가 덜 쓰인 명령어를 읽고 멈춘다(앞 run의 짧은 downsample 뒤에 CPU가
  다음 run을 빨리 만드는 3×3 stride-2 conv에서 드러났다). flush 뒤의 대기나 sentinel 채우기(`/dev/mem` 쓰기)가 그 틈을 메웠고,
  flush 앞의 대기나 다른 주소에 쓰기는 소용이 없었다.
- 고침: vtalib `pynq_driver.cc` `VTAFlushCache` / `VTAInvalidateCache`가 libcma 다음에 `dsb sy`를 한다(`EVTA_NO_DSB=1`이면 뺀다).
  5 µs 대기(`EVTA_PRE_RUN_US`)는 원인을 찾는 동안 기본 5였고, dsb로 고친 뒤 기본 0으로 되돌렸다(`EVTA_PRE_RUN_US=5`로 다시 켬).
  비용은 측정 차이 안이었다(single exe 100회 4336 ms 대 4330 ms).

### 7.8 fc를 EVTA로 (2026-11-02, `VTASave.cpp` `-vta-evta-fc`, 기본 켜짐; vtalib `fullyconnected_evta_packed`)

- fc를 1×1 conv 경로로 보내면 최소 tile 7×7 때문에 연산이 49배가 되어, fc 전용 kernel을 따로 썼다(`VTABundle.cpp`).
  - 출력 블록 o마다 weight 16×16 entry IN/16개가 이어지게 둔다(출력 블록 우선, 출력은 16의 배수로 0 padding).
  - 이 weight를 wgt 버퍼 반쪽(512 entry)씩 한 번에 LOAD하고, 다른 반쪽을 GEMM하는 동안 읽는다(Resnet18: 4덩어리 × 출력 블록 16개).
  - GEMM은 c에 대한 loop 하나, o마다 uop 하나다. 같은 acc entry가 g(≥ 8) uop마다 돌아온다.
  - bias는 acc의 초기값으로 LOAD한다.
- 반올림은 CPU의 `std::round`(0.5를 0에서 먼 쪽으로)와 같게 ALU만으로 한다:
  `s = max(min(acc, 0), -1)`, `acc = (acc + s + 2^(shift-1)) >> shift`, clip.
  처음에 쓴 `SHR 31`은 왼쪽 1 밀기였다. ALU shift 인자는 5비트 부호 있는 수(-16..15)다.
- weight와 bias는 compile time에 EVTA layout으로 weights 파일에 쓴다(Resnet18: weight 516,096 B, bias 4,032 B). CPU는 입력 512 B와
  출력 1000 B를 복사하고 명령어를 만들 뿐이다. vtalib `fullyconnected_evta`(CPU layout weight를 첫 호출에 변환)도 남겨 두었다.
- 조건이 안 맞으면 compile time에 CPU `fullyconnected`로 간다. 조건은 batch 1, offset 0, IN이 16의 배수, scale 비가 2^-shift(0..15),
  덩어리당 출력 블록 ≥ 8이다. 그래서 Resnet50의 2048→1000(덩어리당 4블록)과 VGG16의 512→10은 CPU에 남는다.
- 결과: `EVTA_RES_CHECK=1`에서 Resnet18 512→1000, VGG16 512→512 모두 0개 차이다. 보드 bring-up 탐침은 `EVTA_FC_PROBE=1`이다.
  golden single 30, multi2 3, multi4 3 PASS. Resnet18·Partition·Resnet50은 281, VGG16은 airplane(0)이고, multi2와 multi4의 모든 추론이 281이다.

| 경우 (systolic 400 MHz, `_systimed -r`, run 2~25 median) | 7.6 후 (CPU NEON fc) | EVTA fc |
|---|---|---|
| fc 한 번 | 0.74 ms (OMP 1) / 0.77 ms (OMP 4) | **0.152 ms** |
| Resnet18 single, OMP 1 | 40.56 ms | **39.92 ms** |
| Resnet18 single, OMP 4 | 38.68 ms | **37.99 ms** |
| Resnet18 multi4 copy 하나, 4개 동시 | 41.2 ms | **40.56 ms** (4 copy 458.6 GOPS) |

- 남은 CPU 몫(single OMP 4, 37.99 ms 중):
  - EVTA 호출 22개 안의 CPU 몫(명령어 생성, 시작, 대기) 약 3 ms
  - 입력 전처리 2.2 ms(elemdiv 0.94, elemsub 0.67, quantize 0.58)
  - avgpool 0.23 ms 등

### 7.9 입력 전처리를 conv0 입력 복사 하나로 (2026-11-02, `VTASave.cpp` `-vta-fuse-preprocess`, 기본 켜짐; vtalib `preprocess_nhwc2vtaio_zerofill`)

- 0으로 채운 첫 conv(`ConvPath::FirstBNN`) 앞의 float 입력 quantize와 그 뒤의 elemsub / elemdiv(채널마다 같은 int8 상수),
  NCHW → NHWC transpose(하나까지)를, conv0 입력을 EVTA layout으로 쓰는 복사 한 번에 한다(`findInputPreprocess`).
  각 값은 다음 명령 하나만 써야 한다.
  - elemsub / elemdiv 사슬은 quantize한 값(256개)과 채널에만 달렸으므로, compile time에 표 [C][256]로 만들어 weights 파일에 쓴다.
    표는 CPU elemsub / elemdiv와 같은 float 계산을, bundle이 쓰는 scale(`1.0/<1/scale 소수 6자리>`)로 한다.
  - Resnet18: quantize → elemsub(mean) → elemdiv(std) → zerofill, 표 있음.
  - Resnet50, VGG16: quantize → transpose → zerofill, 표 없음.
  - Resnet18Partition은 conv0이 float이라 해당 없다.
- vtalib는 16 pixel씩 NEON으로 한다.
  - quantize는 `vcvtaq`(0.5를 0에서 먼 쪽으로, `std::round`와 같음)와 포화 narrow로 한다. scale이 2의 거듭제곱이면 나누기 대신 곱하기다(결과가 같다).
  - 표는 `vqtbl4q`/`vqtbx4q` 4번이다.
  - pixel은 `c0 c1 c2 0` word 하나와 0 12바이트로, `vst4q_u32`로 쓴다.
- 검사: 보드 단위 시험에서 0개 차이였다. 시험은 vtalib quantize+elemsub+elemdiv+zerofill 대 새 함수이고, 0.5 경계·포화·2의 거듭제곱이 아닌 scale,
  W 224/37, NHWC/NCHW를 넣었다. golden single 30, multi2 3, multi4 3 PASS, confidence는 그대로다
  (Resnet18 0.655530, Partition 0.530282, Resnet50 0.581379, VGG16 0.453709).
- 앞으로 하드웨어 ALU로 옮길 때: quantize(float → int8)는 CPU나 입력 단계에 남는다. 2의 거듭제곱 scale의 elemsub는 ALU ADD/SHR로 된다.
  채널별 나누기(std)는 MUL이 없어 안 된다. 표를 쓰거나, std를 conv0 weight에 접는 방법이 있다(접으면 경계 padding이 달라져 bit-exact는 아니다).

| 경우 (systolic 400 MHz, `_systimed -r`, run 2~25 median) | 7.8 후 | 전처리 합침 (+ §7.7 dsb) |
|---|---|---|
| 입력 전처리 | 2.2 ms + zerofill 0.2 ms | **0.37 ms** |
| Resnet18 single, OMP 1 | 39.92 ms | **37.08 ms** |
| Resnet18 single, OMP 4 | 37.99 ms | **36.04 ms** |
| Resnet18 multi4 copy 하나, 4개 동시 | 40.56 ms | **37.59 ms** (4 copy 494.8 GOPS) |
| multi4 100회 (`run_all`) | 1115.8 ms | **1060.5 ms** |

### 7.10 명령 record / replay (AOT 0단계, 2026-11-02, vtalib `cmd_queue.h` `ReplayBegin`)

같은 인자의 EVTA 호출(conv, conv+residual, maxpool, fc)은 첫 호출에 기록한 명령과 uop를 버퍼 주소만 고쳐 다시 실행한다.
자세한 내용은 `docs/evta_runtime_aot.md` §5.4에 있다. bit-exact이고, golden PASS, multi4 copy당 4000회 동안 멈춤이 0번이었다.

| 경우 (systolic 400 MHz, `_systimed -r`, run 2~25 median) | 7.9 후 | replay |
|---|---|---|
| Resnet18 single, OMP 1 | 37.08 ms | **34.50 ms** |
| Resnet18 single, OMP 4 | 36.04 ms | **33.48 ms** |
| Resnet18 multi4 copy 하나, 4개 동시 | 37.59 ms (494.8 GOPS) | **34.99 ms** (531.6 GOPS) |
| multi4 100회 (`run_all`) | 1060.5 ms | **977.9 ms** |

### 7.11 층 사이 DRAM 왕복은 얼마나 드나 (측정, 2026-11-02, vtalib `cmd_queue.h` `EVTA_REPLAY_LOG=2`, `EVTA_MEMEXP`)

질문: activation을 DRAM에 쓰고 다음 층이 다시 읽는 일을 없애면(층을 SRAM 안에서 잇기) EVTA 시간이 얼마나 줄어드나.
systolic bitstream에는 하드웨어 카운터와 AXI 모니터가 없어서(`_sysprof`의 해당 열이 0) replay 사본으로 쟀다.

- `EVTA_REPLAY_LOG=2`: replay된 run마다 명령에서 센 GEMM / ALU 안쪽 반복 수, LOAD(uop, wgt, inp, acc)와 STORE(out)의
  DRAM 바이트, `VTADeviceRun`의 실측 µs를 찍는다. 400 MHz에서 반복 하나 = 1 cycle로 보면 GEMM + ALU가 연산의 하한이다.
- `EVTA_MEMEXP=<mask>`: replay 사본의 LOAD / STORE를 원소 하나로 줄인다(bit 0 inp, 1 wgt, 2 acc, 3 STORE; pad는 그대로).
  결과는 쓰레기지만 명령 수, 의존성, 연산은 그대로라서 **그 전송이 없을 때의 시간**이 나온다. 층을 SRAM 안에서 이었을 때 줄어들 수 있는 시간의 상한이다.
- 조건: Resnet18 single, `_sysprof -n 25 -r`, OMP 1, replay된 run 2~25의 run별 median. (fc는 이때 replay되지 않아 빠졌다. 고친 것은 아래에 있다.)

| | ms | 기준 대비 |
|---|---|---|
| EVTA 시간 (mask 0) | **32.18** | |
| GEMM 반복 / 400 MHz | 23.60 | conv0 6.27 (0 채움으로 13/16이 버려짐) |
| ALU 반복 / 400 MHz | 2.32 | |
| inp LOAD 없음 (1) | 30.80 | −1.38 |
| STORE 없음 (8) | 31.20 | −0.98 |
| acc LOAD 없음 (4; bias, residual) | 32.17 | −0.01 |
| **activation 전부 없음 (13 = inp + acc + STORE)** | **29.82** | **−2.36** |
| weight LOAD 없음 (2) | 31.39 | −0.79 |
| DRAM 전송 전부 없음 (15) | 29.13 | −3.05 |

DRAM 바이트(추론 한 번): weight 16.87 MB(conv0가 tile마다 다시 읽어 4.8 MB), inp 5.37 MB(halo를 다시 읽는 몫 포함), out 2.56 MB,
uop 0.42 MB, acc 0.05 MB.

activation 전부 없음(−2.36 ms)을 어디서 줄었는지로 나누면:

| 구간 | 줄어든 시간 | 무엇 |
|---|---|---|
| conv0 | 1.13 ms | 입력 읽기 0.77(네트워크 입력, 층을 이어도 남음), 출력 쓰기 0.36(maxpool 입력) |
| maxpool | 0.18 ms | |
| conv 19개 (stage1~4) | 1.05 ms | conv당 0.03~0.11 ms; residual은 0 |

- **conv→conv를 SRAM 안에서 이으면 줄어드는 것은 최대 약 1.05 ms(EVTA 시간의 3 %)다.** 하드웨어 변경(out→inp 경로,
  store→load 의존성 큐, inp 64 KB에 stage1~2가 안 들어가서 tile 단위 fusion과 halo 재계산)을 들여도 이 이상은 안 된다.
  DAE가 load와 store를 이미 거의 다 GEMM 뒤에 숨긴다.
- **conv0 + maxpool fusion**(소프트웨어만, conv0 tile의 acc에서 ALU MAX 후 store)은 conv0 store 0.36 ms와 maxpool run(1.19 ms 중
  identity GEMM 0.33 + 고정 비용)을 없앤다. ALU MAX는 남는다. pool 경계 행을 다시 계산하는 만큼 빼면 0.5~1 ms로 본다.
- 전송을 모두 없애도 29.13 ms가 남는다. GEMM + ALU 하한 25.92 ms와의 차이 3.2 ms는 메모리가 아닌 비용이다(run마다 고정 비용,
  uop LOAD, 의존성 대기 등; 나눠 재지는 않았다). conv0 GEMM 6.27 ms 중 0 채움으로 버리는 몫(약 5 ms)이 가장 큰 덩어리다.
- 측정 중 발견: bundle이 부르는 `fullyconnected_evta_packed`에는 replay가 없었다(`fullyconnected_evta`에만 있었음). 고친 뒤
  기록 21개(fc 41개 명령), fc 0.15 → 0.14 ms, golden PASS(single 30, multi2 3, multi4 3), `EVTA_RES_CHECK` fc 불일치 0;
  single OMP 1 34.48 ms, multi4 copy당 34.99 ms(변화 없음).

### 7.12 conv0을 packed 입력의 4×1 conv로 (2026-11-02, `VTASave.cpp` `-vta-conv0-pack`, 기본 켜짐; vtalib `preprocess_conv0pack`)

**왜:** conv0(7×7/2, 3채널)은 채널을 16으로 0 채워 돌렸다. GEMM 반복의 13/16이 0을 곱하는 데 쓰였다(§7.11: GEMM 6.27 ms, EVTA 9.13 ms).

**어떻게:** stride 2의 행 두 개와 kw 7개를 채널로 넣는다.
- 입력: packed 픽셀 (q, x)의 채널 (dh·7 + j)·3 + c = pad된 입력 (2q + dh, 2x + j, c). 2·7·3 = 42 → 48채널(3 블록), 115×112.
- conv: 4×1, stride 1, pad 0 (`convolution_wo_tr` 그대로). filter 탭 (k, (dh·7 + j)·3 + c) = 원래 (2k + dh, j, c), 2k + dh = 7이면 0.
  정수 합이 같으므로 출력이 같다.
- GEMM 반복: 출력 픽셀·출력 블록당 49 → 12(4 탭 × 3 블록). schedule 2 thread, 16×16(`VTASchedules.h`; 입력 tile 19×16×3 = 912 entry, multi-EVTA inp의 절반 1024에 들어감).
- 일반형: KH×KW/stride s, C채널이면 ceil(KH/s)×1 / stride 1 conv, CO = s·KW·C를 16으로 올림(`getConv0Pack`). schedule tile이 출력을
  나누고 입력 tile이 1024 entry에 들어갈 때만 쓰고, 아니면 이전의 0 채움 경로다(`prepareFirstBNNVTAConvolutionInst`,
  `preprocess_nhwc2vtaio_zerofill` / `transpose_nhwc2vtaio_zerofill`은 그대로 남겼다; `-vta-conv0-pack=false`로도 그 경로).
  Resnet18, Resnet50(같은 conv0), VGG16 cifar(3×3/1 → 3×1 over 16)에 적용된다.
- 입력 복사(`preprocess_conv0pack`, 전처리 §7.9 포함): packed 행 q는 pad된 입력 행 2q, 2q+1만 읽고 그 두 행은 다른 q가 읽지 않는다.
  스레드마다 그 두 행만 작은 버퍼(약 1.4 KB)에 quantize + table로 쓰고 곧바로 pack한다(pad된 이미지를 메모리에 두지 않는다).
  pack은 픽셀마다 두 행의 32바이트를 64바이트 표로 `vqtbl4q_u8` 한 번에 16채널 블록 하나씩 만든다.

**검증:** `EVTA_DUMP_CONV=<dir>`(vtalib, EVTA conv 호출마다 출력 버퍼를 파일로)로 0 채움 bundle과 새 bundle의 **모든 EVTA conv 출력이
byte 단위로 같다**(Resnet18 40개, Resnet50 106개, VGG16 26개, Partition 36개, Resnetv10S1C0 2개). golden PASS(single 30, multi2 3,
multi4 3), Result / Confidence 그대로.

**측정** (systolic 400 MHz, `_systimed -r`, run 2~25 median; replay 켜짐)

| 경우 | 7.10 후 | conv0 pack |
|---|---|---|
| conv0 EVTA 시간 (`EVTA_REPLAY_LOG=2`) | 9.13 ms (GEMM 하한 6.27) | **3.64 ms** (GEMM 하한 1.63) |
| EVTA 시간 합 (`_sysprof`) | 32.18 ms | 26.82 ms |
| 입력 복사 (`preprocess`, OMP 4) | 0.36 ms | 0.58 ms → 행 단위 **0.44 ms** |
| Resnet18 single, OMP 1 | 34.48 ms | 29.81 ms → 행 단위 **29.19 ms** |
| Resnet18 single, OMP 4 | 33.48 ms | 28.16 ms → 행 단위 **28.02 ms** |
| Resnet18 multi4 copy 하나, 4개 동시 | 34.99 ms | 30.49 ms → 행 단위 **29.76 ms** |
| multi4 100회 (`run_all`) | 977.9 ms | 910.9 ms (한 번은 895.4) → 행 단위 **827.3 ms** |
| single exe 100회 (`run_all`) | 3620.5 ms | 3119.7 ms → 행 단위 **3066.7 ms** |

- GOPS 열은 bundle의 conv 모양으로 연산 수를 세므로 packed conv0(4×1×48)는 원래(7×7×3)와 연산 수가 달라 이전 값과 비교할 수 없다.
- 입력 복사, 보드 단독 측정(OMP 1, Resnet18 conv0 입력): 0 채움 1.55 ms, pad 이미지 + pack 2.18 ms, 행 단위 1.69 ms.
  pad 이미지를 없앤 것보다 컸던 것은 pack 루프의 aliasing이다: byte store가 어떤 메모리든 가리킬 수 있어 `d`의 필드와 표 index를
  픽셀마다 다시 읽었다. 지역 변수로 옮기자 pack이 0.88 → 0.35 ms. 남은 대부분(약 1.35 ms)은 quantize 계산이고 0 채움 경로도 같다.
  6가지 모양(7×7/2, 3×3/1, 5×3/2, 11×11/2(범용 경로), NCHW / NHWC)에서 0 채움 결과와 불일치 0.
- 출력 tile 16×16은 짝수이고 112를 나눈다: 다음 conv0 + maxpool fusion(pool 3×3/2의 경계)이 tile 경계와 맞는다.

### 7.13 conv0 + maxpool을 EVTA 호출 하나로 (2026-11-02, `VTASave.cpp` `-vta-fuse-pool`, 기본 켜짐; vtalib `convolution_wo_tr_pool`)

**어떻게** (vtalib `ConvPool`, conv_kernel의 store 앞).
- conv0 출력 tile(16×16)을 pool window의 halo만큼 위로 1행, 왼쪽으로 1열 더 계산한다(17×17; 이미지 경계 쪽 tile은 그 행 / 열이
  pad라서 더하지 않는다). GEMM이 약 13 % 늘어난다(1.63 → 1.84 ms).
- requant / relu 뒤 acc 안에서 ALU MAX 두 단계: 행 방향(pool 중심 열 2j + halo가 좌우 열과 max, 제자리, 명령 2개),
  열 방향(bias 뒤의 8×8×블록 영역을 0으로 reset하고 중심 열의 2i + halo + dy − 1 행 3개와 max, 명령 3개). 명령마다 그 thread의
  출력 블록 2개를 uop 2개로(연속 uop는 다른 entry에 쓴다). conv 값이 ≥ 0(relu)이라 0에서 시작한 max가 CPU의 경계 안 max와 같다.
- pooled 영역만 STORE한다. conv0 출력(784 KB)은 DRAM에 쓰지 않고, maxpool 호출(LOAD 784 KB, identity GEMM, ALU)이 없어진다.
- 컴파일러(`findPoolFusions`): packed conv0(§7.12) + relu 바로 다음이 EVTA maxpool 3×3/2/1이고 conv 출력의 다른 사용자가 없으며
  schedule tile이 짝수일 때. pool 출력 할당을 conv 앞으로 올린다. vtalib는 모양이 맞지 않으면 conv 후 `maxpool_evta`로 돌린다.

**검증:** `EVTA_DUMP_CONV`(이번에 maxpool 출력도 덤프)로 fusion 전 bundle의 maxpool 출력과 그 뒤 모든 EVTA 출력이 fused bundle의
출력과 byte 단위로 같다(Resnet18 40개, Resnet50 106개; HLS 빌드와 systolic 빌드 모두). `EVTA_RES_CHECK=1`(fused와 conv + CPU maxpool
비교) 200,704개 중 0개 불일치. golden PASS(single 30, multi2 3, multi4 3).

**측정** (systolic 400 MHz, `_systimed -r` / `_sysprof`, run 2~25 median)

| 경우 | 7.12 후 (따로) | fusion 1판 (창 위치마다 명령) | fusion (2단계) |
|---|---|---|---|
| conv0 + maxpool EVTA | 3.64 + 1.19 = 4.83 ms | 4.80 ms | **4.48 ms** |
| EVTA 시간 합 | 26.82 ms | 26.78 ms | 26.47 ms |
| Resnet18 single, OMP 1 | 29.19 ms | 29.25 ms | **28.83 ms** |
| Resnet18 single, OMP 4 | 28.02 ms | 28.02 ms | **27.71 ms** |
| Resnet18 multi4 copy 하나 | 29.76 ms | 29.70 ms | **29.37 ms** |
| multi4 100회 (`run_all`) | 827.3 ms | 823.1 ms | 820.3 ms |
| single exe 100회 (`run_all`) | 3066.7 ms | 3028.7 ms | 3025.1 ms |

- 1판은 창 위치 9개 × 블록마다 ALU 명령 하나(thread·tile당 19개)였고, 줄어든 DRAM 왕복을 명령 수가 다 잡아먹었다(ALU 명령 하나에
  약 130 cycle의 고정 비용으로 보인다). 2단계 + 블록당 uop로 thread·tile당 6개(reset 포함).
- 남은 손해: halo 재계산 GEMM 약 0.2 ms, 늘어난 ALU 명령. §7.11의 예상(0.5~1 ms)보다 작은 0.35 ms다.

### 7.14 EVTA 시간 26.47 ms는 어디에 쓰이나 (측정, 2026-11-02, vtalib `cmd_queue.h` `EVTA_REPLAY_DUMP=<dir>`)

**방법:** `EVTA_REPLAY_DUMP=<dir>`가 기록된 run마다 명령(필드, 의존성 비트)과 uop을 `rec_<id>_<seg>.txt`로 쓴다.
`EVTA_REPLAY_LOG=2`의 run별 실측(24 inference median)과 맞춰 본다. Resnet18 `_sysprof`, systolic 400 MHz, inference당 run 21개.

| 덩어리 | ms | 몫 | 근거 |
|---|---|---|---|
| GEMM 반복 / 400 MHz | 18.85 | 71 % | systolic, 1 cycle / 반복 |
| ALU 반복 × **2** / 400 MHz | 4.63 | 17.5 % | HLS `alu` 루프가 `PIPELINE II = 2`(`vta-hw-zcu102-systolic/.../vta.cc`); 회귀 계수도 1.9 |
| 나머지 | 2.99 | 11 % | conv0 1.08, 그 밖의 run 20개가 run당 60~130 µs |

ALU 4.63 ms의 내역(반복 × 2 cycle):

| ALU 연산 | ms | 무엇 |
|---|---|---|
| ADD (acc의 bias, 블록마다 broadcast) | 0.91 | conv마다 (conv0 0.28) |
| SHR imm | 0.90 | requantize shift (conv0 0.28) |
| MIN 127 | 0.98 | 위쪽 clip (conv0 0.28) |
| MAX 0 / MAX −128 | 0.69 / 0.29 | relu / 아래쪽 clip |
| MAX (pool, conv0) | 0.45 | §7.13 |
| ADD imm (반올림 상수) | 0.16 | shift가 작은 층 |
| residual ADD | (bias ADD에 포함) | §7.2 |

- imm 연산(SHR, MIN, MAX imm, ADD imm)이 ALU 반복의 69 %다. acc를 하나만 읽는 연산이라 하드웨어에서 II = 1로 돌릴 여지가 있다
  (그것만 해도 약 −1.5 ms, 전부 II = 1이면 −2.3 ms).
- 나머지 2.99 ms:
  - conv0 1.08 ms: tile 98개(2 thread × 49)마다 약 11 µs. tile마다 halo 모양이 달라 GEMM uop(약 1100개)을 다시 LOAD하고
    (648 KB/inference, compute 모듈에서 순차), ALU 명령 9개, GEMM 명령 5개.
  - stage 2~4의 3×3 conv: run당 60~70 µs로 거의 일정하다(run 시작·끝의 고정 비용으로 보인다: 첫 tile LOAD, 마지막 STORE,
    명령 fetch 등; 따로 재지 않았다). stage 1은 105~117 µs.
  - 1×1 downsample conv 3개: 333 / 263 / 303 µs 중 GEMM 78 / 71 / 67 µs. GEMM이 작아 LOAD가 숨지 못한다(나머지 129~205 µs).
  - fc: 133 µs 중 GEMM 5 µs(weight 504 KB LOAD).
- 회귀(실측 = GEMM + 2·ALU + a·명령 수 + b): run당 상수 약 100 µs, run 21개에 rms 20 µs. 명령 수 계수는 서로 얽혀 따로 믿기 어렵다.

**run 하나와 명령 하나의 비용** (`vtabundles_xcmpl/hwtest/runbench.cpp`, systolic 400 MHz, `VTASynchronize` 시간 200회 median;
case를 한 프로세스에서 모두 돈다: runtime의 CMA는 `xlnk_reset`으로만 돌아온다)

| 측정 | 값 |
|---|---|
| 가장 작은 run (reset, STORE 1) | **4.7 µs** (CPU flush, MMIO 시작, done poll 포함) |
| GEMM 반복 | 1.0 cycle (1 uop × 4096: 2.33 ns, 16 × 1024: 2.55 ns) |
| GEMM 명령 하나 (1 uop, 1 반복) | 약 25 cycle |
| ALU 반복 (imm도 reg도) | **2.0 cycle** (16 × 2048) |
| ALU 명령 하나 (1 반복) | 약 35 cycle |
| inp LOAD / STORE 큰 것 | 5.0 / 5.2 GB/s |
| inp LOAD / STORE 명령 하나 (1 block) | 0.29 / 0.21 µs |
| 서로 다른 uop kernel 256개 (`gemmu 256 1`) | EVTA 멈춤(조사하지 않음; bundle에는 이런 run이 없다) |

- **run 고정 비용은 4.7 µs뿐이다.** run 21개 × 4.7 µs = 0.1 ms라 run을 합쳐도 줄 것이 거의 없다. 3×3 conv의 run당 60~70 µs는
  run 시작·끝이 아니라 conv 안의 몫(첫 tile LOAD가 숨지 못하는 시간, 마지막 ALU와 STORE 등)이다.
- ALU는 imm도 2 cycle이다. HLS 보고서(`vta-hw-zcu102-systolic/build/.../compute_csynth.rpt`): `EXE_OUT_LOOP_EXE_IN_LOOP_READ_ALU_UOP`
  flatten, II 2, depth 11. acc를 두 번 읽고(src, dst) 한 번 쓰는데 acc_mem(T2P BRAM) 포트가 둘이라서다.
- Resnet18의 imm ALU 명령은 같은 run 안에서 acc 주소를 16 반복 안에 다시 쓰지 않는다(덤프로 확인). II = 1로 바꿔도 겹침이 생기지 않는다.

**정리:** GEMM 밖의 7.6 ms 중 **ALU가 4.6 ms로 가장 크고 하드웨어 II 때문이다.** 소프트웨어만으로 줄일 수 있는 것은 conv0 tile
고정 비용(약 1 ms 중 일부), downsample conv의 LOAD(약 0.3 ms) 정도다(run 합치기는 run당 4.7 µs라 이득이 없다).

### 7.15 ALU imm 연산을 II = 1로 (2026-11-03, bitstream `..._sa_400mhz_alu1_epr`, `vta-hw-zcu102-systolic` `vta.cc` `alu()`)

그림: [구조 14](figs/evta_st14_alu_ii1_regs.svg).

**어떻게:** systolic 빌드(`VTA_SYSTOLIC`)의 `alu()`에 `use_imm`인 명령만 도는 루프를 따로 둔다. 반복마다 acc를 dst 한 번만 읽고(포트 A)
한 번 쓰므로(포트 B) II = 1이다. reg 연산(bias ADD, pool MAX)은 그대로 II = 2 루프다. ISA, vtalib, 컴파일러, bundle은 바뀌지 않는다.
- 같은 반복 안에서 읽은 dst를 몇 clk 뒤 쓰므로, 그 사이 같은 dst를 다시 읽으면 안 된다(acc는 `DEPENDENCE inter false`).
  Resnet18의 ALU 명령 1,566개를 `EVTA_REPLAY_DUMP`로 보면 imm 명령은 16 반복 안에 같은 주소를 다시 쓰지 않는다.
- timing(400 MHz):
  - 첫 빌드: uop 읽기 → 주소 덧셈 → acc_mem 주소(BRAM 58개)가 한 clk이라 WNS −0.819 ns(실패 9,031).
  - uop 읽기 뒤와 주소 계산 뒤에 register(`alu_reg`, `INLINE off` + 반환 register; `hls::reg`는 2020.1에서 내부 namespace)를 넣어
    WNS −0.126 ns(`Performance_ExtraTimingOpt`). 남은 실패는 acc LOAD padding(`load_pad_2d` / `reset_mem`)의 acc 쓰기 enable 등으로
    퍼져 있었고, 배선 후 `phys_opt_design -directive AggressiveExplore`로는 −0.124 ns였다.
  - `IMPL_STRATEGY=Performance_ExplorePostRoutePhysOpt`로 **WNS +0.001 ns**, WHS +0.010 ns, WPWS +0.001 ns. LUT 28.3K → 34.1K.
    여유가 1 ps라, 다음 빌드에서 모자라면 acc LOAD padding 쓰기와 imm 루프의 acc 쓰기 데이터 앞에 register를 더 넣는다(HLS가 주소·데이터·enable을 같이 민다).
- 보드 bit dir: `/home/xilinx/JO/npuprg/bit/evta_sys_alu1_400MHz_pass_261103/` (README 있음).

**검증:** golden 30 PASS(`run_prof.sh -s _systimed -n 2 all`, `BIN=<새 bitstream>`). Resnet18 281 / 0.655530, Resnet50 281 / 0.581379,
Partition 281 / 0.530282, VGG16 0 / 0.453709로 전과 같다. `EVTA_DUMP_CONV`로 Resnet18 40개, Resnet50 106개 파일이 기존 systolic
bitstream과 byte 단위로 같다.

| 측정 (systolic 400 MHz, 같은 실행 파일) | `..._sa_400mhz` | `..._sa_400mhz_alu1_epr` |
|---|---|---|
| runbench `aluimm 16 2048` (ALU imm 반복) | 169.74 µs (2.0 clk) | **87.37 µs (1.0 clk)** |
| runbench `alureg 16 2048`, `gemm 16 1024` | 169.74 / 46.12 µs | 169.54 / 46.42 µs (그대로) |
| Resnet18 EVTA 시간 (`_sysprof`, 25회 평균) | 26.42 ms | **24.89 ms** (−1.53 ms) |
| Resnet18 single OMP 1 (`_systimed -r`, run 2·3·25 중앙값) | 28.98 ms | **27.32 ms** |

예상(§7.14의 −1.5 ms)과 같다. 다음은 RMW reset에 bias를 쓰는 것(bias ALU ADD 0.91 ms)이다.

**multi4 (2026-11-04, `..._4evta_sa_400mhz_alu1.bin`, 보드 `bit/evta_multi4_alu1_400MHz_pass_261104/`):**
- 같은 vta.cc로 첫 빌드는 WNS −0.343 ns였다. 경로는 RESET_MEM 루프 제어 → acc 쓰기 enable(BRAM 58개)과 ALU 결과 → acc 쓰기 데이터.
  - `reset_mem`을 `alu_reg`로 주소를 레지스터에 담아 II = 1로 파이프라인했다(systolic 사본만, 바깥 `load_pad_2d` y 루프의 PIPELINE은 뺐다).
  - imm 루프와 II = 2 루프에서 acc / out에 쓰기 전에 `alu_reg`를 하나씩 넣었다. imm 루프 깊이 10, II = 2 루프 11.
  - single 사본에도 같은 vta.cc가 들어갔다. single bitstream `alu1_epr`은 이 변경 전 것이다.
- Y219 flow 뒤 WNS +0.001, WHS −0.032(smc_3 FIFO 한 net). `holdnets.tcl`(그 net만 다시 배선)로는 그대로였다.
  `flow/holdpin.tcl`(그 끝점에 최소 배선 지연 310 ps)로 WHS +0.007, Y219 PIP 0, stuckchk 0 extra.
- 보드: probes 40/40, golden 3 PASS, Resnet18 4 copy / single 정상, `EVTA_DUMP_CONV` 80개가 이전 multi4 bitstream과 같다.
- Resnet18 EVTA 시간(`_sysprof -n 25 -l -r`): 4 copy 동시 EVTA마다 26.73~26.85 → **25.24~25.37 ms**, single copy 26.49 → **24.97 ms**.
  `hw_multievtax4/script/run_all.sh`의 `SYS_BIN`을 이것으로 바꿨다.
- multi4 `_sysprof` 실행 파일이 11-01 것이라 지금 bundle과 맞지 않아 죽었다(두 bitstream 모두 exit 139). `build_multievtax4_sysprof`에서 `make -j2 hw_bundles`로 다시 빌드했다.

### 7.16 RMW reset에 bias (2026-11-04, bitstream `..._sa_400mhz_bias`)

그림: [구조 15](figs/evta_st15_bias_reset.svg).

**어떻게:** conv의 bias를 GEMM 누산의 시작값으로 쓴다. 지금까지는 GEMM reset이 acc를 0으로 쓰고, GEMM 뒤에 bias를 acc에 LOAD한 뒤
ALU ADD로 더했다. 이제는 bias LOAD를 tile 시작으로 옮기고, reset GEMM이 0 대신 acc[bias + f]를 acc[base + f·T + p]에 쓴다. 정수 덧셈이라 결과는 같다.
- ISA: `VTAGemInsn`의 pad 1비트 `bias_reset`(hw_spec.h 3곳). reset이면서 bias_reset이면 uop의 src가 acc 주소다.
- HLS(`vta.cc`, systolic과 multi4 사본 같음): GEMM uop stream의 비트 `VTA_SA_UOP_B + 5`. stream은 1비트 넓어진다(single 48비트).
- RTL: `sa_ctrl`가 tag에 bias와 src를 싣고, `sa_rmw`는 bias 행이면 acc[src]를 읽어 행을 더하지 않고 dst에 쓴다.
  forwarding은 읽는 주소로 비교한다.
- vtalib(`vtalib-zcu102-systolic` `conv_kernel`): `VTAUopPush` reset_out 2 = bias reset. 이름에 `bias`가 있는 bitstream에서만 켠다
  (`/tmp/evta_bitstream`, `EVTA_BIAS_RESET=0|1`로 강제). 다른 bitstream에서는 전과 같다(golden 30 PASS, `alu1_epr`).
- 검증 순서: C 모델(`sim/systolic_model.cc`, 모드 `bias`, `biashazard`) → RTL testbench 48 vector × (그대로, GAP·BP 30 %) 96/96 PASS
  → HLS CSim 0 errors → 보드.
  - `build/rtl_sim`의 오래된 `xsim.dir`이 있으면 `xvlog`가 CPU 100 %로 멈췄다(`xsim.dir.old_261104`로 치움).
- timing(400 MHz, `Performance_ExplorePostRoutePhysOpt`): WNS +0.000 ns, WHS +0.010, WPWS +0.001. LUT 34.3K, BRAM 258, DSP 273. stuckchk 0 extra.
  - 이 빌드부터 single에도 acc / out 쓰기 앞 register와 RESET_MEM 파이프라인(§7.15 multi4)이 들어 있다.
- 보드 bit dir: `/home/xilinx/JO/npuprg/bit/evta_sys_bias_400MHz_pass_261104/` (README 있음).

**검증:** probes 10/10, golden 30 PASS(`run_prof.sh -s _systimed -n 2 all`). Resnet18 281 / 0.655530, Partition 281, Resnet50 281 / 0.581379,
VGG16 0 / 0.453709로 전과 같다. `EVTA_DUMP_CONV`로 Resnet18 40개, Resnet50 106개 파일이 `alu1_epr`과 byte 단위로 같다.

| 측정 (같은 실행 파일, 같은 날) | `..._sa_400mhz_alu1_epr` | `..._sa_400mhz_bias` |
|---|---|---|
| Resnet18 EVTA 시간 (`_sysprof -n 25 -r`, runs 2-25 중앙값) | 24.889 ms | **24.154 ms** (−0.735 ms) |
| Resnet18 single OMP 1 (`_systimed -n 25 -r`, runs 2-25 중앙값) | 27.303 ms | **26.592 ms** |

예상(bias ADD 0.91 ms)보다 0.15~0.18 ms 적게 줄었다. bias LOAD는 전에도 tile마다 한 번이었고 위치만 앞으로 옮겼으므로 원인이 아니다.
같은 `bias` bitstream에서 `EVTA_BIAS_RESET=0|1`로 conv별 시간(`_sysprof -n 25 -r`, runs 2-25 중앙값)을 비교했다.
`EVTA_MEMEXP`로 DRAM traffic도 뺐다(결과는 쓰레기, 시간만 본다).

| conv | 그대로 | traffic 모두 뺌(`EVTA_MEMEXP=15`) | ADD 반복 × 2 cycle |
|---|---|---|---|
| conv0 (+ pool) | −0.316 ms | −0.292 | 0.28 |
| stage1 3×3 (4개, 개당) | −0.064 ~ −0.068 | −0.062 ~ −0.064 | 0.063 |
| stage2 (5개, 개당) | −0.019 ~ −0.035 | −0.031 ~ −0.036 | 0.031 |
| stage3 (5개, 개당) | +0.001 ~ −0.010 | −0.016 | 0.016 |
| stage4 (5개, 개당) | +0.002 ~ −0.004 | −0.007 ~ −0.009 | 0.008 |
| 합 | **−0.760** | **−0.832** | 0.91 |

- traffic을 빼면 층마다 ADD 시간만큼 줄어든다. stage3/4에서는 tile 끝의 ADD가 다음 tile의 inp LOAD나 앞 tile의 STORE와 겹쳐 있었다.
  그래서 빼도 시간이 거의 줄지 않는다. inp만 빼면(`EVTA_MEMEXP=1`) stage3 conv1/conv4, stage4 conv1/conv4가 줄고,
  STORE만 빼면(`8`) stage3 conv0/conv2/conv3, stage4 conv0/conv2/conv3이 준다. wgt(`2`)나 acc(`4`)만 빼면 바뀌지 않는다.
- traffic을 빼도 0.832 ms이고 0.91보다 적다. §7.14가 반복 × 2 cycle로 셈해서 조금 크게 잡은 것이다(회귀 계수 1.9).
`hw/script/run_all.sh`의 `SYS_BIN`을 이것으로 바꿨다.

**multi4** (`..._4evta_sa_400mhz_bias.bin`, 같은 소스, 보드 bit dir `/home/xilinx/JO/npuprg/bit/evta_multi4_bias_400MHz_pass_261104/`):
- 빌드: `flow/m4_build.sh bias SYSTOLIC=1 FREQ=400 IMPL_STRATEGY=Performance_ExtraTimingOpt`(이 스크립트를 처음 씀). impl_1은 opt_design 뒤에서
  멈췄고 HLS부터 bitstream까지 약 1.5시간 걸렸다. WNS +0.037 ns, WHS +0.010, WPWS +0.001, Y219 PIP 0, stuckchk 0 extra, holdpin 필요 없음.
  LUT 133.3K, BRAM 580, DSP 1092.
- 검증: probes 40/40(EVTA 0-3), `hw_multievtax4/script/run_prof.sh -s _systimed -n 2 all`에서 Conv4, ConvTest, ResConv10 PASS,
  Resnet18 ×4 / ×1 exit 0. `EVTA_DUMP_CONV` 80개 파일이 `..._4evta_sa_400mhz_alu1`과 byte 단위로 같다.
- 시간(`_sysprof -n 25 -l -r`, runs 2-25 중앙값, 두 bitstream을 번갈아 두 번씩, 두 번 모두 ±0.02 ms 안):

| Resnet18 EVTA 시간 | `..._4evta_sa_400mhz_alu1` | `..._4evta_sa_400mhz_bias` |
|---|---|---|
| 4 copy 동시, EVTA마다 | 25.23-25.37 ms | **24.49-24.62 ms** (−0.75) |
| copy 하나(100회) | 24.97 ms | **24.22 ms** |

`hw_multievtax4/script/run_all.sh`의 `SYS_BIN`도 이것으로 바꿨다.

### 7.17 bias reset 다음에 남은 것 (2026-11-04)

지금 Resnet18 single은 EVTA 24.15 ms, 추론 26.59 ms다. §7.14의 측정에 그 뒤 바뀐 것(§7.15 imm II = 1, §7.16 bias reset)을 반영한 추정은 다음과 같다.

| 덩어리 | 대략 | 비고 |
|---|---|---|
| GEMM | 18.85 ms | 16×16, 400 MHz, 1 cycle / 반복. 이론 최소 17.7 ms(3628 Mops ÷ 204.8 GOPS)의 94 % |
| ALU 남은 것 | 약 2.0 ms | imm 연산(SHR, MIN, MAX, 반올림 ADD) 약 1.5(II = 1), pool MAX 0.45(II = 2) |
| 그 밖의 고정 비용 | 약 3 ms | conv0 tile 1.08, run마다 60~130 µs(첫 LOAD, 마지막 STORE, 명령 fetch), load-bound 층 |
| CPU (추론 − EVTA) | 약 2.4 ms | 전처리, softmax, run 호출 |

하드웨어:
1. **acc → out 변환에 requantize** (약 −1.5 ms, 가장 큼) → **했다(§7.18, −1.10 ms)**
   - 지금은 GEMM과 ALU가 단계마다 acc의 하위 8비트를 out에 쓴다(`sa_rmw.v` `out_wr_data`, HLS `vta.cc`의 `accum.range(7, 0)`).
     STORE는 그 int8 out을 DRAM으로 옮기기만 한다. 그래서 STORE 모듈이 아니라 이 변환 자리에서 반올림 → shift → clip(relu 포함)을
     하고, ALU 명령 4~5개(ADD imm, SHR, MIN, MAX)를 없앤다.
   - out은 단계마다 덮어쓰므로 tile의 마지막 GEMM이 쓴 값이 남는다. 추가 cycle 없이 변환 pipeline 단만 늘어난다.
   - ISA: GEMM 명령의 `pad_0`(31비트 남음)에 shift 5비트, relu 1비트, 반올림 1비트, 켜기 1비트 정도면 된다. `memory_type`,
     `alu_opcode`의 2비트 필드는 손대지 않는다. 이전 bitstream은 새 비트를 무시하므로, vtalib은 bias reset처럼 bitstream 이름으로 켜고 끈다(끄면 지금의 ALU 명령).
   - 고려할 점: residual(identity GEMM으로 acc에 더함)과 conv0 + pool(acc에서 MAX)은 순서가 다르다. 처음에는 일반 conv에만 쓴다.
2. **reg-reg ALU를 II = 1로** (약 −0.25 ms): pool MAX와 residual ADD는 acc를 한 번에 두 개 읽어야 한다. acc bank를 나누거나
   읽기 포트를 늘려야 해서 BRAM 비용이 크다.
3. **GEMM 자체** (큰 작업): 이미 이론치의 94 %다. 어레이를 키우거나(DSP 여유: single 273, multi4 1092 / 2448) clock을 올려야 한다
   (400 MHz WNS 0; 32×32, 64×64의 자원과 예상은 §6). 어레이를 키우면 16채널 단위 layout이 ISA, vtalib, 컴파일러 전체에 걸려 있다. DSP 하나에 int8 곱 두 개를
   넣는 것은 속도가 아니라 DSP를 아낀다.

소프트웨어(bitstream 그대로):

4. **run 사이 고정 비용** (약 −1 ms): 층마다 EVTA run을 따로 시작하는 대신 여러 층을 명령 stream 하나로 이어, 다음 층의 LOAD가
   앞 층의 STORE와 겹치게 한다(replay, AOT stage 0을 넓히기).
5. **conv0 tile 키우기**(single EVTA는 inp buffer 전체), **1×1 downsample conv를 같은 입력의 conv와 합치기** (각각 수백 µs).
6. **한 장을 EVTA 4개로 나눠 latency 줄이기** (약 26 → 8~10 ms 예상): 지금 multi4는 copy 4개로 처리량만 늘린다. 층마다 출력
   채널이나 행을 나누고, EVTA 사이를 동기화해야 한다.
7. **CPU 2.4 ms**: 전처리, softmax, run 호출 overhead를 나눠 잰다.

방향:

8. **runtime / driver**: `/dev/mem` mmap과 bitstream 이름으로 기능을 판별하는 대신 HAL 경계(buffer, 명령, 완료 대기)를 정하고
   DRM accel driver로 옮긴다([npu_stack_survey.md](npu_stack_survey.md) §2.3).
9. **다른 모델**: Resnet50과 VGG16은 돌지만 시간을 줄인 것은 Resnet18뿐이다. MobileNet 등은 아직 해 보지 않았다.

EVTA 하나의 속도는 1번, 실제 추론 latency는 6번이 가장 크다.

### 7.18 acc → out 쓰기에 requantize (2026-11-04, bitstream `..._sa_400mhz_rq`)

필드와 실행 방법은 [evta_isa_ext.md](evta_isa_ext.md)에 정리했다. 그림: [구조 16](figs/evta_st16_requantize.svg),
단계별 시간 [구조 17](figs/evta_st17_summary.svg).

**어떻게:** GEMM과 ALU는 단계마다 acc의 하위 8비트를 out에 쓴다. 지금까지는 그 값이 결과가 되도록 ALU SHR / MAX / MIN(반올림 ADD)으로
acc 자체를 바꿨다. 이제는 명령의 `rq_*` 필드(en, round, shift, lo, hi)로 out에 쓰는 값을 clip((acc + round) >> shift, lo, hi)로 만들고,
그 ALU 명령을 없앤다. acc는 바뀌지 않는다.
- ISA: `VTAGemInsn` 비트 141~164, `VTAAluInsn` 144~167(pad 자리). round 0 없음, 1 half up, 2 half away from zero. `rq_en = 0`이면 전과 같다.
- RTL `sa_rmw.v`: acc 쓰기(S3) 뒤에 S4 덧셈, S5 산술 shift, S6 clip. out 쓰기, done, token, busy를 S6으로 옮겼다(forwarding은 그대로).
  `sa_ctrl.v`는 rq 24비트를 FIFO와 tag로 실어 보낸다(uop stream + 24비트).
- HLS `vta.cc`: `requant()`를 ALU 두 루프와 CSim의 GEMM 모델에. HLS 추정 clock과 critical path는 bias 빌드와 같다.
- vtalib `VTARequant(cmd, en, round, shift, lo, hi)`. 일반 conv는 GEMM에, conv + pool은 열 방향 pool MAX에, residual은 residual GEMM에,
  fc는 fc GEMM에 쓴다. 이름에 `_rq`가 있는 bitstream에서만 켠다(`EVTA_REQUANT=0|1`로 강제). `_rq`는 bias reset도 켠다.
- 식의 확인: C 모델 `rqout()`이 지금 vtalib의 ALU 순서(conv, residual 끝, fc)와 5,750만 값에서 같다.
- 검증 순서: C 모델 무작위 300 → RTL testbench 49 vector × (그대로, GAP·BP 30 %) 98/98 → HLS CSim 0 errors → 보드.
- 함께 고친 것: vtalib `NextInsn()`과 `test_lib.cc` `getGEMMInsn()`이 명령을 0으로 채우지 않아 pad 비트가 쓰레기였다. 이제 0으로 채운다.
  새 vtalib으로 `bias` bitstream에서 돌린 Resnet50 층 출력 106개는 전과 같다.
- timing(400 MHz, `Performance_ExplorePostRoutePhysOpt`): WNS +0.039 ns, WHS +0.006, WPWS +0.001. LUT 34.3K → 42.1K, BRAM 258, DSP 273. stuckchk 0 extra.
- 보드 bit dir: `/home/xilinx/JO/npuprg/bit/evta_sys_rq_400MHz_pass_261104/` (README 있음).

**검증:**
- probes 10/10. `run_prof.sh -s _systimed -n 2 all`에서 golden 27 PASS. Resnet18 281 / 0.655530, Partition 281 / 0.530282, Resnet50 281 / 0.581379,
  VGG16 0 / 0.453709로 전과 같다.
- `EVTA_DUMP_CONV`: Resnet18 40개, Resnet50 106개가 `bias` bitstream과 byte 단위로 같다. `EVTA_RES_CHECK=1` 20건 불일치 0.
- 우회: 같은 `_rq` bitstream에서 `EVTA_REQUANT=0`이면 Resnet18 40개가 `bias`와 같다.

| 측정 (같은 실행 파일, 같은 날) | `..._sa_400mhz_bias` | `..._sa_400mhz_rq` |
|---|---|---|
| Resnet18 EVTA 시간 (`_sysprof -n 25 -r`, runs 2-25 중앙값, 번갈아 두 번) | 24.153 / 24.154 ms | **23.053 / 23.054 ms** (−1.10) |
| Resnet18 single OMP 1 (`_systimed -n 25 -r`, runs 2-25 중앙값) | 26.611 ms | **25.493 ms** |

conv별로 conv0(+ pool) −0.47 ms, stage1 −0.06~−0.13, stage2 −0.02~−0.07, stage3/4 −0.01~−0.03, fc −0.005 ms다. 예상 −1.5 ms(§7.14의 imm 연산 시간)보다 적다.
bias reset처럼(§7.16) stage3/4에서는 ALU 시간 일부가 inp LOAD / STORE와 겹쳐 있었던 것으로 보인다(`EVTA_MEMEXP`로 따로 재지 않았다).
`hw/script/run_all.sh`의 `SYS_BIN`을 이것으로 바꿨다.

**multi4** (`..._4evta_sa_396mhz_rq.bin`, 같은 소스, 보드 bit dir `/home/xilinx/JO/npuprg/bit/evta_multi4_rq_396MHz_pass_261104/`):
- 빌드: `flow/m4_build.sh rq SYSTOLIC=1 FREQ=400 IMPL_STRATEGY=Performance_ExtraTimingOpt`. LUT 133.3K → 164.1K(EVTA 4개 × 약 7.7K), BRAM 580, DSP 그대로.
  Y219 flow 뒤 400 MHz WNS −0.127 ns(115 endpoint). route 뒤 `phys_opt_design`(`route_s.tcl opt`)으로 −0.007 ns가 됐지만 row Y219에 PIP 1개가
  남아, 그 net만 RapidWright로 다시 옮겼다(`write_edif` 후 `Y219RWRoute.java`, `route_s.tcl opt2`). 남은 경로는 `store_queue_1` FIFO36 cascade의
  RDEN(2.5 ns 중 1.77 ns, 로직 2단). `phys_opt`을 더 돌려도 −0.007 ns 그대로라, placement · route는 두고 MMCM 배수만 12 → 11.875
  (`mmcm_set.tcl rs_mrqo2.dcp rs_mrqm 11.875`, 395.8 MHz): WNS +0.004 ns, WHS +0.006, WPWS +0.001, row Y219 PIP 0, stuckchk 0 extra.
- 보드: probes 40/40, `run_prof.sh -s _systimed -n 2 all` golden 3 PASS, Resnet18 `EVTA_DUMP_CONV` 80개가 `bias` bitstream과 byte 단위로 같다.

| Resnet18 EVTA 시간 (`_sysprof -n 25 -l -r`, 4 copy 동시, runs 2-25 중앙값, 두 번) | `..._4evta_sa_400mhz_bias` | `..._4evta_sa_396mhz_rq` |
|---|---|---|
| copy 0 / 1 / 2 / 3 | 24.49 / 24.62 / 24.62 / 24.51 ms | **23.65 / 23.77 / 23.77 / 23.66 ms** (−0.85, −3.5 %) |
| 4 EVTA busy union | 583.8 GOPS | **605.1 GOPS** |
| 25회 `total_time` | 706.4 ms | **684.0 ms** |

clock이 1 % 낮아 single(−1.10 ms)보다 덜 줄었다.

**multi4 400 MHz** (`..._4evta_sa_400mhz_rq.bin`, 같은 synth, 보드 bit dir `/home/xilinx/JO/npuprg/bit/evta_multi4_rq_400MHz_pass_261104/`):
- placer directive만 바꿔(`VTA_PLACE_DIRECTIVE=ExtraNetDelay_high flow/run_flow.sh mrqp2`; `ExtraTimingOpt`은 −0.009 ns에서 멈춤) Y219 flow 전 +0.002 ns,
  RapidWright 뒤 −0.205 ns. `route_s.tcl opt`(post-route phys_opt)로 +0.002 ns가 됐고, row Y219에 PIP이 남은 net 1개(fetch_3 CONTROL_BUS FSM,
  100 MHz 쪽이라 timing path 없음)는 `Y219RWRoute.java -Dtd=0`(wirelength-driven; timing-driven이면 RWRoute 2025.2가 통계 출력에서 예외)으로 옮기고
  `route_s.tcl`(phys_opt 없이): **WNS +0.002 ns, WHS +0.005, WPWS +0.001**, row Y219 PIP 0, stuckchk 0 extra.
- 보드: probes 40/40, golden 3 PASS, Resnet18 `EVTA_DUMP_CONV` 80개가 395.8 MHz 판과 byte 단위로 같다.

| Resnet18 EVTA 시간 (`_sysprof -n 25 -l -r`, 번갈아 두 번) | `..._4evta_sa_396mhz_rq` | `..._4evta_sa_400mhz_rq` |
|---|---|---|
| copy 0 / 1 / 2 / 3 | 23.64 / 23.76 / 23.76 / 23.65 ms | **23.44 / 23.56 / 23.56 / 23.44 ms** (−0.9 %) |
| 4 EVTA busy union | 604.8 GOPS | **610.4 GOPS** |
| 25회 `total_time` | 683.3 ms | **678.6 ms** |

bias(400 MHz)보다 EVTA마다 −1.06 ms(−4.3 %). `hw_multievtax4/script/run_all.sh`의 `SYS_BIN`을 400 MHz 판으로 바꿨다.

### 7.19 Resnet50, VGG16에서도 줄었나 (2026-11-05, 측정)

같은 `_sysprof` / `_systimed` 실행 파일(11-04 빌드, bundle은 지금의 backend 옵션 그대로)을 systolic bitstream 네 개에서 차례로 돌렸다
(single EVTA, `-n 25 -r`, runs 2-25 중앙값, OMP 1). VGG16은 CIFAR-10용(32×32 입력)이다.

| EVTA 시간 (`_sysprof`) | `sa_400mhz` | `alu1_epr` | `bias` | `rq` | 합 |
|---|---|---|---|---|---|
| Resnet18 (3628 Mops) | 26.43 ms (137 GOPS) | 24.89 | 24.15 | **23.05 ms (157 GOPS)** | −12.8 % |
| Resnet50 (8174 Mops) | 125.56 ms (65 GOPS) | 120.27 | 118.58 | **113.36 ms (72 GOPS)** | −9.7 % |
| VGG16 CIFAR-10 (627 Mops) | 15.82 ms (40 GOPS) | 15.60 | 15.46 | **15.23 ms (41 GOPS)** | −3.8 % |

| 추론 한 장 (`_systimed`) | `sa_400mhz` | `rq` | EVTA 밖 (`rq`) |
|---|---|---|---|
| Resnet18 | 28.84 ms | 25.53 ms | 2.3 ms |
| Resnet50 | 160.27 ms | 147.92 ms | **33.4 ms** |
| VGG16 CIFAR-10 | 16.11 ms | 15.51 ms | 0.2 ms |

하드웨어 세 단계는 세 모델 모두 줄였지만, 다른 두 모델은 GOPS가 Resnet18의 절반 이하다. 원인은 하드웨어가 아니라 다음 셋이다.
- **Resnet50 1×1 conv (backend schedule):** 3×3 conv 16개는 178~192 GOPS(이론 204.8의 90 % 이상)인데, 1×1 conv 36개는 27~69 GOPS다.
  EVTA 113.4 ms 중 1×1이 약 87 ms다. 1×1은 GEMM 대비 LOAD가 커서 지금 schedule(tile, virtual thread)로는 LOAD가 숨지 않는다.
- **Resnet50 res2_0의 residual이 합쳐지지 않음:** 16개 중 15개는 `convolution_wo_tr_res`이고, res2_0(56×56×256, 두 입력이 모두 conv)만 CPU
  `elemadd` 17.9 ms + `relu` 9.3 ms = **27.2 ms**다(EVTA 밖 33.4 ms의 대부분). 나머지는 CPU fc 2.7 ms(2048 → 1000, EVTA fc 조건 밖),
  avgpool 0.92 ms(7×7 평균은 ALU에 나눗셈이 없어 CPU), conv0 입력 복사 1.39 ms.
- **VGG16 CIFAR-10 뒤쪽 conv (weight LOAD):** 2×2 출력의 512 → 512 conv 3개가 각 1.92 ms(9.8 GOPS), 4×4의 conv 3개가 1.0~1.95 ms(38 GOPS).
  weight 2.36 MB를 1.92 ms에 읽으니 약 1.2 GB/s로, HP 포트(333 MHz × 16 B = 5.3 GB/s)보다 훨씬 느리다. weight를 tile마다 다시 읽는지
  (`EVTA_REPLAY_LOG=2`의 DRAM 바이트) 확인해야 한다. maxpool 4개는 EVTA, 1개(`MaxPool_30`)와 fc 1개는 CPU지만 합쳐 0.2 ms다.

그래서 Resnet18에서 한 backend / runtime 일(layout 유지, fusion, replay)은 다른 모델에도 들어갔지만, Resnet18에 없던 모양(1×1이 많은 bottleneck,
두 conv의 합, 작은 공간에 큰 weight)이 새 병목이다.

**① res2_0 residual (2026-11-05, `VTASave.cpp` `findResidualFusions`, vtalib `convolution_wo_tr_res`):** 합쳐지지 않은 이유는 relu의 출력 scale(1/16)이
elementadd 출력(1/8)과 달라서였다(조건이 "같은 scale"). CPU `relu`는 이때 `int8_t v *= 2`라 64 이상이 음수로 넘어갔다(wrap).
- 이제 sO / sO' = 2^q(q = 0~7)도 합친다. vtalib `sumRelu`를 bool에서 int로: 0 없음, 1 relu, 1 + q는 relu 뒤 << q(ALU SHR에 음수 imm), MIN 127.
  q > 0이면 requantize 대신 ALU 명령으로 한다(반올림 shift와 왼쪽 shift를 한 번에 못 한다). 예전 bundle은 0 / 1을 넘기므로 그대로 돈다.
- CPU `relu`(generic, x86)는 127에서 자른다.
- 검증: `EVTA_RES_CHECK=1`로 Resnet50의 합친 호출 16개와 conv0 + pool 모두 0 differ, 분류 281 / 0.581379(전과 같음).
  Resnet18 bundle은 다시 생성해도 byte 단위로 같고, 새 vtalib으로 EVTA 23.056 ms(전 23.054).

| Resnet50 (`rq`) | 전 | 후 |
|---|---|---|
| 추론 한 장 (`_systimed`) | 147.92 ms | **122.46 ms** (−25.5 ms, −17 %) |
| EVTA 시간 (`_sysprof`) | 113.36 ms | 115.27 ms (res2_0 elemadd + relu가 EVTA로, 1.9 ms) |

**② schedule (2026-11-05, `include/Backends/VTA/VTASchedules.h`):** 표에는 Resnet18 모양만 있어서 Resnet50의 1×1 conv 36개와 3×3 일부,
VGG16의 conv 전부가 기본값(virtual thread 1, tile 14×14)으로 돌았다. virtual thread가 1이면 다음 입력 채널의 LOAD가 앞 GEMM을 기다린다.
- TVM VTA(`tvm/vta/python/vta/top/vta_conv2d.py`)는 층마다 AutoTVM으로 tile_h / w / ci / co와 virtual thread(`oc_nthread`, `h_nthread`
  1~2)를 찾고, 미리 찾은 값(TopHub log)을 쓴다. 같은 방식으로 보드에서 찾았다.
- vtalib-systolic에 `EVTA_SCHED=<H>,<W>,<C>,<KN>,<K>,<stride>:<vt>,<tileH>,<tileW>[;...]`(실행 때 schedule 바꾸기)와 `EVTA_SCHED_LOG=1`을 넣었다.
  모양마다 후보(virtual thread 1 / 2 × 출력을 나누는 tile, inp 반쪽 1024 · acc 2048 entry 안, 최대 16개)를 `_sysprof`로 재어 가장 빠른 것을 골랐다.
- 확인은 같은 실행 파일에서 기본 schedule과 새 schedule의 `EVTA_DUMP_CONV`를 모델마다 따로 비교했다(Resnet50 106개, VGG16 51개 모두 같음).
  처음 확인은 `EVTA_SCHED`가 없는 `_systimed` 실행 파일의 dump라 무효였고, 다시 해 보니 **출력이 7×7보다 작은 conv(VGG16 4×4, 2×2)에서
  virtual thread 2는 결과가 틀렸다**(vtalib이 tile을 `MIN_H_SIZE` / `MIN_W_SIZE` 7로 늘리는 경로; virtual thread 1은 어떤 tile이든 맞지만 빨라지지 않음). 그 둘은 기본값으로 뒀다.
- 거의 모든 모양에서 virtual thread 2와 큰 tile이 이겼다: 1×1 56×56은 (2, 8, 56)으로 −48~61 %, 28×28은 (2, 7, 28)로 −45 %, 14×14는 (2, 7, 14)로 −30 %, 7×7은 (2, 7, 7)로 −23 %.

| (`rq`, single, 25회 중앙값) | 전 | 후 |
|---|---|---|
| Resnet50 EVTA (`_sysprof`) | 115.27 ms (71 GOPS) | **78.08 ms (105 GOPS)** |
| Resnet50 추론 (`_systimed`) | 122.46 ms | **85.71 ms** |
| VGG16 CIFAR-10 EVTA | 15.23 ms (41 GOPS) | **13.30 ms (47 GOPS)** |
| VGG16 CIFAR-10 추론 | 15.51 ms | **13.59 ms** |
| Resnet18 | 23.05 / 25.53 ms | 그대로 (bundle byte 단위로 같음) |

분류는 Resnet50 281 / 0.581379, VGG16 0 / 0.453709로 그대로다. Resnet50 추론은 오늘 두 단계로 147.9 → 85.7 ms(−42 %)다.

**③ 출력이 7×7보다 작은 conv (2026-11-05):**
- **원인 1 — virtual thread 2가 틀린 이유:** vtalib `conv_kernel`은 출력 tile이 `MIN_H_SIZE`/`MIN_W_SIZE`(7)보다 작으면 더미 행·열을 붙여
  7×7로 계산하고, STORE(와 fused residual의 LOAD)를 행마다 나눠 낸다. 이 경로의 출력 채널 블록이 `((filterIdx + fIdx)·vt + thread)`였다.
  맞는 식은 다른 경로와 같은 `(filterIdx·vt + thread)·nFilterinLoop + fIdx`다. vt 1이고 filter loop가 한 번이면(작은 출력에서 그렇다) 두 식이 같아
  드러나지 않았다. acc 쪽 행 offset도 `fIdx·outDummyH·resizedOutTileH`를 `…TileW`로 고쳤다(H, W를 같은 7로 늘릴 때만 같다).
  `vta/vtalib`(020d041), `vtalib-zcu102-systolic` 둘 다 고쳤다.
- **원인 2 — 왜 7×7인가:** HLS GEMM(`vta.cc` `gemm`)은 `DEPENDENCE variable=acc_mem inter false`로 acc 읽기-더하기-쓰기에 hazard 검사가 없다.
  3×3 conv의 uop 순서(`gemmop_mxn`)에서는 같은 acc 주소가 `tile_h·tile_w` uop마다 다시 온다. 그 간격을 파이프라인 깊이보다 길게 하는 것이 최소 tile이다
  (1×1 conv는 명령 하나에서 주소마다 한 번이라 상관없다). Gemmini는 같은 경우 `AccumulatorMem`에서 stall하고, systolic RTL `sa_rmw.v`는
  처리 중인 dst(≤ 3)를 forwarding하므로(§ `evta_systolic.md` §5, 모델·RTL 무작위 검사) 최솟값이 필요 없다. 남는 것은 weight를 바꿀 때의
  16클럭 채우기뿐이라, 2×2 출력은 weight 블록당 49 → 16클럭, ALU는 49 → 4 entry다.
- **바꾼 것:** systolic vtalib의 최소 tile은 systolic bitstream(`/tmp/evta_bitstream`에 `_sa_`)에서 1, 아니면 7(`EVTA_MIN_TILE=<n>`로 바꿈).
  `VTASchedules.h`에 VGG16 4×4 / 2×2 conv 세 개를 vt 2로 다시 넣었다. HLS vtalib(`vta/vtalib`)은 7 그대로다.
- **검증:** 같은 실행 파일에서 기본 / vt 2 / 최소 1 / 최소 1 + vt 2의 VGG16 레이어 dump 51개 모두 같음. Resnet50(159개), Resnet18(60개)도
  최소 1과 기본이 같음. 새 bundle의 dump도 고치기 전 기본 schedule과 같고, 분류는 VGG16 0 / 0.453709, Resnet50 281 / 0.581379,
  Resnet18 281 / 0.655530 그대로다.

| (`rq`, single, 25회 중앙값) | ② 후 | ③ 후 |
|---|---|---|
| VGG16 CIFAR-10 EVTA (`_sysprof`) | 13.30 ms (47 GOPS) | **7.03 ms (89 GOPS)** |
| VGG16 CIFAR-10 추론 (`_systimed`) | 13.59 ms | **7.33 ms** |
| Resnet50 EVTA / 추론 | 78.08 / 85.71 ms | 78.04 / 85.69 ms (7×7 미만 tile 없음) |
| Resnet18 EVTA / 추론 | 23.05 / 25.53 ms | 22.99 / 25.42 ms |

VGG16 추론은 오늘 15.5 → 7.3 ms(−53 %)다. 최소 1에서 schedule을 다시 찾지는 않았다(vt 2, tile = 출력 전체만 재 봄).

**④ fc를 EVTA로 (2026-11-05):**
- Resnet50 fc(2048 → 1000, shift 7)는 chunk당 출력 블록이 4개(wgt 반쪽 512 entry / 입력 블록 128)라 "≥ 8 블록" 조건에서 빠졌다. VGG16 마지막 fc(512 → 10)는 1개다.
  이 조건도 ③과 같은 HLS GEMM acc hazard 때문이다(`gemm_fc`는 같은 acc가 g uop마다 다시 온다). TVM VTA도 batch 1의 마지막 dense는 CPU에 둔다(`graph_pack`의 `stop_pack`).
- 바꾼 것: VTASave는 크기와 상관없이 `fullyconnected_evta_packed`를 낸다. systolic vtalib `fc_evta_plan`은 systolic bitstream에서 1블록부터 받는다(`evtaSystolic`).
  plan이 맞지 않으면(HLS GEMM, 또는 다른 vtalib) packed weight로 CPU에서 같은 반올림·clip으로 계산한다(`fc_packed_cpu`, 두 vtalib).
- 검증: `EVTA_RES_CHECK` fc 3개(2048 → 1000, 512 → 512, 512 → 10) 0 differ, 분류 그대로. Resnet50 fc 2.69 ms(CPU) → 0.44 ms(EVTA),
  **추론 85.69 → 83.40 ms**, EVTA 78.04 → 78.48 ms. VGG16 7.33 → 7.28 ms.

**⑤ 검증 중 찾은 vtalib 버그 — bias entry가 thread 1을 덮음 (2026-11-05):**
- HLS bitstream에서 VGG16 confidence가 0.4247(systolic 0.4537)이었다. simulator도 0.4247이었다(같은 vtalib).
- 원인: `conv_kernel`은 thread마다 acc 절반(1024)에 tile × `nFilterinLoop` 행을 두고 bias를 그 바로 뒤(`base + T·nF`)에 LOAD한다.
  `nFilterinLoop`는 `1024 / T`까지 허용해 bias 자리를 빼지 않았다. T·nF가 절반을 꽉 채우면 thread 0의 bias LOAD가 thread 1의 첫 행
  (GEMM이 끝난 결과)을 덮는다. VGG16 conv0(vt 2, 16×32, nF 2), Conv_5 / Conv_7(16×16, nF 4)이 그렇다. dump 차이도 thread 1의 tile마다 첫 nF 픽셀에만 있었다.
  systolic rq / bias bitstream은 bias reset이 GEMM 전에 thread 1 영역을 다시 쓰므로 맞았다.
- 수정(두 vtalib): `nFilterinLoop ≤ 1024 / (T + 1)`(bias reset이 없을 때). T는 최소 tile로 늘린 뒤의 크기를 쓴다(늘리기 전 크기로 정하면 큰 KN에서 넘칠 수 있었다).
- 검증: VGG16 simulator / HLS / systolic dump 17개 모두 같고 셋 다 0.453709. HLS와 systolic의 3회 dump가 VGG16 51, Resnet50 159, Resnet18 60개 모두 같다.
  systolic 시간은 그대로다(bias reset이 있으면 계산이 같다).

**⑥ conv0 입력 복사를 conv와 겹침 (2026-11-06):**
- conv0 입력 복사(`preprocess_conv0pack`: float quantize + 7×7/2 packing, 224×224×3 → 115×112×48)는 OMP 1에서 Resnet50 1.42 ms, Resnet18 1.65 ms였다.
  보드 microbenchmark(A53 1.2 GHz, 한 코어): quantize만 0.47 ms, packing만 0.44 ms, 같은 크기 memset 0.06 ms, float 읽기 0.13 ms.
  DRAM이 아니라 NEON 명령 수가 한계다(128비트 연산이 2 cycle). offset 0이면 add 생략, `vqtbl4q` 대신 load + `vbsl`, prefetch, `stnp`를 다 해도 1.25 → 1.14 ms 정도였다.
- 그래서 줄이지 않고 숨겼다. conv tile은 행이 바깥 loop이므로 행 i의 EVTA 계산 중에 행 i+1의 입력을 CPU가 채운다(double buffering).
  - pynq driver `Run` → `RunStart` + `RunWait`(`VTADeviceRunStart/Wait`). 기존 `Run`은 둘을 이어 부르는 것이라 다른 호출은 그대로다.
  - `cmd_queue.h`: 기록한 run마다 tag(그 run이 읽는 입력 행 수, `VTASetSegmentTag`)를 두고, replay가 run s를 시작한 뒤 hook(run s+1의 tag)을 부른다(`VTASetSegmentHook`).
  - systolic vtalib `InputStream`: `preprocess_conv0pack_stream`은 인자만 기억하고, 그 buffer를 읽는 conv가 타일 행마다 run을 나눈다.
    hook은 그 행들만 packing하고 그 범위만 flush한다(`preprocess_conv0pack_rows`). 기록하는 첫 호출은 미리 다 채운다. `EVTA_INPUT_STREAM=0`이면 예전처럼 한 번에 채운다.
  - VTASave는 `preprocess_conv0pack` + `VTABufferFlush` 대신 `preprocess_conv0pack_stream(…, buffer, …)`를 낸다. vta/vtalib(HLS)은 한 번에 채우고 flush한다.
- 검증: 세 모델 모두 systolic(stream 켬·끔)·HLS의 3회 dump가 전날 기준과 같다(Resnet50 159, Resnet18 60, VGG16 51개). 분류도 그대로다.
- 결과(`_systimed`, 25회 중앙값, stream 끔 → 켬):

| | 입력 복사 + conv0 | 추론 |
|---|---|---|
| Resnet50 | 1.42 + 3.31 → 3.56 ms | 83.41 → **82.21 ms** |
| Resnet18 | 1.65 + 3.36 → 3.66 ms | 25.43 → **24.08 ms** |
| VGG16 CIFAR-10 | 0.05 + 0.11 → 0.14 ms | 7.29 → 7.27 ms |

  conv0이 늘어난 0.25~0.30 ms는 대부분 첫 run 전에 채우는 첫 행 묶음(115행 중 19행)이다. run을 7개로 나눈 비용은 거의 없다.
  CPU 일을 EVTA와 겹치는 구조(start / wait 분리, run마다 hook)는 3단계(인터럽트 + command ring)의 바탕이기도 하다.

**⑦ multi4에도 (2026-11-06):**
- Resnet18 multi4는 같은 bundle 생성기라 ⑥이 그대로 들어간다. Resnet50, VGG16 CIFAR-10도 multi4 set을 만들었다(`vta/multievta_bundles/MultiEVTAResnet50Test`,
  `MultiEVTAVgg16Cifar10Test`; copy i = EVTA i). main은 하나(`multi_threads_main.cpp`)에 모델만 `multi_model.h`로 다르고, copy 생성은
  `vtabundles_xcmpl/gen_multi4_bundles.sh`(model-compiler 직접 호출; NEST-C CMake에 디렉터리를 더하면 Debug2_remote cmake가 다시 돌아 Glow 전체 재빌드).
- CMA: copy마다 weight를 VTA buffer에 올리면 Resnet50 4 × 25.6 MB = 102 MB로 128 MB에 들어가지 않는다(단일 Resnet50도 queue 64 MB 포함 96 MB).
  load_module의 상수 buffer를 내용이 같으면 process 안에서 하나로 공유한다(vtalib `VTAConstBufferAlloc`, VTASave가 냄; `EVTA_CONST_SHARE=0` = copy마다).
  Resnet50 multi4 실행 중 CMA 여유 85 MB.
- 찾은 버그 둘:
  - 새 main의 PNG 읽기가 단일 main과 달랐다(Resnet50 main은 RGB를 뒤집지 않고 VGG16 / Resnet18 main은 뒤집음, scale은 double). `MODEL_PNG_REVERSE`로 맞춤.
  - systolic vtalib `conv_kernel`: bias reset이 있으면 bias entry 자리를 빼지 않아(⑤) 마지막 thread의 bias가 acc 절반 뒤(2048 이후)에 LOAD된다.
    단일 buffer는 물리 acc가 4096이라 우연히 괜찮았고 multi buffer(2048)는 넘쳤다(VGG16 conv0 vt 2 / 16×32, systolic multi4만 틀림, HLS multi4는 맞음).
    마지막 thread의 bias가 물리 acc(`-DVTA_LOG_ACC_BUFF_SIZE`) 밖이면 자리를 남긴다. 단일은 그대로.
- 검증: 세 모델 모두 multi4 copy 0의 dump가 단일 EVTA와 같고(Resnet50은 HLS multi4도), copy 4개 × 25회 분류가 단일과 같다(281 / 0.581379, 0 / 0.453709).
  단일 bundle의 dump·시간은 그대로.
- 결과(systolic rq multi4, 4 copy 동시, OMP 1, copy별 `_systimed` 25회 중앙값, stream 끔 → 켬):

| | 4 copy 동시 | copy 0 혼자(multi4 bitstream) |
|---|---|---|
| Resnet18 | 25.86~25.97 → **24.58~24.67 ms** | 25.52 → 24.15 ms |
| Resnet50 | 86.88~87.49 → **85.68~86.26 ms** | 84.32 → 83.09 ms |
| VGG16 CIFAR-10 | 8.18~8.51 → 8.19~8.53 ms | 7.38 → 7.37 ms |

  copy 0 혼자도 단일 bitstream보다 조금 느리다(Resnet50 82.18 ms): multi buffer가 작아 schedule이 같지 않다(표는 단일 buffer로 찾음).
  4 copy를 같이 돌리면 copy마다 Resnet50 +2.6~3.1 ms, VGG16 +0.8~1.1 ms 더 걸린다(copy 1, 2가 0, 3보다 느림).

**⑧ multi4가 느린 두 원인 (2026-11-06):**
- copy 0 혼자인데 단일 bitstream보다 느림(Resnet50 83.09 vs 82.18 ms): schedule이 아니다. 호출마다 ops가 같고, 0.9 ms가 conv마다 1~3 %씩 퍼져 있으며
  load 비중이 큰 stride 2 1×1 conv(`branch1`)에서 가장 크다(+0.06~0.10 ms). HP 포트 clock은 둘 다 333 MHz이고, multi4는 Y219 timing 때문에 HP 포트 앞에
  `axi_register_slice`가 하나 더 있다(`vta-hw-zcu102-multi4` `vivado.tcl`) → load latency. bitstream 쪽 일이다.
- 4 copy 동시일 때 느려짐: Resnet50 multi4를 EVTA 조합별로 돌렸다(`MULTI_MASK`, copy별 25회 중앙값).

| 돌린 EVTA | copy별 | 혼자 대비 |
|---|---|---|
| 하나씩 (0, 1, 2, 3) | 83.12 / 83.28 / 83.31 / 83.27 ms | |
| 0+1, 0+2, 0+3, 1+3, 2+3 | 83.95~84.22 ms | +0.85~0.95 ms |
| 1+2 | 84.56 / 84.53 ms | +1.25 ms |
| 0+1+2+3 | 85.65 / 86.25 / 86.24 / 85.79 ms | +2.5 / +2.95 ms |

  어느 쌍이든 약 +0.9 ms(DDR controller 공유), 1+2만 0.35 ms 더(HP1·HP2가 DDR port를 같이 쓰는 구조와 맞다; HP0, HP3은 따로). Resnet50 한 추론의
  EVTA DRAM 전송은 86.6 MB(weight 42.5, inp 30.5, out 12.1; `EVTA_REPLAY_LOG=2`), EVTA 하나 평균 약 1 GB/s, 4개 4 GB/s로 DDR 대역에는 한참 못 미친다:
  대역이 아니라 load 구간의 latency가 경합으로 늘어난다(compute-bound인 ResConv10은 4개 동시에도 그대로였다, `evta_multi4_609gops_report.md`).
  4 copy 처리량은 단일의 3.87배(4 × 83.1 / 85.98).
- 할 수 있는 것(하드웨어): Y219 fence가 필요 없는 보드에서 register slice 빼기, EVTA 1 또는 2를 HPC 포트(다른 DDR port)로 옮기기, HP 포트의 read outstanding 늘리기.
  소프트웨어로는 EVTA 2~3개만 쓸 때 1과 2를 같이 고르지 않기 정도다.

**남은 것:**
- Resnet50 CPU: avgpool 0.9 ms(ALU에 나눗셈 없음, 2단계 ISA). conv0 첫 행 묶음 약 0.2 ms(첫 tile 행을 더 잘게 나누면 줄어듦).
- multi4 하드웨어: ⑧의 register slice, HP1/HP2 port 공유.
  multi-EVTA bundle에도 같은 표가 쓰인다(buffer는 vtalib이 2048 / 1024 entry로 가정).

### 7.20 MobileNetV2 (2026-11-07, `vta/bundles/MobileNetV2Test`, `vtabundles_xcmpl/gen_mobilenetv2_bundle.sh`)

모델은 TFLite의 QAT `mobilenet_v2_1.0_224_quant`를 float ONNX로 바꾼 것(`tfl2onnx.py`, 1001 class, cat_285.png 정답 282)이다.
ONNX zoo의 MXNet `mobilenetv2-7`도 시험했다. power-of-two per-tensor로는 617이 나왔고 channelwise여야 281이 맞았다.
측정은 single EVTA, systolic rq bitstream(`_systimed`, `chrt`), OMP 1, 25 × 4회의 중앙값이다.

| 단계 | 추론 1회 | 분류 (확률) | 바뀐 것 |
|---|---|---|---|
| 처음 | 1048.0 ms | **293** (0.25) | CPU conv 1018.9 ms: depthwise 17개 603.6, 24채널 1×1 conv 4개 260.5, conv0 149.2, logits 5.5. EVTA conv 30개 13.4 ms |
| ① depthwise per-channel + `cw_convolution` | 488.4 ms | 282 (0.906) | depthwise 603.6 → 43.2 ms |
| ② 24채널 → 32채널 (`-vta-pad-channels`) | 234.2 ms | 282 (0.840) | 24채널 conv 4개가 EVTA로(260.5 ms → EVTA +3.8 ms) |
| ③ EVTA shift 반올림 (`-vta-round-shift`) | 234.0 ms | 282 (0.939) | 시간은 같고 정확도만 바뀜 |
| ④ conv0 비대칭 pad를 pack으로 | **84.2 ms** | 282 (0.944) | conv0 149.9 ms → EVTA 약 2.8 ms |
| (④에 `-t 4`) | 44.4 ms | 282 (0.944) | depthwise 43.0 → 11.7 ms |

quantized Interpreter(같은 profile, depthwise per-channel)는 282(0.935)다.

**① 정확도: depthwise weight의 per-tensor scale** (`lib/Quantization/Quantization.cpp` `-enable-channelwise-depthwise`):
- 보드와 quantized Interpreter가 똑같이 293이었다. bundle은 quantizer가 만든 그대로 돌고 있었다.
- onnxruntime에서 Glow 규칙(symmetric, scale은 2의 거듭제곱으로 올림)으로 fake-quantize해서 원인을 갈랐다.
  - depthwise weight만 양자화해도 293이 나왔다. pointwise만, 또는 activation만 양자화하면 282였다.
  - depthwise weight는 BN을 접은 뒤 한 filter 안에서 채널 최대값이 0.3~56으로 퍼진다. scale이 0.5 하나이면 작은 채널은 0~2단계밖에 남지 않는다.
  - depthwise만 채널별 scale로 두면(각 채널 scale도 2의 거듭제곱) 282(0.925)가 된다.
- 이 옵션은 group > 1 conv만 `ChannelwiseQuantizedConvolution`으로 바꾼다. EVTA로 가는 1×1 conv는 per-tensor 그대로다.
- VTASave `saveChannelwiseConvolutionInst`와 vtalib `cw_convolution`(CPU)이 이를 처리한다.
  - VTASave가 compile time에 채널별 bias(acc scale)와 출력 배율 `matMulScale / outScale`을 계산한다.
  - 반올림은 Interpreter와 같이 `std::round(float(sum) * m)`이다.
  - depthwise 경로는 NEON으로 한 번에 16채널씩 처리한다.
- `cw_convolution`의 시간은 C와 상관없이 16채널당 약 290 ns(A53 약 350 cycle)다. benchmark로 재현했고 g++로는 237 ns다.
  A53의 NEON은 128-bit 연산을 반 속도로 하고 int8 dot product가 없다(ARMv8.0). 탭 9개 × 8개에 마무리까지 연산이 약 100개다.
- 한때 "int8 지수 하한 −7"(`lib/Quantization/Base/Base.cpp`)을 의심했지만 원인이 아니다. 그 파일은 `NESTC_WITH_NMP`일 때만 빌드되고, 이 보드는
  Glow의 `Base.cpp`를 쓴다(하한 없음; `cmake/glow/lib/Quantization/Base/CMakeLists.txt`).

**② 24채널** (`VTA.cpp` `padChannelsTo16`, `-vta-pad-channels`, 기본 켜짐):
- 채널 수가 16의 배수가 아닌 int8 activation을 16의 배수로 넓힌다. 조건은 만드는 쪽과 쓰는 쪽이 모두 group 1 conv(상수 filter, bias)이거나
  그런 activation끼리의 Add인 것이다. MobileNetV2에서는 24채널 bottleneck이 해당된다.
- 만드는 conv의 filter 행과 bias는 0으로, 쓰는 conv의 filter 열도 0으로 채운다. 그래서 늘어난 채널은 항상 0이고 Pad / Slice 노드가 없다.
- 결과 확인: 옮긴 conv(expanded_conv_2 expand, 32→144)의 출력을 numpy로 다시 계산했다. 잘라내는 shift로 계산하면 451584개 중 0개가 다르고,
  늘어난 lane은 모두 0이다.
- 확률이 0.906 → 0.840으로 내려간 이유는 반올림이다. EVTA conv는 `acc >> shift`로 잘라내고, CPU conv와 Interpreter는 `std::round`로 반올림한다.
  project conv 출력은 scale이 1/4, 1/2로 거칠어서 −0.5 LSB 편향이 뒤 층으로 퍼진다.

**③ 반올림** (`VTASave.cpp` `convRoundHalf`, `-vta-round-shift`, 기본 꺼짐):
- compile time에 bias에 2^(shift−1)을 더해 shift가 반올림하게 한다. 실행 비용은 없다.
- fuse된 conv(residual, pool)는 shift가 add나 pool 뒤에 있어서 제외한다.
- 켜면 다른 모델의 EVTA conv 출력도 모두 바뀐다. 그래서 기본은 꺼 두고 MobileNetV2Test만 켰다.

**④ conv0** (`getConv0Pack`, vtalib `padEnd`):
- TF SAME stride 2의 pad는 위·왼쪽 0, 아래·오른쪽 1이다. conv0 pack은 pad된 행을 직접 만들므로 위·왼쪽 pad만큼 offset을 두고,
  아래·오른쪽 pad를 `padEnd`로 따로 받는다(두 vtalib).
- 이제 113×112×32 → 32, 2×1 packed conv로 EVTA에서 돈다. schedule은 기본값(1 thread, 8×8)이다.

**남은 것 (OMP 1, 84.2 ms):**
- depthwise 43.0 ms(CPU). EVTA로 옮기는 방법은 결정이 필요하다(아래).
- EVTA conv 35개 20.0 ms(30.7 GOPS). 작은 1×1 conv가 많아 load 비중이 크고, conv0와 새 모양들은 기본 schedule이다.
- logits 1×1 1280→1001 5.9 ms(CPU, 1001 % 16). transpose 8.6 ms(depthwise 앞뒤 NHWC ↔ EVTA layout), elemadd 5.3 ms.
- depthwise를 EVTA에서 돌리는 방법:
  - ISA 변경 없이: 탭마다 대각 16×16 weight block으로 GEMM을 돌린다(GEMM 효율 1/16). 채널별 shift는 ALU SHR의 acc operand(lane별 shift 값)로 한다.
  - ISA를 바꾸면: depthwise 모드나 ALU MAC을 넣는다. `npu_stack_survey.md`의 하드웨어 방식 조사를 참고.
  - 어느 쪽이든 depthwise 앞뒤의 transpose가 함께 없어진다.

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
