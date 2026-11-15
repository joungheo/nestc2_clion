#!/usr/bin/env python3
# JO added (2026-11-15): the figures of docs/evta_epilogue_guide.md. usage: python3 docs/figs/gen_evta_epi_figs.py docs/figs
# figures for docs/evta_epilogue_guide.md (style of docs/figs/evta_alux_*.svg)
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


# ---------------------------------------------------------------- E1: why
f = Fig('evta_epi_01_why', 1400, 560, '그림 E1. epilogue가 없을 때와 있을 때: conv 뒤 원소별 연산의 DRAM 왕복')
f.text(40, 84, '① epilogue 없음 (원소별 연산마다 따로 호출)', 16, INK, 'start', True)
y = 110
f.box(40, y, 190, 70, 'blue', ['EVTA conv', 'GEMM → acc → int8'])
f.box(330, y, 150, 70, 'grey', ['DRAM', 'conv 출력'])
f.box(580, y, 190, 70, 'orange', ['add 호출', 'LOAD 2개 → 더함'])
f.box(870, y, 150, 70, 'grey', ['DRAM', 'add 출력'])
f.box(1120, y, 190, 70, 'orange', ['relu / 표 호출', 'LOAD → 계산'])
for a, b in [(230, 330), (480, 580), (770, 870), (1020, 1120)]:
    f.arrow([(a, y + 35), (b, y + 35)], 'r', 2.2)
for x, t in [(280, 'STORE'), (530, 'LOAD'), (820, 'STORE'), (1070, 'LOAD')]:
    f.text(x, y + 26, t, 12, MK['r'])
f.text(700, y + 104, 'DRAM 왕복이 연산 수만큼 생기고, 호출마다 EVTA 시작·대기 비용이 든다', 14, MK['r'])
f.text(40, 290, '② epilogue (conv 결과가 acc에 있는 동안 STORE 전에 처리)', 16, INK, 'start', True)
y = 316
f.box(40, y, 760, 120, 'blue', [], 14)
f.text(60, y + 28, 'EVTA conv 한 번 (타일마다)', 15, INK, 'start', True)
xs = [60, 215, 370, 525, 680]
labs = [['GEMM', 'acc = Σ x·w'], ['conv rq', 'int8 값'], ['ADD', '+ residual'], ['RQ / CLIP', 'relu, ReLU6'], ['LUT', 'GELU 등']]
cols = ['blue', 'blue', 'green', 'green', 'green']
for x, l, c in zip(xs, labs, cols):
    f.box(x, y + 46, 110, 56, c, l, 13)
for x in xs[:-1]:
    f.arrow([(x + 110, y + 74), (x + 155, y + 74)], 'g')
f.box(900, y + 25, 150, 70, 'grey', ['DRAM', '최종 출력'])
f.arrow([(800, y + 60), (900, y + 60)], 'b', 2.2)
f.text(850, y + 51, 'STORE 1번', 12, MK['b'])
f.box(1120, y + 25, 190, 70, 'blue', ['다음 conv'])
f.arrow([(1050, y + 60), (1120, y + 60)], 'b', 2.2)
f.text(700, 486, '원소별 연산은 EVTA 안(acc)에서 끝나고 DRAM에는 한 번만 쓴다. 녹색 = epilogue 단계', 14, MK['g'])
f.text(700, 516, '예전에는 정해진 패턴 함수(add+relu, 표 하나, ReLU6)만 됐다 → 지금은 단계 목록(프로그램) 하나로 순서 자유', 13, GREY)
f.save()

# ---------------------------------------------------------------- E2: layers
f = Fig('evta_epi_02_layers', 1400, 930, '그림 E2. 누가 무엇을 하나: VTASave(컴파일) → 번들 → vtalib(실행) → EVTA 하드웨어')
f.box(40, 70, 1320, 170, 'purple', [])
f.text(60, 98, '컴파일 시 (PC, model-compiler)   lib/Backends/VTA/VTASave.cpp', 15, INK, 'start', True)
f.box(70, 115, 360, 105, 'white', ['IR 명령열', 'conv → (view) → add → relu …', '(alloc / dealloc은 건너뜀)'], 13)
f.box(520, 115, 360, 105, 'white', ['chain walker', 'findResidualFusions (2^k scale)', 'findMulResidualFusions (임의 scale)'], 13)
f.box(970, 115, 360, 105, 'white', ['번들 .cpp에 쓰는 것', 'VTAEpilogue 구조체 + 단계 목록', 'VTASetConvEpilogue(&e); conv 호출'], 13)
f.arrow([(430, 167), (520, 167)], 'p', 2)
f.arrow([(880, 167), (970, 167)], 'p', 2)
f.box(40, 270, 1320, 120, 'grey', [])
f.text(60, 297, '번들 코드 (DeiT out-proj 예)', 15, INK, 'start', True)
f.lines(70, 325, ['VTAEpilogue e = {3, {{6 /*MUL*/, 5762}, {1 /*ADD*/, 0, 0, 0, 20107}, {2 /*RQ*/, 2, 14, -128, 127}}};',
                  'e.t[0] = residual_vbuf;  VTASetConvEpilogue(&e);',
                  'convolution_wo_tr(in, W, bias, out, …, nVirtualThread 2, tileH 9, tileW 11);  VTASetConvEpilogue(nullptr);'],
        13, INK, 'start', 20, mono=True)
f.arrow([(1150, 240), (1150, 270)], 'p', 2)
f.box(40, 420, 1320, 250, 'green', [])
f.text(60, 447, '실행 시 (보드 CPU)   vtalib-zcu102-systolic/lib/Bundle/VTABundle.cpp', 15, INK, 'start', True)
f.box(70, 465, 290, 150, 'white', ['convolution_wo_tr', 'epilogue가 있으면', 'convEpilogue()로'], 13)
f.box(400, 465, 290, 150, 'white', ['lowerEpilogue', '목록 → 표준형', 'f1 → MUL → ADD → RQ → f2', '이 비트스트림에서 되나?'], 13)
f.box(730, 465, 290, 150, 'white', ['conv_kernel (타일마다)', 'LOAD / GEMM / ALU / STORE', '명령 + uop + 의존 토큰', '→ 명령 큐 (DRAM)'], 13)
f.box(1060, 465, 270, 150, 'orange', ['못 하면 (CPU 경로)', '보통 conv 실행 후', 'VTAEpilogueRef를', '원소마다 CPU에서'], 13)
f.arrow([(360, 540), (400, 540)], 'g', 2)
f.arrow([(690, 520), (730, 520)], 'g', 2)
f.text(710, 510, '예', 12, MK['g'])
f.arrow([(545, 615), (545, 638), (1195, 638), (1195, 615)], 'o', 1.8)
f.text(870, 658, '아니오 (표 2개, ADD 2개, 확장 없는 비트스트림의 LUT / MUL …)', 12, MK['o'])
f.arrow([(700, 390), (700, 420)], 'k', 2)
f.box(40, 700, 1320, 200, 'blue', [])
f.text(60, 727, 'EVTA (FPGA, 400 MHz)   — epilogue를 모른다. vtalib이 낸 명령만 실행', 15, INK, 'start', True)
f.box(70, 750, 220, 120, 'white', ['fetch', 'DRAM 명령열을 읽어', '모듈별 큐로 분배'], 13)
f.box(360, 750, 220, 120, 'white', ['load', 'inp / wgt LOAD', '(residual도 여기로)'], 13)
f.box(650, 750, 300, 120, 'white', ['compute', 'GEMM (systolic 16×16)', 'ALU: MUL / MADD / LUT …', 'rq: 쓰면서 반올림·clip'], 13)
f.box(1020, 750, 220, 120, 'white', ['store', 'out → DRAM'], 13)
f.arrow([(290, 810), (360, 810)], 'b', 2)
f.arrow([(580, 810), (650, 810)], 'b', 2)
f.arrow([(950, 810), (1020, 810)], 'b', 2)
f.arrow([(875, 670), (875, 700)], 'k', 2)
f.text(880, 690, 'RunStart: 명령 수·주소 레지스터, START', 12, INK, 'start')
f.save()

# ---------------------------------------------------------------- E3: program -> canonical form
f = Fig('evta_epi_03_lowering', 1400, 760, '그림 E3. lowering: 어떤 단계 목록이든 표준형 하나로 줄이고, 단항 부분은 세 종류로 분류')
f.text(40, 84, '단계 목록 (VTASave가 낸 것, 최대 8단계)', 15, INK, 'start', True)
steps = [('MUL 5762', 'orange'), ('ADD d=20107', 'green'), ('RQ 2,14', 'green')]
f.text(40, 118, 'DeiT out-proj:', 13, GREY, 'start')
x = 160
for t, c in steps:
    f.box(x, 98, 130, 34, c, [t], 13, bold0=False); x += 145
f.text(40, 168, 'Resnet50 res2_0:', 13, GREY, 'start')
x = 160
for t, c in [('ADD 0,1,0', 'green'), ('RQ 2,1', 'green'), ('CLIP 0,127', 'yellow'), ('SHL 1', 'yellow'), ('CLIP -128,127', 'yellow')]:
    f.box(x, 148, 130, 34, c, [t], 13, bold0=False); x += 145
f.text(40, 218, 'DeiT fc1:', 13, GREY, 'start')
f.box(160, 198, 130, 34, 'purple', ['LUT 0 (GELU)'], 13, bold0=False)
f.arrow([(700, 245), (700, 285)], 'k', 2)
f.text(712, 270, 'lowerEpilogue', 13, INK, 'start', True)
f.text(40, 316, '표준형 (모든 목록이 이 꼴):', 15, INK, 'start', True)
y = 340
parts = [('conv int8 x', 'blue', 150), ('f1', 'yellow', 120), ('× mulV', 'orange', 120), ('ADD', 'green', 210), ('RQ', 'green', 190), ('f2', 'yellow', 120), ('out int8', 'blue', 130)]
sub = ['', 'ADD 앞 단항', 'MUL (새로)', '(v<<shlV) + t·mulT<<shlT', 'round, shift, lo, hi', 'ADD 뒤 단항', '']
x = 40
for (t, c, w), s2 in zip(parts, sub):
    f.box(x, y, w, 64, c, [t, s2] if s2 else [t], 13)
    nx = x + w + 30
    if t != 'out int8':
        f.arrow([(x + w, y + 32), (nx, y + 32)], 'k')
    x = nx
f.text(700, 440, 'ADD가 없으면 f1만 있다. ADD는 하나까지 (residual 텐서 하나를 inp에 둠)', 13, GREY)
f.text(40, 490, 'f1 / f2 분류: −128..127 입력 256개를 실제로 계산해 보고 모양을 찾는다', 15, INK, 'start', True)
kinds = [('종류 0: clip(x, a, b)', 'yellow', ['예: relu, ReLU6, 아무것도 안 함', 'EVTA: 앞 clip 범위에 합침 → 명령 0개']),
         ('종류 1: clip(x << q, a, b)', 'yellow', ['예: relu 뒤 scale ½ (SHL 1)', 'EVTA: ALU SHR −q, MAX, MIN']),
         ('종류 2: 그 밖 (표)', 'purple', ['예: GELU, sigmoid', 'EVTA: ALU LUT (_alux 이상)', '표는 conv마다 하나'])]
x = 40
for t, c, ls in kinds:
    f.box(x, 515, 420, 150, c, [t] + ls, 14)
    x += 450
f.text(700, 705, '못 하는 경우(표 2개, ADD 2개, MUL add 뒤 shift·표, 확장 없는 비트스트림의 LUT·MUL, rq 없는 비트스트림의 round 2) → false → CPU 경로', 13, MK['o'])
f.text(700, 732, '결과는 (프로그램, 표 내용, conv relu/clip, EVTA 번호)로 캐시하고 replay key에도 넣는다', 13, GREY)
f.save()

# ---------------------------------------------------------------- E4: memory layout
f = Fig('evta_epi_04_sram', 1400, 640, '그림 E4. EVTA 안 메모리 배치 (단일 EVTA): acc / inp를 thread 두 개가 반씩, residual 자리')
f.text(40, 80, 'acc (int32, 항목 = 16채널 × 4 B, 4096 항목)', 15, INK, 'start', True)
X0, W = 60, 1280
def seg(a, b, y, h, col, ls, size=13):
    x = X0 + W * a / 4096; w = W * (b - a) / 4096
    f.box(x, y, w, h, col, ls, size, r=2, bold0=False)
y = 100
seg(0, 600, y, 80, 'blue', ['thread 0: X 타일', '(DeiT: 99 항목 × 6 블록)'])
seg(600, 1024, y, 80, 'white', ['(빈 곳)'])
seg(1024, 1624, y, 80, 'blue', ['thread 1: X 타일'])
seg(1624, 2048, y, 80, 'white', ['(빈 곳)'])
seg(2048, 2642, y, 80, 'green', ['thread 0', 'Y = residual'])
seg(2642, 3072, y, 80, 'white', [])
seg(3072, 3666, y, 80, 'green', ['thread 1', 'Y = residual'])
seg(3666, 4096, y, 80, 'white', [])
for a in [0, 1024, 2048, 3072, 4096]:
    f.text(X0 + W * a / 4096, y + 102, str(a), 12, GREY)
f.box(X0, 225, W * 2048 / 4096, 40, 'blue', ['conv가 쓰는 2048 항목 (ACC_MEM_ENTRY)'], 13, bold0=False, dash=True)
f.box(X0 + W / 2, 225, W / 2, 40, 'green', ['MUL add의 Y (11-15: 위쪽 절반, multi-EVTA는 X 옆)'], 13, bold0=False, dash=True)
f.text(40, 320, 'inp (int8, 항목 = 16 B, 4096 항목)', 15, INK, 'start', True)
y = 336
seg(0, 1024, y, 80, 'orange', ['thread 0: 입력 타일', '(GEMM 뒤 residual LOAD 자리로 재사용)'])
seg(1024, 2048, y, 80, 'orange', ['thread 1: 입력 타일 / residual'])
seg(2048, 4096, y, 80, 'white', ['(conv가 안 씀: residual을 미리 LOAD할 수 있는 자리)'])
for a in [0, 1024, 2048, 4096]:
    f.text(X0 + W * a / 4096, y + 102, str(a), 12, GREY)
f.text(40, 500, '왜 thread 두 개인가', 15, INK, 'start', True)
f.lines(60, 530, ['• thread 0이 계산하는 동안 thread 1 자리에 다음 타일을 LOAD → LOAD와 계산이 겹친다 (virtual thread)',
                  '• residual은 inp의 입력 자리를 다시 쓰므로, 그 타일의 GEMM이 끝나야 LOAD할 수 있다 → MUL add가 느렸던 이유 하나',
                  '• Y를 X 옆에 두면 한 타일에 넣을 출력 블록 수(nFilterinLoop)가 6 → 3으로 줄어 입력을 두 번 읽었다 → 위쪽 절반으로 옮김'],
        14, INK, 'start', 26)
f.save()

# ---------------------------------------------------------------- E5: one tile, two add paths
f = Fig('evta_epi_05_tile', 1400, 900, '그림 E5. 타일 하나에서 나가는 명령: 모듈별 시간 흐름과 두 가지 ADD 경로')
rows = [('load', 120), ('compute', 220), ('store', 320)]
for n, yy in rows:
    f.text(40, yy + 30, n, 15, INK, 'start', True)
    f.line(140, yy + 60, 1360, yy + 60, 'gr', 1)
def blk(x, w, row, col, ls):
    yy = dict(rows)[row]
    f.box(x, yy, w, 50, col, ls, 12, r=3, bold0=False)
blk(150, 150, 'load', 'orange', ['입력·weight LOAD'])
blk(330, 260, 'compute', 'blue', ['GEMM: acc X = Σ in·w + bias'])
blk(610, 140, 'load', 'orange', ['residual LOAD'])
blk(610, 180, 'compute', 'blue', ['conv rq: SHR, MAX, MIN', '→ X = int8 conv 값'])
blk(810, 70, 'compute', 'grey', ['(대기)'])
blk(900, 230, 'compute', 'green', ['epilogue ADD 경로', '(아래 a 또는 b)'])
blk(1170, 120, 'store', 'blue', ['STORE X'])
f.arrow([(300, 160), (330, 230)], 'r', 1.5)
f.text(318, 205, '토큰', 11, MK['r'], 'start')
f.arrow([(590, 230), (610, 160)], 'r', 1.5)
f.text(585, 190, '"inp 비었음"', 11, MK['r'], 'end')
f.arrow([(750, 160), (900, 228)], 'r', 1.5)
f.text(800, 160, '"residual 있음"', 11, MK['r'], 'start')
f.arrow([(1130, 250), (1170, 330)], 'r', 1.5)
f.text(1160, 290, '토큰', 11, MK['r'], 'start')
f.text(700, 410, '빨간 화살표 = 모듈 사이 의존 토큰 (fetch가 모든 명령을 미리 나눠 주고, 모듈은 토큰이 올 때까지 기다린다)', 13, MK['r'])
f.box(40, 450, 650, 400, 'green', [])
f.text(60, 480, '(a) shift add — Resnet, MobileNetV2 (scale 비 2^k)', 15, INK, 'start', True)
for k, (c1, c2) in enumerate([('X <<= shlV', 'ALU SHR −shlV'), ('X += residual · 2^shlT·I', 'GEMM (identity weight)'),
                               ('rq(round 2, shift m, clip)', 'GEMM이 쓸 때 바로'), ('[f2: CLIP / SHL / LUT]', 'ALU')]):
    f.text(70, 525 + k * 44, c1, 14, INK, 'start', mono=True); f.text(400, 525 + k * 44, c2, 14, MK['g'], 'start')
f.lines(70, 720, ['• weight가 int8이라 residual에는 2^k (≤ 64)만 곱할 수 있다', '• ALU 확장 없이도 된다 (확장 전부터 있던 길)',
                  '• Resnet18 8개, Resnet50 16개, MobileNetV2 10개 conv'], 14, INK, 'start', 26)
f.box(710, 450, 650, 400, 'orange', [])
f.text(730, 480, '(b) MUL add — DeiT out-proj / fc2 (scale 비 임의, 11-15)', 15, INK, 'start', True)
for k, (c1, c2) in enumerate([('reset Y; Y += residual·I', 'GEMM'), ('Y = Y × mulT (20107)', 'ALU MUL'),
                               ('X = X × mulV (5762) + Y', 'ALU MADD + rq(round 2, >>14)'), ('', '(_alux: MUL X, ADD X += Y)')]):
    f.text(740, 525 + k * 44, c1, 14, INK, 'start', mono=True); f.text(1010, 525 + k * 44, c2, 14, MK['o'], 'start')
f.lines(740, 700, ['• 20107처럼 큰 배율은 ALU MUL(16비트 상수)이 필요 →', '  residual이 acc에 따로 있어야 한다 (Y)',
                   '• 결과 바이트는 elemadd_evta와 같다', '• 하지만 대기 + ALU 5 pass가 겹치지 않아 더 느림 → 기본 꺼짐'], 14, INK, 'start', 26)
f.save()

# ---------------------------------------------------------------- E6: run flow
f = Fig('evta_epi_06_run', 1400, 740, '그림 E6. 명령이 하드웨어에 가는 길: 명령 큐(DRAM) → 레지스터 → fetch 분배 → done, 그리고 replay')
f.box(40, 80, 400, 250, 'green', [])
f.text(60, 108, '보드 CPU (vtalib)', 15, INK, 'start', True)
f.lines(60, 145, ['1. conv_kernel: 타일마다', '   LOAD / GEMM / ALU / STORE + uop', '   + VTADepPush / VTADepPop', '2. VTASynchronize: cache flush', '3. RunStart: 레지스터 쓰기', '4. RunWait: compute done polling', '5. 출력 VTABufferInvalidate'], 14, INK, 'start', 25)
f.box(520, 80, 360, 250, 'grey', [])
f.text(540, 108, 'DRAM (CMA, 캐시 비일관)', 15, INK, 'start', True)
for i, (t, c) in enumerate([('입력 activation', 'orange'), ('weight / bias (시작 때 1번)', 'orange'), ('출력 / residual', 'orange'), ('명령열 + uop', 'green')]):
    f.box(545, 125 + i * 50, 310, 40, c, [t], 13, bold0=False)
f.box(960, 80, 400, 250, 'blue', [])
f.text(980, 108, 'EVTA', 15, INK, 'start', True)
f.box(990, 130, 150, 60, 'white', ['fetch'], 14)
for i, t in enumerate(['load 큐', 'compute 큐', 'store 큐']):
    f.box(1200, 125 + i * 65, 140, 50, 'white', [t], 13, bold0=False)
    f.arrow([(1140, 160), (1200, 150 + i * 65)], 'b', 1.5)
f.arrow([(440, 280), (545, 290)], 'g', 2)
f.text(490, 270, '쓰기', 12, MK['g'])
f.arrow([(855, 290), (990, 175)], 'b', 2)
f.text(915, 220, 'DMA로 읽기', 12, MK['b'])
f.arrow([(240, 330), (240, 352), (1065, 352), (1065, 190)], 'k', 1.5, dash=True)
f.text(650, 372, 'RunStart: INSN_COUNT, INSN_ADDR 레지스터 쓰기 → START (MMIO)', 12, INK)
f.text(1160, 320, '명령 안: DRAM 주소 → SRAM 주소, 크기', 11, GREY)
f.text(40, 430, 'replay (AOT stage 0): 같은 인자의 conv를 두 번째 부를 때', 15, INK, 'start', True)
f.box(60, 450, 380, 120, 'white', ['첫 호출', 'conv_kernel이 명령을 만들고', '기록 (ReplayBegin / End)'], 14)
f.box(520, 450, 380, 120, 'white', ['두 번째 호출부터', '기록을 복사하고 버퍼 주소만 바꿈', '(relocation) → 바로 RunStart'], 14)
f.box(980, 450, 380, 120, 'white', ['replay key', 'conv 모양, schedule, epilogue', 'lowering 결과(mulV, mulT …)'], 14)
f.arrow([(440, 510), (520, 510)], 'k', 2)
f.arrow([(980, 510), (900, 510)], 'k', 1.5, dash=True)
f.text(700, 620, '그래서 conv_kernel은 모양마다 한 번만 돈다. DeiT처럼 같은 conv가 12층 반복되면 CPU의 명령 만드는 시간이 거의 없다', 14, INK)
f.text(700, 652, '큐가 가득 차면 여러 run으로 나눠 보낸다 (EVTA_REPLAY_LOG=2가 run마다 GEMM / ALU 반복 수, DRAM 바이트, 시간을 찍는다)', 13, GREY)
f.save()

# ---------------------------------------------------------------- E7: bitstream table
f = Fig('evta_epi_07_bitstreams', 1400, 520, '그림 E7. 비트스트림마다 EVTA에서 되는 것 (안 되면 CPU 경로, 결과 바이트는 같다)')
cols = ['', 'shift add', '종류 1 (shift·clip)', '표 (LUT)', 'MUL add', '그 밖']
rowsd = [('HLS GEMM (vta/vtalib)', ['○', '○ add 뒤만', 'CPU', 'CPU', 'CPU']),
         ('systolic _rq (확장 없음)', ['○', '○', 'CPU', 'CPU', 'CPU']),
         ('_alux', ['○', '○', '○', '○ MUL·MUL·ADD', 'CPU']),
         ('_alux2 / _alux3', ['○', '○', '○', '○ MUL + MADD', 'CPU'])]
cw = [300, 190, 210, 170, 230, 160]
x0, y0, rh = 60, 70, 70
xs = [x0]
for w in cw: xs.append(xs[-1] + w)
for j, t in enumerate(cols):
    f.box(xs[j], y0, cw[j], 50, 'grey', [t] if t else [], 14)
for i, (n, v) in enumerate(rowsd):
    y = y0 + 50 + i * rh
    f.box(xs[0], y, cw[0], rh, 'white', [n], 14, r=0)
    for j, t in enumerate(v):
        f.box(xs[j + 1], y, cw[j + 1], rh, 'green' if t.startswith('○') else 'orange', [t], 14, r=0, bold0=False)
f.text(700, 450, '그 밖 = 표 2개, ADD 2개, MUL add 뒤의 shift·표 등. 같은 실행 파일이 /tmp/evta_bitstream 이름을 보고 길을 고른다', 14, INK)
f.text(700, 480, 'CPU 경로 = 보통 conv + VTAEpilogueRef (같은 정수 식) → 어느 길로 가도 출력 바이트가 같다 (EVTA_EPI_CHECK=1로 확인)', 14, GREY)
f.save()
print('ok')
