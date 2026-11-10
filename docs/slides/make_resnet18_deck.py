#!/usr/bin/env python3
"""Resnet18 on EVTA optimization deck (from docs/resnet18_optimization.md and docs/figs)."""
import os, sys
from pptx import Presentation
from pptx.util import Inches, Pt, Emu
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE
from pptx.chart.data import CategoryChartData
from pptx.enum.chart import XL_CHART_TYPE, XL_LABEL_POSITION
from pptx.oxml.ns import qn
from PIL import Image

#### JO added (2026-11-10): the Resnet18 optimization deck (docs/resnet18_optimization.md as slides, the figures of docs/figs).
#### Needs python-pptx, cairosvg, Pillow (a venv: python3 -m venv v && v/bin/pip install python-pptx cairosvg).
#### usage: <venv>/bin/python docs/slides/make_resnet18_deck.py [out.pptx]   (default docs/slides/resnet18_optimization.pptx)
import glob, tempfile
import cairosvg
HERE = os.path.dirname(os.path.abspath(__file__))
FIGS = os.path.join(HERE, '..', 'figs')
PNG = tempfile.mkdtemp(prefix='r18deck_')
for f in glob.glob(os.path.join(FIGS, 'evta_fig*.svg')) + glob.glob(os.path.join(FIGS, 'evta_st*.svg')):
    cairosvg.svg2png(url=f, write_to=os.path.join(PNG, os.path.basename(f)[:-4] + '.png'), output_width=1800,
                     background_color='white')
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'resnet18_optimization.pptx')

# palette: deep slate (dominant), teal (EVTA / improvement), orange (before / CPU), light tint
DARK = RGBColor(0x1B, 0x26, 0x3B)
TEAL = RGBColor(0x0E, 0x7C, 0x86)
TEAL_L = RGBColor(0xD8, 0xEE, 0xEF)
ORANGE = RGBColor(0xD9, 0x6C, 0x2B)
GREY = RGBColor(0x5B, 0x63, 0x70)
LIGHT = RGBColor(0xF3, 0xF5, 0xF7)
WHITE = RGBColor(0xFF, 0xFF, 0xFF)
TEXT = RGBColor(0x1F, 0x24, 0x2D)
LATIN, EA = 'Calibri', 'Malgun Gothic'

prs = Presentation()
prs.slide_width, prs.slide_height = Inches(13.333), Inches(7.5)
BLANK = prs.slide_layouts[6]
SW, SH = 13.333, 7.5


def fonts(run, size, bold=False, color=TEXT, italic=False):
    f = run.font
    f.size, f.bold, f.italic, f.name = Pt(size), bold, italic, LATIN
    f.color.rgb = color
    rpr = run._r.get_or_add_rPr()
    for tag in ('a:ea', 'a:cs'):
        el = rpr.find(qn(tag))
        if el is None:
            el = rpr.makeelement(qn(tag), {})
            rpr.append(el)
        el.set('typeface', EA)


def text(slide, x, y, w, h, paras, size=16, color=TEXT, bold=False, align=PP_ALIGN.LEFT, anchor=MSO_ANCHOR.TOP,
         space=6, bullet=False, name=None):
    """paras: list of str or list of (str, dict) runs."""
    tb = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
    if name:
        tb.name = name
    tf = tb.text_frame
    tf.word_wrap = True
    tf.margin_left = tf.margin_right = Inches(0.05)
    tf.margin_top = tf.margin_bottom = Inches(0.03)
    tf.vertical_anchor = anchor
    for i, p in enumerate(paras):
        para = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        para.alignment = align
        para.space_after = Pt(space)
        runs = p if isinstance(p, list) else [(p, {})]
        if bullet:
            pPr = para._p.get_or_add_pPr()
            pPr.set('marL', str(Emu(Inches(0.22))))
            pPr.set('indent', str(-Emu(Inches(0.2))))
            bu = pPr.makeelement(qn('a:buChar'), {'char': '•'})
            pPr.append(bu)
        for t, o in runs:
            r = para.add_run()
            r.text = t
            fonts(r, o.get('size', size), o.get('bold', bold), o.get('color', color), o.get('italic', False))
    return tb


def title(slide, t, sub=None, dark=False):
    c = WHITE if dark else DARK
    text(slide, 0.5, 0.3, 12.3, 0.7, [t], size=30, bold=True, color=c, name='Title')
    if sub:
        text(slide, 0.5, 0.95, 12.3, 0.45, [sub], size=15, color=(TEAL_L if dark else GREY), name='Subtitle')


def bg(slide, color):
    f = slide.background.fill
    f.solid()
    f.fore_color.rgb = color


def rect(slide, x, y, w, h, fill, line=None, shape=MSO_SHAPE.ROUNDED_RECTANGLE, name=None):
    s = slide.shapes.add_shape(shape, Inches(x), Inches(y), Inches(w), Inches(h))
    s.fill.solid()
    s.fill.fore_color.rgb = fill
    if line is None:
        s.line.fill.background()
    else:
        s.line.color.rgb = line
    if shape == MSO_SHAPE.ROUNDED_RECTANGLE:
        s.adjustments[0] = 0.06
    s.shadow.inherit = False
    if name:
        s.name = name
    return s


def picture(slide, png, x, y, maxw, maxh, align='center'):
    im = Image.open(os.path.join(PNG, png + '.png'))
    ar = im.width / im.height
    w = maxw
    h = w / ar
    if h > maxh:
        h = maxh
        w = h * ar
    px = x + (maxw - w) / 2 if align == 'center' else x
    pic = slide.shapes.add_picture(os.path.join(PNG, png + '.png'), Inches(px), Inches(y), Inches(w), Inches(h))
    pic.name = png
    return pic


def stat(slide, x, y, w, big, label, color=TEAL, bigsize=40):
    text(slide, x, y, w, 0.8, [big], size=bigsize, bold=True, color=color)
    text(slide, x, y + 0.78, w, 0.6, [label], size=13, color=GREY)


def notes(slide, t):
    slide.notes_slide.notes_text_frame.text = t


def step(t, png, before, after, unit, label, points, note, sub=None, after_color=TEAL):
    """A step slide: big figure left, before -> after callout and three short points right."""
    s = prs.slides.add_slide(BLANK)
    bg(s, WHITE)
    title(s, t, sub)
    top = 1.5 if sub else 1.15
    pic = picture(s, png, 0.4, top, 9.3, 7.5 - top - 0.35)
    ph = max(pic.height / 914400, 4.6)
    x = 9.95
    rect(s, x - 0.15, top, 3.25, ph, LIGHT, name='Panel')
    text(s, x, top + 0.15, 3.0, 0.4, [label], size=13, color=GREY)
    text(s, x, top + 0.5, 3.0, 0.6, [[(before, {'color': ORANGE}), (' →', {'color': GREY, 'size': 22})]], size=28, bold=True)
    text(s, x, top + 1.05, 3.0, 0.8, [[(after, {'color': after_color}), (' ' + unit, {'size': 18, 'color': after_color})]], size=40, bold=True)
    text(s, x, top + 2.05, 2.95, ph - 2.2, points, size=14, bullet=True, space=8)
    notes(s, note)
    return s


def section(t, sub):
    s = prs.slides.add_slide(BLANK)
    bg(s, DARK)
    text(s, 0.8, 2.7, 11.5, 1.0, [t], size=40, bold=True, color=WHITE)
    text(s, 0.8, 3.75, 11.5, 0.8, [sub], size=18, color=TEAL_L)
    return s


def table(slide, x, y, w, rows, colw, size=13, rowh=0.36, head=DARK):
    nr, nc = len(rows), len(rows[0])
    gt = slide.shapes.add_table(nr, nc, Inches(x), Inches(y), Inches(w), Inches(rowh * nr)).table
    for j, cw in enumerate(colw):
        gt.columns[j].width = Inches(cw)
    for i, row in enumerate(rows):
        gt.rows[i].height = Inches(rowh)
        for j, v in enumerate(row):
            cell = gt.cell(i, j)
            cell.margin_left = cell.margin_right = Inches(0.06)
            cell.margin_top = cell.margin_bottom = Inches(0.02)
            tf = cell.text_frame
            tf.word_wrap = True
            p = tf.paragraphs[0]
            r = p.add_run()
            r.text = str(v)
            fonts(r, size, bold=(i == 0), color=(WHITE if i == 0 else TEXT))
            cell.fill.solid()
            cell.fill.fore_color.rgb = head if i == 0 else (LIGHT if i % 2 else WHITE)
    return gt


def bar_chart(slide, x, y, w, h, cats, vals, title_txt, color=TEAL, horizontal=True, fmt='0.0', size=12):
    cd = CategoryChartData()
    cd.categories = cats
    cd.add_series('ms', vals)
    ct = XL_CHART_TYPE.BAR_CLUSTERED if horizontal else XL_CHART_TYPE.COLUMN_CLUSTERED
    gf = slide.shapes.add_chart(ct, Inches(x), Inches(y), Inches(w), Inches(h), cd)
    ch = gf.chart
    ch.has_legend = False
    ch.has_title = True
    ch.chart_title.text_frame.text = title_txt
    for r in ch.chart_title.text_frame.paragraphs[0].runs:
        fonts(r, 14, True, DARK)
    pl = ch.plots[0]
    pl.gap_width = 40
    pl.has_data_labels = True
    dl = pl.data_labels
    dl.number_format, dl.number_format_is_linked = fmt, False
    dl.position = XL_LABEL_POSITION.OUTSIDE_END
    dl.font.size, dl.font.color.rgb, dl.font.name = Pt(size), TEXT, LATIN
    ser = pl.series[0]
    ser.format.fill.solid()
    ser.format.fill.fore_color.rgb = color
    ca, va = ch.category_axis, ch.value_axis
    ca.tick_labels.font.size, ca.tick_labels.font.name = Pt(size), LATIN
    ca.tick_labels.font.color.rgb = TEXT
    ca.format.line.color.rgb = RGBColor(0xC8, 0xCC, 0xD2)
    va.visible = False
    va.has_major_gridlines = False
    if horizontal:
        ca.reverse_order = True
    return ch


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
text(s, 0.8, 6.6, 11.8, 0.4, ['자세한 기록: docs/resnet18_optimization.md, docs/evta_runtime_compare.md §3, §7'], size=12, color=TEAL_L)
notes(s, 'Resnet18 1장 시간을 HLS 333 MHz + conv0 CPU float(134.8 ms, OMP 4)에서 systolic 400 MHz, 1코어 23.3 ms로 줄인 과정. '
         '하드웨어를 바꾸기 전에 runtime / 컴파일러 단위의 CPU 일을 먼저 없앴고(148.7 → 28.8 ms), 그 뒤 하드웨어 세 단계와 DMA 묶기를 했다. '
         '모든 단계는 출력이 byte 단위로 같다.')

# ---------------------------------------------------------------- 2 overview chart
s = prs.slides.add_slide(BLANK)
bg(s, WHITE)
title(s, '한눈에: 단계마다 줄어든 1장 시간', 'single EVTA, 1코어(OMP 1), systolic 400 MHz, 25장 중앙값')
cats = ['10-29 시작 (conv0 EVTA)', '① layout 유지', '② conv + add + relu', 'maxpool EVTA', 'transpose 16 B', 'buffer pool',
        'CPU fc NEON', 'fc EVTA', '전처리 합치기', 'record / replay', 'conv0 pack', 'conv0 + pool', 'ALU II = 1 (HW)',
        'bias reset (HW)', 'requantize (HW)', '최소 tile 1', '입력 겹치기', 'DMA 묶기']
vals = [148.7, 91.1, 67.1, 52.5, 46.9, 44.3, 40.6, 39.9, 37.1, 34.5, 29.2, 28.8, 27.3, 26.6, 25.5, 25.4, 24.1, 23.3]
ch = bar_chart(s, 0.4, 1.4, 8.9, 5.9, cats, vals, '1장 시간 (ms)', size=11)
for i in (12, 13, 14):
    pt = ch.plots[0].series[0].points[i]
    pt.format.fill.solid()
    pt.format.fill.fore_color.rgb = ORANGE
rect(s, 9.6, 1.45, 3.3, 5.8, LIGHT, name='Panel')
text(s, 9.8, 1.6, 3.0, 5.6, [
    [('CPU 일 없애기', {'bold': True, 'color': TEAL})], '148.7 → 28.8 ms (10-31 ~ 11-02). 하드웨어 변경 없음',
    [('하드웨어 세 단계', {'bold': True, 'color': ORANGE})], 'EVTA 26.4 → 23.05 ms (11-03 ~ 11-04)',
    [('그 뒤', {'bold': True, 'color': TEAL})], '최소 tile, 입력 겹치기, DMA 묶기 → 23.3 ms',
    [('결과는 매 단계 같음', {'bold': True, 'color': DARK})], 'EVTA 출력 byte 비교, golden 30개, 분류 281 (0.655530)'],
    size=14, space=6)
notes(s, '하드웨어 단계(주황) 값은 같은 날 이전 비트스트림과 번갈아 잰 값이라 앞 단계의 끝값과 조금 다르다(예: 28.83 / 28.98).')

# ---------------------------------------------------------------- section 1: start
section('1. 출발점: 시간은 어디에 쓰였나', '10-26 ~ 10-29: EVTA가 계산하는 시간은 전체의 20–40 %뿐이었다')

s = prs.slides.add_slide(BLANK)
bg(s, WHITE)
title(s, '처음 1장 134.8 ms 중 57 %가 CPU float conv0', 'HLS GEMM 333 MHz, OMP 4 (10-26 / 27)')
bar_chart(s, 0.4, 1.4, 7.6, 5.6, ['conv0 (CPU float)', 'EVTA 하드웨어', 'CPU 연산 (add, pool, relu, fc, quantize)',
                                  'transpose (conv마다 두 번)', 'Buffer Alloc / Free', '명령 생성 (JIT)'],
          [76.65, 24.16, 15.88, 12.31, 3.61, 1.62], '1장 134.8 ms의 구성 (ms)', color=ORANGE, size=13)
rect(s, 8.4, 1.45, 4.5, 5.6, LIGHT, name='Panel')
text(s, 8.6, 1.6, 4.1, 5.4, [
    [('conv0이 EVTA로 안 갔다', {'bold': True, 'color': ORANGE})],
    '입력이 3채널이라 EVTA 조건(16의 배수) 밖. 16채널로 0 채워 EVTA로 보내자 131.8 → 78.6 ms (10-28)',
    [('하드웨어 클럭', {'bold': True, 'color': TEAL})],
    'GEMM을 RTL systolic 어레이로 바꿔 333 → 400 MHz, HP 포트는 333 MHz로 분리 (+18 %)',
    [('conv0 schedule', {'bold': True, 'color': TEAL})], 'vt 2, tile 8×16 → 73.6 ms (10-29)'], size=14, space=7)
notes(s, 'evta_runtime_aot.md §3.1. conv0(7×7/2, 3채널)은 CPU convolutionFloat로 76.65 ms였다. EVTA 시간 24.16 ms, 명령 생성 1.62 ms.')

step('10-28: conv0을 CPU float에서 EVTA로 (0 채움)', 'evta_st01_conv0_to_evta', '131.8', '78.6', 'ms', '1장 (OMP 4)',
     ['3채널 입력을 16채널로 0 채움', 'zerofill transpose + EVTA conv', '연산의 13/16은 0을 곱함 → 나중에 conv0 pack'],
     '-DVTA_RESNET18_WITH_SKIPQUANT0=OFF로 conv0도 양자화해 EVTA로 보낸다. schedule 2 virtual threads 8×16.')

s = prs.slides.add_slide(BLANK)
bg(s, WHITE)
title(s, '10-29 분석: 시간을 잡아먹은 것은 EVTA가 아니었다', 'systolic 400 MHz, 1장 73.6 ms (OMP 4) / 148.9 ms (OMP 1)')
picture(s, 'evta_fig01_inference_flow', 0.4, 1.45, 7.4, 5.7)
table(s, 8.05, 1.5, 4.85, [['호출', 'OMP 4', 'OMP 1'], ['EVTA conv (+ JIT)', '32.1', '32.2'], ['transpose ×2', '15.8', '59.6'],
                           ['conv0 zerofill', '6.4', '6.5'], ['elemadd / maxpool / relu', '10.5', '37.5'],
                           ['Buffer Alloc / Free', '5.0', '4.5'], ['fc, 전처리', '3.4', '8.1']], [2.55, 1.15, 1.15], size=13)
text(s, 8.05, 4.25, 4.85, 2.9, [[('결론', {'bold': True, 'color': TEAL})],
                                'DAE 하드웨어는 큰 3×3 conv에서 이미 93 %(190 GOPS)',
                                '손해는 "conv 하나 = 함수 하나 = 동기화 한 번 = layout 왕복 한 번"이라는 runtime 단위',
                                'GEMM 32×32보다 CPU 일을 먼저 없앤다'], size=14, space=6)
notes(s, 'evta_runtime_compare.md §3, §7. 32×32를 먼저 하면 73.6 → 약 56 ms(−24 %)에 그치고, CPU 일을 없앤 뒤 하면 36 → 약 19 ms(−47 %).')

s = prs.slides.add_slide(BLANK)
bg(s, WHITE)
title(s, '무엇을 어디서 바꿨나: 컴파일러, runtime, 하드웨어', '그림 2: 단계마다 바꾼 위치')
picture(s, 'evta_fig02_change_map', 0.4, 1.45, 12.5, 5.75)
notes(s, '컴파일러(VTA.cpp / VTASave.cpp), vtalib(runtime, CPU 함수), 하드웨어(비트스트림)로 나눠 보면, 대부분의 시간은 컴파일러와 runtime 변경으로 줄었다.')

# ---------------------------------------------------------------- section 2: CPU work
section('2. CPU 일을 없애고 EVTA로 옮기기', '10-31 ~ 11-02, 하드웨어 변경 없음: 148.7 → 28.8 ms')

step('① 층 사이 activation을 EVTA layout 그대로', 'evta_st02_keep_layout', '148.7', '91.1', 'ms', '1장 (1코어)',
     ['conv 출력 N{C/16}HW16을 다음 conv가 그대로 읽음', 'NHWC 왕복 transpose 두 번이 항등 변환이었다', 'CPU 연산은 layout 몰라도 됨'],
     'VTASave findPackedActivations, -vta-keep-layout. 쓰는 명령이 모두 EVTA conv / elemadd / relu / pool인 activation만 EVTA layout으로 '
     '두고 처음부터 VTA 버퍼(<name>_vbuf)로 잡는다. pool은 "배치 N·C/16, 채널 16인 NHWC"로 본다. OMP 4: 73.6 → 56.4 ms, multi4 100회 3.9 → 2.79 s.')
step('② conv + elemadd + relu를 EVTA 호출 하나로', 'evta_st03_residual_fusion', '90.9', '67.1', 'ms', '1장 (1코어)',
     ['residual을 inp에 LOAD', '단위행렬 weight로 GEMM해 acc에 더함', 'MIN / MAX / ADD / SHR만 사용 (2-bit opcode로 됨)'],
     'findResidualFusions, vtalib convolution_wo_tr_res. scale이 2의 거듭제곱이라 곱셈이 필요 없다. 반올림 차이는 음수에서만 나는데 relu가 0으로 만들므로 '
     'relu가 붙은 add만 합친다. EVTA_RES_CHECK=1로 CPU 경로와 0 불일치. EVTA 시간은 +1.1 ms.')
step('maxpool을 EVTA ALU로', 'evta_st04_maxpool_evta', '67.1', '52.5', 'ms', '1장 (1코어)',
     ['입력 창을 LOAD, 단위 GEMM으로 acc에', '창 위치마다 ALU MAX 명령 하나', '입력이 0 이상(relu 뒤)일 때만'],
     'maxpool_evta. 출력 tile 3행 × 56열, tile 두 개를 inp / acc 두 반쪽에서 번갈아 처리해 load / compute / store가 겹친다. '
     '가장자리는 LOAD의 0 padding이라 입력이 0 이상일 때만 CPU와 같다.')
step('CPU transpose를 16 byte 복사로', 'evta_st05_transpose_neon', '52.5', '46.9', 'ms', '1장 (1코어)',
     ['NHWC ↔ N{C/16}HW16은 픽셀마다 16 byte 이동', 'byte 단위 루프 → 16 byte 복사', 'conv0 zerofill 6.8 → 0.2 ms'],
     'vtalib CPUBundle_generic.cpp. CMA 버퍼도 cache가 켜져 있어 malloc 메모리와 속도가 같았다. 느렸던 것은 코드였다.')
step('VTA buffer pool', 'evta_st06_buffer_pool', '47.0', '44.3', 'ms', '1장 (1코어)',
     ['run마다 버퍼 23개 cma_alloc / free ioctl', 'Free한 버퍼를 크기별 list에 남겨 재사용', 'run당 2.7 → 0.05 ms'],
     'vtalib api.cc, EVTA_BUF_POOL_MB(기본 32, 0이면 끔). 재사용 버퍼에는 이전 내용이 남는다(cma_alloc도 지우지 않음).')
step('CPU fc를 NEON으로', 'evta_st07_cpu_fc_neon', '44.3', '40.6', 'ms', '1장 (1코어)',
     ['weight를 1000 byte 간격으로 건너뛰며 읽었다', '행을 순서대로 8행씩, NEON 누산', 'fc 4.43 → 0.74 ms, bit 단위로 같음'],
     'vtalib cpuFullyConnected. thread마다 출력의 연속 구간, 출력 64개 합을 NEON register 16개에, 다음 8행 prefetch.')
step('multi4 멈춤의 원인: flush 뒤 dsb 누락', 'evta_fig10_multi4_hang_dsb', '400회에 1번', '0번', '/ 4000회', 'multi4 멈춤',
     ['libcma flush에 dsb가 없었다', 'MMIO 시작이 명령의 DRAM 도착보다 먼저 나감', 'VTAFlushCache 뒤 dsb sy'],
     'evta_runtime_compare.md §7.7. 5 µs 대기를 flush 앞에 두면 멈추고 뒤에 두면 안 멈추는 실험으로 원인을 갈랐다. EVTA_NO_DSB=1로 재현.')
step('fc를 EVTA GEMM으로', 'evta_st08_fc_evta', '40.6', '39.9', 'ms', '1장 (1코어)',
     ['weight를 compile time에 EVTA layout으로', 'wgt 반쪽씩 LOAD와 GEMM을 겹침', 'fc 0.74 → 0.15 ms'],
     'VTASave -vta-evta-fc, vtalib fullyconnected_evta_packed. 반올림은 std::round와 같게 ALU만으로: s = max(min(acc,0),−1), (acc + s + 2^(shift−1)) >> shift.')
step('입력 전처리를 conv0 입력 복사 하나로', 'evta_st09_preprocess_fuse', '39.9', '37.1', 'ms', '1장 (1코어)',
     ['quantize → −mean → ÷std → zerofill', '값 256개 × 채널의 표를 compile time에', 'NEON 표 찾기 한 번, 2.4 → 0.37 ms'],
     'VTASave -vta-fuse-preprocess, vtalib preprocess_nhwc2vtaio_zerofill. 표는 CPU elemsub / elemdiv와 같은 float 계산으로 만든다.')
step('명령 record / replay (AOT 0단계)', 'evta_st10_replay', '37.1', '34.5', 'ms', '1장 (1코어)',
     ['conv 호출마다 CPU가 명령을 다시 만들었다', '첫 호출의 명령과 주소 표를 기록', '다음부터 복사하고 주소만 고침'],
     'vtalib cmd_queue.h ReplayBegin, EVTA_REPLAY=0으로 끔. 원래 VTA + TVM은 uop를 컴파일러에서 만들었다. 이 형식이 컴파일 때 생성(AOT 1단계)의 시험대다.')

s = prs.slides.add_slide(BLANK)
bg(s, WHITE)
title(s, '하드웨어를 바꾸기 전에 쟀다: 층 사이 DRAM 왕복은 싸다', 'EVTA_MEMEXP로 LOAD / STORE를 빼고 잰 EVTA 시간 (11-02)')
bar_chart(s, 0.4, 1.4, 7.4, 5.7, ['그대로', 'inp LOAD 없음', 'STORE 없음', 'weight LOAD 없음', 'activation 전부 없음', 'DRAM 전송 전부 없음',
                                  'GEMM + ALU 하한'], [32.18, 30.80, 31.20, 31.39, 29.82, 29.13, 25.92], 'EVTA 시간 (ms)', size=13)
rect(s, 8.2, 1.45, 4.7, 5.7, LIGHT, name='Panel')
text(s, 8.4, 1.6, 4.3, 5.5, [
    [('conv → conv를 SRAM 안에서 이어도 최대 1.05 ms (3 %)', {'bold': True, 'color': TEAL})],
    'DAE가 LOAD / STORE를 이미 GEMM 뒤에 숨기고 있었다 → 그 하드웨어 경로는 만들지 않음',
    [('가장 큰 낭비는 conv0의 0 채움', {'bold': True, 'color': ORANGE})], 'GEMM 6.27 ms 중 약 5 ms가 0을 곱함 → 다음 작업'],
    size=14, space=8)
notes(s, 'EVTA_REPLAY_LOG=2(run별 반복 수, DRAM byte, 시간)와 EVTA_MEMEXP=<mask>(replay 사본의 LOAD / STORE를 원소 하나로; 결과는 틀어짐).')

step('conv0을 packed 입력의 4×1 conv로', 'evta_st11_conv0_pack', '34.5', '29.2', 'ms', '1장 (1코어)',
     ['stride 2의 행 두 개와 kw 7개를 채널로 접음', '7×7 / 2, 3채널 → 4×1 / 1, 48채널', 'conv0 9.13 → 3.64 ms'],
     'VTASave -vta-conv0-pack, getConv0Pack, vtalib preprocess_conv0pack. 정수 합이 같으므로 출력이 같다. EVTA_DUMP_CONV로 40개 conv 출력이 byte 단위로 같음.')
step('conv0 + maxpool을 EVTA 호출 하나로', 'evta_st12_conv0_pool', '29.19', '28.83', 'ms', '1장 (1코어)',
     ['conv0 tile에 pool halo를 다시 계산', 'acc 안에서 ALU MAX 두 단계', 'pooled 영역만 STORE (784 KB 왕복 없음)'],
     'VTASave -vta-fuse-pool, vtalib convolution_wo_tr_pool. 첫 판은 창 위치마다 명령을 내 명령 고정비가 이득을 다 먹었다(4.80 ms). 두 단계로 바꿔 4.48 ms.')

# ---------------------------------------------------------------- section 3: hardware
section('3. 남은 EVTA 시간과 하드웨어', '11-02 ~ 11-04: EVTA 26.4 → 23.05 ms, ISA는 남는 비트를 써서')

s = prs.slides.add_slide(BLANK)
bg(s, WHITE)
title(s, 'EVTA 26.47 ms는 어디에 쓰이나', 'EVTA_REPLAY_DUMP와 runbench로 잼 (11-02)')
bar_chart(s, 0.4, 1.4, 6.0, 3.0, ['GEMM 반복', 'ALU 반복 × 2', '나머지 (conv0 tile, 고정비)'], [18.85, 4.63, 2.99], 'EVTA 26.47 ms의 구성 (ms)', size=13)
bar_chart(s, 0.4, 4.4, 6.0, 2.9, ['MIN 127', 'ADD (bias)', 'SHR imm', 'MAX 0 / −128', 'MAX (pool)', 'ADD imm'], [0.98, 0.91, 0.90, 0.98, 0.45, 0.16],
          'ALU 4.63 ms의 내역 (ms)', color=ORANGE, fmt='0.00', size=12)
table(s, 6.8, 1.5, 6.1, [['측정 (runbench)', '값'], ['가장 작은 run', '4.7 µs'], ['GEMM 반복', '1.0 cycle'], ['ALU 반복 (imm도 reg도)', '2.0 cycle'],
                         ['GEMM / ALU 명령 하나', '약 25 / 35 cycle'], ['큰 LOAD / STORE', '5.0 / 5.2 GB/s']], [3.6, 2.5], size=14)
text(s, 6.8, 4.0, 6.1, 3.0, [[('run 21개 × 4.7 µs = 0.1 ms', {'bold': True, 'color': TEAL})], 'run을 합쳐도 얻을 것이 없다',
                             [('ALU가 II = 2', {'bold': True, 'color': ORANGE})], 'acc를 두 번 읽고 한 번 쓰는데 BRAM 포트가 둘 → 하드웨어를 고친다'],
     size=14, space=7)
notes(s, 'imm 연산(SHR, MIN, MAX imm, ADD imm)이 ALU 반복의 69 %. acc를 하나만 읽으므로 II = 1이 가능하다.')

step('GEMM을 RTL systolic 어레이로 (10-27 ~ 28)', 'evta_st13_systolic_gemm', '333', '400', 'MHz', 'EVTA 클럭',
     ['HLS GEMM의 inp broadcast(fanout 346)가 critical', 'weight-stationary 16×16 DSP48E2 어레이', 'single +18 %'],
     'docs/evta_systolic_design.md. HLS compute는 uop만 내보내고 RTL gemm_sa가 계산한다. HP 포트는 333 MHz로 분리.')
step('ALU imm 연산을 II = 1로', 'evta_st14_alu_ii1_regs', '28.98', '27.32', 'ms', '1장 (1코어, 같은 날)',
     ['imm 연산만 도는 루프를 따로', 'acc 읽기 한 번 + 쓰기 한 번', 'EVTA 26.42 → 24.89 ms'],
     'vta.cc alu(). 400 MHz timing: 첫 빌드 WNS −0.819 ns → register 두 개 → −0.126 → Performance_ExplorePostRoutePhysOpt로 +0.001 ns. '
     'Resnet18 ALU 명령 1,566개를 dump로 보고 같은 dst를 16 반복 안에 다시 쓰지 않음을 확인.')
step('RMW reset에 bias', 'evta_st15_bias_reset', '27.30', '26.59', 'ms', '1장 (1코어, 같은 날)',
     ['GEMM 시작값을 0 대신 bias로', 'bias ADD 명령이 없어짐', 'EVTA 24.89 → 24.15 ms'],
     'ISA: VTAGemInsn pad 1 bit(bias_reset). RTL sa_ctrl / sa_rmw. C 모델 → RTL TB 96/96 → CSim → 보드. 예상(0.91 ms)보다 덜 준 이유는 stage3/4에서 ADD가 '
     '이미 LOAD / STORE와 겹쳐 있었기 때문(EVTA_MEMEXP로 확인).')
step('requantize를 acc → out 쓰기에', 'evta_st16_requantize', '26.61', '25.49', 'ms', '1장 (1코어, 같은 날)',
     ['rq_* 필드: clip((acc + round) >> shift)', 'ALU SHR / MAX / MIN 명령이 없어짐', 'EVTA 24.15 → 23.05 ms'],
     'ISA: GEMM 비트 141–164, ALU 144–167(pad 자리). RTL sa_rmw S4–S6. vtalib은 비트스트림 이름(_rq)으로 켠다. 함께 고친 결함: 명령 pad 비트가 0으로 안 채워졌다.')

s = prs.slides.add_slide(BLANK)
bg(s, WHITE)
title(s, '하드웨어 세 단계 요약', '같은 실행 파일, 비트스트림만 바꿈')
picture(s, 'evta_st17_summary', 0.4, 1.45, 8.6, 5.75)
bar_chart(s, 9.2, 1.4, 3.8, 4.2, ['기본 400 MHz', 'ALU II = 1', 'bias reset', 'requantize'], [26.43, 24.89, 24.15, 23.05], 'Resnet18 EVTA (ms)',
          color=ORANGE, fmt='0.00', size=12)
text(s, 9.3, 5.7, 3.6, 1.4, [[('−12.8 %', {'bold': True, 'color': ORANGE, 'size': 28})], '137 → 157 GOPS'], size=14)
notes(s, 'multi4도 같은 소스로 통과: EVTA마다 26.73 → 23.44–23.56 ms, 4개 busy union 610.4 GOPS(400 MHz rq).')

# ---------------------------------------------------------------- section 4: after
section('4. 그 뒤, multi4, 남은 것', '11-05 ~ 11-10: 25.5 → 23.3 ms')

s = prs.slides.add_slide(BLANK)
bg(s, WHITE)
title(s, '그 뒤 세 단계', 'single, 1코어')
for i, (h, a, b, d) in enumerate([
        ('systolic 최소 tile 1', '25.53', '25.42',
         'HLS GEMM은 acc hazard 검사가 없어 tile이 7×7 이상이어야 했다. systolic RMW는 forwarding이 있어 필요 없다. 이를 고치다 vt 2 더미 tile 결함도 찾음'),
        ('conv0 입력 복사를 conv와 겹치기', '25.43', '24.08',
         '입력 복사 1.65 ms는 NEON 명령 수가 한계라 줄지 않았다. RunStart / RunWait로 나누고, run i가 도는 동안 행 i+1을 채운다'),
        ('conv DMA 묶기', '24.08', '23.27',
         '연속인 LOAD / STORE 행을 한 행으로, 1×1 weight는 한 번에. Resnet50(−12.6 ms)에서 훨씬 컸다')]):
    x = 0.5 + i * 4.2
    rect(s, x, 1.5, 3.9, 4.3, LIGHT, name=f'Card{i}')
    text(s, x + 0.2, 1.65, 3.5, 0.7, [h], size=18, bold=True, color=DARK)
    text(s, x + 0.2, 2.4, 3.5, 0.6, [[(a, {'color': ORANGE}), (' →', {'color': GREY, 'size': 20})]], size=26, bold=True)
    text(s, x + 0.2, 2.95, 3.5, 0.8, [[(b, {'color': TEAL}), (' ms', {'size': 18, 'color': TEAL})]], size=38, bold=True)
    text(s, x + 0.2, 3.95, 3.5, 3.0, [d], size=14)
notes(s, 'EVTA_MIN_TILE, EVTA_INPUT_STREAM, EVTA_CONV_BURST로 각각 끌 수 있다(실행 환경변수, systolic vtalib).')

s = prs.slides.add_slide(BLANK)
bg(s, WHITE)
title(s, 'multi4: EVTA 4개에 copy 4개, 100장 처리 시간', 'copy마다 OMP 1, 4 copy 동시 (run_all 100회 wall)')
bar_chart(s, 0.4, 1.4, 8.4, 5.8, ['시작 (10-29)', '① layout 유지', '② residual fusion', 'maxpool EVTA', 'transpose 16 B', 'buffer pool',
                                  'CPU fc NEON', '전처리 합치기', 'replay', 'conv0 pack', 'conv0 + pool'],
          [3909, 2793, 1851, 1471, 1314, 1189, 1110, 1060.5, 977.9, 827.3, 820.3], '100장 (ms)', fmt='0', size=12)
rect(s, 9.1, 1.45, 3.8, 5.7, LIGHT, name='Panel')
stat(s, 9.3, 1.65, 3.5, '4.8배', '100장 3.9 s → 0.82 s (11-02)')
stat(s, 9.3, 3.15, 3.5, '610 GOPS', '하드웨어 세 단계 뒤 4 EVTA busy union (11-04)')
stat(s, 9.3, 4.65, 3.5, '24.6 ms', '4 copy 동시, copy마다 1장 (11-06)')
notes(s, '4 copy를 같이 돌리면 copy마다 조금 느려진다: 어느 쌍이든 DDR controller 공유로 약 +0.9 ms, HP1 + HP2는 +0.35 ms 더(Resnet50으로 잼).')

s = prs.slides.add_slide(BLANK)
bg(s, WHITE)
title(s, '작업 중 발견하고 고친 결함', '모든 단계를 byte 비교했기 때문에 찾았다')
table(s, 0.5, 1.5, 12.3, [['결함', '증상', '고친 곳'],
                          ['libcma flush에 dsb 없음', 'multi4가 약 400회에 한 번 멈춤', 'vtalib VTAFlushCache'],
                          ['fc packed 경로에 replay 없음', 'fc만 매번 JIT', 'vtalib'],
                          ['명령 pad 비트를 0으로 안 채움', '새 필드(rq_*)에 쓰레기', 'NextInsn()'],
                          ['vt 2 더미 tile의 채널 블록 식', '7×7보다 작은 출력에서 오답', '두 vtalib'],
                          ['bias entry가 thread 1의 acc를 덮음', 'VGG16 conv0 오답(HLS, 시뮬레이터)', 'conv_kernel'],
                          ['multi buffer에서 bias가 acc 밖', 'systolic multi4 VGG16 오답', 'systolic conv_kernel'],
                          ['vt가 출력 블록 수를 못 나누면 계산 없음', 'DeiT Q·Kᵀ 오답, 오류 메시지 없음', '두 vtalib']],
      [4.3, 4.6, 3.4], size=14, rowh=0.55)
text(s, 0.5, 6.15, 12.3, 0.9, ['검증 도구: EVTA_DUMP_CONV(층 출력 dump), EVTA_RES_CHECK(CPU 경로와 실행 중 비교), golden 30개, '
                               'C 모델 → RTL testbench → CSim'], size=14, color=GREY)

s = prs.slides.add_slide(BLANK)
bg(s, WHITE)
title(s, '지금 23.3 ms와 다음 후보', 'EVTA 호출 22.65 ms(160 GOPS), CPU는 1 ms 아래')
table(s, 0.5, 1.5, 7.6, [['후보', '예상', '비고'],
                         ['한 장을 EVTA 4개로 나누기', '23 → 7–9 ms', '출력 채널 분할, runtime API'],
                         ['여러 층을 한 명령 stream으로', '약 −1 ms', '구간 제출'],
                         ['reg-reg ALU II = 1', '약 −0.25 ms', 'BRAM 비용 큼'],
                         ['conv0 tile, 1×1 합치기', '수백 µs', '소프트웨어'],
                         ['GEMM 32×32 / 2배 클럭', 'EVTA 약 2.5배', 'ISA, SRAM, backend'],
                         ['같은 ZCU102 DPU 비교', 'DPU 5.4 ms', '효율은 우리 75 % > 60 %']],
      [3.2, 1.9, 2.5], size=14, rowh=0.5)
for i, (h, d) in enumerate([('runtime 단위가 성능을 정했다', '하드웨어 변경 없이 148.7 → 28.8 ms'),
                            ('바꾸기 전에 쟀다', 'DRAM 왕복 상한 1 ms → 만들지 않음, ALU II는 확인 뒤 고침'),
                            ('ISA는 남는 비트로', '비트스트림 이름으로 켜고, 이전 판과 같은 실행 파일로 비교'),
                            ('검증 도구를 먼저', '모든 단계를 byte 단위로 비교')]):
    y = 1.5 + i * 1.4
    rect(s, 8.5, y, 4.4, 1.25, TEAL_L, name=f'Lesson{i}')
    text(s, 8.65, y + 0.1, 4.1, 0.45, [h], size=16, bold=True, color=DARK)
    text(s, 8.65, y + 0.55, 4.1, 0.7, [d], size=13, color=TEXT)
notes(s, '자세한 후보와 근거: resnet18_optimization.md §8, 전체 계획: unified_runtime_plan.md, deit_optimization.md §6.3.')

# ---------------------------------------------------------------- closing
s = prs.slides.add_slide(BLANK)
bg(s, DARK)
text(s, 0.8, 2.2, 11.8, 1.0, ['134.8 → 23.3 ms, 결과는 매 단계 그대로'], size=38, bold=True, color=WHITE)
text(s, 0.8, 3.3, 11.8, 2.5, ['runtime / 컴파일러 단위의 CPU 일을 먼저 없앴고(×5.2), 하드웨어는 측정으로 원인을 확인한 뒤 고쳤다(EVTA −12.8 %).',
                              '다음: 한 장을 EVTA 4개로 나누기, 구간 제출, 그리고 같은 방법을 Resnet50 / MobileNetV2 / DeiT로.',
                              '문서: docs/resnet18_optimization.md, docs/evta_overview.md, docs/evta_walkthrough.md'],
     size=18, color=TEAL_L, space=12)

prs.save(OUT)
print(OUT, len(prs.slides._sldIdLst), 'slides')
