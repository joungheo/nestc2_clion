# EVTA를 다른 보드에 넣을 수 있나 (2026-10-30)

ZCU102(XCZU9EG)에서 돌고 있는 EVTA(16x16 GEMM, int8)와 NEST-C runtime을 더 작은/싼 AMD 보드로 옮길 때
하드웨어 크기와 SW가 어떻게 되는지 정리했다. 합성은 해보지 않았고, EVTA 쪽 수치는 이 저장소의
utilization report, 보드 쪽 수치는 아래 출처에서 가져왔다.

## 1. 지금 EVTA가 쓰는 자원

| 설정 | LUT | FF | DSP | BRAM36 | 출처 |
|---|---|---|---|---|---|
| single 16_16_19_18 (380 MHz) | 약 26K | 약 36K | 약 268 | 258 | `vta-hw-zcu102-systolic/build/.../16_16_19_18_380mhz.utilization.rpt`, `vta-hw-zcu102-single/build/wgt16_clk200` |
| multi-EVTA 버퍼 16_15_18_17 (EVTA 하나당) | 약 26K | 약 36K | 약 268 | 약 139 | multi4 BRAM 556 ÷ 4 |

- NEST-C VTA backend는 블록 크기 16을 코드에 고정해 두었다(`lib/Backends/VTA/VTA.cpp:509`, `VTASave.cpp:805` 등).
  블록을 8로 줄이면 backend도 고쳐야 한다.
- 16_15_18_17은 vtalib의 `NESTC_EVTA_MULTI` 버퍼 설정이고 schedule(`VTASchedules.h`)도 이 크기에 맞는다.
  single EVTA로 이 설정을 쓰면 SW는 거의 그대로다.

## 2. 후보 보드

| | ZCU102 (지금) | Kria K24 (XCK24) | Kria K26 (KV260) | Spartan US+ SCU35 (EK-SCU35-G) | Spartan US+ SCU200 |
|---|---|---|---|---|---|
| 칩 | ZU9EG | ZU3급 MPSoC | ZU5급 MPSoC | SU35P | SU200P |
| LUT | 274K | 70.5K | 117K | 16K | 100K |
| DSP | 2,520 | 360 | 1,248 | 48 | 384 |
| BRAM36 | 912 | 216 | 144 (+URAM 64) | 48 (on-chip 전체 1.93 Mb) | 192 (+URAM 64) |
| hard CPU | A53 x4 + R5 | A53 x4 + R5F | A53 x4 + R5F | 없음 | 없음 |
| 외부 메모리 | DDR4 | LPDDR4 (용량 미확인) | DDR4 4 GB | HyperRAM 8 MB | LPDDR4X/5 hard MC 2개 (보드 용량 미확인) |
| 가격 | - | SOM $250 (industrial $350), KD240 kit $399 | 미확인 | $229 | 미확인 |

## 3. 보드별 판단

### Spartan UltraScale+ SCU35: 지금 EVTA는 안 들어간다
- LUT 1.6배, DSP 5.6배, BRAM 5배가 모자란다.
- 줄이는 방법: 블록 16은 두고 GEMM을 시간 분할(HLS II=8 → MAC 32개, wgt/acc 포트 폭 1/8), ALU도 시간 분할,
  버퍼를 uop 13/inp 14/wgt 16/acc 15(약 120 KB) 정도로. LUT는 16K에 들어갈지 합성 전에는 모른다.
  성능은 32 MAC x 200 MHz = 최대 12.8 GOPS, 실제로는 HyperRAM 대역(수백 MB/s)에 막힌다.
- **CPU가 없다.** PL에 MicroBlaze V를 넣고 baremetal로 돌려야 한다: `pynq_driver`(/dev/mem, libcma, pthread)를
  물리 주소 + cache flush/invalidate driver로 새로 쓰고, runtime의 64 MB 선할당을 줄이고, bundle을 riscv gcc로
  OpenMP 없이 빌드하고 weight를 flash/배열로 넣어야 한다. transpose/elemadd 등 CPU 연산은 soft core에서
  수십 배 느려지므로 EVTA로 옮기는 것(`evta_runtime_compare.md` §7 ①②)이 필수가 된다.
- **Resnet18 weight(int8 11.7 MB)가 HyperRAM 8 MB에 안 들어간다.** 작은 CNN(MNIST, CIFAR-10 ResNet-8/20, KWS)만 현실적.

### Spartan UltraScale+ SCU200: EVTA 하나는 들어간다, CPU는 여전히 없다
- 16_15_18_17이면 DSP 268/384, BRAM 약 139/192, LUT 26K/100K. EVTA 2개는 DSP가 모자란다.
  16_16_19_18을 쓰려면 wgt를 URAM으로 옮겨야 한다(폭 때문에 URAM 30개 안팎).
- 메모리는 LPDDR4X hard controller(최대 4266 Mb/s)라 대역과 용량 문제는 없다(보드 용량은 미확인).
- runtime 위치:
  - **A. PCIe Gen4 x8로 host PC에 붙이기(권장).** runtime/bundle은 x86에서, EVTA 데이터는 device LPDDR4X에.
    `pynq_driver`를 XDMA/QDMA + BAR MMIO driver로 새로 써야 하고, 층마다 PCIe 왕복을 피하려면
    ①(층 사이 layout 유지), ②(elemadd/relu/maxpool을 EVTA로)가 먼저 필요하다.
  - B. PL에 MicroBlaze V + Linux: 지금 구조와 비슷하지만 JIT와 CPU 연산이 A53보다 훨씬 느리다.
- 클럭은 같은 16 nm fabric이라 300 MHz대는 기대할 만하나 380/400 MHz는 합성으로 확인해야 한다.

### Kria K24 (ZU3급 MPSoC): 가장 가깝다
- 16_15_18_17이면 DSP 268/360, BRAM 약 139/216, LUT 26K/70K로 들어간다. 16_16_19_18(BRAM 258)은 안 된다.
- PS8(A53, Linux, HP 포트)이 같아서 EVTA RTL, NEST-C backend, bundle, vtalib JIT는 그대로 쓴다. 바꿀 것:

| 항목 | 할 일 |
|---|---|
| `vivado.tcl` | zcu102 대신 K24 part와 PS8 설정(DDR, HP 포트, 클럭). XCK24 part는 Vivado 2020.1에 없을 가능성이 크다 |
| `pynq_driver` 메모리 할당 | PYNQ 2.4 `libcma`(xlnk) 대신 u-dma-buf 또는 dma-heap (Kria Ubuntu/PYNQ 3.x에는 xlnk가 없다). SW에서 가장 큰 일 |
| bitstream 로딩 | `fpga_manager` + `xlnk_reset` 대신 `xmutil`/`fpgautil`(dfx-mgr) |
| `vtabundles_xcmpl` | multi 버퍼 설정을 EVTA 1개로 |
| 클럭 | commercial SOM speed grade가 ZCU102(-2)보다 낮으면 300 MHz 안팎 예상 (확인 필요) |

- 성능은 300 MHz면 EVTA 약 120 GOPS(ZCU102 380 MHz의 약 80 %). CPU 쪽은 A53 클럭이 같다면 비슷하다.
- ZU2(DSP 240)는 DSP가 조금 모자라 int8 2 MAC/DSP packing 같은 HLS 수정이 필요하고,
  ZU1(DSP 216, BRAM 108)은 블록 축소가 필요해 권하지 않는다.
- 개발 보드 가격으로는 K24 kit(KD240 $399)가 KV260보다 꼭 싸지는 않다. 양산 SOM 단가/칩 크기를 줄이는 게 목적이면 K24.

## 4. 정리

| | SCU35 | SCU200 | K24 |
|---|---|---|---|
| EVTA 16x16 | 안 들어감 (축소 재설계) | 들어감 (16_15_18_17) | 들어감 (16_15_18_17) |
| Resnet18 | 메모리 부족 | 가능 | 가능 |
| runtime | baremetal soft core 이식 | PCIe host driver 또는 MicroBlaze V Linux | 지금 흐름 + 메모리 할당/로딩 교체 |
| Vivado | 새 버전 + Vitis HLS | 새 버전 + Vitis HLS | 새 버전일 가능성 (part) |

다음 단계로는 Vivado 2020.1에서 XCZU3EG part로 16_15_18_17 EVTA를 합성해 보면 K24급에 들어가는지와 타이밍을 바로 볼 수 있다.

## 5. 확인하지 못한 것
- SU35P(SBVB625)의 PCIe/LPDDR4X hard controller 유무 (XA SU35P 표는 메모리 컨트롤러 0개)
- SCU200 키트 가격, LPDDR4X 용량과 폭, PCIe 카드 형태 여부
- K24 LPDDR4 용량(part 이름으로는 2 GB), speed grade, PYNQ 지원, XCK24를 지원하는 최소 Vivado 버전
- ZU1 LUT 수, KV260 kit 현재 가격, MicroBlaze V LUT 수와 Linux 지원 범위, HyperRAM 실효 대역

## 6. 출처
- [AMD SCU35 Evaluation Kit](https://www.amd.com/en/products/adaptive-socs-and-fpgas/evaluation-boards/scu35.html)
- [element14 EK-SCU35-G](https://au.element14.com/amd/ek-scu35-g/eval-board-fpga-spartan-ultrascale/dp/4842632)
- [Wevolver SCU35 specs](https://www.wevolver.com/iframe/specs/amd-xilinx-spartan-ultrascale-fpga-scu35-evaluation-kit)
- [AMD Spartan UltraScale+ XA](https://www.amd.com/zh-cn/products/adaptive-socs-and-fpgas/fpga/spartan-ultrascale-plus-xa.html)
- [AMD SCU200 Evaluation Kit](https://www.amd.com/en/products/adaptive-socs-and-fpgas/evaluation-boards/scu200.html),
  [zh-tw](https://www.amd.com/zh-tw/products/adaptive-socs-and-fpgas/evaluation-boards/scu200.html)
- [CNX Software: Spartan UltraScale+ 발표](https://www.cnx-software.com/2024/03/08/amd-announces-the-spartan-ultrascale-fpga-family-for-cost-sensitive-and-io-intensive-applications/)
- [DS985 K24 SOM Programmable Logic](https://docs.amd.com/r/en-US/ds985-k24-som/Programmable-Logic)
- [AMD Kria K24 commercial](https://www.amd.com/en/products/system-on-modules/kria/k24/k24c-commercial.html),
  [KD240 Starter Kit](https://www.amd.com/en/products/system-on-modules/kria/k24/kd240-drives-starter-kit.html)
- [CNX Software: Kria K24](https://www.cnx-software.com/2023/09/22/amd-kria-k24-zynq-ultrascale-system-on-module-targets-motor-control-and-dsp-applications/?amp=1)
- [DS891 Zynq UltraScale+ MPSoC overview](https://www.amd.com/content/dam/xilinx/support/documents/data_sheets/ds891-zynq-ultrascale-plus-overview.pdf)
- [Mouser cost-optimized product selection guide](https://www.mouser.lt/pdfDocs/cost-optimized-product-selection-guide.pdf)
