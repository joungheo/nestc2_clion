# 2026-10-28 EVTA synth 요약: systolic 세션 상태 점검(380 MHz systolic 보드 동작 commit 확인), main/systolic branch 맞추기 제안, vta_synth 일일 보고서 첫날(10-19) 확인

- 기간: 2026-10-28 07:00 ~ 10-29 07:00 (KST)
- 이날 기록에는 한 세션만 있다. [PC]는 PC에서 돌린 vta_synth 세션이다. PC 작업 트리와 보드 상태를 점검했고, 보고서 도구 결과를 확인했다.

## 시간순

**08:28 [PC] "systolic 쪽은 계속 하고 있어?"**
- systolic 세션은 쉬고 있는 상태로 보인다. 마지막 활동은 5시간쯤 전(10-28 03:36 commit)이다.
- 지금 도는 작업은 없다. PC에 Vivado가 없고, 보드에서도 EVTA 실행이 없다.
- 그 세션의 프로그램(`claude --resume`, 20시간째)은 켜져 있지만 입력을 기다리는 상태로 보인다.
- PC 작업 트리는 깨끗하다. 전날 commit하지 않았던 RTL 수정까지 모두 commit되어 origin/systolic(`ac1faeb0`)에 push되어 있다.
- main에 합친 뒤 systolic에 생긴 commit 3개는 다음과 같다.

  | 시각 | commit | 내용 |
  |---|---|---|
  | 00:53 | `bb1e13e9` | systolic 비트스트림이 보드 probe 시험 통과 |
  | 01:54 | `f24a5504` | `gemm_sa`가 GEMM 의존성 신호(dependence token)를 직접 보냄. 333 MHz에서 HLS GEMM과 같은 속도 |
  | 03:36 | `ac1faeb0` | 333 MHz를 넘으면 HP 포트를 별도 클럭으로 분리, BRAM mux select에 레지스터 추가. 380 MHz systolic이 보드에서 동작 |

- 빌드 dir에는 400 MHz 시도(`_sa_400mhz`, 02:47)도 있다. 380 MHz로 commit한 것으로 보아 400은 통과하지 못한 것 같다(추정, `docs/evta_systolic.md`에서 확인 필요).
- branch 상태: systolic이 main보다 3개(위 commit들) 앞서고, main이 systolic보다 1개(문서 정리 `8d29663`) 앞선다.
- 제안: systolic 세션이 쉬는 지금 맞춘다. 1) 보드 main에 `origin/systolic`을 merge하고 push, 2) PC `systolic`에서 `git merge main`. 사용자 확인을 물었다.

**15:12 [PC] "vta_systolic 세션에서 daily report를 작성해서 docs/dailyreport/vta_systolic/에 저장했어. 이 세션에서도 같게 첫 날부터 요약해서 vta_synth/ 아래 저장하자. 일단 제일 첫날만 해봐."**
- 첫날(10-19) 보고서 `docs/dailyreport/vta_synth/vta_synth_요약_261019.md`는 이미 있었다. 15:10~15:12에 다른 세션이 만든 것으로 보인다. 이 세션의 기록은 10-19 12:44 KST부터 있으므로 10-19가 첫날이 맞다.
- vta_systolic 세션이 `tools/daily_report.sh`에 `SET=vta_synth`를 추가했다(이 세션 `57e1dbd5` 기록만 읽음). 생성 명령은 `SET=vta_synth daily_report.sh 261019`로 추정. 도구 수정 3개 파일은 아직 commit되지 않았다.
- 내용을 처음부터 끝까지 확인했다. 범위는 10-19 07:00 ~ 10-20 07:00, 형식은 vta_systolic 보고서와 같고 요청 27개가 시간순으로 정리되어 있다. 주요 내용:
  - branch가 갈라진 일과 remote 이름 변경
  - `vta-hw-zcu102-single` / `vtalib-zcu102-single` 생성, 헤더 검증
  - zcu102 타깃 추가, B1~B7 수정
  - Vivado 2020.1 docker 설치(첫 설치 실패로 boost 헤더가 지워졌다가 복구)
  - 첫 비트스트림: 333 MHz, WNS +0.027
  - `_prof` 비트스트림 세 번 빌드, 최종 WNS single +0.054, multi2 +0.004, multi4 +0.003
  - `BOARD_TODO.md` 인수인계
- 수치는 세션이 기억하는 결과 및 CLAUDE.md 값과 맞았다.
- 나머지 날짜(261020~261027)도 한 번에 만들지 물었다:
  `SET=vta_synth docs/dailyreport/tools/daily_report.sh 261020 261021 261022 261023 261024 261025 261026 261027`

## 결정이 남은 것 (10-29 07:00 기준)
- main과 systolic branch 맞추기(보드 main에 `origin/systolic` merge·push, PC `systolic`에서 `git merge main`): 사용자 답을 기다린다.
- vta_synth 보고서 나머지 날짜(261020~261027) 생성: 사용자 답을 기다린다.
- `daily_report.sh`의 `SET=vta_synth` 추가 등 도구 수정 3개 파일이 commit되지 않은 상태다.
- systolic 400 MHz가 통과하지 못했는지는 기록상 미확인(`docs/evta_systolic.md` 확인 필요).
