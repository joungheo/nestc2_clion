# VTA → ETRI EVTA → 이번 작업: 코드 비교, 그리고 output → input 경로(MOV)

원본 VTA(Apache TVM), ETRI가 만든 EVTA, 2026-09 ~ 11월 이번 작업에서 바꾼 것을 코드로 비교했다. 그리고 NEST-C 논문이 말하는 EVTA의 **output buffer → input buffer 경로(MOV)**를 지금 하드웨어에 넣을 수 있는지, 넣으면 얼마나 얻는지를 정리했다.

| 무엇 | 비교에 쓴 것 |
|---|---|
| 원본 VTA | `tvm/` submodule(TVM `1a1be09c2`, 2021-07-21). 하드웨어 `tvm/3rdparty/vta-hw`, runtime `tvm/vta/runtime` |
| ETRI EVTA | `vta/vtalib`의 마지막 ETRI commit `e9e7d29`(2022-10-12), NEST-C `lib/Backends/VTA`의 마지막 ETRI commit `661cd4fd`(2022-10-12) |
| 이번 작업 | 그 뒤 commit(작성자 joungheo)과 코드의 `//#### JO` 표시 |
| 논문 | `~/npudesignkit/lib/ref/paper/NEST-C/`: NEST-C(ETRI Journal 2024, §4.4 EVTA, Figure 4), 「확장 가능한 HLS 기반 딥러닝 가속 하드웨어 개발」(권용인 외, 2021 임베디드공학회 추계), `VTA_EVTA_생성.png` |

**ETRI의 EVTA 하드웨어 소스(HLS / RTL)는 이 저장소에 없다.** 받은 것은 비트스트림(`zcu102_1x16_i8w8a32_16_16_19_18.bit`, etri/nest-data)뿐이다. 지금의 `vta-hw-zcu102-*`는 2026-10-20에 원본 `tvm/3rdparty/vta-hw`를 복사하고, ISA 헤더만 vtalib의 것으로 바꿔 시작했다(`44b33307`). 그래서 ETRI 하드웨어가 무엇을 바꿨는지는 다음 세 가지로 추정한다. 하드웨어의 원본 대비 차이(§3.3)는 모두 이번 작업분이다.
- ISA 헤더
- ETRI runtime이 그 기능을 쓰는 방식
- 논문, 그리고 보드 probe로 확인한 사실

## 1. 한눈에

| 층 | 원본 VTA → ETRI EVTA | ETRI EVTA → 이번 작업 |
|---|---|---|
| ISA | 명령 128 → **256 bit**, uop 32 → 64 bit, `XP_GEMM`(binary GEMM), `MOVE`(SRAM 사이 이동), `gemm_from_output` | 실제 비트스트림에 맞춰 `memory_type` / `alu_opcode` 2 bit, pad 비트에 `bias_reset`, `rq_*`(requantize) |
| 하드웨어 | compute에 MOV, FP16 / FP32 / binary 연산, multi-EVTA(최대 4개가 DRAM 공유) — 논문 기준, 소스 없음 | 원본 `vta.cc`에서 다시 시작: 333 → 380 / 400 MHz, **RTL systolic GEMM**, ALU imm II = 1, bias reset, requantize, multi4 Y219 배선 |
| runtime | TVM runtime(1,768줄)을 ZCU102용으로 다시 짬: `cmd_queue.h`, `pynq_driver.cc`(libcma), multi-EVTA 핸들 | +4,176 / −297줄: record / replay, `dsb sy`, buffer pool, DMA 묶기, matmul, conv batch 등 |
| bundle 라이브러리 | **새로 만듦**: `VTABundle.cpp`(conv / fc / pool / elementwise 등 약 45개 함수), CPU 연산, ACL | conv + residual / pool / depthwise, fc packed, NEON 함수들, stream 입력, matmul_evta |
| 컴파일러 | TVM Python(graph_pack, AutoTVM) 대신 **Glow 기반 NEST-C VTA backend**(`VTA.cpp`, `VTASave.cpp`, `VTASchedules.h`) | `VTASave.cpp` 5,122 → 8,388줄: layout 유지, fusion들, conv0 pack, Transformer 지원 등 |

## 2. 원본 VTA → ETRI EVTA

### 2.1 ISA (`hw_spec.h`)

| 항목 | 원본 VTA (`hw_spec_const.h`) | ETRI EVTA (`zcu102/vta/hw_spec.h`) |
|---|---|---|
| 명령 폭 | `VTA_LOG_INS_WIDTH 7` = **128 bit** | `8` = **256 bit**. 모든 명령 끝에 `pad_1`(64) 등, 일반 명령은 `pad_2`, `pad_3`까지 |
| uop 폭 | `VTA_LOG_UOP_WIDTH 5` = 32 bit | `6` = **64 bit** (dst / src / wgt index를 넓게) |
| opcode | LOAD 0, STORE 1, GEMM 2, FINISH 3, ALU 4 (3 bit) | + **`XP_GEMM` 5**: XNOR-popcount binary GEMM(BNN). `VTAXPGemInsn` |
| GEMM 필드 | `reset_reg`, uop 범위, 두 단계 loop의 factor | + **`gemm_from_output`**(1 bit): GEMM이 입력을 inp 대신 **out buffer에서** 읽음 |
| 메모리 종류 (`memory_type`) | UOP 0, WGT 1, INP 2, ACC 3, OUT 4 (3 bit) | + **`MOVE` 5**: LOAD가 DRAM 대신 SRAM(out)에서 읽어 inp에 씀 |
| ALU opcode | MIN, MAX, ADD, SHR, MUL (3 bit) | 같음, `VTA_MUL_ARG_BIT_WIDTH 8` 추가 |
| 기타 | | `VTA_BUFFER_NTIMES`(buffer 깊이 배수), `VTA_MEMOP_PAD_VAL_BIT_WIDTH` |

**실제 비트스트림은 헤더와 달랐다(09-29, 이번 작업 초기 probe).**
- ETRI 비트스트림은 `memory_type`과 `alu_opcode`가 **2 bit**였다. 헤더대로 3 bit로 만들면 LOAD / STORE 주소가 한 bit씩 밀려 결과가 0이 되거나 멈췄다.
- 그래서 `MOVE`(5)와 `MUL`(4)은 그 비트스트림에서 인코딩할 수 없다. 이 기능들은 비트스트림에 없거나, 다른 판에만 있었던 것으로 본다.
- 지금 vtalib의 헤더 세 곳은 2 bit로 고쳐 두었다(`//#### JO`, `docs/evta_isa_ext.md` §2.1).

### 2.2 output → input 경로: ETRI가 넣은 두 가지

NEST-C 논문 §4.4:
> "It modifies VTA's compute module to include an MOV instruction, reducing power usage and improving performance by eliminating DRAM data transfers between the output and input buffers."

Figure 4에서는 compute 모듈 아래에 OUTPUT BUFFER → INPUT BUFFER로 가는 "MOV" 화살표로 그렸다. 코드에는 두 형태로 남아 있다.

| 형태 | 코드 (`vtalib e9e7d29`) | 하는 일 |
|---|---|---|
| LOAD `memory_type = MOVE` | `cmd_queue.h` `LoadBuffer2D`: MOVE면 `dram_base`에 DRAM 주소 대신 SRAM 주소(`*(uint32_t *)src`)를 넣음, 원소 크기는 inp와 같음 | out buffer의 행들을 inp buffer로 복사 |
| GEMM `gemm_from_output` | `PushGEMMOp(kernel, from_outmem)`, `PushXPGEMMOp(kernel, from_outmem)` | GEMM이 src를 out buffer에서 읽음(복사도 없이 바로) |

- **어디서 썼나:** 둘 다 BNN(binary) 경로에서만 썼다. `VTABundle.cpp`의 BNN conv 호출 네 곳(2929, 2940, 3000, 3011행)이 `from_outmem = true`로 부른다. int8 conv 경로(`convolution_wo_tr`)는 쓰지 않는다.
- **시뮬레이터:** `sim_driver_bnn.cc`의 `RunXPGEMM`이 XNOR-popcount(`popcount(~(inp ^ wgt)) * 2 − 8`)를 계산한다.
- 이번 작업에서 쓴 비트스트림(etri, 그 뒤 자체 빌드 모두)에는 이 경로가 없다.

### 2.3 하드웨어 (논문 기준)

| 항목 | 내용 | 출처 |
|---|---|---|
| 기반 | Apache TVM VTA. VTA는 PYNQ-Z1(Zynq-7020, Cortex-A9)에서 51 GOPS | `VTA_EVTA_생성.png` |
| EVTA 모듈 | LOAD′ / STORE′ / COMPUTE′ / ETC를 HLS로 추가하고, 여러 크기로 합성 | 위 그림 |
| 성능 | ZCU102에서 170–682 GOPS (EVTA 1–4개) | 위 그림 |
| MOV | compute 모듈에 out → inp 이동 | NEST-C 논문 §4.4 |
| 연산 형식 | INT8, FP16, FP32, binary | NEST-C 논문 §4.4 |
| multi-EVTA | 같은 EVTA를 자원만큼 여러 개, DRAM 공유, 실행 시 주소만 설정 | 2021 임베디드공학회 논문 그림 4 |
| multi-EVTA 결과 | ZCU102, Quad EVTA: FF 8 → 51 %, LUT 11 → 54 %, BRAM 15 → 39 %. Resnet18 11.4 → 22.9장/s | 같은 논문 표 2, 3 |
| NEST-C의 EVTA 결과 | 333 MHz EVTA 4개. Resnet18: CPU 1,262 ms → CPU + NPU 247 ms → CPU + 2NPU 125 ms. 양자화와 layout 변환이 CPU에 남아 NPU를 늘려도 덜 줄었다 | NEST-C 논문 표 2 |
| ISA 확장 논문 | 「HLS 기반 딥러닝 가속 하드웨어의 ISA 확장을 통한 성능 향상」(권용인 외, 2020) | 2021 논문 참고문헌 [1], 이 폴더에는 없음 |

### 2.4 runtime과 bundle 라이브러리

- **runtime API:** TVM `vta/runtime`의 API(`VTABufferAlloc`, `VTALoadBuffer2D`, `VTAPushGEMMOp`, `VTASynchronize` 등)를 그대로 두었다.
  - 더한 것: `VTABufferGetVirtAddr`, `VTAPushXPGEMMOp`, `VTAUopBufferReset`
  - 구현은 ZCU102용 헤더로 다시 짰다: `include/zcu102/cmd_queue.h`, `dat_queue.h`, `uop_kernel.h`, `lib/zcu102/pynq_driver.cc`(libcma, `/dev/mem`), `api.cc`
  - multi-EVTA: 함수마다 EVTA 번호 `idEVTA`(86곳)를 받고, EVTA마다 명령 핸들을 둔다.
- **bundle 라이브러리(`lib/Bundle/VTABundle.cpp`, 원본에 없음):** Glow가 만든 bundle이 연산마다 부르는 함수다. 원본 TVM은 연산마다 코드를 생성해 runtime을 직접 부른다.
  - 함수: `convolution_wo_tr(_scale, _wo_ch)`, `xp_convolution_wo_tr`, `convolution`, `cpuvtaconvolution`, `nonvtaconvolution(_f32)`, `bnn_nonvtaconvolution_scale`, `fullyconnected`, `maxpool`, `avgpool`, `elemadd(_relu, _scale)`, `elemsub`, `elemdiv`, `elemmax`, `elemsign`, `relu`, `softmax`, `quantize`, `dequantize`, `transpose(_6dim)`, `transpose_nhwc2vtaio(_pack, _zerofill)`, `transpose_vtaio2nhwc`, `splat`, `typecast` 등
  - conv는 `conv_kernel`이 tile, 입력 채널, 가상 스레드마다 LOAD / GEMM / ALU / STORE 명령을 실행할 때 만든다(JIT).
- **CPU 연산:** `CPUBundle_generic.cpp` / `CPUBundle_aarch64.cpp`(ARM Compute Library) / `CPUBundle_x86.cpp`
- **시뮬레이터:** `lib/simulator`(`sim_driver.cc`, BNN을 포함한 `sim_driver_bnn.cc`)

### 2.5 컴파일러

| | 원본 VTA | ETRI EVTA |
|---|---|---|
| 틀 | TVM(Relay → TE schedule → TIR) | Glow 기반 NEST-C |
| 변환 | `graph_pack`이 NCHW를 NCHWnc로 묶고, 구간 입구 / 출구에서만 layout을 바꿈 | 6차원 layout(4차원 입력의 마지막 두 축을 EVTA가 한 번에 처리하는 행렬 크기로)을 graph IR로 표현, CPU / multi-EVTA 분할(`NestPartitioner`, PartitionTuner) |
| 코드 생성 | conv마다 TIR → runtime 호출 코드, uop는 컴파일 때 | `VTASave.cpp`가 `VTABundle.cpp` 함수를 부르는 C++ bundle을 만듦, uop는 실행 때(JIT) |
| tile 선택 | AutoTVM(TopHub log) | 보드 profiling + `VTASchedules.h` 표(논문: "profiling and auto-tuning") |

## 3. ETRI EVTA → 이번 작업 (2026-09-29 ~ 11-10)

### 3.1 양

| 부분 | 바뀐 양 |
|---|---|
| `vta/vtalib` (`e9e7d29..HEAD`) | 파일 19개, +4,176 / −297줄 |
| `vtalib-zcu102-systolic` | 위에 더해 `VTABundle.cpp`만 +610 / −89줄 |
| NEST-C VTA backend (`661cd4fd..HEAD`) | +3,809 / −167줄 (`VTASave.cpp` 5,122 → 8,388줄) |
| 하드웨어 `vta.cc` (원본 TVM 대비) | single 56줄, systolic / multi4 363줄 + 새 RTL 1,008줄(`gemm_sa.v`, `sa_array.v`, `sa_pe.v`, `sa_ctrl.v`, `sa_rmw.v`) |

### 3.2 ISA

| 비트 | 필드 | 뜻 |
|---|---|---|
| (LOAD / STORE, ALU) | `memory_type`, `alu_opcode` 3 → 2 bit | 실제 비트스트림에 맞춤 |
| GEMM 140 | `bias_reset` | GEMM 시작값을 0 대신 bias로 |
| GEMM 141–164, ALU 144–167 | `rq_en`, `rq_round`, `rq_shift`, `rq_lo`, `rq_hi` | out에 쓸 때 clip((acc + round) >> shift) |

모두 원래의 pad 비트 자리에 넣었다. 기능이 없는 비트스트림은 그 비트를 읽지 않는다. vtalib은 비트스트림 이름(`bias`, `_rq`)을 보고 이 기능을 켠다(`docs/evta_isa_ext.md`).

### 3.3 하드웨어 (원본 `tvm/3rdparty/vta-hw` 대비)

| 바꾼 것 | 파일 | 문서 |
|---|---|---|
| ZCU102 target, 256-bit 명령 헤더, HP 포트 분리, PLL, export 스크립트 | `config/`, `scripts/vivado.tcl`, `hls.tcl`, `bit2bin.py` | `vta_synth_notes.md` |
| compute 카운터, AXI perf monitor(`PROFILE=1`) | `vta.cc` (single의 56줄 대부분) | 같음 |
| **GEMM을 RTL systolic(weight-stationary 16×16 DSP48E2) 어레이로** | `rtl/gemm_sa.v` 등, `vta.cc` `VTA_SYSTOLIC` | `evta_systolic_design.md` |
| ALU imm 연산 II = 1, acc / out 쓰기 앞 register, RESET_MEM 파이프라인 | `vta.cc` `alu()` | `evta_runtime_compare.md` §7.15 |
| bias reset, requantize | `vta.cc`, `sa_ctrl.v`, `sa_rmw.v` | §7.16, §7.18, `evta_isa_ext.md` |
| multi2 / multi4 (EVTA i 주소, HP 포트), 결함 행 Y219를 피하는 배선(RapidWright) | `vta-hw-zcu102-multi*` | `vta_synth_notes.md` §3 |

### 3.4 runtime, bundle, 컴파일러

자세한 단계와 효과는 모델별 문서에 있다: `resnet18_optimization.md`, `resnet50_optimization.md`, `vgg16_optimization.md`, `mobilenetv2_optimization.md`, `deit_optimization.md`. 요약:
- **runtime:** record / replay(AOT 0단계), `dsb sy`, buffer pool, `RunStart` / `RunWait`와 run hook, conv batch, `VTABufferView`, DMA 묶기, 상수 buffer 공유, 비트스트림 검사
- **bundle:** conv + residual / conv0 + pool / depthwise / maxpool / fc를 EVTA에서, NEON CPU 연산, conv0 pack, matmul_evta
- **컴파일러:** 층 사이 EVTA layout 유지, fusion들, 전처리 표, 24 → 32 채널, 반올림 shift, Transformer(MatMul → conv, LUT, LayerNorm / softmax 함수) 등

## 4. output → input 경로(MOV)를 지금 하드웨어에 넣을 수 있나

### 4.1 얼마나 얻을 수 있나: 측정한 상한

2026-11-10, 지금 하드웨어(systolic 400 MHz `_rq`)와 지금 vtalib(DMA 묶기 포함)으로 쟀다. `EVTA_MEMEXP`로 replay 사본의 inp LOAD(bit 0)와 STORE(bit 3)를 원소 하나로 줄이면, 층 사이 activation이 DRAM을 오가지 않을 때의 EVTA 시간이 나온다(결과는 틀어지는 실험). 값은 `_sysprof` 5장 중앙값이다.

| 모델 | 그대로 | inp LOAD 없음 | STORE 없음 | 둘 다 없음 | 상한 (줄어드는 시간) |
|---|---|---|---|---|---|
| Resnet18 | 22.24 ms | 21.31 | 21.77 | 20.83 | **−1.41 ms (6.3 %)** |
| Resnet50 | 66.08 ms | 59.65 | 64.04 | 57.64 | **−8.44 ms (12.8 %)** |
| VGG16 (CIFAR-10) | 6.78 ms | 6.58 | 6.66 | 6.46 | **−0.32 ms (4.7 %)** |
| MobileNetV2 | 15.86 ms | 13.17 | 13.93 | 11.22 | **−4.64 ms (29 %)** |
| DeiT-tiny | 19.78 ms | 19.67 | 17.92 | 17.81 | **−1.98 ms (10 %)**, 실제로는 거의 0 (§4.3) |

**이 값은 상한이다. MOV로 실제로 줄일 수 있는 것은 이보다 작다.**
- **빼기 어려운 전송까지 들어 있다.** 네트워크의 첫 입력과 마지막 출력, 그리고 3×3 conv의 halo를 다시 읽는 몫이다.
- **out buffer 크기의 제약:** MOV를 쓰려면 다음 층 tile에 필요한 **모든 입력 채널 블록**이 out buffer(4096 entry)에 같이 있어야 한다. 지금 conv는 출력 블록 묶음(nFilterinLoop)마다 STORE하므로, 이를 바꾸는 스케줄이 함께 필요하다.
- **앞선 측정과의 비교:** 11-02 측정(Resnet18, DMA 묶기 이전)에서 conv → conv 사이만 보면 최대 1.05 ms였다(`evta_runtime_compare.md` §7.11).
- **MobileNetV2:** 큰 몫이 depthwise conv의 작은 DMA다. depthwise는 halo가 필요해 MOV로 줄이기 어렵다. 같은 몫은 depthwise의 DMA 묶기(소프트웨어)로 줄이는 편이 쉽다(`mobilenetv2_optimization.md` §5).
- **DeiT:** Linear 사이에 LayerNorm / softmax / GELU가 CPU에서 돌아 출력이 어차피 DRAM으로 가야 한다. 이 연산들을 EVTA로 옮긴 뒤에야(`deit_optimization.md` §6.2 H2, H4) MOV를 쓸 자리가 생긴다.

### 4.2 넣는다면 어떻게

지금 하드웨어에서 버퍼는 이렇게 연결돼 있다(`vta-hw-zcu102-systolic`).
- `inp_mem`: block design의 BRAM 하나. 포트 A는 load(쓰기), 포트 B는 compute(읽기)
- `out_mem`: 포트 A는 compute / RTL `sa_rmw`(쓰기, `gemm_busy`로 mux), 포트 B는 store(읽기)
- `acc_mem`: compute 밖의 BRAM, RMW와 ALU가 씀

| 방법 | 내용 | 장점 | 어려운 점 |
|---|---|---|---|
| **A. compute의 MOV 명령 (논문 방식, 추천)** | 새 opcode(3 bit opcode의 빈 값 6 또는 7)를 compute가 처리. ALU처럼 uop 범위와 두 단계 loop로 out[i] → inp[j]를 한 cycle에 한 행(16 byte)씩 복사 | compute는 이미 out(포트 A)과 inp(포트 B)에 닿아 있다. GEMM 뒤에 순서대로 실행되고(GEMM은 ALU처럼 drain한 뒤), inp를 load와 나눠 쓰는 순서는 기존 load ↔ compute 토큰으로 지킨다. LOAD / STORE 명령 형식은 그대로 | `inp_mem` 포트 B를 읽기 / 쓰기로(지금 compute는 읽기만), out 포트 A에서 읽기(systolic에서는 RMW와 mux), 400 MHz timing |
| B. GEMM이 out에서 바로 읽기 (`gemm_from_output`) | GEMM의 src를 out_mem으로 | 복사가 없다 | systolic `gemm_sa`가 out을 읽을 포트가 없다(두 포트 모두 사용 중). 같은 tile을 다시 읽는 GEMM이 out을 덮어쓰면 안 됨 |
| C. LOAD `memory_type = MOVE` (ETRI 코드 방식) | load가 out에서 읽어 inp에 씀 | runtime 코드가 이미 있다 | `memory_type`을 3 bit로 넓히면 LOAD / STORE 비트 배치가 다 바뀐다(모든 비트스트림 · vtalib 짝 변경). load는 out에 닿지 않고, store → load 토큰이 없다 |

**A의 구현 범위 (추정):**
- HLS `vta.cc` `compute()`: MOV 분기와 루프(II = 1), `inp_mem` 포트 B 쓰기
- block design에서 `inp_mem` 포트 B의 write enable 연결
- `hw_spec.h`에 opcode 6과 명령 구조체(ALU 형식 재사용)
- vtalib `VTAPushMOVOp`, `conv_kernel`에서 다음 층 입력을 inp에 두는 경로
- 컴파일러: 층을 이어서 같은 EVTA 호출 안에 둘 수 있는 곳 찾기
- 검증: C 모델 → CSim → 보드, dump 비교

HLS와 vtalib 부분은 bias reset / requantize와 비슷하게 며칠 규모다. 컴파일러 쪽(tile 단위로 층을 잇는 스케줄)이 더 크다.

### 4.3 어디에 쓸 수 있나

MOV는 "다음 층이 쓸 입력 tile 전체가 이번 층의 출력으로 SRAM에 이미 있을 때"만 쓸 수 있다.

| 경우 | 쓸 수 있나 |
|---|---|
| 1×1 conv → 1×1 conv, tile이 같음 | ○ 바로 쓸 수 있음 |
| 3×3 conv 앞 | △ 이웃 tile의 halo가 필요하다. halo 재계산이나 tile 단위 층 fusion(스케줄러)이 있어야 함 |
| 층 사이에 CPU 연산(LayerNorm, softmax, GELU)이 있음 | ✗ CPU가 DRAM에서 읽어야 한다. DeiT의 Linear 사이가 모두 이 경우 |
| attention Q·Kᵀ, A·V | △ K / V는 weight 쪽(wgt_mem)으로 가야 해서 out → wgt 경로가 따로 필요 |
| stage 1–2 큰 feature map | ✗ inp 64 KB에 한 층 입력 전체가 들어가지 않음 |

### 4.4 판단

- **넣을 수 있다.** 논문과 같은 방법 A(compute의 MOV 명령)가 지금 구조에 가장 잘 맞는다.
  - compute는 이미 두 버퍼에 닿아 있다.
  - opcode의 빈 값을 쓰면 LOAD / STORE 비트 배치를 바꾸지 않아도 된다.
- **효과는 모델에 따라 다르다. 지금 바로 할 만큼 크지는 않다.**
  - 상한은 Resnet18 6 %, Resnet50 13 %, VGG16 5 %다.
  - DAE 구조가 LOAD / STORE를 이미 GEMM 뒤에 숨기고 있고, DMA 묶기(§7.29) 뒤에는 드러난 전송이 더 줄었다.
  - 하드웨어보다 큰 일은 컴파일러다. 층을 tile 단위로 이어 "다음 층 입력이 SRAM에 남는" 스케줄을 만들어야 효과가 난다.
- **가치가 커지는 때:**
  1. **배열을 키울 때(32×32):** 계산이 4배 빨라지면 같은 전송의 비중이 커진다.
  2. **EVTA 4개가 DDR을 같이 쓸 때:** 지금 copy마다 약 1 ms의 경합이 있다(`resnet50_optimization.md` §4).
  3. **비선형 연산을 EVTA로 옮긴 뒤 Transformer에서:** Linear → LayerNorm → Linear가 모두 EVTA에서 이어질 때
  4. **전력:** 논문이 든 이유다. Resnet18 한 장의 activation DRAM 전송(약 8 MB)이 줄어든다. 측정하지는 않았다.
- **추천 순서** (`unified_runtime_plan.md` §4.4, `deit_optimization.md` §6.3에 맞춤):
  1. **ISA 개정 때 MOV용 opcode 하나를 예약**하고 명령 형식을 정한다. ALU 형식을 재사용한다. 비트스트림과 vtalib의 짝을 한 번만 바꾸려는 것이다.
  2. **소프트웨어로 먼저 줄인다.** 같은 전송 몫을 depthwise DMA 묶기, schedule 재탐색으로 줄인다.
  3. **배열을 키우거나 tile 단위 층 fusion(AOT 1단계와 함께)을 할 때** compute의 MOV를 구현한다.
     - 대상은 1×1 → 1×1과, out buffer에 한 tile의 모든 출력 채널이 들어가는 작은 feature map(Resnet50 stage 3–4)부터다.
     - 결과는 지금과 같이 `EVTA_DUMP_CONV`로 byte 단위 비교한다.
