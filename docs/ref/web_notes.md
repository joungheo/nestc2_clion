# 웹 문서 발췌 (2026-10-27)

원문을 받을 수 없거나(AMD 문서 사이트, 재게시본) 재배포가 안 되는 웹 문서의 요점과 짧은 인용이다. 분석은 `docs/evta_systolic.md` §9.

## Xilinx WP486 "Deep Learning with INT8 Optimization on Xilinx Devices" (2016)
출처: https://www.edge-ai-vision.com/2016/11/deep-learning-with-int8-optimization-on-xilinx-devices/ (재게시본, 원 PDF는 받지 못함)
- "Pack 8-bit input a and b in the 27-bit port p of the DSP48E2 multiplier via the pre-adder". a는 18비트 왼쪽으로 shift, 공유 operand c는 18비트 포트.
- 45비트 곱 = a·c·2^18 + b·c. "a minimum of 16 + 8 = 24-bits total input size".
- "weight sharing instead of input sharing can also be used".
- "accumulation of up to 7 product terms only can be guaranteed with no overflow for the lower bits"; "After 7 product terms, an additional DSP slice is required".
- "8 DSP slices here perform 7×2 INT8 multiply-add operations" → 1.75배. unsigned INT8은 1.78배라고도 쓴다. 클럭 수치는 없다.

## AMD PG338 DPUCZDX8G, Clocking / Computation Clock
출처: https://docs.amd.com/r/3.4-English/pg338-dpu/Clocking , https://docs.amd.com/r/3.4-English/pg338-dpu/Computation-Clock
- "There are three clock domains in the DPUCZDX8G IP: the register configuration, the data controller, and the computation unit."
- "The DSP slices in the computation unit module are in the `dpu_2x_clk` domain, which runs at twice the clock frequency of the data controller module."
- "The two related clocks must be edge-aligned."
- 이번에 받은 페이지에는 주파수 수치가 없었다(325/650 MHz는 이전 세션의 확인, 다시 확인 필요).

## Tensil ZCU104 튜토리얼
- README의 링크 https://www.tensil.ai/docs/tutorials/resnet20-zcu104/ 는 2026-10-27 현재 관계없는 도메인으로 308 리다이렉트된다(따라가지 않음). web.archive.org는 접근 불가.

## Gemmini FPGA 설정 탐색 (Sensors 2023, PMC10007457)
- https://pmc.ncbi.nlm.nih.gov/articles/PMC10007457/ : 브라우저 확인 페이지만 받음. 검색 요약: FPGA에서 array size, 메모리, im2col을 바꿔 면적·클럭·전력을 측정, WS가 OS보다 약 3배 빠름.

## AutoSA 논문 (FPGA'21)
- http://cadlab.cs.ucla.edu/~jaywang/papers/fpga21-autosa.pdf : 60초 시간 초과로 받지 못함.
