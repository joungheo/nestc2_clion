# KV260에 모델 하나를 올려 공급한다면: 비교 대상, 장점, DeiT-tiny를 고른 이유 (2026-11-08 ~ 11-10)

2026-11-08 밤부터 11-09 새벽까지 검색하고 비교한 내용을 정리했다. 11-09 ~ 11-10의 DeiT-tiny 작업 결과도 함께 넣었다.
그때 대화의 내용은 `docs/dailyreport/vta_systolic/vta_systolic_요약_261108.md`에 시간순으로 있다.

- **값의 출처:** "확인됨" 또는 출처를 적은 값은 그때 검색으로 찾은 것이다. "추정"은 우리 ZCU102 실측에서 환산하거나 기억에 의존한 값이다.
- **우리 실측:** ZCU102, systolic 16×16 EVTA 400 MHz(`..._sa_400mhz_rq.bin`), `run_prof.sh -s _systimed`, 25장 중앙값.
- **KV260 값:** KV260(XCK26 = ZU5EV급, -1 등급)에서는 아직 합성도 실행도 하지 않았다. KV260의 우리 값은 모두 추정이다.

## 1. 결론

- **범용 비전 CNN:** Resnet18이나 MobileNetV2를 가격·전력 대비 속도로 겨루면 Pi 5 + Hailo-8과 Jetson Orin Nano Super를 이길 수 없다. 같은 KV260에 무료로 올리는 AMD DPU에도 진다. 차이의 대부분은 배열 크기(MAC 수)다. 효율(피크 대비 실효)은 우리가 DPU와 같거나 높다.
- **처음에 든 장점 가운데 정정한 것:**
  - "I/O와 전처리를 PL에서 직접 처리": Jetson과 Pi에도 MIPI 카메라 ISP가 있어서 일반적인 경우는 장점이 아니다.
  - "결정적인 지연": 우리도 매 layer에 Linux 위의 CPU가 관여해서, 지금은 장점이 아니다.
- **가장 큰 장점: 새 모델이 나왔을 때 빠르게 최적화할 수 있다.**
  - **고정 NPU의 약점:** Hailo-8, AMD DPU, Coral 같은 고정 NPU는 하드웨어가 이미 정해져 있다. 새 모델에 처음 보는 연산이 있으면 CPU로 넘기거나 컴파일이 실패한다. 업체가 컴파일러를 고쳐 줄 때까지 기다려야 하고, 하드웨어는 다음 칩이 나와야 바뀐다.
    - Hailo-8은 Transformer를 연산량이 비슷한 CNN보다 약 30배 느리게 돌린다(DeiT-tiny 11.2 ms 대 Resnet18 0.37 ms).
    - YOLO12는 Hailo-8 컴파일이 실패한다.
    - Vitis AI DPU는 softmax를 CPU로 넘긴다(§5.1).
  - **우리:** NPU 하드웨어(RTL)와 컴파일러(NEST-C)를 둘 다 직접 가지고 있다. 그래서 새 모델에 맞춰 소프트웨어만이 아니라 하드웨어도 바꿀 수 있다. FPGA라서 이미 설치된 장비도 bitstream만 바꾸면 된다.
  - **실제로 해 본 근거:**
    - depthwise conv를 하루 안에 EVTA로 옮겼다.
    - requantize와 bias reset 같은 RTL 변경도 하루 안팎이었다.
    - MobileNetV2는 이틀 만에 1,048 → 28.6 ms가 됐다.
    - DeiT-tiny는 이틀 만에 3.77 s → 88.5 ms가 됐다(§6).
  - **그래서 기존 NPU가 약한 새 모델을 찾았다.** Hailo / Vitis AI model zoo에서 지원이 약하거나 느린 최신 모델을 찾았고, 그렇게 나온 후보가 DeiT-tiny / ViT-tiny, YOLO12n, RT-DETR, FastViT / EfficientFormer / LeViT다(§5). 그중 첫 검증 모델로 DeiT-tiny를 골랐다.
  - **조건:** 경쟁사도 새 모델을 "돌리기"는 대개 한다(CPU fallback, plugin). 우리는 기본 속도에서 뒤에서 출발하고, EVTA 연산 범위가 아직 좁다. 그래서 이 장점은 고정 NPU가 CPU로 넘겨 크게 느려지는 모델에서 통한다(§4.4).
- **그 밖에 남는 장점(PL을 쓰는 부분):**
  - 표준이 아닌 센서와 인터페이스
  - ISP에 없는 전처리를 센서 속도로
  - 인코더 하드웨어(VCU)
  - 산업 등급과 장기 공급
- **DeiT-tiny 진행(ZCU102 실측):**
  - 1장 3.77 s에서 시작해 1코어 88.5 ms, 4코어(`-t 4`) 55.7–57.9 ms가 됐다.
  - EVTA 시간은 19.4 ms(130 GOPS)로, DRAM 전송을 모두 뺀 하한 수준이다.
  - Hailo-8(11.2 ms)과는 아직 5배 차이다. 남은 시간은 대부분 CPU의 비선형 연산과 데이터 이동이다.

## 2. 비교 대상 플랫폼

### 2.1 사양 (11-09 00:18)

| | **KV260 + 우리 EVTA** | **Pi 5 + AI HAT+ (Hailo-8)** | **Jetson Orin Nano Super** | Pi 5 + AI Kit (Hailo-8L) | Coral USB | RK3588 보드 (Orange Pi 5 등) |
|---|---|---|---|---|---|---|
| 가격 | 약 $249 + 전원 $25 (전원 확인됨; 보드 값은 DigiKey 표시가 제각각) | HAT **$110** (확인됨) + Pi 5 약 $60–80 → 약 $170–190 | **$249** (확인됨) | 약 $70 + Pi 5 | 약 $60 + host | 약 $100–180 |
| int8 피크 | 16×16 @ 약 300 MHz ≈ 0.15 TOPS (같은 칩에 32×16이나 2개 구성까지 가능) | 26 TOPS | 67 TOPS (sparse 기준, 확인됨) | 13 TOPS | 4 TOPS | NPU 6 TOPS |
| 전력 | 벽 전원 약 15–16 W, FPGA만 약 6.5 W (확인됨) | Pi 5 약 5–8 W + Hailo-8 약 2.5 W | 7 / 15 / 25 W 모드 (확인됨) | Pi 5 + 약 1.5 W | 약 2 W | 5–10 W |
| 크기 | 약 119×140 mm (carrier 포함; K26 SOM만 77×60 mm) | 85×56 mm + HAT를 겹쳐 쌓음 | 100×79 mm (dev kit) | 위와 같음 | 65×30 mm 동글 | 약 100×62 mm |
| 주요 인터페이스 | USB3 ×4, GbE, HDMI, DP, MIPI CSI (IAS ×2, RPi 카메라), Pmod, **PL을 통한 임의의 I/O** | USB3 ×2, GbE, micro HDMI ×2, MIPI ×2, PCIe ×1 (HAT가 차지) | USB3 ×4, GbE, DP, MIPI CSI ×2, M.2 ×2 | 위와 같음 | USB3 | USB3, 2.5GbE, HDMI, MIPI, PCIe |
| 소프트웨어 | 우리 NEST-C (ONNX → bundle) | Hailo Dataflow Compiler (TFLite / ONNX) | CUDA / TensorRT, 모델 지원 범위가 가장 넓음 | 위와 같음 | TFLite (Edge TPU용 컴파일 제한) | RKNN Toolkit |

**같은 KV260 위의 직접 경쟁자는 AMD DPU다.**
- Vitis AI DPU(B4096 등)는 KV260에 무료로 올릴 수 있다.
- 우리 16×16의 약 8배 피크다(1.23 TOPS 대 약 0.15 TOPS).

### 2.2 KV260 SOM 가격과 칩 가격 (11-09 00:10)

| 품목 | 가격 | 비고 |
|---|---|---|
| XCZU5EV-1SFVC784E 칩 단품 | €1,799 (DigiKey ES, 부가세 제외) / €2,141 (DigiKey DE) / $2,251 (Octopart) / $2,889 (Mouser) | 리드타임 40주. 1,000개 단가는 공개되지 않음(AMD나 대리점 견적 필요) |
| Kria K26 SOM (SM-K26-XCL2GC) | €349–351, $369 (1개) | ZU5EV급 칩 + DDR4 4 GB + eMMC 16 GB + 전원 |
| K26 수량별 (Cytech) | 100개 이상 $451, 1,000개 이상 $426 | 재고 0인 참고가 |
| K26 산업 등급 (SM-K26-XCL2GI) | AMD 권장가 $450 | |

- 칩을 따로 사는 것보다 **K26 SOM에 직접 설계한 carrier 보드**를 붙이는 편이 훨씬 싸다. carrier는 4–6층 기판이면 된다.
- KV260 개발 키트가 바로 K26 + carrier 구조다. 그래서 KV260에서 개발한 것을 그대로 옮길 수 있다.

### 2.3 같은 칩 계열의 싼 보드 (11-09 00:05, 자원 값은 기억에 의존)

| 판정 | 보드 |
|---|---|
| ✗ (16×16이 안 들어감) | EBAZ4205(약 $27–35, 7010), MiniZed, Cora Z7, ZUBoard 1CG(8×16으로 줄이면 가능) |
| △ (DSP packing, buffer 축소, DSP48E1 이식 필요) | PYNQ-Z2, Arty Z7-20 (Zynq-7020) |
| ○ (HDL 이식 없이) | PYNQ-ZU($199, ZU5EG), Ultra96-V2($249, ZU3EG, wgt buffer 축소), **KV260**(약 $249, ZU5EV; wgt buffer를 URAM으로) |

- 추천은 KV260이다(DSP 1,248, URAM, AMD 지원).
- 저가 칩은 -1 등급이라 클럭은 300 MHz 안팎으로 예상한다.

## 3. 성능 비교

### 3.1 CNN 공개 수치 (int8, 224×224, 1장씩; 11-09 00:31)

| 플랫폼 | 가격 | Resnet18 | MobileNetV2 | ResNet-50 | 전력 | 측정 출처 |
|---|---|---|---|---|---|---|
| **KV260 + 우리 EVTA** (16×16, 약 300 MHz 가정) | 약 $249 + 전원 $25 | **약 32 ms 추정** (ZCU102 400 MHz 실측 24.1 ms를 클럭 비율로 환산) | **약 35 ms 추정** (ZCU102 28.6 ms, CPU 부분 포함) | 약 110 ms 추정 (ZCU102 82.2 ms) | 보드 약 15 W | ZCU102 실측에서 환산 |
| **KV260 + AMD DPU** (B4096 1코어, 300 MHz, 1.23 TOPS) | 위와 같음 | **186 FPS (약 5.4 ms)**, 여러 스레드 214 FPS | **291.5 FPS (약 3.4 ms)**, 여러 스레드 339 | 85.7 FPS (약 11.7 ms) | 보드 약 15–16 W, FPGA 약 6.5 W | Vitis AI 3.0 표 (Resnet18만 1.4판, 전처리·후처리 포함) |
| **Pi 5 + AI HAT+ (Hailo-8, 26 TOPS)** | HAT $110 + Pi 5 | **2,718 FPS** | **2,597 FPS** | 1,372 FPS | Hailo-8 약 2.5 W + Pi 5 | Hailo model zoo, 배치 1 |
| Pi 5 + AI Kit (Hailo-8L, 13 TOPS) | 약 $70 + Pi 5 | 1,281 FPS | 1,738 FPS | 138 FPS (배치 8은 502) | Pi 5 + 약 1.5 W | Hailo model zoo, 배치 1 |
| **Jetson Orin Nano Super** (67 TOPS sparse) | $249 | 공식 수치 없음 | 공식 수치 없음 | 공식 수치 없음. 커뮤니티 값은 정밀도나 모드가 불명확 (105–950 qps) | 7 / 15 / 25 W | NVIDIA: super 모드 표는 없음, 직접 `trtexec --int8`로 재라고 함 |
| RK3588 (Orange Pi 5 Max, NPU 6 TOPS) | 약 $100–180 | **4.09 ms (244 FPS)**, NPU 3코어 | – | – | 5–10 W | 블로그 실측 (10회 평균) |
| Coral Edge TPU (4 TOPS) | 약 $60 + host | – | 2.6 ms | 42 ms (weight가 칩에 다 들어가지 않음) | 약 2 W | Coral 공식 |

- **Hailo FPS:** x86 host에 PCIe Gen3 ×4로 연결해 잰 처리량이다. Pi 5는 Gen3 ×1이라 이보다 낮을 수 있다. 다른 표에 나온 "Hailo-8 Resnet18 1.2 ms"는 1장이 파이프라인을 다 지나는 지연이라 1/FPS(0.37 ms)보다 길다.
- **KV260 DPU:** 전처리와 후처리를 포함한 end-to-end 값이다. 우리 수치(이미지를 넣어 결과가 나올 때까지)와 대체로 같은 기준이다.
- **같은 KV260에서:** DPU가 우리보다 Resnet18 약 6배, MobileNetV2 약 10배 빠르다. 피크 차이(약 8배)와 맞아서, 효율보다 배열 크기의 차이다.
- **Hailo-8 대 KV260 DPU:** Hailo-8이 MobileNetV2 약 9배, Resnet18 약 15배 빠르고 전력은 몇 분의 1이다. RK3588(4 ms)도 KV260 DPU(5.4 ms)보다 빠르고 값은 절반이다.

**11-10 기준 우리 ZCU102 실측**(OMP 1)으로 KV260 추정을 고친 값이다. KV260 값은 EVTA 부분을 300/400으로 늘리고, CPU 부분은 그대로 둔 대략값이다.

| 모델 | ZCU102 11-08 | ZCU102 11-10 | KV260 추정 (11-10 기준) |
|---|---|---|---|
| Resnet18 | 24.1 ms | 23.3 ms | 약 31 ms |
| MobileNetV2 | 28.6 ms | 18.8 ms | 약 23 ms |
| ResNet-50 | 82.2 ms | 69.6 ms | 약 92 ms |

### 3.2 피크 대비 효율 (11-08 21:25, 23:14)

**MobileNetV2** (224×224, int8, 배치 1, 약 0.6 GOP; 효율 = 0.6 GOP ÷ 측정 시간 ÷ 피크)

| 장치 | 피크 int8 | MobileNetV2 1장 | 실효 / 피크 |
|---|---|---|---|
| Snapdragon 8 Elite Hexagon NPU (스마트폰) | 수십 TOPS (비공개) | **약 0.30 ms** (TFLite), 0.37 ms (QNN) | – |
| Google Coral Edge TPU | 4 TOPS | 2.6 ms | 약 6 % |
| Xilinx DPU, **같은 ZCU102** (B4096 ×3, 281 MHz) | 3.45 TOPS (코어 1개 1.15) | 스레드 1개 약 3.6 ms (274.9 FPS), 여러 스레드 745 FPS | 코어 1개 기준 약 14 % |
| **우리 EVTA** (16×16, 400 MHz), 11-08 | 0.205 TOPS | **28.6 ms** (OMP 1), 22.5 ms (OMP 4, depthwise는 CPU) | 약 10 % |
| **우리 EVTA**, 11-10 | 0.205 TOPS | **18.8 ms** (OMP 1) | 약 16 % |

**Resnet18** (224×224, int8, 약 3.7 GOP)

| 장치 | 피크 int8 | Resnet18 1장 | 실효 성능 / 피크 |
|---|---|---|---|
| Snapdragon 8 Elite NPU (스마트폰) | 비공개 (수십 TOPS) | **약 0.23–0.30 ms** (TFLite) | 약 13 TOPS 실효 |
| Xilinx DPU, **같은 ZCU102** (B4096 ×3, 281 MHz) | 코어 1개 1.15 TOPS, 3개 3.45 TOPS | 스레드 1개 184.9 FPS (약 5.4 ms), 여러 스레드 477 FPS | 코어 1개 약 685 GOPS = **약 60 %** |
| **우리 EVTA 1개** (systolic 16×16, 400 MHz) | 0.205 TOPS | **24.1 ms** (41 FPS; 11-10 23.3 ms) | 153 GOPS = **약 75 %** (conv만 157 GOPS) |
| **우리 multi4** (EVTA 4개 동시) | 0.82 TOPS | 4개가 각각 1장에 24.6 ms → 약 163 FPS | |
| Hailo-8 | 26 TOPS | Resnet18 수치는 못 찾음. ResNet-50이 1,332 FPS (파이프라인 처리량) | |
| Coral Edge TPU | 4 TOPS | Resnet18 수치는 못 찾음. ResNet-50이 42 ms | |

- **DPU와의 차이:** 1장 시간은 4.5배, 처리량은 2.9배(477 대 163 FPS)다. 피크 차이(코어 1개 5.6배, 전체 4.2배)를 생각하면 효율은 따라잡았다.
- **구조의 차이:**
  - 우리 EVTA: weight-stationary 16×16, layer마다 DRAM ↔ buffer
  - DPU B4096: 코어당 2,048 MAC, DSP를 2배 클럭으로, 3코어. 우리의 약 24배 MAC이다.
  - Coral: weight를 칩 안 8 MB SRAM에 상주시킨다.
  - Hailo-8: dataflow 구조로, layer를 칩 위에 펼쳐 파이프라인으로 돌린다.
  - Hexagon: layer를 묶어 칩 안에서 처리한다.

### 3.3 더 작은 Zynq에 넣는다면 (11-08 23:58)

| 자원 | 우리 사용량 (`..._sa_400mhz_rq`) | ZCU102 | Zynq-7045 | Zynq-7020 |
|---|---|---|---|---|
| LUT | 42,119 | 274,080 | 218,600 | 53,200 |
| FF | 50,587 | 548,160 | 437,200 | 106,400 |
| BRAM36 | 256 | 912 | 545 | **140** |
| DSP | 273 (PE마다 DSP48E2 1개) | 2,520 | 900 | **220** |

| | 피크 | 예상 Resnet18 | 같은 칩의 공개 비교 대상 |
|---|---|---|---|
| 7045 @ 200 MHz | 102 GOPS | 약 50 ms 안팎 | NEURAghe 150 ms |
| 7020 @ 125 MHz (DSP packing) | 64 GOPS | 약 75–90 ms | MINT 18.86 GOPS → 3.6 GOP 기준 약 190 ms |

- XC7Z045 칩은 DigiKey 약 $2,873–2,932(리드타임 40주)다. ZC706 공식 보드는 $3,495, 중국산 Puzhi PZ7045는 약 $599다.
- 7045/7020은 DSP48E1 이식과 A9용 NEON 이식이 필요하다. 그래서 KV260보다 나은 선택이 아니다.

## 4. KV260 제품의 장점: 처음 주장과 정정

### 4.1 처음 든 네 가지 (11-09 00:18)

1. I/O와 전처리를 PL에서 직접 처리
2. 결정적인 지연
3. 수명과 등급 (K26 산업 등급, 장기 공급)
4. 자체 IP (NPU와 컴파일러를 직접 소유)

### 4.2 정정 ① 카메라 처리 하드웨어는 Jetson과 Pi에도 있다 (11-09 00:39)

칩 기능은 알고 있는 사양 기준이다. 따로 검색해 확인하지는 않았다.

| | Jetson Orin Nano | Raspberry Pi 5 (+ Hailo) | KV260 (ZU5EV) |
|---|---|---|---|
| 카메라 입력 | MIPI CSI-2, 카메라 최대 4대 (8 lane) | MIPI CSI-2 4 lane ×2 (카메라 2대) | MIPI ×2–3 (carrier에 따라 다름) + **PL에 직접 연결하는 아무 인터페이스** |
| ISP (debayer, 화이트밸런스, 노이즈 제거 등) | 있음 (전용 하드웨어) | 있음 (Pi ISP, libcamera) | 칩에는 없음. PL에 AMD ISP IP나 직접 만든 IP를 넣음 |
| 리사이즈, 색 변환 | VIC 하드웨어 | ISP가 모델 입력 크기의 저해상도 출력을 같이 냄 | PL IP |
| 영상 코덱 | 디코드 하드웨어 있음, **인코드 하드웨어 없음** (Orin Nano) | H.265 디코드만 | **VCU: H.264/H.265 인코드·디코드 하드웨어** (EV 등급) |
| 개발 편의 | DeepStream, GStreamer, Argus | rpicam-apps + Hailo 후처리 연동 | Vitis 영상 파이프라인, 직접 설계 부담이 큼 |

표준 MIPI 카메라 한두 대로 분류나 검출만 하는 제품이면, Jetson이나 Pi + Hailo가 더 쉽고 싸고 빠르다. FPGA가 실제로 다른 점은 아래 넷이다.
1. **표준이 아닌 센서와 인터페이스:**
   - SLVS-EC / sub-LVDS 센서, line-scan 카메라, CoaXPress, Camera Link, SDI
   - 열화상 raw 출력, 고속 ADC(레이더, 초음파), 4대 이상을 동기화한 카메라
2. **ISP에 없는 전처리를 센서 데이터 속도로:** 스테레오 정렬, 특수 컬러 필터 배열, 센서 퓨전, 픽셀 단위 보정을 메모리를 거치지 않고 줄 단위로 한다. Jetson은 이런 처리를 추론과 같은 GPU에서 해야 한다.
3. **센서에서 출력까지 결정적인 지연** (아래 4.3에서 정정)
4. **인코딩까지 같이 하기:** Orin Nano와 Pi 5에는 인코더 하드웨어가 없다. ZU5EV는 VCU가 있다.

### 4.3 정정 ② 결정적인 지연은 지금 우리 구조에도 해당하지 않는다 (11-09 00:47)

우리 EVTA도 매 layer에 CPU가 관여한다.
- Linux 위 vtalib가 명령을 넣고, cache flush / invalidate와 polling을 한다.
- elemadd, 전처리 같은 CPU 연산도 있다.

11-08 측정값의 퍼짐(single rq, OMP 1, `chrt -f 10`, run 2–25):

| 모델 | 중앙값 | 최소–최대 | 퍼짐 |
|---|---|---|---|
| Resnet18 | 24.079 ms | 24.068–24.184 | 약 0.5 % |
| Resnet50 | 82.201 ms | 82.131–82.225 | 약 0.1 % |
| VGG16 | 7.273 ms | 7.256–7.310 | 약 0.7 % |

- Jetson이나 Pi + Hailo도 Linux에서 비슷할 것으로 본다(추정).
- 차이는 드물게 튀는 최악값에서 난다. 몇 시간에 걸친 p99.99 측정이 필요하다.
- 장점으로 내세우려면 CPU를 경로에서 빼야 한다. 방법은 셋 중 하나다.
  - 명령 ring + interrupt
  - 하드웨어 loop FSM(센서 → PL 전처리 → EVTA 전체 layer → 출력)
  - ZU5EV의 Cortex-R5에서 bare-metal / RTOS로 EVTA 제어. Pi 5에는 이런 코어가 없다.

### 4.4 가장 큰 장점: 하드웨어와 컴파일러를 둘 다 가진 점 (11-09 00:53)

새 모델이 나와 기존 model zoo에 없을 때, 고정 NPU는 업체의 컴파일러 갱신을 기다리거나 CPU로 넘겨 느리게 돌린다. 우리는 소프트웨어와 하드웨어를 모두 바꿔 빠르게 최적화해서 공급할 수 있다.

**근거:**
- depthwise conv를 EVTA로 옮기는 데 하루가 걸리지 않았다(대각 weight, vector SHR, schedule 탐색).
- requantize나 bias reset 같은 RTL 변경도 bitstream과 검증까지 하루 안팎이었다(11-04).
- MobileNetV2는 이틀 작업으로 1,048 ms → 28.6 ms가 됐다.
- 현장 장비도 bitstream만 갱신하면 하드웨어가 바뀐다.

**과장일 수 있는 부분:**
- 경쟁사도 새 모델을 "돌리기"는 대개 한다(CUDA, plugin, host CPU fallback).
- 우리는 기본 속도에서 10–30배 뒤에서 출발한다.
- EVTA 연산 범위가 좁다. activation끼리의 matmul, softmax, layernorm, GELU가 없고, ALU opcode가 2 bit라 MUL을 인코딩하지 못한다.
- 모델마다 사람이 맞추는 일이라 확장이 어렵다.

**통하는 곳:** 고정 NPU가 CPU로 넘겨 크게 느려지는 연산이 핵심인 모델이다(새 양자화 형식, 특수 activation, 신호 처리 + NN). 같은 KV260의 AMD DPU 대비로도 확실한 장점이다.

### 4.5 남는 장점

| 장점 | 지금 성립하나 | 조건 |
|---|---|---|
| **새 모델에 하드웨어와 소프트웨어를 함께 맞춤** (고정 NPU가 약한 모델) | ○ 근거 있음 (depthwise, MobileNetV2, DeiT-tiny) | 연산 범위 확장(ISA), 배열 확대, 대응 시간을 줄이는 도구 → §5, §6 |
| 표준이 아닌 센서와 인터페이스 | ○ | PL IP 설계 필요 |
| ISP에 없는 전처리를 센서 속도로 | ○ | PL IP 설계 필요 |
| 인코더 하드웨어 (VCU) | ○ | ZU5EV(EV 등급) |
| 산업 등급, 장기 공급 | ○ | K26 -I SOM ($450) |
| 결정적인 지연 | ✗ (지금) | 명령 ring / HW loop / R5 제어 |
| 표준 카메라 + 범용 CNN의 가격·속도 | ✗ | – |

## 5. 새 모델을 찾은 이유와 DeiT-tiny를 고른 이유 (11-09 00:59)

§4.4의 결론은 "새 모델이 나왔을 때 고정 NPU는 느리거나 못 돌리고, 우리는 하드웨어까지 바꿔 빠르게 맞출 수 있다"였다. 이것을 보이려면 기존 NPU가 약한 최신 모델로 직접 검증해야 한다. 그래서 Hailo와 Vitis AI에서 지원이 약하거나 느린 모델을 찾기 시작했다. 아래 표와 실패 사례에서 DeiT-tiny / ViT-tiny, YOLO12, RT-DETR 등이 나왔다.

### 5.1 Hailo-8 model zoo (공식 표, 배치 1)

| 모델 | 연산량 | 정확도 float → Hailo | FPS | 1장 환산 |
|---|---|---|---|---|
| resnet_v1_18 (CNN) | 3.64 GOP | 71.3 → 71.1 | 2,718 | 0.37 ms |
| mobilenet_v3 (CNN) | – | 72.2 → 71.8 | 3,697 | 0.27 ms |
| **vit_tiny** | 약 2.5 GOP | 75.5 → 73.9 (−1.6) | **89.2** | **11.2 ms** |
| **deit_tiny** | 약 2.5 GOP | 69.1 → 68.5 | **89.1** | **11.2 ms** |
| deit_small | | 78.2 → 77.0 | 70.0 | 14.3 ms |
| swin_tiny | | 81.3 → 79.2 (−2.1) | 31.7 | 31.5 ms |
| levit384 | | 82.3 → 78.6 (**−3.7**) | 65.6 | |
| fastvit_sa12 | | 79.8 → 76.7 (**−3.1**) | 1,113 | |
| efficientformer_l1 | | 79.1 → 76.2 (**−2.9**) | 96.4 | |

ViT 계열의 연산량은 기억에 의존한 값이다.

- **Hailo-8의 Transformer:** 연산량이 비슷한 CNN보다 **약 30배 느리다**(DeiT-tiny 11.2 ms 대 Resnet18 0.37 ms).
- **다른 최신 모델:** YOLO12 기본 모델은 Hailo-8 컴파일이 실패한다(`UnsupportedShuffleLayerError`). RT-DETR와 YOLOv10 등은 Ultralytics의 Hailo export에서 지원하지 않는다.
- **Vitis AI DPU:** softmax를 CPU로 넘긴다.
- **후보 순위:** 1) ViT-tiny / DeiT-tiny, 2) YOLO12n, 3) RT-DETR, 4) FastViT / EfficientFormer / LeViT.
- **고른 이유:** DeiT-tiny는 연산의 대부분이 우리 GEMM으로 되고(아래 표), Hailo-8과의 격차가 CNN보다 훨씬 작다. 그때 추정은 16×16(KV260 약 300 MHz, 효율 75 %)이면 약 20 ms, 32×32이면 약 5 ms였다.

### 5.2 DeiT-tiny의 연산 (198 token)

| 연산 | MAC / 원소 수 | 비중 | 11-09 당시 EVTA로 |
|---|---|---|---|
| Linear (Q/K/V, proj, MLP) = activation × 상수 weight | 약 1.05 G MAC | **약 85 %** | **가능.** 1×1 conv나 fc와 같은 GEMM이고, 차원 192/576/768이 모두 16의 배수 |
| QKᵀ, attn·V = activation × activation | 약 0.18 G MAC | 약 15 % | GEMM으로는 가능. 실행 중에 한쪽 activation을 wgt layout으로 다시 배치해야 함 |
| patch embedding conv 16×16/16 | 약 0.03 G MAC | 2 % | 가능 |
| softmax / LayerNorm / GELU | 원소 약 1.4 M / 0.95 M / 1.8 M | MAC 없음 | **불가.** ALU에 exp, 곱셈, lane 간 합이 없음 → CPU |

합계는 약 1.25 G MAC(2.5 GOP)이다. 16×16 @ 400 MHz(102 GMAC/s)에서 100 % 가동 시 하한은 12.2 ms다.

### 5.3 NEST-C에서 처음 확인한 단계 (11-09 01:12)

| 단계 | 결과 | 내용 |
|---|---|---|
| ONNX import | △ 전처리 필요 | 동적 shape 계산(Shape/Gather/Where 약 300개)을 Glow가 처리하지 못함. 입력을 1×3×224×224로 고정하고 상수로 접음(`fold_onnx.py`): 1,261 → 608 연산, 원본과 출력 차이 0 |
| float 실행 (Interpreter) | ○ | cat 281, dog 207, zebra 340 |
| int8 양자화 (Interpreter, power-of-2) | ○ | 3장 모두 float와 top-1 같음 |
| VTA bundle 생성 | ✗ → 11-09에 해결 | graph hash 불안정(우회), MatMul이 `isOpSupported`에 없음, VTASave에 처리 없는 instruction은 segfault |

## 6. DeiT-tiny 성능 (ZCU102 실측)

### 6.1 진행

1장, systolic 400 MHz `_rq`(§7.26 이전은 HLS 380 MHz), 세 장(cat / dog / zebra) 모두 정답이다. § 번호는 `docs/evta_runtime_compare.md`의 절이다.

| 날짜 | 단계 | 1장 OMP 1 | `-t 4` |
|---|---|---|---|
| 11-09 | 처음 보드에서 끝까지 (대부분 CPU float, §7.22) | 3.77 s | |
| 11-09 | Linear를 EVTA 1×1 conv로 (§7.23) | 475 ms | |
| 11-09 | attention Q·Kᵀ / A·V도 EVTA로 (§7.24) | 359 ms | |
| 11-09 | GELU 표, softmax int8, LayerNorm 조각 (§7.25) | 206 ms | |
| 11-10 | matmul 포장 NEON (HLS 380 MHz) | 198.9 ms | |
| 11-10 | 같은 번들을 systolic 400 MHz로 | 184.4 ms | |
| 11-10 | Linear bias를 conv bias에 접기 | 167.7 ms | |
| 11-10 | attention 전치를 int8로 | 152.3 ms | |
| 11-10 | LayerNorm 한 함수 (§7.26) | 137.2 ms | |
| 11-10 | Q/K를 EVTA 배치 그대로, 3 head를 한 호출로 | 130.7 ms | |
| 11-10 | softmax / LayerNorm 역수 곱 (§7.27) | 123.5 ms | 91.9 ms |
| 11-10 | EVTA schedule 가상 스레드 2개, vt 2 결함 수정 (§7.28) | 107.4 ms | 75.2 ms |
| 11-10 | conv DMA 묶기 `EVTA_CONV_BURST` (§7.29) | **88.5 ms** | **55.7–57.9 ms** |

- **값:** logit과 확률(0.754596 / 0.950148 / 0.962557)은 §7.27 이후 그대로다.
- **EVTA 바쁜 시간:** 55.8 → 38.3 → **19.4 ms**(130 GOPS)다. DRAM 전송을 모두 뺀 하한(19.6 ms)과 같은 수준이다.
- **아직 모르는 것:** ImageNet 검증 이미지로 잰 정확도.

### 6.2 지금 남은 시간 (`-t 4`, 11-10 마지막 실행, 1장 57.9 ms)

| 항목 | 시간 | 어디서 |
|---|---|---|
| EVTA conv (Linear, patch embedding) | 17.1 ms | EVTA |
| layerNormI8 | 5.6 ms | CPU |
| lut8 (GELU 표) | 5.2 ms | CPU, 단일 스레드 |
| matmul_evta_qk (Q·Kᵀ) | 4.3 ms | EVTA + CPU 포장 |
| matmul_evta_n (A·V) | 4.1 ms | EVTA + CPU 포장 |
| elemadd | 3.9 ms | CPU, 단일 스레드 |
| softmaxI8 | 3.9 ms | CPU |
| transpose (NHWC ↔ EVTA 배치) | 5.4 ms | CPU |
| quantize | 3.0 ms | CPU |

### 6.3 다른 NPU의 DeiT 계열 공개 수치

| 장치 | 모델 | 지연 | 비고 |
|---|---|---|---|
| Hailo-8 (26 TOPS) | deit_tiny | **11.2 ms** (89.1 FPS) | Hailo model zoo, 배치 1 (§5.1) |
| Huawei Ascend 910 | deit_tiny_distilled | 5.74 ms (174 FPS, batch 1) | 데이터센터 칩. 사용자가 붙인 자료 (gitcode) |
| Renesas R-Car X5H (NPX6) | DeiT III-Small (tiny 아님) | 약 7.4 ms (INT8, 1코어, 850 MHz) | 사용자가 붙인 자료 (huggingface) |
| Arm Ethos-U85 (256 MAC/cycle, 400 MHz) | deit-tiny (INT8, 일부 INT16) | 수치 미확인 | 피크가 우리 16×16 @ 400 MHz와 같아 하한도 같은 12 ms대 |
| **ZCU102 우리 EVTA** (16×16, 400 MHz) | deit-tiny distilled | **88.5 ms** (OMP 1), **55.7–57.9 ms** (`-t 4`) | 실측, 이 문서 §6.1 |
| KV260 우리 EVTA (추정) | | `-t 4` 약 62–64 ms | EVTA 19.4 ms × 400/300 + CPU 부분 그대로 |

### 6.4 Hailo-8(11.2 ms) 수준으로 가려면

| 단계 | 내용 | 예상 (`-t 4`) |
|---|---|---|
| 지금 | | 55.7–57.9 ms |
| A | CPU 연산 정리: lut8 / elemadd 병렬화, 전치·quantize 없애기, EVTA와 CPU 겹치기, 정수 LayerNorm / softmax | 30–35 ms |
| B | store 경로에 int8 LUT(GELU), 행 reduction이 있는 작은 vector unit(softmax / LayerNorm) | 15–20 ms (EVTA 하한 12 ms) |
| C | B를 multi4나 32×32 배열로 (ZU5EV DSP 1,248) | 5–8 ms |

추정값이다(11-10 대화에서의 계산). KV260에서는 클럭이 300 MHz 안팎이라 EVTA 부분이 약 1.3배 길어진다.

## 7. 출처

- Hailo model zoo: [HAILO8 classification](https://github.com/hailo-ai/hailo_model_zoo/blob/master/docs/public_models/HAILO8/HAILO8_classification.rst), [HAILO8L classification](https://github.com/hailo-ai/hailo_model_zoo/blob/master/docs/public_models/HAILO8L/HAILO8L_classification.rst)
- AMD: [UG1354 (Vitis AI 3.0) KV260 performance](https://docs.amd.com/r/3.0-English/ug1354-xilinx-ai-sdk/KV260-Vision-AI-Starter-Kit), [KV260 power supply adapter](https://www.amd.com/en/products/system-on-modules/kria/k26/kv260-vision-starter-kit/power-supply-adapter.html), [K26 industrial](https://www.amd.com/en/products/system-on-modules/kria/k26/k26i-industrial.html)
- [element14: Vitis AI 1.4 flow for Kria KV260](https://community.element14.com/technologies/fpga-group/b/blog/posts/vitis-ai-1-4-flow-for-kria-kv260-vision-ai-starter-kit)
- [HotHardware: Kria KV260 review](https://hothardware.com/reviews/xilinx-kria-kv260-vision-ai-starter-kit-review?page=2), [DigiKey: KV260 Vision AI Starter Kit](https://www.digikey.co.za/en/product-highlight/x/xilinx/kv260-vision-ai-starter-kit)
- NVIDIA: [Jetson benchmarks](https://developer.nvidia.com/embedded/jetson-benchmarks), [forum: Orin Nano/NX ResNet-50 benchmark on JetPack 6.2](https://forums.developer.nvidia.com/t/orin-nano-nx-resnet-50-benchmark-on-r36-4-3-jetpack6-2/325862), [forum: Introducing the most affordable generative AI supercomputer](https://forums.developer.nvidia.com/t/introducing-the-most-affordable-generative-ai-supercomputer/317147)
- [JetsonHacks: Jetson Orin Nano Super Developer Kit](https://jetsonhacks.com/2024/12/17/jetson-orin-nano-super-developer-kit/), [Hackster: Jetson Orin Nano Super](https://www.hackster.io/news/nvidia-unveils-the-jetson-orin-nano-super-delivers-a-1-7x-gen-ai-speed-boost-for-all-3602c2e98c40), [Core Electronics](https://core-electronics.com.au/nvidia-jetson-orin-nano-developer-kit.html.plain.md)
- Raspberry Pi AI HAT+: [Tom's Hardware](https://tomshardware.com/raspberry-pi/raspberry-pi-release-higher-performance-ai-hat-13-and-26-tops-variants), [Neowin ($110)](https://www.neowin.net/news/the-new-raspberry-pi-ai-hat-delivers-26-tops-of-performance-for-110/), [The Pi Hut](https://thepihut.com/collections/raspberry-pi/products/raspberry-pi-ai-hat-plus)
- [tinycomputers.io: RK3588 NPU benchmarks](https://tinycomputers.io/posts/rockchip-rk3588-npu-benchmarks.html), [Coral Edge TPU benchmarks](https://www.coral.ai/docs/edgetpu/benchmarks)
- XCZU5EV / K26 가격: [DigiKey ES XCZU5EV](https://www.digikey.es/en/products/detail/amd/XCZU5EV-1SFVC784E/7034644), [DigiKey DE](https://www.digikey.de/en/products/detail/amd/XCZU5EV-1SFVC784E/7034644), [Octopart](https://octopart.com/part/amd/XCZU5EV-1SFVC784E), [DigiKey ES SM-K26-XCL2GC](https://www.digikey.es/en/products/detail/amd/SM-K26-XCL2GC/13985266), [Farnell IT](https://it.farnell.com/xilinx/sm-k26-xcl2gc/system-on-module-zynq-ultrascale/dp/3763930), [Cytech](https://www.cytechsystems.com.cn/de/product/sm-k26-xcl2gc), [ichome](https://ichome.com/product-detail/xilinx/sm-k26-xcl2gc)
- 사용자가 붙인 자료(11-10): [Arm DeiT-tiny for Ethos-U](https://huggingface.co/Arm/deit-tiny-int8-ethos-u-executorch), [Ascend 910 DeiT-tiny distilled](https://ai.gitcode.com/gyccc/timm-deit_tiny_distilled_patch16_224.fb_in1k-NPU), [Renesas DeiT3-Small](https://huggingface.co/Renesas/DeiT3-Small-Patch16-224-ONNX)
- 우리 실측: `docs/evta_runtime_compare.md` §7.19–§7.29, `docs/dailyreport/vta_systolic/vta_systolic_요약_261108.md`
