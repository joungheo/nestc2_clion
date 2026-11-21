# NPU IP로 하는 비즈니스: 모델마다 맞춘 FPGA NPU는 왜 효율이 낮고, 어떤 회사가 어떻게 돈을 버나

2026-11-10에 논의하고 검색한 내용을 따로 정리한 문서다. 처음 기록은 [kv260_product_compare.md](kv260_product_compare.md) §7에 있다.
여기서는 그 내용을 처음 읽는 사람도 알 수 있게 용어를 풀고, 우리 작업에서 겪은 일로 근거를 더했다.

- **값의 출처:** 회사 상황, 투자액, 시장 규모는 11-10 검색 결과다(출처는 §7). 그 뒤로 다시 확인하지 않았다.
- **"알고 있는 내용"이라고 적은 것:** 검색하지 않은 일반 지식이다.
- **우리 작업의 수치:** CLAUDE.md와 각 최적화 문서의 기록이다.

## 0. 용어

| 용어 | 뜻 |
|---|---|
| **NPU** | 신경망 연산(행렬 곱, conv)을 빠르게 하는 전용 하드웨어. 우리 EVTA가 NPU다 |
| **IP (반도체 IP)** | 칩 안에 넣을 회로 설계(RTL 등)를 상품으로 파는 것. 사는 회사가 자기 칩(SoC)에 넣는다. 예: Arm CPU 설계 |
| **SoC** | CPU, NPU, 메모리 제어기 등을 한 칩에 넣은 것. 스마트폰 AP, 차량용 칩 |
| **라이선스비** | IP를 쓸 권리를 살 때 처음 한 번 내는 돈 |
| **royalty** | 그 IP가 든 칩이 팔릴 때마다 한 개에 얼마씩 받는 돈 |
| **NRE** (Non-Recurring Engineering) | 고객 한 곳을 위해 한 번 하는 설계 작업과 그 비용. 다음 고객에게 다시 팔 수 없다 |
| **ASIC** | 한 용도로 만든 칩. 한 번 만들면 회로를 바꿀 수 없다 |
| **FPGA** | 회로를 파일(bitstream)로 다시 올릴 수 있는 칩. 같은 회로라면 ASIC보다 느리고, 비싸고, 전력을 더 쓴다 |
| **eFPGA** | SoC 안에 넣는 작은 FPGA 블록(IP) |
| **bitstream** | FPGA에 올리는 회로 파일. 우리 `.bin` |
| **고정 NPU** | 하드웨어가 정해진 NPU. Hailo-8, AMD DPU, Coral. 새 연산은 컴파일러가 지원할 때까지 CPU로 간다 |
| **프로그래머블 NPU** | 명령어로 동작을 바꾸는 NPU. 새 연산을 하드웨어를 바꾸지 않고 소프트웨어로 더한다 |

## 1. 결론

- **"고객 모델마다 FPGA에 NPU를 맞춰 넣어 파는 것"은 규모를 키우기 어렵다.** 할 수는 있지만 고객마다 하는 NRE가 된다.
  그렇게 해 온 회사(Flex Logix, Mipsology)는 기술은 인정받았다. 하지만 혼자 크지 못하고 대기업에 인수됐다(§3).
- **NPU IP로 돈을 버는 회사는 있다.** Arm, CEVA, Cadence, Synopsys, VeriSilicon, Imagination, Quadric이다(§4).
  - 수익 구조는 정해져 있다. 칩 회사에 라이선스를 팔고, 칩이 팔리면 royalty를 받는다.
  - "새 모델도 하드웨어 변경 없이 돈다"(Quadric)는 실제로 투자와 매출을 얻는 문구다.
  - 다만 이 문구는 모델마다 하드웨어를 바꾸는 것이 아니다. 처음부터 **프로그래머블한** 하드웨어를 만들고, 새 모델은 **소프트웨어로** 따라간다는 뜻이다.
- **우리에게 맞는 방향(§5):**
  - 하드웨어는 모델마다 새로 만들지 않는다. EVTA ISA와 ALU 확장처럼 **여러 모델에 쓰이는 일반 기능**으로 키운다.
  - 새 모델 대응은 컴파일러(NEST-C)의 일로 만든다. 지금 VTASave에 모델마다 손으로 넣는 패턴을 줄여 간다.
  - 공급 형태는 FPGA 솔루션(K26 SOM + bitstream + 모델, §5의 B)이 가장 현실적이다. 시간이 지나면 IP 라이선스(A)로 넓힐 수 있다.

## 2. 왜 이 질문이 나왔나

- **새 모델은 계속 나온다**(알고 있는 내용).
  - YOLO는 v8(2023), v9 / v10 / 11(2024), 12(2025)로 거의 매년 새 버전이 나오고, attention 같은 새 연산이 들어간다.
  - 비전 모델은 CNN에서 ViT / DeiT(2020–21), Swin, 하이브리드(FastViT, EfficientFormer)로 갔다. 이제 LLM과 VLM이 edge로 내려온다.
- **고정 NPU는 새 모델에 약하다.** Hailo, Vitis AI, RKNN 모두 컴파일러를 계속 고친다.
  - 그래도 Hailo-8은 YOLO12 컴파일이 실패하고, DeiT-tiny를 비슷한 연산량의 CNN보다 약 30배 느리게 돌린다(11.2 ms 대 Resnet18 0.37 ms, [kv260_product_compare.md](kv260_product_compare.md) §5.1).
  - 소프트웨어로 안 되면 칩을 새로 낸다. 보통 2–3년 걸린다. Arm은 Transformer 지원이 약한 Ethos-U55 / U65 다음에 U85를 냈고, Hailo는 생성형 AI용 Hailo-10H를 따로 냈다.
- **그래서 떠오르는 생각:** "FPGA는 하드웨어를 바꿀 수 있으니, 고객의 새 모델에 맞춰 NPU를 고쳐 bitstream으로 주면 되지 않나?"
  - 우리가 실제로 해 본 것: depthwise conv를 하루 안에 EVTA로 옮겼다. requantize, bias reset 같은 RTL 변경도 하루 안팎이었다. DeiT-tiny는 이틀 만에 3.77 s → 88.5 ms가 됐다.
  - 기술로는 된다. 문제는 이것이 **사업으로** 효율적이냐다.

## 3. 모델마다 맞춘 FPGA NPU가 효율이 낮은 이유

### 3.1 고객마다 하는 일이 NRE가 된다

```
고객 A의 모델 ─▶ 분석 ─▶ RTL 수정 ─▶ 합성 / timing ─▶ 검증 ─▶ 컴파일러 수정 ─▶ 납품
고객 B의 모델 ─▶ 분석 ─▶ RTL 수정 ─▶ 합성 / timing ─▶ 검증 ─▶ 컴파일러 수정 ─▶ 납품
                 (고객마다 처음부터 다시. 사람 수만큼만 고객을 받는다)

IP / 프로그래머블 NPU:
하드웨어 한 번 ─▶ 칩 / bitstream 하나 ─▶ 고객 A, B, C ... (새 모델은 컴파일러 갱신으로)
```

- 한 고객을 위해 고친 회로는 다른 고객에게 다시 팔기 어렵다. 매출이 일하는 사람 수에 묶인다.
- 고객은 그 맞춤 하드웨어를 몇 년 동안 유지보수해 주기를 바란다. 고객이 늘수록 관리할 판(版)도 늘어난다.

### 3.2 FPGA 회로를 바꾸는 일은 생각보다 비싸다 (우리 기록)

| 겪은 일 | 기록 |
|---|---|
| bitstream 한 번 만드는 데 HLS 약 10분 + Vivado 약 40분 | `docs/evta_user_manual.md` §2 |
| 400 MHz에서 timing이 안 맞아 전략을 바꿔 가며 여러 번 다시 빌드. 몇 번은 RTL에 register를 더해야 맞았다(`_alux` 400 MHz WNS -0.055 ns → register 추가 후 +0.014 ns) | CLAUDE.md, `docs/evta_isa_ext.md` §8.12–8.13 |
| ALU 확장으로 pipeline이 깊어져 "17 step 안에 쓴 값을 읽지 말 것" 같은 규칙이 생겼다. 컴파일러와 검사 도구(`hazard_dump.py`)가 함께 바뀌어야 했다 | `docs/evta_isa_ext.md` §8.12 |
| 이 보드의 결함 행(Y219) 때문에 multi4는 배치와 route를 따로 해야 했다 | `vta-hw-.../y219route/README.md` |
| 합성 중 PC 과열로 여러 번 꺼짐 | [host_pc_shutdown.md](host_pc_shutdown.md) |

- 회로를 하나 바꿀 때마다 합성, timing, 보드 시험(golden, dump 비교)을 처음부터 다시 한다. 하루에 몇 번밖에 못 돈다.
- 고객마다 이것을 하면, 한 고객에 드는 사람과 시간이 크다.

### 3.3 FPGA 쪽에는 무료 경쟁자가 있다

- AMD는 자기 FPGA(Kria, Zynq)에 DPU IP와 Vitis AI 컴파일러를 **무료로** 준다.
- FPGA로 AI 가속을 사려는 고객은 먼저 이것을 쓴다. 맞춤 NPU는 "DPU로는 안 되는" 경우에만 팔린다. 시장이 좁다.

### 3.4 같은 회로라면 FPGA는 ASIC보다 불리하다 (알고 있는 내용)

- 같은 NPU를 ASIC으로 만들면 FPGA보다 클럭이 높고, 칩 값이 싸고, 전력이 적다.
- 그래서 물량이 커지면 고객은 ASIC(또는 ASIC NPU가 든 칩)으로 옮긴다. FPGA 맞춤 NPU는 물량이 작은 산업용 틈새에 머문다.
- 우리 비교에서도 범용 CNN을 가격·전력 대비 속도로 겨루면 Pi 5 + Hailo-8, Jetson Orin Nano Super에 진다([kv260_product_compare.md](kv260_product_compare.md) §1).

### 3.5 실제로 그렇게 끝난 회사

| 회사 | 한 일 | 결과 (11-10 검색) |
|---|---|---|
| **Flex Logix** (InferX) | 재구성 가능한 eFPGA를 AI / DSP 가속에. 처음에는 칩(X1)을 팔았다 | 2023년 칩 판매를 접고 IP만 팔았다. 2024-11 ADI가 기술과 팀을 인수(조건 비공개) |
| **Mipsology** (Zebra) | FPGA에서 모델을 바꾸지 않고 추론하는 소프트웨어 | 2023-08 AMD가 인수. AMD의 통합 AI 소프트웨어 스택 쪽으로 흡수 |
| (오픈소스) AMD FINN, hls4ml | 모델마다 그 모델 전용 dataflow 하드웨어를 자동으로 만드는 도구 | 연구와 도구 수준. 이것으로 사업하는 회사는 없다(알고 있는 내용) |

- 두 회사 모두 기술 가치는 인정받아 대기업이 샀다. 하지만 독립 회사로 크지는 못했다.
- 이유는 §3.1–§3.4와 같다. 모델마다 하는 일이 용역이 되기 쉽고, FPGA 쪽은 AMD의 무료 도구와 겨뤄야 한다.

## 4. NPU IP로 돈을 버는 회사

### 4.1 회사들 (11-10 검색)

| 회사 | 형태 | 내세우는 점 | 상황 |
|---|---|---|---|
| **Quadric** (Chimera GPNPU) | NPU IP 라이선스 | "완전 프로그래머블이라 지금과 앞으로의 모델을 하드웨어 변경 없이 돌린다." 2023-06 하드웨어 변경 없이 ViT 지원, 같은 해 Llama 2도 소프트웨어 이식으로 지원 | 2026-01 Series C $30M, 누적 약 $72M(출처에 따라 $76M). 2025년 제품 매출 3배 이상 |
| **CEVA** (NeuPro) | NPU IP + royalty | DSP IP 회사가 NPU로 넓혔다 | 2025년 NPU 라이선스 10건, AI가 라이선스 매출의 20% 이상. Microchip이 NeuPro 전체를 라이선스 |
| **Arm** (Ethos), **Cadence**, **Synopsys** (ARC NPX), **VeriSilicon**, **Imagination** | NPU IP | SoC 회사에 CPU 등 다른 IP와 묶어서 판다 | 시장의 주류. 칩 회사 안의 자체 설계팀과도 경쟁한다 |

### 4.2 어떻게 돈을 버나

```
NPU IP 회사 ──(설계 + 컴파일러)──▶ 칩 회사(SoC) ──(칩)──▶ 장비 회사 ──▶ 제품
     ▲  라이선스비(처음 한 번)            │
     └────── royalty(칩 하나마다) ───────┘
```

- **라이선스비 + royalty:** 처음에 라이선스비를 받고, 칩이 팔리면 개수만큼 royalty를 받는다. Arm과 CEVA의 구조다.
- **royalty는 늦게 들어온다.** 고객이 칩을 설계해서 내놓기까지 2–3년이 걸린다. 그동안은 라이선스비로 버텨야 한다.
- **하드웨어는 한 번, 고객은 여럿:** 같은 IP를 여러 칩 회사에 판다. 고객마다 바뀌는 것은 설정(배열 크기, buffer)과 컴파일러 지원 정도다. 그래서 규모가 커진다. §3.1의 맞춤 방식과 정반대다.
- **컴파일러가 상품의 절반이다.** 칩 회사는 "우리 모델이 돌아가느냐"로 고른다. 그래서 IP 회사는 컴파일러와 model zoo를 계속 갱신한다.

### 4.3 시장 규모 (11-10 검색)

- 한 추정은 NPU IP 시장을 2025년 약 $1.7억, 2035년 $6억 이상으로 본다.
- NPU 시장 전체(칩 포함)는 한 보고서가 2025년 약 $172억으로 본다. 범위가 달라 직접 비교할 수는 없다.
- 칩 시장에 비하면 IP는 작은 틈새다. 그 안을 Arm 같은 큰 회사가 나눠 갖는다.

## 5. 우리에게 맞는 형태

FPGA라서 ASIC IP 회사에는 없는 선택지(B)가 하나 더 있다.

| 형태 | 내용 | 장단점 |
|---|---|---|
| A. NPU IP 라이선스 | EVTA RTL + NEST-C를 칩 회사에 공급 | 시장은 있다. 하지만 Arm / CEVA / Quadric과 겨뤄야 하고, royalty까지 오래 걸린다. 성능부터 갖춰야 한다 |
| **B. FPGA 솔루션 공급** | K26 SOM + bitstream + 모델 하나를 장비 회사에 묶어서 공급 | 고객의 새 모델에 bitstream 갱신으로 대응해 "하드웨어도 바뀐다"는 장점이 가장 잘 드러난다. 고객 수가 적은 산업용 틈새다 |
| C. 모델 대응 서비스 | 새 모델이 나오면 하드웨어와 소프트웨어를 최적화해 납품 | 용역이라 규모를 키우기 어렵다(Mipsology와 같은 길, §3) |

**어떻게 하나:**
- **가장 현실적인 것은 B다.** 시간이 지나면 A로 넓힐 수 있다.
- **하드웨어는 고객마다 고치지 않는다.**
  - 바꾸는 것은 여러 모델에 쓰이는 일반 기능으로 한정한다. 예: ALU 확장(LUT, MUL, 행 단위 합 / 최대)은 layer norm, softmax, GELU, avgpool, elemadd에 같이 쓰인다.
  - 그러면 bitstream 종류가 고객 수만큼 늘지 않는다. §3.2의 합성 비용도 한 번만 든다.
  - 이것은 Quadric이 "프로그래머블"을 앞세우는 이유와 같다.
- **새 모델 대응은 컴파일러의 일로 만든다.**
  - 지금은 모델마다 VTASave에 패턴을 손으로 추가한다. 대응 시간이 사람에 달려 있다.
  - 퓨전 규칙을 producer + tail로 일반화한 것([evta_fusion_rules.md](evta_fusion_rules.md))이 첫걸음이다. 다른 행렬 곱도 1x1 conv 모양으로 내리는 일반 경로가 다음이다.
  - CPU로 빠지는 연산과 그 이유가 늘 요약으로 나오게 했다(`VTA_FUSION_LOG`). 새 모델을 넣으면 무엇을 더해야 하는지 바로 보인다.
- **증명할 사례가 필요하다.** "새 모델 대응이 빠르다"를 보여 줄 사례다.
  - DeiT-tiny(이틀에 3.77 s → 88.5 ms, 지금 37 ms)가 첫 사례다([deit_optimization.md](deit_optimization.md)).
  - YOLO12 같은 모델로 근거를 더 쌓아야 한다.

## 6. 남은 질문

- **B의 고객:** 산업용 장비 회사 가운데 표준이 아닌 센서, 장기 공급, 새 모델을 함께 원하는 곳이 얼마나 되나. 아직 조사하지 않았다.
- **KV260에서의 실제 값:** KV260(XCK26)에서는 아직 합성도 실행도 하지 않았다. 지금의 KV260 값은 모두 ZCU102에서 환산한 추정이다([kv260_product_compare.md](kv260_product_compare.md)).
- **여러 가속기에 쓰는 runtime:** A로 가려면 우리 runtime이 다른 NPU와 같은 틀에 들어가야 한다. 계획은 [unified_runtime_plan.md](unified_runtime_plan.md)에 있다.

## 7. 출처

검색은 2026-11-10에 했다. 다른 플랫폼(Hailo, Jetson, KV260)의 출처는 [kv260_product_compare.md](kv260_product_compare.md) §8에 있다.

- NPU IP 업체: [Quadric Series C (Edge AI and Vision Alliance, 2026-01)](https://www.edge-ai-vision.com/2026/01/quadric-inference-engine-for-on-device-ai-chips-raises-30m-series-c-as-design-wins-accelerate-across-edge/), [Quadric ViT support (BusinessWire, 2023)](https://www.businesswire.com/news/home/20230620311697/en/Quadric-Announces-Vision-Transformer-Support-for-Chimera-GPNPUs), [SemiWiki: Quadric Series C](https://semiwiki.com/ip/quadric/371357-quadric-extends-series-c-to-46-million-as-edge-ai-demand-accelerates/), [Ceva 2025 AI licensing (EE Journal)](https://www.eejournal.com/industry_news/ceva-highlights-breakthrough-year-for-ai-licensing-and-physical-ai-adoption-in-2025/), [Ceva NeuPro 20 % (TechEdgeAI)](https://techedgeai.com/?p=30253), [Microchip licenses CEVA NPUs](https://www.epdtonthenet.net/article/218645/Microchip-Commences-Licensing-of-an-Extensive-Range-of-CEVA-NPUs.aspx), [NPU IP market estimate](https://briefglance.com/articles/cevas-ai-surge-powering-the-physical-ai-revolution-at-the-edge), [NPU market report](https://www.meticulousresearch.com/product/neural-processing-units-npu-market-6881), [EE Times Japan: Flex Logix moves to IP (2023)](https://eetimes.itmedia.co.jp/ee/articles/2305/12/news098.html), [EE Times Japan: ADI acquires Flex Logix (2024-11)](https://eetimes.itmedia.co.jp/ee/articles/2411/12/news111.html), [AMD acquires Mipsology (2023)](https://www.amd.com/en/blogs/2023/amd-acquires-mipsology-to-deepen-ai-inference-soft.html), [Phoronix: AMD acquires Mipsology](https://www.phoronix.com/news/AMD-Acquires-Mipsology)
