#!/usr/bin/env python3
"""Resnet18 on EVTA, brief deck (6 slides) in the lab template (npu-slides skill).
#### JO added (2026-11-15): the main points of resnet18_optimization_short.pptx only.
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
d.cover(['Resnet18 EVTA 가속 최적화', '결과 보고'], '2026.11.15.')

# ---------------------------------------------------------------- 2 overview
s = d.slide('1', '개요', '목표 및 결과', side=('개요', '최적화 결과 요약'))
d.bar(s, 'ZCU102 EVTA에서 Resnet18 1장 추론 시간 6.4배 단축')
d.stat(s, 0.75, 1.5, 3.0, '148.7 → 23.3 ms', '1장 추론 시간 (1코어)', big_size=24)
d.stat(s, 3.95, 1.5, 3.0, '6.4배', 'conv0 EVTA 적용 시점 대비')
d.stat(s, 7.15, 1.5, 3.0, '160 GOPS', 'EVTA 호출 기준 (하한의 78 %)')
d.box(s, 0.65, 2.95, 9.6, 3.0, [
    (1, '대상 및 환경'),
    (2, ['Resnet18 (ImageNet), int8 양자화, ', ('ZCU102 FPGA + EVTA', 'blue'), ' (systolic 400 MHz)']),
    (2, 'NEST-C 컴파일러 → EVTA 실행 코드 생성, 단일 EVTA · CPU 1코어 기준'),
    (1, '주요 결과'),
    (2, ['컴파일러 / runtime 개선으로 ', ('약 122 ms 단축', 'blue'), ' (하드웨어 변경 없음)']),
    (2, ['ALU · requantize 하드웨어 개선으로 EVTA 시간 ', ('26.4 → 23.05 ms', 'blue')]),
    (2, ['모든 단계에서 출력 동일 확인 ', ('(층 출력 byte 비교, golden 30개, 분류 결과 281)', 'grey')]),
])
d.notes(s, '시작점은 conv0까지 EVTA로 보낸 10월 말 기준 148.7 ms. 처음 측정한 134.8 ms는 OMP 4코어 값이라 기준에서 뺐다.')

# ---------------------------------------------------------------- 3 starting point
s = d.slide('2', '현황 분석', '병목 구간', side=('현황', '최적화 전 시간 구성'))
d.bar(s, '최적화 전 1장 148.9 ms 중 EVTA 계산은 약 20 %')
d.chart(s, 0.6, 1.4, 5.6, 4.7, ['transpose (conv마다 2회)', 'elemadd / maxpool / relu (CPU)', 'EVTA conv (명령 생성 포함)',
                                 'fc, 전처리', 'conv0 입력 zero-fill', 'buffer 할당 / 해제'],
        [('ms', [59.6, 37.5, 32.2, 8.1, 6.5, 4.5])], highlight={2: 'orange'})
d.caption(s, '최적화 전 1장 시간 구성 (ms, 1코어)', 0.6, 6.15, 5.6)
d.box(s, 6.45, 1.45, 3.8, 5.0, [
    (1, '하드웨어는 이미 충분'),
    (2, ['큰 3×3 conv에서 ', ('190 GOPS', 'blue'), ' (이론치의 93 %)']),
    (1, '시간은 대부분 CPU 쪽'),
    (2, '층마다 layout 변환 2회'),
    (2, 'elemadd · pool · relu를 CPU에서 처리'),
    (2, 'conv마다 명령 생성 · 동기화'),
    (1, '개선 방향'),
    (2, ['GEMM 확장보다 ', ('CPU 작업 제거를 먼저', 'red', True)]),
], size1=14, size2=12)
d.notes(s, 'GEMM 32×32를 먼저 했다면 −24 %, CPU 일을 줄인 뒤에 하면 −47 %. 그래서 소프트웨어부터 정리했다.')

# ---------------------------------------------------------------- 4 software
s = d.slide('3', '최적화 내용', '컴파일러 · runtime', side=('최적화', '소프트웨어 개선 항목'))
d.bar(s, '하드웨어 변경 없이 약 122 ms 단축 (148.7 → 28.8 ms)')
d.table(s, 0.65, 1.45, 9.6, [
    ['구분', '주요 내용', '단축'],
    ['① 층 간 데이터 이동 제거', ['conv 출력을 EVTA layout 그대로 다음 층에 전달, buffer pool 재사용'], '−65.9 ms'],
    ['② CPU 연산의 EVTA 이전', ['conv + residual add + relu 단일 호출, maxpool · fc EVTA 처리'], '−43.2 ms'],
    ['③ 첫 conv 및 입력 처리', ['3채널 conv0를 4×1 packed conv로 변환, 전처리 · 입력 복사를 conv와 중첩'], '−9.5 ms'],
    ['④ runtime 명령 / DMA', ['명령 record / replay, 연속 DMA 묶음 처리'], '−3.5 ms'],
], col_w=[2.6, 5.8, 1.2], size=13, row_h=0.6, bold_cols=(0,))
d.chart(s, 0.65, 4.65, 9.6, 1.8, ['① 데이터 이동', '② CPU → EVTA', '③ conv0 · 입력', '④ runtime'],
        [('ms', [65.9, 43.2, 9.5, 3.5])], label_size=11, colors=('sky',))
d.notes(s, 'residual add는 scale이 2의 거듭제곱이라 shift만으로 EVTA ALU에서 처리. 매 단계 EVTA_RES_CHECK로 CPU 경로와 실행 중 비교해 불일치 0.')

# ---------------------------------------------------------------- 5 hardware + verification
s = d.slide('4', '최적화 내용', '하드웨어 · 검증', side=('최적화', '하드웨어 개선 및 검증'))
d.bar(s, '측정으로 병목(ALU II = 2)을 확인한 뒤 하드웨어 수정')
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
d.notes(s, '층 사이 DRAM 왕복을 모두 없애도 −1 ms(3 %)라 SRAM 안에서 층을 잇는 경로는 만들지 않았다. 4 EVTA 동시 실행 시 610 GOPS.')

# ---------------------------------------------------------------- 6 conclusion
s = d.slide('5', '결론', '향후 계획', side=('결론', '결론 및 향후 계획'))
d.bar(s, '결론')
d.box(s, 0.65, 1.4, 9.6, 2.2, [
    (2, ['성능은 하드웨어보다 ', ('runtime 처리 단위', 'blue', True), '가 좌우 → 하드웨어 그대로 148.7 → 28.8 ms']),
    (2, ['변경 전 상한 측정으로 효과 없는 작업 배제 ', ('(DRAM 왕복 제거: 상한 1 ms)', 'grey')]),
    (2, '검증 도구를 먼저 갖추고 모든 단계에서 출력 동일성 확인'),
], size2=14, line=1.6)
d.bar(s, '향후 계획', y=3.95)
d.box(s, 0.65, 4.5, 9.6, 2.2, [
    (2, ['1장을 EVTA 4개로 분할 (출력 채널 분할) → ', ('23 → 7~9 ms 예상', 'blue', True)]),
    (2, 'GEMM 32×32 / 클럭 2배 검토 (ISA · SRAM · backend 변경 필요)'),
    (2, ['동일 방법 타 모델 적용 중: ', ('Resnet50 68.8 ms, MobileNetV2 17.6 ms, DeiT-tiny 47.0 ms', 'blue')]),
], size2=14, line=1.6)
d.notes(s, '자세한 내용: docs/resnet18_optimization.md, 전체판 docs/slides/resnet18_optimization.pptx (34장), 요약판 _short (10장).')

print(d.save(OUT))
