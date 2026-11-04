# EVTA 하드웨어 구조도

EVTA bitstream의 내부 구조를 전체에서 부분 순서로 그린다. 그림은 소스에서 그대로 옮겼다:
HLS 모듈은 `vta.cc`(fetch / load / compute / store), 연결은 Vivado block design(`scripts/vivado.tcl`),
크기는 config(`vta_config.json` → `build/hardware/xilinx/include/<CONF>/vta_config.tcl`)와 `include/vta/hw_spec.h`.
생성: `python3 docs/figs/src/make_evta_hls_figs.py`.

- 1장: 기존 HLS EVTA(single, `vta-hw-zcu102-single`, 16/16/19/18). GEMM이 HLS compute 안에 있는 원래 VTA 구조.
- 2장: systolic EVTA(`vta-hw-zcu102-systolic`, GEMM이 RTL `gemm_sa`).
- 바꾼 것마다 이전 / 이후는 [evta_overview.md](evta_overview.md)의 "구조 그림"(st01~st17), 명령 비트는 [evta_isa_ext.md](evta_isa_ext.md).

## 1. 기존 HLS EVTA

### 1.1 전체

![HLS 1](figs/evta_hls_01_overall.svg)

- HLS IP 4개가 따로 돈다. fetch는 run마다 한 번(START), 나머지는 명령 하나 처리하고 다시 시작(AUTORESTART 0x81).
- 명령 큐 3개(32 B × 512)는 fetch가 채우고, dependence 큐 4개(1 B × 1024)는 모듈끼리 token을 주고받는다.
- SRAM은 쓰는 모듈 하나(포트 A), 읽는 모듈 하나(포트 B): inp / wgt는 load → compute, out은 compute → store.
- DRAM 쪽 m_axi 5개는 smartconnect로 PS HP0 하나(128비트)에, 레지스터는 PS HPM0(100 MHz) → axi_xbar로.

### 1.2 fetch

![HLS 2](figs/evta_hls_02_fetch.svg)

### 1.3 load

![HLS 3](figs/evta_hls_03_load.svg)

### 1.4 compute (GEMM, ALU)

![HLS 4](figs/evta_hls_04_compute_gemm.svg)

### 1.5 store

![HLS 5](figs/evta_hls_05_store.svg)

### 1.6 interface와 버퍼

![HLS 6](figs/evta_hls_06_interfaces.svg)

### 1.7 dependence token

![HLS 7](figs/evta_hls_07_dependence.svg)

## 2. systolic EVTA

`vta-hw-zcu102-systolic`(SYSTOLIC=1, FREQ=400; multi4는 같은 RTL을 EVTA마다 하나씩). 소스: `vta.cc`의 `VTA_SYSTOLIC` 부분,
`rtl/gemm_sa_bd.v`, `gemm_sa.v`, `sa_ctrl.v`, `sa_array.v`, `sa_pe.v`, `sa_rmw.v`, `scripts/vivado.tcl`. 생성: `python3 docs/figs/src/make_evta_sa_figs.py`.
초록이 HLS EVTA에서 바뀌거나 새로 생긴 것이다. 설계 근거와 측정은 [evta_systolic_design.md](evta_systolic_design.md), [evta_systolic.md](evta_systolic.md).

### 2.1 전체

![SA 1](figs/evta_sa_01_overall.svg)

- GEMM 계산이 HLS compute에서 RTL `gemm_sa`로 나갔다. compute는 GEMM 단계마다 uop 하나(65비트)를 AXI-Stream으로 보내고 done을 센다.
- acc_mem이 compute 안 배열에서 block design BRAM이 되어, compute(ALU, acc LOAD)와 gemm_sa가 port mux로 나눠 쓴다.
- HP 쪽은 333 MHz(`hp_pll`), 나머지는 400 MHz. 레지스터 쪽에 `reg_dwc`(128 → 32비트).

### 2.2 fetch (같음)

![SA 2](figs/evta_sa_02_fetch.svg)

### 2.3 load (reset_mem pipeline)

![SA 3](figs/evta_sa_03_load.svg)

### 2.4 compute (HLS 쪽)

![SA 4](figs/evta_sa_04_compute.svg)

### 2.5 gemm_sa (RTL)

![SA 5](figs/evta_sa_05_gemm_sa.svg)

### 2.6 sa_array와 PE (DSP48E2)

![SA 6](figs/evta_sa_06_array_pe.svg)

### 2.7 sa_ctrl (uop FIFO, run, weight fill, issue)

![SA 7](figs/evta_sa_07_sa_ctrl.svg)

### 2.8 sa_rmw (acc read-modify-write, out 쓰기)

![SA 8](figs/evta_sa_08_sa_rmw.svg)

### 2.9 store (같음)

![SA 9](figs/evta_sa_09_store.svg)

### 2.10 interface와 버퍼 (바뀐 것)

![SA 10](figs/evta_sa_10_interfaces.svg)

### 2.11 dependence (GEMM token은 gemm_sa가)

![SA 11](figs/evta_sa_11_dependence.svg)
