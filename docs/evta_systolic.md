# EVTA GEMM을 systolic(weight-stationary) 구조로 — 계획, 조사, 방향 결정

최종 갱신: 2026-10-27 (Phase A·B·C 완료, Phase D 전). `docs/vta_synth_notes.md` §8에서 옮겼다(그 문서는 합성·보드·실행 명령, 이 문서는 systolic 전환).
`vta_synth_notes §x`는 그 문서의 절이다. 명령의 변수(`$N`, `$B`, `$VIV`, `$RW`, `$BASEBIT`)도 그 문서 머리말·vta_synth_notes §2.5 0)의 값이다.

다른 세션이 여기서 이어 시작할 수 있게 정리한 검토다. 읽는 순서:
- **§0 방향 결정**(무엇을 할지와 그 근거, 아래 절들의 요약)
- 계획: §1 제안 → §2 결론 → §3·§4 지금 HW와 uop 순서 → §5 HW 세부 설계 → §6 득실 → §7 순서
- 조사: §9 보고서(Gemmini, VTA 원논문, DSP-Fetch, FireFly v2, 2510.08137, DPU, WP486, Tensil, AutoSA, gemm_hls; 종합 §9.12). 원문은 `docs/ref/`
- 실험: §8 Phase A(타이밍, 클럭 sweep, 보드), §10 Phase B(uop 순서)·Phase C(사이클 모델)
- **Phase D 설계서(글과 그림)**: `docs/evta_systolic_design.md`
- 코드: 브랜치 `systolic`(2026-10-28 main에 fast-forward로 합침, 이후 작업도 `systolic`에서 하고 main에 다시 합친다), `vta-hw-zcu102-systolic`(HW), `vtalib-zcu102-systolic`(런타임, uop 순서와 uop cache 수정).

## 0. 방향 결정 (2026-10-27)

**질문**: EVTA를 더 빠르게 하려면 어디에 투자하나. 선택지는 아래 다섯 가지다. 숫자는 이 문서의 측정(vta_synth_notes §6, §8, §10)과 조사(§9)에서 왔다.

| 안 | 내용 | 기대 이득 (333 MHz 대비) | 근거 | 비용 / 위험 |
|---|---|---|---|---|
| A | 지금 HLS GEMM 그대로, 클럭만 360~380 MHz + HP 포트 333 MHz 분리 | 클럭 +8~14%, EVTA 시간 −7~12% (보드 측정) | §8 3)~6): base가 380까지 timing 통과, single/multi2/multi4 보드 golden PASS. WPWS 위반은 HP 분리로 해결 중 | 작다. `vivado.tcl`만. multi4 380(HP 분리) 빌드 중 |
| B | GEMM을 systolic RTL로(§5), 같은 클럭 1개로 400 MHz | 클럭 +20% | §8 3): 400 MHz 실패 경로 상위 200 중 169개가 GEMM 어레이(inp broadcast, wgt→DSP, 덧셈 트리)이고 systolic에서 없어진다. §10 모델: drain 생략 시 가동률 99.6~100% | 크다. RTL, cosim, acc를 compute 밖으로. 400을 맞추려면 load 쓰기, out_mem, AXI FIFO, reset 복제도 고쳐야 한다 |
| C | B + 어레이만 2배 클럭(DPU 방식, 목표 ~600 MHz), 나머지 300~333 MHz | GEMM 처리량 최대 2배(같은 256 MAC) | §9.12: DPU 325/650 MHz, DSP-Fetch 666 MHz(OOC), **보드 시스템**: FireFly v2 ZU3EG 300/600·ZU7EV 250/500, U50 300/600. HP 대역: EVTA당 1.66 GB/s(vta_synth_notes §6.2) x2 = 3.3 GB/s < HP 128비트 x 333 MHz = 5.3 GB/s | B 위에 2:1 폭 변환과 클럭 경계. 큰 ZU+(5EV/7EV)는 보드에서 500으로 내려갔다(§9.5). GEMM 가동률이 낮은 conv(Conv4 73%)는 load/store가 막아 2배가 안 된다 |
| D | INT8 2-MAC packing(DSP 하나에 같은 입력을 쓰는 곱 2개, WP486) | DSP 수 절반, 또는 같은 DSP로 MAC 2배 | §9.8 | MAC을 늘리려면 BLOCK_OUT 32 → ISA·`hw_spec.h`·컴파일러 변경. 지금 DSP는 모자라지 않다 |
| E | 어레이 32x32(MAC 4배) | 이론 4배 | §6 | ISA BLOCK, 레이아웃, NEST-C 스케줄, SRAM 폭 4배. multi4는 이미 BRAM 556/912. 가장 큰 작업 |

**전체 속도에 대한 주의**: 위 이득은 EVTA 시간(VTA_SYNC)의 이득이다. Resnet18 같은 실제 네트워크는 CPU 연산(conv0, transpose, elemadd)이 큰 몫이고,
4 copy 동시 실행은 CPU-bound다(vta_synth_notes §6.5, 609 GOPS 보고서). 그래서 end-to-end 이득은 EVTA 몫만큼만 나온다. conv 사이 CPU 간격 숨기기(vta_synth_notes §6.5, 4-EVTA 약 +4.6%)와
CPU 연산을 EVTA로 옮기는 일은 GEMM 구조와 별개로 남는다.

**결정(제안)**:
1. **A를 먼저 끝낸다.** 비용이 거의 없고 보드에서 이미 동작한다. HP 분리 판(multi4 380 → single, multi2)이 WPWS ≥ 0이고 보드 PASS면 기본 비트스트림 후보로 둔다.
2. **B를 Phase D로 진행하되, C로 갈 수 있게 만든다.** 어레이 입출력을 레지스터 경계와 valid/태그 스트림으로만 두면(§5 그대로) 나중에 2:1 변환만 넣어 어레이 클럭을 분리할 수 있다.
   B만으로는 +20%(400 MHz)이고, 그 값도 GEMM 밖 경로(load 쓰기, out_mem, AXI, reset)를 같이 고쳐야 나온다. 투자 대비 이득의 대부분은 C에서 나온다.
3. **D, E는 지금 하지 않는다.** ISA와 컴파일러가 바뀌는 별도 프로젝트다. C 뒤에도 GEMM이 병목이면 다시 본다.

**결정 전에 확인할 것(아직 모름)**:
- C의 우리 보드 클럭: 보드 시스템 사례는 ZU3EG 600, ZU5EV/7EV 500, U50 600 MHz다(§9.5, §9.6). ZU9EG -2의 DSP Fmax(DS925)와, y219 fence 안 DSP 열 배치에서 몇 MHz가 나오는지는 Phase D 어레이 OOC 합성으로 먼저 잰다.
- Conv4형(GEMM 73%)처럼 load/store가 GEMM 뒤에 숨지 않는 conv의 비율: vta_synth_notes §6.2 `_timed` 순위로 Resnet18 conv 시간 중 GEMM 가동률이 낮은 conv의 몫을 계산한다.
- 조사에서 못 본 수치: Gemmini의 FPGA 클럭(PMC 2023 Gemmini FPGA 연구 본문 미확인), AutoSA·Tensil의 클럭(§9.13).

## 1. 제안된 구조

사용자 제안(16x16 어레이, PE(ic, oc)):
- **입력**: 1x16 입력 벡터를 길이가 다른 shift register로 넣는다. 위 행부터 한 클럭씩 늦게 들어간다(skew).
- **weight**: double register를 쓴다. 다음 16x16 블록을 위에서부터 한 행(16 x int8 = 128비트)씩 shift해 16클럭에 채운다.
  채워지면 한 클럭에 전체가 계산용 레지스터로 넘어간다.
- **출력**: 열(oc)마다 결과가 나오는 시각이 다르다. 열마다 다른 acc bank로 들어가게 한다.

## 2. 결론 요약

- **ISA와 컴파일러(NEST-C)는 바꿀 필요가 없다.** GEMM 명령의 의미("uop 하나 = `acc[dst] += inp[src](1x16) · wgt[wgt](16x16)ᵀ`",
  `vta-hw-zcu102-*/hardware/xilinx/src/vta.cc` `gemm()` 236행)를 HW가 그대로 지키면 된다.
  - 바뀌지 않는 것: `hw_spec.h`, 명령·uop 포맷, `VTASave.cpp`/`VTA.cpp`(NEST-C는 `convolution_wo_tr(...)` 호출과 tile 인자만 만든다),
    vtalib 시뮬레이터(golden 기준으로 그대로 씀).
  - tile 스케줄(`include/Backends/VTA/VTASchedules.h`)은 성능 때문에 다시 맞출 수 있다(필수 아님).
- **런타임은 uop 순서를 반드시 바꿔야 한다**(성능). vtalib `lib/Bundle/VTABundle.cpp`의 `gemmop_mxn`(132행), `gemmop_1x1`(156행) 두 함수다.
- **HW가 큰 일이다**: deskew와 acc bank, acc hazard, 명령 경계의 파이프라인 비우기, HLS가 아닌 RTL.

## 3. 지금 HW가 GEMM을 하는 방식

- `vta.cc` `gemm()`: `iter_out` x `iter_in` x uop(`uop_bgn..uop_end`) 루프에 `#pragma HLS PIPELINE II = 1`.
  - 사이클마다 `wgt_mem`에서 16x16 블록 전체(2048비트), `inp_mem`에서 1x16, `acc_mem`에서 1x16 int32를 읽는다.
  - 256개 곱과 열마다 16입력 합을 계산해 acc와 out에 쓴다.
  - 주소는 uop의 dst/src/wgt + 루프 factor(`dst/src/wgt_factor_out/in`)다.
- 그래서 사이클마다 weight 블록이 바뀌어도 속도가 같다. 지금 ResConv10의 GEMM 가동률은 93.5%다(vta_synth_notes §6.2).
- zcu102 SRAM bank(`pkg_config.py`): 모든 bank가 128비트다. wgt 원소 하나 = 16 bank x 128비트, acc = 4 x 128비트.

## 4. 런타임: uop 순서 (vtalib `gemmop_mxn`, `gemmop_1x1`)

**지금 순서** (`gemmop_mxn`, 빠르게 도는 것이 아래):

```
it_out (VTAUopLoopBegin(tile_w, dst 1, src stride_w, wgt 0)) : 출력 x
  it_in (VTAUopLoopBegin(KW, dst 0, src 1, wgt 1))           : 커널 kx
    uop: for ky(KH) for kernelIdx(n_kernel_) for y(tile_h)    : 출력 y가 가장 안쪽
         dst = out_base + (kernelIdx·tile_h + y)·tile_w
         src = in_base + (y·stride + ky)·in_tile_w
         wgt = wgt_base + ky·KW + kernelIdx·KH·KW
```

- 같은 weight가 연속으로 쓰이는 사이클 수 = `tile_h`다. ResConv10은 7, Conv4는 8이다.
  - weight 블록 하나는 conv 전체에서 49번(7x7 출력) 쓰이지만 7사이클씩 끊겨 7번 나뉜다.
- weight-stationary에서는 weight를 채우는 데 16클럭이 든다. 그래서 **같은 weight로 16사이클 이상 계산해야 다음 weight 채우기가 숨겨진다.**
  지금 순서 그대로면 7사이클 계산 후 9사이클을 기다리므로 가동률이 약 7/16 = 44%다.
- `gemmop_1x1`도 같다: `it_out` = x, `it_in` = n_kernel(wgt +1, dst +tile_w·tile_h), uop = y. 연속 = `tile_h`.

**바꿀 순서** (weight 고정, 출력 픽셀 전체가 안쪽):

```
it_out (VTAUopLoopBegin(n_kernel_, dst tile_h·tile_w, src 0, wgt KH·KW)) : 출력블록(kernelIdx)
  it_in (VTAUopLoopBegin(KW, dst 0, src 1, wgt 1))                       : 커널 kx
    uop: for ky(KH) for y(tile_h) for x(tile_w)                           : 픽셀이 가장 안쪽
         dst = out_base + y·tile_w + x
         src = in_base + (y·stride + ky)·in_tile_w + x·stride
         wgt = wgt_base + ky·KW
```

- 같은 weight가 `tile_h·tile_w` 사이클 연속으로 쓰인다. ResConv10은 49, Conv4(8x56)는 448이다.
- uop 수는 KH·tile_h·tile_w다. ResConv10은 147, Conv4는 1,344로 uop 버퍼(8192)에 충분히 들어간다.
  - 다만 uop을 DRAM에서 읽어 오는 양이 늘어난다. `VTAPushGEMMOp`의 uop 캐시(`vGemmUOpHandle`)로 두 번째 호출부터 다시 만들지는 않지만,
    uop SRAM으로 load되는 양이 tile마다 늘어나는지는 확인해야 한다.
- 같은 `dst`가 다시 오는 간격은 `tile_h·tile_w` 사이클(≥ 49, 최소 tile 7x7)이다. 이것이 §5 hazard 조건과 맞물린다.
- **정확성 검증은 지금 비트스트림에서 먼저 할 수 있다.** 정수 누산의 순서만 바뀌므로 출력이 비트 단위로 같아야 한다.
  - 시뮬레이터 빌드 → 지금 보드(single, multi4)에서 `run_all.sh` → ResConv10/Conv4 시간이 거의 같은지 본다. 한 명령은 vta_synth_notes §2.5 7)(Phase B, §10).
  - 지금 HW는 순서와 상관없이 사이클당 1 GEMM이므로 시간이 거의 같아야 한다.
- 같은 함수를 쓰는 곳: `conv_kernel`(548/550행)과 `conv_kernel_scale`(933/935행)이 `gemmop_mxn`/`gemmop_1x1`을 쓴다.
  - `reset_acc`(89행, GEMM `reset_reg`)는 weight를 쓰지 않으므로 영향이 없다. HW에서 reset 경로는 따로 두면 된다.
  - XP-GEMM/BNN 경로는 이 비트스트림에서 이미 쓸 수 없다(vta_synth_notes §3.2 #6).

## 5. HW 세부 설계 (2026-10-27 개정, §8 Phase A·§9 조사 반영)

처음 안(열별 acc bank, BCIN shift로 16클럭 채우기)을 조사 뒤 아래처럼 바꿨다. 바뀐 이유는 항목마다 적었다.

**배치** — PE(i, j): i = ic(행), j = oc(열). 열 j = DSP48E2 16개를 PCIN/PCOUT로 cascade한 한 DSP 열.
- 부분합 Σ_i x[i]·w[j][i]는 열 안에서 P cascade로 흐른다. 지금 HLS의 16입력 덧셈 트리(400 MHz에서 최악, §8)가 없어진다.
- ZU9EG의 DSP cascade는 한 DSP 열 안에서 세로로만 된다(ACIN/BCIN/PCIN 모두 세로). y219 fence 안에 DSP48E2_X0..X17 x Y0..Y85가 있어 16개 cascade 열 16개가 들어간다.
- 곱셈 int8 x int8, P는 48비트. 16항 합은 int21이면 충분하다. INT8 2-MAC packing(DSP 반)은 나중 선택.

**입력 x (A 포트)** — fanout 1 경로로 바꾸는 것이 핵심(§6, §8).
- inp_mem 한 행(128비트) → 행 i는 i클럭 지연(skew FF) → 행 i 안에서 열 0→15로 FF 한 칸씩(가로, fabric FF, fanout 1).
  열 j의 행 i는 픽셀을 t+i+j에 받는다. A cascade는 세로라 가로 전달에 쓸 수 없다.
- FF: skew 8x(0+…+15)=960 + 가로 16x16x8=2048. AREG=1이면 가로 한 칸을 DSP A 레지스터가 대신한다.

**weight (B 포트)** — UG579로 확인(§9.3): BREG=2, BCASCREG=1이면 BCOUT은 B1에서 나가고 곱셈기는 B2를 쓴다(INMODE[4]=0). CEB1/CEB2는 따로다.
- B1 = prefetch chain(BCIN으로 위 행에서 한 칸씩), B2 = 계산 중 고정. 2409.03508의 "DSP-Fetch"가 정확히 이 구조다(ZU3EG 14x14, 666 MHz).
- **CE는 wavefront로 늦춘다.** PE(i, j)는 픽셀을 t+i+j에 받으므로 weight 교체(CEB2)도 PE(i, j)에서 T+i+j에 일어나야 한다.
  - CEB1을 한꺼번에 켜면 아래 행이 B2로 넘기기 전에 B1이 덮인다. 그러면 다음 채우기를 T+15 이후에야 시작할 수 있어 주기가 31클럭이 된다.
  - CEB1을 **PE(i, j)에서 T+i+j부터 T+15+j까지** 켠다. 시작은 wavefront로 늦추고, 끝은 열 안의 모든 행이 같다.
    PISO는 T+j부터 깊은 행의 값부터 넣는다. 그러면 **weight 주기 16클럭**이 된다.
    - 시작만 늦추고 끝도 늦추면 모든 행이 같은 값을 받는다.
    - 확인: 한 열의 비트 정확 모델로 주기 16, 17, 20은 오류 0/128, 15는 7/128이었다(2026-10-27).
  - CE 신호는 대각선 shift register(행 +1, 열 +1)로 만든다. Gemmini가 propagate 비트를 데이터와 같이 skew하는 것과 같다.
- chain 입구: 열 j 맨 위에 128비트 PISO(병렬 로드, 8비트씩 16클럭 shift). wgt_mem bank j(128비트) = w[j][0..15]다.
  2048비트 wgt 읽기는 weight마다 한 번(≥16클럭에 한 번)이다. BRAM→PISO는 fanout 1이라 레지스터를 더 넣어도 된다.
- 대안(B 포트 병렬 로드, chain 없음)도 주기 16이 되지만 BRAM→DSP 256개 거리 경로(지금 wgt→DSP 0.06~0.16 ns)가 남는다. chain을 기본으로 한다.
- weight 주소 비교: RTL이 uop의 wgt 주소를 지금 채운 것과 비교한다. 바뀌었는데 16클럭이 안 지났으면 uop 스트림을 멈춘다(AXIS backpressure).

**출력 deskew와 acc** — 열별 acc bank 안을 버리고 Gemmini 방식(출력 쪽 RMW)으로 바꿨다.
- 열 j의 합은 열 위쪽에서 t+15+j+(DSP 지연)에 나온다. 열 j를 15−j클럭 늦추는 deskew FF(약 120x24비트)로 행을 맞춘다.
- 맞춰진 16 x int32 행 + dst 태그(같은 지연의 태그 pipe) → **RMW 단계**: acc[dst] 읽기 → 16 덧셈 → acc[dst], out_mem[dst](하위 8비트) 쓰기.
- 이유:
  1. acc를 512비트 한 행 그대로 둘 수 있다. ALU·STORE·bias load의 접근 방식이 지금과 같다.
  2. hazard 창이 systolic 깊이(~50)가 아니라 RMW 깊이(BRAM 2 + 덧셈 1 + 쓰기, 약 4클럭)다.
- hazard: RMW 안 in-flight dst(≤4개)와 비교해 forwarding한다. 어레이는 멈출 수 없으므로 Gemmini처럼 stall하지 않는다.
  §4 순서면 같은 dst 간격이 ≥49라 forwarding이 쓰일 일은 없지만 정확성을 순서에 맡기지 않는다.
- `reset_reg` GEMM: 태그에 reset 비트를 싣고 같은 pipe로 보낸다. RMW가 0을 쓴다. 순서는 자동으로 지켜진다.

**HLS와 RTL의 경계** (§9.1)
- 지금 `acc_mem`은 HLS compute 안의 4096 x 512 true dual-port RAM(BRAM36 114개, 두 포트 모두 HLS가 씀)이다. 이것을 compute 밖으로 꺼낸다.
  - BD에 512비트 2포트 RAM. 포트 0, 1 모두 `gemm_busy`로 RTL(RMW 읽기/쓰기)과 HLS(ALU, bias load, 지금 그대로) 사이를 mux한다.
  - GEMM과 ALU는 compute 안에서 명령 단위로 순서대로 돌므로 동시 접근은 없다. HLS는 GEMM 명령을 내고 done을 받을 때까지 acc를 건드리지 않는다.
  - HLS 쪽은 `INTERFACE bram port = acc_mem` + `RESOURCE core = RAM_T2P latency = 2`. 모양은 inp_mem과 같다. 확인할 것: 2포트 외부 bram 인터페이스와 `DEPENDENCE` pragma가 HLS 2020.1에서 같이 되는지.
- out_mem 포트 A: RTL(GEMM 결과)과 HLS(ALU 결과)를 같은 `gemm_busy`로 mux한다.
- inp_mem·wgt_mem 포트 B: RTL만 쓴다(ALU는 inp/wgt를 읽지 않음).
- HLS → RTL: 지금 GEMM 루프(II=1)가 계산 대신 uop 스트림 {dst, src, wgt, reset, last}를 AXIS로 내보낸다. RTL → HLS: done 1개.
- 명령 경계: RMW가 순서대로라 연속한 GEMM 명령 사이에는 drain이 필요 없다.
  - ~~처음 구현은 단순하게 GEMM 명령마다 done을 기다린다~~ → §10 모델: 명령마다 기다리면 ResConv10형이 88%로 떨어진다.
    처음부터 ALU/STORE 토큰 앞에서만 기다리고, 다음 GEMM 명령의 첫 weight를 미리 채운다.

**HLS vs RTL**: 어레이, skew/deskew, CE wavefront, PISO, RMW, mux는 RTL. 큐, uop decode, ALU, 토큰은 HLS 그대로.
**검증**: §7 3단계 그대로(C 사이클 모델 → RTL 단위 TB → cosim → `gemmprobe`/`rawtest*` → `run_all.sh`).

## 6. 얻는 것과 잃는 것

- **MAC 수는 그대로다.** 사이클당 256 MAC(16x16)은 지금과 같다. ResConv10은 이미 GEMM 가동률 93.5%다.
  그러니 이득은 거의 **클럭(타이밍)**에서만 나온다.
  - 입력 broadcast(1x16을 16열 DSP 모두로)와 16입력 덧셈 트리가 이웃 PE 연결로 바뀐다. 배선이 짧아진다.
  - 어레이를 32x32로 키울 때(MAC 4배) 확장이 훨씬 쉽다. 그때는 ISA의 BLOCK 크기, `hw_spec.h`, 레이아웃, 컴파일러까지 바뀐다(별도 작업).
- **타이밍 근거 (확인한 것)**: multi4 rs_rst(333 MHz, `vta-hw-zcu102-multi4/build/y219/rs_rst.timing.rpt`)의 worst setup 경로다.
  - `inp_mem_3_0` BRAM → `compute_3/inst/add_ln700_42_reg` DSP 입력
  - slack +0.008 ns, 데이터 경로 2.500 ns(배선 65%), logic level 0
  - 즉 **입력 SRAM에서 GEMM DSP로 가는 broadcast 경로가 critical**이다. systolic의 skew shift register가 정확히 이 경로를 끊는다.
  - 이 리포트는 그룹마다 최악 경로 하나만 담고 있다. 할 일: `report_timing -max_paths 100 -group clk_out1_vta_pll_clk_0`로
    상위 경로 중 GEMM(compute 안 DSP/덧셈 트리) 비율을 본다. single(`vta-hw-zcu102-single/build/.../export/*.timing.rpt`)도 본다.
- **잃는 것**: weight 재사용이 적은 연산은 느려진다.
  - weight 블록마다 픽셀이 16개보다 적은 연산(GEMV, 출력이 아주 작은 1x1 conv)은 16클럭을 채우고 몇 사이클만 계산한다. 최악이면 1/16이다.
  - 지금 번들에서 FC는 VTA를 쓰지 않는다. `fullyconnected()`는 CPU 함수다(`CPUBundle_aarch64.cpp` 4098행).
    vtalib에서 GEMM을 내는 곳은 `conv_kernel`/`conv_kernel_scale`뿐이다(`VTAPushGEMMOp` 6곳).
    conv는 최소 tile 7x7(`MIN_H_SIZE/MIN_W_SIZE`, 작으면 더미로 채움)이므로 weight당 ≥ 49사이클이 보장된다.
  - 나중에 FC를 VTA로 보낼 때는 broadcast 경로를 남기거나 batch를 모아야 한다.

## 7. 다음 세션의 시작 순서

1. **타이밍 근거 확인 (HW 작업 전에)** — 2026-10-27 완료, Phase A 클럭 sweep과 360 MHz 보드 검증까지 §8
   - rs_rst DCP(`build/y219/`)를 열어 `report_timing -max_paths 100`을 본다. 명령은 §8 1) 아래.
   - 333 MHz에서 GEMM 경로가 상위를 차지하는지, systolic으로 몇 MHz를 기대할 수 있는지 판단한다.
   - GEMM이 병목이 아니면 클럭 이득이 작다. 그때는 이 작업의 가치를 다시 판단한다.
2. **런타임 uop 순서 변경**
   - `gemmop_mxn`/`gemmop_1x1`을 §4 순서로 바꾼다. `#### JO` 표시를 달고 원래 코드는 주석으로 남긴다.
     원본 `vta/vtalib`와 복사본 3개를 같이 맞춘다(vta_synth_notes §3.4 #1의 비교 규칙).
   - 지금 비트스트림에서 golden과 시간을 확인한다: `run_all.sh`(single, multi2, multi4)와 ResConv10/Conv4 `_prof` VTA_SYNC. 명령은 vta_synth_notes §2.5 6)·7).
   - 이 단계만으로도 HW 교체 전에 런타임 쪽 검증이 끝난다.
3. **HW GEMM 교체**
   - HLS C 모델(`vta.cc` `gemm()`과 같은 결과를 내는 사이클 모델)을 먼저 만든다.
   - 그다음 RTL과 cosim, single 비트스트림(fence `y219`)에서 시작해 multi2, multi4(재배선 흐름) 순서로 간다.
4. 성능 비교
   - 같은 측정(vta_synth_notes §6.1, `OMP_NUM_THREADS=1`, conv 반복, VTA_SYNC)으로 클럭과 GOPS를 비교한다.

## 8. Phase A: 타이밍 근거와 파이프라인만으로 갈 수 있는 클럭 (2026-10-27)

작업 위치: 브랜치 `systolic`, `vta-hw-zcu102-systolic`(single 복사본). 아래 결과를 다시 내는 명령은 vta_synth_notes §2.5에 있다. 스크립트와 결과: `vta-hw-zcu102-systolic/hardware/xilinx/scripts/timing/`(`top_paths.tcl`, `y219pips.tcl`), 결과 `build/timing/*.paths.csv`, 빌드 로그 `build/phaseA/`.

**1) 지금 333 MHz의 critical path** (`report_timing -max_paths 1000 -nworst 1`, routed DCP)

실행 위치: 호스트, `$N/vta-hw-zcu102-systolic/build/timing`(결과 `<name>.paths.rpt`, `<name>.paths.csv`도 여기):
```bash
cd $N/vta-hw-zcu102-systolic/build/timing
T=../../hardware/xilinx/scripts/timing/top_paths.tcl
SD=$N/vta-hw-zcu102-single/build/hardware/xilinx/vivado/zcu102_1x16_i8w8a32_16_16_19_18/vta.runs/impl_1/vta_wrapper_routed.dcp
MD=$N/vta-hw-zcu102-multi4/build/y219/rs_rst.dcp
docker exec -u ubuntu -w $PWD vivado2020-build bash -c "$VIV >/dev/null; vivado -mode batch -nojournal -log single.log -source $T -tclargs $SD single > /dev/null 2>&1 & \
  vivado -mode batch -nojournal -log multi4.log -source $T -tclargs $MD multi4 > /dev/null 2>&1 & wait"
sort -t, -k1 -g single.paths.csv | head      # slack, logic level, datapath, 배선 %, start, end, ...
```
- single(10-23 y219 pass)과 multi4(rs_rst) 모두 상위 1000 경로가 slack 0.23 ns 안에 몰려 있다(slack wall).
- 상위 100개: single 100/100, multi4 96/100이 `inp_mem` BRAM → GEMM DSP A 입력이다.
  - logic 0, BRAM 출력 레지스터 없음, **fanout 346**, 데이터 경로 2.35 ns(배선 61%). §6의 가설이 맞다.
- 그 밖의 경로 종류별 최악 slack:

| 경로 | single | multi4 |
|---|---|---|
| inp_mem → DSP | 0.004 | 0.008 |
| load → inp/wgt BRAM 쓰기 | 0.150 | 0.017 |
| wgt_mem → DSP B | 0.161 | — |
| compute 덧셈 트리 (8 level) | 0.172 | 0.119 |
| reset fanout | — | 0.097 |

**2) 빌드 옵션** (커밋 `ab32e110`, 기본값이면 빌드는 이전과 같다). systolic 트리 `vta-hw-zcu102-systolic/hardware/xilinx`의 make 변수다(vta_synth_notes §2.5 1)).
- `make ... MEM_OREG=1`: inp/wgt BRAM 포트 B(compute 읽기)에 출력 레지스터를 켜고, HLS에 read latency 2를 알린다(`vta.cc` `#ifdef VTA_MEM_OREG`).
  - GEMM 루프는 II=1, 깊이 12다. 기준은 깊이 11이다(보드의 10-23 single IP `hls/<CONF>`와 오늘 base `_prof` IP 모두 wgt 16 bank x 128비트).
    acc read→write 거리는 약 3사이클 그대로라(ST_45 load, ST_47~48 store) 새 hazard는 없다.
    - 주의: single `hls/<CONF>_prof`(10-20)는 wgt 2 x 1024비트인 옛 구성이라 비교 기준이 아니다.
- `make ... FREQ=<MHz>`: PLL만 바꾼다. IP는 그대로 쓴다. 빌드 dir 이름은 `<CONF>_prof[_oreg][_<FREQ>mhz]`.

**3) 클럭 sweep** (single, y219 fence, `PROFILE=1`, 기본 implementation strategy)

실행 위치: 호스트. make는 docker 안 `$N/vta-hw-zcu102-systolic/hardware/xilinx`에서, 로그는 `$N/vta-hw-zcu102-systolic/build/phaseA/`:
```bash
cd $N/vta-hw-zcu102-systolic/hardware/xilinx; L=$N/vta-hw-zcu102-systolic/build/phaseA; mkdir -p $L
# IP 먼저 (base, oreg)
docker exec -u ubuntu -w $PWD vivado2020-build bash -c "$VIV >/dev/null; make ip PROFILE=1 > $L/ip_base.log 2>&1; make ip PROFILE=1 MEM_OREG=1 > $L/ip_oreg.log 2>&1"
# 클럭마다 bin (동시에)
docker exec -u ubuntu -w $PWD vivado2020-build bash -c "$VIV >/dev/null; for f in 360 380 400; do \
  make bin PROFILE=1 FREQ=\$f JOBS=8 > $L/bin_base_\$f.log 2>&1 & make bin PROFILE=1 MEM_OREG=1 FREQ=\$f JOBS=8 > $L/bin_oreg_\$f.log 2>&1 & done; wait"
for v in prof_360mhz prof_380mhz prof_400mhz prof_oreg_360mhz prof_oreg_380mhz prof_oreg_400mhz; do
  echo "$v $(grep -A3 'WNS(ns)' ../../build/hardware/xilinx/vivado/zcu102_1x16_i8w8a32_16_16_19_18_$v/export/vtahw_zcu102_1x16_i8w8a32_16_16_19_18_$v.timing.rpt | sed -n 3p | awk '{print "WNS",$1,"WHS",$5,"WPWS",$9}')"; done
# 실패한 판의 경로 종류: 1)의 top_paths.tcl을 <BUILD>/vta.runs/impl_1/vta_wrapper_routed.dcp에 돌린다 (build/timing에서, 이름 예: prof_400mhz)
```

| 변형 | 333 | 360 | 380 | 400 |
|---|---|---|---|---|
| base (지금 구조) | +0.004 (10-23판) | **+0.011** | **+0.007** | −0.106 (852 실패) |
| oreg | +0.026 | **+0.041** | −0.007 (12 실패) | −0.162 (653 실패) |

- **base도 360, 380 MHz를 통과한다.** slack wall은 placer가 목표에 맞춰 멈춘 모양이지 구조적 한계가 아니다.
- 이 범위에서는 oreg 효과보다 P&R 편차(±0.05 ns 정도)가 크다. 380에서 base +0.007, oreg −0.007이다. 출력 레지스터만으로는 이득이 없다.
- base 400 실패(상위 200): wgt_mem → DSP B 99, 덧셈 트리(DSP_OUTPUT → FDRE) 45, inp_mem → DSP A 25, compute 내부 FF 17, load → inp_mem 5.
  - **GEMM 어레이 경로가 169/200이고, 셋 다 §5 구조에서 없어진다**(wgt는 PISO + B1 chain, inp는 fanout 1, 합은 P cascade).
  - 그룹 최악: load → inp_mem −0.106(6 level), compute FF −0.102, wgt → DSP −0.098, axi_xbar → store −0.083, g2l_queue → compute −0.080, reset −0.068.
- oreg 380 실패: reset fanout(`proc_sys_reset` → fetch regslice) 73/200, inp broadcast 71/200, acc_mem 쓰기, load → inp_mem.
- oreg 400 실패의 종류(상위 200 중 개수, 그룹 최악 slack):

| 경로 | 개수 | 최악 slack | systolic으로 없어지나 |
|---|---|---|---|
| inp_mem → DSP (출력 레지스터를 켜도 배선 88%, 1.88 ns) | 80 | −0.130 | **예** (fanout 1) |
| compute 덧셈 트리 (DSP_OUTPUT → FDRE, 8 level) | 42 | −0.162 | **예** (P cascade) |
| compute → out_mem 쓰기 | 10 | −0.151 | 아니오 (RTL RMW로 옮기면 손볼 수 있음) |
| load → wgt/inp BRAM 쓰기 | 7 | −0.146 | 아니오 |
| fetch → axi_xbar FIFO, axi_xbar 내부 | — | −0.137 | 아니오 |
| reset fanout | 17 | — | 아니오 |

- 해석:
  - BRAM 출력 레지스터는 BRAM Tco만 줄인다. fanout 346 배선은 그대로라 inp broadcast가 400 MHz에서도 상위에 남는다. 구조(fanout)를 바꿔야 끊긴다.
  - systolic은 400 MHz 실패 경로 중 가장 많은 두 종류(상위 200 중 122개, 최악 −0.162)를 없앤다.
  - 400 MHz를 맞추려면 load 쓰기, out_mem 쓰기, fetch/AXI FIFO, reset 복제도 같이 고쳐야 한다(HLS pipeline/regslice, `proc_sys_reset` 복제).
- 기대 이득: 지금 구조로 360~380 MHz(333 대비 +8~14%, 360은 보드 확인). systolic과 주변 정리(load 쓰기, out_mem, 큐/AXI, reset)로 400 MHz(+20%). 그 위는 어레이만 2배 클럭(§9.12).
**4) 360/380 MHz 보드 검증** (2026-10-27, `bit/evta_single_phaseA_261027/`, `/lib/firmware`에 같은 이름)
- 비트스트림: `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_prof_360mhz`(base, md5 `15768d7c…`), `..._prof_oreg_360mhz`(oreg, md5 `b7a69d41…`),
  `..._prof_380mhz`(base, md5 `9884bb74…`).
- 호스트(vta_synth_notes §2.2): 세 판 모두 stuckchk 194, 0 extra. Y219 행 PIP net 0(`scripts/timing/y219pips.tcl`). 명령은 vta_synth_notes §2.2(export dir에서, `X=vtahw_zcu102_1x16_i8w8a32_16_16_19_18_prof_360mhz` 등).
- 보드로 복사: vta_synth_notes §2.5 4) (가), `D=/home/xilinx/JO/npuprg/bit/evta_single_phaseA_261027`.
- HW 프로브(`hwtest/*_nc`, 케이스마다 bitstream 재로드): 기준 `vtahw_single_y219.bin`과 세 판 모두 통과.
  보드 `cd $B/vtabundles_xcmpl/hwtest && sudo -n /bin/sh -c "NUM_EVTA=0 /bin/sh ./probes.sh vtahw_single_y219.bin vtahw_zcu102_1x16_i8w8a32_16_16_19_18_prof_360mhz.bin vtahw_zcu102_1x16_i8w8a32_16_16_19_18_prof_oreg_360mhz.bin vtahw_zcu102_1x16_i8w8a32_16_16_19_18_prof_380mhz.bin"`.
  gemmprobe reset/zero/ident/perm/ones 0/16 wrong, sramscan wgt/inp(+invert) 0 bytes wrong, donerace 16/16.
- 번들 golden(`hw/script/run_all.sh`, `BIN=<위>.bin`, 명령은 vta_synth_notes §2.5 6) (가)): 세 판 모두 30 PASS + golden 없는 5개 정상 실행(Resnet18/Partition/50 → 281, Vgg16 → 0, 이전과 같음).
- 속도(`hw/script/run_prof.sh`, `_prof` 번들 = `vta/vtalib` + `NESTC_EVTA_PROFILE`, total_vta_time, 2회 반복 편차 0.1% 이내).
  보드 `cd $B/vtabundles_xcmpl/hw && sudo -n /bin/sh -c "BIN=<x>.bin script/run_prof.sh ResConv10Test Conv4Test"`, 결과 `result/prof_summary.txt`:

| 번들 | 333 (`vtahw_single_y219`) | base 360 | oreg 360 |
|---|---|---|---|
| ResConv10 | 2.895 ms | 2.686 ms (−7.2%) | 2.683~2.686 ms (−7.3%) |
| Conv4 | 3.547 ms | 3.292~3.295 ms (−7.2%) | 3.293~3.294 ms (−7.2%) |

  base 380: ResConv10 2.545 ms(−12.1%), Conv4 3.124 ms(−11.9%). 클럭 비율 기대치 −12.4%.

  - 360 MHz의 클럭 비율 기대치는 −7.5%다. 시간이 클럭에 비례한다(DDR 병목 아님). oreg의 latency +1은 차이가 없다.
  - **지금 구조(base)로 380 MHz까지 보드에서 동작한다(+14% 클럭, VTA 시간 −12%).** single 기본 비트스트림으로 바꿀지는 multi2/multi4도 본 뒤 정한다.
  - 380 판은 `_prof`(profile 카운터 + APM) 빌드다. 배포판(profile 없음)은 같은 클럭으로 다시 만들어 확인해야 한다(placement가 달라짐).
- 보드 sudo: `xilinx`는 `/bin/sh`, `hwtest/*`, `hw/*`, `cp * /lib/firmware/*`만 비밀번호 없이 된다.
  환경 변수를 넘기려면 hw dir에서 `sudo -n /bin/sh -c 'BIN=<x>.bin script/run_all.sh'`.

**5) multi2 360/380 MHz** (2026-10-27, profile 없는 배포판, `vta-hw-zcu102-multi2` `make bin FREQ=<MHz>`, fence `y219`)
- 명령: 빌드 vta_synth_notes §2.5 1)(`T=multi2; F=360`, `F=380`), 검사 vta_synth_notes §2.2, 복사 vta_synth_notes §2.5 4) (가)(`D=/home/xilinx/JO/npuprg/bit/evta_multi2_phaseA_261027`),
  프로브 `NUM_EVTA=2`(vta_synth_notes §2.5 5)), 번들·속도는 보드 `cd $B/vtabundles_xcmpl/hw_multievtax2` 뒤 vta_synth_notes §2.5 6) (나)
  (속도: `sudo -n /bin/sh -c "OMP_NUM_THREADS=1 BIN=<x>.bin script/run_prof.sh MultiEVTAResConv10Test MultiEVTAConv4Test"`).
- 빌드: 360 WNS +0.003 / WHS +0.010, 380 WNS +0.001 / WHS +0.010. stuckchk 194, 0 extra. Y219 행 PIP net 0.
- 보드 `bit/evta_multi2_phaseA_261027/`(`/lib/firmware`에 같은 이름):
  `vtahw_zcu102_1x16_i8w8a32_16_15_18_17_2evta_360mhz.bin`(md5 `00c8202c…`), `..._380mhz.bin`(md5 `6cb08c59…`).
- HW 프로브(EVTA 0, 1 각각 `*_evta<e>_m`): 두 판 모두 gemmprobe 5개, sramscan 4개, donerace 16/16 통과.
- `hw_multievtax2/script/run_all.sh`: 두 판 모두 MultiEVTAConv4/ResConv10/x2Conv PASS, Resnet18 2개 x 100회 모두 281.
- 속도(`run_prof.sh`, `OMP_NUM_THREADS=1`, MultiEVTAConv4 total_vta_time): 333(`vtahw_multi2_y219`) 179.19 ms,
  360 165.97 ms(−7.4%), 380 157.34 ms(−12.2%). ResConv10은 `_prof`가 빌드돼 있지 않았다.
  run_all의 ResConv10 end-to-end(CPU transpose 포함, EVTA 2개 x 100회): 360 210.3 GOPS, 380 215.5 GOPS.
- **multi2도 380 MHz까지 보드에서 동작한다.**

**주의: 333 MHz 위의 판은 모두 PS8 HP 포트 규격을 벗어난다** (2026-10-27, 위 4)·5)를 확인할 때 WNS/WHS만 보고 놓쳤다)
- `report_timing_summary`의 Pulse Width: `PS8/SAXIGP<i>RCLK`, `WCLK` Min Period 요구 3.000 ns(333 MHz)다.
  확인: `grep -A3 'WNS(ns)' <x>.timing.rpt | sed -n 3p | awk '{print "WPWS",$9}'`, 어느 핀인지는 `grep -B2 -A12 'Pulse Width Checks' <x>.timing.rpt | grep -i saxigp`.
  360은 slack −0.222(2.778 ns), 380은 −0.368이다. single, multi2, multi4 판 모두 WPWS < 0이고, 333 판(3.003 ns)만 통과한다.
- 보드에서는 위 결과대로 동작했지만 규격 밖이다(온도, 칩마다 보장 안 됨). 기본 비트스트림으로 쓰면 안 된다.
- 고치는 방법: HP 포트(`saxihp<i>_fpd_aclk`)와 그 앞 register slice/SmartConnect를 333 MHz 이하의 별도 클럭으로 두고,
  EVTA 쪽 클럭과 AXI clock converter로 잇는다. HP 128 bit x 333 MHz = 5.3 GB/s라 EVTA 대역폭은 충분할 것이다.
  systolic의 400 MHz 목표(§0 B, C)도 같은 분리가 필요하다.

**6) multi4 360 MHz** (2026-10-27): `make bin FREQ=360`(fence `split`) 뒤 `run_flow.sh m360`.
- 명령: 빌드 vta_synth_notes §2.5 1)(`T=multi4; F=360`), 재배선 vta_synth_notes §2.5 2)(`F=360; TAG=m360`), 검사 vta_synth_notes §2.5 3) (나), 복사 vta_synth_notes §2.5 4) (나)
  (`D=/home/xilinx/JO/npuprg/bit/evta_multi4_phaseA_261027`), 프로브 `NUM_EVTA=4`, 번들·속도 vta_synth_notes §2.5 6) (나)(`hw_multievtax4`, `run_prof.sh` 인자 없음).
- `make bin`은 기본 impl의 write_bitstream DRC(RTSTAT-6 partial conflict 2개)로 실패하지만, flow는 `vta_wrapper_opt.dcp`에서 다시 시작하므로 상관없다.
- rs_m360: WNS +0.018 / WHS +0.009, WPWS −0.222(위 HP 포트). Y219 행 PIP net 0. 행 Y219의 모든 tile(CLE, BRAM, DSP, INT_INTF 포함)에서 PIP 0.
- stuckchk: **1 extra**(frame 24215, word 62 bit 14).
  - RapidWright `bitstream` API로 frame을 tile에 매핑하면 `INT_X35Y219` minor 23이다
    (호스트 `cd $N/vta-hw-zcu102-multi4/build/y219` 뒤 `RAPIDWRIGHT_PATH=$RW/d2025.2 java -Xmx16g -cp $RW/rapidwright-2025.2.2-standalone-lin64.jar ../../hardware/xilinx/scripts/y219route/StuckLoc.java rs_m360.bit $BASEBIT | grep 'cr X.Y3'`).
  - U-Ray segbits로는 PIP `INT_NODE_SDQ_77_INT_OUT0 -> NN4_W_BEG4`에 해당한다
    (`grep '^INT.PIP.NN4_W_BEG4.INT_NODE_SDQ_77_INT_OUT0 ' $N/vta-hw-zcu102-multi4/build/wiretest/uray/segbits_int.db`: 그 PIP를 켜는 `<minor>_<bit>` 목록, `!`는 0이어야 하는 비트. 이름 순서는 `INT.PIP.<도착>.<출발>`).
  - DCP에서는 이 PIP와 두 node에 net이 없다(reroute 전 routed_m360도 같다). DCP로 bit를 다시 써도 같은 bit가 1이다.
  - Vivado가 net 없는 node에 쓰는 설정으로 보인다(base 194개와 같은 종류). 보드에서 0으로 읽혀도 쓰지 않는 PIP가 꺼질 뿐이다.
- 보드 `bit/evta_multi4_phaseA_261027/vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta_360mhz.bin`(md5 `4347a2d6…`, `/lib/firmware`에 같은 이름).
- HW 프로브: EVTA 0~3 모두 gemmprobe 5개, sramscan 4개, donerace 16/16 통과.
- `hw_multievtax4/script/run_all.sh`(`OMP_NUM_THREADS=1`): Conv4, Conv, ResConv10 PASS. Resnet18 multi/single 100회 모두 281.
- 속도(`run_prof.sh`, total_vta_time, 333 = `vtahw_multi4_y219`):

| 번들 | 333 | 360 |
|---|---|---|
| ResConv10 | 146.42 ms | 135.56 ms (−7.4%) |
| Conv4 | 194.39 ms | 166.35 ms (−14.4%, 1회 측정이라 편차일 수 있음) |
| Conv | 54.18 ms | 50.90 ms (−6.0%) |
| Resnet18 multi | 582.09 ms | 539.29 ms (−7.4%) |
| Resnet18 single | 1303.72 ms | 1208.40 ms (−7.3%) |

- **multi4도 360 MHz에서 보드에서 동작한다.** 380은 HP 포트 규격 문제(위 주의)를 먼저 풀고 시도한다.
- `flow/place_d.tcl`의 입력 impl dir을 `VTA_IMPL_DIR`로 바꿀 수 있게 했다. `run_flow.sh`가 이 변수를 docker에 넘긴다.
  build/y219 기준 상대 경로: `VTA_IMPL_DIR=../hardware/xilinx/vivado/zcu102_1x16_i8w8a32_16_15_18_17_4evta_360mhz/vta.runs/impl_1`.

**7) multi4 380 MHz, HP 포트 333 MHz 분리** (2026-10-27, `vivado.tcl` `70ccf2f7`)
- 구조: EVTA 클럭(380)과 별도 MMCM `hp_pll`(333.0 MHz)을 두고, SmartConnect(`NUM_CLKS 2`)가 둘 사이를 넘긴다. `hpslice<i>`, `S_AXI_HP<i>_FPD`, APM slot 0은 HP 클럭.
  333 MHz 이하로 빌드하면(`FREQ`가 없거나 ≤ 333) 분리하지 않아 이전과 같다. `VTA_HP_FREQ`로 HP 클럭을 바꿀 수 있다.
- 흐름과 명령(작업 dir `$N/vta-hw-zcu102-multi4/build/y219`, `viv()`는 `vta_synth_notes` §3.3):
  1. `make bin FREQ=380 JOBS=8`(vta_synth_notes §2.5 1)). 360 때처럼 기본 impl의 write_bitstream DRC(RTSTAT-6)로 `make_rc=2`, 상관없다.
  2. `VTA_IMPL_DIR=../hardware/xilinx/vivado/zcu102_1x16_i8w8a32_16_15_18_17_4evta_380mhz/vta.runs/impl_1 ../../hardware/xilinx/scripts/y219route/flow/run_flow.sh m380`
     → rs_m380: WNS −0.046(2, `axi_xbar` → `fetch_2` 레지스터, 배선 91%), WHS −0.001(16), WPWS +0.001.
  3. `viv route_s_m380o ../../hardware/xilinx/scripts/y219route/flow/route_s.tcl rws_m380.dcp rs_m380o opt`(배선 뒤 phys_opt 3회) → WNS +0.021, WHS −0.001(16).
     `phys_opt_design -hold_fix`(rs_m380h)는 바뀐 것이 없었다.
  4. `viv holdnets_m380 ../../hardware/xilinx/scripts/y219route/flow/holdnets.tcl rs_m380o.dcp rs_m380n`: hold 위반 16개가 모두 `smc_2` FIFO의 같은 net 하나였다.
     그 net만 풀고 다시 배선 → **rs_m380n: WNS +0.021, WHS +0.001, WPWS +0.001**. Y219 행 PIP 0, 미배선 0.
  5. `python3 $N/vta-hw-zcu102-multi4/hardware/xilinx/scripts/bit2bin.py rs_m380n.bit rs_m380n.bin`, stuckchk 194 / **0 extra**.
- 클럭: EVTA 379.96 MHz(2.632 ns), HP 333.01 MHz(3.003 ns). 두 클럭 사이 168 경로씩은 setup만(datapath-only max delay, SmartConnect CDC) 여유 1.5 ns.
- 보드 `bit/evta_multi4_phaseA_261027/vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta_380mhz.bin`(md5 `91212da9…`, `/lib/firmware`에 같은 이름).
- HW 프로브(`probes_m4.sh`, EVTA 0~3): 40/40 통과(gemmprobe 5, sramscan 4, donerace 16/16 각각).
- `hw_multievtax4/script/run_all.sh`(`OMP_NUM_THREADS=1`): Conv4, Conv, ResConv10 PASS. Resnet18 multi(4 copy)/single 100회 모두 281(틀리면 "The maxIdx should be 281"을 찍는데 0줄).
- 속도(`run_prof.sh`, total_vta_time, 같은 날 333 MHz `vtahw_multi4_y219.bin`도 다시 잼):

| 번들 | 333 | 380 (HP 333) | 변화 |
|---|---|---|---|
| ResConv10 | 146.41 ms | 129.58 ms | −11.5% |
| Conv4 | 179.60 ms | 158.77 ms | −11.6% |
| Conv | 54.32 ms | 54.35 ms | 0% |
| Resnet18 multi | 582.03 ms | 516.57 ms | −11.2% |
| Resnet18 single | 1303.82 ms | 1157.39 ms | −11.2% |

  - 클럭 비율 기대치는 −12.4%다. 큰 conv와 Resnet18은 거의 클럭만큼 빨라졌다.
  - **Conv(MultiEVTAConvTest)는 그대로다.** 360 MHz(HP도 360) 때는 −6.0%였다. 이 번들은 load/store 몫이 커서, DMA가 HP 클럭(333)에 묶이면 EVTA 클럭을 올려도 줄지 않는 것으로 보인다.
    HP 포트를 규격 안에 두는 대가다. 확인하려면 VTA_SYNC 안 load/store 시간을 따로 봐야 한다(PROFILE=1 비트스트림).
  - 6)의 Conv4 333 값 194.39 ms는 한 번 잰 값이었고 오늘은 179.60 ms다. 360 MHz의 −14.4%는 그 편차 때문이다(오늘 기준이면 −7.4%).
- **multi4는 380 MHz에서 HP 규격을 지키면서 보드에서 동작한다.** 기본 비트스트림으로 바꿀지는 single, multi2도 HP 분리로 다시 만든 뒤 정한다.
- **ResConv10 conv만 반복**(609.7 GOPS 측정과 같은 방법, vta_synth_notes §6.1: `multi4resconv10Bundle <mask> 400 conv`, `OMP_NUM_THREADS=1`,
  실행마다 `load_bit.sh`로 재로드, 333과 380을 번갈아 3회, 2026-10-27):

| 비트스트림 | EVTA 4개 (mask 15) | EVTA 1개 (mask 1) | conv 한 번 (EVTA당) |
|---|---|---|---|
| 333 MHz `vtahw_multi4_y219` | 609.60, 609.67 GOPS (1회차 586.76: EVTA3만 1.576 ms로 느림) | 152.53~152.62 | 1.516 ms |
| 380 MHz (HP 333) | **685.07, 685.24, 685.06 GOPS** | 171.57~171.60 | 1.349 ms |
| 비 | **+12.4%** | +12.5% | −11.0% |

  - 모든 실행에서 4개 출력 모두 golden. 이론 최대(4 x 256 MAC x 2 x 379.96 MHz = 778.2 GOPS)의 88.0%.
  - 클럭 비율은 +14.0%(379.96 / 333.33)다. 이득이 그보다 작은 것은 conv 사이 CPU 간격(명령어 생성, 약 66 µs, vta_synth_notes §6.2)이 클럭과 무관하기 때문이다.
    333 MHz의 EVTA 시간 1.449 ms를 클럭 비율로 줄이면 1.271 ms, 여기에 CPU 간격을 더하면 1.338 ms로 측정값 1.349 ms와 거의 맞는다.
  - HP 클럭이 333으로 묶여도 ResConv10은 영향이 없다(GEMM 가동률 93.5%, load/store가 GEMM 뒤에 숨는다). Conv 번들이 안 빨라진 것(위 표)과 대조된다.

**8) single, multi2 380 MHz(HP 333 분리)와 기본 비트스트림 변경** (2026-10-28)
- single: `vta-hw-zcu102-systolic`을 `SYSTOLIC` 없이 빌드했다(HLS GEMM, 이 트리에 HP 분리와 `reg_dwc`가 있다).
  `make ip CSIM=1`, `make bin FREQ=380 JOBS=4 IMPL_STRATEGY=Performance_ExtraTimingOpt BUILD_TAG=Performance_ExtraTimingOpt`.
- multi2: `vta-hw-zcu102-multi4`를 `NUM_EVTA=2`로 빌드했다(같은 `vivado.tcl`, EVTA 2개면 fence `y219`). 명령은 위와 같고 `NUM_EVTA=2`를 더한다.
- 기본 구현 전략으로는 둘 다 setup이 모자랐다(single −0.043, multi2 −0.023, AXI 인터커넥트·폭 변환 FIFO). `Performance_ExtraTimingOpt`로:

| 판 | WNS | WHS | WPWS | BRAM / DSP | 검사 |
|---|---|---|---|---|---|
| single 380 | +0.041 | +0.010 | +0.001 | 258 / 268 | stuckchk 0 extra, Y219 PIP 0 |
| multi2 380 | +0.009 | +0.010 | +0.001 | 290 / 536 | stuckchk 0 extra, Y219 PIP 0 |

- 보드 `bit/evta_single_380MHz_pass_261028/vtahw_zcu102_1x16_i8w8a32_16_16_19_18_380mhz.bin`(md5 `9b3a1e02…`),
  `bit/evta_multi2_380MHz_pass_261028/vtahw_zcu102_1x16_i8w8a32_16_15_18_17_2evta_380mhz_hp333.bin`(md5 `2d9519dd…`). `/lib/firmware`에 같은 이름, README 있음.
  multi2의 `_hp333`은 10-27의 규격 밖 판(`..._2evta_380mhz.bin`, HP 380)과 구분하려고 붙였다.
- probes: single 10/10, multi2 20/20. golden: single 30 PASS(Resnet18/Partition/50 → 281, Vgg16 → 0), multi2 3 PASS, Resnet18 multi/single 100회 모두 281.
- 속도(`run_prof.sh`, total_vta_time, 같은 날 333 MHz와):

| 번들 | 333 | 380 (HP 333) | 변화 |
|---|---|---|---|
| single ResConv10 | 2.894 ms | 2.561 ms | −11.5% |
| single Conv4 | 3.547 ms | 3.134 ms | −11.6% |
| single Resnet18 | 48.375 ms | 42.944 ms | −11.2% |
| single Resnet50 | 252.154 ms | 246.106 ms | −2.4% |
| single Vgg16Cifar10 | 33.533 ms | 31.087 ms | −7.3% |
| multi2 Conv4 | 179.204 ms | 158.411 ms | −11.6% |
| multi2 Resnet18 x2 | 1184.843 ms | 1053.490 ms | −11.1% |

  Resnet50은 load/store 비중이 커서 HP 클럭(333)에 묶인다(systolic 400의 Resnet50 −6.8%와 같은 현상).
- **기본 비트스트림을 380 MHz로 바꿨다.** `vtabundles_xcmpl/hw*/script/run_all.sh`의 `BIN`과 `collect_hw_bundles.py`의 `SETS`:
  single `..._16_16_19_18_380mhz.bin`, multi2 `..._2evta_380mhz_hp333.bin`, multi4 `..._4evta_380mhz.bin`(7)).
  systolic 판은 `SYS_BIN`(single `..._16_16_19_18_sa_400mhz.bin`, multi4 `..._4evta_sa_400mhz.bin`)이다.
  `run_prof.sh -s _sys*`(vtalib-zcu102-systolic로 빌드한 실행 파일)는 `SYS_BIN`으로 돈다. `BIN=<file>`을 주면 그것이 우선이다.
  333 MHz 판(`vtahw_*_y219.bin`)은 `BIN=`으로 계속 쓸 수 있다.

## 9. 조사 보고서: 외부 설계·논문·문서 분석 (2026-10-27)

받은 원문은 `docs/ref/`에 있다(목록·라이선스·git 포함 여부는 `docs/ref/README.md`). 각 소스를 같은 틀로 정리했다:
무엇인가 → 구조 → 핵심 수치 → 우리 설계(§5)와 같은 점·다른 점 → 가져올 것 → 확인 못 한 것.
맨 끝 §9.12에 소스를 가로질러 비교한 표와 방향(§0)에 주는 결론을 모았다.

### 9.1 Gemmini (UC Berkeley, Chisel, BSD-3) — 가장 가까운 공개 WS 어레이

- **무엇**: RISC-V SoC(Chipyard)에 붙는 DNN 가속기 생성기. 논문 arXiv 1911.09925(DAC'21), 소스 `docs/ref/gemmini/*.scala`.
- **구조**
  - 기본 설정(`Configs.scala`): 16x16 mesh(`meshRows/meshColumns = 16`, `tileRows/tileColumns = 1`), 입력 `SInt(8)`, 누산 `SInt(32)`,
    **어레이 출력 `SInt(20)`**, scratchpad 256 KB, accumulator 64 KB, `dataflow = BOTH`(WS/OS 둘 다). `chipConfig`는 WS 전용, `largeChipConfig`는 32x32.
  - PE(`PE.scala`): 레지스터 `c1`, `c2` 두 개를 둔다. WS에서는 둘이 **weight double buffer**다. `propagate` 비트가 어느 쪽을 곱셈에 쓰고 어느 쪽을 `in_d`(위에서 내려오는 새 weight)로 채울지 고른다.
    `prop = PROPAGATE`이면 `c2`로 곱하고 `c1 := d`, 아니면 반대. 부분합은 `in_b → mac → out_b`로 세로로 흐른다(WS에서 `in_c := b`).
  - Tile/Mesh(`Tile.scala`, `Mesh.scala`): tile 안은 조합회로, tile 사이에 `ShiftRegister(…, tile_latency+1)` 파이프 레지스터. a(입력)는 가로, b(부분합)와 d(weight preload)는 세로.
  - MeshWithDelays(`MeshWithDelays.scala`): 입력 행렬을 `shifted()`로 skew(행 j를 `j*(tile_latency+1)` 지연)하고 출력을 deskew한다.
    `propagate`도 `ShiftRegister(in_prop, i*(tile_latency+1))`로 **데이터와 같은 대각선으로 skew**한다. 행렬마다 `matmul_id`(태그)를 흘린다.
  - AccumulatorMem(`AccumulatorMem.scala`): `acc_latency`(≥2) 깊이의 `pipelined_writes`로 RMW(읽어서 더해 쓰기). 같은 주소가 pipe에 있으면 `rmw_req.ready`를 내려 **stall**한다(forwarding 없음).
- **핵심 수치**(논문, ASIC): systolic 1.89 GHz vs vector(NVDLA형) 0.69 GHz(같은 공정), 면적은 16x16 어레이가 SoC의 11.3%, scratchpad 52.9%.
  ResNet50 22.8 FPS @1 GHz(im2col 블록 포함). **im2col이 없으면 host CPU가 end-to-end를 좌우하고, BOOM CPU로 바꾸면 2.0배**라고 보고한다.
- **같은 점**: 16x16 int8 WS, 세로 부분합, 가로 입력, skew/deskew, 태그 pipe, 출력 쪽 RMW accumulator. 어레이 출력 폭 20비트는 우리 계산(16항 합 int21, §5)과 거의 같다.
- **다른 점**
  - weight 교체: Gemmini는 PE의 c1/c2(fabric 레지스터)와 propagate 비트, 우리는 DSP48E2 내부 B1/B2와 CEB1/CEB2 wavefront(§5, §9.4). 같은 원리를 DSP 안으로 넣은 것이다.
  - hazard: Gemmini는 stall, 우리는 어레이를 멈출 수 없어 forwarding(§5).
  - 연산 단위: Gemmini 명령은 행렬(최대 16x16 x 16x16), 우리는 uop 하나가 1x16 · 16x16. 그래서 우리는 uop 순서(§4)로 weight 재사용을 만든다.
- **가져올 것**: propagate 비트를 데이터와 같은 대각선으로 흘리는 방식(우리 CE wavefront 생성기와 같은 구조), 태그 pipe, 출력 RMW.
  그리고 **"host CPU 부담이 end-to-end를 정한다"**는 결과는 우리 Resnet18이 CPU-bound인 것(vta_synth_notes §6.5)과 같은 현상이다. GEMM만 빠르게 해서는 전체가 안 빨라진다.
- **확인 못 한 것**: Gemmini의 FPGA 클럭. 2023 Sensors의 FPGA 구현 연구(PMC10007457)는 WS가 OS보다 약 3배 빠르다고 하지만 본문을 받지 못해 장치·클럭을 모른다.

### 9.2 VTA 원논문 (arXiv 1807.04188) — 우리 EVTA의 원형

- **무엇**: TVM VTA의 HW/SW 설계 논문. EVTA는 이것의 ETRI 변형이다.
- **관련 내용**
  - GEMM core는 "parallel vector dot-product using reduction trees, but can be substituted with other implementations such as systolic arrays"라고 쓰여 있다. 원 설계도 systolic 교체를 염두에 뒀다.
  - GEMM intrinsic 모양(예: (1,32)x(32,32) vs (4,16)x(16,16))이 ISA 파라미터다(BATCH, BLOCK_IN, BLOCK_OUT).
  - 설계 공간 탐색의 회로 knob: **"Circuit Pipelining: e.g. for GEMM core between [11, 20] stages"**, **"PLL Frequency Sweeps: e.g. 250 vs. 300 vs. 333MHz"**. 많은 후보가 "fail placement, routing, or timing closure"로 떨어진다.
  - access-execute decoupling(load/compute/store 큐와 토큰)으로 메모리 지연을 숨긴다.
- **우리와의 관계**: 지금 EVTA는 원논문 구성(reduction tree, II=1, 333 MHz)에 가깝다. 원논문도 PLL sweep 상한이 333 MHz였고, 우리 Phase A가 그 위(360~380)를 처음 보드에서 확인했다.
  BATCH=1, BLOCK=16이라 GEMM 하나가 1x16 · 16x16이고, systolic에서 이것이 weight 재사용 문제(§4)가 된다.
- **가져올 것**: ISA를 그대로 두고 GEMM core만 바꾼다는 전제(§2)가 원 설계 의도와 맞는다.

### 9.3 UG579 v1.11 (UltraScale Architecture DSP Slice) — B1/B2와 CE의 근거

- **무엇**: DSP48E2 사용자 가이드. `docs/ref/ug579_v1.11.pdf`(git 제외, AMD 문서).
- **확인한 것**
  - dual B register(Figure 2-6): B1, B2 두 단에 **CEB1, CEB2가 따로** 있다(Table 3-2 핀).
  - `BREG`는 0/1/2, `BCASCREG`는 BREG=2일 때 1 또는 2. **BCASCREG=1이면 BCOUT은 B1 뒤에서 나간다.** 그래서 B1끼리 BCIN/BCOUT로 이어진 chain을 만들 수 있고, 곱셈기는 B2를 쓴다.
  - `INMODE[4]=0`이면 곱셈기 B 입력은 BREG 설정에 따른 B2(BREG=2)다. INMODE[4]로 B1을 고를 수도 있다(§9.4의 in-DSP multiplexing이 이것을 쓴다).
  - cascade(ACIN/BCIN/PCIN)는 같은 DSP 열 안에서 세로로만 이어진다.
- **우리 설계에 적용**(§5): B1 = prefetch chain, B2 = 계산용. CEB1은 PE(i, j)에서 [T+i+j, T+15+j], CEB2는 T+i+j. 한 열의 비트 정확 모델로 주기 16에서 오류 0을 확인했다.

### 9.4 Li et al., "Revealing Untapped DSP Optimization Potentials for FPGA-Based Systolic Matrix Engines" (arXiv 2409.03508)

- **무엇**: DSP48E2의 잘 안 쓰이는 기능(B1/B2 이중 레지스터와 개별 CE, 동적 mux, 세 cascade 경로)으로 TPUv1형(WS)과 Vitis AI DPU형(OS) systolic 엔진을 개선한 논문. 중국과학원 자동화연구소. FireFly(§9.5)와 같은 그룹.
- **기법 1: in-DSP operand prefetching (WS용)**
  - `BREG=2`, `BCASCREG=1`, `B_INPUT=Cascade`. B1이 BCIN/BCOUT로 이어진 **weight prefetch chain**, B2가 곱셈에 쓰는 고정 weight. "When data stored in the B2 registers expires, the data in the B1 registers shifts into B2."
  - weight 적재 경로와 부분합 경로가 모두 DSP 안(세로 cascade)에 들어가 fabric이 거의 없다.
- **기법 2: in-DSP multiplexing (DDR용)**: DPU는 DSP를 2배 클럭(Clk×2)으로 돌리고 CLB mux로 느린 클럭 데이터 두 조각을 번갈아 넣는다.
  논문은 B1/B2를 ping-pong으로 쓰고 INMODE mux로 고른다. LUT mux가 없어지고 staging 레지스터는 느린 클럭에 남는다(timing 압박 감소).
- **기법 3: ring accumulator**: 빠른 클럭의 DSP 두 개를 고리로 묶어 accumulator 수를 절반으로.
- **핵심 수치**(XCZU3EG, Vivado OOC)

| 설계 (INT8 14x14 TPUv1형) | LUT | FF | DSP | 클럭 | WNS | 전력 |
|---|---|---|---|---|---|---|
| tinyTPU (broadcast, packing 없음) | 120 | 129 | 196 | 400 MHz | 0.076 | 0.25 W |
| Libano (DDR mux, CLB 누산) | 23,080 | 60,422 | 196 | 666 | 0.044 | 4.87 |
| CLB-Fetch (weight 경로만 fabric) | 168 | 6,195 | 210 | 666 | 0.083 | 0.94 |
| DSP-Fetch (전부 DSP 안) | 167 | 4,516 | 210 | 666 | 0.052 | 0.93 |

  DPU B1024 재현(Table II): 공식 vs 제안, 둘 다 666 MHz. AccDSP 64 → 32, MuxLUT 128 → 0, 전체 LUT 1,280 → 158, 전력 1.03 → 0.826 W.
  FireFly 적용(Table III): 666 MHz, FF 4,344 → 2,296.
- **같은 점**: 우리 §5의 weight 경로(B1 chain + B2 고정)가 DSP-Fetch와 같다. 입력을 가로 FF로, 부분합을 세로 P cascade로 보내는 배치도 같다.
- **다른 점·주의**
  - 수치는 **어레이 단독 OOC**다. AXI, 큐, BRAM 공급 경로가 없다.
  - tinyTPU가 400 MHz에 그친 이유를 "data broadcasting instead of staging"이라고 쓴다. 우리 지금 HLS의 inp broadcast(fanout 346, §8 1))와 같은 원인이다.
  - 14x14 + INT8 packing(196 → 210 DSP)이라 DSP 하나에 MAC 2개다. 우리는 packing 없이 시작한다(§9.8).
- **가져올 것**: B1/B2 설정값과 CE 파형(우리 §5 wavefront는 이 논문의 Fig. 3을 skew 어레이로 확장한 것), C 단계에서 in-DSP multiplexing(LUT mux 없이 2배 클럭).
- "peak clock rate of DSP48E2 can exceed 700MHz for a device of the fastest speed grade"라고 인용한다(우리 보드 xczu9eg-2는 가장 빠른 등급이 아니다).

### 9.5 FireFly v2 (arXiv 2309.16158) — 보드 위 시스템에서 DSP 2배 클럭

- **무엇**: SNN 가속기. 같은 그룹. 중요한 것은 **OOC가 아니라 Zynq US+ 보드의 전체 시스템**에서 DSP 2배 클럭을 낸 결과다.
- **구조**: 계산 엔진만 2배 클럭. 원칙 세 가지를 명시한다:
  "the circuits in the doubled-frequency domain should be well-decoupled from the circuits in the low-frequency domain",
  "avoid the use of LUTs in the doubled frequency domain, and instead, only use DSP48E2 and flip-flops", "avoid high-fanout nets".
  느린 쪽과 빠른 쪽 사이는 **gearbox**(2배 폭 → 2배 속도, 양쪽 대역폭 같음). 어레이는 OS(weight 세로, spike 가로).
- **핵심 수치**(Table III, 실제 구현)

| 장치 | DSP | 클럭(시스템/엔진) | 피크 | 전력 |
|---|---|---|---|---|
| xczu3eg (FireFly v1) | 288 | 300 | 1,382 GOP/s | 3.11 W |
| xczu3eg (v2) | 256 | **300/600** | 2,458 | 6.2 W |
| xczu5ev (v2) | 512 | **250/500** | 4,096 | 4.9 W |
| xczu7ev (v2) | 1,024 | **250/500** | 8,192 | 13.5 W |

  큰 장치(5EV/7EV)에서는 500 MHz로 내려갔다. 장치가 크고 어레이가 클수록 2배 클럭의 상한이 내려간다.
- **우리에게 주는 것**: ZU9EG(7EV보다 크다)에서 **엔진 500~600 MHz, 시스템 250~333 MHz**가 현실적인 범위라는 근거다. 우리 C안(§0)의 목표를 666이 아니라 **~600 MHz(시스템 300~333의 2배 근처)**로 잡는 것이 맞다.
  gearbox와 "빠른 도메인에는 DSP와 FF만" 원칙은 §5의 "모든 경계를 레지스터로" 방침과 같다.

### 9.6 Petropoulos & Antonakopoulos (arXiv 2510.08137, CC-BY 4.0) — 2 클럭 도메인 GEMM 시스템

- **무엇**: Alveo U50(HBM)용 GEMM 가속기. PU(processing unit) 여러 개.
- **구조**: 시스템 300 MHz, **systolic array와 on-chip weight memory(URAM cascade) 600 MHz**. HBM 컨트롤러 450 MHz.
  "A stream-width adapter … converts the 256-bit interface at 300 MHz (system clock) to 128 bits at 600 MHz (fast clock)".
  activation은 DSP 입력 cascade로 세로, 부분합은 C 포트로 가로(우리와 방향이 반대). URAM cascade로 weight를 공급한다.
- **핵심 수치**: PU2x는 DSP 514, LUT 16.5K. 최대 구성(PU1x 5 + PU2x 5) URAM 100%, DSP 64.8%. ResNet-18 1,237.7 FPS, ResNet-50 584.9 FPS,
  측정/이론 성능비 최대 98%, 12.7 FPS/W. 첫 conv는 호스트가 IM2COL로 바꿔 넣는다(정렬 제약).
- **우리에게 주는 것**: "시스템 1x + 어레이와 weight 메모리 2x"가 보드 시스템에서 98% 효율로 동작한 사례. 2x 쪽에 **weight 공급 메모리까지** 넣었다는 점이 다르다.
  우리 C안에서 wgt_mem → PISO만 빠른 쪽에 둘지, 느린 쪽 2배 폭으로 줄지 정할 때 참고한다(대역폭이 같으면 폭 2배·속도 1x가 단순하다).

### 9.7 Xilinx DPU (PG338, DPUCZDX8G)

- **무엇**: Xilinx의 상용 CNN IP(암호화). ZCU102/ZCU104 공식 지원.
- **구조**: 클럭 도메인 셋(레지스터 설정, 데이터 컨트롤러, 계산). "The DSP slices in the computation unit module are in the `dpu_2x_clk` domain, which runs at twice the clock frequency of the data controller module." "The two related clocks must be edge-aligned."
  OS systolic, 채널·픽셀 병렬. §9.4에 따르면 CLB mux로 2배 클럭 DSP에 데이터를 번갈아 넣고 결과를 느린 쪽 accumulator로 돌린다.
- **수치**: 이전 세션에서 PG338로 확인한 것은 데이터 325 MHz / DSP 650 MHz다. 이번에 받은 Clocking 페이지에는 수치가 없어 다시 확인하지 못했다(§9.13).
- **우리에게 주는 것**: 같은 MMCM의 edge-aligned 1x/2x 클럭. 우리 `pll_clk`(clk_wiz)에 CLKOUT 하나를 더 두면 된다(지금 HP 분리용 `hp_pll`은 다른 MMCM이라 비동기이고, 어레이용 2x는 같은 MMCM에서 내야 한다).

### 9.8 Xilinx WP486 "Deep Learning with INT8 Optimization on Xilinx Devices"

- **무엇**: DSP48E2 하나로 int8 곱 두 개(공유 operand 하나)를 하는 방법.
- **방법**: int8 a, b를 pre-adder로 27비트 포트에 붙인다(a를 18비트 왼쪽으로). c(공유)는 18비트 포트. 45비트 곱 = a·c·2^18 + b·c.
  "a minimum of 16 + 8 = 24-bits total input size". 공유 operand는 weight여도 된다.
- **한계**: "accumulation of up to 7 product terms only can be guaranteed with no overflow for the lower bits", 7항마다 DSP 하나를 더 써서 보정한다. 그래서 8 DSP로 14 MAC = **1.75배**.
- **우리에게 주는 것**: 우리 열은 16항을 P cascade로 더한다. 7항 제한 때문에 packing을 쓰려면 cascade를 7항마다 끊고 보정해야 한다(§9.4의 DSP-Fetch도 INT8 correction용 LUT/CARRY를 쓴다).
  MAC을 늘리려면 BLOCK_OUT 32(ISA 변경)도 필요하다. **지금 단계에서는 이득보다 복잡도가 크다**(§0 D 보류의 근거).

### 9.9 Tensil (Apache-2.0) — 오픈 소스 TCU

- **무엇**: Chisel 기반 systolic TCU와 컴파일러. `docs/ref/tensil/*.tarch`.
- **구성**: ultra96v2 `array_size 16`, zcu104 `array_size 32`, 둘 다 `data_type FP16BP8`(16비트 고정소수점, 소수 8비트), `accumulator_depth 4096`, `local_depth` 20480/16384.
- **수치**: README가 "ResNet at 300 FPS on ZCU104"(ResNet20 CIFAR) 튜토리얼을 링크한다. 클럭은 확인하지 못했다(문서 사이트가 다른 도메인으로 리다이렉트된다).
- **우리에게 주는 것**: 16비트 고정소수점이라 DSP당 MAC 1개, 우리 int8과 직접 비교하기 어렵다. ZCU104에서 32x32를 쓴다는 점(E안의 규모 참고) 정도.

### 9.10 AutoSA (UCLA, MIT) — HLS systolic 생성기

- **무엇**: polyhedral 컴파일러로 C 루프에서 HLS systolic 배열을 생성한다(FPGA'21). `docs/ref/autosa/README.md`.
- **수치**(이전 세션, 논문 요약): U250에서 MM int8 6.95 TOPs, int16 3.41 TOPs, fp32 934 GFLOPs. I/O 모듈 daisy-chain(L1/L2/L3), double buffering, SIMD.
- **우리에게 주는 것**: HLS 생성물이다. 우리 문제는 HLS 경로의 fanout과 배선(§8)이므로 HLS로 다시 만드는 것은 근거가 약하다. 논문 PDF(cadlab.cs.ucla.edu)는 이번에도 받지 못해 클럭을 확인하지 못했다.

### 9.11 de Fine Licht et al., gemm_hls (arXiv 1912.06526, BSD-3)

- **무엇**: communication-avoiding GEMM을 HLS로 만든 설계. VCU1525(VU9P).
- **수치**(논문 Table): FP16 171.3 MHz 606 GOp/s, FP32 145.7 MHz 409 GOp/s, FP64 181.2 MHz 132 GOp/s, **uint8 186.5 MHz 1,544 GOp/s**. 목표 200 MHz, SLR을 넘으면 클럭이 떨어진다.
- **우리에게 주는 것**: HLS systolic의 클럭이 200 MHz 근처라는 사례. 큰 HLS 어레이가 배선 때문에 클럭을 잃는 것은 우리 §8 관찰과 같다.

### 9.12 종합: 소스 비교와 방향에 주는 결론

| 소스 | 장치 | 측정 | 어레이 | dataflow | weight 교체 | 부분합 | 클럭 |
|---|---|---|---|---|---|---|---|
| 우리 지금 HLS | ZU9EG-2 | 보드 | 16x16 int8 | (트리) | 매 사이클 2048비트 읽기 | 16입력 덧셈 트리 | 333 (380 보드 OK) |
| 우리 §5 계획 | ZU9EG-2 | — | 16x16 int8 | WS | B1 chain + B2, CE wavefront | P cascade | 목표 400 / C안 ~600 |
| Gemmini | ASIC | 논문 | 16x16 int8 | WS/OS | PE c1/c2 + propagate | 세로 b | 1.89 GHz(ASIC) |
| DSP-Fetch | ZU3EG | OOC | 14x14 int8 packed | WS | B1 chain + B2 | P cascade | 666 |
| FireFly v2 | ZU3EG/5EV/7EV | 보드 | 16x16x(4~8)x4 | OS | — | DSP | 300/600, 250/500 |
| 2510.08137 | U50 | 보드 | RSA x CSA | — | URAM cascade @2x | C 포트 가로 | 300/600 |
| DPU | ZU+ | 상용 | B512~B4096 | OS | CLB mux @2x | DSP | 325/650 |
| tinyTPU | ZU3EG | OOC | 14x14 | WS | broadcast | — | 400 |
| gemm_hls | VU9P | 보드 | HLS | — | — | — | 145~186 |

**결론**
1. **fanout을 없애면 400 MHz는 근거가 있다.** broadcast형 tinyTPU가 400, staging형이 666이다(같은 장치, OOC). 우리 400 MHz 실패의 주원인도 broadcast다(§8). → B안의 목표 400은 보수적이고 타당하다.
2. **2배 클럭(C안)은 보드 시스템에서도 됐다.** FireFly v2(ZU+ 보드 300/600, 250/500), 2510.08137(U50 300/600), DPU(325/650). 공통 조건: 빠른 도메인에는 DSP와 FF만, 경계는 gearbox, 같은 MMCM의 edge-aligned 클럭.
   큰 장치에서 500으로 내려간 사례가 있으므로 **ZU9EG 목표는 ~600(시스템 300~333)**으로 잡는다.
3. **weight 경로는 DSP 안 B1/B2가 정답이다.** DSP-Fetch가 fabric 대비 FF 27% 감소, 같은 클럭. 우리 §5 설계 그대로 간다.
4. **INT8 packing은 보류.** 16항 cascade에 7항 제한과 보정이 붙고, MAC 증가에는 ISA 변경이 필요하다.
5. **end-to-end는 CPU가 정한다.** Gemmini(im2col 없으면 CPU가 좌우)와 우리 측정이 같다. GEMM 작업과 별도로 CPU 쪽(conv0, transpose, im2col) 이전을 계획에 넣어야 한다.
6. **HLS로 systolic을 다시 만드는 길(AutoSA, gemm_hls)은 택하지 않는다.** 공개 HLS 사례의 클럭이 우리 지금 HLS보다 낮다.

### 9.13 확인하지 못한 것

- Gemmini의 FPGA 클럭(Sensors 2023 본문, PMC 접속이 브라우저 확인 페이지로 막힘).
- Tensil 클럭(문서 사이트 리다이렉트), AutoSA 클럭(논문 PDF 시간 초과).
- DPU 325/650 MHz 수치: PG338 Clocking 페이지에는 수치가 없었다. Computation Clock 페이지는 "twice"와 "edge-aligned"만 확인했다.
- WP486 원문 PDF(재게시본으로 확인).
- ZU9EG -2 speed grade의 DSP48E2 Fmax(DS925). C안 목표의 상한을 정할 때 필요하다.

## 10. Phase B(uop 순서)와 Phase C(사이클 모델) (2026-10-27)

**Phase B — 런타임 uop 순서** (`vtalib-zcu102-systolic/lib/Bundle/VTABundle.cpp`, 커밋 `3982d469`, `f2dd47a0`)
- `gemmop_mxn`: it_out = kernelIdx(dst tile_h·tile_w, wgt KH·KW), it_in = kx, uops = ky x y x x.
  - uop 수가 KH·tile_h·tile_w > uop 버퍼/4(2048)이면 원래 순서(`gemmop_mxn_old`)를 쓴다.
- `gemmop_1x1`: it_out = kernelIdx, it_in = x, uops = y. uop 수는 그대로다.
- 정확성: 500개 무작위 tiling에서 (dst, src, wgt) 멀티셋이 원래와 같다. 같은 dst가 다시 오는 간격은 ≥ tile_h·tile_w다.
- VTABundle.cpp는 hw_spec.h를 include하지 않는다. uop 버퍼 깊이는 `-DVTA_LOG_UOP_BUFF_SIZE`로 계산한다(`GEMM_UOP_BUFF_DEPTH`).
- 보드 빌드: `vtabundles_xcmpl/build_sysprof/` (`-DVTALIB=/home/xilinx/JO/npuprg/systolic/vtalib-zcu102-systolic -DNESTC_EVTA_PROFILE=ON -DBUNDLE_EXE_SUFFIX=_sysprof`). 풀기·빌드·실행 명령은 vta_synth_notes §2.5 7).
  - 보드의 main 트리는 건드리지 않았다. vtalib는 `git archive origin/systolic vtalib-zcu102-systolic`로 `/home/xilinx/JO/npuprg/systolic/`에 풀었다.
- 보드 결과(333 MHz `vtahw_single_y219.bin`, 보드 `cd $B/vtabundles_xcmpl/hw` 뒤 `sudo -n /bin/sh -c "BIN=vtahw_single_y219.bin script/run_prof.sh all"`와 `... run_prof.sh -s _sysprof all"`, `_prof` 대 `_sysprof`):
  - golden 30개 모두 PASS. VTA 시간은 같다: Conv4 3.546 → 3.565 ms(+0.5%), ResConv10 2.896 → 2.894 ms, 나머지 ±0.5%.
  - **Resnet18 849, Resnet50 51, Resnet18Partition 737로 틀렸다**(원래 순서는 281). 세 번들만 "EVTA uop buffer overflowed"를 찍었다.
  - 원인: vtalib `UopQueue`(옛 TVM 코드)의 버그 두 개. uop 커널이 uop SRAM(8192)을 넘쳐 0으로 되돌아갈 때만 드러난다.
    원래 순서는 커널이 작아 넘친 적이 없었다.
    1. uop LOAD의 DRAM offset을 `cache_` 안 앞 커널 크기의 합으로 정했다. `ReadBarrier`도 `cache_`를 그 순서로 복사했다.
       eviction이 `cache_`에서 커널을 지우면, 같은 배치에서 먼저 나간 LOAD가 다른 위치를 읽는다.
    2. `Synchronize`마다 `Reset()`이 `cache_`를 비우지만, 커널의 cached 표시는 남는다.
       뒤에 SRAM이 넘쳐도 그 커널들은 무효화되지 않고, 덮어쓴 uop으로 실행된다.
  - 수정(`vtalib-zcu102-systolic` `include/zcu102/dat_queue.h`, `lib/simulator/runtime.cc`, 같은 내용):
    배치마다 append-only `batch_`(offset을 Push 때 고정)를 `ReadBarrier`가 복사한다.
    `cache_`는 SRAM에 있는 커널을 sync를 넘어 들고 있고, SRAM이 0으로 되돌아갈 때 모두 무효화한다.
  - 이 버그는 원본 `vta/vtalib`와 복사본 3개에도 있다. 큰 uop 커널을 쓰는 다른 변경에서도 같은 일이 생긴다.
  - 수정 후(커밋 `vtalib-zcu102-systolic: fix the uop cache`, 다시 `run_prof.sh -s _sysprof all`):
    golden 30개 PASS, Resnet18/Resnet18Partition/Resnet50 → 281, Vgg16 → 0. overflow는 그대로 5번 나지만 결과가 맞다.
    VTA 시간: Resnet18 48.378 → 48.535 ms(+0.3%), Resnet50 245.63 → 246.03 ms(+0.2%), 나머지 ±0.5%(uop load가 늘어난 만큼).
  - **Phase B 완료: 새 uop 순서는 지금 비트스트림에서 결과가 같고 시간도 거의 같다.**
  - **검증 범위**: HW는 바꾸지 않았다. 지금 HLS GEMM(16열 덧셈 트리, 열마다 시각 차이 없음)에서 런타임의 uop 순서만 바꿔 돌렸다.
    즉 "새 순서가 같은 누산을 만든다"와 "uop cache 수정"을 보드로 확인한 것이고, systolic HW의 정확성은 아직 확인하지 않았다(Phase D).

**Phase C — 사이클 모델** (`vta-hw-zcu102-systolic/hardware/xilinx/sim/systolic_model.cc`, `g++ -O2 -std=c++14`)
- 실행(호스트, 아무 dir): `g++ -O2 -std=c++14 -Wall -o /tmp/systolic_model $N/vta-hw-zcu102-systolic/hardware/xilinx/sim/systolic_model.cc && /tmp/systolic_model 300 1`(인자: 무작위 테스트 수, seed).
- §5 구조를 레지스터 단위로 옮겼다.
  - A: skew와 가로 FF. B: PISO, B1 chain, B2, CEB1/CEB2 wavefront. P cascade, deskew, 태그 pipe.
  - RMW(forwarding 포함)와 issue 제어기(LOOKAHEAD 깊이 uop FIFO에서 다음 weight를 미리 읽음, 주기 16).
- 기준: `vta.cc` `gemm()`과 같은 순차 계산. 단 `reset_reg`일 때 out = 0이다(HLS는 acc+dot의 하위 8비트, §5).
- 무작위 300개(명령 1~4개, 임의 uop/factor/reset, drain on/off, LOOKAHEAD 1~64): **모두 비트 단위로 일치**(forwarding 68회).
- **열마다 다른 출력 시각과 acc 주소 맞추기(모델에서)**: 열 j의 합은 열 0보다 j클럭 늦게 나온다. 열 j를 15−j단 deskew FF로 늦춰 16열이 같은 클럭에 한 행으로 모이게 하고,
  dst와 reset은 issue부터 RMW까지 같은 깊이(`OUT_OFS`)의 태그 pipe로 보낸다. 그래서 RMW는 열마다 따로가 아니라 `acc[dst]` 512비트 한 행을 한 번에 읽고 더해 쓴다.
  - 이것은 C++ 레지스터 모델(`systolic_model.cc`)에서만 확인했다. RTL은 아직 없다.
  - 모델은 PE당 지연 1클럭, BRAM 2클럭, RMW 3클럭으로 두었다. DSP48E2의 실제 파이프 단(AREG/BREG/MREG/PREG)은 모든 열에 같은 상수 지연을 더하므로 `OUT_OFS`(태그 pipe 깊이)만 바뀐다.
    Phase D에서 RTL 시뮬레이션으로 이 상수를 맞추고, 같은 무작위 테스트를 RTL과 모델 양쪽에 돌려 비교한다.
- 가동률(GEMM / 사이클, LOOKAHEAD 32, RMW 3, 명령 사이 handshake 4):

| 경우 | 원래 순서 | 새 순서, 명령마다 drain | 새 순서, drain 없음 |
|---|---|---|---|
| 3x3 7x7 nk1 (ResConv10형) | 41.7% | 88.2% | 99.8% |
| 3x3 7x7 nk4 | 43.2% | 96.8% | 100.0% |
| 3x3 8x56 nk1 (Conv4형) | 49.7% | 98.6% | 99.9% |
| 1x1 7x7 nk4 | 39.4% | 76.9% | 99.6% |

- 해석:
  - 원래 순서는 §4 예상대로 약 44%다(7사이클 계산, 9사이클 대기). Phase B가 필수다.
  - 명령마다 drain을 기다리면 명령당 약 40사이클(파이프 깊이 + RMW + handshake)과 첫 weight 채우기 19사이클이 든다.
    작은 명령(ResConv10 nk1 441 GEMM, 1x1)에서 88%, 77%로 떨어진다. 지금 HLS(93.5%)보다 낮다.
  - **drain을 GEMM 명령 사이에서 생략하고, 다음 명령의 첫 weight를 미리 채우는 것이 필수다.** §5의 "처음 구현은 단순하게"를 바꿔, 첫 구현부터 넣는다.
    - drain은 ALU/STORE 토큰 앞에서만 한다.
    - RMW가 순서대로라 정확성은 그대로다(모델의 drain 없음 모드도 비트 일치).
