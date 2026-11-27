# 하드웨어 판 관리: 어느 bitstream이 어느 소스와 vtalib의 짝인가 (2026-11-27)

하드웨어는 같은 dir(`vta-hw-zcu102-systolic`, `vta-hw-zcu102-multi4`)에서 계속 고친다. vtalib도 같은 dir(`vtalib-zcu102-systolic`)에서 따라 고친다.
그래서 예전 bitstream을 다시 돌릴 때 필요한 것을 이 문서에 모았다.

- bitstream마다 무엇으로 만들었는지(소스 commit, make 명령, WNS) 남기는 법
- 어떤 vtalib / 실행 파일로 돌리는지
- 정말 예전 vtalib가 필요할 때 어떻게 하는지

판마다 더한 기능과 시간은 [evta_isa_ext.md](evta_isa_ext.md) §5에, 이름 조각의 뜻은 그 절의 "bitstream 이름 읽는 법"에 있다.

용어:

- **tag**: git에서 commit 하나에 붙인 이름(`git tag`). 지워지지 않는 책갈피다.
- **worktree**: 같은 저장소의 다른 commit을 다른 dir에 꺼낸 것(`git worktree add`). 지금 트리를 건드리지 않는다.
- **보관 dir**: 보드의 `/home/xilinx/JO/npuprg/bit/`. 문서에 `bit/…`로 적힌 곳이다. bitstream마다 dir 하나가 있고, 그 안에 `.bin` / `.bit` / 보고서 / `README.md` / `build_info.txt`가 있다.
- **실행 파일 세트**: 시험한 실행 파일을 weights, int8 입력과 함께 보관한 것(`bit/exe_sets/<이름>/`).

## 1. 원칙: 최신 vtalib가 예전 bitstream도 돌린다

- vtalib-systolic은 실행할 때 `/tmp/evta_bitstream`(실행 스크립트가 쓰는 bitstream 이름)을 읽는다. 이름 조각으로 기능을 켜고 끈다.
  - 기능과 이름 조각: `_rq`, `bias`, `_alux<k>`, `_amul`, `_dw`, `<n>evta`
  - 끄는 변수: `EVTA_REQUANT`, `EVTA_BIAS_RESET`, `EVTA_ALUX`, `EVTA_ALUMUL`, `EVTA_DWU`
- 새 ISA 비트는 명령의 빈 비트(pad)에 넣었다(`bias_reset`, `rq_*`, `alu_op_ext*`, `dw_win`, uop `dw` / `emit`). 예전 하드웨어는 그 비트를 보지 않는다.
- 그래서 **대부분은 최신 실행 파일에 `BIN=<예전 판>`만 주면 된다**(§4.1).
  - 예: 2026-11-26의 실행 파일 하나로 `_amul` / `_amul_dw` / `_alux3` / `_alux3_dw`를 모두 돌렸고, 모두 0 differ였다.
- **이 원칙을 지키려고 새 기능마다 이렇게 한다.**
  1. 이름 조각을 새로 정한다.
  2. 빈 비트를 쓴다.
  3. 끄는 `EVTA_*` 변수를 둔다.
  4. 예전 판에서는 예전 경로로 돈다.
- **같은 이름으로 동작이 바뀌는 하드웨어 변경은 하지 않는다.**
  - 예: ALU 깊이가 바뀌어 hazard 규칙이 달라지는 것.
  - 해야 하면 이름을 바꾼다. `_rq2`처럼 고친 판은 vtalib가 같은 것으로 본다.
- 깨지는 경우가 생기면 §4.3으로 예전 vtalib를 다시 빌드한다.
  - 예: 명령 필드의 자리나 폭을 바꿈(2026-09의 `memory_type` 3 → 2 bit 같은 것).
- 버퍼 크기(single 16/16/19/18, multi 16/15/18/17)는 판 종류마다 정해져 있다. 빌드 dir(`BUNDLE_SET`)이 고르므로 판 버전과는 따로다.

## 2. 기록: 무엇으로 만든 bitstream인가

| 무엇 | 어디에 | 누가 쓰나 |
|---|---|---|
| bitstream의 소스 commit, 고치지 않고 빌드한 파일(diff), make 변수, WNS / WHS, 클럭, 자원 | `build_info.txt`: `make bin`이 export dir에 `<이름>.build_info.txt`로, 보관 dir에는 `build_info.txt`로 | `utils/hwver/build_info.sh`(Makefile이 부른다, 2026-11-27부터) |
| 사람이 읽는 요약 | 보관 dir의 `README.md` | 사람 |
| 기본 판의 소스 | git tag `hw-<종류>-<기능>-<날짜>` | 사람(§3) |
| 실행 파일이 어느 vtalib commit인가 | 실행 파일 안 `NESTC_BUILD` 문자열(`run_prof.sh`, `bininfo.sh`가 출력) | `build_stamp.cmake`(2026-11-24부터) |
| bundle이 어느 컴파일러 commit인가 | bundle `.cpp`의 `// model-compiler:` 줄 | model-compiler |
| 시험한 실행 파일 | `bit/exe_sets/<이름>/`의 `MANIFEST.txt`(md5, `NESTC_BUILD`, model-compiler 줄) | `vtabundles_xcmpl/save_exe_set.sh` |

**`make bin`이 자동으로 하는 것** (네 하드웨어 트리 모두: systolic, multi4, single, multi2):

1. HLS를 시작할 때 `<hls dir>/src_hls.txt`를 쓴다. 거기에 git commit과 commit 안 된 하드웨어 파일 목록이 들어간다. 그런 파일이 있으면 `src_hls.txt.diff`도 쓴다.
2. Vivado를 시작할 때 `<vivado dir>/src_vivado.txt`를 같은 방식으로 쓴다.
3. `.bin`이 생기면 `export/<이름>.build_info.txt`를 쓴다.
   - 내용: 두 stamp, `VTA_*` make 변수(`VTA_SYSTOLIC`, `VTA_DWU`, `VTA_FREQ`, `VTA_IMPL_STRATEGY` …), 타이밍 요약, 클럭, LUT / FF / BRAM / DSP.
   - diff 파일도 옆에 복사한다.

**사람이 하는 것** (make 뒤의 단계, 보드에 올릴 때):

```bash
# 배선 뒤 단계(phys_opt, mmcm_set.tcl, multi4 y219 흐름)를 거친 최종 .bin은 그 단계의 보고서로 다시 쓴다
utils/hwver/build_info.sh write <최종 .bin> --src <hls dir>/src_hls.txt --src <vivado dir>/src_vivado.txt \
    --timing <마지막 timing.rpt> --util <util.rpt> --step "<단계 1과 WNS>" --step "<단계 2와 WNS>" --out <dir>/build_info.txt
# 보드 보관 dir에 .bin / .bit / 보고서 / build_info.txt / README.md를 둔다 (bit/evta_<종류>_<기능>_<MHz>MHz_<yymmdd>/)
# 보드 시험이나 쓰임이 정해지면 줄을 더한다 (key: 날짜 내용)
utils/hwver/build_info.sh add bit/<dir>/build_info.txt test "probes 40/40, EVTA_RES_CHECK 7 models 0 differ"
utils/hwver/build_info.sh add bit/<dir>/build_info.txt role "SYS_BIN of hw_multievtax4 since 2026-11-26"
utils/hwver/build_info.sh add bit/<dir>/build_info.txt exe  "exe_sets/<set>: <결과>"
```

- 기본 판이 되면 §3에 줄을 더하고, 빌드에 쓴 commit에 tag를 붙인다(`git tag -a <이름> <commit> -m "<bin> ..."; git push origin <이름>`).
- 2026-11-27 전의 판은 stamp가 없었다. 주요 판 8개(§3 표의 systolic 판)의 `build_info.txt`는 README와 문서, git 기록에서 채워 넣었다(`note: backfilled` 줄).
  - 소스는 날짜로 맞춘 tag commit이다. 빌드할 때 작업 트리가 그 commit과 바이트까지 같았는지는 확인할 수 없다.

## 3. 주요 판 (보드 `/lib/firmware`, 보관 dir `bit/`)

| bitstream (`vtahw_zcu102_1x16_i8w8a32_` 뒤) | md5 앞 8자 | 소스 tag (= commit) | 만든 법, WNS | vtalib-systolic | 쓰임 | 보관 dir, 실행 파일 세트 |
|---|---|---|---|---|---|---|
| `16_16_19_18_380mhz.bin`, `16_15_18_17_4evta_380mhz.bin` 등 | | `hw-hls-380-261028` (`709b616`) | HLS GEMM 380 MHz, HP 333 | (`vta/vtalib`) | `BIN` 기본(`run_all.sh`, HLS vtalib) | `bit/evta_single_380MHz_pass_261028/` 등 |
| `16_16_19_18_sa_400mhz_rq.bin` | 56645900 | `hw-single-sa-rq-261104` (`8ff0e4b`) | ExplorePostRoutePhysOpt, +0.039 | `d26c3e3` 이후 | single 기본 11-04 ~ 11-21 | `bit/evta_sys_rq_400MHz_pass_261104/` |
| `16_15_18_17_4evta_sa_400mhz_rq.bin` | 6a7b3df3 | `hw-multi4-sa-rq-261104` (`797a4ad`) | ExtraNetDelay_high + y219 흐름, +0.002 | `d26c3e3` 이후 | multi4 기본 11-04 ~ 11-23 | `bit/evta_multi4_rq_400MHz_pass_261104/` |
| `16_16_19_18_sa_alux3_400mhz_rq.bin` | 52616f85 | `hw-single-alux3-261115` (`8191320`) | ExplorePostRoutePhysOpt, +0.022 | `8191320` 이후 | **single 기본(11-22부터)** | `bit/evta_single_alux3_400MHz_261115/`; `exe_261126_38e3da6`, `exe_261123_398f358` |
| `16_15_18_17_4evta_sa_amul_400mhz_hp322_rq.bin` | 12824472 | `hw-multi4-amul-261123` (`4b80d48`) | y219 흐름 + HP 321.7 MHz, 0.000 | `398f358` 이후 | multi4 기본 11-24 ~ 11-25 | `bit/evta_multi4_amul_400MHz_pass_261123/` |
| `16_15_18_17_4evta_sa_amul_dw_400mhz_hp304_rq.bin` | 8ebdfbb4 | `hw-multi4-amul-dw-261125` (`818a73c`) | `VTA_CONTAIN=1` y219 흐름 + HP 304.3 MHz, +0.002 | `818a73c` 이후 | **multi4 기본(11-26부터)** | `bit/evta_multi4_amul_dw_400MHz_261125/`; `exe_261126_38e3da6`, `exe_261127_6c862f4` |
| `16_16_19_18_sa_alux3_dw_387mhz_rq.bin` | 641d91ba | `hw-single-alux3-dw387-261126` (`818a73c`) | ExtraTimingOpt → phys_opt → PLL 387.5, +0.010 | `818a73c` 이후 | MobileNetV2용(`BIN=`) | `bit/evta_single_alux3_dw_387MHz_261126/`; `exe_261127_6c862f4` |
| `16_16_19_18_sa_alux3_dw_383mhz_rq.bin`, `_375mhz_rq.bin` | 93ce01ba, 3680407d | (387과 같음), 375는 tag 없음 | PLL 383.3 / 375 | `818a73c` 이후 | 쓰지 않음 / 첫 시험판 | `bit/evta_single_alux3_dw_383MHz_261126/`, `..._375MHz_261125/` |

- 위 표에 없는 판(`_alu1_epr`, `_bias`, `_alux`, `_alux2`, `_amul`, `_amul_bc` …)과 그 vtalib는 [evta_isa_ext.md](evta_isa_ext.md) §5 표에 있다. 보관 dir README에 빌드 기록이 있는 것도 많다.
- 같은 commit에 두 판(`818a73c`)이 있는 것은 multi4와 single을 같은 소스로 만들었기 때문이다.
- tag 목록: `git tag -l 'hw-*' -n1`. ubun 쪽은 `git fetch --tags`로 받는다.

**실행 파일 세트** (보드 `bit/exe_sets/`, 2026-11-27 저장, 모두 `run_exe_set.sh`로 다시 돌려 0 differ를 확인했다)

| 세트 | vtalib (`NESTC_BUILD`) | 들어 있는 것 | 크기 |
|---|---|---|---|
| `exe_261126_38e3da6` | `38e3da6` 11-26(= `evta_perf_latest.md`의 측정 판), 보드 `build_sys` / `build_multievtax4_sys` | single Resnet18, VGG16, DeiT, MobileNetV2; multi4 Resnet18, MobileNetV2 | 112 MB |
| `exe_261127_6c862f4` | `6c862f4` 11-27(ubun의 `INPUT_PIPELINE`까지) | single / multi4 MobileNetV2Tf | 26 MB |
| `exe_261123_398f358` | `398f358` 11-23(`old_vtalib.sh hw-multi4-amul-261123`로 만든 것, §4.3) | single MobileNetV2Tf | 5.5 MB |

## 4. 예전 판을 다시 돌리기

### 4.1 대부분: 최신 실행 파일 + `BIN=`

```bash
# 보드, vtabundles_xcmpl/
sudo /bin/sh -c "BIN=vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta_sa_amul_400mhz_hp322_rq.bin hw_multievtax4/script/run_prof.sh -s _sys -n 25 MultiEVTAMobileNetV2TfTest"
sudo /bin/sh -c "EVTA_DWU=0 BIN=<_dw 판> ..."          # 같은 판에서 새 기능만 끄고 비교
```

- 실행 스크립트가 판 이름을 `/tmp/evta_bitstream`에 쓰므로, vtalib가 그 판에 있는 기능만 쓴다.
- 결과는 "그 판의 하드웨어 + 지금 소프트웨어"다. 예전에 문서에 적힌 시간과 다를 수 있다. 소프트웨어가 그 뒤 빨라졌기 때문이다.

### 4.2 그때 잰 실행 파일 그대로: 저장한 세트

```bash
# 보드, vtabundles_xcmpl/ (bitstream을 실행 파일마다 다시 올리고, 세트 dir의 weights / i8 입력으로 돈다)
sudo /bin/sh -c "BIN=<판> ./run_exe_set.sh exe_261126_38e3da6 -n 25 hw/Resnet18Test hw/MobileNetV2Test"
sudo /bin/sh -c "EVTA_RES_CHECK=1 BIN=<판> ./run_exe_set.sh exe_261127_6c862f4 -n 2"      # Dir를 안 주면 세트 전체
# 새 세트 저장 (sudo 필요 없음): 실행 파일(*Bundle_sys 또는 -s <끝 이름>), *.weights.bin, i8/, multi_model.h
./save_exe_set.sh exe_<yymmdd>_<vtalib commit> hw/<Dir> hw_multievtax4/<Dir> ...
./save_exe_set.sh -s _pcsysv_hw-multi4-amul-261123 exe_261123_398f358 hw/MobileNetV2TfTest
```

- **언제 저장하나:** 판을 기본으로 바꿀 때, 또는 문서에 시간을 적을 때. 그 측정에 쓴 실행 파일을 남긴다.
  - 크기를 생각해 모델 전부가 아니라 대표 몇 개만 남긴다. 보드 디스크(`/`)는 2026-11-27에 92 % 차 있다.
- 세트 이름은 `exe_<날짜>_<vtalib commit>`이다. bitstream의 `build_info.txt`에 `exe:` 줄로 어느 세트로 무엇을 확인했는지 적는다.
- 세트 실행 파일은 그때의 main을 쓴다. 그래서 출력 형식이 다를 수 있다.
  - 예: `exe_261123_398f358`은 PC `hw/`의 옛 main이라 `THROUGHPUT` 줄이 없고 `Inference time`(100회 합)만 나온다. `run_exe_set.sh`는 둘 다 보여 준다.

### 4.3 정말 예전 vtalib가 필요할 때: worktree + 새 build dir

```bash
# PC (nestc-build 컨테이너에서 cross build) 또는 보드 (native, make -j2)
utils/hwver/old_vtalib.sh <tag|commit> [single|multievtax4] <target> ...
#   예: utils/hwver/old_vtalib.sh hw-multi4-amul-261123 single vtaMobileNetV2TfBundle
```

- 하는 일:
  1. `<tree>/../nestc_hwver/<이름>/`에 그 commit을 worktree로 꺼낸다(343 MB, 처음 한 번). 지금 트리와 main 브랜치는 그대로다.
  2. `vtabundles_xcmpl/build_pc[_m4]_pcsysv_<이름>`(PC) 또는 `build[_multievtax4]_sysv_<이름>`(보드)를 만든다. `-DVTALIB=<worktree>/vtalib-zcu102-systolic`이다.
  3. target을 빌드한다.
- 실행 파일은 `hw*/<Dir>/<target>_pcsysv_<이름>` / `_sysv_<이름>`이다. 이름이 `_pcsys` / `_sys`로 시작하므로 `run_prof.sh -s <그 이름>`이 systolic 판을 올린다. PC에서 만든 것은 scp로 보드에 보낸다.
- **bundle은 지금 것을 쓴다**(`hw*/<Dir>/`의 `.cpp` / weights). 예전 vtalib에 없는 함수를 bundle이 부르면 link가 실패한다.
  - 그때는 같은 worktree에서 컴파일러(`Debug2_remote`)를 빌드하고 bundle을 다시 만들어야 한다. 시간이 많이 든다. 거의 필요 없을 것이다.
- 2026-11-27 시험: `hw-multi4-amul-261123`(vtalib `398f358`, 11-23)으로 MobileNetV2Tf를 PC에서 cross build했다. 11-25 컴파일러 bundle이 그대로 link됐다.
  - `_sa_amul_400mhz_rq` / `_alux3` 판에서 `EVTA_RES_CHECK` 152건 0 differ, 282였다.
  - 1장 약 15.4 ms였다(옛 main, 100회 합 1538–1567 ms, float 입력, 첫 장 포함). 지금 판(int8 입력, 첫 장 뺌)은 13.06 ms인데, 조건이 달라 그대로 비교하지는 않는다.
- 정리: `git worktree remove <tree>/../nestc_hwver/<이름>`, build dir은 `rm -rf`.

## 5. 새 판을 만들 때 할 일 (요약)

1. `make bin ...`: `build_info.txt`가 자동으로 생긴다. 하드웨어 파일을 고친 채 빌드하면 diff도 남는다. 되도록 commit한 뒤 빌드한다.
2. 배선 뒤 단계가 있으면 `build_info.sh write ... --step`으로 최종 `.bin`의 것을 쓴다(§2).
3. 보드에 올린다.
   - `/lib/firmware/<이름>.bin`
   - 보관 dir `bit/evta_<종류>_<기능>_<MHz>MHz_<yymmdd>/`: `.bin`, `.bit`, 보고서, `build_info.txt`, `README.md`
4. 시험한 결과를 `build_info.sh add ... test`로 적는다. 측정에 쓴 실행 파일은 `save_exe_set.sh`로 저장하고 `add ... exe`로 적는다.
5. 기본 판이 되면 tag를 붙이고 §3에 줄을 더한다. `docs/recent_changes.md`에도 적는다.
