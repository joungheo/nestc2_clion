# 최근 변경 (두 PC와 보드가 서로 알아야 할 것)

호스트 PC 두 대(`ubun`: user `ubuntu`, `ubuntu2`: user `jerry`)와 보드가 같은 저장소로 일한다. 소스는 git으로 합쳐지지만
빌드한 것(model-compiler, 실행 파일, bitstream)은 git에 없다. 그래서 **다른 쪽이 무엇을 해야 하는지**를 여기에 적는다.

**쓰는 법**
- `git pull` 한 뒤 이 파일의 맨 위부터 지난번에 본 줄까지 읽는다.
- push할 때, 다른 PC나 보드가 무엇을 해야 하는 변경이면 **맨 위에** 항목을 하나 더한다(같은 commit에 넣는다).
  - 해야 할 일이 없는 변경(문서만, 측정 기록만)은 적지 않아도 된다.
- 항목 형식: `### <날짜 시각> <PC> <commit> — <한 줄 요약>`, 그 아래에
  - **바뀐 것**: 파일과 내용 한두 줄
  - **다른 쪽이 할 일**: 아래 중 해당하는 것
    - `pull`만
    - `model-compiler 다시 빌드` (`lib/`, `include/`, `tools/`, `cmake/`가 바뀜; 보드 `Debug2_remote`, PC `nestc-build` 각각)
    - `bundle 다시 생성` (컴파일러 출력이 바뀜: 어느 Dir인지)
    - `실행 파일 다시 빌드` (`vtalib-zcu102-systolic/`, `vta/vtalib`, `vtabundles_xcmpl/` CMake가 바뀜: 어느 build dir인지)
    - `bitstream` (새 판: 보드 경로 `bit/<dir>/`, `/lib/firmware` 파일 이름, 짝이 되는 vtalib commit)
- 오래된 항목은 지우지 않는다. 자세한 내용은 항목에서 다른 문서로 링크한다.
- 지금 빌드된 것이 어느 commit인지는 파일 자체에 적혀 있다(2026-11-24부터, [evta_user_manual.md](evta_user_manual.md) §4.4 "빌드 흔적"):
  bundle `.cpp`의 둘째 줄 `// model-compiler:`, 실행 파일의 `NESTC_BUILD` 문자열(`run_prof.sh`가 실행마다 찍는다).

---

### 2026-11-25 ubun d4d8ddf2 — int8 입력을 고를 수 있음(기본 float): `.i8` 파일, `INPUT_INT8_SAVE`, `PROF_INPUT` (MobileNetV2Tf multi4 247.4 → 253.2 fps)
- **바뀐 것:** `vtalib-zcu102-systolic` `VTAInputInt8.h`(새 파일), `VTABundle.cpp`, `CPUBundle_generic.cpp`(float 묶기의 quantize를 `PreprocessRow`로 뺌, 같은 바이트).
  공통 main `multi_threads_main.cpp`(10곳 같은 파일), `vtabundles_xcmpl/hw*/script/run_prof.sh`(`PROF_INPUT`). [evta_dpu_compare.md](evta_dpu_compare.md) §6.10.
- **다른 쪽이 할 일:** `pull`. `.i8` 입력을 쓰려면 `실행 파일 다시 빌드`(systolic vtalib의 build dir). bundle과 model-compiler는 그대로다. png 입력은 전과 같다.

### 2026-11-25 ubun 0b1439b7 — VTASave `-vta-skip-evta-invalidate`: EVTA만 읽는 출력의 invalidate를 뺌 (MobileNetV2 −0.5 ms)
- **바뀐 것:** `lib/Backends/VTA/VTASave.cpp` `dropEvtaOnlyInvalidates`(기본 켬; bundle `.cpp`의 옵션 목록에 줄이 하나 는다).
  CNN bundle의 invalidate가 하나만 남는다. DeiT는 그대로다. MobileNetV2Tf multi4 246.5 → 254.5 fps([evta_dpu_compare.md](evta_dpu_compare.md) §6.9).
- **다른 쪽이 할 일:** `model-compiler 다시 빌드`(보드 `Debug2_remote`, PC `nestc-build`). `bundle 다시 생성`: CNN bundle(MobileNetV2 / Resnet / VGG16, single과 multi4)은
  다시 만들 때 invalidate가 빠진다. 다시 만들지 않아도 결과는 같다(시간만 다름). vtalib는 바뀌지 않았다.
  - 2026-11-25 18:10 ubun 확인: 보드 `Debug2_remote`의 model-compiler를 다시 빌드했다(`VTASave.cpp`만, 2분 44초). multi4 MobileNetV2 / Resnet18 / VGG16을 옵션을
    켜고 끄고 만들어 비교했다(`~/JO/ubun_wt/gen/`). 바뀐 것은 invalidate 줄(52 → 1, 21 → 1, 17 → 1)과 옵션 주석 한 줄뿐이고, weights는 같다. 측정에 쓴 도구의 출력과
    바이트가 같다. MobileNetV2 실행: conv 출력 dump 208개 같음, `EVTA_RES_CHECK` 228개 0 differ, 282 (0.920282). MobileNetV2Tf도 같다(model을 ubun에서 다시 만듦, md5 같음;
    [evta_dpu_compare.md](evta_dpu_compare.md) §6.9). 공유 `Debug2_remote/vta/multievta_bundles`와 `vtabundles_xcmpl/hw_multievtax4`의 bundle은 다시 만들지 않았다.

### 2026-11-25 ubun b5c4e7d — vtalib-systolic: conv0 입력 묶기를 빠르게 (MobileNetV2 1.23 → 0.73 ms, 같은 바이트)
- **바뀐 것:** `vtalib-zcu102-systolic/lib/Bundle/CPUBundle_generic.cpp` `conv0packRows`, `preprocess_conv0pack_rows`
  (3×3 / 2를 3채널에 거는 conv0의 묶기, prefetch, offset 0이면 덧셈 없음). `VTABundle.cpp`: `EVTA_INPUT_STREAM_TIME=1`(`163dd9b`).
  MobileNetV2Tf 혼자 14.84 → 14.35 ms, multi4 238.3 → 246.5 fps([evta_dpu_compare.md](evta_dpu_compare.md) §6.8). 다른 모델의 conv0(7×7 등)은 그대로다.
- **다른 쪽이 할 일:** `실행 파일 다시 빌드`(systolic vtalib의 build dir; bundle은 그대로). 보드 공유 트리는 ubuntu2의 commit하지 않은
  수정(`vtalib-zcu102-systolic` 15개 파일, `run_prof.sh`, `sched_search.py`)이 있어 pull하지 않았다. 측정은 `~/JO/ubun_wt/`(git archive)의
  따로 빌드한 실행 파일(`<Dir>/*_pcsysubun`)로 했다.

### 2026-11-24 ubuntu2 744b1fb — MobileNetV2 112×112 층 3개의 schedule을 4개 동시 실행에서 다시 찾음, multi4 main에 `MULTI_FREE` / `MULTI_STAGGER_US`
- **바뀐 것:**
  - `include/Backends/VTA/VTASchedules.h`: 112, 112, 32 → 16 (1, 16, 112), 112, 112, 16 → 96 (2, 2, 112), 첫 depthwise 112, 112, 32 (2, 4, 112).
    multi4 처리량 +0.9–1.5 %, EVTA 1개는 느려지지 않음, conv dump 208개 같음([evta_sched_search.md](evta_sched_search.md) §5).
  - `vtabundles_xcmpl/sched_search.py`: `SCHED_SET=hw_multievtax4 SCHED_EXE=<exe>`(4개 동시 탐색), depthwise 후보. `hw*/script/run_prof.sh`: `PROF_EXE`.
  - `multi_threads_main.cpp`(공통본 10개): `MULTI_FREE=1`, `MULTI_STAGGER_US`, `THROUGHPUT` 줄([evta_dpu_compare.md](evta_dpu_compare.md) §6.6).
- **다른 쪽이 할 일:**
  - `pull`.
  - `model-compiler 다시 빌드`.
    - 이유: schedule 표 `VTASchedules.h`는 `include/` 아래 header라 model-compiler 안에 컴파일된다. bundle이 conv 호출의 인자로 그 값을 적기 때문이다.
    - 다시 빌드하지 않은 model-compiler로 MobileNetV2 bundle을 만들면 옛 schedule이 들어간다. 결과는 같고 multi4가 약 1 % 느릴 뿐이다.
    - 다른 모델의 bundle은 바뀌지 않는다(이 세 모양이 없다).
  - `bundle 다시 생성`은 급하지 않다. MobileNetV2(Tf / QAT / Val) bundle은 다음에 다시 만들 때 반영된다. 그 전에는 실행할 때 `EVTA_SCHED='112,112,32,0,3,1:2,4,112;112,112,32,16,1,1:1,16,112;112,112,16,96,1,1:2,2,112'`로 같은 효과를 낸다.
  - multi4 / single 실행 파일에서 `MULTI_FREE`를 쓰려면 해당 build dir에서 다시 빌드한다(보드 `build_multievtax4_sys`, `build_sys`는 11-24에 다시 빌드했다).

### 2026-11-24 ubuntu2 16ff33a — ResNet50Tv의 CPU 전처리와 layer4.0 relu를 EVTA로 (EVTA 1개 68.41 → 63.68 ms, multi4 52.9 → 56.6 fps)
- **바뀐 것:** [evta_dpu_compare.md](evta_dpu_compare.md) §6.5.
  - `lib/Backends/VTA/VTASave.cpp`: `findInputPreprocess`가 제자리 elemsub / elemdiv를 받는다. `findResidualFusions`: relu가 2^-q 큰 scale로 갈 때(q < 0) `CLIP` + `RQ round 1`, 제자리 add의 residual은 만든 conv를 하나 더 센다.
  - `vtalib-zcu102-systolic/lib/Bundle/VTABundle.cpp` `lowerEpilogue`: 단항 kind 3 `clip((x + 2^(k-1)) >> k)`를 기본 ALU로. `tests/epi_conv_test.cc`에 경우 추가.
- **다른 쪽이 할 일:** `model-compiler 다시 빌드`. `bundle 다시 생성`: `Resnet50TvTest`(`gen_calib_bundle.sh Resnet50TvTest ../vta/bundles/Resnet50TvTest/calib/resnet50_tv_calib_n200_b2048.yaml.gz`), `MultiEVTAResnet50TvTest`(`gen_multi4_bundles.sh`); 다른 bundle은 같다(이 꼴이 없다). `실행 파일 다시 빌드`(systolic vtalib의 모든 build dir: 새 bundle은 새 vtalib가 있어야 EVTA에서 돈다; 옛 vtalib는 그 conv의 relu를 `_alux*`에서 ALU 표로, 그 밖의 판에서 CPU로 한다: 결과는 같다).
- 보드: `hw/Resnet50TvTest`, `hw_multievtax4/MultiEVTAResnet50TvTest`의 bundle과 `build_systimed` / `build_multievtax4_systimed`의 그 실행 파일은 11-24 18:06에 새로 만들었다(`VTABundle.cpp`는 scp로 먼저 넣었다: pull 전에 `git checkout vtalib-zcu102-systolic/lib/Bundle/VTABundle.cpp`). 옛 것은 `~/JO/r50tv_old261124/`.

### 2026-11-24 ubun 4c2ee3f — 빌드 흔적: bundle과 실행 파일에 compiler / vtalib commit을 남긴다
- **바뀐 것:**
  - `lib/Backends/VTA/VTASave.cpp` `compilerStamp`: bundle `.cpp`의 옵션 목록 다음 줄에
    `// model-compiler: compiler sources at <commit> (<날짜>)[ + uncommitted changes], binary built <날짜> on <host>`.
    model-compiler가 그 commit보다 오래됐으면 `WARNING: ... rebuild model-compiler`도 붙는다.
  - `vtabundles_xcmpl/build_stamp.cmake`, `CMakeLists.txt`: 모든 실행 파일에 `NESTC_BUILD vtalib=<dir> <commit> <날짜>[ dirty] host=<host> dir=<build dir>`.
  - `vtabundles_xcmpl/bininfo.sh` `exe_info`, `hw*/script/run_prof.sh`: 실행마다 `exe :`, `bundle :` 줄을 찍고(summary에도),
    bitstream의 기능보다 실행 파일의 vtalib가 오래됐으면 `WARNING`을 찍는다(예: `_amul` 판에 `398f358` 전 vtalib).
- **다른 쪽이 할 일:** `pull`. `model-compiler 다시 빌드`(그래야 새 bundle에 줄이 생긴다). 실행 파일은 다음 빌드 때 저절로 흔적이 들어간다
  (`vtabundles_xcmpl` CMakeLists가 바뀌어 각 build dir에서 cmake가 한 번 다시 돈다).
- 보드에서 확인: model-compiler를 다시 빌드했다(16:44). 다시 만든 Resnet18 bundle의 줄은 `compiler sources at 4c2ee3f (2026-11-24 16:41), binary built 2026-11-24 16:44 on pynq`,
  그 줄을 뺀 나머지는 전과 같다. 다시 링크한 multi4 Resnet18 실행 파일은 `vtalib=vtalib-zcu102-systolic 398f358 ... host=pynq dir=build_multievtax4_systimed`.
  다른 실행 파일과 bundle은 다시 빌드 / 생성할 때 흔적이 생긴다(그 전에는 `no build stamp`로 나온다).
- bundle을 바이트 비교할 때는 이 줄을 빼고 비교한다: `diff <(grep -v '^// model-compiler:' a.cpp) <(grep -v '^// model-compiler:' b.cpp)`.

### 2026-11-24 ubun 90f05a6 — multi4 Resnet18 / VGG16 / DeiT bundle을 지금 컴파일러로 다시 만듦
- **바뀐 것:** `vtabundles_xcmpl/gen_multi4_bundles.sh`가 single과 같은 모델 / profile / 옵션을 쓴다(Resnet18 200장 profile, VGG16 정규화 모델 +
  100장 profile, `-vta-round-shift`, DeiT pruning 모델 + `symmetric`). VGG16 `multi_model.h` `MODEL_EXPECT` 0 → 8.
  보드의 `hw_multievtax4/MultiEVTA{Resnet18,Vgg16Cifar10,DeiT}Test/`의 bundle과 `build_multievtax4_systimed` 실행 파일은 다시 만들었다.
  결과: [evta_isa_ext.md](evta_isa_ext.md) 표 E 아래.
- **다른 쪽이 할 일:** PC에서 multi4 `_pcsys*`를 cross build한다면 보드의 새 `hw_multievtax4/<Dir>/*.cpp`를 가져다 쓴다
  (또는 `gen_multi4_bundles.sh`로 같은 것을 만든다: copy 0 = single bundle).

### 2026-11-24 ubun 0fd4f6a — multi4 `SYS_BIN` 기본 = `_4evta_sa_amul_400mhz_hp322_rq.bin`
- **바뀐 것:** `vtabundles_xcmpl/collect_hw_bundles.py`의 `SYS_BINS`(이것이 `hw_multievtax4/script/run_all.sh`를 다시 쓴다). single은 `_alux3` 그대로.
  bitstream 이름 읽는 법(`_amul`, `_amul_bc`, `_rq2`, `_hp322`): [evta_isa_ext.md](evta_isa_ext.md) §5.
- **다른 쪽이 할 일:** `pull`. `run_all.sh`의 `SYS_BIN`을 손으로 고치지 말고 `collect_hw_bundles.py`를 고친다.

### 2026-11-23 ubun 398f358 — ALU MUL immediate만 (`_amul`), vtalib `VTAHasAluMul`
- **바뀐 것:** `vtalib-zcu102-systolic`: 이름에 `_amul` 또는 `_alux`가 있으면 avgpool을 EVTA ALU MUL로(`avgpool_evta`). HLS `vta.cc` `ALUMUL=1`.
  bitstream: single `..._sa_amul_400mhz_rq.bin`, multi4 `..._4evta_sa_amul_400mhz_hp322_rq.bin`(보드 `bit/evta_multi4_amul_400MHz_pass_261123/`).
  [evta_isa_ext.md](evta_isa_ext.md) §5.2 ⑨.
- **다른 쪽이 할 일:** `실행 파일 다시 빌드`(systolic vtalib 쓰는 모든 build dir; PC `_pcsys*`도). 그 전 vtalib 실행 파일은 `_amul` 판에서 avgpool을 CPU로 한다.
  보드 `build_systimed`, `build_multievtax4_systimed`는 11-23 / 11-24에 다시 빌드했다.

### 2026-11-22 d5838ff, ab4aacf — 정확도 수정, 200장 profile, VGG16 입력 정규화, `-quantization-pow2-mse`
- **바뀐 것:** `VTASave.cpp`(첫 conv bias의 round shift 항), `VTA.cpp`, `Quantization.cpp`, `VTASchedules.h`(860d8d3). [evta_accuracy.md](evta_accuracy.md) §6, §7.
- **다른 쪽이 할 일:** `model-compiler 다시 빌드`, `bundle 다시 생성`. 보드 `Debug2_remote`는 11-24에 다시 빌드했다(그 전 11-21 빌드는 이 수정이 없었다).
