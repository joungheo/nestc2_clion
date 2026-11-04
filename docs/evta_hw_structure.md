# EVTA 하드웨어 구조도

EVTA bitstream의 내부 구조를 전체에서 부분 순서로 그린다. 그림은 소스에서 그대로 옮겼다:
HLS 모듈은 `vta.cc`(fetch / load / compute / store), 연결은 Vivado block design(`scripts/vivado.tcl`),
크기는 config(`vta_config.json` → `build/hardware/xilinx/include/<CONF>/vta_config.tcl`)와 `include/vta/hw_spec.h`.
생성: `python3 docs/figs/src/make_evta_hls_figs.py`.

- 1장: 기존 HLS EVTA(single, `vta-hw-zcu102-single`, 16/16/19/18). GEMM이 HLS compute 안에 있는 원래 VTA 구조.
- 2장(다음): systolic EVTA(`vta-hw-zcu102-systolic`, GEMM이 RTL `gemm_sa`).
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

(다음에 같은 순서로 그린다.)
