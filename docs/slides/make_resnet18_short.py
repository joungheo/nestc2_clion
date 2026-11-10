#!/usr/bin/env python3
"""Resnet18 on EVTA optimization, short deck (10 slides): similar steps grouped, the main points only.
#### JO added (2026-11-10): same helpers and figures as make_resnet18_deck.py (its part before the slides is reused).
#### usage: <venv with python-pptx, cairosvg>/bin/python docs/slides/make_resnet18_short.py [out.pptx]
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(HERE, 'make_resnet18_deck.py'), encoding='utf-8').read()
head = src[:src.index('# ---------------------------------------------------------------- 1 title')]
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'resnet18_optimization_short.pptx')
sys.argv = sys.argv[:1]
exec(compile(head.replace("OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'resnet18_optimization.pptx')", ''),
             'make_resnet18_deck.py', 'exec'))


def group(t, sub, png, rows, total, note):
    """A group slide: figure left, the steps of the group (before -> after) and the total right."""
    s = prs.slides.add_slide(BLANK)
    bg(s, WHITE)
    title(s, t, sub)
    pic = picture(s, png, 0.4, 1.5, 7.9, 5.65)
    ph = max(pic.height / 914400, 5.0)
    rect(s, 8.55, 1.5, 4.35, ph, LIGHT, name='Panel')
    text(s, 8.75, 1.62, 4.0, 0.4, ['1장 (1코어)에서 줄어든 시간'], size=13, color=GREY)
    text(s, 8.75, 1.95, 4.0, 0.8, [[('−' + total, {'color': TEAL}), (' ms', {'size': 20, 'color': TEAL})]], size=40, bold=True)
    y = 2.95
    for name, ba, d in rows:
        text(s, 8.75, y, 4.0, 0.35, [[(name, {'bold': True, 'color': DARK})]], size=14)
        text(s, 8.75, y + 0.33, 4.0, 0.62, [[(ba, {'color': GREY}), ('   ' + d, {'color': TEXT})]], size=13)
        y += 0.98
    notes(s, note)
    return s


# ---------------------------------------------------------------- 1 title
s = prs.slides.add_slide(BLANK)
bg(s, DARK)
text(s, 0.8, 1.6, 11.8, 1.0, ['Resnet18을 EVTA에서 빠르게'], size=44, bold=True, color=WHITE)
text(s, 0.8, 2.6, 11.8, 0.6, ['ZCU102 · EVTA(VTA 계열 int8 NPU) · NEST-C, 2026-10-23 ~ 11-10'], size=20, color=TEAL_L)
for i, (big, lab) in enumerate([('134.8 → 23.3 ms', '1장 실행 시간'), ('6.4배', '148.7 → 23.3 ms (1코어, conv0 EVTA 이후 기준)'),
                                ('160 GOPS', 'EVTA 호출, 하한 17.7 ms의 78 %')]):
    x = 0.8 + i * 4.0
    text(s, x, 4.3, 3.8, 0.8, [big], size=32, bold=True, color=WHITE)
    text(s, x, 5.05, 3.7, 0.8, [lab], size=14, color=TEAL_L)
text(s, 0.8, 6.6, 11.8, 0.4, ['모든 단계에서 결과는 그대로: EVTA 출력 byte 비교, golden 30개, 분류 281'], size=13, color=TEAL_L)

# ---------------------------------------------------------------- 2 overview
s = prs.slides.add_slide(BLANK)
bg(s, WHITE)
title(s, '한눈에: 무엇이 얼마나 줄였나', '1장 148.7 → 23.3 ms (single EVTA, 1코어, systolic 400 MHz). 비슷한 단계를 다섯 묶음으로')
ch = bar_chart(s, 0.4, 1.45, 7.9, 5.6, ['① 층 사이 데이터 이동 없애기', '② CPU 연산을 EVTA로', '③ conv0와 입력',
                                       '④ runtime (replay, DMA)', '⑤ 하드웨어 (ALU, bias, requantize)'],
               [65.9, 43.2, 9.5, 3.5, 3.5], '줄어든 시간 (ms)', size=14)
ch.plots[0].series[0].points[4].format.fill.solid()
ch.plots[0].series[0].points[4].format.fill.fore_color.rgb = ORANGE
rect(s, 8.6, 1.5, 4.3, 5.55, LIGHT, name='Panel')
text(s, 8.8, 1.65, 3.95, 5.3, [
    [('소프트웨어가 대부분', {'bold': True, 'color': TEAL})], '①–④ 컴파일러와 runtime 변경으로 약 122 ms. 하드웨어는 그대로',
    [('하드웨어는 마지막에', {'bold': True, 'color': ORANGE})], '측정으로 원인(ALU II = 2)을 확인한 뒤 EVTA 26.4 → 23.05 ms',
    [('지금', {'bold': True, 'color': DARK})], 'EVTA 호출 22.65 ms, CPU 1 ms 아래. 시간은 EVTA가 정한다'],
     size=14, space=7)
notes(s, '묶음별 합은 단계마다 잰 1장 시간 차이를 더한 값이라, 측정 날짜가 다른 만큼 합이 정확히 125.4 ms가 되지는 않는다(약 125.6 ms).')

# ---------------------------------------------------------------- 3 start
s = prs.slides.add_slide(BLANK)
bg(s, WHITE)
title(s, '출발점: EVTA가 계산하는 시간은 20 %뿐이었다', '10-29, systolic 400 MHz, 1장 148.9 ms (1코어)')
bar_chart(s, 0.4, 1.45, 7.6, 5.6, ['transpose (conv마다 두 번)', 'elemadd / maxpool / relu (CPU)', 'EVTA conv (+ 명령 생성)',
                                  'fc, 전처리', 'conv0 입력 zerofill', 'Buffer Alloc / Free'],
          [59.6, 37.5, 32.2, 8.1, 6.5, 4.5], '1장 148.9 ms의 구성 (ms)', color=ORANGE, size=14)
rect(s, 8.4, 1.5, 4.5, 5.55, LIGHT, name='Panel')
text(s, 8.6, 1.65, 4.1, 5.3, [
    [('DAE 하드웨어는 이미 잘 돈다', {'bold': True, 'color': TEAL})], '큰 3×3 conv에서 190 GOPS (이론의 93 %)',
    [('손해는 runtime의 단위', {'bold': True, 'color': ORANGE})], '"conv 하나 = 함수 하나 = 동기화 한 번 = layout 왕복 한 번"',
    [('그래서 순서를 정했다', {'bold': True, 'color': DARK})], 'GEMM 32×32보다 CPU 일을 먼저. 32×32를 먼저 하면 −24 %, 나중에 하면 −47 %'],
     size=14, space=7)
notes(s, '이 전에 conv0(3채널)을 16채널로 0 채워 EVTA로 보냈다(131.8 → 78.6 ms, OMP 4, 10-28). GEMM은 RTL systolic 어레이로 바꿔 333 → 400 MHz.')

# ---------------------------------------------------------------- 4–8 groups
group('① 층 사이 데이터 이동 없애기', 'conv 출력을 EVTA layout 그대로 다음 conv에', 'evta_fig03_activation_layout',
      [('층 사이 EVTA layout 유지', '148.7 → 91.1', 'NHWC 왕복 transpose 없음'),
       ('남은 transpose를 16 byte 복사로', '52.5 → 46.9', 'byte 루프 → 16 byte'),
       ('VTA buffer pool', '47.0 → 44.3', 'cma_alloc ioctl 없음')], '65.9',
      'EVTA layout N{C/16}HW16은 채널 수가 바뀌어도 블록 수만 다르다. conv 출력 → NHWC → 다음 conv 입력의 두 transpose는 항등 변환이었다. '
      'VTASave findPackedActivations(-vta-keep-layout). buffer pool은 vtalib api.cc(EVTA_BUF_POOL_MB).')
group('② CPU 연산을 EVTA로', '2-bit ALU(MIN / MAX / ADD / SHR)와 단위행렬 GEMM만으로', 'evta_fig04_residual_block',
      [('conv + elemadd + relu 한 호출', '90.9 → 67.1', 'residual을 단위 GEMM으로'),
       ('maxpool을 ALU MAX로', '67.1 → 52.5', '입력 ≥ 0일 때'),
       ('fc: CPU NEON → EVTA', '44.3 → 39.9', 'weight를 compile time에 배치'),
       ('conv0 + maxpool 한 호출', '29.19 → 28.83', 'pool halo 재계산')], '43.2',
      'scale이 2의 거듭제곱이라 곱셈 없이 shift로 된다. 반올림 차이는 음수에서만 나서 relu가 붙은 add만 합친다. EVTA_RES_CHECK=1로 CPU 경로와 실행 중 비교, 0 불일치.')
group('③ conv0와 입력', '채널이 3개뿐인 첫 conv와 전처리', 'evta_fig05_conv0_pack',
      [('전처리를 입력 복사 하나로', '39.9 → 37.1', '값 256 × 채널 표, NEON'),
       ('conv0을 packed 4×1 conv로', '34.5 → 29.2', '0을 곱하던 13/16 제거'),
       ('입력 복사를 conv와 겹치기', '25.43 → 24.08', 'run i 동안 행 i+1')], '9.5',
      'conv0 7×7/2 3채널을 stride 2의 행 두 개와 kw 7개를 채널로 접어 4×1/1 48채널 conv로. 정수 합이 같아 출력이 같다(dump 40개 byte 동일). '
      '입력 복사 1.65 ms는 NEON 명령 수가 한계라 줄이지 않고 숨겼다(RunStart / RunWait, run마다 hook).')
group('④ runtime: 명령과 DMA', 'conv마다 CPU가 명령을 다시 만들던 것, 짧은 DMA', 'evta_fig09_evta_call_runtime',
      [('명령 record / replay', '37.1 → 34.5', '첫 호출을 기록, 주소만 고침'),
       ('flush 뒤 dsb sy', '멈춤 400회에 1번 → 0', 'multi4 안정'),
       ('conv DMA 묶기', '24.08 → 23.27', '연속 행을 한 행으로')], '3.5',
      'replay는 AOT 0단계(컴파일 때 명령 생성의 시험대). dsb는 libcma flush에 없던 것으로, EVTA 시작 MMIO가 명령의 DRAM 도착보다 먼저 나가 multi4에서 멈췄다. '
      'DMA 묶기는 Resnet50에서 −12.6 ms로 훨씬 컸다.')

s = prs.slides.add_slide(BLANK)
bg(s, WHITE)
title(s, '⑤ 하드웨어: 측정으로 원인을 확인한 뒤 고쳤다', 'EVTA 26.47 ms = GEMM 18.85 + ALU 4.63(II = 2) + 나머지 2.99 (11-02)')
picture(s, 'evta_fig13_alu_ii', 0.4, 1.5, 7.9, 5.65)
bar_chart(s, 8.5, 1.45, 4.45, 3.3, ['기본 400 MHz', 'ALU imm II = 1', 'bias reset', 'requantize'], [26.43, 24.89, 24.15, 23.05],
          'Resnet18 EVTA (ms)', color=ORANGE, fmt='0.00', size=13)
text(s, 8.6, 4.85, 4.3, 2.3, [
    [('안 만든 것도 측정으로', {'bold': True, 'color': TEAL})], '층 사이 DRAM 왕복을 다 없애도 −1 ms(3 %) → SRAM 안에서 층을 잇는 경로는 만들지 않음',
    [('ISA는 남는 비트로', {'bold': True, 'color': ORANGE})], 'bias_reset, rq_* 필드. 비트스트림 이름으로 켜서 같은 실행 파일로 비교'],
     size=13, space=5)
notes(s, 'ALU imm 연산은 acc를 하나만 읽으므로 II = 1 루프를 따로 둔다(WNS +0.001 ns). bias reset은 GEMM 시작값을 bias로. requantize는 out 쓰기 길에 '
         'clip((acc + round) >> shift)을 넣어 SHR / MAX / MIN 명령을 없앴다. 각각 C 모델 → RTL testbench → CSim → 보드 순서로 검증.')

# ---------------------------------------------------------------- 9 multi4 + correctness
s = prs.slides.add_slide(BLANK)
bg(s, WHITE)
title(s, 'EVTA 4개에서도, 결과는 매 단계 그대로', 'multi4: copy 4개를 EVTA 4개에서 동시에 (copy마다 1코어)')
bar_chart(s, 0.4, 1.45, 6.3, 5.6, ['시작 (10-29)', 'layout 유지', 'residual fusion', 'maxpool EVTA', 'transpose / pool / fc',
                                  '전처리, replay', 'conv0 pack + pool'], [3909, 2793, 1851, 1471, 1110, 977.9, 820.3],
          'multi4 100장 처리 시간 (ms)', fmt='0', size=13)
stat(s, 7.0, 1.5, 2.8, '4.8배', '100장 3.9 → 0.82 s')
stat(s, 10.0, 1.5, 2.9, '610 GOPS', '하드웨어 뒤 4 EVTA 동시')
rect(s, 7.0, 3.1, 5.9, 3.95, LIGHT, name='Panel')
text(s, 7.2, 3.2, 5.5, 3.8, [
    [('검증 도구를 먼저 만들었다', {'bold': True, 'color': TEAL})],
    'EVTA_DUMP_CONV(층 출력 dump), EVTA_RES_CHECK(CPU 경로와 실행 중 비교), golden 30개',
    [('그래서 찾은 결함 8개', {'bold': True, 'color': ORANGE})],
    'flush 뒤 dsb 누락(multi4 멈춤), 명령 pad 비트, vt 2 더미 tile, bias가 다른 thread의 acc를 덮음, '
    'vt가 블록 수를 못 나누면 계산 없음 등'], size=14, space=7)
notes(s, '4 copy를 같이 돌리면 DDR controller 공유로 copy마다 약 1 ms 느려진다(HP1 + HP2 쌍은 조금 더).')

# ---------------------------------------------------------------- 10 now / next
s = prs.slides.add_slide(BLANK)
bg(s, DARK)
title(s, '지금 23.3 ms, 다음은', '배운 것과 다음 후보', dark=True)
for i, (h, d) in enumerate([('runtime 단위가 성능을 정했다', '하드웨어 그대로 148.7 → 28.8 ms'),
                            ('바꾸기 전에 쟀다', 'DRAM 상한 1 ms → 안 만듦, ALU II → 고침'),
                            ('ISA는 남는 비트로', '이전 비트스트림과 같은 실행 파일로 비교'),
                            ('검증 도구를 먼저', '모든 단계를 byte 단위로 비교')]):
    x = 0.6 + (i % 2) * 3.05
    y = 1.7 + (i // 2) * 1.75
    rect(s, x, y, 2.85, 1.55, TEAL, name=f'Lesson{i}')
    text(s, x + 0.15, y + 0.12, 2.6, 0.7, [h], size=15, bold=True, color=WHITE)
    text(s, x + 0.15, y + 0.8, 2.6, 0.7, [d], size=12, color=TEAL_L)
text(s, 6.9, 1.7, 6.0, 4.5, [
    [('한 장을 EVTA 4개로 나누기', {'bold': True, 'color': WHITE})], [('출력 채널 분할, 1장 23 → 7–9 ms 예상', {'color': TEAL_L})],
    [('여러 층을 한 명령 stream으로', {'bold': True, 'color': WHITE})], [('구간 제출, 약 −1 ms', {'color': TEAL_L})],
    [('GEMM 32×32 / 2배 클럭', {'bold': True, 'color': WHITE})], [('EVTA 약 2.5배, ISA / SRAM / backend 변경', {'color': TEAL_L})],
    [('같은 방법을 다른 모델로', {'bold': True, 'color': WHITE})], [('Resnet50 69.6 ms, MobileNetV2 18.7 ms, DeiT 88.5 ms', {'color': TEAL_L})]],
     size=16, space=4)
text(s, 0.6, 6.85, 12.2, 0.4, ['자세히: docs/resnet18_optimization.md, 전체 판(34장): docs/slides/resnet18_optimization.pptx'], size=12, color=TEAL_L)

prs.save(OUT)
print(OUT, len(prs.slides._sldIdLst), 'slides')
