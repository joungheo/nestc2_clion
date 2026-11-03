# 2026-10-27 EVTA synth 요약: hw dir를 `script/`(git) · `result/` · `trace/`로 분리, 비트스트림 로드 helper `load_bit.sh`, systolic branch를 main에 fast-forward로 합침

- 기간: 2026-10-27 07:00 ~ 10-28 07:00 (KST)
- 이날 이 기록의 세션은 하나다. [보드]는 보드 `main`에서 ssh로 한 작업이다. 수정, commit, push는 모두 보드에서 했고, PC에서는 로컬 `main`을 따라오게 한 것과 git 밖의 파일 정리만 했다.
- 같은 시각 PC의 `systolic` branch에서는 다른 세션이 작업 중이었다(그 내용은 `vta_systolic_요약_261027.md`).

## 시간순

**09:35 [보드] "run_all.sh, run_prof.sh 등도 git에 포함시키고 싶어. … 그 아래 dir를 하나 만들어서 옮긴다든지 …" / 09:40 "잠깐 멈춰봐. 출력들을 옮긴다고 실행을 다시 해야 한다면 다른 방법을 생각하자."**
- 처음에는 출력(bundle dir, 실행 파일, `*_prof`, 로그, `results.txt`, `*_summary.txt`)을 각 hw dir의 `out/`으로 `mv`했다. 대상은 `hw/`, `hw_multievtax2/`, `hw_multievtax4/`.
- 다시 실행할 필요는 없었다. 옮긴 실행 파일은 그대로 실행된다. 다만 `build*/`에서 다음 `make` 때 소스 경로가 바뀐 bundle의 `.cpp`는 다시 컴파일된다(vtalib, Glow는 상관없음).
- 새 경로 확인으로 돌린 짧은 테스트:

  | 실행 | 결과 |
  |---|---|
  | multi4 `run_all.sh MultiEVTAResConv10Test` | PASS |
  | multi4 `run_prof.sh` (기존 `_prof` 실행 파일) | PASS |
  | single `run_all.sh ReluTest Conv4Test` | 둘 다 PASS |

- commit 전에 멈추고 이 방식과 "출력은 두고 스크립트만 따로" 중 선택을 물었다.

**09:43 [보드] "출력은 그대로 두고 스크립트만 hw/script/ dir를 만들어서 옮기고 이 script dir만 git에 넣는 걸로."**
- `out/`으로 옮겼던 것은 전부 원래 자리로 되돌렸다. 세 hw dir에 `script/`를 만들고 `run_all.sh`, `run_prof.sh`를 옮겼다. 그동안 git에서 빠져 있던 `hw/run_all.sh`도 이제 들어간다.
- 두 스크립트는 자기 hw dir로 직접 `cd`하므로 어디서 불러도 된다(예: `sudo hw/script/run_all.sh Conv4Test`).
- `.gitignore`는 hw dir에서 `script/`와 `*Src/`(`MultiEVTAConv4TestSrc`)만 빼고 전부 무시한다. `collect_hw_bundles.py`도 `script/`에 쓰게 바꿨고, README, CLAUDE.md, 609 GOPS 보고서 경로를 고쳤다. commit `f7230ea`.
- `collect_hw_bundles.py`로 single, multievtax4의 `run_all.sh`를 다시 만들면 commit한 내용과 같았다. 보드 확인: single `ReluTest` PASS, multi4 `MultiEVTAResConv10Test`는 `run_all.sh`와 `run_prof.sh`(`_prof`) 모두 PASS.

**10:00 [보드] "보드에서 git push 한 거야? PC에서 main에서 pull 해줘. 근데 systolic branch에서는 바뀐 사실을 모르나?"**
- 보드에서 push했다(`fc859af..f7230ea main -> main`). PC 작업 트리는 `systolic`이고 커밋 안 한 변경이 있어서, `git fetch origin main:main`으로 로컬 `main`만 fast-forward했다.
- `systolic`은 이 변경을 모른다. 두 branch는 `fc859af`에서 갈라졌고, `systolic`은 `vtabundles_xcmpl/`, CLAUDE.md, 보고서를 건드리지 않아 `git merge main`은 충돌이 없을 것으로 봤다.
- PC에서 다른 세션이 systolic 작업 중이라(확인 중에 auto commit `0427917`이 생김) merge, checkout은 하지 않았다.

**11:42 [보드] "script로 옮겼는데 run_all.sh이 왜 남아있어? 예전 버전인가? 그러면 없애자"**
- 남은 것은 PC의 `vtabundles_xcmpl/hw/run_all.sh`(10-17판)였고 지웠다. 예전 etri 비트스트림 고정(`BIN`으로 못 바꿈), flock 없음이 근거다.
- 원래 git이 무시하던 파일이라 merge, checkout으로 옮겨지거나 지워지지 않았다. 보드에는 원래 없었다.
- PC `systolic`에는 이미 `main`이 merge돼 있었다(`ccc4b4fb`).

**11:45 [보드] "hw/에 test 결과 .txt 파일들이 … result/ 라는 dir를 만들어서 옮기자. git 할 필요가 있을까?" / 11:49 "hw_multievtax2/4도 … 똑같이 해줘야지."**
- 결과 파일의 출처를 확인했다.

  | 파일 | 만든 것 |
  |---|---|
  | `results.txt` | `run_all.sh` (bundle마다 PASS, MISMATCH 등 한 줄) |
  | `prof_summary.txt`, `timed_summary.txt`, `sysprof_summary.txt` | `run_prof.sh` (`_prof`, `_timed`, `_sysprof`) |
  | `collect_report.txt` | `collect_hw_bundles.py` |
  | `test/261016_*.txt` | 스크립트가 아니라 10-16에 직접 저장해 둔 실행 결과로 보임 |
  | `run_all_y219.log` (보드에만) | 10-23 Y219 비트스트림 시험 때 nohup으로 받은 `run_all.sh` 출력 |

- 세 hw dir 모두 `result/`로 옮겼고, `run_all.sh`, `run_prof.sh`, `collect_hw_bundles.py`가 `result/`에 쓰도록 바꿨다(commit `3f0d49b`). multi2/4는 11:45 턴에서 이미 같이 해 둔 상태였다.
- 보드 확인: single `ReluTest` PASS(`hw/result/results.txt`에 기록), multi4 `run_prof.sh MultiEVTAResConv10Test` PASS(`hw_multievtax4/result/prof_summary.txt`에 기록).
- git에는 넣지 않는 것으로 정했다. 실행마다 덮어쓰고, 남길 숫자는 BOARD_TODO, README, 609 GOPS 보고서에 이미 있다. 손으로 남긴 `261016_*.txt`를 보관하려면 `log/` 같은 곳이 맞다고 제안했다.

**11:51 [보드] "sds_trace_data.dat는 trace/ 만들어서 옮기자."**
- 원인: PYNQ `libcma.so`가 프로세스 종료 때 현재 dir에 `sds_trace_data.dat`(32바이트)를 쓴다. 스크립트가 hw dir에서 `Xlnk().xlnk_reset()`을 실행해서 생긴 것이다.
- 세 dir의 `run_all.sh`, `run_prof.sh`가 `trace/`를 만들고 그 안에서 reset을 실행하게 했다(commit `10be7ce`). `/tmp`를 현재 dir로 두고 `ReluTest`(`run_all.sh`), multi4 `MultiEVTAResConv10Test`(`run_prof.sh`)를 돌려 둘 다 PASS, 새 trace는 `trace/`에만 생겼다.
- bundle 실행 파일도 자기 bundle dir에 같은 파일을 남기지만 출력 dir이라 그대로 두었다.

**11:54 [보드] "지금까지 PC main에서 작업한거야? 보드가 아니고? 그럼 보드에서 pull 해서 맞춰야지." / 11:56 "보드에서 push한 걸 PC에서 pull 부터하자. run_all.sh 지운 것과 trace도 양쪽에 똑같게 만들자."**
- 작업은 처음부터 보드에서 했다(`f7230ea`, `3f0d49b`, `10be7ce`). 보드는 최신이었다.

  | | commit |
  |---|---|
  | 보드 (`main`, 작업 트리) | `10be7ce` |
  | origin/main | `10be7ce` |
  | PC 로컬 `main` | `10be7ce` |
  | PC 작업 트리 | `systolic` (`main` 중 `f7230ea`까지만 merge됨) |

- PC는 `systolic`에 `main`을 merge하는 방식으로 pull했다(`14cd842b`, 충돌 없음). hw dir 구조(`script/`, `result/`, `trace/`)는 양쪽이 같고, 남은 차이는 실행 출력(multi bundle dir은 보드에만 등)뿐이다.
- 보드에만 root 소유 `sds_trace_data.dat`가 두 개 남았다(저장소 맨 위 10-23, `vtabundles_xcmpl/` 맨 위 10-26). 지울지 옮길지를 물었다.

**11:59 [보드] "둘다 지우고, .gitignore에 추가하자. 앞으로는 trace/에서 출력되게 하면 좋겠어."**
- 두 파일을 지우고 최상위 `.gitignore`에 `sds_trace_data.dat`를 추가했다(commit `2a3db89`, PC도 pull해서 `systolic` `eadda23e`).
- 파일 이름이 라이브러리에 고정돼 있어서, 손으로 비트스트림을 올릴 때 쓸 helper `hw/script/load_bit.sh`를 만들었다. firmware 쓰기, 같은 lock, `xlnk_reset`을 `hw/trace/`에서 실행한다.
  - 사용법: `sudo vtabundles_xcmpl/hw/script/load_bit.sh vtahw_multi4_y219.bin`
  - 보드에서 저장소 맨 위에서 실행: `fpga state operating`, trace는 `hw/trace/`에만 생겼다.
- `hwtest/`의 `run_rawtests.sh`, `run_rawtests2.sh`, `run_convvar.sh`도 reset을 `hwtest/trace/`에서 하게 바꿨다. **이 셋은 문법 검사만 했고 실제로 돌리지 않았다.**
- `vta_synth_notes.md` 비트스트림 로드 순서, 609 GOPS 보고서 재현 절차, README에 `load_bit.sh`를 적었다.

**10-28 00:28 [보드] "문서들 정리하고 커밋하자. systolic branch … 지금까지 거의 빌드만 해서 main에 merge 하는 게 좋지 않을까?"**
- 중요: **systolic은 "거의 빌드만"이 아니었다.** origin/systolic(`82755c22`)은 main 이후 commit 28개(마지막 10-27 23:34)다.
  - 새 dir `vta-hw-zcu102-systolic/`, `vtalib-zcu102-systolic/`(single 사본, 약 20만 줄) 위에 RTL `gemm_sa`, HLS compute(`SYSTOLIC=1`), cycle model, uop 순서 변경, uop cache 수정.
  - 결과: multi4 380 MHz(HP 333) ResConv10 conv-only **685.1 GOPS**(609.6 대비 +12.4%) 보드 통과, multi2 360/380 MHz 통과.
  - 문서 `docs/evta_systolic.md`(633줄), `docs/evta_systolic_design.md`, 그림 4개, `vta_synth_notes.md` 640줄 변경. `docs/ref/`(7.9 MB) 외부 자료(gemmini Scala 소스, autosa, tensil, gemm_hls README/LICENSE, arXiv PDF 0.9 MB).
  - 공용 파일(single, multi2, multi4의 `Makefile`, `vivado.tcl`, y219route flow) 변경은 `FREQ=<MHz>`를 줄 때만 적용돼, 기본 빌드는 지금 main과 같다.
- merge를 권했다: origin/systolic이 main을 다 포함하므로 fast-forward로 충돌이 없다. `docs/ref/`가 main 기록에 영구히 남는 것만 확인을 요청했다.

**10-28 00:34 [보드] "진행하자."**
- 보드 `main`을 `origin/systolic`(`82755c2`)으로 fast-forward했다(merge commit 없음, `docs/ref/` 포함 28 commits).
- 문서 정리 commit `8d29663`(보드에서 push):

  | 파일 | 고친 내용 |
  |---|---|
  | `CLAUDE.md` | `FREQ=<MHz>` 옵션(기본값 그대로), multi4 380 MHz(HP 333) 685.1 GOPS, `*-systolic` 사본과 "작업은 `systolic`에서 하고 main에 다시 합친다" 규칙, `load_bit.sh` |
  | `vtabundles_xcmpl/WORKLOG.md` | 새 §11 hw dir 구성(`script/`만 git, `result/`, `trace/`, 번들 dir은 빠짐), 명령 요약 경로 |
  | `docs/vta_synth_notes.md` | 손 로드 명령 → `load_bit.sh` + plreset, Ctrl-C 뒤 reset은 `hw/trace/`에서, 결과 위치 `result/` |
  | `docs/evta_systolic.md` | `hw/script/run_prof.sh` 경로, "2026-10-28 main에 합침" |
  | `vtabundles_xcmpl/evta_hw/README.md` | `hw/run_all.sh` → `hw/script/run_all.sh` |

- `log/BOARD_TODO.md`는 날짜별 기록이라 예전 경로를 그대로 두었다.
- 보드 `main`, origin/main, PC 로컬 `main` 모두 `8d29663`. PC 작업 트리는 `systolic`(`82755c2`)에 그대로 있다(다른 세션이 Vivado 실행 중, RTL 파일 10개 미commit). systolic은 이 문서 commit 하나만 뒤져 있다.

## 결정이 남은 것 (10-28 07:00 기준)
- PC `systolic`에 `main`(`8d29663`)을 다시 merge하는 것: systolic 세션 작업이 끝난 뒤 하기로 하고 남겨 두었다. 같은 문서의 몇 줄을 양쪽이 고쳐서 충돌 가능성은 낮지만 0은 아니다.
- `hwtest/`의 `run_rawtests.sh`, `run_rawtests2.sh`, `run_convvar.sh`는 trace 위치 변경 후 실제 실행 확인이 아직이다.
- `hw/result/`의 `261016_*.txt`(손으로 남긴 비교 기록)를 `log/` 등으로 옮겨 git에 넣을지는 답이 없었다.
- 결정된 것: hw dir는 `script/`만 git, 결과는 `result/`, trace는 `trace/`(git 밖). `sds_trace_data.dat`는 최상위 `.gitignore`에 추가. 손으로 비트스트림을 올릴 때는 `load_bit.sh`. systolic은 main에 합쳤고(`docs/ref/` 포함), 이후 작업은 `systolic`에서 하고 main에 다시 합친다.
