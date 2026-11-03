# 2026-10-26 EVTA synth 요약: ResConv10 4-EVTA 동시 609.7 GOPS(이론 최대의 89%, DDR 경쟁 없음), 기본 bitstream을 333 MHz Y219판으로 통일, PC↔보드 git 동기화와 run_prof.sh 재로드 버그 수정

- 기간: 2026-10-26 07:00 ~ 10-27 07:00 (KST)
- 세션은 하나다. 보드 실험은 모두 이 PC에서 ssh로 돌렸다. 작업 위치에 따라 이름을 붙였다.
  - [PC]: PC `main`에서 고친 턴이다. 12:48까지는 PC에서 고쳐 scp/rsync나 push로 보드에 보냈다.
  - [보드]: 보드 `main`에서 고쳐 보드에서 push하고 PC에서 pull한 턴이다. 사용자가 12:48에 이 방향으로 바로잡았다.

## 시간순

**07:42 [PC] "커밋 해주고 기본을 333MHz 판으로 바꾸자."**
- 보드 `/lib/firmware/vtahw_multi4_y219.bin`을 333 MHz판(`rs_rst.bin`)으로 바꿨다. 이 이름으로 Conv4를 4개 EVTA에서 다시 돌려 489.66 GOPS, 4개 출력 모두 golden과 일치했다. 309.6 MHz판은 `bit/evta_multi4_y219_261025/rs_hpm.bin`에 남겼다.
- 커밋 2개(push 안 함): `dfec80f7` multi4 333 MHz(`vivado.tcl`의 HP 포트 앞 register slice, EVTA별 리셋 블록, Y219 flow, 문서), `ed8dcccd` `multievtax4` 번들 세트와 `multi4conv4/`.

**10:21 [PC] "bit 파일들 이름을 정리하자. ... 날자 앞에 333MHz_pass_ 를 추가해줘. ... .bit, .bin, .hwh 면 돼? ... 이름도 원래처럼 vtahw_zcu102_... 로"**
- multi2는 `261025`가 아니라 `261024`였다. 세 bitstream 모두 333 MHz판임을 확인했다.

  | 새 이름 (`/home/xilinx/JO/npuprg/bit/`) | 내용 |
  |---|---|
  | `evta_single_y219_333MHz_pass_261023/` | `vtahw_zcu102_1x16_i8w8a32_16_16_19_18.*` |
  | `evta_multi2_y219_333MHz_pass_261024/` | `vtahw_zcu102_1x16_i8w8a32_16_15_18_17_2evta.*` |
  | `evta_multi4_y219_333MHz_pass_261026/` | `vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta.*` (`rs_rst.bin`에서 이름 변경) |

- 세 곳 모두 `.bin`, `.bit`, `.hwh`, `.timing.rpt`, `.utilization.rpt` 다섯 개로 맞췄다. 세 `.bin`은 `/lib/firmware` 기본 파일과 md5가 같다.
- multi4의 `export/`는 10-24 옛 빌드라 쓰지 않았다. `.hwh`는 이번 빌드의 `hw_handoff/vta.hwh`(10-26), `.utilization.rpt`는 `rs_rst.dcp`로 새로 만들었다.
- 실행에 필요한 것은 `.bin`(fpga_manager), 또는 `.bit`+`.hwh`(PYNQ `Overlay`, stuckchk)다. `.rpt`는 기록용, `.xsa`/`.dcp`는 PC에만 둔다.

**11:05 [PC] "vtalib 쪽도 바뀐 게 있어?" / 11:47 "copy는 유지하자. 원본은 multi2/multi4를 포함하게 수정해두자. ... xcmpl/CMakeLists.txt는 왜 원본과 다르지?"**
- copy 3개와 원본 `vta/vtalib`의 차이는 profiling(`PROFILE=1`) 빌드의 AXI perf monitor 부분뿐이다(옵션 `NESTC_EVTA_PROFILE_AXI_MONITOR`, EVTA별 monitor 주소). 일반 bitstream 번들에는 영향이 없다.
- copy는 그대로 두고, 원본에 옵션(기본 OFF)과 monitor 주소 `AXI_PERF_MONITOR + index * 0x10000`, 주석을 넣었다.
- `vtabundles_xcmpl/CMakeLists.txt`는 원본을 고친 것이 아니라 보드용 독립 CMake다. 번들 `.cpp` 생성(시뮬레이터 빌드 `Debug2_remote`)에는 쓰지 않고, 생성된 번들을 보드 실행파일로 컴파일할 때만 쓴다. `-DVTALIB=`로 vtalib을 고르고, `BUNDLE_SET`별로 `NESTC_EVTA_MULTI`와 `VTA_MAX_XFER`를 정한다.

**12:03 [PC] "single 경우 profiling 옵션을 켜고 돌린 게 성공적이었나? ... prof counter를 넣으면 순수 HW 시간을 알 수 있잖아." / 12:14 "1.864 ms 쓰고 prof 빌드는 미루자. -D 매크로를 번들에도 적용한다는 게 무슨 말이야?"**
- 중요: single `PROFILE=1`은 성공한 적이 없다. 10-20 `_prof` bit 3개는 compute가 done을 내지 않아 멈췄고, 원인은 Y219 결함 행이었다(multi2 +2037비트, multi4 +5183비트). fence 이후 prof판은 다시 만들지 않았다.
- 지금 prof 카운터(`0x50/0x60/0x70`)는 cycle이 아니라 명령어 필드로 센 개수이고, APM은 바이트·transaction·latency만 준다.
- 1.864 ms는 `VTA_SYNC`의 `exe_time`(시작 레지스터 쓰기 직후 ~ done polling)이라 SW 시간은 거의 없다(오차 1% 미만으로 봄). SW가 섞인 값은 "conv only" 2.02 ms다.
- 결정: prof 빌드는 꼭 필요할 때까지 미루고 `exe_time`을 쓴다.
- `-D` 매크로: `vtabundles_xcmpl`이 vtalib의 `COMPILE_DEFINITIONS`(버퍼 크기, `VTA_DEVICE_NUM`, EVTA별 주소 등)를 읽어 번들에도 붙인다. 안 그러면 번들과 vtalib의 구조체·상수 크기가 달라진다. 원본과 copy는 코드가 같으므로 원본 하나에 `BUNDLE_SET` 옵션만 바꿔도 된다.

**12:38 [PC] "번들 생성과 컴파일 모두 보드에서 하지 않아? ... 누가 어디를 고치는지 확인하고 싶어."**
- bit 빌드는 PC(docker), 번들 생성·컴파일·실행은 보드다. 보드 git은 옛 커밋(`40ae09a`)에 머물러 있었고, 파일은 scp로 받은 상태였다.
- 중요: 보드의 `vtalib-zcu102-single`, `multi2` copy에 done race 수정이 빠져 있었다(어느 빌드 dir도 쓰지 않음). `vta/vtalib`에는 있었다.
- 매크로 위치: 보드 `vtabundles_xcmpl/build_multievtax4/CMakeFiles/multi4conv4Bundle.dir/flags.make`의 `CXX_DEFINES`. 출처는 `vtalib-zcu102-multi4/CMakeLists.txt`의 `NESTC_EVTA_MULTI` 분기다.

**12:48 [PC] "done race 수정과 monitor 수정이 원래 보드에 들어갔어야 하는 거 아냐? 거기서 git push 하고 PC에서 git pull 해서 동기화 하는건데."**
- 중요: done race 수정(`d07c292e`, vtalib `163b3b0`)은 10-23에 push했지만 보드에서 pull하지 않았다. 앞으로 git으로만 동기화한다(메모리에 기록).
- PC에서 vtalib `0357e76`, nest-compiler `d804e39e`를 push했다. 보드의 로컬 변경 14개가 들어올 커밋과 같은 것을 확인한 뒤 되돌리고 `git pull --ff-only`했다. 보드 `vta/vtalib`은 submodule 초기화가 안 되어 있어 안에서 직접 pull했다.
- 보드의 `vta-hw-zcu102-*`는 pull로 같이 들어온 것이고 보드에서는 필요 없다. HW(`vta_config.json`)와 SW(vtalib CMake, `hw_spec.h`)는 같은 상수를 각자 가지고 있어서 손으로 맞춰야 한다.

**12:58 [PC] "지금까지 done race 안 된 상태에서 실험한 거야? ... 바뀐 코드로 한번 더 테스트 해야지 않아?"**
- 10-23 이후 실험은 모두 done race 수정판으로 했다. 보드 빌드 dir별로 확인했다.

  | 보드 빌드 dir | 쓴 vtalib | 라이브러리 빌드 시각 (보드 UTC) | done race |
  |---|---|---|---|
  | `build` (single 30개 번들) | `vta/vtalib` | 10-23 10:32 = KST 19:32 | 있음 |
  | `build_multievtax2` | `vta/vtalib` | 10-23 15:32 | 있음 |
  | `build_multievtax4`(+`_prof`) | `vtalib-zcu102-multi4` | 10-25 | 있음 (10-25 동기화) |
  | `hwtest` `*_m` | `vta/vtalib` | 10-23 15:31 | 있음 |
  | `build_prof`, `build_timed` | 옛 기본값 | 10-17 | 없음. 10-23 이후 안 씀 |

- 동기화한 vtalib으로 보드에서 다시 빌드해 전체 재테스트했다(13:05). 결과는 이전과 같다.
  - hwtest(multi4 333 MHz): EVTA 0-3 gemmprobe 4종 0/16 wrong, sramscan 4종 0 bytes wrong, donerace 16/16 OK, mg4 3회 Golden Matched. 출력을 30줄로 잘라 EVTA0 줄과 regpat은 남지 않았다(실패 표시는 없음).
  - single(`vtahw_single_y219.bin`): golden 30 PASS, Resnet18/Resnet50/Resnet18Partition 281, Vgg16Cifar10 0.
  - multi2: ConvTest PASS, Resnet18 2-EVTA판(14.8 s)·single판(13.4 s) 100회 모두 281.
  - multi4: ConvTest 출력 0-3 golden, Resnet18 4-EVTA판(14.5 s)·single판(13.3 s) 모두 281, Conv4 4-EVTA 485.1 GOPS(이전 489.6, 1% 차이; Resnet18 직후 load 6.5 영향으로 봄).

**13:08 [보드] "원본과 copy 3개를 주석까지 동일하게 맞추지 왜 그건 뺐어?" / 13:09 "어느쪽이 더 최근까지를 반영한 주석인지를 비교하고 맞출지를 봐야지." / 13:16 "보드의 원본과 copy를 먼저 비교 ... 그 다음 보드와 PC의 주석을 비교하고"**
- 보드에서 원본의 `CMakeLists.txt`, `pynq_driver.cc`를 copy 3개에 복사해 `401d9a8`로 push, PC pull했다.
- 비교를 뒤늦게 했다. 덮어쓴 copy 주석은 10-20 것이고, 원본(10-26) 주석에 그 내용이 모두 들어 있어 사라진 정보는 없다. 보드에만 있던 주석도 없었다(reflog로 확인).
- 앞으로는 ① 보드에서 원본과 copy 비교, ② 보드와 PC 주석 비교를 먼저 보여 주고 복사한다(메모리에 기록).

**13:26 [보드] "vtabundles_xcmpl/multi4conv4는 뭐야? ... run_all.sh로 돌리면 돼? ... 같은 bundle을 copy해서 4개 evta에 동시에 돌리는 건가?" / 13:44 "MultiEVTAConv4TestSrc/로 옮기자. .gitignore ... 2,3번은 진행해줘." / 13:50 "파일들만 새 이름의 dir 아래로"**
- `multi4conv4/`는 생성 스크립트와 4-thread main이다. single `Conv4Test`를 4개로 복사해 EVTA i에 고정하고 같은 input으로 동시에 돌리는 처리량 측정이다(레이어 분할 아님).
- 파일 2개를 `hw_multievtax4/MultiEVTAConv4TestSrc/` 바로 아래로 옮겼다. `.gitignore`는 `hw_*/` 전체 대신 번들 dir과 결과 파일만 무시한다.
- `collect_hw_bundles.py`가 multievtax2(mask 3)·4(mask 15)에 `MultiEVTAConv4Test`를 만들고 `run_all.sh`에 등록한다. 함수별 시간용 `build_multievtax2/4_prof`, `_timed`를 만들었다. multievtax2의 `run_all.sh` 기본 bit도 Y219판으로 바꿨다.
- 빌드 실패 2개: timed 사본 경로가 겹침(→ `timed/<dir>/<target>/<cpp>`), `build_multievtax2`에서 cmake를 다시 안 돌려 target이 없음.
- 보드 커밋 `8002aea`. Conv4 일반·prof·timed 모두 PASS. 아래는 `_prof`/`_timed`로 잰 시간이다.

  | | `convolution_wo_tr` 1회 | 번들 전체 1회 |
  |---|---|---|
  | multievtax4 (4개 동시) | 1.87-1.89 ms = 122-123 GOPS (각 EVTA) | 8.3-9.0 ms |
  | multievtax2 (2개 동시) | 1.87-1.88 ms = 123 GOPS (각 EVTA) | 5.0-5.2 ms |

  conv 시간은 EVTA 수와 상관없다. 4개일 때 번들 전체가 긴 것은 CPU transpose가 코어 4개를 나눠 쓰기 때문이다.

**13:57 [보드] "해줘." (single `build_prof`·`build_timed` 다시 빌드)**
- 10-17에 빌드된(done race 이전) 두 dir를 동기화된 `vta/vtalib`으로 다시 빌드했다(번들 70개).
- 중요: `run_prof.sh`가 지금까지 bitstream을 다시 올리지 못했다. `run_all.sh`의 `BIN=${BIN:-<파일>}` 줄을 그대로 읽어 `${BIN:-...}` 문자열을 firmware에 썼다. 앞의 multi Conv4 prof/timed PASS는 직전 `run_all.sh`가 같은 bit을 올려 둔 덕분이었다. 수정(`a37e3e0`): 파일 이름만 꺼내고, 환경변수 `BIN=` 우선, 파일이 없으면 멈춤. 고친 뒤 multi4·multi2 Conv4 prof/timed 다시 PASS.
- single 35개 번들을 `BIN=vtahw_single_y219.bin`으로 `_prof`/`_timed` 실행: golden 30 PASS, 나머지 5개 exit 0(`_prof`는 두 번째 실행 결과).

  | 번들 | 전체 시간 | 그중 EVTA 실행 시간 (`_prof`) |
  |---|---|---|
  | Conv4Test | 5.4 ms (2회) | 3.55 ms = 1회당 1.77 ms |
  | Resnet18 | 184 ms | 48 ms |
  | Resnet18Partition | 234 ms | 48 ms |
  | Resnet50 | 1709 ms | 246 ms |
  | VGG16 Cifar10 | 66 ms | 34 ms |

  Resnet18Partition은 timed `.cpp` 9개가 각각 표를 내고, 마지막 partition 하나가 87 ms다.
- 첫 `_prof` 실행은 Conv3·Conv5·ConvTest MISMATCH, 7번째 번들에서 segfault였다. 그 시각 single과 multi4 bitstream이 약 1초 간격으로 번갈아 올라갔고 커널 SError Oops가 7번 찍혔다.

**14:08 [보드] (사용자가 붙인 `hw_multievtax4`의 `sudo ./run_prof.sh -s _timed` 출력: Synchronous External Abort, SError, `cma_alloc` 실패)**
- 중요: 사용자의 multi4 실행과 이 세션의 single `_prof` 실행이 겹쳐 FPGA가 1초 간격으로 바뀐 충돌이었다. 실행 전에 확인하지 않은 이 세션의 잘못이다. 앞으로 실행 전에 `ps`/`dmesg`로 확인한다(메모리에 기록).
- 같은 명령을 단독으로 다시 돌린 결과(`hw_multievtax4`, `_timed`):

  | 번들 | 결과 | EVTA별 마지막 1회 시간 |
  |---|---|---|
  | MultiEVTAConv4Test | PASS | 8.9 / 8.7 / 8.5 / 9.7 ms |
  | MultiEVTAConvTest | PASS | 1.7 / 1.0 / 10.3 / 1.0 ms |
  | multivtaMxnetResnet18 | exit 0 | 589 / 565 / 496 / 418 ms |
  | singlevtaMxnetResnet18 | exit 0 | 136 ms |

**14:12 [보드] "넣는 게 좋겠어. single의 기본 bin 파일은 어떻게 할까? 어느 게 더 빠르지?" / 14:28 "바꾸자."**
- lock(`68a51f0`): `run_all.sh`(세 hw dir)와 `run_prof.sh`가 `/tmp/evta_run.lock`을 잡고, 다른 실행이 있으면 `another EVTA run is active`로 끝난다(rc=1 확인). 실행파일 직접 실행과 hwtest 스크립트에는 걸리지 않는다.
- single bitstream 비교: lock을 건 상태에서 single 전체 `_prof`를 etri bit과 Y219판으로 각각 돌렸다. 둘 다 golden 30 PASS, 나머지 5개 exit 0, stuckchk 194(추가 0). EVTA 실행 시간(`total_vta_time`, ms):

  | 번들 | etri | Y219판 | etri/Y219 |
  |---|---|---|---|
  | Conv4Test, ResConv1·3·4·6·7·10 | | | 1.01 |
  | Resnet18 | 48.9 | 48.4 | 1.01 |
  | Resnet50 | 263.7 | 245.6 | 1.07 |
  | ResConv5 / ResConv8 | 0.585 / 0.677 | 0.533 / 0.589 | 1.10 / 1.15 |
  | Vgg16Cifar10 | 39.8 | 33.5 | 1.19 |
  | Conv_inception_5b_5x5 | 0.376 | 0.312 | 1.21 |

  큰 conv는 1% 차이이고, 작거나 메모리 접근이 많은 레이어에서 Y219판이 4-21% 빠르다. 원인은 측정하지 않았고 load/store 경로 차이로 추정한다.
- 결정: single 기본값을 `vtahw_single_y219.bin`으로 바꿨다(`50cdfe8`). etri판은 `BIN=zcu102_1x16_i8w8a32_16_16_19_18.bin`으로 쓸 수 있다. 바꾼 뒤 다시 돌리지는 않았다.

**14:31 [보드] "collect_hw_bundles.py가 bin 파일이 필요해? ... bit 파일을 다른 유저들에게 줄 때 xcmpl/CMakeList.txt를 같이 줘야 해?" / 14:37 "하자."**
- bin 이름은 `run_all.sh`의 `BIN` 기본값을 채우는 데만 쓴다.
- 받는 쪽 SW가 맞춰야 할 것: 명령어 필드 2-bit(`hw_spec.h`), 버퍼 크기, EVTA 수와 주소, 비코히어런트, done race 수정, `VTA_MAX_XFER`, prof 옵션. `VTA_MAX_XFER`만 `vtabundles_xcmpl`에 있었다.
- `VTA_MAX_XFER`를 vtalib CMake로 옮겼다(vtalib `f716ed3`, copy 3개 동일, nest-compiler `e119fac9`): 옵션 `NESTC_EVTA_MAX_XFER`, 비우면 multi 4 MB, single 32 MB. `vtabundles_xcmpl`은 multievtax2 8 MB, multievtax4 4 MB를 넘긴다(값은 이전과 같음). 다시 빌드 후 single Conv4·ResConv1 PASS, Resnet18 281, multievtax2·4 `run_all.sh` 전체 PASS.
- 보드 bit dir 3곳에 README(파일 구성, WNS/WHS, md5, 맞춰야 할 SW 항목, vtalib 커밋, 빌드 옵션, 보드 조건)를 넣었다. git 밖이다. 결함 없는 보드에서도 돈다는 부분은 시험하지 않았다고 적었다.

**14:51 [보드] "xcmpl/CMakeLists.txt, vtalib/CMakeList.txt, pynq_driver.cc 이 세개만 copy 해 가라고 하면 될까?" / 14:53 "joungheo 이름을 jerry로 바꿀 수 있어?"**
- 세 파일로는 부족하다. 원래 vtalib(`e9e7d29`) 대비 꼭 필요한 것: `hw_spec.h`(2 bit), `runtime.h`/`api.cc`(`VTABufferFlush`/`VTABufferInvalidate`), NEST-C `VTASave.cpp`(번들에 flush/invalidate, 번들 재생성 필요), `pynq_driver.cc`, vtalib `CMakeLists.txt`. 선택: `CPUBundle_generic.cpp`(Resnet18 conv0 1547 → 76 ms), `vtabundles_xcmpl/CMakeLists.txt`.
- 추천: vtalib을 `f716ed3` 이후로 통째로 바꾸고 `VTASave.cpp`(`37a0d575`)를 가져가게 하거나, patch 2개(`git diff e9e7d29 f716ed3`, `VTASave.cpp` diff)를 준다.
- GitHub 이름 `jerry`, `jerryoh`, `jerry-oh`, `jerryjo`는 모두 사용 중이다. 변경은 계정 주인만 Settings에서 할 수 있다. 바꾸면 `.gitmodules`와 PC·보드 remote 주소를 고쳐야 한다.

**15:01 [PC] "nest-compiler 옆에 있는 xcmpl/은 ... 없애자. PC에서도 없애자."**
- 보드에는 이미 없었다. PC의 `/home/ubuntu/npuprg/nestc2_clion/vtabundles_xcmpl`(1.9 MB, 9-30)을 현재 dir과 비교한 뒤 지웠다. 그 dir에만 있던 것은 9-30 옛 스크립트, `Conv3Test/`, 9-30 로그였다.

**16:43 [보드] "MultiEVTAResnet18에서 어느 layer conv이 149GOPS까지 나오네. ... 합해서 600GOPS에 근접하지 않을까?"**
- single `_timed`와 multi `_timed` csv에서 모든 번들의 layer별 GOPS를 정렬했다.

  | GOPS | 시간 | layer | 번들 |
  |---|---|---|---|
  | 152.5 | 1.516 ms | conv | ResConv10Test (single) |
  | 150.8 | 1.534 ms | res5_2_branch2b | Resnet50Test |
  | 149.8 | 1.544 ms | stage4_conv1 | Resnet18PartitionTest |
  | 149.4 | 1.548 ms | stage4_conv3 | MultiEVTAResnet18 (multi4) |
  | 146.5 | 1.578 ms | res4_x_branch2b | Resnet50Test |

- 상위는 모두 231,211,008 ops 3x3 conv(512ch 7x7, 256ch 14x14)다. Conv4(64ch 56x56)는 123 GOPS다. EVTA 1개 이론 최대는 170.7 GOPS, 4개면 683 GOPS다.
- MultiEVTAResnet18의 149 GOPS는 동시 실행 값이 아니다(copy별 stage4_conv1 시작 시각 401.5, 475.9, 526.2, 404.3 ms). 그래서 `MultiEVTAResConv10Test`로 직접 재기로 했다.

**16:46 [보드] "진행하자." → 17:08 ResConv10 4-EVTA 동시 609.7 GOPS**
- `make_multi4conv4.py`를 `hw/` 아래 conv 하나짜리 번들이면 어느 것이든 복사하도록 일반화했다(`multiconv.h` 생성, main 공용). Conv4로 다시 생성한 결과는 주석·들여쓰기만 다르다. `MultiEVTAResConv10Test`를 multi2·4에 추가했다(커밋 `bc2abf3`).
- 측정 중 보드 ssh가 한 번 타임아웃됐다(직전 load 0.35, 이 세션 실행 전이었음).
- 측정: 333 MHz, conv만 400회, `OMP_NUM_THREADS=1`, 같은 조건 2~3회 반복. 단위 GOPS, —는 안 잰 조합이다. 모든 출력이 golden과 같다.

  | 비트스트림 | 번들 | EVTA 1개 | EVTA 2개 | EVTA 4개 |
  |---|---|---|---|---|
  | multi4 | ResConv10 (7x7x512→512) | 152.6 | — | **609.7** |
  | multi4 | Conv4 (56x56x64→64) | 123.5 | — | 491.6~493.4 |
  | multi2 | ResConv10 | 152.8 | 305.5 | — |
  | multi2 | Conv4 | 123.6 | 247.1 | — |

- 중요: 4개 동시가 1개의 정확히 4배다. EVTA마다 conv 한 번에 weight 2.36 MB를 읽어도 HP 포트·DDR 경쟁이 없었다. 이론 최대 682.7 GOPS의 89%다.
- 중요: 처음 558~579 GOPS로 낮게 나온 것은 `OMP_NUM_THREADS=1` 없이 돌려 OpenMP 팀이 코어 4개를 나눠 쓴 탓이다.
- CPU transpose까지 포함한 end-to-end는 296.6 GOPS(ResConv10, EVTA 4개)로 CPU가 병목이다.
- 보드에서 github.com 22번 포트가 타임아웃돼 push하지 못했다(443 ssh로 재시도). 이 세션의 `pkill -f "git push"`가 ssh 셸까지 죽여 한 번 실패했다. 21:42에 push·pull이 끝났다.

**21:04 [보드] "609GOPS라는 게 ... 각 evta GOPS를 단순히 합한 거지?" / 21:17 "231.2Mops는 어디서 나온 거야? ... 어떻게 4개가 돌기 시작한 시점부터 다 돈 시점까지를 잰 거야?"**
- 합이 아니라 wall 시간 기준 처리량이다. 4 × 400회 × 231.2 Mops = 369.9 Gops를 606.748 ms로 나눴다. 같은 측정의 EVTA별 시간:

  | EVTA | 400회 시간 | GOPS |
  |---|---|---|
  | 0 | 606.370 ms | 152.52 |
  | 1 | 606.558 ms | 152.47 |
  | 2 | 606.736 ms | 152.43 |
  | 3 | 606.521 ms | 152.48 |
  | 전체 (wall) | 606.748 ms | 609.71 |

  `OMP_NUM_THREADS=1` 없던 첫 측정은 EVTA별 313~332 ms로 달라, 합 569.6과 RESULT 557.9가 달랐다.
- 231.2 Mops = 2 × 1·7·7 × 512 × 512 × 3·3(MAC당 2 ops), `multiconv.h`의 `MC_OPS`.
- conv-only 모드: 버퍼 할당·입력 transpose를 앞에서 한 번, 출력 transpose·해제를 뒤에서 한 번 하고, 그 사이에 `convolution_wo_tr`(CPU 명령 생성 + EVTA 실행 + `VTASynchronize`)만 400번 부른다.
- 시간: 스레드 4개 + main이 start barrier에서 함께 출발하고, main이 end barrier(가장 늦은 EVTA 완료) 직후 `t_end`를 잰다. 오차는 수 µs다.
- 같은 conv를 4개가 각자 돌린 처리량이고, conv 하나의 latency(1.516 ms)는 줄지 않는다.

**21:29 [보드] "속도 측정 실험까지 여러 문서에 보충하자. ... 609GOPS 실험은 보고서를 자세히 ... 그 이후에 커밋과 push도 하자." / 22:01 "claude docs에 올려 공유 링크를 만들자."**
- 문서 작성 전에 `_prof`로 다시 쟀다. conv 한 번의 EVTA 시간은 1개 1.4473 ms, 4개 동시 1.4491 ms(+0.12%, DDR 경쟁 없음). conv 사이 CPU 간격 66 µs(4.4%). 측정 구간의 82.8% 동안 4개가 모두 돌았다.
- 새 보고서 `docs/evta_multi4_609gops_report.md`: ResConv10 구조와 양자화(출력 = clip((acc+bias)>>7)), `conv_kernel` 동작(7x7 tile, 입력 채널 블록 32개, 가상 스레드 2개), DDR 대역폭(EVTA당 1.66 GB/s), GEMM 가동률 93.5%(Conv4는 73%, 원인은 추정), 측정 방법, 재현 명령.
- `docs/vta_synth_notes.md`: §0, §2.4, §3.3(333 MHz까지 간 과정), §3.4(10-26에 찾은 문제 8가지), §6(측정 방법). 그 밖에 `log/BOARD_TODO.md`, `vtabundles_xcmpl/README.md`, `WORKLOG.md`, `CLAUDE.md`. 보드 commit `7365874` push, PC pull 완료.
- `MultiEVTAResConv10Test`는 직접 실행으로 golden만 확인했고 `run_all.sh`로는 아직 안 돌렸다.
- 보고서를 Claude Docs 문서로 올렸다(`claude.ai/artifact/Pq7QCnRFBgbqFZuaRYvtLd`, EVTA 1·2·4개 GOPS 막대 차트 포함). 처음에는 본인만 보고, Share로 공유, Export로 Word/PDF/Markdown을 받는다.

**10-27 00:21 [보드] "GEMM을 systolic 구조로 바꾸면 runtime이나 컴파일러에서 많이 바꿔야 할까? ..." / 00:34 "vta_synth_notes.md에 추가해 줘." / 00:37 "해줘."**
- ISA와 컴파일러(NEST-C)는 그대로 두고, 런타임은 vtalib의 `gemmop_mxn`, `gemmop_1x1` uop 순서만 바꾸면 된다. 지금 순서에서는 같은 weight가 7~8사이클만 이어져 systolic에서 가동률이 약 44%로 떨어진다. weight를 고정하고 출력 픽셀을 안쪽에서 돌리면 ResConv10 49사이클, Conv4 448사이클이다.
- HW 일이 크다: 출력 deskew와 열별 acc bank, acc hazard, 명령 경계 drain(명령마다 약 40~50사이클), RTL로 작성, weight SRAM 폭.
- MAC 수는 같으므로 이득은 클럭에서만 나온다. 확인 결과 rs_rst 333 MHz의 worst setup 경로는 `inp_mem_3` BRAM → `compute_3` GEMM DSP 입력(slack +0.008 ns, 배선 65%)으로 input broadcast 경로다. 상위 100개 경로는 아직 보지 않았다.
- 지난 답의 FC 걱정은 해당 없다. 지금 번들의 FC는 CPU 함수이고, conv tile은 최소 7x7이다.
- `docs/vta_synth_notes.md` §8(8.1~8.7)로 정리해 보드 commit `fc859af`(139줄 추가) push, PC pull했다. 다른 세션은 §8.7 순서로 시작한다.

## 결정이 남은 것 (10-27 07:00 기준)
- `/lib/firmware`의 `vtahw_multi4_y219_333.bin`(기본 `vtahw_multi4_y219.bin`과 같은 파일)을 지울지.
- 다른 유저용 patch 2개를 만들어 bit dir 옆에 두고 README에 "적용 방법"을 추가할지. bit dir README 사본을 저장소(`docs/bitstreams/` 등)에 둘지.
- GitHub 계정 이름 변경: 사용자가 새 이름을 정하면 `.gitmodules`와 remote 주소를 고친다.
- `MultiEVTAResConv10Test`를 `run_all.sh`로 돌려 확인하기.
- systolic 작업: §8.7 1단계(rs_rst timing report 상위 100개 경로 확인)부터 다른 세션에서 시작.
- 이미 결정됨: multi4 기본 firmware 333 MHz판, single 기본 bitstream Y219판, prof 빌드는 꼭 필요할 때까지 미루고 `exe_time` 사용, vtalib copy 3개 유지(원본과 코드·주석 동일), PC↔보드 동기화는 git으로만(보드에서 고친 것은 보드에서 push), 실행 lock, `VTA_MAX_XFER`를 vtalib CMake로 이전, PC의 옛 `vtabundles_xcmpl` 삭제.
