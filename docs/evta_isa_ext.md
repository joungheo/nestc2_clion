# EVTA ISA 확장과 bitstream별 기능 (명령 비트, 켜는 방법, 다른 모델 돌리기)

원래 VTA ISA에 EVTA 작업 중 덧붙이거나 바꾼 비트 필드를 한곳에 모은다. 어떤 bitstream이 어떤 필드를 해석하는지,
vtalib이 언제 그 필드를 쓰는지, 다른 모델을 돌릴 때 무엇을 확인하는지를 이 문서만 보고 알 수 있게 쓴다.
명령 전체의 사용법(빌드, 실행, 측정)은 [evta_user_manual.md](evta_user_manual.md), 일의 경과는 [evta_overview.md](evta_overview.md)에 있다.
구조 그림: bias reset [구조 15](figs/evta_st15_bias_reset.svg), requantize [구조 16](figs/evta_st16_requantize.svg).

## 1. 한눈에

| 기능 | 명령 비트 | 해석하는 bitstream | vtalib이 켜는 조건 | 강제 | 도입 |
|---|---|---|---|---|---|
| `memory_type`, `alu_opcode` 2비트 | 원래 3비트 → 2비트 | 모든 EVTA bitstream(etri 포함) | 항상 | 없음 | 09-29 |
| bias reset | GEMM `bias_reset`(비트 140) | systolic, 이름에 `bias` 또는 `_rq` | bitstream 이름 | `EVTA_BIAS_RESET=0\|1` | 11-04 |
| requantize (out 쓰기) | GEMM `rq_*`(141~164), ALU `rq_*`(144~167) | systolic, 이름에 `_rq` | bitstream 이름 | `EVTA_REQUANT=0\|1` | 11-04 |

- "bitstream 이름"은 run 스크립트가 올린 bitstream 이름을 적어 두는 `/tmp/evta_bitstream`이다(§5).
- 확장 비트는 모두 원래의 pad 비트 자리에 있다. 그 기능이 없는 bitstream은 그 비트를 읽지 않는다. 그래서 같은 실행 파일이
  모든 bitstream에서 돈다. 기능은 vtalib이 bitstream 이름을 보고 켠다.
- 명령의 pad 비트는 2026-11-04부터 0으로 채운다(vtalib `dat_queue.h` `NextInsn`, HLS 시험 `test_lib.cc` `getGEMMInsn`).
  그 전에는 초기화되지 않아 쓰레기 값이 들어 있었다. 새 필드를 pad 자리에 더할 때는 이 초기화가 전제다.

## 2. 명령 형식 (256비트)

명령 하나는 256비트(64비트 word 4개)다. 비트 번호는 명령 안의 위치(word 0의 bit 0 = 0)이고,
single 설정(buffer 16/16/19/18)에서 `vtalib-zcu102-systolic/include/zcu102/vta/hw_spec.h`를 컴파일해 확인한 값이다.
multi 설정(16/15/18/17)에서는 앞쪽 필드 폭이 달라 위치가 다를 수 있지만, 필드 순서와 폭은 같다.

### 2.1 2비트 필드 (`memory_type`, `alu_opcode`)

- 원래 VTA는 3비트다. etri bitstream(`zcu102_1x16_i8w8a32_16_16_19_18.bit`)과 그 뒤의 모든 EVTA bitstream은 **2비트**다.
- hw_spec.h는 3곳 모두 2비트로 맞춰 둔다(`//#### JO` 표시). 3비트로 두면 LOAD/STORE 주소가 한 비트씩 밀려 결과가 0이 되거나 멈춘다.
- 그래서 `VTA_ALU_OPCODE_MUL`(4)과 `VTA_MEM_ID_MOVE`(5)는 인코딩할 수 없다. BNN / XP-GEMM 경로는 이 bitstream들에서 돌지 않는다.
- 다른 bitstream을 받으면 `vtabundles_xcmpl/hwtest/rawtest*.cpp` 프로브로 비트폭부터 확인한다.

| `memory_type` | 값 | `alu_opcode` | 값 |
|---|---|---|---|
| UOP | 0 | MIN | 0 |
| WGT | 1 | MAX | 1 |
| INP | 2 | ADD | 2 |
| ACC | 3 | SHR | 3 |

### 2.2 GEMM 명령 (`VTAGemInsn`)의 확장 비트

| 비트 | 필드 | 폭 | 뜻 |
|---|---|---|---|
| 140 | `bias_reset` | 1 | `reset_reg`와 같이 1이면 reset 단계가 0 대신 acc[src]를 acc[dst]에 쓴다(uop의 src가 acc 주소) |
| 141 | `rq_en` | 1 | 1이면 이 명령이 out에 쓰는 int8을 requantize한 값으로 |
| 142~143 | `rq_round` | 2 | 0: 반올림 없음(floor), 1: + 2^(shift−1)(half up), 2: + 2^(shift−1) − (acc < 0)(half away from zero) |
| 144~148 | `rq_shift` | 5 | 산술 오른쪽 shift 0~31 |
| 149~156 | `rq_lo` | 8 (signed) | clip 하한 |
| 157~164 | `rq_hi` | 8 (signed) | clip 상한 |
| 165~171 | `pad_0` | 7 | 비어 있음 |
| 192~255 | `pad_1` | 64 | 비어 있음 |

### 2.3 ALU 명령 (`VTAAluInsn`)의 확장 비트

| 비트 | 필드 | 폭 | 뜻 |
|---|---|---|---|
| 128~143 | `imm` | 16 (signed) | (원래 필드) 즉시값. SHR의 shift 양은 하위 5비트(−16~15) |
| 144 | `rq_en` | 1 | GEMM과 같다 |
| 145~146 | `rq_round` | 2 | |
| 147~151 | `rq_shift` | 5 | |
| 152~159 | `rq_lo` | 8 | |
| 160~167 | `rq_hi` | 8 | |
| 168~175 | `pad_0` | 8 | 비어 있음 |

### 2.4 LOAD / STORE 명령 (`VTAMemInsn`)

바뀐 것은 `memory_type`(2비트)뿐이다. 비트 160~255(`pad_0` 32, `pad_1` 64)는 비어 있다.

## 3. 각 기능의 동작

### 3.1 out에 쓰는 값

GEMM과 ALU는 단계마다 acc(int32)를 갱신하면서 같은 주소의 out(int8)에도 쓴다. STORE는 out을 DRAM으로 옮기기만 한다.
그래서 마지막으로 out을 쓴 명령이 그 주소의 결과를 정한다.

- `rq_en = 0`: out = acc의 하위 8비트(원래 VTA)
- `rq_en = 1`: out = clip((acc + r) >> shift, lo, hi)
  - r = 0 (round 0 또는 shift 0), 2^(shift−1) (round 1), 2^(shift−1) − (acc < 0) (round 2)
  - 덧셈은 32비트에서 넘치면 감긴다(ALU ADD와 같다). shift는 산술 shift다.
- acc는 바뀌지 않는다. 다음 명령은 requantize 전의 int32 값을 그대로 읽는다.
- **우회:** `rq_en = 0`이면 out은 전과 bit 단위로 같다(RTL은 반올림 0, shift 0으로 강제하고 clip 대신 하위 8비트를 고른다;
  HLS `requant()`는 원래 값을 돌려준다). 다만 GEMM의 out 쓰기와 완료(done)는 `rq_en`과 상관없이 3 cycle 늦어진 채다
  (pipeline 단이 늘 있다; 측정에서 차이는 보이지 않았다). 보드에서 `_rq` bitstream + `EVTA_REQUANT=0`의 층 출력이 `bias` bitstream과 같다.

하드웨어:
- GEMM(RTL `gemm_sa`): `vta-hw-zcu102-systolic/hardware/xilinx/rtl/sa_rmw.v`. acc 쓰기(S3) 뒤에 S4 덧셈, S5 shift, S6 clip을 두고
  out은 S6에서 쓴다. 명령 완료(done)와 dependence token도 S6에 맞췄다.
- ALU(HLS): `vta.cc`의 `requant()`, `alu()`의 두 루프(imm II = 1, reg II = 2). 같은 함수가 CSim의 GEMM 모델에도 쓰인다.
- HLS → RTL GEMM uop stream: 명령의 `rq_*` 24비트를 uop마다 싣는다(`vta.h` `VTA_SA_RQ` = `VTA_SA_UOP_B` + 6, §4).

### 3.2 bias reset

conv의 bias를 GEMM 누산의 시작값으로 쓴다. vtalib은 tile마다 bias(nF개 entry)를 acc의 빈 자리에 LOAD한 뒤 reset GEMM을
`bias_reset`으로 낸다. reset 단계는 acc[base + f·T + p] = acc[bias + f]를 쓰고, GEMM 뒤의 ALU ADD는 빠진다.
하드웨어는 `sa_rmw.v`(bias 행은 acc[src]를 읽고 행을 더하지 않음)와 `sa_ctrl.v`(tag에 bias, src)다.
자세한 내용은 [evta_runtime_compare.md](evta_runtime_compare.md) §7.16에 있다.

### 3.3 vtalib이 requantize를 쓰는 곳 (`vtalib-zcu102-systolic`)

API는 `VTARequant(cmd, en, round, shift, lo, hi)`다(`include/zcu102/vta/runtime.h`, `lib/zcu102/api.cc`). 부른 뒤에 push하는
GEMM과 ALU 명령에 그 값이 들어가고, `en = 0`으로 다시 끈다(`cmd_queue.h` `SetRequant` / `ApplyRequant`).

| 경로 (`lib/Bundle/VTABundle.cpp`) | 켜는 곳 | 값 | 빠지는 ALU 명령 |
|---|---|---|---|
| 일반 conv (`conv_kernel`, residual / pool 없음) | tile의 모든 GEMM(+ bias reset이 없으면 bias ADD) | round 0, shift, lo = relu ? 0 : −128, hi 127 | SHR, MAX, MIN |
| conv + max pool (`convolution_wo_tr_pool`, relu일 때) | 열 방향 pool MAX 세 번(ALU) | round 0, shift, 0, 127 | conv 뒤의 SHR, MAX, MIN |
| conv + residual (`convolution_wo_tr_res`) | residual identity GEMM | round 1(sumShr > 0), sumShr, relu ? 0 : −128, 127 | 끝의 ADD, SHR, MAX, MIN |
| fc (`fc_evta_core`) | fc GEMM 전부 | round 2(shift > 0), shift, −128, 127 | reset, ADD, MIN, MAX, ADD, ADD, SHR, MAX, MIN |

- residual 경로의 conv 부분(SHR / MAX / MIN, 그 뒤의 ADD 배수)은 acc 값으로 쓰이므로 그대로 둔다.
- max pool은 requantize 전의 acc에서 해도 결과가 같다. requantize(floor shift, clip)는 값의 순서를 바꾸지 않으므로
  max와 순서를 바꿔도 된다. pooled 영역의 시작값 0은 relu 역할을 한다.
- maxpool만 따로 하는 `maxpool_evta`는 int8 입력이라 requantize가 필요 없다.
- bias 없는 conv, shift 0 conv도 같은 식으로 처리한다.
- 바꾸지 않은 경로: `conv_kernel_scale`(`convolution_wo_tr_scale`, 채널별 scaling factor, VTASave.cpp가 내지 않음), `xp_*`(XP-GEMM,
  이 bitstream들에서 돌지 않음). 이 경로는 requantize bitstream에서도 이전처럼 ALU로 한다.

### 3.4 C 모델과 식의 출처

| 무엇 | 어디 |
|---|---|
| out 식 | `vta.cc` `requant()`, `sim/systolic_model.cc` `rqout()`, `sa_rmw.v` S4~S6 (셋이 같다) |
| 지금 ALU 명령과 같은지 | 2026-11-04, 5,750만 값에서 conv(SHR/MAX/MIN), residual 끝(ADD/SHR), fc(반올림 half away from zero)와 같음 |

## 4. HLS → gemm_sa uop stream

| 비트(`VTA_SA_UOP_B` 기준) | 필드 | 도입 |
|---|---|---|
| 0 ~ B−1 | dst, src, wgt (최종 주소) | 10-27 |
| B | reset | 10-27 |
| B+1 | last (명령의 마지막 단계) | 10-27 |
| B+2 | first (명령의 첫 단계, weight 다시 채움) | 10-27 |
| B+3, B+4 | g2l, g2s token | 10-27 |
| B+5 | bias (reset && bias_reset) | 11-04 |
| B+6 ~ B+29 | rq {hi, lo, shift, round, en} | 11-04 |

- `B` = log2(acc depth) + log2(inp depth) + log2(wgt depth). stream 폭은 `B + 30`비트를 8비트 단위로 올린 값이다
  (`vivado.tcl` `CONFIG.UB`, single·multi4 각각).
- RTL 쪽 폭: `gemm_sa.v` / `sa_ctrl.v` `s_uop_tdata[DW+SW+WW+29:0]`, `gemm_sa_bd.v`는 `gen_gemm_sa_bd.py`로 다시 만든다.

## 5. bitstream과 vtalib의 짝

| bitstream (`/lib/firmware`) | EVTA | bias reset | requantize | 실행 파일 |
|---|---|---|---|---|
| `zcu102_1x16_i8w8a32_16_16_19_18.bin` (etri) | 1 | × | × | `build/`, `build_prof/`, `build_timed/` (`vta/vtalib`) |
| `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_380mhz.bin` 등 HLS GEMM | 1~4 | × | × | 위와 같음 |
| `..._16_16_19_18_sa_400mhz_alu1_epr.bin` | 1 | × | × | `build_sys*` (`vtalib-zcu102-systolic`) |
| `..._16_16_19_18_sa_400mhz_bias.bin` | 1 | ○ | × | `build_sys*` |
| `..._16_16_19_18_sa_400mhz_rq.bin` | 1 | ○ | ○ | `build_sys*` |
| `..._16_15_18_17_4evta_sa_400mhz_alu1.bin` | 4 | × | × | `build_multievtax4_sys*` |
| `..._16_15_18_17_4evta_sa_400mhz_bias.bin` | 4 | ○ | × | `build_multievtax4_sys*` |
| `..._16_15_18_17_4evta_sa_396mhz_rq.bin` (395.8 MHz) | 4 | ○ | ○ | `build_multievtax4_sys*` |
| `..._16_15_18_17_4evta_sa_400mhz_rq.bin` | 4 | ○ | ○ | `build_multievtax4_sys*` |

- **`vtalib-zcu102-systolic`은 systolic bitstream에만 쓴다.** uop 순서가 HLS GEMM과 다르다.
- **기능은 bitstream 이름으로 켠다.** `run_all.sh`, `run_prof.sh`, `load_bit.sh`, `hwtest/*.sh`가 `/tmp/evta_bitstream`에 이름을 쓴다.
  vtalib이 그 이름에서 `bias` / `_rq`를 찾는다. 다른 방법으로 bitstream을 올렸으면 이름을 직접 쓰거나 환경 변수로 강제한다.
  - 이름에 `bias`나 `_rq`가 있으면 bias reset, `_rq`가 있으면 requantize가 켜진다.
  - 로드 실패(`FAILED...`)거나 파일이 없으면 둘 다 꺼진다.
- **기능이 없는 bitstream에서 강제로 켜면 결과가 틀린다**(`EVTA_REQUANT=1`이면 out이 하위 8비트로 남아 쓰레기, bias reset은 bias가 빠진다).
  반대로 켤 수 있는 bitstream에서 `=0`으로 끄는 것은 안전하다. 이전 경로와 비교할 때 쓴다.
- 기본 bitstream은 `vtabundles_xcmpl/hw*/script/run_all.sh` 맨 위의 `BIN`(HLS GEMM)과 `SYS_BIN`(systolic, `-s _sys*`)이다.
  다른 것은 `BIN=<file>`로 준다(`-s _sys*`에서도 `BIN`이 우선).

## 6. 다른 모델 돌리기 (VGG, Resnet50, MobileNet 등)

### 6.1 순서

1. **bundle 소스 만들기**(보드, simulator 빌드): `cd Debug2_remote && ./build_sim_bundles.sh <target>`
   ([evta_user_manual.md](evta_user_manual.md) §2). 컴파일러 옵션은 같은 문서 §2.2. 조건이 안 맞는 층은 옵션과 상관없이 이전 경로로 간다.
2. **보드용으로 복사, 빌드:** `vtabundles_xcmpl/collect_hw_bundles.py [Dir]`, 그다음 `build_sysprof` / `build_systimed`에서
   `make -j2 <target>`(§3). 이 dir들은 `-DVTALIB=<root>/vtalib-zcu102-systolic`로 만들어져 있다.
3. **기준값 만들기:** 같은 실행 파일을 기능이 없는 systolic bitstream(또는 같은 bitstream에서 `EVTA_REQUANT=0 EVTA_BIAS_RESET=0`)으로 돌려
   `EVTA_DUMP_CONV=<dir_old>`로 층 출력을 남긴다.
4. **새 기능으로 돌리기:** `_rq` bitstream으로 `EVTA_DUMP_CONV=<dir_new>`를 남긴다.
5. **비교:** `cmp -r` 또는 `diff -rq <dir_old> <dir_new>`. 파일 수와 내용이 모두 같아야 한다. fused 경로는 `EVTA_RES_CHECK=1`로 실행 중에도 비교한다.
6. **golden / 분류 결과:** `run_prof.sh -s _systimed -n 2 <Dir>`의 summary 줄(golden PASS, 또는 분류 index와 확률)을 본다.
7. **시간:** `run_prof.sh -s _sysprof -n 25 -r <Dir>`로 두 bitstream을 번갈아 잰다.

```bash
# 보드, vtabundles_xcmpl/ (sudo는 sudo -n /bin/sh -c "..." 형태)
OLD=vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_400mhz_alu1_epr.bin
NEW=vtahw_zcu102_1x16_i8w8a32_16_16_19_18_sa_400mhz_rq.bin
sudo -n /bin/sh -c "mkdir -p /tmp/d_old /tmp/d_new; \
  EVTA_DUMP_CONV=/tmp/d_old BIN=$OLD hw/script/run_prof.sh -s _systimed -n 1 <Dir>; \
  EVTA_DUMP_CONV=/tmp/d_new BIN=$NEW hw/script/run_prof.sh -s _systimed -n 1 <Dir>"
diff -rq /tmp/d_old /tmp/d_new && echo IDENTICAL
sudo -n /bin/sh -c "BIN=$NEW hw/script/run_prof.sh -s _systimed -n 2 <Dir>; cat hw/result/systimed_summary.txt"
```

### 6.2 모델마다 확인할 것

| 확인 | 왜 | 어떻게 |
|---|---|---|
| 층의 scale 비가 2의 거듭제곱인가 | requantize는 shift만 한다(곱셈 없음) | `EVTA_REPLAY_DUMP=<dir>`의 명령(`rq` 필드), 또는 bundle `.cpp`에서 그 층이 부르는 함수 |
| 활성화가 relu / 없음인가 | lo는 0 또는 −128, hi는 127만 쓴다. relu6 같은 위쪽 clip은 아직 vtalib이 내지 않는다 | bundle `.cpp`의 relu 인자 |
| shift가 0~31인가 | `rq_shift` 5비트. ALU SHR 경로는 −16~15 | 위와 같음 |
| depthwise conv | EVTA에서 도는지 확인하지 않았다(MobileNet) | bundle `.cpp`에서 그 층이 CPU 함수인지 EVTA 함수인지 |
| 2026-11-04 이전 bitstream | `rq_*`를 해석하지 않는다 | 이름에 `_rq`가 없으면 vtalib이 쓰지 않는다 |

### 6.3 새 필드를 더할 때

1. 빈 자리: GEMM 165~171, 192~255 / ALU 168~175, 192~255 / LOAD·STORE 160~255.
   필드 폭의 합이 원래 pad 폭과 같게 나눈다(64비트 경계를 넘지 않게).
2. 3곳의 hw_spec.h를 같이 바꾼다: `vta-hw-zcu102-systolic/include/vta/`, `vta-hw-zcu102-multi4/include/vta/`,
   `vtalib-zcu102-systolic/include/zcu102/vta/`. 위치는 작은 C 프로그램으로 비트를 찍어 확인한다(§2의 표가 그렇게 만든 것이다).
3. 하드웨어가 그 필드를 해석하는 bitstream 이름에 표식을 넣고(`bias`, `_rq`처럼), vtalib은 그 이름을 보고 켠다(`evtaBiasReset`,
   `evtaRequant`). 환경 변수 강제도 같이 둔다.
4. 검증 순서: C 모델(`sim/systolic_model.cc`) → RTL testbench(`rtl/tb/run_xsim.sh`, GAP·BP 30 %도) → HLS CSim(`make ip CSIM=1`)
   → 보드(probes, golden, `EVTA_DUMP_CONV` byte 비교).

## 7. 검증 기록

| 기능 | C 모델 | RTL | CSim | 보드 |
|---|---|---|---|---|
| bias reset (11-04) | `bias`, `biashazard` | 96/96 | 0 errors | single golden 30, dumps 동일 / multi4 golden 3, dumps 80개 동일 |
| requantize (11-04) | 무작위 300 | 49 vector × 2 = 98/98 | 0 errors | single: probes 10/10, golden 27 PASS + 분류 4개 같음, dumps 146개 동일, `EVTA_RES_CHECK` 0, `EVTA_REQUANT=0` 우회 동일 | / multi4(395.8 MHz): probes 40/40, golden 3 PASS, dumps 80개가 `bias`와 동일

## 8. 다음 ISA 개정: ALU 확장 (설계, 2026-11-10, 아직 구현 전)

DeiT의 LayerNorm / softmax / GELU, 2의 거듭제곱이 아닌 scale, avgpool, YOLO 계열 activation을 EVTA에 두려고 ALU를 넓힌다.
H1(MUL), H2(LUT), H4(행 reduction)의 일부(`deit_optimization.md` §6.2)를 **한 번의 개정**으로 묶는다.
bitstream과 vtalib의 짝이 한 번만 바뀌게 하려는 것이다.

### 8.1 방식: `alu_opcode`는 그대로, pad에 `alu_op_ext`

- 2비트 `alu_opcode`를 3비트로 넓히면 그 뒤 필드(`use_imm`, `imm`, `rq_*`)가 모두 밀린다. 그러면 모든 bitstream과 vtalib을 같이 바꿔야 한다.
- 대신 **pad 자리(ALU 168~175)에 `alu_op_ext` 2비트**를 둔다. 연산 번호는 `alu_op_ext << 2 | alu_opcode`(4비트, 16개)다.
- `alu_op_ext = 0`이면 지금의 MIN / MAX / ADD / SHR 그대로다.
  - pad는 11-04부터 0으로 채운다(§1).
  - 그래서 **이전 실행 파일 + 새 bitstream**은 지금과 똑같이 돈다.
- **새 실행 파일 + 이전 bitstream:**
  - vtalib은 bitstream 이름에 `_alux`가 있을 때만 새 연산을 쓴다(`_rq`와 같은 방식, `EVTA_ALUX=0|1`로 강제).
  - 없으면 그 연산을 CPU 함수로 계산한다.
- **명령 비트는 번들 `.cpp`에 없다.** vtalib이 실행 때 만든다(JIT / replay). 그래서 vtalib만 바뀌는 경우는 다시 링크만 하면 된다.
  - 컴파일러가 새 연산을 쓰게 바뀐 모델(예: avgpool을 EVTA로)만 번들을 다시 생성한다.

| 비트 | 명령 | 필드 | 폭 | 뜻 |
|---|---|---|---|---|
| 168~169 | ALU | `alu_op_ext` | 2 | 연산 번호의 위 2비트 (§8.2) |
| 170 | ALU | `lut_en` | 1 | out에 쓰는 int8을 표로 바꿈(requantize 뒤) |
| 171~175 | ALU | `pad_0` | 5 | 비어 있음 |
| 165 | GEMM | `lut_en` | 1 | GEMM이 out에 쓰는 int8을 표로 바꿈(GELU를 fc1 출력에 바로) |
| 166~171 | GEMM | `pad_0` | 6 | 비어 있음 |

위치는 single 설정에서 정한 것이다. 넣을 때 §6.3 순서대로 세 곳의 hw_spec.h에서 비트를 찍어 확인한다.

### 8.2 연산 표

| 번호 | 이름 | 식 (lane마다, src = imm 또는 acc[src]) | 쓰는 곳 |
|---|---|---|---|
| 0–3 | MIN, MAX, ADD, SHR | 지금과 같음. SHR에 음수 shift(−16 … −1)를 주면 왼쪽 shift라서 **SHL은 따로 두지 않는다** | |
| 4 | **MUL** | dst = dst × src[15:0] (32비트로 자름). requantize(`rq_*`)와 같이 쓰면 (dst × m + round) >> s, clip | 2의 거듭제곱이 아닌 scale(residual, concat, depthwise per-channel), **avgpool ÷n**, LayerNorm ×γ, softmax ×(1/합), SE / gating의 원소별 곱 |
| 5 | **SUB** | dst = dst − src (imm이면 ADD −imm과 같아 vector 형태가 주 용도) | LayerNorm x − 평균, softmax x − max |
| 6 | **CPY** | dst = src | concat, nearest upsample(YOLO), acc 안의 재배치 |
| 7 | **LUT** | dst = ext(T[dst[7:0]]), imm 0번 비트 = 부호 확장 여부. out도 같은 값 | exp(softmax, x − max 뒤), GELU / SiLU / sigmoid / tanh / hard-swish, 역수 · rsqrt의 가수 |
| 8 | **LUTSET** | T[16·imm + lane] = src[lane][7:0] (acc 한 행 → 표 16칸, 16번이면 256칸) | 표 채우기. 새 memory_type이 필요 없다 |
| 9 | **RSUM** | dst[모든 lane] = Σ lane src | LayerNorm 평균 / 분산, softmax 합. 채널 블록 사이는 ADD로 먼저 더함 |
| 10 | **RMAX** | dst[모든 lane] = max lane src | softmax의 행 max |
| 11 | **CLZ** | dst = 부호 비트를 뺀 앞자리 0의 수 | 1/x, 1/√x: CLZ → SHR(vector)로 정규화 → LUT → 지수만큼 shift |
| 12–15 | 예약 | | |

- **avgpool(n이 2의 거듭제곱이 아님):** 창의 합은 ADD(maxpool의 MAX와 같은 uop 패턴), 나누기는 MUL m + `rq` shift k 한 번이다.
  - m = round(2^k / n)이고, 결과가 CPU와 같아지는 k를 컴파일 때 모든 합의 범위에서 확인해 고른다.
- **LayerNorm (EVTA 배치에서 한 token = 채널 블록 × 16 lane):**
  1. 블록 ADD → RSUM(합) → MUL(1/C) = 평균
  2. SUB
  3. MUL(제곱) → 블록 ADD → RSUM → CLZ / LUT = rsqrt
  4. MUL
  5. MUL γ + ADD β, `rq`
- **softmax:**
  1. 블록 MAX → RMAX
  2. SUB 후 `rq`로 int8 clip
  3. LUT exp
  4. 블록 ADD → RSUM
  5. 역수(CLZ / LUT)
  6. MUL, `rq`
- 식이 정수로 바뀌므로 값은 지금 CPU 함수와 다를 수 있다. `deit_optimization.md` §6.1의 정수 LayerNorm / softmax를 먼저 CPU에서 정하고, ImageNet 정확도를 확인한 뒤 그 식을 그대로 하드웨어에 옮긴다.

### 8.3 ALU로 안 되는 것

| 연산 | 어디서 | 이유 |
|---|---|---|
| **matmul** | **GEMM** (지금도 `matmul_evta`) | 내적은 16×16 MAC 배열이 한다. ALU는 lane마다 연산 하나라 256배 느리다. matmul에 필요한 하드웨어는 연산이 아니라 **데이터 이동**이다: K / V를 weight 배치로 쓰는 STORE 옵션 또는 out → wgt 경로(H3, MOV). 지금은 CPU가 실행 때 묶는다 |
| out → inp / wgt 이동 (MOV) | 명령 opcode 6 (ALU 밖) | `vta_to_evta_diff.md` §4. opcode 5는 ETRI 헤더의 XP_GEMM이 쓰고 있다 |
| 전치, 배치 변환 | STORE 옵션 (H3) | 주소 생성의 문제다 |
| 넓은 범위의 exp / 나누기 | LUT + CLZ로 근사 | 정밀도는 int8 출력에 맞춘다 |

### 8.4 하드웨어 비용 (추정)

| 부분 | 무엇 | 자원 | 400 MHz에서의 위험 |
|---|---|---|---|
| MUL | lane마다 32 × 16 곱셈 | DSP48E2 16개 (single 기준 여유 많음, multi4는 ×4) | DSP pipeline 3단. II는 그대로, latency만 는다 |
| SUB, CPY, CLZ | lane마다 | LUT 수백 개 | 작음 |
| RSUM / RMAX | 16 lane 32비트 덧셈 / 비교 tree | LUT 약 1,000개, 4단 pipeline | 중간 |
| LUT / LUTSET | lane마다 256 × 8 표 사본(16개를 같이 읽으려고) | LUTRAM 약 512개 | 작음 |
| GEMM 출력 LUT | `sa_rmw.v` S6 뒤 같은 표 사본 | LUTRAM 약 512개 | 작음. ALU 표와 따로 두거나 LUTSET이 둘 다 씀 |

- 가장 큰 위험은 자원이 아니라 timing이다: multi4의 400 MHz와 Y219 배선(§7, `vta_synth_notes.md` §3).
- ALU vector 연산(src와 dst를 둘 다 읽음)은 지금처럼 II = 2다. imm 연산은 II = 1이다.

### 8.4.1 GEMM과 ALU는 동시에 돌지 않는다: 어디에 둘 것인가

- **지금의 순서:** compute는 명령 큐를 순서대로 처리한다. GEMM은 `gemm_sa`로 보내고, 다른 명령(ALU 포함) 앞에서는 나가 있는 GEMM이 모두 끝나기를 기다린다(`vta.cc` compute 앞 주석, `gemm_done`).
  - `acc_mem`의 두 포트도 `gemm_sa`가 돌 때는 그쪽으로 묶인다.
  - 그래서 ALU는 그 tile의 GEMM이 끝난 뒤에 돈다. 겹치는 것은 LOAD / STORE와 compute(DAE, virtual thread)뿐이다.
- **비용:** requantize 이전 Resnet18에서 ALU가 EVTA 시간의 17.5 %였다(4.63 / 26.47 ms, `evta_runtime_compare.md` §7.14).
  - 그 대부분을 **GEMM이 out에 쓰는 길 위로** 옮겨 없앴다: bias reset, requantize(`sa_rmw.v` S4–S6).
- **그래서 연산을 두 종류로 나눈다:**

| 종류 | 예 | 어디서 | 추가 시간 |
|---|---|---|---|
| 원소 하나만 보는 연산 | requantize, clip, relu, LUT(GELU, SiLU), **per-tensor scale 곱** | GEMM 출력 경로(`rq_*`, `lut_en`, 더할 것: GEMM pad 192~255에 `rq_mul` 16비트) | 0 (GEMM 쓰기와 같이) |
| 한 token의 모든 채널이 필요한 연산 | 평균, 분산, 행 max / 합, x − 평균, ×rsqrt | ALU 명령(별도 pass) | 있음 |

- **별도 pass의 크기:** DeiT LayerNorm 한 번은 197 token × 12 블록 = 2,364 vector다. 약 15개 연산 × 1–2 cycle이면 400 MHz에서 약 0.1 ms이고, 25번이면 약 3 ms다.
  - 겹치지 않아도 지금 CPU(LayerNorm 5.6 + 앞뒤 배치 변환 5.4 ms, `-t 4`)보다 짧다.
- **ALU를 GEMM과 겹치려면 (나중, ISA는 그대로):**
  - `acc_mem`을 virtual thread별 두 bank로 나눈다. virtual thread는 이미 acc의 절반씩을 쓴다.
  - 그러면 `gemm_sa`가 bank 0에 쓰는 동안 ALU가 bank 1을 처리할 수 있다.
  - compute는 다른 bank의 GEMM을 기다리지 않게 바꾼다.
  - BRAM 양은 같고, 바뀌는 것은 포트 mux와 compute의 대기 조건이다.
  - ALU pass가 GEMM보다 짧으면 그 시간이 숨는다. 컴파일러가 다음 Linear의 GEMM과 엇갈려 배치해야 한다.

### 8.6 정수 LayerNorm / softmax 식과 정확도 (CPU에서 확인, 2026-11-10)

§8.2의 연산(ADD, MUL, SUB, RSUM / RMAX, CLZ, LUT)만으로 계산하는 식을 bundle CPU 함수에 넣었다. 실행할 때 환경 변수로 켜고, 정확도를 ImageNetV2로 쟀다.
- 코드: `lib/Backends/VTA/VTASave.cpp` `nestc_cpu::layerNormI8`(`NESTC_LN_INT=1`), `softmaxInt`(`NESTC_SM_INT=1|2`)
- 하드웨어로 옮길 때 이 C 코드를 기준(golden)으로 쓴다.

**LayerNorm** (한 token = C 채널, x는 int8)

| 단계 | 정수 계산 | ALU 연산 | 크기 (C ≤ 1024) |
|---|---|---|---|
| 1 | S = Σx, Q = Σx² | MUL(제곱), 블록 ADD, RSUM | Q ≤ 2^24 |
| 2 | W = C·Q − S² (= C²·분산, 정확) | MUL imm, MUL vector, SUB | < 2^31 |
| 3 | 1/√W: 짝수 shift로 W를 [2^14, 2^16)에 맞추고(CLZ), 위 8비트로 8비트 표 T(255/√u)를 찾음 | CLZ, SHR(vector), LUT | 상대 오차 ≤ 0.4 % |
| 4 | p = (C·x − S)·T >> 8 | MUL, SUB, SHR | < 2^16 |
| 5 | y = (p·G[l] + round) >> (gs + sh/2 − 8) + b[l], clip | MUL(vector, G = γ / out scale, 14비트), `rq` | < 2^31 |

- 평균을 따로 구해 빼지 않는다. 처음 식(평균을 1/4 LSB로 반올림)은 분산이 작은 행에서 5 LSB까지 틀렸다.
- (C·x − S) / √(C·Q − S²) = (x − 평균) / 표준편차이므로, 1/√ 표 말고는 정확하다.
- 무작위 197 × 192 행에서 float 계산과 최대 1 LSB 차이였다(72 %는 같음).

**softmax** (한 행, 입력 int8 score, scale s)

| 단계 | 정수 계산 | ALU 연산 |
|---|---|---|
| 1 | m = max x | 블록 MAX, RMAX |
| 2 | e = E[min(m − x, 255)], E[k] = round(2^b·exp(−k·s)) | SUB, LUT (b = 8 또는 16) |
| 3 | S = Σe, 1/S: CLZ로 [2^15, 2^16)에 맞추고 8비트 표 | 블록 ADD, RSUM, CLZ, LUT |
| 4 | p = e·R >> (shift) → 다음 텐서의 scale로 | MUL, `rq` |

- exp 표가 8비트면 작은 값이 0으로 반올림된다. 그래서 큰 확률이 최대 2.6 step(1/128 단위) 커진다.
- 16비트면 0.42 step이다. 하드웨어 LUT는 출력 16비트 옵션을 둘 만하다. 다만 정확도에서는 차이가 보이지 않았다(아래 표).

**정확도** (보드, systolic 400 MHz `_rq`, ImageNetV2 앞 9,000장. 같은 번들에 환경 변수만 바꿈)

| 설정 | top-1 |
|---|---|
| 지금 (Glow의 int8 LayerNorm 체인, float softmax) | 56.17 % |
| LayerNorm float (`NESTC_LN_FLOAT=1`) | 56.61 % |
| **LayerNorm 정수식** (`NESTC_LN_INT=1`) | **56.54 %** |
| softmax 정수, 8비트 exp 표 (`NESTC_SM_INT=1`) | 56.17 % |
| softmax 정수, 16비트 exp 표 (`NESTC_SM_INT=2`) | 55.98 % |
| **LayerNorm 정수 + softmax 정수 16비트** | **56.49 %** |

- 9,000장의 표준오차는 약 ±0.5 %p다. 정수식은 정확도를 잃지 않는다.
- 그래서 이 식을 그대로 하드웨어 ALU 확장(§8.2)의 기준으로 삼는다.
- 정확도 손실은 대부분 다른 데서 온다(`deit_optimization.md` §6.6).
- power-of-2 scale 몫(약 −2.3 %p)은 GEMM 출력 경로의 `rq_mul`(§8.4.1)로 줄일 수 있다.
  - 다만 2026-11-11에 소프트웨어만으로 거의 다 되찾았다: activation scale은 자유롭게, EVTA 연산의 비율만 2의 거듭제곱으로(`-vta-pow2-ratio`).
  - 그래서 DeiT에는 `rq_mul`가 필요 없다(`deit_optimization.md` §6.6.3).

**2026-11-11 수정과 결과**
- 출력 bias는 shift 전의 고정소수점(2^t)에 더한다. 반올림을 한 번만 하는 것이고, conv bias와 같은 방식이다.
  - 하드웨어에서는 bias 벡터를 acc에 ADD한 뒤 `rq` shift를 하면 된다.
  - 전에는 bias를 int8 단위로 먼저 반올림했다. b × scale 비가 .5인 채널에서 출력의 25 %가 1씩 달랐다.
- `NESTC_LN_INT=2`는 1/√ 표를 16비트로 하고 하위 8비트로 선형 보간한다(LUT 두 번, MUL, ADD). 정확도는 8비트 표와 같아서, **하드웨어 LUT는 8비트 출력이면 된다.**
- scale 자유 번들(9,000장)에서의 정확도:

| 설정 | top-1 |
|---|---|
| LayerNorm float | 60.41 % |
| 정수 LayerNorm 8비트 표 (이제 기본, `NESTC_LN_INT=0`이면 Glow int8 체인 59.71 %) | **60.29 %** |
| 정수 LayerNorm 16비트 보간 표 | 60.30 % |
| 정수 LayerNorm + 정수 softmax 16비트 exp | 60.13 % |

- CPU에서는 NEON int32로 짰다. 1코어 8.0 ms로, Glow 체인 20.2 ms보다 빠르다.

### 8.5 일의 양과 순서 (추정)

| 단계 | 내용 | 기간 |
|---|---|---|
| 1 | 정수 LayerNorm / softmax 식을 CPU에서 정하고 ImageNet 확인 (소프트웨어, `deit_optimization.md` §6.1) | **11-10 끝남 (§8.6)** |
| 2 | `hw_spec.h` 세 곳, C 모델(`systolic_model.cc`), vtalib 시뮬레이터 `sim_driver.cc`에 연산 12개 | 2–3일 |
| 3 | HLS `vta.cc` `alu()` / `alu_lane()`, `sa_rmw.v` 출력 LUT, `vta_test.cc` `alu_test`에 새 연산, RTL testbench, CSim | 1–1.5주 |
| 4 | single `_alux` bitstream(합성 약 1시간 × 몇 번), 보드 probe, golden, dump 비교 | 2–4일 |
| 5 | multi4 bitstream (Y219 flow) | 1–3일 |
| 6 | vtalib API(`VTAPushALUOp` ext 인자, `VTALutSet`), bitstream 이름 검사, CPU 대체 경로 | 2–3일 |
| 7 | 컴파일러 / bundle: avgpool, 2^n이 아닌 residual scale, GELU 출력 LUT, LayerNorm, softmax, SiLU (연산마다) | 연산마다 1–3일, 합 2–3주 |

- 하드웨어와 runtime(2–6)은 3–4주, 컴파일러까지 합하면 6–7주로 본다.
- 효과(추정, DeiT `-t 4` 57.9 ms 기준):
  - GELU 5.2, LayerNorm 5.6, softmax 3.9 ms가 EVTA로 가고, 그 앞뒤 배치 변환 5.4 ms도 없어진다(CPU를 오가지 않음).
  - 합해서 약 −15–20 ms다. EVTA 쪽은 ALU 시간만큼 는다.
- CNN 쪽 효과는 작다: avgpool 0.2–1 ms, 2^n이 아닌 scale 때문에 CPU에 남던 층. 대신 YOLO(SiLU, upsample, concat)와 MobileNetV3 / EfficientNet(hard-swish, SE)은 이 확장이 있어야 EVTA에 올라간다.
