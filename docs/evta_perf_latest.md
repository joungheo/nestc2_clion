# 최신 성능 기록 (2026-11-26)

보드(ZCU102)에서 모델 7개를 **지금 판**(컴파일러, vtalib, bitstream)으로 한 가지 방법으로 잰 기록이다. 다음에 다시 잴 때는
`vtabundles_xcmpl/perf_record.sh`로 같은 방법을 쓰고 이 문서를 새 날짜로 고친다(§5).

용어: **multi4** = EVTA 4개 bitstream, **single** = EVTA 1개 bitstream, **4개 동시** = copy 넷(EVTA마다 하나)이 함께 도는 처리량,
**혼자** = 실행 파일 하나가 EVTA 하나로 1장씩 도는 시간, **copy 0** = multi4에서 EVTA 0을 쓰는 사본, **fps** = 초당 처리한 그림 수.

## 1. 조건

| | |
|---|---|
| 컴파일러 | `0b1439b` 이후(그 뒤 컴파일러 변경 없음). 보드 `Debug2_remote` model-compiler(11-25 18:04) |
| bundle | 11-26 보드에서 다시 만듦(§4). single과 multi4 copy 0의 weights가 같다 |
| vtalib | `vtalib-zcu102-systolic` `818a73c`(depthwise 창 장치 포함, ubun의 11-25 CPU 시간 줄이기 포함) |
| 실행 파일 | 보드 `build_sys`, `build_multievtax4_sys`(`_sys`, 시간 재는 코드 없음), 범용 main(`multi_threads_main.cpp`) |
| 입력 | int8(`<Dir>/i8/<그림>.i8`, bundle 자신의 quantize로 만든 것). cat_285.png, VGG16은 airplane/0001.png |
| 재는 법 | OMP 1. 혼자 = 4 × 25장(첫 장 뺀 평균), 4개 동시 = copy마다 25장. bitstream을 번갈아 3번씩, 표는 3번의 평균 |
| 결과 확인 | 모든 실행 파일 `EVTA_RES_CHECK=1`(EVTA 호출마다 CPU와 비교) **0 differ**, 분류가 전과 같다(Resnet 281, VGG16 8, MobileNetV2 282, DeiT 281) |

bitstream:

| 이름 (`/lib/firmware/vtahw_zcu102_1x16_i8w8a32_…`) | EVTA | 클럭 | 무엇 |
|---|---|---|---|
| `16_15_18_17_4evta_sa_amul_dw_400mhz_hp304_rq.bin` | 4 | 400 / HP 304 MHz | **multi4 기본(11-26부터)**. `_amul` + depthwise 창 장치 |
| `16_15_18_17_4evta_sa_amul_400mhz_hp322_rq.bin` | 4 | 400 / HP 322 MHz | 그 전 multi4 기본(`BIN=`으로) |
| `16_16_19_18_sa_alux3_400mhz_rq.bin` | 1 | 400 MHz | **single 기본**. ALU 확장 3단계 |
| `16_16_19_18_sa_alux3_dw_387mhz_rq.bin` | 1 | 387.5 MHz | 11-26 새로 만듦. `_alux3` + depthwise 창 장치(§3) |

HP 클럭 = PS의 DDR 포트(HP)와 EVTA DMA 사이 클럭.

## 2. multi4

| 모델 | 4개 동시 `_dw` (fps) | 4개 동시 `_amul` (fps) | copy당 ms `_dw` / `_amul` | 혼자 ms `_dw` / `_amul` |
|---|---|---|---|---|
| Resnet18 (MXNet) | 170.3 | 171.3 | 23.52 / 23.36 | 22.42 / 22.38 |
| Resnet50 (Caffe2) | 55.7 | 55.4 | 71.81 / 72.17 | 66.58 / 66.06 |
| Resnet50Tv (torchvision, DPU 비교) | 55.9 | 55.5 | 71.62 / 72.15 | 65.96 / 65.43 |
| VGG16 CIFAR-10 | 497.7 | 508.3 | 8.04 / 7.87 | 7.07 / 6.90 |
| MobileNetV2 (QAT) | **282.6** | 263.9 | 14.17 / 15.16 | **12.10** / 13.55 |
| MobileNetV2Tf (TF-slim, DPU 비교) | **281.5** | 256.7 | 14.22 / 15.60 | **12.14** / 13.55 |
| DeiT-tiny | 44.2 | 43.7 | 90.45 / 91.66 | 80.81 / 80.72 |

- **depthwise 창 장치는 MobileNetV2 두 모델만 빠르게 한다:** 4개 동시 +7–10 %, 혼자 −11 %. 다른 모델에는 depthwise conv가 없다.
- **HP 클럭(322 → 304 MHz)의 값:** 혼자 Resnet50 +0.5 ms(+0.8 %), VGG16 +0.17 ms(+2.4 %), 4개 동시는 VGG16 −2 %, 나머지는 잡음 안.
  4개 동시의 3번 값은 ±3 %쯤 흔들린다(예: MobileNetV2 267.7–292.3 fps).
- **AMD DPU S(B4096 1개)와:** MobileNetV2Tf 281.5 대 270.0 fps(104 %), Resnet50Tv 55.9 대 79.9 fps(70 %). [evta_dpu_compare.md](evta_dpu_compare.md)
- DeiT는 multi4 bitstream에 ALU 확장 2 / 3단계가 없어 layer norm / softmax가 CPU로 간다. 그래서 single(34.4 ms)보다 copy가 훨씬 느리다.

## 3. single

| 모델 | `_alux3` 400 MHz (ms) | `_alux3_dw` 387.5 MHz (ms) | 같은 판, `EVTA_DWU=0` |
|---|---|---|---|
| Resnet18 | **22.15** | 22.81 (+2.9 %) | 22.86 |
| Resnet50 | **62.33** | 63.96 (+2.6 %) | 63.88 |
| Resnet50Tv | **61.78** | 63.33 (+2.5 %) | 63.34 |
| VGG16 | **6.68** | 6.75 (+1.1 %) | 6.75 |
| MobileNetV2 | 13.04 | **11.70** (−10.3 %) | 13.29 |
| MobileNetV2Tf | 13.06 | **11.65** (−10.8 %) | 13.30 |
| DeiT | **34.44** | 35.10 (+1.9 %) | 35.10 |

- `_dw` 판은 400 MHz를 맞추지 못해 387.5 MHz다. 클럭이 3 % 낮아 MobileNetV2 말고는 1–3 % 느리다. 그래서 **single 기본은 `_alux3` 400 MHz 그대로**다.
  MobileNetV2를 single로 잴 때는 `BIN=…_sa_alux3_dw_387mhz_rq.bin`.
- multi4 copy 0 혼자(§2)와 single은 bitstream이 달라 시간이 다르다(버퍼 크기, ALU 확장, 클럭). 예: MobileNetV2Tf 13.55(`_amul` copy 0) / 13.06(single `_alux3`) ms.

**single `_dw` 빌드** (`docs/evta_dw_unit.md` §8–9의 경험을 그대로 썼다):

| 단계 | 결과 |
|---|---|
| `make bin SYSTOLIC=1 ALUX=1 DWU=1 FREQ=400` 두 전략 동시에 (`BUILD_TAG=rq_alux3c` / `d`) | ExplorePostRoutePhysOpt −0.158 ns, **ExtraTimingOpt −0.092 ns**(실패 498개, 모두 EVTA 클럭) |
| d의 배선 결과에 `phys_opt_design -directive AggressiveExplore` (`build/single_dw400/popt2.tcl`) | −0.070 ns. 그 뒤 `route_design -directive Explore`는 −0.091 ns로 되돌아가서 쓰지 않았다 |
| `flow/mmcm_set.tcl popt2_d.dcp dw387 11.625` (VCO 1162.5 MHz ÷ 3, 배치·배선 그대로) | **WNS +0.010, WHS 0.000 ns**, 387.5 MHz |
| (같이 만든 판) `mmcm_set.tcl popt_d.dcp dw383 11.5` | WNS +0.016 ns, 383.3 MHz. 보드에 있지만 387.5가 1 % 빠르다 |

- 자원: LUT 71.1k(`_alux3` 69.8k), FF 93.1k, BRAM 259, DSP 351. 보드 `bit/evta_single_alux3_dw_387MHz_261126/`, PC `build/single_dw400/`.
- 보드 시험: `run_alux_board_tests.sh`(ALU 확장, epilogue, layer norm, softmax, elemadd, `dw_evta_test` 창 / 대각) 모두 0 differ, 7모델 `EVTA_RES_CHECK` 0 differ.

## 4. 이번에 바꾼 것 (보드에서 측정 전에)

- **bundle 다시 생성:** Resnet18, Resnet50, Resnet50Tv, VGG16, MobileNetV2(QAT), DeiT(multi4)를 지금 컴파일러로.
  invalidate 빼기(`-vta-skip-evta-invalidate`)로 bundle마다 `VTABufferInvalidate`가 16–53개에서 1개로 줄었다. single은 weights와 코드가
  예전 bundle과 같고(invalidate 줄과 MobileNetV2의 112 × 112 schedule 셋만 다르다), 옛 bundle은 보드 `~/JO/ubuntu2_wt/old_bundles_261126/`.
  - 옛 multi4 bundle 두 개는 single과 달랐다: `MultiEVTAResnet50Test`는 11-22의 torchvision 모델이 남아 있었고(`onnx__Conv_*` 이름),
    `MultiEVTAMobileNetV2Test`는 다른 profile(입력 scale 1/128, single은 1/64). `MultiEVTADeiTTest`는 11-24 전 bundle이었다.
    지금은 일곱 모두 copy 0 weights = single bundle.
  - 명령: `gen_calib_bundle.sh <Dir> ../vta/bundles/<Dir>/calib/<profile>`(Resnet18 / Resnet50Tv / VGG16 / MobileNetV2: `*_b2048.yaml.gz`,
    Resnet50: `resnet50_calib_n200.yaml`), `gen_multi4_bundles.sh <Dir>`(Resnet50: `PROF=…/resnet50_calib_n200.yaml MCX=-vta-round-shift`,
    MobileNetV2: `PROF=<b2048 profile 풀어 둔 것>`), 그다음 `collect_hw_bundles.py`.
- **범용 main:** single Resnet18 / Resnet50 / Resnet50Tv / VGG16(`70a02e0`)과 multi4 Resnet18 / Resnet50 / Resnet50Tv / VGG16 / DeiT(`8f3bda0`)가
  MobileNetV2와 같은 `multi_threads_main.cpp`를 쓴다: int8 입력, 같은 시간 재기. 예전 main(Glow 기본 main, ETRI multi main)에는 없었다.
- **`run_prof.sh`(`b580636`):** `hw/`의 범용 main 실행 파일을 `-s _sys`로 돌리면 `BUNDLE_RUNS` 때문에 1장만 쟀다(그 값은 첫 replay라 15 ms쯤).
  이제 `BUNDLE_RUNS`는 timed / prof 빌드에만 넘기고, 나머지는 `MULTI_RUNS`(4 × n장).

## 5. 다시 재는 법

```bash
# 보드, nest-compiler 트리에서 (다른 EVTA 실행이 없을 때; run_prof.sh가 lock을 잡는다)
sudo /bin/sh -c "vtabundles_xcmpl/perf_record.sh -r 3"          # i8, check, m4, single, 기본 bitstream(SYS_BIN)으로
sudo /bin/sh -c "M4_BITS='<새>.bin <옛>.bin' S1_BITS='<a>.bin <b>.bin' vtabundles_xcmpl/perf_record.sh -r 3 m4 single"   # 번갈아 비교
# 결과: $OUT(기본 /tmp/perf_record_<날짜>)/summary.txt
```

- 실행 파일을 새 vtalib로 다시 빌드할 때: `cd vtabundles_xcmpl/build_multievtax4_sys && make -j2 multivta<X>Bundle singlevta<X>Bundle`,
  `cd build_sys && make -j2 vta<X>Bundle`(`<X>` = MxnetResnet18, Caffe2Resnet50, TvResnet50, VGG16Cifar10, MobileNetV2, MobileNetV2Tf, DeiT).
- 이번 원자료: 보드 `~/JO/ubuntu2_wt/perf_261126/`(multi4 `m4_<판>_r<n>_<Dir>_{multi,single}.log`, single `sd_<판>x<n>_r1_<Dir>.log`).

## 6. 1×1 weight 한 번 LOAD(`38e3da6`) 뒤 (2026-11-26 17:02–17:13, ubun)

보드 공유 `build_sys`, `build_multievtax4_sys`를 `38e3da6`(vtalib-systolic `EVTA_WGT_FIT`, [mobilenetv2_multi4_sw.md](mobilenetv2_multi4_sw.md) §7)으로 다시 빌드하고,
같은 실행 파일로 `EVTA_WGT_FIT=0`(전) / `1`(후)을 번갈아 2번씩 `perf_record.sh -r 1 single m4`로 쟀다. 표는 2번의 평균.
`check`(`EVTA_RES_CHECK=1`)는 21개 실행 파일 모두 0 differ, 분류는 §1과 같다. 원자료: 보드 `~/JO/ubun_wt/perf_261126_wgtfit/`.
한 장의 연산량(GOP, `run_prof.sh`가 찍는 값): Resnet18 3.700, Resnet50 / Resnet50Tv 8.178, VGG16 0.630, MobileNetV2 / MobileNetV2Tf 0.658, DeiT 2.182. GOPS = GOP × fps.

**single** (혼자, ms):

| 모델 | `_alux3` 400 전 | `_alux3` 400 후 | `_alux3_dw` 387.5 전 | `_alux3_dw` 387.5 후 |
|---|---|---|---|---|
| Resnet18 | 22.21 | 22.16 | 22.80 | 22.81 |
| Resnet50 | 62.35 | 62.27 | 63.92 | 63.90 |
| Resnet50Tv | 61.78 | 61.73 | 63.31 | 63.28 |
| VGG16 | 6.69 | 6.66 | 6.76 | 6.78 |
| MobileNetV2 | 13.02 | **12.86** | 11.66 | **11.47** |
| MobileNetV2Tf | 13.05 | **12.89** | 11.69 | **11.52** |
| DeiT | 34.43 | 34.38 | 35.13 | 35.15 |

**multi4** (`_amul_dw`, 4개 동시 fps (GOPS, 네 EVTA 합) / 혼자 copy 0 ms):

| 모델 | 4개 동시 전 | 4개 동시 후 | 혼자 전 | 혼자 후 |
|---|---|---|---|---|
| Resnet18 | 166.6 (616) | 171.1 (633) | 22.42 | 22.43 |
| Resnet50 | 55.6 (455) | 57.1 (467) | 66.54 | **64.23** |
| Resnet50Tv | 56.0 (458) | 57.8 (473) | 65.94 | **63.66** |
| VGG16 | 505.7 (319) | 482.6 (304) | 7.06 | 7.06 |
| MobileNetV2 | 286.8 (189) | 284.3 (187) | 12.21 | **11.77** |
| MobileNetV2Tf | 284.7 (187) | 286.5 (189) | 12.09 | **11.80** |
| DeiT | 43.9 (96) | 43.3 (94) | 80.87 | 80.79 |

- 1×1 conv의 thread weight가 wgt 몫을 넘는 모델만 빨라진다. multi4(몫 512 entry): Resnet50 / 50Tv −3.5 %, MobileNetV2 두 모델 −2.4–3.6 %(혼자).
  single(몫 1024 entry)은 MobileNetV2 두 모델의 7×7 960 → 160 / 320만 해당해 −1.2–1.6 %, Resnet50은 잡음 안이다.
- 4개 동시는 이 2번에서 ±3–5 % 흔들려(VGG16 466.6–506.6 fps, 명령이 같은데도) 차이를 말할 수 없다. 혼자 값이 기준이다.
  MobileNetV2Tf 4개 동시는 앞서 같은 실행 파일을 4번씩 번갈아 잰 284.9 → 292.0 fps(187 → 192 GOPS, §7.4 of mobilenetv2_multi4_sw.md)를 쓴다.
- single 기본 bitstream은 §3과 같은 결론이다. `_dw` 387.5 MHz가 MobileNetV2 두 모델에서 −10.6 %(11.5 대 12.9 ms), 나머지는 1–3 % 느리다.
- 처음 돌린 것(16:53)은 다른 사용자의 EVTA 실행이 lock을 잡고 있어 하나도 돌지 않았는데, `perf_record.sh`가 남아 있던 옛 로그를 읽어 결과처럼 적었다. 그래서 버렸다.
  `perf_record.sh`는 이제 lock을 기다리고(`LOCK_WAIT`, 기본 1800 s) 새로 쓰인 로그만 읽는다(`no new log`).
