# 2026-11-02 EVTA systolic 요약: multi4 멈춤 원인은 libcma flush 뒤 `dsb` 누락(수정 후 copy당 4000회 무멈춤), Resnet18 single OMP 1 39.92 → 28.83 ms(전처리 합치기·replay·conv0 pack·conv0+pool fusion), EVTA 시간의 17.5%가 ALU II=2 → imm ALU 루프 II=1 HLS 수정

- 기간: 2026-11-02 07:00 ~ 11-03 07:00 (KST)
- [PC]는 PC에서 `main`을 고치고 보드 실험을 ssh로 돌린 세션이다(commit·push 뒤 보드 `main`과 `vta/vtalib`에서 pull). [보고서]는 `docs/dailyreport/vta_synth/` 일일 보고서를 만든 세션이다.

## 시간순

**10:19 [PC] "3. run 앞 5us 대기를 기본값으로 일단 두자. 실험 두 가지도 해보자. 원인을 찾아보자. 1. 입력 전처리 합치기는 해보자. … 2. AOT 0단계는 마지막에" / 10:20 "DRAM에 쓰고 다시 읽어서 버퍼 채우는 걸 없애는 것도 그 다음으로" / 10:21 "2번 AOT 0단계까지 다 끝나고 마지막에"**
- 순서가 정해졌다: 멈춤 원인 → 입력 전처리 합치기 → AOT 0단계 → 마지막에 층 사이 DRAM 왕복 없애기.

**10:24 [PC] "Resnet18 외에 다른 모델들에서도 거의 비슷하게 적용이 될까? … 일단 가볍게 research를 해 봐."**
- 보드 측정이 아니라 지금 EVTA 능력(int8 16×16 GEMM, ALU MIN/MAX/ADD/SHR만, 2의 거듭제곱 scale, MOVE·LUT 없음)에 맞춘 책상 분석이다.

  | Family | 주요 op | CPU에 남는 것 | 예상 |
  |---|---|---|---|
  | ResNet 18/34/50/101 | conv(+BN), relu, add, maxpool, global avgpool, fc | avgpool(÷49, MUL 없음); Resnet50 fc는 조건 하나가 안 맞음 | 거의 다 EVTA. avgpool은 fc weight에 compile time에 접을 수 있음 |
  | VGG | conv, relu, maxpool, fc | 거의 없음(큰 fc는 DRAM 대역에 묶임) | 거의 다 EVTA |
  | MobileNet v1/v2, MnasNet | depthwise, relu6, 1×1 conv, add | depthwise(16×16 GEMM의 1/16만 사용, 채널별 곱 없음) | 1×1은 맞지만 depthwise 비중이 큼. relu6은 MIN/MAX |
  | EfficientNet, MobileNet v3 | depthwise, swish/h-swish, SE block | swish, sigmoid, SE 곱 | MUL과 LUT 없이는 많은 부분이 못 감 |
  | YOLO (tiny v3/v4, v5/v8) | conv, leaky/SiLU, upsample, concat, NMS | leaky 0.1(SHR 3 = 0.125는 결과가 다름), SiLU, NMS | conv와 concat은 감(N{C/16}HW16에서 concat은 주소 배치). 활성화·후처리는 CPU |
  | Transformer (BERT, ViT) | matmul, softmax, layernorm, GELU | softmax/layernorm/GELU; attention matmul은 activation을 weight로 LOAD해야 함 | GEMM만 감 |
- 결론: ResNet·VGG 같은 conv 위주 분류망은 지금 방향 그대로 된다. mobile·detection 모델은 ALU MUL(3비트 opcode, HW 변경), LUT 활성화 장치, depthwise 모드가 필요하다. 실험(ResNet50·VGG, 이어서 MobileNetV2·YOLO-tiny의 CPU 몫 측정)은 지금 작업 뒤로 미뤘다.

**11:03 [PC] multi4 멈춤 원인 발견, 입력 전처리 합치기**
- 중요: **원인은 PYNQ `libcma.so`의 `cma_flush_cache`가 끝에 `dsb`를 하지 않는 것**이다(디스어셈블로 확인). EVTA 시작 레지스터 쓰기가 명령어의 DRAM 도착보다 먼저 나갈 수 있고, EVTA 4개가 DRAM을 같이 쓸 때 덜 쓰인 명령어를 읽고 멈췄다.
- 실험: systolic multi4 Resnet18을 100회씩 반복하다 멈추면 중단.

  | run 앞에서 하는 일 | 결과 |
  |---|---|
  | 아무것도 안 함 (대조) | 7번째 100회에서 멈춤 |
  | 5 µs 대기를 flush **앞**에 | 2번째 100회에서 멈춤 |
  | 명령어 버퍼 두 개 번갈아 쓰기, 대기 없음 | 4번째 100회에서 멈춤 |
  | **flush 뒤 `dsb sy`, 대기 없음** | **copy당 4000회, 멈춤 0** |

  멈추게 한 것은 flush 뒤의 시간이지, 앞 run이 끝난 뒤의 시간이나 같은 주소 재사용이 아니었다. 수정: vtalib `VTAFlushCache`/`VTAInvalidateCache`에서 libcma 호출 뒤 `dsb sy`(`EVTA_NO_DSB=1`로 끔). `EVTA_PRE_RUN_US`는 요청대로 기본 5로 두었다.
- **입력 전처리 합치기:** 컴파일러가 conv0 앞 전처리를 찾아 conv0 입력을 EVTA layout으로 쓰는 복사 한 번에 합친다. Resnet18은 quantize → 평균 빼기 → 표준편차 나누기(채널마다 256칸 표를 compile time에 weights에 넣음), Resnet50·VGG16은 quantize → NCHW→NHWC. NEON 16 pixel씩. 보드 단위 시험(0.5 경계, 포화, 2의 거듭제곱 아닌 scale, 폭 224/37, NHWC/NCHW)에서 차이 0, golden 모두 PASS, confidence 같음(0.655530 / 0.530282 / 0.581379 / 0.453709).
- 측정은 systolic 400 MHz bitstream에서 했다.

  | (systolic 400 MHz) | 이전 | 지금 |
  |---|---|---|
  | 입력 전처리 | 2.2 + 0.2 ms | **0.37 ms** |
  | Resnet18 single, OMP 1 | 39.92 ms | **37.08 ms** |
  | Resnet18 single, OMP 4 | 37.99 ms | **36.04 ms** |
  | multi4 copy당 (4개 동시) | 40.56 ms | **37.59 ms** (494.8 GOPS) |
  | multi4 100회 | 1115.8 ms | **1060.5 ms** |
- HW ALU로 옮길 때: 평균 빼기는 2의 거듭제곱 scale이면 ADD/SHR로 되고, 채널별 나누기는 MUL이 없어 표를 두거나 conv0 weight에 접어야 한다(접으면 경계 padding 때문에 결과가 조금 다름). 문서 `docs/evta_runtime_compare.md` §7.7, §7.9.

**12:12 [PC] "고쳐졌으면 기본값을 0으로 내려야지. AOT 0단계는 어디를 얼마나 고쳐야 해?"**
- 5 µs 대기 기본값을 0으로 내렸다(보드 `94ab8dd`). 새 기본값으로 보드 실행 파일을 다시 빌드해 돌려 보지는 않았다.
- AOT 0단계 계획: 호출마다 처음 한 번 만든 명령과 uop를 저장해 두고, 같은 호출이면 바로 실행. 예상 이득 약 2~3 ms(6~8%, 추정). 고칠 곳은 `cmd_queue.h`(150~200줄), `dat_queue.h`(50줄), `VTABundle.cpp` 진입 함수 4개(40줄), systolic 사본 복사, 컴파일러는 0. 합쳐 300줄 안팎, 보드 검증까지 2~3일로 봤다.

**12:28 [PC] "원래 TVM은 … uop를 runtime에서 안 하고 컴파일러에서 하는 거였잖아. … 최종적으로 컴파일러로 가는 데 문제 없겠지? 일단 이 방식으로 고치자."**
- 중요: 정정. **TVM도 uop와 명령을 실행 중에 만든다(JIT).** TVM 컴파일러가 만드는 것은 그 일을 하는 C 코드이고, NEST-C는 그것을 vtalib의 손으로 짠 `conv_kernel`로 둔 것이다. **양이 많아서 runtime에서 만드는 것도 아니다:** Resnet18 전체가 명령 8,096개(259 KB), uop 포함 **653 KB**(weights 11 MB). 기록 하나가 1단계 blob(명령 + uop + relocation 표)과 같은 모양이라 컴파일러로 가는 데 문제는 없다.
- 구현(vtalib 두 벌): 진입 함수 4곳(conv, conv+residual, maxpool, fc)이 스칼라 인자 전부를 키로 쓰고, 버퍼 주소가 다르면 relocation 표대로 고친다. 같은 모양 conv는 기록을 공유한다(Resnet18 호출 22개에 기록 20개). 기록 시작·재생 끝에 uop SRAM을 비었다고 표시한다. `vta/vtalib`의 uop cache를 10-27 systolic 사본 수정판으로 맞췄다. 옵션 `EVTA_REPLAY=0`, `EVTA_REPLAY_LOG=1`, `EVTA_REPLAY_MB`(기본 16).

  | (systolic 400 MHz) | 이전 | replay |
  |---|---|---|
  | Resnet18 single, OMP 1 | 37.08 ms | **34.50 ms** |
  | Resnet18 single, OMP 4 | 36.04 ms | **33.48 ms** |
  | multi4 copy당 (4개 동시) | 37.59 ms (494.8 GOPS) | **34.99 ms (531.6 GOPS)** |
  | multi4 100회 | 1060.5 ms | **977.9 ms** |

  single 100회는 3851 → 3620.5 ms. replay를 끈 대조는 이전과 같았다(single 37.01 ms, multi4 37.55 ms). golden single 30, multi2 3, multi4 3 PASS, multi4 copy당 4000회 멈춤 0. 문서 `docs/evta_runtime_aot.md` §5.4, `evta_runtime_compare.md` §7.10.

**14:01 [PC] "DRAM 없애기는 어떤 일들이 필요하고 얼마나 될까?"**
- 추정(새 측정 아님): Resnet18 activation 왕복은 약 6 MB, 4 GB/s로 약 1.5 ms이고 DAE로 대부분 이미 가려져 있다.

  | 구간 | 읽기 | 쓰기 |
  |---|---|---|
  | conv0 → maxpool | 0.8 MB(maxpool 입력) | 0.8 + 0.2 MB |
  | stage1 (56×56×64, 200 KB/층) | 약 1.0 MB | 0.8 MB |
  | stage2 (100 KB/층) | 약 0.9 MB | 0.5 MB |
  | stage3, stage4 (50 KB, 25 KB/층) | 약 0.7 MB | 0.4 MB |
- A. conv0 + maxpool은 HW 변경 없이 2~3일, 순이득 0.5~1 ms. B. conv→conv는 ISA·inp BRAM 쓰기 포트·토큰 큐 추가 등 HW 1~2주 + SW 1~2주, inp 64 KB에 stage1/2가 안 들어가 tile fusion과 halo 재계산이 필요, 이득 1~1.5 ms. 먼저 반나절 측정을 제안했다.

**14:08 [PC] "측정 1번부터 해보자."**
- 방법: 지금 systolic bitstream에는 카운터가 없어 replay 저장본을 이용했다. `EVTA_REPLAY_LOG=2`(run마다 GEMM/ALU 반복, DRAM 바이트, 실측 시간), `EVTA_MEMEXP=<mask>`(저장본 LOAD/STORE를 원소 1개로 줄여 상한을 봄, 결과는 쓰레기).
- Resnet18 single, systolic 400 MHz, 25회 중 2~25회 median:

  | 경우 | EVTA 시간 | 줄어든 시간 |
  |---|---|---|
  | 그대로 | **32.18 ms** | |
  | GEMM+ALU 이론 하한 | 25.92 ms | |
  | activation 전송 없음(inp+acc+STORE) | 29.82 ms | −2.36 ms |
  | weight LOAD 없음 | 31.39 ms | −0.79 ms |
  | DRAM 전송 전부 없음 | 29.13 ms | −3.05 ms |

  activation −2.36 ms 내역: conv0 1.13 ms(0.77은 네트워크 입력 읽기라 남음, 0.36은 출력 쓰기), maxpool 0.18 ms, **나머지 conv 19개 합 1.05 ms**(conv당 0.03~0.11 ms).
- 중요: **conv→conv 잇기는 하지 않는 게 낫다**(몇 주 작업에 최대 1.05 ms). conv0+maxpool fusion은 할 만하다. 더 큰 곳은 conv0의 0 채움 낭비(GEMM 6.27 ms 중 약 5 ms)다.
- 하다가 고친 것: fc가 replay되지 않았다(bundle은 `fullyconnected_evta_packed`를 부르는데 replay는 `fullyconnected_evta`에만 있었음). 수정 후 fc 0.15 → 0.14 ms, single OMP 1 34.48 ms, multi4 copy당 34.99 ms. 문서 §7.11.

**15:11 [PC] "conv0 0 채움 개선을 먼저하고 나서 conv0 + maxpool fusion을 해도 문제가 없어? … 어떤 순서가 그 다음 걸 하는 데 더 문제가 없을까"**
- 0 채움 개선이 먼저다. fusion은 conv0 tile 모양·halo에 묶이는데 0 채움 개선이 tile을 바꾼다. conv0 출력이 DRAM에 남아 있을 때 bit 비교가 쉽다. 바뀌는 쪽이 입력/출력으로 갈려 겹치는 것은 schedule 하나다.
- 미리 지킬 것: 출력 tile 높이·너비 짝수이고 112를 나눔, 일반 `convolution_wo_tr` 호출 형태 유지, `-vta-...` 옵션으로 켜고 끄기.

**15:15 [PC] "0 채움 개선부터 시작하자." / 15:17 "0 채움 원래 방식의 함수도 남겨두자."**
- 방식: stride 2의 입력 행 2개와 kw 7개를 채널로 넣어 2×7×3 = 42 → 48채널, 입력 115×112×48, conv0이 **4×1, stride 1, pad 0** conv(GEMM 반복 49 → 12). 일반화: KH×KW / stride s → ceil(KH/s)×1(Resnet18, Resnet50, VGG16 cifar 3×3 → 3×1). 원래 0 채움 경로는 남겼다(`-vta-conv0-pack=false` 또는 schedule이 안 맞는 모양). 출력 tile 16×16.
- 검증: 새 `EVTA_DUMP_CONV=<dir>`로 이전/새 bundle의 conv 출력 210개(Resnet18 40, Resnet50 106, VGG16 26 포함) 모두 같음. golden single 30, multi2 3, multi4 3 PASS.

  | (systolic 400 MHz) | 이전 | conv0 pack |
  |---|---|---|
  | conv0 EVTA | 9.13 ms | **3.64 ms** |
  | single, OMP 1 | 34.48 ms | **29.81 ms** |
  | single, OMP 4 | 33.48 ms | **28.16 ms** |
  | multi4 copy당 | 34.99 ms | **30.49 ms** |
  | multi4 100회 | 977.9 ms | **910.9 ms** (다른 한 번은 895.4) |

  출력 GOPS는 packed 모양으로 세어 이전과 비교할 수 없다. CPU 입력 복사가 0 채움 때보다 느리다(OMP 4에서 0.36 → 0.58 ms, OMP 1 약 +0.9 ms).
- 진행 중 다른 사용자가 "06:38쯤" `sudo ./run_prof.sh -l -s _systimed MultiEVTAResnet18Test`를 돌려 스크립트를 멈췄다가 이어 했다. 그 측정은 새 conv0 경로로 돌았을 수 있다. 문서 §7.12.

**16:08 [PC] "어떤 순서가 더 검증이 쉬워? 효율적이야?" / 16:09 "좋아. 입력 복사부터 시작하자."**
- 입력 복사 개선(CPU 함수 하나, 기존 도구로 검증, 반나절)을 먼저 하고 fusion을 하기로 했다. 효과는 비슷하다(입력 복사 OMP 1 약 0.9 ms, OMP 4 약 0.2 ms / fusion 0.5~1 ms, 추정).
- 바꾼 것: packed 행 q가 pad 입력 행 2q, 2q+1만 읽으므로 스레드마다 두 행(약 1.4 KB)만 quantize하고 바로 pack한다. 중요: **실제로 시간을 줄인 것은 aliasing 수정**이었다. byte store마다 차원 값·표 index를 다시 읽던 것을 지역 변수로 옮겨 pack 0.88 → 0.35 ms. pad 이미지를 없앤 것만으로는 거의 줄지 않았다.
- 보드 단독 측정(Resnet18 conv0 입력, OMP 1). 남은 약 1.35 ms는 두 경로 공통의 quantize 계산이다.

  | 방식 | 시간 |
  |---|---|
  | 0 채움 (예전 경로) | 1.55 ms |
  | pad 이미지 + pack (이전 버전) | 2.18 ms |
  | 행 단위 (이번) | **1.69 ms** |
- 검증: 6가지 모양(7×7/2, 3×3/1, 5×3/2, 11×11/2 범용, NCHW/NHWC) 불일치 0, 덤프 비교(Resnet18 40, Resnet50 106, VGG16 26) 같음, golden PASS.

  | (systolic 400 MHz) | conv0 pack 직후 | 행 단위 |
  |---|---|---|
  | 입력 복사 (OMP 4) | 0.58 ms | **0.44 ms** |
  | single, OMP 1 | 29.81 ms | **29.19 ms** |
  | single, OMP 4 | 28.16 ms | **28.02 ms** |
  | multi4 copy당 | 30.49 ms | **29.76 ms** |
  | multi4 100회 | 910.9 ms | **827.3 ms** |

  multi4 100회는 같은 조건에서도 895~911 ms로 흔들렸던 값이라 827.3 ms의 폭은 실제보다 크게 나왔을 수 있다.

**16:24 [PC] "conv0 + maxpool fusion 시작하자."**
- 방식: conv0 출력 tile(16×16)의 위쪽 1행·왼쪽 1열을 더 계산(halo, 이미지 경계는 pad라 생략; GEMM 1.63 → 1.84 ms), relu 뒤 store 전에 acc 안에서 ALU MAX, pool 결과만 store(784 KB 쓰기와 maxpool 호출이 없어짐). 컴파일러 `-vta-fuse-pool`(기본 켜짐), 모양이 안 맞으면 기존 경로.
- 검증: `EVTA_DUMP_CONV`가 maxpool 출력도 쓴다. fusion 전 maxpool 출력과 그 뒤 모든 EVTA 출력이 byte 단위로 같음(Resnet18 40, Resnet50 106, HLS·systolic 빌드 모두). `EVTA_RES_CHECK=1`로 200,704개 중 불일치 0. golden PASS.

  | (systolic 400 MHz) | 따로 | fusion 1판 | fusion (최종) |
  |---|---|---|---|
  | conv0 + maxpool EVTA | 3.64 + 1.19 = 4.83 ms | 4.80 ms | **4.48 ms** |
  | single, OMP 1 | 29.19 ms | 29.25 ms | **28.83 ms** |
  | single, OMP 4 | 28.02 ms | 28.02 ms | **27.71 ms** |
  | multi4 copy당 | 29.76 ms | 29.70 ms | **29.37 ms** |

  1판은 window 위치 9개 × 블록마다 ALU 명령을 내 thread·tile당 19개였고, ALU 명령 하나에 약 130 cycle 고정 비용(추정)이 절약분을 다 먹었다. 최종판은 가로 2번·세로 3번 두 단계로 나눠 명령을 6개로 줄였다. 이득 0.35 ms는 예상(0.5~1 ms)보다 작다(halo GEMM 약 0.2 ms와 남은 ALU 비용).
- 측정 뒤 보드가 잠깐 응답하지 않았다(재부팅 아님, 네트워크로 추정). github 연결 패킷 손실 50%로 pull 실패. 문서 §7.13.

**18:12 [PC] "다시 보드에서 pull 해줘. conv0 + maxpool … 다른 모델들을 컴파일해서 evta에서 돌릴 때 유연성이 떨어지나?"**
- 보드 pull 완료: main `75a801f`, `vta/vtalib` `7ffbe6b`(두 단계판).
- 유연성: 다른 모델이 EVTA에서 돌 범위는 줄지 않는다. `findPoolFusions`는 packed conv0 + relu 바로 뒤 EVTA maxpool 3×3/2/1, 출력을 그 pool만 씀, tile·acc 용량이 맞을 때만 합친다. VGG·Resnet50은 fusion이 걸리지 않았고 출력이 byte 단위로 같았다. 런타임 `convolution_wo_tr_pool`에도 대비 경로가 있고 `-vta-fuse-pool=false`로 끌 수 있다.
- 비용: `pool` 인자가 `conv_kernel`까지 가는 유지보수 부담, 적용 범위가 좁음(2×2/2는 halo가 없어 다음 후보), tiling이 pool에 묶임, conv 출력이 DRAM에 안 남음. 권고: 기본 켜 둠.

**18:16 [PC] "이제 남은 일이 뭐지? CPU에서 하던 걸 evta로 거의 다 보낸 것 같은데."**
- single OMP 1 28.83 ms 중 EVTA 26.47 ms, CPU 몫 약 2.4 ms.

  | 덩어리 | 크기 | 비고 |
  |---|---|---|
  | GEMM 하한 | 약 19.2 ms (추정) | §7.11의 23.60 ms에서 conv0만 6.27 → 1.84 ms로 바꿔 계산 |
  | EVTA 안의 비-GEMM 시간 | 약 7 ms | ALU, run 고정 비용, uop LOAD, 의존성 대기, 일부 DRAM 전송 |
  | CPU와 호출 오버헤드 | 약 2.4 ms | 입력 quantize(OMP 1 약 1.35 ms), avgpool·softmax, flush/invalidate, 호출 |
- 후보(효과 순): 1) EVTA 안 7 ms 분석(권고), 2) 한 inference를 EVTA 4개로 나눠 latency 줄이기, 3) CPU 2.4 ms(uint8 입력이면 표 하나로 quantize), 4) 다른 모델로 넓히기, 5) HW(ALU MUL, loop FSM, 인터럽트, 클럭). 목표가 처리량인지 latency인지 물었다.

**18:19 [PC] "1. EVTA 안의 7ms 분석부터 해보자." / 20:36 "측정이 끝났을 것 같은데."**
- 새 옵션 `EVTA_REPLAY_DUMP=<dir>`(기록된 명령·uop를 텍스트로, `29acc011`).
- 중요: 18:23에 시작했다고 한 측정은 **돌지 않았다.** 보드에 올린 스크립트가 0바이트였는데 시작 메시지만 보고 확인하지 않아 약 2시간이 지나갔다. base64로 다시 올리고 크기(1009바이트)를 확인한 뒤 실행했다.

**20:41 [보고서] "나머지 날짜 중 20~24까지만 만들자."**
- 10-20~10-24 보고서 5개를 `docs/dailyreport/vta_synth/`에 만들었다(10-20만 본문 확인, 나머지는 제목·줄 수만). commit하지 않았다.

**20:48 [PC] EVTA 안 7.6 ms 분석 결과**
- Resnet18 25회, run별 실측(24회 median)과 `EVTA_REPLAY_DUMP`의 명령·uop를 맞춰 봤다.

  | 덩어리 | ms | 비율 |
  |---|---|---|
  | GEMM 반복 ÷ 400 MHz | 18.85 | 71% |
  | **ALU 반복 × 2 cycle** | **4.63** | **17.5%** |
  | 나머지 | 2.99 | 11% |
- 중요: **ALU가 4.6 ms이고 원인은 HLS ALU 루프의 `PIPELINE II = 2`**(`vta.cc`에서 확인, 회귀로도 반복당 1.9 cycle). ALU 내역: 위쪽 clip 0.98, bias ADD(residual 포함) 0.91, SHR 0.90, relu 0.69, pool MAX 0.45, 아래쪽 clip 0.29, 반올림 ADD 0.16 ms.
- 나머지 2.99 ms: conv0 1.08 ms(tile 98개 × 약 11 µs, tile마다 uop 약 1100개 재LOAD, 명령 14개), 3×3 conv run당 60~70 µs(따로 재지 않음), 1×1 downsample 3개는 LOAD가 안 숨음(900 µs 중 GEMM 216 µs).
- 후보: 1) ALU HW(imm만 II=1이면 약 −1.5 ms, 전체 II=1이면 −2.3 ms), 2) conv0 tile 키우기(최대 약 −0.5 ms), 3) run 합치기(먼저 빈 run 측정), 4) downsample conv 합치기(약 −0.3 ms). 문서 §7.14.

**20:58 [PC] "3번의 빈 run 측정을 먼저 하고, 그다음 1번(ALU II=1)는 어떻게 하드웨어를 고칠지 보자."**
- `vtabundles_xcmpl/hwtest/runbench.cpp`로 쟀다(systolic 400 MHz, 200회 median).

  | 측정 | 값 |
  |---|---|
  | 가장 작은 run | **4.7 µs** |
  | GEMM 반복 하나 | 1.0 cycle |
  | GEMM 명령 하나 | 약 25 cycle |
  | ALU 반복 하나 (imm, reg 모두) | **2.0 cycle** |
  | ALU 명령 하나 | 약 35 cycle |
  | 큰 LOAD / STORE | 5.0 / 5.2 GB/s |
- 중요: **run 합치기는 하지 않는다**(21개를 다 합쳐도 0.1 ms). 3×3 conv의 run당 60~70 µs는 conv 안(첫 tile LOAD, 마지막 ALU·STORE)에서 생긴다.
- 측정 중 문제: 측정 프로그램이 CMA를 돌려주지 않아 이후 실행이 할당 실패 → 한 프로세스에서 모두 돌리고 그 전에 CMA reset. uop kernel 256개를 쓰는 경우 하나에서 EVTA가 멈춰(bundle에 없는 형태) 그 경우만 뺐다.
- ALU II=1 설계: ALU 루프는 3중 루프 합침, II=2, 깊이 11. 반복마다 acc를 두 번 읽고(src, dst) 한 번 쓰는데 acc_mem은 2포트다. imm 연산은 src를 안 읽으므로 imm 전용 루프를 II=1로 둔다(`vta-hw-zcu102-systolic/.../vta.cc`, systolic 빌드만). ISA·vtalib·컴파일러는 그대로. Resnet18 ALU 명령 1,566개에서 imm 명령이 16 반복 안에 같은 acc 주소를 다시 쓰는 일은 없었다. 예상 약 −1.5 ms(28.8 → 약 27.3 ms). reg 연산(bias ADD, pool MAX)은 acc 포트 3개가 필요해 범위에서 뺐다.

**22:09 [PC] "1번, HLS 수정과 C simulation 하자."**
- `alu()` 앞에 `use_imm` 명령만 도는 루프를 넣고 연산을 `alu_lane()`으로 뺐다. `make ip CSIM=1 SYSTOLIC=1` "CSim done with 0 errors"(ALU MIN/MAX/ADD/SHR imm·reg, uop 압축 켬/끔, Blocked GEMM 3종). commit `b1755839`.
- 합성 결과(compute):

  | 루프 | II | 깊이 |
  |---|---|---|
  | `ALU_IMM_OUT_LOOP_ALU_IMM_IN_LOOP_ALU_IMM_UOP` (새 imm 루프) | **1** | 7 |
  | `EXE_OUT_LOOP_EXE_IN_LOOP_READ_ALU_UOP` (reg 연산, 기존) | 2 | 9 (전에는 11) |
  | `SA_OUT_LOOP_SA_IN_LOOP_SA_UOP` (GEMM) | 1 | 5 |

  자원은 compute BRAM 37, LUT 31.1K, FF 12.6K(이전 보고서를 덮어써서 비교 못 함). "Unable to satisfy pipeline directive" 경고는 기존 `reset_mem`·`load`/`compute` 코드에서 나온 것이다. HLS 클럭 추정 2.635 ns(목표 2.0 ns보다 김). 400 MHz 달성 여부는 Vivado 구현에서만 확인된다.

**22:18 [PC] "바로 시작하자."**
- 400 MHz bitstream 빌드(`make bin SYSTOLIC=1 FREQ=400 IMPL_STRATEGY=Performance_ExtraTimingOpt`, 다른 `BUILD_TAG`)를 시작하는 요청이다. 기록에 빌드 결과는 없다.
- (11-03 추가) 22:46:43에 PC가 로그 없이 꺼졌다. 그때 Vivado는 `route_design`(Phase 4.1)을 하고 있었다. 10-25와 10-28에도 Vivado가 돌던 중에 같은 방식으로 꺼졌다. 빌드는 11-03에 `JOBS=4`로 CPU 온도를 기록하면서 다시 돌린다. 꺼진 빌드의 디렉터리는 `..._sa_400mhz_alu1_crash1102`로 남겼다.

**22:25 [보고서] "나머지 날짜도 다 만들자."**
- 10-25~10-28 보고서 4개를 만들어 `docs/dailyreport/vta_synth/`에 10-19~10-28이 모두 있다. 10-29~11-01은 이 세션에 요청이 없어 건너뛰었다. 새 보고서 9개와 vta_systolic 보고서 4개(261029~261101)는 commit하지 않았다.

**22:46 [PC] "bias를 GEMM 쪽으로 옮기는 소프트웨어 방법이 가능해? DSP48E 내부에 있는 add를 이용하는 건가? … 외부 logic을 사용하지 않고도 mul, bias add, acc를 할 수 있지?"**
- 기록에 이 질문의 답은 없다.

## 결정이 남은 것 (11-03 07:00 기준)
- 기다리는 것:
  - imm ALU II=1 systolic 400 MHz bitstream 빌드 결과(WNS/WHS/WPWS)와 보드 확인(golden 30, `EVTA_DUMP_CONV` 비교, `runbench aluimm` 1 cycle, Resnet18 시간). 기록에 결과 없음.
  - 22:46 질문(bias를 GEMM/DSP48E 쪽으로 옮기기)에 대한 답.
  - 목표가 처리량인지 한 장 latency인지(한 inference를 EVTA 4개로 나눌지의 우선순위).
  - 보고서 13개(vta_synth 9개, vta_systolic 4개) commit 여부.
  - conv0+maxpool fusion을 기본 켜 둘지(권고는 켜 둠, 사용자 답 없음).
- 이미 결정된 것:
  - 5 µs 대기(`EVTA_PRE_RUN_US`) 기본 0, 멈춤은 `dsb sy`로 해결.
  - conv→conv DRAM 왕복 없애기(HW 변경)와 run 합치기는 하지 않는다.
  - 원래 0 채움 conv0 경로는 남긴다.
  - 다른 모델(ResNet50·VGG, 이어서 MobileNetV2·YOLO-tiny) 실험은 지금 작업 뒤로 미룬다.
