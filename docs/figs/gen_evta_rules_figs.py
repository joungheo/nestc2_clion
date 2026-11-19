#!/usr/bin/env python3
# JO added (2026-11-20): the figures of docs/evta_fusion_rules_guide.md. usage: python3 docs/figs/gen_evta_rules_figs.py docs/figs
# (style and helpers of docs/figs/gen_evta_ahead_figs.py / gen_evta_pool_figs.py)
import sys, html
OUT = sys.argv[1]
INK, GREY, WHITE = '#333333', '#888888', '#ffffff'
C = {  # fill, stroke
    'blue': ('#e8f0fa', '#2f5f98'), 'green': ('#e9f5e7', '#3c8a35'), 'orange': ('#fdf0e2', '#c06a10'),
    'purple': ('#f1eaf8', '#6d4596'), 'red': ('#fbe9e7', '#c0392b'), 'yellow': ('#fbf6dc', '#9a7b00'),
    'grey': ('#f2f2f2', '#888888'), 'white': ('#ffffff', '#333333')}
MK = {'k': INK, 'b': '#2f5f98', 'o': '#c06a10', 'g': '#3c8a35', 'r': '#c0392b', 'p': '#6d4596', 'y': '#9a7b00', 'gr': GREY}
FONT = "'Noto Sans CJK KR', 'Noto Sans KR', 'Malgun Gothic', 'Apple SD Gothic Neo', sans-serif"


class Fig:
    def __init__(self, name, w, h, title):
        self.name, self.w, self.h, self.s = name, w, h, []
        self.title = title

    def add(self, x):
        self.s.append(x)

    def text(self, x, y, t, size=14, color=INK, anchor='middle', bold=False, mono=False, italic=False):
        f = ' font-family="DejaVu Sans Mono, Consolas, monospace" xml:space="preserve"' if mono else ''
        st = ' font-style="italic"' if italic else ''
        self.add(f'<text x="{x}" y="{y}" font-size="{size}" fill="{color}" text-anchor="{anchor}"'
                 f' font-weight="{"bold" if bold else "normal"}"{f}{st}>{html.escape(t)}</text>')

    def lines(self, x, y, ls, size=14, color=INK, anchor='middle', gap=None, bold0=False, mono=False):
        gap = gap or size * 1.35
        for i, t in enumerate(ls):
            self.text(x, y + i * gap, t, size, color, anchor, bold0 and i == 0, mono)

    def box(self, x, y, w, h, col='blue', ls=(), size=14, r=6, dash=False, bold0=True, sw=1.6, mono=False, align='middle'):
        f, s = C[col]
        d = ' stroke-dasharray="6,4"' if dash else ''
        self.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{f}" stroke="{s}" stroke-width="{sw}"{d}/>')
        if ls:
            gap = size * 1.35
            y0 = y + h / 2 - (len(ls) - 1) * gap / 2 + size * 0.35
            tx = x + w / 2 if align == 'middle' else x + 10
            self.lines(tx, y0, ls, size, INK, align if align == 'middle' else 'start', gap, bold0, mono)

    def arrow(self, pts, c='k', w=1.8, dash=False, both=False):
        p = ' '.join(f'{a},{b}' for a, b in pts)
        d = ' stroke-dasharray="6,4"' if dash else ''
        ms = f' marker-start="url(#a{c})"' if both else ''
        self.add(f'<polyline points="{p}" fill="none" stroke="{MK[c]}" stroke-width="{w}"{d}{ms} marker-end="url(#a{c})"/>')

    def line(self, x1, y1, x2, y2, c='gr', w=1.2, dash=False):
        d = ' stroke-dasharray="5,4"' if dash else ''
        self.add(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{MK[c]}" stroke-width="{w}"{d}/>')

    def save(self):
        defs = ''.join(f'<marker id="a{k}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" '
                       f'orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="{v}"/></marker>' for k, v in MK.items())
        out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" viewBox="0 0 {self.w} {self.h}" font-family="{FONT}">',
               f'<title>{html.escape(self.title)}</title>', f'<defs>{defs}</defs>',
               f'<rect x="0" y="0" width="{self.w}" height="{self.h}" fill="#ffffff"/>']
        out.append(f'<text x="{self.w / 2}" y="34" font-size="19" fill="{INK}" text-anchor="middle" font-weight="bold">{html.escape(self.title)}</text>')
        open(f'{OUT}/{self.name}.svg', 'w').write('\n'.join(out + self.s + ['</svg>\n']))


# ---------------------------------------------------------------- R1: the checks of each finder before / after
f = Fig('evta_rules_01_checks', 1400, 640, '그림 R1. finder마다 따로 적던 확인 (전)과 한 함수 canTail (후)')
f.text(40, 80, '① 전: 생산자 판정과 "이미 꼬리가 있나" 확인이 finder마다 다르다', 16, INK, 'start', True)
X = [40, 330, 640, 760, 880]   # finder, producer, fusions, pool, outLut
W = [290, 310, 120, 120, 120]
hdr = ['finder', '생산자 판정', 'fusions', 'poolFusions', 'outLutConv']
for x, w, t in zip(X, W, hdr):
    f.box(x, 96, w, 34, 'grey', [t], 13)
rows = [
    ('findResidualFusions', 'ConvPath::VTA', '–', '–', '–', ''),
    ('findPoolFusions (보통 conv)', 'VTA + relu', '✔', '–', '✔*', 'r'),
    ('findPoolFusions (첫 conv)', 'FirstBNN + RELU + pack', '–', '–', '–', 'r'),
    ('findOutLutConvs', 'ConvPath::VTA', '✔', '✔', '✔', ''),
    ('findMulResidualFusions', 'ConvPath::VTA', '✔', '✔', '✔', ''),
    ('findEmbedFusions', 'VTA 또는 FirstBNN', '✔', '✔', '✔', ''),
    ('findAttnKeepLayout', 'ConvPath::VTA', '✔', '–', '✔', 'r'),
]
for r, row in enumerate(rows):
    y = 134 + r * 40
    for k, (x, w) in enumerate(zip(X, W)):
        col = 'white'
        if row[5] == 'r' and ((r == 1 and k == 4) or (r == 2 and k >= 2) or (r == 6 and k == 3)):
            col = 'red'
        f.box(x, y, w, 36, col, [row[k]], 13, bold0=False, mono=(k < 2))
f.text(1020, 160, '✔ = 확인함, – = 확인 안 함', 13, INK, 'start')
f.text(1020, 196, '빨강 = 틀리거나 빠진 확인', 13, MK['r'], 'start', True)
f.lines(1020, 232, ['✔*: outLutConv는 findOutLutConvs가 채우는데', '그 finder가 Pool보다 뒤에 돈다.',
                    '확인할 때 늘 비어 있다 (효과 없음).'], 13, INK, 'start')
f.lines(1020, 310, ['첫 conv: 아무 확인도 없다.', '2x2 pool은 아예 볼 수 없었다', '(kk != 3이면 버림: VGG16 pool 놓침).'],
        13, INK, 'start')
f.lines(1020, 388, ['AttnKeepLayout: pool 꼬리를 안 봄', '(DeiT에는 pool이 없어 드러나지 않았다).'], 13, INK, 'start')
f.text(40, 450, '② 후: 모든 finder가 같은 한 줄을 묻는다', 16, INK, 'start', True)
f.box(40, 468, 1320, 92, 'green', [], 14)
f.text(700, 502, 'canTail(I, TAIL_x, ctx)  =  (evtaTails(evtaGemmOf(I)) & TAIL_x)  &&  ctx->tailFree(I)', 17, INK, 'middle', True, True)
f.text(700, 538, '어떤 생산자인가 → 그 호출이 이 꼬리를 하나 → 아직 꼬리가 없나 (fusions · poolFusions · outLutConv 세 기록 모두 빔)', 14, INK)
f.text(700, 600, '꼬리마다 다른 조건 (scale, 모양, tile, relu)은 각 finder에 그대로 남는다. 다섯 모델 번들은 바이트까지 같다.', 14, INK)
f.save()

# ---------------------------------------------------------------- R2: how canTail decides
f = Fig('evta_rules_02_cantail', 1400, 600, '그림 R2. canTail이 답을 내는 순서 (두 예)')
bits = ['CHAIN', 'MUL_ADD', 'CONST_ADD', 'POOL3', 'POOL2', 'OUT_LUT']
def bitrow(x, y, on, hl=None, label=''):
    for k, b in enumerate(bits):
        col = 'green' if on[k] else 'white'
        if hl is not None and k == hl:
            col = 'yellow' if on[k] else 'red'
        f.box(x + k * 104, y, 100, 44, col, [b, '1' if on[k] else '0'], 12, bold0=False)
    if label:
        f.text(x - 12, y + 28, label, 13, INK, 'end', True)
f.text(40, 80, '생산자 종류별 꼬리 표 evtaTails (비트 하나 = 꼬리 하나)', 16, INK, 'start', True)
bitrow(300, 96, [1, 1, 1, 1, 1, 1], label='EvtaGemm::Conv')
bitrow(300, 150, [0, 0, 1, 1, 0, 0], label='EvtaGemm::FirstConv')
bitrow(300, 204, [0, 0, 0, 0, 0, 0], label='EvtaGemm::None')
f.lines(960, 112, ['Conv: convolution_wo_tr[_res|_pool]', 'FirstConv: conv0 pack / zero-fill 첫 conv',
                   'None: CPU conv, conv 아닌 명령'], 13, INK, 'start', 54)
f.text(40, 300, '예 ① VGG16 Conv_2__2 (보통 EVTA conv) 뒤의 2x2 pool: findPoolFusions가 TAIL_POOL2를 묻는다', 15, INK, 'start', True)
f.box(40, 318, 220, 60, 'blue', ['Conv_2__2', 'getConvPath = VTA'], 13)
f.arrow([(260, 348), (300, 348)], 'k')
f.box(300, 318, 150, 60, 'white', ['evtaGemmOf', '→ Conv'], 13)
f.arrow([(450, 348), (490, 348)], 'k')
f.box(490, 318, 240, 60, 'yellow', ['evtaTails(Conv) & POOL2', '= 16 ≠ 0  ✔'], 13)
f.arrow([(730, 348), (770, 348)], 'k')
f.box(770, 318, 230, 60, 'yellow', ['tailFree(Conv_2__2)', '세 기록에 없음  ✔'], 13)
f.arrow([(1000, 348), (1040, 348)], 'g', 2.2)
f.box(1040, 318, 320, 60, 'green', ['true → 모양 · tile 조건 확인 후', 'poolFusions[Conv_2__2] 기록'], 13)
f.text(40, 430, '예 ② Resnet18 첫 conv (conv0 pack) 뒤에 2x2 pool이 왔다면', 15, INK, 'start', True)
f.box(40, 448, 220, 60, 'blue', ['conv0', 'getConvPath = FirstBNN'], 13)
f.arrow([(260, 478), (300, 478)], 'k')
f.box(300, 448, 150, 60, 'white', ['evtaGemmOf', '→ FirstConv'], 13)
f.arrow([(450, 478), (490, 478)], 'k')
f.box(490, 448, 240, 60, 'red', ['evtaTails(FirstConv) & POOL2', '= 0  ✘'], 13)
f.arrow([(730, 478), (1040, 478)], 'r', 2.2)
f.box(1040, 448, 320, 60, 'red', ['false → 합치지 않음', '(conv0 pack + 2x2는 시험 안 함)'], 13)
f.text(700, 570, '새 생산자나 꼬리를 더할 때는 vtalib 호출과 번들 생성이 받는지 확인한 뒤 evtaTails에 비트 하나를 켠다.', 14, INK)
f.save()

# ---------------------------------------------------------------- R3: finder order and tailFree
f = Fig('evta_rules_03_order', 1400, 540, '그림 R3. finder 순서와 tailFree: 먼저 준 꼬리가 이긴다')
order = [('Residual', 'fusions'), ('EvtaMaxPools', ''), ('InputPre-\nprocess', ''), ('Pool', 'poolFusions'),
         ('LutRegions', ''), ('OutLutConvs', 'outLutConv'), ('… OutLut-\nMatMuls', ''), ('PackedRows', ''),
         ('MulResidual', 'fusions'), ('Embed', 'fusions')]
x0, bw, gap = 40, 118, 14
for k, (n, rec) in enumerate(order):
    x = x0 + k * (bw + gap)
    col = 'blue' if rec else 'grey'
    f.box(x, 150, bw, 64, col, n.split('\n'), 13, bold0=False)
    if k:
        f.arrow([(x - gap, 182), (x, 182)], 'k', 1.4)
    if rec:
        f.box(x + 4, 250, bw - 8, 34, 'green', [rec], 12, bold0=False, mono=True)
        f.arrow([(x + bw / 2, 214), (x + bw / 2, 250)], 'g', 1.4)
f.text(40, 76, 'VTA::save가 부르는 순서 (파랑 = 꼬리를 주는 finder, 초록 = 그 finder가 쓰는 기록)', 15, INK, 'start', True)
xp, xo = x0 + 3 * (bw + gap) + bw / 2, x0 + 5 * (bw + gap) + bw / 2
f.arrow([(xp, 150), (xp, 122), (xo, 122), (xo, 146)], 'r', 1.8, dash=True)
f.text((xp + xo) / 2, 115, '전: Pool이 outLutConv를 봤지만 아직 비어 있다', 12, MK['r'])
f.box(40, 330, 640, 180, 'white', [], 14)
f.lines(60, 360, ['tailFree(I) = !fusions.count(I) && !poolFusions.count(I)', '                && !outLutConv.count(I)'],
        14, INK, 'start', 22, mono=True)
f.lines(60, 420, ['• 꼬리는 생산자마다 하나 (tile의 STORE 앞 자리가 하나).', '• 뒤 finder는 앞 finder가 준 꼬리를 보고 건너뛴다.',
                  '• 순서가 바뀌어도 "하나만"은 지켜진다.'], 14, INK, 'start')
f.box(720, 330, 640, 180, 'yellow', [], 14)
f.text(740, 360, '예: DeiT fc1 (1x1 conv) 뒤의 GELU 표', 14, INK, 'start', True)
f.lines(740, 392, ['Residual: fc1 뒤가 add가 아님 → 꼬리 없음', 'Pool: pool 아님 → 없음',
                   'OutLutConvs: canTail(fc1, OUT_LUT) ✔ → outLutConv[fc1]', 'MulResidual / Embed: tailFree(fc1) = false → 건너뜀'],
        13, INK, 'start', 26)
f.save()

# ---------------------------------------------------------------- R4: IrSeq
f = Fig('evta_rules_04_irseq', 1400, 520, '그림 R4. IrSeq: 세 finder에 복사돼 있던 IR 순회를 하나로')
f.text(40, 80, 'IR 명령 열 (예: VGG16 conv → pool)', 15, INK, 'start', True)
ir = [('0', 'alloc  A', 'grey'), ('1', 'conv   C : src → A', 'blue'), ('2', 'alloc  B', 'grey'),
      ('3', 'dealloc src', 'grey'), ('4', 'maxpool P : A → B', 'green'), ('5', 'dealloc A', 'grey'), ('6', 'conv   C2 : B → …', 'blue')]
for k, (i, t, col) in enumerate(ir):
    y = 96 + k * 46
    f.box(40, y, 50, 40, 'white', [i], 13, bold0=False)
    f.box(96, y, 300, 40, col, [t], 13, bold0=False, mono=True, align='start')
f.box(470, 96, 420, 120, 'white', [], 14)
f.text(490, 122, 'ins', 14, INK, 'start', True, True)
f.text(560, 122, '명령 순서 그대로 (0 … 6)', 13, INK, 'start')
f.text(490, 152, 'allocAt', 14, INK, 'start', True, True)
f.text(580, 152, 'A → 0, B → 2 (할당 위치)', 13, INK, 'start')
f.text(490, 182, 'users', 14, INK, 'start', True, True)
f.text(560, 182, 'A → 2 (C, P), B → 2 (P, C2)', 13, INK, 'start')
f.text(490, 204, '          alloc / dealloc은 세지 않음', 12, GREY, 'start')
f.box(470, 232, 420, 70, 'white', [], 14)
f.text(490, 260, 'next(1) = 4', 14, INK, 'start', True, True)
f.text(490, 286, 'alloc / dealloc을 건너뛴 다음 명령', 13, INK, 'start')
f.arrow([(396, 142), (430, 142), (430, 280), (460, 280)], 'b', 1.4)
f.arrow([(396, 280), (440, 280)], 'g', 1.4, dash=True)
f.box(940, 96, 420, 206, 'yellow', [], 14)
f.text(960, 124, 'finder가 이 값으로 묻는 것', 14, INK, 'start', True)
f.lines(960, 154, ['• users[A] == 2: conv 출력을 pool만 읽나', '  (합치면 A는 DRAM에 없다)',
                   '• allocAt[B] > 1: pool 출력 B가 conv 뒤에', '  할당됨 → 앞으로 당긴다 (hoist)',
                   '• next(1): conv 바로 뒤 명령이 꼬리인가'], 13, INK, 'start', 24)
f.text(40, 450, '전: findResidualFusions, findPoolFusions, findMulResidualFusions에 같은 20줄이 세 벌 (주석만 조금씩 다름)', 14, MK['r'], 'start')
f.text(40, 482, '후: struct IrSeq 하나. reportFusions (VTA_FUSION_LOG)도 같은 것을 쓴다.', 14, MK['g'], 'start', True)
f.save()

# ---------------------------------------------------------------- R5: VTA_FUSION_LOG
f = Fig('evta_rules_05_log', 1400, 700, '그림 R5. VTA_FUSION_LOG=1: 어디서 무엇을 적나')
f.box(40, 80, 250, 70, 'blue', ['finder들', '(find…Fusions 등)'], 14)
f.arrow([(290, 115), (340, 115)], 'k')
f.box(340, 80, 300, 70, 'green', ['reportFusions', '생산자 · 꼬리 · 빠진 꼬리'], 14)
f.arrow([(640, 115), (690, 115)], 'k')
f.box(690, 80, 670, 70, 'blue', ['번들 코드 내기 (명령마다 save…Inst)', 'EVTA 조건이 안 맞으면 noteCpuGemm(I, 이유)'], 14)
f.text(40, 196, '① 생산자 줄 (finder 뒤, 2026-11-19)', 15, INK, 'start', True)
f.box(40, 210, 600, 200, 'white', [], 13)
f.lines(56, 236, ['Conv_2__2 (conv): max pool', 'Conv_28__2 (conv): no tail; read by', '  MaxPool_30__2 (maxpool)', '  <- a tail op not fused (a CPU pool)',
                  '13 EVTA producers, 4 with a tail,', '  1 read by a tail op not fused'], 13, INK, 'start', 28, mono=True)
f.text(40, 446, '② CPU GEMM 줄 (번들을 다 낸 뒤, 2026-11-20 추가)', 15, INK, 'start', True)
f.box(40, 460, 1320, 110, 'white', [], 13)
f.lines(56, 488, ['cpu …layer_11_attention_MatMul_MatMul_0__2: matmul 2 x 64 x 198: 2 rows: no H x W with W >= 7 (largest W 1)',
                  'cpu Conv_…expanded_conv_2_expand…: conv: input channels 24 not a multiple of 16   (-vta-pad-channels=0일 때)',
                  '6 GEMMs on the CPU'], 13, INK, 'start', 28, mono=True)
hooks = [('saveConvolutionInst', 'getConvPath(I, &why)'), ('saveFullyConnectedInst', 'saveEvtaFcPacked(…, &why)'),
         ('MatMul (generic CPU)', 'matmul_evta 조건'), ('saveChannelwiseConv…', 'dw_convolution_evta 조건')]
for k, (a, b) in enumerate(hooks):
    x = 690 + k * 170
    f.box(x, 210, 160, 90, 'yellow', [a, b], 11, bold0=True)
    f.arrow([(x + 80, 150), (x + 80, 210)], 'o', 1.4)
    f.arrow([(x + 80, 300), (x + 80, 360)], 'o', 1.4)
f.box(690, 360, 670, 50, 'yellow', ['ctx.cpuGemms (명령, 이유) → 끝에 한꺼번에 출력'], 13)
f.arrow([(1025, 410), (1025, 460)], 'o', 1.6)
f.text(700, 620, '①은 "EVTA에서 돌지만 꼬리를 못 받은 것", ②는 "EVTA에서 아예 안 도는 GEMM"을 보여 준다. 둘 다 stderr에만 쓰고 번들은 같다.', 14, INK)
f.text(700, 650, '이유는 조건을 다시 쓰지 않고 실제로 EVTA 길을 떠나는 자리에서 적는다 (판정과 로그가 어긋나지 않게).', 14, INK)
f.save()
