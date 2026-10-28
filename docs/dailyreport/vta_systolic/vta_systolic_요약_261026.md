# 2026-10-26 EVTA systolic 요약: multi4 ResConv10 4개 동시 609.7 GOPS(DDR 경쟁 없음), 기본 bitstream을 333 MHz Y219판으로 통일, systolic GEMM 계획과 Phase A 시작

- 기간: 2026-10-26 07:00 ~ 10-27 07:00 (KST)
- [보드]는 보드 `main`(번들 빌드·실행, 보드에서 commit·push → PC pull)에서 한 작업이고, [PC]는 10-27 01시부터 PC의 `systolic` branch에서 한 작업이다.

## 시간순

**07:42 [보드] "커밋 해주고 기본을 333MHz 판으로 바꾸자."**
- 보드의 `/lib/firmware/vtahw_multi4_y219.bin`을 333 MHz판(`rs_rst.bin`)으로 바꿨다. 이 이름으로 Conv4를 4개 EVTA에서 돌려 489.66 GOPS, 4개 출력 모두 golden과 일치했다.
- 309.6 MHz판은 `bit/evta_multi4_y219_261025/rs_hpm.bin`에 남겼다.
- 커밋 `dfec80f7`(multi4 333 MHz across Y219: `vivado.tcl` HP 앞 register slice·EVTA별 리셋, Y219 flow, 문서), `ed8dcccd`(`multievtax4` 번들 세트, `multi4conv4/`).

**10:21 [보드] "bit 파일들 이름을 정리하자. ... 날자 앞에 333MHz_pass_ 를 추가해줘. ... vtahw_zcu102_... 로 바꿔주고."**
- multi2 경로는 `261025`가 아니라 `261024`였다. 세 dir 모두 333 MHz판이고, `.bin`, `.bit`, `.hwh`, `.timing.rpt`, `.utilization.rpt` 다섯 개로 맞췄다.

  | 새 이름 (`/home/xilinx/JO/npuprg/bit/`) | 내용 |
  |---|---|
  | `evta_single_y219_333MHz_pass_261023/` | `vtahw_zcu102_1x16_i8w8a32_16_16_19_18.*` |
  | `evta_multi2_y219_333MHz_pass_261024/` | `vtahw_zcu102_1x16_i8w8a32_16_15_18_17_2evta.*` |
  | `evta_multi4_y219_333MHz_pass_261026/` | `vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta.*` (`rs_rst.bin`에서 이름 변경) |
- multi4 `export/`는 10-24 옛 빌드라 쓰지 않았다. `.hwh`는 10-26 빌드의 `hw_handoff/vta.hwh`, `.utilization.rpt`는 `rs_rst.dcp`로 새로 만들었다.
- 실행에는 `.bin`(fpga_manager) 또는 `.bit`+`.hwh`(PYNQ `Overlay`, stuckchk)가 필요하다. `.rpt`는 기록용이다. `.xsa`, `.dcp`(187 MB)는 넣지 않았다.

**11:05 [보드] "vtalib 쪽도 바뀐 게 있어?" / 11:47 "copy는 유지하고 원본이 multi2/multi4를 포함하게 수정"**
- copy 3개와 원본 `vta/vtalib`의 차이는 profiling용 AXI perf monitor 부분 하나뿐이었다(`NESTC_EVTA_PROFILE_AXI_MONITOR` 옵션, `pynq_driver.cc`의 monitor clear/enable, multi의 EVTA별 주소 `0xA0050000 + i*0x10000`).
- 원본을 고쳤다: 옵션 추가(기본 OFF), monitor 주소를 `AXI_PERF_MONITOR + index * 0x10000`으로. single은 index 0이라 주소가 같다.
- `vtabundles_xcmpl/CMakeLists.txt`는 원본을 고친 것이 아니라, 시뮬레이터 빌드가 생성한 번들 `.cpp`를 보드 실행파일로 컴파일만 하는 독립 CMake 프로젝트다(`-DVTALIB=`, vtalib `-D` 매크로를 번들에도 적용, `BUNDLE_SET`별 `NESTC_EVTA_MULTI`·`VTA_MAX_XFER`).

**12:03 [보드] "single prof 성공했었나? single, multi4 prof 빌드하자." / 12:14 "1.864 ms를 쓰고 prof 빌드는 미루자. -D 매크로 적용이 무슨 말?"**
- single `PROFILE=1`은 성공한 적이 없다. 10-20 `_prof` bit 3개는 Y219 결함 셀(multi2 +2037, multi4 +5183비트)을 써서 compute done이 안 났다.
- 지금 prof 카운터(`0x50/0x60/0x70`)는 cycle이 아니라 명령어 필드로 계산한 개수이고, APM은 바이트·transaction·latency만 센다.
- 1.864 ms(`VTA_SYNC`의 `exe_time`)는 시작 레지스터 쓰기 직후~done polling이라 SW가 거의 안 들어간다(오차 1% 미만으로 봄). SW가 섞인 값은 "conv only" 2.02 ms 쪽이다.
- 결정: prof 빌드(HW cycle 카운터 B안)는 꼭 필요해질 때까지 미룬다.
- 매크로: vtalib `CMakeLists.txt`가 정한 `COMPILE_DEFINITIONS`를 `vtabundles_xcmpl`이 읽어 모든 번들에 붙인다. 안 그러면 vtalib(15/18/17)과 번들(헤더 기본 16/19/18)의 버퍼 크기가 달라질 수 있다. 원본과 copy는 코드가 같으므로 원본 하나에 cmake 옵션만 바꿔 써도 된다.

**12:38 [보드] "보드에 있는 vta-hw, vtalib dir 들이 모두 최신이 아니야?" / 12:48 "보드에서 git push, PC에서 git pull로 동기화하는 건데"**
- 번들 생성(`Debug2_remote`)과 번들 컴파일(`vtabundles_xcmpl/build*/`), 실행은 모두 보드에서, bit 빌드만 PC에서 한다.
- 중요: 보드 git이 옛 커밋(`40ae09a`)에 머물러 있었고, scp로 보낸 파일이 커밋 안 된 변경으로 쌓여 있었다. 보드의 `vtalib-zcu102-single`·`multi2` copy에는 **done race 수정이 빠져 있었다**(10-23 push 후 보드에서 pull 안 함).
- 정리: PC에서 vtalib `0357e76`, nest-compiler `d804e39e`를 push → 보드의 로컬 변경 14개를 커밋과 비교(모두 같음) → 되돌리고 `git pull --ff-only`. 보드 `vta/vtalib`은 submodule 초기화가 안 되어 있어 안에서 직접 pull했다. 이후 git 추적 파일 차이 0개. git으로만 동기화하는 방식을 메모리에 기록했다.
- `vta-hw-zcu102-*`는 보드에서 필요 없다. HW(`vta_config.json`→`pkg_config.py`)와 SW(vtalib `CMakeLists.txt`, `hw_spec.h`)는 같은 값을 각자 가지므로 상수를 바꿀 때는 둘을 손으로 맞춰야 한다.

**12:58 [보드] "지금까지 done race 안 된 상태에서 실험한 거야? 바뀐 코드로 한번 더 테스트 해야지 않아?"**
- 보드 빌드 dir마다 쓴 vtalib과 빌드 시각을 확인했다. 10-23 이후 실험은 모두 done race 수정판으로 했다.

  | 보드 빌드 dir | 쓴 vtalib | 라이브러리 빌드 시각 (보드 UTC) | done race |
  |---|---|---|---|
  | `build` (single 30개 번들) | `vta/vtalib` | 10-23 10:32 = KST 19:32 | 있음 |
  | `build_multievtax2` | `vta/vtalib` | 10-23 15:32 | 있음 |
  | `build_multievtax4`(+`_prof`) | `vtalib-zcu102-multi4` | 10-25 | 있음 (10-25 동기화) |
  | `hwtest` `*_m` | `vta/vtalib` | 10-23 15:31 | 있음 |
  | `build_prof`, `build_timed` | 옛 기본값 | 10-17 | 없음. 10-23 이후 안 씀 |
- **재테스트 (13:05):** 동기화한 vtalib으로 보드에서 다시 빌드하고 전부 다시 돌렸다. 결과는 이전과 같다.

  | 대상 | 결과 |
  |---|---|
  | hwtest (multi4 333 bit) | EVTA 0-3 gemmprobe 4종 0/16 wrong, sramscan 4종 0 bytes wrong, donerace 16/16 OK, mg4 3회 Golden Matched (EVTA0 줄과 regpat은 출력을 30줄로 잘라 화면에 안 남음, 실패 표시 없음) |
  | single (`vtahw_single_y219.bin`) | golden 30 PASS. Resnet18/50/Resnet18Partition 281, Vgg16Cifar10 0, Resnetv10S1C0 3.5 ms에 종료 |
  | multi2 | ConvTest PASS. Resnet18 2-EVTA(14.8 s)·single(13.4 s) 100회 모두 281 |
  | multi4 | ConvTest 0-3 golden. Resnet18 4-EVTA(14.5 s)·single(13.3 s) 모두 281 |
  | multi4 Conv4 4-EVTA 동시 | 485.1 GOPS, 모두 golden (이전 489.6과 1% 차이. Resnet18 직후 load 6.5 영향으로 봄) |

**13:08 [보드] "원본과 copy 3개를 주석까지 동일하게 맞추지" / 13:09 "어느 쪽이 더 최근 주석인지 비교하고 맞출지를 봐야지" / 13:16 "보드 원본·copy 비교, 그 다음 보드·PC 주석 비교"**
- 보드에서 원본의 두 파일을 copy 3개에 복사해 `401d9a8`로 push, PC pull.
- 순서가 바뀌었지만 확인 결과 원본(10-26) 주석이 copy(10-20)보다 최신이고 잃은 정보는 없었다. 덮어쓰기 직전 보드에는 커밋 안 된 변경이 없어 PC로 옮길 것도 없었다. 남은 차이는 git 밖 `config.mk`뿐이다.
- 앞으로 ① 보드 원본·copy 비교 → ② 보드·PC 주석 비교 → 판단 후 복사 순서를 메모리에 기록했다.

**13:26 [보드] "vtabundles_xcmpl/multi4conv4는 뭐야? run_all.sh로 돌리면 돼? 함수별 시간이 나와?" / 13:44 "MultiEVTAConv4TestSrc/로 옮기고 .gitignore 정리, 2·3번 진행" / 13:50 "파일들만 새 dir 아래로"**
- `multi4conv4/`는 single `hw/Conv4Test`를 4개로 복사해 EVTA i에 고정하는 생성 스크립트와 4-thread main이다. 같은 일을 4개 EVTA가 각자 하는 처리량 측정이다.
- 파일 2개를 `hw_multievtax4/MultiEVTAConv4TestSrc/` 바로 아래로 옮겼다. `.gitignore`는 번들 Test dir과 결과 파일만 무시하고, `run_all.sh`, `run_prof.sh`, `*Src/`는 git에 넣는다.
- `collect_hw_bundles.py`가 multievtax2(mask 3)·4(mask 15)에 `MultiEVTAConv4Test`를 만들고 run 줄을 추가한다. `timed_bundle.py`는 `.cpp`마다 `timing_<cpp>.csv`를 쓴다. multievtax2 기본 bit도 Y219판으로 바꿨다(커밋 `8002aea`).
- 빌드 중 고친 것:

  | # | 실패 | 원인 → 수정 |
  |---|---|---|
  | 1 | timed 빌드 2개 cmake 실패 | 같은 `mxnet_exported_resnet18.cpp`를 두 실행파일이 써서 timed 사본 경로가 겹침 → `timed/<dir>/<target>/<cpp>` |
  | 2 | `build_multievtax2`에 Conv4 target 없음 | 그 dir에서 cmake를 다시 안 돌림 → cmake 재실행 |
- Conv4 결과(일반·prof·timed 모두 PASS)다. conv 시간은 EVTA 수와 무관하고, 번들 전체 시간 차이는 4 thread의 CPU transpose가 코어 4개를 나눠 쓰기 때문이다.

  | | `convolution_wo_tr` 1회 | 번들 전체 1회 |
  |---|---|---|
  | multievtax4 (4개 동시) | 1.87-1.89 ms = 122-123 GOPS (각 EVTA) | 8.3-9.0 ms |
  | multievtax2 (2개 동시) | 1.87-1.88 ms = 123 GOPS (각 EVTA) | 5.0-5.2 ms |

**13:57 [보드] "해줘." (single `build_prof`·`build_timed` 재빌드)**
- 두 dir을 동기화된 `vta/vtalib`으로 다시 빌드했다(번들 실행파일 70개).
- 중요: **`run_prof.sh`가 bitstream을 다시 올리지 못하고 있었다.** `run_all.sh`의 `BIN=${BIN:-<파일>}` 줄을 그대로 읽어 `${BIN:-...}` 문자열을 firmware에 썼다. 앞서 multi Conv4 prof/timed PASS는 직전 `run_all.sh`가 올린 bit 덕분이었다. 파일명만 꺼내고 `BIN=` 우선, 없으면 멈추게 고쳤다(`a37e3e0`). 고친 상태로 multi4·multi2 Conv4 prof/timed 재실행 PASS.
- single 35개 번들 결과: `_timed`, `_prof` 모두 golden 30 PASS, 나머지 5개 exit 0(`_prof`는 두 번째 실행 결과). `BIN=vtahw_single_y219.bin` 지정.

  | 번들 | 전체 시간 | 그중 EVTA 실행 시간 (`_prof`) |
  |---|---|---|
  | Conv4Test | 5.4 ms (2회) | 3.55 ms = 1회당 1.77 ms |
  | Resnet18 | 184 ms | 48 ms |
  | Resnet18Partition | 234 ms | 48 ms |
  | Resnet50 | 1709 ms | 246 ms |
  | VGG16 Cifar10 | 66 ms | 34 ms |

  Resnet18Partition은 timed `.cpp` 9개 중 마지막 partition 하나가 87 ms를 차지한다.

**14:08 [보드] (사용자가 붙인 `run_prof.sh -s _timed` 실행 중 SError 로그)**
- 중요: 첫 `_prof` 실행 실패(Conv3·Conv5·ConvTest MISMATCH, segfault, SError Oops 7번)는 **사용자의 multi4 실행과 Claude의 single 실행이 겹쳐** bitstream이 1초 간격으로 번갈아 올라간 충돌이었다. 실행 전 확인하지 않은 실수였다.
- 단독으로 다시 돌린 `hw_multievtax4` `_timed` 결과:

  | 번들 | 결과 | EVTA별 마지막 1회 시간 |
  |---|---|---|
  | MultiEVTAConv4Test | PASS | 8.9 / 8.7 / 8.5 / 9.7 ms |
  | MultiEVTAConvTest | PASS | 1.7 / 1.0 / 10.3 / 1.0 ms |
  | multivtaMxnetResnet18 | exit 0 | 589 / 565 / 496 / 418 ms |
  | singlevtaMxnetResnet18 | exit 0 | 136 ms |
- 실행 전 `ps`/`dmesg` 확인을 메모리에 기록했다.

**14:12 [보드] "(lock) 넣는 게 좋겠어. single의 기본 bin 파일은? 어느 게 더 빠르지?" / 14:28 "바꾸자."**
- `run_all.sh`(세 hw dir)와 `run_prof.sh`가 `/tmp/evta_run.lock`을 잡는다. 실행파일 직접 실행이나 hwtest에는 lock이 없다(`68a51f0`).
- single 전체 `_prof`를 두 bitstream으로 돌린 비교다(lock 상태). 두 쪽 모두 golden 30 PASS, 5개 exit 0, stuckchk 194·추가 0. 값은 EVTA 실행 시간(`total_vta_time`, ms)이다.

  | 번들 | etri | Y219판 | etri/Y219 |
  |---|---|---|---|
  | Conv4Test, ResConv1·3·4·6·7·10 | | | 1.01 |
  | Resnet18 | 48.9 | 48.4 | 1.01 |
  | Resnet50 | 263.7 | 245.6 | 1.07 |
  | ResConv5 / ResConv8 | 0.585 / 0.677 | 0.533 / 0.589 | 1.10 / 1.15 |
  | Vgg16Cifar10 | 39.8 | 33.5 | 1.19 |
  | Conv_inception_5b_5x5 | 0.376 | 0.312 | 1.21 |

  큰 conv는 같고, 작거나 메모리 접근이 많은 층에서 Y219판이 4-21% 빠르다. 원인은 측정하지 않았고 load/store 경로 차이로 추정한다.
- single 기본값을 `vtahw_single_y219.bin`으로 바꿨다(`50cdfe8`). 변경 뒤 재실행은 하지 않았다(기본값 한 줄 변경).

**14:31 [보드] "collect_hw_bundles.py가 bin 파일이 필요해? bit를 줄 때 xcmpl/CMakeList.txt를 같이 줘야 해?" / 14:37 "하자."**
- bin 이름은 `run_all.sh`의 기본값을 채울 때만 쓴다.
- 받는 쪽이 맞춰야 할 것: 2-bit 필드 `hw_spec.h`, 버퍼 크기, EVTA 수·주소, 비코히어런트, done race 수정, `VTA_MAX_XFER`, prof 옵션. `VTA_MAX_XFER`만 `vtabundles_xcmpl`에 있었다.
- `VTA_MAX_XFER`를 vtalib 옵션 `NESTC_EVTA_MAX_XFER`로 옮겼다(비우면 multi 4 MB, single 32 MB, vtalib `f716ed3`, nest-compiler `e119fac9`). 재빌드 후 single Conv4·ResConv1 PASS, Resnet18 281, multievtax2·4 `run_all.sh` 전체 PASS.
- 보드 bit dir 3곳에 README를 넣었다(git 밖). 결함 없는 보드에서도 돌 것이라는 부분은 시험하지 않았다고 적었다.

**14:51 [보드] "xcmpl/CMakeLists.txt, vtalib/CMakeList.txt, pynq_driver.cc 세 개만 copy 해 가라고 하면 될까?" / 14:53 "joungheo를 jerry로" / 15:01 "옛 xcmpl/ 없애자"**
- 세 파일로는 부족하다. 원래 vtalib(`e9e7d29`) 대비 `hw_spec.h`(2 bit), `runtime.h`·`api.cc`(`VTABufferFlush`/`VTABufferInvalidate`), NEST-C `VTASave.cpp`(`37a0d575`, 번들 재생성 필요)가 꼭 필요하다. vtalib 통째로(`f716ed3` 이후) 바꾸는 것을 추천했다.
- `jerry`, `jerryoh`, `jerry-oh`, `jerryjo`는 모두 사용 중이다. 이름 변경은 계정 주인만 Settings에서 할 수 있다.
- PC의 옛 `/home/ubuntu/npuprg/nestc2_clion/vtabundles_xcmpl`(1.9 MB, 9-30)을 비교 후 지웠다. 보드에는 지울 것이 없었다.

**16:43 [보드] "MultiEVTAResnet18에서 어느 layer conv가 149GOPS까지 나오네. ... 합해서 600GOPS에 근접하지 않을까?" / 16:46 "진행하자."**
- 이미 있는 single·multi `_timed` csv를 모아 layer별 GOPS로 정렬했다.

  | GOPS | 시간 | layer | 번들 |
  |---|---|---|---|
  | 152.5 | 1.516 ms | conv | ResConv10Test (single) |
  | 150.8 | 1.534 ms | res5_2_branch2b | Resnet50Test |
  | 149.8 | 1.544 ms | stage4_conv1 | Resnet18PartitionTest |
  | 149.4 | 1.548 ms | stage4_conv3 | MultiEVTAResnet18 (multi4) |
  | 146.5 | 1.578 ms | res4_x_branch2b | Resnet50Test |

  상위는 모두 231,211,008 ops의 3x3 conv(512ch 7x7, 256ch 14x14)이다. Conv4(64ch 56x56)는 123 GOPS. EVTA 1개 이론 최대 170.7 GOPS. MultiEVTAResnet18의 149는 copy별 시작 시각이 최대 120 ms 어긋나 거의 혼자 돈 값이었다.
- `make_multi4conv4.py`를 conv 하나짜리 번들이면 어느 것이든 복사하게 일반화하고(`multiconv.h` 생성), `MultiEVTAResConv10Test`를 multi2·4에 추가했다. 측정 직전 보드 ssh가 끊겼다가 돌아왔다.
- **측정 (17:08):** conv만 400회, 333 MHz, `OMP_NUM_THREADS=1`, 같은 조건 2~3회 반복. 모든 출력 golden 일치. 단위 GOPS, —는 안 잰 조합.

  | 비트스트림 | 번들 | EVTA 1개 | EVTA 2개 | EVTA 4개 |
  |---|---|---|---|---|
  | multi4 | ResConv10 (7x7x512→512) | 152.6 | — | **609.7** |
  | multi4 | Conv4 (56x56x64→64) | 123.5 | — | 491.6~493.4 |
  | multi2 | ResConv10 | 152.8 | 305.5 | — |
  | multi2 | Conv4 | 123.6 | 247.1 | — |

  - 중요: 4개 동시 값이 1개의 정확히 4배다. EVTA마다 conv당 weight 2.36 MB를 읽어도 HP 포트·DDR 경쟁이 없다. 이론 최대 682.7 GOPS의 89%.
  - 처음 558~579 GOPS는 `OMP_NUM_THREADS=1` 없이 돌려 OpenMP 팀이 코어 4개를 나눠 쓴 탓이었다.
  - CPU transpose 포함 end-to-end는 296.6 GOPS(CPU 병목).
- 커밋 `bc2abf3`. 보드에서 github.com 22번 포트가 타임아웃돼 push가 안 됐고, 443 포트 ssh로 다시 시도했다. 중간에 `pkill -f "git push"`가 셸까지 죽여 한 번 실패했다.

**21:04 [보드] "609GOPS는 각 evta GOPS를 단순히 합한 거지?" / 21:17 "231.2Mops는 어디서? 어떻게 4개가 돌기 시작한 시점부터 다 돈 시점까지를 잰 거야?"**
- 아니다. barrier(참가자 5)로 4 thread가 같이 출발하고, main이 start~end barrier의 wall 시간을 잰다. GOPS = 4 × 400 × 231.2 Mops(= 369.9 Gops) / 606.748 ms = 609.7.
- 231.2 Mops는 conv 1회 연산량 = 2 × 1·7·7 × 512 × 512 × 3·3. conv-only 모드는 alloc·transpose를 앞뒤 한 번만 하고 그 사이 `convolution_wo_tr`만 400번 부른다(CPU 명령어 생성 + EVTA 실행 + `VTASynchronize` 포함).
- 같은 측정의 EVTA별 시간:

  | EVTA | 400회 시간 | GOPS |
  |---|---|---|
  | 0 | 606.370 ms | 152.52 |
  | 1 | 606.558 ms | 152.47 |
  | 2 | 606.736 ms | 152.43 |
  | 3 | 606.521 ms | 152.48 |
  | 전체 (wall) | 606.748 ms | 609.71 |

  `OMP_NUM_THREADS=1` 없던 첫 측정은 EVTA별 313~332 ms로 달라, GOPS 합 569.6, RESULT(wall 기준) 557.9였다. 609.7은 처리량(throughput)이고 conv 하나의 latency는 여전히 1.516 ms다.

**21:29 [보드] "속도 측정 실험까지 여러 문서에 보충하자. ... 609GOPS 실험은 보고서를 자세히" / 22:01 "claude docs에 올려 공유 링크를"**
- 문서를 쓰기 전 `_prof`로 EVTA HW 시간을 다시 쟀다.

  | ResConv10 conv 1회 | 값 |
  |---|---|
  | EVTA 시간, 1개 | 1.4473 ms |
  | EVTA 시간, 4개 동시 | 1.4491 ms (+0.12%, DDR 경쟁 없음) |
  | 같은 EVTA의 conv 사이 CPU 간격 | 66 µs (conv 1회의 4.4%) |
  | 4개가 모두 돈 비율 | 측정 구간의 82.8% |
- 새 보고서 `docs/evta_multi4_609gops_report.md`: ResConv10 구조·양자화(clip((acc+bias)>>7)), `conv_kernel`(7x7 tile, 입력 채널 블록 32개, 가상 스레드 2개), EVTA당 DDR 1.66 GB/s, GEMM 가동률 93.5%(Conv4는 tile 7개라 73%, 원인은 추정), 측정 방법, 재현 명령.
- `docs/vta_synth_notes.md`: §0, §2.4, §3.3(333 MHz까지: register slice → EVTA별 리셋), §3.4(새 절, 10-26 문제 8가지), §6(측정 방법). 커밋 `7365874` push, PC pull.

  | §3.4 10-26 문제 | 내용 (이날 기록 기준) |
  |---|---|
  | 1 | 보드 vtalib copy에 done race 수정이 빠짐 (pull 안 함) |
  | 2 | `_timed` 빌드 cmake 중복 규칙 (같은 `.cpp` 사본 경로 겹침) |
  | 3 | `run_prof.sh`가 bitstream을 다시 로드하지 않음 (`${BIN:-...}` 문자열) |
  | 4 | 다른 실행과 겹쳐 생긴 SError |
  | 5 | configure 순서 때문에 빌드 타깃이 없음 (`build_multievtax2` Conv4) |
  | 6 | `VTA_MAX_XFER`가 xcmpl에만 있음 |
  | 7 | OpenMP 과다 스레드 (558~579 GOPS) |
  | 8 | 보드 네트워크 끊김 (ssh, github 22번 포트) |
- `MultiEVTAResConv10Test`는 직접 실행으로 golden만 확인했고 `run_all.sh`로는 아직 안 돌렸다.
- Claude Docs 문서: [EVTA 4개 동시 실행 609.7 GOPS 측정 보고서](https://claude.ai/artifact/Pq7QCnRFBgbqFZuaRYvtLd) (EVTA 1·2·4개 GOPS 막대 차트 포함).

**10-27 00:21 [보드] "GEMM을 systolic 구조로 바꾸면 runtime이나 컴파일러에서 많이 바꿔야 할까? ..." / 00:34 "vta_synth_notes.md에 추가해 줘" / 00:37 "해줘."**
- ISA·컴파일러(NEST-C)는 그대로 둔다. 런타임은 vtalib `gemmop_mxn`, `gemmop_1x1`의 uop 순서만 바꾼다. HW 일이 크다.
- 지금 순서는 같은 weight가 연속 7(ResConv10)~8(Conv4)사이클만 쓰여, weight를 16클럭 shift로 채우는 구조에서는 가동률이 약 44%로 떨어진다. weight 고정·출력 픽셀 안쪽 순서로 바꾸면 49/448사이클 연속이 된다(uop 147개).
- HW 할 일: 열별 acc bank와 deskew, acc hazard(in-flight dst 검사), 명령 경계 drain(약 40~50사이클), RTL로 작성.
- 확인해서 고친 점: rs_rst 333의 worst setup 경로는 `inp_mem_3` BRAM → `compute_3` GEMM DSP(slack +0.008 ns, 배선 65%)로 input broadcast가 critical이다. FC는 CPU 함수라 weight-stationary 손해는 지금 해당 없다.
- `vta_synth_notes.md` §8(설계 검토)로 넣고 `fc859af` push, PC pull.

**10-27 01:11 [PC] "§8.7 순서로 시작하자. ... *-systolic으로 복사하고, systolic 브랜치에서" / 01:19 "GEMM을 systolic 구조로 바꾸기 위해 계획을 세워줘."**
- `vta-hw-zcu102-systolic`, `vtalib-zcu102-systolic`을 단일 EVTA 소스에서 복사했다(커밋 `2b81b777`).
- §8.7 1단계: single(10-23 y219 pass DCP)과 multi4(rs_rst)에서 최악 경로 1000개를 봤다. 1000개가 모두 slack 0.23 ns 안에 몰려 있다.

  | 경로 | single slack | multi4 slack |
  |---|---|---|
  | `inp_mem` BRAM → GEMM DSP A (logic 0, fanout 346, BRAM 출력 레지스터 없음) | **0.004** | **0.008** |
  | `load` → `inp_mem`/`wgt_mem` BRAM 쓰기 | 0.150 | 0.017 |
  | `wgt_mem` → DSP B | 0.161 | 상위 1000 밖 |
  | compute 내부 덧셈 트리 (8 level) | 0.172 | 0.119 |
  | reset fanout (`proc_sys_reset` → store/fetch/load) | — | 0.097 |
  | `axi_smc` 내부 | 0.177 | — |

  - 중요: 상위 100개는 거의 다 inp broadcast(single 100/100, multi4 96/100)라 §8.6 가설은 맞다. 그러나 다음 벽이 0.1~0.17 ns 뒤에 있어, **systolic만으로는 약 345~350 MHz(4~5%)** 정도로 본다. BRAM 출력 레지스터를 켜는 더 싼 대안도 있다.
- 계획: Phase A(파이프라인만으로 상한 측정) → B(런타임 uop 순서, 지금 bit로 검증) → C(사이클 C 모델) → D(RTL GEMM: HLS compute는 decode·ALU·토큰, GEMM은 AXIS descriptor로 RTL, acc_mem 밖으로, DSP PCIN/PCOUT cascade, B1/B2 weight double buffer) → E(xsim, cosim) → F(single 보드) → G(multi2 → multi4).

**10-27 01:38 [PC] "phase A를 먼저 하자."**
- Makefile 옵션 `MEM_OREG=1`(compute 쪽 inp/wgt BRAM port B 출력 레지스터, HLS read latency 2), `FREQ=<MHz>`(PLL만 변경)를 넣었다. 옵션이 없으면 빌드는 이전과 같다(커밋 `ab32e110`).
- HLS: GEMM 루프 II=1, 깊이 12, acc read→write 거리 약 3사이클 그대로라 새 hazard 없음. 처음 비교한 기준 IP(10-20)는 wgt bank 구성이 옛 것이라 기준 IP(`MEM_OREG=0`)를 새로 만든다.
- 백그라운드 빌드 다섯 개(y219 fence, `PROFILE=1`). 결과는 10-27 보고서에 있다.

  | 변형 | 클럭 | 목적 |
  |---|---|---|
  | oreg | 333 / 360 / 380 / 400 MHz | 출력 레지스터를 켠 뒤 어디까지 timing이 맞는지(WNS ≥ 0) |
  | base | 360 MHz | 대조군: 레지스터 없이 360 MHz가 실패하는지 |

**10-27 01:47~01:53 [PC] "Phase A 하는 도중에 Gemmini, Tensil, UG579 조사 (acc 경계, DSP B1/B2·CEB 제어, PE 입력 레지스터, ZU+ 클럭 상한), §8.5 세부 개정" / "autoSA나 다른 systolic array GEMM open source도 같이 분석"**
- 이 기간 안에 결과 보고는 없다(10-27 08:59 이어서 진행).

## 결정이 남은 것 (10-27 07:00 기준)
- 이미 결정됨: 기본 bitstream은 single·multi2·multi4 모두 333 MHz Y219판. prof(HW cycle 카운터) 빌드는 필요해질 때까지 미루고 HW 시간은 `VTA_SYNC` `exe_time`을 쓴다. vtalib copy 3개는 실험용으로 유지하고 원본과 같이 고친다. PC↔보드 동기화는 git으로만. 실행에는 `/tmp/evta_run.lock`. systolic은 Phase A부터.
- 사용자 답을 기다리는 것:
  - `/lib/firmware`의 중복 `vtahw_multi4_y219_333.bin`을 지울지.
  - 다른 유저용 patch 2개(vtalib `git diff e9e7d29 f716ed3`, `VTASave.cpp`)를 만들어 bit dir README에 적용 방법을 넣을지. bit dir README 사본을 저장소(`docs/bitstreams/` 등)에 둘지.
  - GitHub 이름 변경(새 이름을 정하면 `.gitmodules`와 remote 주소 정리).
  - `MultiEVTAResConv10Test`를 `run_all.sh`로 돌려 보는 것.
