# Resnet18 실행 시간을 줄여 온 과정 (2026-10-23 ~ 11-10)

ZCU102 위 EVTA(VTA 계열 int8 NPU)와 NEST-C로 Resnet18 1장을 줄인 과정을 정리했다. 시작은 134.8 ms(HLS GEMM 333 MHz, conv0은 CPU float)였고, 지금은 **23.3 ms**(systolic 400 MHz, 1코어)다. 단계마다 왜 바꿨는지, 어떻게 바꿨는지, 얼마나 줄었는지, 어떻게 검증했는지를 적었다.

- **자세한 기록:** `docs/evta_runtime_compare.md` §3, §7–§7.19, §7.29
- **날짜순 요약과 그림:** `docs/evta_overview.md`
- **AOT와 replay:** `docs/evta_runtime_aot.md`
- **하드웨어 확장 필드:** `docs/evta_isa_ext.md`
- **systolic 설계:** `docs/evta_systolic_design.md`, `docs/evta_systolic.md`

## 0. 한눈에

| 날짜 | 단계 | 1장 OMP 1 | 1장 OMP 4 | 바꾼 곳 | § (`evta_runtime_compare.md`) |
|---|---|---|---|---|---|
| 10-23 ~ 26 | 시작: HLS GEMM 333 MHz, conv0 CPU float | | 134.8 ms | – | `evta_runtime_aot.md` §3 |
| 10-28 | conv0 CPU (10-28 측정; 클럭 380 MHz / systolic 400 MHz 작업과 같은 시기) | | 131.8 ms | 비트스트림 | overview §3 |
| 10-28 | conv0을 16채널로 0 채워 EVTA로 | | 78.6 ms | 컴파일러 | overview §3 |
| 10-29 | conv0 schedule (vt 2, 8×16) | 148.7 ms | 73.6 ms | schedule | §3 |
| 10-31 | ① 층 사이 EVTA layout 유지 | 91.1 ms | 56.4 ms | 컴파일러 | §7.1 |
| 10-31 | ② conv + elemadd + relu 한 호출 | 67.1 ms | 50.2 ms | 컴파일러 + vtalib | §7.2 |
| 10-31 | maxpool EVTA | 52.5 ms | 48.7 ms | 컴파일러 + vtalib | §7.3 |
| 11-01 | transpose 16 byte 복사 | 46.9 ms | 41.8 ms | vtalib CPU | §7.4 |
| 11-01 | VTA buffer pool | 44.3 ms | 39.1 ms | vtalib runtime | §7.5 |
| 11-01 | CPU fc NEON | 40.6 ms | 38.7 ms | vtalib CPU | §7.6 |
| 11-02 | fc EVTA | 39.9 ms | 38.0 ms | 컴파일러 + vtalib | §7.8 |
| 11-02 | 입력 전처리 합치기 | 37.1 ms | 36.0 ms | 컴파일러 + vtalib | §7.9 |
| 11-02 | 명령 record / replay | 34.5 ms | 33.5 ms | vtalib runtime | §7.10 |
| 11-02 | conv0 packed 입력 (4×1 conv) | 29.2 ms | 28.0 ms | 컴파일러 + vtalib | §7.12 |
| 11-02 | conv0 + maxpool 한 호출 | 28.83 ms | 27.71 ms | 컴파일러 + vtalib | §7.13 |
| 11-03 | ALU imm II = 1 | 28.98 → 27.32 ms | | **하드웨어** | §7.15 |
| 11-04 | RMW reset에 bias | 27.30 → 26.59 ms | | **하드웨어** + vtalib | §7.16 |
| 11-04 | requantize를 out 쓰기에 | 26.61 → 25.49 ms | | **하드웨어(ISA)** + vtalib | §7.18 |
| 11-05 | systolic 최소 tile 1 | 25.53 → 25.42 ms | | vtalib | §7.19 ③ |
| 11-06 | conv0 입력 복사를 conv와 겹침 | 25.43 → 24.08 ms | | vtalib runtime + 컴파일러 | §7.19 ⑥ |
| 11-10 | conv DMA 묶기 (`EVTA_CONV_BURST`) | 24.08 → **23.27 ms** | | vtalib | §7.29 |

- **줄어든 폭:** 1코어 기준 148.7 → 23.3 ms로 **6.4배**다(conv0 EVTA 이후 기준). 시작점(134.8 ms, OMP 4)과 비교하면 약 5.8배다.
- **모든 단계의 검증:** 결과가 이전과 같다. EVTA 출력 dump(`EVTA_DUMP_CONV`)가 byte 단위로 같거나, 실행 중 CPU 경로와 비교(`EVTA_RES_CHECK=1`)해 0 불일치이고, golden(single 30, multi2 3, multi4 3)을 통과했다. 분류는 처음부터 281이고, 확률 0.655530도 그대로다.
- **"a → b" 표기:** 같은 날 이전 비트스트림이나 이전 vtalib과 번갈아 잰 값이다. 날짜가 다르면 ±0.2 ms 정도 흔들린다.

**하한:** Resnet18 연산량은 3,628 Mops다. 16×16 @ 400 MHz(204.8 GOPS)에서 100 % 가동이면 17.7 ms다. 지금 EVTA 호출 합이 22.65 ms이니 약 160 GOPS, 하한의 78 %다.

## 1. 측정 조건

| 항목 | 내용 |
|---|---|
| 모델 | `mxnet_exported_resnet18.onnx`, 224×224, int8, power-of-2 scale |
| 번들 | `Debug2_remote`의 model-compiler → `vtabundles_xcmpl/hw/Resnet18Test` |
| 하드웨어 | ZCU102(XCZU9EG), EVTA 1개(single) 또는 4개(multi4). 10-29부터 systolic 400 MHz 비트스트림으로 잼 |
| 측정 | `run_prof.sh -s _systimed`(문장별 wall time), `-s _sysprof`(EVTA 바쁜 시간), `-r`(`chrt -f 10`), 25장 중 runs 2–25 중앙값 |
| OMP | 10-31부터 기본 1코어(multi4의 copy 하나와 같은 조건). OMP 4는 `-t 4` |

## 2. 출발점: 시간이 어디에 쓰였나

### 2.1 10-26 / 27 (HLS GEMM 333 MHz, conv0 CPU float, OMP 4)

| 항목 | 시간 | 비율 | 비고 |
|---|---:|---:|---|
| conv0 (`convolutionFloat`, CPU) | 76.65 ms | 56.9 % | 첫 conv(입력 3채널)는 EVTA로 안 감 |
| CPU 연산 (elemadd 4.63, maxpool 3.29, quantize 2.94, relu 2.51, fc 1.21, 기타 1.30) | 15.88 ms | 11.8 % | |
| transpose (`nhwc2vtaio` 4.70 + `vtaio2nhwc` 7.60) | 12.31 ms | 9.1 % | conv마다 두 번 |
| `VTABufferAlloc` / `Free` (각 38회) | 3.61 ms | 2.7 % | conv마다 CMA 할당 |
| **EVTA 하드웨어 시간 (19회)** | 24.16 ms | 17.9 % | |
| 명령 생성 + 실행 시작 | 1.62 ms | 1.2 % | JIT 비용 |
| 합계 / 측정한 추론 | 134.2 / 134.8 ms | | |

### 2.2 10-28 ~ 29 하드웨어 클럭과 conv0

| 날짜 | 바꾼 것 | 왜 | 결과 |
|---|---|---|---|
| 10-27 ~ 28 | HP 포트를 333 MHz로 분리하고 EVTA를 380 MHz로(HLS GEMM). GEMM을 RTL systolic 어레이(`gemm_sa`, weight-stationary 16×16 DSP48E2)로 바꿔 400 MHz | 333 MHz의 critical path가 GEMM의 inp broadcast(fanout 346)와 덧셈 트리였다. systolic의 skew shift register가 그 경로를 끊는다 | single 약 +18 % (`evta_systolic.md`) |
| 10-28 | conv0(7×7/2, 3채널)을 16채널로 0 채워 EVTA로(`ConvPath::FirstBNN`, `transpose_nhwc2vtaio_zerofill`) | CPU float conv0이 76.65 ms로 전체의 57 % | 131.8 → **78.6 ms** (OMP 4) |
| 10-29 | conv0 schedule을 vt 2, tile 8×16으로(`VTASchedules.h`) | 기본 1 / 8×8에서 tile 수가 많음 | conv0 −26 %, **73.6 ms** |

### 2.3 10-29 분석 (systolic 400, `-r`, OMP 4 73.6 ms / OMP 1 148.9 ms)

| 호출 | OMP 4 | OMP 1 | 어디서 | 비고 |
|---|---|---|---|---|
| `convolution_wo_tr` | **32.1 ms** | 32.2 ms | EVTA + CPU | EVTA 29.7 ms + 명령 생성 2.5 ms |
| `transpose_vtaio2nhwc` | 11.0 | 41.8 | CPU | conv 출력을 NHWC로 |
| `transpose_nhwc2vtaio` | 4.8 | 17.8 | CPU | 다음 conv 입력을 EVTA layout으로 |
| `transpose_nhwc2vtaio_zerofill` | 6.4 | 6.5 | CPU | conv0 입력 3 → 16채널, OpenMP 없음 |
| `elemadd` | 4.7 | 16.1 | CPU | residual add 8개 |
| `maxpool` | 3.3 | 12.8 | CPU | 112×112×64 |
| `relu` | 2.5 | 8.6 | CPU | |
| `VTABufferAlloc` / `Free` / `Flush` | 5.0 | 4.5 | CPU | conv마다 CMA 할당·해제 |
| `fullyconnected` | 1.2 | 4.4 | CPU | |
| quantize, elemsub, elemdiv (전처리) | 2.2 | 3.7 | CPU | |

EVTA 쪽만 본 conv별 시간(`_sysprof`):

| conv | EVTA 시간 | GOPS (최대 204.8) | 무엇에 묶였나 |
|---|---|---|---|
| stage4 3×3 512→512 (7×7) | 1.22 ms | 190 (93 %) | GEMM. DAE가 LOAD를 다 숨김 |
| stage1 3×3 64→64 (56×56) | 1.50 ms | 155 (76 %) | GEMM + tile 경계 |
| stage2–4 1×1 downsample | 0.26–0.33 ms | 39–49 | conv마다의 고정 비용 |
| conv0 7×7 (3→16 0 채움) | 9.13 ms | 실효 26 | 0 채움으로 연산 13/16이 버려짐 |

**결론(10-29):**
- EVTA가 실제로 계산하는 시간은 OMP 4에서 40 %, OMP 1에서 20 %뿐이다.
- 나머지는 runtime과 backend가 만든 CPU 일이다: 층마다 transpose, CPU elemadd / relu / maxpool, 층마다 버퍼 할당, 층마다 명령 생성.
- DAE 구조(fetch / load / compute / store가 token으로 맞물림) 자체는 손해가 없다. 큰 3×3 conv에서 93 %가 나온다.
- 손해는 "conv 하나 = 함수 하나 = 동기화 한 번 = layout 왕복 한 번"이라는 runtime의 단위에서 난다.
- 그래서 GEMM을 32×32로 키우는 일보다 CPU 일을 먼저 없애기로 했다. 32×32를 먼저 하면 73.6 → 약 56 ms(−24 %)에 그치고, CPU 일을 없앤 뒤에 하면 36 → 약 19 ms(−47 %)가 된다(`evta_runtime_compare.md` §7 예상표).

| 순서 | 할 일 | 줄어드는 것 | 예상 추론 (OMP 4) |
|---|---|---|---|
| ① | 층 사이 EVTA layout 유지 | transpose 15.8 + 버퍼 약 4.5 | 약 53 ms |
| ② | elemadd, relu, maxpool을 EVTA ALU로 | CPU 10.4 ms | 약 44 ms |
| ③ | 명령 AOT + 구간 제출 | 명령 생성 2.5 ms 등 | 약 41 ms |
| ④ | conv0 zerofill / 변환 개선 | 약 5 ms | 약 36 ms |
| ⑤ | GEMM 32×32 | EVTA 29.7 → 약 12 ms | 약 19 ms |

## 3. CPU 일을 없애고 EVTA로 옮기기 (10-31 ~ 11-02, 하드웨어 변경 없음)

### 3.1 ① 층 사이 EVTA layout 유지 (§7.1, `-vta-keep-layout`)

- **왜:** conv 출력은 EVTA layout N{C/16}HW16으로 나오고 다음 conv도 이 layout을 읽는다. 그런데 bundle은 conv마다 NHWC로 바꿨다가(`vtaio2nhwc`) 다시 되돌렸다(`nhwc2vtaio`). 둘을 합치면 항등 변환이었다. 채널 수가 64 → 128로 바뀌어도 16채널 블록 수만 다르고, H / W 변화도 layout 안에 있다.
- **어떻게:**
  - `VTASave.cpp` `findPackedActivations`가 activation(4-D, N = 1, int8, C % 16 = 0, H·W > 1) 가운데 쓰는 명령이 모두 EVTA conv / elemadd / relu / maxpool / avgpool인 것을 찾는다. 그런 activation은 EVTA layout으로 둔다.
  - 이 activation은 처음부터 VTA 버퍼(`<name>_vbuf`)로 잡는다.
  - CPU 연산은 layout을 몰라도 된다. elemadd / relu는 원소별이라 그대로 쓰고, pool은 N{C/16}HW16을 "배치 N·C/16, 채널 16인 NHWC"로 보고 호출한다.
  - conv 뒤에는 invalidate, CPU 연산 뒤에는 flush를 한다. 그래프 입출력과 다른 연산이 닿는 activation은 NHWC 그대로라 다른 모델에서도 안전하다.

| 경우 | 이전 | ① 후 | 변화 |
|---|---|---|---|
| single, OMP 4 | 73.6 ms | **56.4 ms** | −23 % |
| single, OMP 1 | 148.7 ms | **91.1 ms** | −39 % |
| multi4 copy 하나, OMP 1 | 153–160 ms | **94.9 ms** | −39 % |
| multi4 4 copy 100회 | 3,909 ms | **2,793 ms** | −29 % |

### 3.2 ② conv + elemadd + relu를 EVTA 호출 하나로 (§7.2, `-vta-fuse-residual`, vtalib `convolution_wo_tr_res`)

- **왜:** residual block의 `relu(conv(x) + residual)`에서 elemadd와 relu가 CPU로 갔다. OMP 1에서 23.8 ms였다.
- **어떻게:** conv는 store 전에 acc 안에서 ALU로 bias / shift / clip을 한다. 그 자리에 다음 단계를 붙였다.
  1. conv requant까지 그대로 한다.
  2. `acc <<= convShl`(ALU ADD acc, acc)
  3. residual tile을 inp SRAM에 출력 tile과 같은 배치로 LOAD한다.
  4. `acc += residual × (2^resShl·I)`: 마지막 wgt entry에 둔 **단위행렬 weight로 GEMM**한다.
  5. `(acc + 2^(m−1)) >> m`, `MAX 0`, `MIN 127` 뒤 STORE한다.
  - scale이 2의 거듭제곱이라 곱셈이 필요 없다. 쓰는 opcode는 MIN, MAX, ADD, SHR(2 bit로 됨)뿐이라 하드웨어는 그대로다.
  - **반올림의 차이:** EVTA는 .5를 올리고, CPU `std::round`는 0에서 먼 쪽으로 보낸다. 음수 합에서만 다르고 relu가 그 값을 0으로 만들므로, relu가 붙은 elemadd만 합친다.
- **조건(`findResidualFusions`):**
  - conv 출력을 그 elemadd만 쓴다.
  - 피연산자가 모두 EVTA layout이다.
  - scale 비가 2^k이다.
  - conv와 relu 사이에는 alloc / dealloc만 있다.
- **검증:** `EVTA_RES_CHECK=1`로 fused 출력을 일반 conv + CPU elemadd / relu와 원소 단위로 비교했다. single과 multi4 모두 0 불일치였다.

| 경우 | ①만 | ② 후 | 변화 |
|---|---|---|---|
| single, OMP 1 | 90.93 ms | **67.13 ms** | −26.2 % |
| single, OMP 4 | 56.64 ms | **50.23 ms** | −11.3 % |
| EVTA 하드웨어 시간 | 29.68 ms | 30.80 ms | +1.1 ms (residual LOAD, 단위 GEMM, ALU) |
| multi4 4 copy 100회 | 2,470 ms | **1,851 ms** | −25 % |

### 3.3 maxpool을 EVTA로 (§7.3, `-vta-evta-maxpool`, vtalib `maxpool_evta`)

- **왜:** conv0 뒤 maxpool(112×112×64, 3×3/2)이 CPU에서 OMP 1 약 15 ms였다.
- **어떻게:**
  - 출력을 16채널 블록 하나의 oh × ow tile로 나눈다(Resnet18은 3행 × 56열).
  - tile마다 입력 창(halo 포함)을 LOAD하고, 단위행렬 GEMM으로 acc에 옮긴다. 출력 자리를 0으로 두고, 창의 K×K 위치마다 ALU MAX 명령을 하나씩 낸 뒤 STORE한다.
  - tile 두 개를 inp / acc의 두 반쪽에서 동시에 처리해 load / compute / store가 겹친다.
  - **이미지 가장자리:** LOAD의 padding 0으로 채운다. 그래서 입력이 0 이상일 때만 CPU와 같다. 입력을 마지막으로 쓴 명령이 relu가 붙은 EVTA conv나 relu일 때만 EVTA로 보낸다.
- **검증:** `EVTA_RES_CHECK=1`로 CPU maxpool과 비교해 0 불일치였다.

| 경우 | ② 후 | + EVTA maxpool |
|---|---|---|
| single, OMP 1 | 67.13 ms | **52.48 ms** |
| single, OMP 4 | 50.23 ms | **48.71 ms** |
| multi4 4 copy 100회 | 1,851 ms | **1,471 ms** |

### 3.4 CPU transpose를 16 byte 복사로 (§7.4, vtalib `CPUBundle_generic.cpp`)

- **왜:** NHWC ↔ N{C/16}HW16 변환은 픽셀마다 연속 16 byte를 옮기는 것뿐이다. 그런데 vtalib은 byte 단위로 옮겼다. `_zerofill`은 malloc한 NCHW 버퍼를 거치는 2단계였다. CMA 버퍼도 cache가 켜져 있어 malloc 메모리와 속도가 같았으니, 느린 원인은 코드였다.
- **어떻게:** 16 byte 단위 복사로 바꿨다. 결과는 같다.

| transpose (run당) | 예전 | 16 byte 복사 |
|---|---|---|
| `nhwc2vtaio` 56×56×64, OMP 1 | 1.75 ms | 0.20 ms |
| `vtaio2nhwc` 56×56×64, OMP 1 | 3.31 ms | 0.26 ms |
| conv0 `zerofill` 224×224×3 → 16, OMP 4 | 6.8 ms | 0.2 ms |
| **Resnet18 single, OMP 1** | 52.48 ms | **46.90 ms** |
| Resnet18 single, OMP 4 | 48.71 ms | 41.81 ms |
| multi4 4 copy 100회 | 1,471 ms | 1,314 ms |

### 3.5 VTA buffer pool (§7.5, vtalib `api.cc`)

- **왜:** bundle은 run마다 activation 버퍼 23개를 `VTABufferAlloc` / `Free`했다. 매번 `cma_alloc` / `cma_free` ioctl이라 run당 2.7 ms가 들었다.
- **어떻게:**
  - `VTABufferFree`는 버퍼를 크기별 free list에 남긴다. 같은 크기의 `VTABufferAlloc`이 그것을 꺼내 쓴다. mutex로 보호해 multi4의 thread 4개가 같은 list를 쓴다.
  - list에 남기는 양은 `EVTA_BUF_POOL_MB`(기본 32)까지다. `0`이면 예전처럼 바로 해제한다.
  - 재사용 버퍼에는 이전 내용이 남는다. `cma_alloc`도 지우지 않으므로 동작은 같다.

| 경우 | pool 끔 | pool 켬 |
|---|---|---|
| run당 Alloc + Free | 2.7 ms | 0.05 ms |
| **single, OMP 1** | 46.97 ms | **44.27 ms** |
| single, OMP 4 | 41.89 ms | 39.07 ms |
| multi4 4 copy 100회 | 1,286 ms | 1,189 ms |

### 3.6 CPU fc NEON (§7.6, vtalib `cpuFullyConnected`)

- **왜:** dense(512 → 1000, weight 512 KB)가 OMP 1에서 4.4 ms였다. 출력 64개씩 나눠 k마다 weight 행의 64 byte를 읽었다. 그래서 cache에 없는 weight를 1000 byte 간격으로 건너뛰며 DRAM에서 읽었고, prefetch가 걸리지 않았다.
- **어떻게:**
  - thread마다 출력의 연속 구간을 맡긴다.
  - weight 행을 순서대로 8행씩 읽고, 출력 64개의 합을 NEON register 16개에 둔다(int8 × int8 → int16 → int32).
  - 다음 8행은 prefetch한다. 정수 합이라 결과는 bit 단위로 같다.

| 경우 | 이전 | NEON |
|---|---|---|
| fc, OMP 1 | 4.43 ms | 0.74 ms |
| **single, OMP 1** | 44.27 ms | **40.56 ms** |

### 3.7 multi4 멈춤 원인: `dsb sy` (§7.7, vtalib `pynq_driver.cc`)

- **증상:** EVTA 4개가 같이 돌 때만, 약 400회에 한 번 멈췄다. 늘 1×1 downsample conv 바로 다음의 3×3 stride-2 conv였다.
- **원인을 가른 실험:**

| run 앞에서 하는 일 | 멈춤 |
|---|---|
| 없음 | 7번째 100회에서 |
| 5 µs 대기를 flush **앞**에 | 2번째 100회에서 |
| uop / 명령 버퍼를 반쪽씩 번갈아 씀 | 4번째 100회에서 |
| cache flush 뒤 `dsb sy` | **4000회 0번** |

- **원인:** PYNQ 2.4 `libcma.so`의 `cma_flush_cache`는 `dc civac` loop뿐이고 `dsb`가 없다. 그래서 EVTA를 시작하는 MMIO 쓰기가, 정리된 명령 / uop가 DRAM에 닿기 전에 나갈 수 있었다.
- **고침:** `VTAFlushCache` / `VTAInvalidateCache`가 libcma 다음에 `dsb sy`를 한다(`EVTA_NO_DSB=1`이면 뺀다). 비용은 측정 오차 안이었다.
- 함께 넣은 것: run 시간 제한과 hang dump(`EVTA_RUN_TIMEOUT_MS`).

### 3.8 fc를 EVTA로 (§7.8, `-vta-evta-fc`, vtalib `fullyconnected_evta_packed`)

- **왜:** fc를 1×1 conv 경로로 보내면 HLS GEMM의 최소 tile 7×7 때문에 연산이 49배가 됐다.
- **어떻게:**
  - **weight 배치:** weight와 bias는 compile time에 EVTA layout(출력 블록 우선)으로 weights 파일에 쓴다.
  - **LOAD와 GEMM 겹치기:** weight를 wgt 버퍼 반쪽(512 entry)씩 LOAD하고, 다른 반쪽을 GEMM하는 동안 다음 반쪽을 읽는다.
  - **bias:** acc의 초기값으로 LOAD한다.
  - **반올림:** CPU `std::round`와 같게 ALU만으로 한다. `s = max(min(acc, 0), −1)`, `acc = (acc + s + 2^(shift−1)) >> shift`, 그 다음 clip이다.
  - **CPU 경로로 되돌리는 경우:** 조건(batch 1, offset 0, IN % 16, 2^−shift scale, 덩어리당 출력 블록 ≥ 8)이 맞지 않으면 compile time에 CPU fc로 간다.

| 경우 | CPU NEON fc | EVTA fc |
|---|---|---|
| fc 한 번 | 0.74 ms | **0.152 ms** |
| **single, OMP 1** | 40.56 ms | **39.92 ms** |

### 3.9 입력 전처리를 conv0 입력 복사 하나로 (§7.9, `-vta-fuse-preprocess`)

- **왜:** conv0 앞에서 CPU가 quantize, elemsub(mean), elemdiv(std), zerofill을 따로 했다. 합쳐서 2.4 ms였다.
- **어떻게:**
  - **표:** elemsub / elemdiv 사슬은 quantize한 값(256개)과 채널에만 달렸다. 그래서 compile time에 표 [C][256]으로 만들어 weights 파일에 넣는다. 표는 CPU와 같은 float 계산으로 만든다.
  - **NEON 한 번:** vtalib `preprocess_nhwc2vtaio_zerofill`이 16 pixel씩 처리한다. `vcvtaq`로 quantize하고, `vqtbl4q` 4번으로 표를 찾고, `vst4q_u32`로 EVTA layout에 쓴다.
  - **검증:** 0.5 경계, 포화, 2의 거듭제곱이 아닌 scale을 넣은 단위 시험에서 0 불일치였다.

| 경우 | 이전 | 합침 |
|---|---|---|
| 입력 전처리 | 2.2 + 0.2 ms | **0.37 ms** |
| **single, OMP 1** | 39.92 ms | **37.08 ms** |

### 3.10 명령 record / replay (§7.10, AOT 0단계, vtalib `cmd_queue.h` `ReplayBegin`)

- **왜:** vtalib은 conv를 부를 때마다 CPU가 256-bit 명령과 uop를 다시 만들었다(JIT). 원래 VTA + TVM은 uop를 컴파일러에서 만들었는데, NEST-C는 Glow에 붙이기 쉽게 runtime에서 만들었다. 명령 생성 비용은 run당 약 2.5 ms였다(OMP 4).
- **어떻게:**
  - **기록:** 같은 인자의 EVTA 호출(conv, conv + residual, maxpool, fc)은 첫 호출에 명령과 uop를 기록한다. 이때 버퍼 주소를 고칠 relocation 표(입력 / weight / bias / 출력)도 함께 기록한다.
  - **재생:** 두 번째부터는 복사하고 주소만 고친 뒤 flush해서 바로 실행한다.
  - 나중에 컴파일러 AOT(1단계)로 가는 형식의 시험대이기도 하다.
  - `EVTA_REPLAY=0`이면 끈다.
- **검증:** bit-exact였고, multi4 copy당 4000회 동안 멈춤이 없었다.

| 경우 | 이전 | replay |
|---|---|---|
| **single, OMP 1** | 37.08 ms | **34.50 ms** |
| single, OMP 4 | 36.04 ms | 33.48 ms |
| multi4 100회 | 1,060.5 ms | 977.9 ms |

### 3.11 측정: 층 사이 DRAM 왕복은 얼마나 드나 (§7.11)

층을 SRAM 안에서 잇는 하드웨어를 만들 가치가 있는지 먼저 쟀다. 쓴 도구는 둘이다.
- `EVTA_REPLAY_LOG=2`: run마다 GEMM / ALU 반복 수, DRAM byte, 시간을 출력한다.
- `EVTA_MEMEXP`: replay 사본의 LOAD / STORE를 원소 하나로 줄인다. 결과는 틀어지지만, 그 전송이 없을 때의 시간이 나온다.

| | EVTA 시간 | 기준 대비 |
|---|---|---|
| 그대로 | 32.18 ms | |
| GEMM 반복 / 400 MHz | 23.60 ms | conv0 6.27(0 채움으로 13/16 버려짐) |
| ALU 반복 / 400 MHz | 2.32 ms | |
| inp LOAD 없음 | 30.80 ms | −1.38 |
| STORE 없음 | 31.20 ms | −0.98 |
| **activation 전부 없음** | **29.82 ms** | **−2.36** |
| weight LOAD 없음 | 31.39 ms | −0.79 |
| DRAM 전송 전부 없음 | 29.13 ms | −3.05 |

- conv → conv를 SRAM 안에서 이어도 줄어드는 것은 최대 약 1.05 ms(3 %)다. DAE가 load와 store를 이미 거의 다 GEMM 뒤에 숨기고 있었다. **그래서 그 하드웨어 경로는 만들지 않았다.**
- 가장 큰 덩어리는 conv0의 0 채움(GEMM 약 5 ms 낭비)이었다. 그래서 다음 작업을 conv0으로 정했다.

### 3.12 conv0을 packed 입력의 4×1 conv로 (§7.12, `-vta-conv0-pack`)

- **왜:** 7×7/2 conv0을 3 → 16채널로 0 채워 돌리면 GEMM의 13/16이 0을 곱한다(9.13 ms).
- **어떻게:** stride 2의 행 두 개와 kw 7개를 채널로 접는다.
  - **입력:** packed 픽셀 (q, x)의 채널 (dh·7 + j)·3 + c는 pad된 입력 (2q + dh, 2x + j, c)다. 2·7·3 = 42를 48채널(3 블록)로 맞추고, 크기는 115×112다.
  - **conv:** 4×1, stride 1, pad 0으로 기존 `convolution_wo_tr`를 그대로 쓴다. filter 탭은 (k, (dh·7 + j)·3 + c) = 원래 (2k + dh, j, c)이고, 2k + dh = 7이면 0이다. 정수 합이 같으므로 출력도 같다.
  - **GEMM 반복:** 출력 픽셀·출력 블록당 49에서 12(4 탭 × 3 블록)로 준다.
  - **schedule:** 2 thread, 16×16이다. 입력 tile이 912 entry라 multi-EVTA inp의 절반(1024)에 들어간다.
  - **일반형(`getConv0Pack`):** KH×KW / stride s, C채널이면 ceil(KH/s)×1 / stride 1 conv, CO = s·KW·C(16으로 올림)이다. 조건이 맞지 않으면 0 채움 경로를 그대로 쓴다.
  - **입력 복사(`preprocess_conv0pack`):** packed 행 q는 pad된 입력 행 2q, 2q+1만 읽는다. 그래서 thread마다 그 두 행만 작은 버퍼(약 1.4 KB)에 quantize + 표로 쓰고 곧바로 pack한다. pack 루프의 aliasing(byte store 때문에 표 index를 매번 다시 읽음)을 지역 변수로 고쳐 pack이 0.88 → 0.35 ms가 됐다.
- **검증:** `EVTA_DUMP_CONV`로 모든 EVTA conv 출력 40개가 0 채움 경로와 byte 단위로 같다(Resnet50 106개, VGG16 26개도).

| 경우 | 이전 | conv0 pack |
|---|---|---|
| conv0 EVTA 시간 | 9.13 ms (GEMM 하한 6.27) | **3.64 ms** (GEMM 하한 1.63) |
| EVTA 시간 합 | 32.18 ms | 26.82 ms |
| **single, OMP 1** | 34.48 ms | **29.19 ms** |
| single, OMP 4 | 33.48 ms | 28.02 ms |
| multi4 100회 | 977.9 ms | 827.3 ms |

### 3.13 conv0 + maxpool을 EVTA 호출 하나로 (§7.13, `-vta-fuse-pool`, vtalib `convolution_wo_tr_pool`)

- **왜:** conv0 출력(784 KB)을 DRAM에 쓰고, maxpool 호출이 그것을 다시 읽어 단위 GEMM과 ALU를 돌렸다(1.19 ms).
- **어떻게:**
  - conv0 출력 tile(16×16)을 pool window의 halo만큼 위로 1행, 왼쪽으로 1열 더 계산한다(17×17). 이 때문에 GEMM이 약 13 % 늘어난다.
  - requant / relu 뒤 acc 안에서 ALU MAX를 두 단계로 한다. 행 방향(명령 2개)과 열 방향(reset + 명령 3개)이다.
  - pooled 영역만 STORE한다.
  - **1판과 2판:** 1판은 창 위치마다 명령을 냈고(tile당 19개), 줄어든 DRAM 왕복을 명령 고정비가 다 잡아먹었다. 2판은 두 단계에 블록당 uop로 해서 tile당 6개가 됐다.
- **검증:** maxpool 출력과 그 뒤 모든 EVTA 출력이 byte 단위로 같다. `EVTA_RES_CHECK=1`에서 200,704개 중 0 불일치였다.

| 경우 | 따로 | 1판 | 2판 |
|---|---|---|---|
| conv0 + maxpool EVTA | 3.64 + 1.19 = 4.83 ms | 4.80 ms | **4.48 ms** |
| **single, OMP 1** | 29.19 ms | 29.25 ms | **28.83 ms** |
| single, OMP 4 | 28.02 ms | 28.02 ms | 27.71 ms |
| multi4 100회 | 827.3 ms | 823.1 ms | 820.3 ms |

## 4. 남은 EVTA 시간과 하드웨어 개선 (11-02 ~ 11-04)

### 4.1 EVTA 26.47 ms는 어디에 쓰이나 (§7.14, `EVTA_REPLAY_DUMP`)

| 덩어리 | ms | 몫 | 근거 |
|---|---|---|---|
| GEMM 반복 / 400 MHz | 18.85 | 71 % | systolic, 1 cycle / 반복 |
| ALU 반복 × **2** / 400 MHz | 4.63 | 17.5 % | HLS `alu` 루프가 `PIPELINE II = 2` |
| 나머지 | 2.99 | 11 % | conv0 tile 1.08, 그 밖의 run 20개가 run당 60–130 µs |

ALU 4.63 ms의 내역:

| ALU 연산 | ms | 무엇 |
|---|---|---|
| ADD (bias) | 0.91 | conv마다 |
| SHR imm | 0.90 | requantize shift |
| MIN 127 | 0.98 | 위쪽 clip |
| MAX 0 / MAX −128 | 0.69 / 0.29 | relu / 아래쪽 clip |
| MAX (pool, conv0) | 0.45 | §3.13 |
| ADD imm (반올림 상수) | 0.16 | |

run과 명령 하나의 비용 (`hwtest/runbench.cpp`):

| 측정 | 값 |
|---|---|
| 가장 작은 run (reset, STORE 1) | **4.7 µs** (flush, MMIO 시작, done poll 포함) |
| GEMM 반복 | 1.0 cycle |
| GEMM 명령 하나 | 약 25 cycle |
| ALU 반복 (imm도 reg도) | **2.0 cycle** |
| ALU 명령 하나 | 약 35 cycle |
| inp LOAD / STORE 큰 것 | 5.0 / 5.2 GB/s |

- **run 고정비:** 4.7 µs × 21 run = 0.1 ms라 run을 합쳐도 얻을 것이 없다.
- **ALU가 2 cycle인 이유:** HLS 보고서상 acc를 두 번 읽고(src, dst) 한 번 쓰는데, acc_mem의 포트가 둘뿐이라 II = 2가 된다.
- **결론:** GEMM 밖의 7.6 ms 가운데 ALU 4.6 ms가 가장 크고, 원인은 하드웨어 II다. 그래서 하드웨어를 고치기로 했다.

### 4.2 ALU imm 연산을 II = 1로 (§7.15, 비트스트림 `..._sa_400mhz_alu1_epr`)

- **왜:** imm 연산(SHR, MIN, MAX imm, ADD imm)이 ALU 반복의 69 %다. 이 연산들은 acc를 dst 하나만 읽으므로 II = 1이 가능하다.
- **어떻게:**
  - systolic 빌드의 `vta.cc` `alu()`에 `use_imm` 명령만 도는 루프를 따로 둔다. 반복마다 acc를 포트 A로 한 번 읽고 포트 B로 한 번 쓴다.
  - reg 연산(bias ADD, pool MAX)은 II = 2 그대로다. ISA, vtalib, 컴파일러, bundle은 바뀌지 않는다.
  - **hazard 확인:** 같은 dst를 몇 clk 안에 다시 읽으면 안 된다. Resnet18 ALU 명령 1,566개를 `EVTA_REPLAY_DUMP`로 보니, imm 명령은 16 반복 안에 같은 주소를 다시 쓰지 않았다.
  - **400 MHz timing:** 첫 빌드는 WNS −0.819 ns였다. uop 읽기 뒤와 주소 계산 뒤에 register(`alu_reg`)를 넣어 −0.126 ns가 됐고, `Performance_ExplorePostRoutePhysOpt` 전략으로 **WNS +0.001 ns**가 됐다.
- **검증:** golden 30 PASS, `EVTA_DUMP_CONV` Resnet18 40개 / Resnet50 106개가 byte 단위로 같다.

| 측정 (같은 실행 파일) | `..._sa_400mhz` | `..._alu1_epr` |
|---|---|---|
| runbench ALU imm 반복 | 2.0 clk | **1.0 clk** |
| Resnet18 EVTA | 26.42 ms | **24.89 ms** |
| **Resnet18 single, OMP 1** | 28.98 ms | **27.32 ms** |

multi4(11-04)는 `reset_mem` 파이프라인과 acc / out 쓰기 앞 register를 더 넣고 Y219 flow를 거쳐 통과했다. EVTA마다 26.73–26.85 → 25.24–25.37 ms였다.

### 4.3 RMW reset에 bias (§7.16, 비트스트림 `..._sa_400mhz_bias`)

- **왜:** GEMM reset이 acc를 0으로 쓰고, GEMM 뒤에 bias를 acc에 LOAD해 ALU ADD로 더했다(0.91 ms).
- **어떻게:** bias LOAD를 tile 시작으로 옮기고, reset GEMM이 0 대신 acc[bias + f]를 acc[base + f·T + p]에 쓴다. 정수 덧셈이라 결과는 같다.
  - ISA: `VTAGemInsn`의 pad 1 bit를 `bias_reset`으로 쓴다(reset이면 uop의 src가 acc 주소).
  - HLS: GEMM uop stream에 bit 하나를 더했다.
  - RTL: `sa_ctrl`가 tag에 bias와 src를 싣고, `sa_rmw`는 bias 행이면 acc[src]를 읽어 dst에 쓴다. forwarding은 읽는 주소로 비교한다.
  - vtalib: 이름에 `bias`가 있는 비트스트림에서만 켠다(`EVTA_BIAS_RESET=0|1`).
  - **검증 순서:** C 모델 → RTL testbench 96/96 → HLS CSim → 보드
- **검증:** golden 30 PASS, dump byte 동일

| 측정 | `alu1_epr` | `bias` |
|---|---|---|
| Resnet18 EVTA | 24.889 ms | **24.154 ms** |
| **Resnet18 single, OMP 1** | 27.303 ms | **26.592 ms** |

- **예상보다 덜 준 이유:** 예상(0.91 ms)보다 0.15 ms 적게 줄었다. `EVTA_BIAS_RESET=0|1`과 `EVTA_MEMEXP`로 conv별로 재 보니 이유가 나왔다. stage3 / 4에서는 tile 끝의 ADD가 다음 tile의 inp LOAD나 앞 tile의 STORE와 이미 겹쳐 있었다.

| conv | 그대로 | traffic 모두 뺌 | ADD 반복 × 2 cycle |
|---|---|---|---|
| conv0 (+ pool) | −0.316 ms | −0.292 | 0.28 |
| stage1 3×3 (개당) | −0.064 ~ −0.068 | −0.062 ~ −0.064 | 0.063 |
| stage2 (개당) | −0.019 ~ −0.035 | −0.031 ~ −0.036 | 0.031 |
| stage3 (개당) | +0.001 ~ −0.010 | −0.016 | 0.016 |
| stage4 (개당) | +0.002 ~ −0.004 | −0.007 ~ −0.009 | 0.008 |
| 합 | **−0.760** | **−0.832** | 0.91 |

multi4: EVTA마다 25.23–25.37 → 24.49–24.62 ms

### 4.4 requantize를 acc → out 쓰기에 (§7.18, 비트스트림 `..._sa_400mhz_rq`, `docs/evta_isa_ext.md`)

- **왜:** GEMM과 ALU는 단계마다 acc의 하위 8 bit를 out에 쓴다. 그 값이 결과가 되도록 ALU SHR / MAX / MIN(반올림 ADD)으로 acc 자체를 바꾸고 있었다. bias reset 뒤 남은 ALU의 대부분이 이것이었다.
- **어떻게:**
  - 명령의 `rq_*` 필드(en, round, shift, lo, hi)로 out에 쓰는 값을 clip((acc + round) >> shift, lo, hi)로 만든다. 그러면 그 ALU 명령들이 필요 없다. acc는 바뀌지 않는다.
  - ISA: `VTAGemInsn` 비트 141–164, `VTAAluInsn` 144–167을 쓴다(pad 자리). round는 0 없음, 1 half up, 2 half away from zero다. `rq_en = 0`이면 전과 같다.
  - RTL `sa_rmw.v`: acc 쓰기(S3) 뒤에 S4 덧셈, S5 산술 shift, S6 clip을 둔다. out 쓰기 / done / token을 S6으로 옮겼다.
  - vtalib `VTARequant`: 일반 conv는 GEMM에, conv + pool은 열 방향 pool MAX에, residual은 residual GEMM에, fc는 fc GEMM에 붙인다. 이름에 `_rq`가 있는 비트스트림에서만 켠다.
  - **식의 확인:** C 모델 `rqout()`이 지금 vtalib의 ALU 순서와 5,750만 값에서 같다.
  - **함께 고친 결함:** vtalib `NextInsn()`이 명령을 0으로 채우지 않아 pad 비트가 쓰레기였다. 이제 0으로 채운다.
  - 400 MHz timing: WNS +0.039 ns, LUT 34.3K → 42.1K
- **검증:** golden PASS, dump byte 동일, `EVTA_RES_CHECK=1` 20건 불일치 0. `EVTA_REQUANT=0`이면 `bias`와 같다.

| 측정 | `bias` | `rq` |
|---|---|---|
| Resnet18 EVTA | 24.153 ms | **23.053 ms** (−1.10) |
| **Resnet18 single, OMP 1** | 26.611 ms | **25.493 ms** |

multi4: 395.8 MHz 판(MMCM 배수 조정)으로 먼저 통과했다. 그 다음 placer directive를 바꾸고 Y219 net 하나를 RapidWright로 다시 배선해 400 MHz로 통과했다. EVTA마다 24.49–24.62 → **23.44–23.56 ms**, busy union 583.8 → **610.4 GOPS**다.

### 4.5 하드웨어 세 단계를 함께 (11-05, 같은 실행 파일에서 비트스트림만 바꿈)

| | 기본 400 MHz | ALU II = 1 | bias reset | requantize | 줄어든 비율 |
|---|---|---|---|---|---|
| Resnet18 EVTA | 26.43 ms (137 GOPS) | 24.89 | 24.15 | **23.05 ms (157 GOPS)** | −12.8 % |

## 5. 그 뒤 (11-05 ~ 11-10)

| 날짜 | 바꾼 것 | 왜 / 어떻게 | Resnet18 single OMP 1 |
|---|---|---|---|
| 11-05 | systolic 최소 tile 1 (§7.19 ③) | HLS GEMM은 acc hazard 검사가 없어 tile이 7×7 이상이어야 했다. systolic `sa_rmw.v`는 forwarding이 있어 최소가 필요 없다. 이 결함을 고치다 vt 2 더미 tile의 채널 블록 식 결함도 찾았다(두 vtalib) | 25.53 → **25.42 ms** (EVTA 23.05 → 22.99) |
| 11-05 | bias entry가 thread 1의 acc를 덮던 결함 (§7.19 ⑤) | `conv_kernel`이 bias 자리를 빼지 않아 VGG16에서 틀림. Resnet18은 영향 없음(dump 같음) | 그대로 |
| 11-06 | conv0 입력 복사를 conv와 겹침 (§7.19 ⑥) | conv0 입력 복사(quantize + packing) 1.65 ms는 NEON 명령 수가 한계라 더 줄지 않았다. 그래서 숨겼다. 내용은 아래와 같다 | 25.43 → **24.08 ms** |
| 11-10 | conv DMA 묶기 (§7.29, `EVTA_CONV_BURST`) | 연속인 LOAD / STORE 행을 한 행으로, 1×1 conv의 weight를 한 번에, 폭 전체 tile의 출력 블록을 STORE 하나로 보낸다. Resnet18은 3×3이 대부분이라 효과가 작다 | 24.08 → **23.27 ms** |

**11-06 겹치기의 내용:**
- pynq driver의 `Run`을 `RunStart` + `RunWait`로 나눴다.
- replay는 run마다 hook을 부른다.
- systolic vtalib `InputStream`은 conv0을 tile 행마다 run으로 나눈다. run i가 도는 동안 CPU가 행 i+1의 입력을 채운다(double buffering).
- 이 구조는 interrupt + 명령 링(장기 계획 3단계)의 바탕이기도 하다.

## 6. multi4 처리량의 변화

EVTA 4개에 bundle copy 4개를 동시에 돌린다(OMP 1).

| 날짜 | 단계 | 100회 wall (4 copy) | copy 하나 1장 |
|---|---|---|---|
| 10-29 | 시작 | 약 3,909 ms | 153–160 ms |
| 10-31 | ① layout 유지 | 2,793 ms | 94.9 ms |
| 10-31 | ② residual fusion | 1,851 ms | 70.7 ms |
| 10-31 | maxpool EVTA | 1,471 ms | 56.1–56.5 ms |
| 11-01 | transpose 16 byte | 1,314 ms | 49.5 ms |
| 11-01 | buffer pool | 1,189 ms | |
| 11-01 | CPU fc NEON | 1,089–1,131 ms | 41.2 ms |
| 11-02 | fc EVTA | | 40.56 ms |
| 11-02 | 전처리 합치기 | 1,060.5 ms | 37.59 ms |
| 11-02 | replay | 977.9 ms | 34.99 ms |
| 11-02 | conv0 pack | 827.3 ms | 29.76 ms |
| 11-02 | conv0 + pool | **820.3 ms** | 29.37 ms |
| 11-04 | 하드웨어 세 단계 (multi4 400 MHz rq) | | EVTA마다 23.44–23.56 ms, 610.4 GOPS |
| 11-06 | conv0 입력 겹침 | | **24.58–24.67 ms** |

**4 copy를 같이 돌리면 copy마다 조금 느려진다(§7.19 ⑧, Resnet50으로 잼):**
- 어느 쌍이든 약 +0.9 ms다. DDR controller를 같이 쓰기 때문이다.
- HP1 + HP2 쌍은 0.35 ms가 더 붙는다.
- 4개를 다 돌리면 +2.5–2.95 ms다.
- 대역이 아니라 load latency가 늘어나는 것이다(EVTA당 약 1 GB/s).

## 7. 작업 중 발견하고 고친 결함

| 결함 | 증상 | 고친 곳 (§) |
|---|---|---|
| libcma flush에 `dsb` 없음 | multi4가 약 400회에 한 번 멈춤 | vtalib `VTAFlushCache` (§7.7) |
| `fullyconnected_evta_packed`에 replay 없음 | fc만 매번 JIT | vtalib (§7.11) |
| `_zerofill`이 N > 1에서 batch를 섞음 | (N = 1에서는 안 드러남) | vtalib transpose (§7.4) |
| vtalib 명령 pad 비트가 0으로 채워지지 않음 | 새 필드(`rq_*`)에 쓰레기 | `NextInsn()` (§7.18) |
| vt 2 더미 tile의 채널 블록 식 | 7×7보다 작은 출력에서 오답 | 두 vtalib (§7.19 ③) |
| bias entry가 thread 1의 acc를 덮음 | VGG16 conv0 오답(HLS, 시뮬레이터) | `conv_kernel` (§7.19 ⑤) |
| multi buffer에서 마지막 thread의 bias가 acc 밖 | systolic multi4 VGG16 오답 | systolic `conv_kernel` (§7.19 ⑦) |
| vt가 출력 블록 수를 못 나누면 계산 없음 | (DeiT에서 발견, Resnet18은 짝수라 무관) | 두 vtalib (§7.28) |

## 8. 지금 상태와 남은 것

### 8.1 지금 (11-10, single, OMP 1, 23.27 ms)

| 항목 | 시간 |
|---|---|
| EVTA 호출 합 (`convolution_wo_tr` 등) | 22.65 ms (약 160 GOPS, 하한 17.7 ms의 78 %) |
| CPU fc / 기타 | 0.13 ms + 약 0.5 ms |

CPU 몫은 거의 없어졌다. 이제 시간은 EVTA가 정한다.

### 8.2 다음 후보 (§7.17과 그 뒤 측정 기준, 추정)

| 후보 | 내용 | 예상 | 비고 |
|---|---|---|---|
| reg-reg ALU II = 1 | pool MAX, residual ADD는 acc를 두 개 읽음 | 약 −0.25 ms | acc bank 분할이나 포트 추가로 BRAM 비용이 큼 |
| conv0 tile 키우기, 1×1 downsample을 같은 입력의 conv와 합치기 | 소프트웨어 | 각각 수백 µs | |
| 여러 층을 한 명령 stream으로 | 다음 층의 LOAD가 앞 층의 STORE와 겹침 | 약 −1 ms | runtime(구간 제출) |
| **한 장을 EVTA 4개로** | 층마다 출력 채널(KN)이나 행을 나눔. 지금 multi4는 처리량만 늘림 | 1장 약 23 → 7–9 ms | `convolution_wo_tr`이 핸들 배열을 받고, start / wait를 나누는 API |
| GEMM 32×32 / 2배 클럭 | MAC 4배 또는 처리량 2배 | EVTA 약 2.5배 (weight 대역에 막힘, §6) | ISA, SRAM, backend의 블록 16 고정 |
| multi4 하드웨어 | HP 포트 앞 register slice 빼기(Y219 fence가 필요 없는 보드), HP1 / HP2 공유 피하기 | copy당 1–3 ms | 비트스트림 |
| 다른 NPU와 비교 | 같은 ZCU102 DPU는 Resnet18 약 5.4 ms(스레드 1개), 효율은 우리가 높음(75 % 대 60 %) | | `kv260_product_compare.md` §3.2 |

### 8.3 배운 것

1. **runtime의 단위가 성능을 정했다.**
   - DAE 하드웨어는 처음부터 93 % 효율을 낼 수 있었다.
   - 시간을 잡아먹은 것은 conv마다 layout 왕복, 할당, CPU 연산, 명령 생성이었다.
   - 그것을 없애는 것만으로(하드웨어 변경 없이) 148.7 → 28.8 ms가 됐다.
2. **하드웨어를 바꾸기 전에 쟀다.**
   - `EVTA_MEMEXP`로 "층을 SRAM 안에서 잇는 하드웨어"의 상한이 1 ms뿐임을 확인하고 만들지 않았다.
   - `EVTA_REPLAY_DUMP`와 runbench로 ALU II가 진짜 원인임을 확인한 뒤에 고쳤다.
3. **ISA는 남는 비트를 쓰고, 비트스트림 이름으로 켰다.**
   - bias reset과 requantize는 명령의 pad 비트를 썼고, vtalib이 비트스트림 이름(`bias`, `_rq`)을 보고 켰다.
   - 그래서 이전 비트스트림과 같은 실행 파일로 비교할 수 있었다.
   - 다음 ISA 개정은 이것을 정식 버전 관리로 바꿔야 한다(`unified_runtime_plan.md`의 장치 능력 조회).
4. **검증 도구를 먼저 만들었다.** 모든 단계를 byte 단위로 비교할 수 있었기 때문에 빠르게 진행할 수 있었다. 쓴 도구는 `EVTA_DUMP_CONV`(층 출력 dump), `EVTA_RES_CHECK`(CPU 경로와 실행 중 비교), golden, C 모델 → RTL testbench → CSim 순서다.

## 9. 재현

```bash
cd vtabundles_xcmpl
sudo hw/script/run_prof.sh -s _systimed -r Resnet18Test          # 1장, 1코어, 문장별
sudo hw/script/run_prof.sh -s _systimed -r -t 4 Resnet18Test     # 4코어
sudo hw/script/run_prof.sh -s _sysprof -r Resnet18Test           # EVTA 호출별 시간과 GOPS
sudo /bin/sh -c "hw_multievtax4/script/run_prof.sh -s _sysprof -n 25 -l -r MultiEVTAResnet18Test"   # multi4
sudo hw/script/run_all.sh Resnet18Test                           # golden (HLS 380 MHz)
```

단계별로 끄는 방법(그 단계 이전과 같은 동작):

| 단계 | 끄는 방법 |
|---|---|
| layout 유지 | `-vta-keep-layout=false` (번들 생성) |
| residual fusion | `-vta-fuse-residual=false` |
| maxpool EVTA | `-vta-evta-maxpool=false` |
| fc EVTA | `-vta-evta-fc=false` |
| 전처리 합치기 | `-vta-fuse-preprocess=false` |
| conv0 pack | `-vta-conv0-pack=false` |
| conv0 + pool | `-vta-fuse-pool=false` |
| buffer pool | `EVTA_BUF_POOL_MB=0` (실행 시) |
| `dsb sy` | `EVTA_NO_DSB=1` |
| replay | `EVTA_REPLAY=0` |
| bias reset / requantize | `EVTA_BIAS_RESET=0`, `EVTA_REQUANT=0`, 또는 이전 비트스트림(`BIN=...`) |
| systolic 최소 tile | `EVTA_MIN_TILE=7` |
| conv0 입력 겹침 | `EVTA_INPUT_STREAM=0` |
| DMA 묶기 | `EVTA_CONV_BURST=0` |

검증용 환경변수: `EVTA_DUMP_CONV=<dir>`, `EVTA_RES_CHECK=1`, `EVTA_REPLAY_LOG=2`, `EVTA_REPLAY_DUMP=<dir>`, `EVTA_MEMEXP=<mask>`, `EVTA_SCHED=...`
