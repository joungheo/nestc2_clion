# 2026-11-01 EVTA systolic 요약: multi4 멈춤이 run 앞 5 µs 대기로 사라짐(원인은 미확정), fc를 NEON 0.74 ms → EVTA 0.152 ms로(bit-exact), buffer pool로 Alloc+Free 2.7 → 0.05 ms

- 기간: 2026-11-01 07:00 ~ 11-02 07:00 (KST)
- 세션은 하나다. [PC]는 PC의 `main`에서 코드를 고치고, 보드에서 pull해 ssh로 빌드·측정한 작업이다.
- 시각은 KST다. "보드 시간"이라고 쓴 시각만 보드 시계 기준이며, KST와 다르다.

## 시간순

**11:29 [PC] "A로 진행하자." (vtalib buffer pool)**
- `VTABufferFree`가 buffer를 바로 해제하지 않고 크기별 목록에 남기고, 같은 크기의 `VTABufferAlloc`이 꺼내 쓴다. 그래서 `cma_alloc`은 첫 run에서만 불린다.
- multi4는 4개 thread가 목록을 같이 쓰므로 mutex로 보호한다. 목록에는 `EVTA_BUF_POOL_MB`(기본 32) MB까지만 남기고, `0`이면 예전처럼 바로 해제한다.
- 고친 곳은 `vta/vtalib/lib/zcu102/api.cc`와 `vtalib-zcu102-systolic`의 같은 파일이다. bundle은 다시 만들지 않았고, 보드에서 build dir 14개를 다시 빌드했다.

pool을 켠 경우와 `EVTA_BUF_POOL_MB=0`으로 끈 경우를 비교했다. systolic 400 MHz, `-r`, run 2~25 중앙값이다.

| 경우 | pool 끔 | pool 켬 |
|---|---|---|
| Resnet18 single, OMP 1 | 46.97 ms | **44.27 ms** (−5.7 %) |
| Resnet18 single, OMP 4 | 41.89 ms | **39.07 ms** (−6.7 %) |
| run당 Alloc 23번 + Free 23번 | 2.7 ms | **0.05 ms** |
| multi4 4 copy 100회 | 1286 ms | **1189 ms** (84.1회/s) |
| single exe 100회 | 4740 ms | **4460 ms** |
| Conv4Test, 4 EVTA, 100회 전체 (3번) | 322~325 ms | **243~254 ms** |
| ResConv10Test, 4 EVTA, 100회 전체 (3번) | 172~189 ms | 147~176 ms |

- ResConv10은 켜도 꺼도 run마다 차이가 커서 pool의 효과라고 말하기 어렵다.
- 첫 run의 200 ms alloc은 OMP 4일 때만 생긴다(OMP 1은 1.5 ms). pool이 없으면 run 2에도 생겼고, 켜면 run 1에만 남는다. 처음 buffer를 잡을 때 커널 쪽에서 걸리는 비용으로 보인다.
- 정확성: hw와 multi4의 `run_all.sh` golden 모두 PASS다. Resnet18은 Result 281, Confidence 0.655530으로 그대로라, 재사용 buffer에 이전 데이터가 남아 있어도 결과가 같았다. 이때 multi4 Resnet18 실행 파일은 Result를 출력하지 않아 직접 확인하지 못했다. dmesg에 SError, Oops는 없다.
- commit: vtalib 0887a0a, main 0b2a0bcd, 12adebd4(문서, `docs/evta_runtime_compare.md` §7.5와 `CLAUDE.md`).

**12:32 [PC] (MultiEVTAConvTest 출력 붙여넣기) / 12:33 "이건 왜 이렇게 많이 돌리고 많이 출력해?" / 12:38 "interrupt 한 적 없는데."**
- interrupt 표시는 사용자가 누른 것이 아니라 도구 쪽에서 생긴 것으로 보인다. 작업은 마지막 측정까지 끝나 있었고, 빠진 것은 마무리 보고뿐이었다.
- 많이 돈 이유: `multievtaConvTestBundleMain.cpp`가 EVTA마다 10000번 고정이었고 `MULTI_RUNS`를 읽지 않았다. 8192는 `_prof`의 `PROFILE_MAX_EVENTS` 한도다.
- 많이 출력한 이유: 10-31에 `prof_summary.py`에 넣은 "VTA ms per run" 줄이 모든 run을 출력했다.
- 수정(d607f406): main이 `MULTI_RUNS`를 읽고(없으면 10000번), run이 30을 넘으면 처음 10개와 마지막 5개만 보인다. multi4 build dir 6개 빌드 rc=0, 다시 돌리니 EVTA마다 25 runs, PASS다.

사용자가 붙인 `_prof` 출력이다. VTA2, `multievtaConvTestBundle2.cpp`, 8192 runs, conv 하나다.

| Mops | run1 ms | run2 ms | run3 ms | run8192 ms | GOPS1 | GOPS2 | GOPS3 | GOPS8192 |
|---|---|---|---|---|---|---|---|---|
| 3.6 | 0.054 | 0.049 | 0.049 | 0.049 | 66.90 | 73.73 | 73.73 | 73.73 |

- 이 test는 4 EVTA를 동시에 돌려도 약 78 GOPS다. 각 EVTA의 시작이 run마다 최대 4 ms씩 어긋나 실제로 겹치는 시간이 거의 없기 때문이다.

**12:44 [PC] "`sudo ./run_prof.sh -s _systimed -r -l` 로 돌렸는데 여기서 멈추는데." (`MultiEVTAResnet18Test`)**

멈추기 직전에 끝난 ResConv10 `_systimed` 결과다(사용자가 붙인 출력). 위는 copy 하나(`timing_vtaResConv10TestBundle3.csv`), 아래는 4 copy 동시이며 모두 runs 2-101이다. 924.8 Mops per run(전부 EVTA)이고, 4 copy 줄에 "3.99 EVTAs at once on average"가 찍혔다. 출력에는 `lockstep (-l): off`로 찍혀 있어 명령의 `-l`과 다르며, 이유는 기록에 없다.

| | whole run 중앙값 (min, max) | whole GOPS | conv 중앙값 (min, max) | conv GOPS | cpu conv/fc |
|---|---|---|---|---|---|
| copy 하나 | 1.421 ms (1.415, 1.494) | 162.76 | 1.293 ms (1.291, 1.304) | 178.78 | 0.000 ms |
| 4 copy 동시 | 1.423 ms (1.418, 1.500) | 649.81 | busy union 1.296 ms (1.294, 1.327) | 713.67 | 0.000 ms |

- 멈춘 경과(보드 시간): 03:40:38 시작, 4 copy가 76 runs까지 함께 돌고 77번째에서 멈췄다. 5분 동안 ssh도 안 됐다. `chrt -f 10` thread가 끝나지 않는 EVTA를 polling하며 CPU를 잡아 네트워크 처리까지 밀린 것으로 추정한다(미확인).
- 03:45:40 `timeout 300`이 프로세스를 죽였고(exit=124), 다음 bitstream load가 `cma_alloc: alloc failed, req-size: 6473 pages, ret: -12`로 실패했다. 다음 single exe는 지난번 넣은 검사가 거부해서(exit=3) SError, Oops는 없었다.
- 보드 pull 문제: `collect_hw_bundles.py`가 `run_all.sh`를 주석 없이 다시 생성해 git과 달라졌다. git의 파일에서 주석을 지웠다(8176bae). 보드의 생성 파일은 `stash@{0}`에 넣고 pull했다.
- `run_times.py`의 긴 출력도 run이 30개를 넘으면 처음 10개와 마지막 5개만 보이게 했다. load 재시도는 아래 18:30 항목의 표에 정리했다.

**17:47 [PC] "fc 를 EVTA GEMM으로 옮기는 건 어떻게 가능해? 아니면 CPU에서 코드 최적화는 어떻게 하는 거지?" / 17:52 "1번부터 진행해보자."**
- fc는 입력 512, 출력 1000, weight int8 512 KB다. 입력·weight·출력 offset이 모두 0이고, 출력은 오른쪽 8비트 shift다.
- 두 방법을 제시했다: 1) NEON intrinsic, 2) 1×1 conv로 EVTA에. 작업량이 작은 1번을 권했다(예상 4.4 → 0.3 ms 안팎, 추정).
- 중요: 원인은 벡터화(처음 추정)만이 아니었다. 앞 conv들이 지나가면 weight 512 KB가 cache에 없는데, 예전 코드는 1000바이트 간격으로 64바이트씩 건너뛰며 읽어 DRAM에서 prefetch가 거의 안 됐다.
- 변경(`cpuFullyConnected`, 두 복사본): weight 행을 순서대로 8행씩 읽고, 출력 64개의 합을 NEON register에 두며, 다음 8행을 prefetch한다.
- 결과 확인: 정수 합이라 bit 단위로 같다. 크기·offset·thread 수 576가지를 기준 구현과 비교해 모두 같았고, FCTest PASS다.

NEON fc 전후를 비교했다. "이전"은 buffer pool을 넣은 뒤 값이다. systolic 400 MHz, `-r`, run 2~25 중앙값이다.

| 경우 | 이전 (pool 후) | 지금 |
|---|---|---|
| fc, OMP 1 | 4.43 ms | **0.74 ms** |
| fc, OMP 4 | 1.20 ms | **0.77 ms** |
| Resnet18 single, OMP 1 | 44.27 ms | **40.56 ms** |
| Resnet18 single, OMP 4 | 39.07 ms | **38.68 ms** |
| multi4, 4 copy 100회 | 1189 ms | **1089 / 1131 ms** (약 90회/s) |
| single exe 100회 | 4460 ms | **4094 ms** |

- multi4 `_systimed`(`-l` 없이): 4 copy 동시일 때 copy 하나의 run이 41.2 ms다. 이전 측정은 49.5 ms인데 그 뒤 pool도 들어갔으므로 전부 fc 덕분은 아니다. fc는 copy마다 0.77 ms다.
- 정확성: hw `run_all` 30개 PASS, multi4도 PASS. Result 281, Confidence는 그대로다(Resnet18 0.655530, Resnet50 0.702210, Partition 0.530282).
- OMP 4는 bench에서 0.23 ms인데 bundle 안에서는 OMP 1과 거의 같다. thread를 깨우는 비용으로 짐작만 하고, 확인하지 않았다.
- 중요: 측정 중 multi4 `_sys` 100회가 4번 중 2번, 68번째 run 근처에서 멈췄다. `-l` 없이도 멈췄으므로 **lockstep만의 문제가 아니다**(12:47에 lockstep을 의심한 것의 정정). 03:40(보드 시간) 멈춤은 fc 변경 전이라 fc와는 상관없어 보인다. pool과의 관련은 확인하지 못했다.
- commit: vtalib e69d3d7, main cb4a5446, 5e6e071, b58b6be1(문서, §7.6과 `CLAUDE.md`).

**18:19 [PC] "multi4가 멈추는 원인을 찾아보자." / 18:22 "vta_systolic_요약_261030.md를 작성해줘. 어제 안 한 것 같아."**
- 10-30 요약: 10-30 07:00 ~ 10-31 07:00에는 세션 요청도, main과 vtalib commit도 없었다. 자동 실행도 요청 0건이라 건너뛰었다(`~/.cache/daily_report.log`). 이 사실과 앞뒤 요약 링크만 넣었고 commit하지 않았다.
- 시간 제한과 dump: EVTA 대기가 `EVTA_RUN_TIMEOUT_MS`(기본 5000 ms)를 넘으면 4개 EVTA의 제어 레지스터, done 상태, run 번호, 명령어 파일 `/tmp/evta_hang_<pid>_<addr>.insn`을 남기고 exit 5로 끝난다. 1 ms 제한으로 출력을 확인했다.

죽은 run 뒤 bitstream load 실패를 고친 순서다. 고칠 때마다 원인 판단이 바뀌어 네 단계를 거쳤다.

| # | 그때 본 원인 | 수정 | 결과 |
|---|---|---|---|
| 1 | CMA가 아직 다 돌아오지 않은 때 load (12:47) | 실패하면 5초 뒤, 10초 뒤 재시도. `run_all.sh`, `run_prof.sh`, `load_bit.sh`, `collect_hw_bundles.py` (d5255635) | timeout 뒤 CMA가 약 60초 동안 돌아오지 않아 부족 |
| 2 | 반환이 약 60초 걸림 (18:18) | 5초 간격으로 120초까지 재시도, `timeout -k 10` (5e6e071) | 이후 원인을 다시 봄 |
| 3 | core dump로 보고 abort 대신 `_exit` | `_exit`로 종료 | 이것만으로는 해결 안 됨(`_exit`는 그대로 둠) |
| 4 | 비정상 종료한 프로세스의 CMA buffer가 `xlnk_reset` 때까지 남음 | load가 처음 실패하면 `xlnk_reset` 후 재시도 | 5초 안에 복구, 다음 실행 파일도 정상 |

**18:34~18:48 [PC] multi4 멈춤 추적: 멈추는 호출을 `stage3_conv0`로 좁힘 (백그라운드 결과)**

같은 실행 파일을 멈출 때까지 반복해 멈춘 순간의 dump를 잡았다. 먼저 systolic 400 MHz에서 copy당 25회씩, 다음으로 RTL `gemm_sa` 문제인지 보려고 HLS 380 MHz에서 copy당 100회씩 돌렸다.

| 비트스트림 | 멈춘 반복 | 멈춘 EVTA | run 번호 | 명령어 수 | 나머지 EVTA |
|---|---|---|---|---|---|
| systolic 400 MHz | 21번째 | EVTA2 | 223 = 21×10+13 | 146 | run 231에서 끝내고 barrier 대기 |
| systolic 400 MHz | 3번째 | EVTA1 | 391 = 21×18+13 | 146 | barrier 대기 |
| HLS 380 MHz | 16번째 (copy당 약 1600회) | EVTA1 | 181 = 21×8+13 | 131 | barrier 대기 |

- 멈추는 곳은 매번 Resnet18의 13번째 EVTA 호출 `stage3_conv0`다(28×28×128 → 14×14×256, 3×3 stride 2, 출력 14×14를 tile 하나로, virtual thread 2개).
- 빈도: systolic은 copy당 약 300~500회 추론에 한 번, HLS는 약 1600회에 한 번이다. EVTA 하나만 쓰는 single exe는 수천 회 동안 한 번도 멈추지 않았다.
- 멈춘 상태: fetch는 명령어를 모두 내보냈고(`0x06`), load/compute/store는 auto-restart 대기, compute가 FINISH를 쓰지 못했다(done_o 0).

단계마다 원인 후보를 하나씩 뺐다. 처음에는 systolic RTL과 cache flush를 의심했다.

| 후보 | 확인 방법 | 결과 |
|---|---|---|
| 명령어 자체 | 의존성 token 정적 시뮬레이션, 이전 run들과 비교 | 끝까지 진행되고, 같은 명령어가 수백 번 정상 → 배제 |
| cache flush 누락 | 멈춘 순간 DRAM과 CPU cache의 명령어·uop 비교 | 0 bytes 다름 → 이때는 배제 (11-02 00:40에 "5초 뒤에 본 값"이라 다시 후보) |
| systolic RTL | HLS 380 MHz로 반복 | HLS도 멈춤 → systolic만의 문제 아님 |
| FIFO가 가득 찬 deadlock | 의존성 FIFO 깊이 1024, 명령어 FIFO 512 | 명령어 146개라 찰 수 없음 → 배제 |

- 후보 제시: 1) 출력 buffer에 표식을 채워 STORE 진행 확인(vtalib만, 몇 시간), 2) schedule을 바꿔 우회(반나절), 3) ILA(하루 이상). 1번을 권했다.

**21:06 [PC] "1번을 해보자."**
- `EVTA_STORE_SENTINEL=1`: run마다 시작 전에 그 run의 STORE 영역을 `0xA5`로 채운다(`/dev/mem`, cache를 거치지 않음). 시간 제한에 걸리면 EVTA마다 STORE 진행을 출력한다. 1 ms 제한 시험에서 출력이 정상이었다(`393 STOREs, 393 written ...`). 켰을 때 100회 추론은 1180 ms다(전에는 약 1100~1130 ms).
- 중요: **표식을 켜면 멈춤이 사라져서, 원래 보려던 STORE 진행은 보지 못했다.** 같은 빌드로 표식을 끈 대조에서 바로 멈춰 표식이 멈춤을 가린다는 것을 확인했다. 대조의 모양은 이전과 같다(EVTA2, run 34 = 두 번째 추론의 13번째 호출, 명령어 146개, 나머지는 run 42까지 완료, DRAM·cache 0 바이트 다름).
- 표식의 효과를 나누려고 `EVTA_STORE_SENTINEL=2`(mmap/munmap만)와 `EVTA_PRE_RUN_US=<n>`(대기만)을 넣었다. 먼저 대기만 시험했다.

같은 빌드, systolic 400 MHz, multi4 Resnet18, copy당 100회씩 반복한 결과다.

| 조건 | copy당 추론 횟수 | 멈춤 | 100회 반복 시간 (single exe) |
|---|---|---|---|
| 아무것도 안 함 (대조) | 약 400회 | **4번째 반복에서 멈춤** (EVTA2, run 34 = 두 번째 추론의 13번째 호출) | 4330 ms |
| 표식 켬 (`EVTA_STORE_SENTINEL=1`) | 2000회 | 0번 | 4640 ms |
| run 앞에 40 µs 대기만 (`EVTA_PRE_RUN_US=40`) | 2000회 | **0번** | 4410 ms |

- 전에는 300~500회에 한 번 멈췄으니, 2000회 동안 0번이 우연일 확률은 1% 정도다.
- 원인은 DRAM에 무엇을 쓰느냐가 아니라 run을 시작하는 타이밍으로 본다. 앞 run의 done 직후 다음 run을 시작하면 하드웨어가 앞 run을 다 끝내지 않은 상태일 수 있다는 가설이다. 왜 늘 13번째 호출인지는 이때 설명하지 못했다.
- 진단 기능은 모두 기본 꺼짐이고, commit·push했다.

**11-02 00:17 [PC] "1, 2번 차례로 진행하자." (대기 하한 찾기, EVTA0에만 대기)**
- 대기를 특정 EVTA에만 넣는 `EVTA_PRE_RUN_MASK`를 추가했다. 실험당 copy당 최대 2000회이고, 멈추면 다음 실험으로 넘어간다. 실험 하나를 40분으로 말했는데 실제로는 8분 정도였다(시간 추정 정정).

모든 EVTA에 run 앞 대기를 넣어 하한을 찾은 실험이다.

| run 앞 대기 | copy당 추론 | 멈춤 | 100회 반복 시간 (single exe) |
|---|---|---|---|
| 없음 (대조) | 약 400회 | 1번 | 4330 ms |
| 40 µs | 2000회 | 0번 | 4410 ms |
| 10 µs | 2000회 | 0번 | 4340 ms |
| 5 µs | 2000회 | 0번 | 4336 ms |

대기를 EVTA 일부에만 넣은 실험까지 포함한 결과다.

| 조건 | 결과 |
|---|---|
| 대기 없음 (대조) | 약 400회 안에 멈춤 (EVTA2, 13번째 호출 `stage3_conv0`) |
| 모든 EVTA에 5 µs / 10 µs / 40 µs 대기 | 셋 다 2000회 동안 멈춤 0번 |
| EVTA0에만 40 µs 대기 | 5번째 반복에서 **대기가 없는 EVTA1이 멈춤** (run 1908 = 21×90+18, **18번째 호출 `stage4_conv0`**, 명령어 136개) |

- 중요: **5 µs로 멈춤이 사라지고 비용은 측정 차이 안이다**(4336 대 4330 ms). 하한은 5 µs 아래라서 찾지 못했다.
- 대기를 넣은 EVTA0은 멈추지 않고 대기가 없는 EVTA1이 멈췄으니, 각 EVTA가 자기 연속 run 사이에서 겪는 문제로 보인다. 멈춘 것이 한 번뿐이라 강한 증거는 아니다.
- 중요: 멈춘 13번째와 18번째 호출은 둘 다 **1×1 downsample conv(12번째, 17번째 호출) 바로 다음**의 3×3 stride 2 conv다. 두 호출 사이에 CPU 작업이 거의 없어 바로 다음 run이 시작된다.
- 유력한 가설(미확인): 명령어 buffer는 run마다 같은 물리 주소를 다시 쓴다. flush 직후 fetch를 시작하면 DRAM에 덜 반영된 내용, 즉 앞 run 명령어 일부를 읽어 token이 어긋날 수 있다. DRAM 경합, 5 µs 대기로 사라지는 것과 맞는다. 다른 후보는 EVTA 쪽이 done 직후 아직 완전히 쉬지 않은 상태라는 것이다.

원인을 가를 다음 실험(둘 다 10분 정도)으로 제안한 것이다. 아직 돌리지 않았다.

| 실험 | 멈춤이 사라지면 | 계속 멈추면 |
|---|---|---|
| 같은 5~10 µs 대기를 cache flush **앞**에 넣기 (지금은 flush 뒤, 시작 직전) | EVTA 쪽 settle 문제 | flush 반영 문제 |
| 명령어 buffer를 run마다 두 개 번갈아 쓰기 (대기 없음) | 같은 주소 재사용(stale read)이 원인으로 확정되고, 그대로 해결책이 됨 | 다른 원인 |

**11-02 00:32 [PC] "fc를 CPU에서 벡터로 처리하게 해서 시간을 많이 줄였는데 이걸 evta에 넣으면 얼마나 더 줄까?"**
- fc를 EVTA에서 돌린 값이 아니라 측정값에 근거한 추정으로 0.77 ms → 0.3~0.45 ms, 추론당 약 1%로 답하고 "노력에 비해 이득이 작다"고 판단했다.

당시 시간 구성이다. single, OMP 4, `_systimed`, 11-01 측정값이다.

| 항목 | 시간 |
|---|---|
| 전체 | 38.68 ms |
| EVTA 호출 21개 (CPU 몫 포함) | 33.36 ms |
| CPU fc | **0.77 ms** |
| 나머지 CPU (avgpool, conv0 0 채움, 입출력 변환, softmax 등) | 약 4.5 ms |

**11-02 00:43 [PC] "...CPU에서 도는 함수를 최소화하는 게 중요할 것 같아. fc를 GEMM에서 돌려보는 것도 좋은 실험일 것 같고. fc 를 evta로 보내보자." / 00:57 "모두 vtalib에서만 바꾸면 되는 runtime 부분 수정만 하면 되는 거였어?... backend 에 속한 거지?"**
- 사용자 결정의 이유: Kria260, ZU1/2나 CPU 없는 Spartan + MicroBlaze에서는 CPU에서 도는 함수를 줄이는 것이 중요하다.
- 구조: vtalib `fullyconnected_evta`, `VTASave.cpp`의 `-vta-evta-fc`(기본 켬). weight를 4덩어리로 나눠 한 덩어리를 읽는 동안 다른 덩어리를 GEMM한다. 반올림은 `std::round`와 같게 ALU 연산만으로 맞췄다.
- 중요: 결과가 3배가 되는 버그를 고쳤다. 부호를 구하려고 쓴 `SHR 31`이 하드웨어에서 왼쪽 1 밀기로 동작했다. ALU shift 인자가 5비트 부호 있는 수(-16..15)인 것으로 보인다. `MIN 0` → `MAX -1`로 바꿨다.
- 실제 fc 0.154 ms는 추정(0.3~0.45 ms)보다 빨랐다. 입력 복사, 명령어 생성, EVTA 실행, 출력 복사를 모두 포함한 값이다.
- offset이 있거나, scale 비가 2의 거듭제곱이 아니거나, batch가 1보다 크면 CPU fc를 쓴다. Resnet50 2048→1000은 weight 한 덩어리에 출력 블록이 4개만 들어가 같은 acc 주소가 너무 가깝게 다시 오므로 CPU로 간다.
- 질문의 답: 바꾼 것은 **backend의 마지막 단계(bundle 코드 생성)와 vtalib(runtime)** 둘뿐이고, 앞쪽 컴파일 단계는 손대지 않았다.

| 단계 | 위치 | 이번 작업 |
|---|---|---|
| 모델 import, graph 최적화, 양자화, lowering, IRGen | `lib/Graph`, `lib/Optimizer`, `lib/Quantization` 등 | 바꾼 것 없음 (빌드 옵션 `VTA_RESNET18_WITH_SKIPQUANT0` 기본값만 바꿈) |
| **backend 코드 생성** (bundle 생성) | `lib/Backends/VTA/VTASave.cpp`, `VTASchedules.h` | layout 유지, conv+add+relu 합치기, maxpool EVTA, fc EVTA, flush/invalidate 호출, conv0 schedule |
| runtime | `vta/vtalib` | 위 기능들의 실제 구현, buffer pool, 16바이트 transpose, NEON fc, 멈춤 진단 |

- 약한 CPU 쪽에서 본 한계: 런타임 준비 작업이 CPU에 남고(fc weight 변환), 앞 단계가 이 결정을 모른다. backend 전용 노드와 `transformPostLowering`으로 옮기면 비용 모델과 분할에도 반영되지만 작업이 크다.

**11-02 01:13 [PC] "multi4 bundle 재생성과 golden 테스트, 시간 테스트 먼저 하고 1번, fc weight 변환을 compile time으로 보내는 걸 하자."**
- multi4 copy 4개를 다시 만들었고, 각자 자기 EVTA 번호로 fc를 EVTA에서 부른다.
- golden: single 30, multi2 3, multi4 3 모두 PASS다. Resnet18, Resnet18Partition, Resnet50은 281, VGG16은 airplane(0), multi2와 multi4는 모든 추론이 281이다.
- 실행 중 CPU 결과와 직접 비교해도 0개 차이다(Resnet18 512→1000, VGG16 512→512 fc).
- compile time 변환: `VTASave.cpp`가 weight 516,096 B, bias 4,032 B(Resnet18)를 EVTA layout으로 weights 파일에 쓰고, bundle은 `fullyconnected_evta_packed`를 부른다. CPU는 입력 512 B와 출력 1000 B만 복사한다. 첫 호출 때의 변환 루프가 없어졌다. Resnet18에서 0/1000 differ, Result 281, confidence 0.655530이다.
- 조건이 맞지 않는 fc는 compile time에 CPU fc로 간다(Resnet50 2048→1000, VGG16 512→10).

fc의 CPU NEON판과 EVTA판 두 가지를 비교했다. systolic 400 MHz, `_systimed`, 25회 중앙값이다. 매 추론의 시간은 두 EVTA판이 같고, 줄어든 것은 첫 실행 때의 CPU 일과 런타임 변환 코드다.

| | CPU NEON fc | EVTA fc, 런타임 변환판 | EVTA fc, 01:44 보고 (fc 0.152 ms = compile time 변환판) |
|---|---|---|---|
| fc 한 번 | 0.74 ms (OMP 1) / 0.77 ms (OMP 4) | 0.154 ms | **0.152 ms** |
| Resnet18 single, OMP 1 | 40.56 ms | 39.95 ms | **39.92 ms** |
| Resnet18 single, OMP 4 | 38.68 ms | 37.98 ms | **37.99 ms** |
| multi4 copy당 (4개 동시) | 41.2 ms | 40.59 ms | **40.56 ms** |

- 문서 `docs/evta_runtime_compare.md` §7.7(멈춤), §7.8(EVTA fc)과 `CLAUDE.md`를 고쳤다. 보드 collect가 만들던 `run_all.sh` 차이도 git에 반영해 보드 작업 트리가 깨끗하다. 모두 commit·push했다.

**11-02 01:17 [PC] "layer 사이에서 store로 다시 DRAM으로 쓰는 건 몇 번이나 있어? ...output buffer에서 다시 input buffer로 간다든지, GEMM으로 보낸다든지..."**
- EVTA 호출은 추론당 22개이고(conv와 합친 conv 19, maxpool 1, conv0 1, fc 1) 22번 모두 출력을 DRAM에 STORE한다. 그중 20곳은 다음 단계도 EVTA다. CPU가 끼는 곳은 conv0 앞 전처리와 fc 앞 avgpool 두 군데다. 합친 conv는 residual도 DRAM에서 다시 읽는다.
- 중요: 시간을 먹는 것은 DRAM 왕복이 아니라 **호출 사이의 CPU 일**이다. activation은 추론당 2 MB 안팎으로, 4 GB/s로 잡아도 0.5 ms이고 대부분 계산과 겹친다.

Resnet18 시간 구성이다. `_systimed`, OMP 4, 전체 37.98 ms다. EVTA 계산 시간은 `_sysprof` 합이다.

| 항목 | 시간 |
|---|---|
| EVTA 호출 22개의 wall time | 35.04 ms |
| 그중 EVTA가 실제로 계산한 시간 (`_sysprof` 합) | 약 32.0 ms |
| **호출 안의 CPU 몫** (명령·uop 생성, flush, 시작, 완료 대기; EVTA는 그동안 쉼) | **약 3.0 ms (8 %)** |
| 입력 전처리 (elemdiv 0.94 + elemsub 0.67 + quantize 0.58) | **2.2 ms** |
| avgpool, conv0 0 채움, invalidate 등 | 약 0.7 ms |

- 출력 → 입력 buffer 직행은 지금 HW로 안 된다. inp에 쓰는 것은 LOAD뿐이고, MOVE(5)는 2비트 `memory_type`으로 표현할 수 없다. 용량도 모자란다(single inp 64 KB, stage1 200 KB, stage2 100 KB는 안 들어가고 stage3 50 KB, stage4 25 KB만 들어감). HW 수정과 층 융합 scheduling이 둘 다 필요한데, 줄어드는 것은 주로 대역폭이다.
- HW 수정 없이 줄이는 방법: 1) AOT와 호출 묶기(`docs/evta_runtime_aot.md`, 0단계 record/replay 3~5일, 최대 약 3 ms), 2) 입력 전처리를 NEON 루프 하나로(약 0.2 ms 예상, 추정). 정규화를 conv0 weight와 bias에 접는 방법은 padding 경계 문제를 따져 봐야 한다.

## 결정이 남은 것 (11-02 07:00 기준)
- 사용자 답을 기다리는 것:
  - multi4 멈춤 우회책(run 앞 5 µs 대기)을 기본으로 켤지. 지금은 기본 꺼짐이라 기본 설정의 multi4는 여전히 멈출 수 있다. 앞서 제안한 값은 "앞 run의 done 뒤 최소 10 µs"다.
  - 원인을 가르는 실험 2개(대기를 flush 앞으로 옮기기, 명령어 buffer 두 개 번갈아 쓰기)를 돌릴지, 우회책과 어느 쪽을 먼저 할지.
  - 다음 작업: 권고는 입력 전처리 합치기(2.2 ms) → AOT 0단계(약 3 ms)이고, out→inp HW 경로는 그 뒤에 따진다.
- 이미 결정·완료된 것:
  - buffer pool(A안), fc NEON, fc EVTA(`-vta-evta-fc` 기본 켬), fc weight compile time 변환을 넣었다.
  - multi4 멈춤 진단 옵션(`EVTA_STORE_SENTINEL`, `EVTA_PRE_RUN_US`, `EVTA_PRE_RUN_MASK`)은 기본 꺼짐으로 두고, `EVTA_RUN_TIMEOUT_MS`(기본 5초)는 넣었다.
- 확인되지 않은 것: 멈춤의 근본 원인, 왜 downsample 다음 호출인지(가설만 있음), OMP 4에서 fc가 빨라지지 않는 이유, pool과 멈춤의 관련.
