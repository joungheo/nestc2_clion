#!/usr/bin/env python3
"""Resnet18 on EVTA, brief deck (8 slides) in the lab template (npu-slides skill).
#### JO added (2026-11-15): the main points of resnet18_optimization_short.pptx only, plus EVTA x 4 and the other models.
#### usage: ~/.venvs/slides/bin/python docs/slides/make_resnet18_brief.py [out.pptx]
"""
import os
import sys

sys.path.insert(0, os.path.expanduser('~/.claude/skills/npu-slides/scripts'))
from npuslides import Deck  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'resnet18_optimization_brief.pptx')

d = Deck()

# ---------------------------------------------------------------- 1 cover
d.cover(['EVTA NPU 가속 최적화', '결과 보고 (Resnet18 중심)'], '2026.11.15.')

# ---------------------------------------------------------------- 2 overview
s = d.slide('1', '개요', '목표 및 결과', side=('개요', '최적화 결과 요약'))
d.bar(s, 'Resnet18 1장 추론 6.4배 단축, EVTA 4개 동시 600 GOPS 이상')
d.stat(s, 0.75, 1.5, 3.0, '148.7 → 23.3 ms', 'Resnet18 1장 추론 (1코어)', big_size=24)
d.stat(s, 3.95, 1.5, 3.0, '6.4배', '컴파일러 · runtime · 하드웨어 개선')
d.stat(s, 7.15, 1.5, 3.0, '609.7 GOPS', 'EVTA 4개 동시 (이론치의 89 %)')
d.box(s, 0.65, 2.95, 9.6, 3.3, [
    (1, '대상 및 환경'),
    (2, ['ZCU102 FPGA + ', ('EVTA', 'blue'), ' (VTA 계열 int8 NPU, 16×16 GEMM), NEST-C 컴파일러로 실행 코드 생성']),
    (2, 'Resnet18로 기능 개발 후 Resnet50 · VGG16 · MobileNetV2 · DeiT-tiny에 적용'),
    (1, '주요 결과'),
    (2, ['컴파일러 / runtime 개선만으로 Resnet18 ', ('약 122 ms 단축', 'blue'), ' (하드웨어 변경 없음)']),
    (2, ['EVTA 4개 동시 ', ('609.7 GOPS', 'blue'), ', 5개 모델 모두 처음 측정 대비 ', ('2.3 ~ 80배', 'blue'), ' 단축']),
    (2, ['모든 단계에서 출력 동일 확인 ', ('(층 출력 byte 비교, golden 30개)', 'grey')]),
])
d.notes(s, 'Resnet18 기준점은 conv0까지 EVTA로 보낸 10월 말 148.7 ms (1코어). 처음 잰 134.8 ms는 4코어 값이라 기준에서 제외. '
           '609.7 GOPS는 ResConv10(3×3, 7×7×512 → 512)을 EVTA 4개가 동시에 실행한 처리량.')

# ---------------------------------------------------------------- 3 starting point
s = d.slide('2', '현황 분석', '병목 구간', side=('현황', '최적화 전 시간 구성'))
d.bar(s, '최적화 전 Resnet18 1장 148.9 ms 중 EVTA 계산은 약 20 %')
d.chart(s, 0.6, 1.4, 5.6, 4.7, ['transpose (conv마다 2회)', 'elemadd / maxpool / relu (CPU)', 'EVTA conv (명령 생성 포함)',
                                 'fc, 전처리', 'conv0 입력 zero-fill', 'buffer 할당 / 해제'],
        [('ms', [59.6, 37.5, 32.2, 8.1, 6.5, 4.5])], highlight={2: 'orange'})
d.caption(s, '최적화 전 1장 시간 구성 (ms, 1코어)', 0.6, 6.15, 5.6)
d.box(s, 6.45, 1.45, 3.8, 5.0, [
    (1, '하드웨어 성능은 충분'),
    (2, ['큰 3×3 conv에서 ', ('190 GOPS', 'blue'), ' (이론치의 93 %)']),
    (1, '시간 대부분이 CPU 쪽'),
    (2, '층마다 layout 변환 2회'),
    (2, 'elemadd · pool · relu CPU 처리'),
    (2, 'conv마다 명령 생성 · 동기화'),
    (1, '개선 방향'),
    (2, ['GEMM 확장보다 ', ('CPU 작업 제거 우선', 'red', True)]),
], size1=14, size2=12)
d.notes(s, 'GEMM 32×32를 먼저 했다면 −24 %, CPU 일을 줄인 뒤에 하면 −47 %. 그래서 소프트웨어부터 정리했다.')

# ---------------------------------------------------------------- 4 software
s = d.slide('3', '최적화 내용', '컴파일러 · runtime', side=('최적화', '소프트웨어 개선 항목'))
d.bar(s, '하드웨어 변경 없이 약 122 ms 단축 (148.7 → 28.8 ms)')
d.table(s, 0.65, 1.45, 9.6, [
    ['구분', '주요 내용', '단축'],
    ['① 층 간 데이터 이동 제거', ['conv 출력을 EVTA layout 그대로 다음 층에 전달, buffer 재사용'], '−65.9 ms'],
    ['② CPU 연산의 EVTA 이전', ['conv + residual add + relu 단일 호출, maxpool · fc EVTA 처리'], '−43.2 ms'],
    ['③ 첫 conv 및 입력 처리', ['3채널 conv0를 packed conv로 변환, 입력 복사를 conv와 중첩'], '−9.5 ms'],
    ['④ runtime 명령 / DMA', ['명령 record / replay, 연속 DMA 묶음 처리'], '−3.5 ms'],
], col_w=[2.6, 5.8, 1.2], size=13, row_h=0.6, bold_cols=(0,))
d.chart(s, 0.65, 4.65, 9.6, 1.8, ['① 데이터 이동', '② CPU → EVTA', '③ conv0 · 입력', '④ runtime'],
        [('ms', [65.9, 43.2, 9.5, 3.5])], label_size=11, colors=('sky',))
d.notes(s, 'residual add는 scale이 2의 거듭제곱이라 shift만으로 EVTA ALU에서 처리. 매 단계 EVTA_RES_CHECK로 CPU 경로와 실행 중 비교해 불일치 0.')

# ---------------------------------------------------------------- 5 hardware + verification
s = d.slide('4', '최적화 내용', '하드웨어 · 검증', side=('최적화', '하드웨어 개선 및 검증'))
d.bar(s, '측정으로 병목(ALU II = 2) 확인 후 하드웨어 수정')
d.chart(s, 0.6, 1.4, 4.9, 3.9, ['기본 (400 MHz)', 'ALU imm II = 1', 'bias reset', 'requantize'],
        [('ms', [26.43, 24.89, 24.15, 23.05])], number_format='0.00', highlight={3: 'orange'})
d.caption(s, 'Resnet18 EVTA 시간 (ms)', 0.6, 5.35, 4.9)
d.box(s, 5.75, 1.45, 4.5, 3.9, [
    (1, 'ALU / requantize 개선'),
    (2, 'ALU 즉시값 연산 II 2 → 1'),
    (2, 'conv 시작값을 bias로 (ADD 제거)'),
    (2, 'out 쓰기에서 requantize (명령 3개 제거)'),
    (1, 'ISA 확장 방식'),
    (2, '명령의 남는 비트 사용, 기존 실행 파일 호환'),
], size1=14, size2=12)
d.box(s, 0.65, 5.75, 9.6, 0.85, [
    (0, [('검증  ', 'red', True), '모든 단계 층 출력 byte 비교 · golden 30개 · CPU 경로 실행 중 비교 → ', ('결함 8건 사전 발견', 'blue', True)]),
], size2=13, line=1.2)
d.notes(s, '층 사이 DRAM 왕복을 모두 없애도 −1 ms(3 %)라 SRAM 안에서 층을 잇는 경로는 만들지 않았다.')

# ---------------------------------------------------------------- 6 EVTA x 4
s = d.slide('5', '확장', 'EVTA 4개 동시 실행', side=('확장', '다중 EVTA 성능'))
d.bar(s, 'EVTA 4개 동시 실행으로 600 GOPS 돌파 (최대 685 GOPS)')
d.stat(s, 0.75, 1.45, 2.95, '609.7 GOPS', 'EVTA 4개, 333 MHz (이론치 89 %)')
d.stat(s, 3.9, 1.45, 2.95, '685 GOPS', 'GEMM 380 MHz로 상향 (+12 %)')
d.stat(s, 7.05, 1.45, 2.95, '4.8배', 'Resnet18 100장 3.9 → 0.82 s')
d.chart(s, 0.6, 2.85, 5.4, 3.55, ['시작 (10-29)', 'layout 유지', 'residual 융합', 'maxpool EVTA', 'transpose · pool · fc',
                                  '전처리 · replay', 'conv0 pack + pool'],
        [('s', [3.909, 2.793, 1.851, 1.471, 1.110, 0.978, 0.820])], number_format='0.00', highlight={6: 'orange'},
        label_size=10)
d.caption(s, 'Resnet18 100장 처리 시간 (s, EVTA 4개)', 0.6, 6.42, 5.4)
d.box(s, 6.25, 2.9, 4.0, 3.5, [
    (1, '동시 실행해도 성능 유지'),
    (2, 'EVTA 1개 152.6 → 4개 609.7 GOPS'),
    (2, 'EVTA당 시간 증가 0.12 %'),
    (2, 'HP 포트 · DDR 경쟁 없음'),
    (1, '보드 문제 해결'),
    (2, 'PL 결함 행(Y219) 우회 배치'),
    (2, 'flush 후 dsb 누락 → 멈춤 해결'),
], size1=14, size2=12)
d.notes(s, '609.7 GOPS: ResConv10(3×3 conv, 7×7×512 → 512)을 EVTA 4개가 각각 400번 동시 실행한 처리량, 333 MHz, 출력 golden 일치. '
           '380 MHz(HP 포트는 333 MHz로 분리)에서 685.1 GOPS. 100장 처리는 Resnet18 copy 4개를 EVTA 4개에서 동시에(copy마다 1코어).')

# ---------------------------------------------------------------- 7 other models
s = d.slide('6', '적용 결과', '모델별 성능', side=('결과', '모델별 처리 시간'))
d.bar(s, '같은 컴파일러 · runtime 기능으로 5개 모델 모두 2.3 ~ 80배 단축')
d.table(s, 0.65, 1.45, 9.6, [
    ['모델', '처음 측정', '현재', '단축', '주요 내용'],
    ['Resnet18', '148.7 ms', '23.1 ms', '6.4배', ['층 간 이동 제거, 연산 EVTA 이전']],
    ['Resnet50', '160.3 ms', '68.8 ms', '2.3배', ['bottleneck residual 융합, DMA 묶음']],
    ['VGG16 (CIFAR-10)', '16.1 ms', '7.0 ms', '2.3배', ['작은 tile 지원, 층별 스케줄 조정']],
    ['MobileNetV2', '1,048 ms', '17.6 ms', '60배', ['depthwise conv EVTA 처리, 채널별 양자화']],
    ['DeiT-tiny', '3,770 ms', '47.0 ms', '80배', ['matmul → conv, LayerNorm · softmax EVTA']],
], col_w=[2.05, 1.15, 1.05, 0.85, 4.5], size=14, row_h=0.75, bold_cols=(0, 3))
d.caption(s, '단일 EVTA, CPU 1코어 기준 1장 추론 시간 (2026-11-15)', 0.65, 6.2, 9.6)
d.notes(s, '모두 단일 EVTA, CPU 1코어, 2026-11-15 기준 (Resnet18 23.1 ms는 ALU 확장 400 MHz 비트스트림; 앞 장들의 23.3 ms는 11-10 값). '
           '처음 측정: Resnet50 · VGG16은 Resnet18 기능이 들어간 상태(11-05), MobileNetV2는 11-07(분류도 오답), '
           'DeiT-tiny는 11-09(matmul이 CPU float). DeiT-tiny ImageNetV2 정확도 60.2 % (float 62.3 %).')

# ---------------------------------------------------------------- 8 conclusion
s = d.slide('7', '결론', '향후 계획', side=('결론', '결론 및 향후 계획'))
d.bar(s, '결론')
d.box(s, 0.65, 1.4, 9.6, 2.2, [
    (2, ['성능은 하드웨어보다 ', ('runtime 처리 단위', 'blue', True), '가 좌우 (하드웨어 그대로 148.7 → 28.8 ms)']),
    (2, ['변경 전 상한 측정으로 효과 없는 작업 배제 ', ('(DRAM 왕복 제거: 상한 1 ms)', 'grey')]),
    (2, '검증 도구를 먼저 갖추고 모든 단계에서 출력 동일성 확인'),
], size2=14, line=1.6)
d.bar(s, '향후 계획', y=3.95)
d.box(s, 0.65, 4.5, 9.6, 2.2, [
    (2, ['1장을 EVTA 4개로 분할 (출력 채널 분할) → ', ('Resnet18 23 → 7~9 ms 예상', 'blue', True)]),
    (2, 'DeiT-tiny 남은 CPU 작업(table 변환 · transpose)의 EVTA 이전'),
    (2, 'GEMM 32×32 / 클럭 2배 검토 (ISA · SRAM · backend 변경 필요)'),
], size2=14, line=1.6)
d.notes(s, '자세한 내용: docs/resnet18_optimization.md, docs/evta_multi4_609gops_report.md, 모델별 docs/*_optimization.md. '
           '전체판 docs/slides/resnet18_optimization.pptx (34장), 요약판 _short (10장).')

print(d.save(OUT))
