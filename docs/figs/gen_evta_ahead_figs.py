#!/usr/bin/env python3
# JO added (2026-11-15): the figures of docs/evta_epilogue_guide.md. usage: python3 docs/figs/gen_evta_epi_figs.py docs/figs
# figures for docs/evta_load_ahead_guide.md (style of docs/figs/evta_alux_*.svg)
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




def lanes(f, x0, y0, names, gap=56, w=1280, label_w=150):
    for i, n in enumerate(names):
        y = y0 + i * gap
        f.text(x0 + label_w - 12, y + 26, n, 14, INK, 'end', True)
        f.line(x0 + label_w, y + 40, x0 + w, y + 40, 'gr', 1, True)
    return [y0 + i * gap for i in range(len(names))]


def blk(f, x0, ys, row, a, b, col, t, size=13, dash=False):
    f.box(x0 + a, ys[row] + 6, b - a, 34, col, [t], size, r=4, dash=dash)


# ---------------------------------------------------------------- A1: timeline before / after
f = Fig('evta_ahead_01_timeline', 1400, 720, '그림 A1. 한 EVTA run 안의 tile 두 개: 전에는 다음 tile의 LOAD가 앞 tile의 STORE 뒤에야 시작했다')
X = 210
S = 1.75
def T(a, b): return (a * S, b * S)
f.text(40, 84, '① 전 (EVTA_LOAD_AHEAD=0)', 16, INK, 'start', True)
ys = lanes(f, 40, 100, ['load 큐', 'compute 큐', 'store 큐'], 50, 1360, 160)
for (a, b), r, c, t in [(T(0, 60), 0, 'orange', '입력 · 가중치 0'), (T(150, 190), 0, 'yellow', 'residual 0'),
                        (T(40, 150), 1, 'blue', 'GEMM 0'), (T(190, 250), 1, 'green', 'epilogue 0'), (T(250, 300), 2, 'purple', 'STORE 0'),
                        (T(300, 360), 0, 'orange', '입력 · 가중치 1'), (T(450, 490), 0, 'yellow', 'residual 1'),
                        (T(340, 450), 1, 'blue', 'GEMM 1'), (T(490, 550), 1, 'green', 'epilogue 1'), (T(550, 600), 2, 'purple', 'STORE 1')]:
    blk(f, X, ys, r, a, b, c, t)
for a, b in [T(150, 190), T(250, 340)]:
    f.box(X + a, ys[1] + 6, b - a, 34, 'red', ['기다림'], 12, r=4, dash=True)
f.arrow([(X + 300 * S, ys[2] + 6), (X + 300 * S, ys[0] + 40)], 'r', 1.6)
f.text(X + 300 * S + 8, ys[2] + 62, 'STORE 0이 끝나야 reset 1 → 그 토큰으로 LOAD 1 시작', 12, MK['r'], 'start')
f.text(X + 600 * S + 10, ys[1] + 28, '끝 600', 13, MK['r'], 'start', True)

f.text(40, 330, '② 후 (EVTA_LOAD_AHEAD=2, 기본)', 16, INK, 'start', True)
ys = lanes(f, 40, 346, ['load 큐', 'compute 큐', 'store 큐'], 50, 1360, 160)
for (a, b), r, c, t in [(T(0, 60), 0, 'orange', '입력 · 가중치 0'), (T(60, 100), 0, 'yellow', 'residual 0'),
                        (T(40, 150), 1, 'blue', 'GEMM 0'), (T(150, 210), 1, 'green', 'epilogue 0'), (T(210, 260), 2, 'purple', 'STORE 0'),
                        (T(150, 210), 0, 'orange', '입력 1'), (T(210, 250), 0, 'yellow', 'residual 1'),
                        (T(260, 370), 1, 'blue', 'GEMM 1'), (T(370, 430), 1, 'green', 'epilogue 1'), (T(430, 480), 2, 'purple', 'STORE 1')]:
    blk(f, X, ys, r, a, b, c, t)
f.box(X + 210 * S, ys[1] + 6, 50 * S, 34, 'grey', ['STORE 대기'], 12, r=4, dash=True)
f.arrow([(X + 150 * S, ys[1] + 6), (X + 150 * S, ys[0] + 40)], 'g', 1.6)
f.text(X + 150 * S - 6, ys[0] - 2, '마지막 GEMM 토큰', 12, MK['g'], 'end')
f.arrow([(X + 210 * S, ys[1] + 6), (X + 210 * S, ys[0] + 40)], 'g', 1.6)
f.text(X + 250 * S + 4, ys[0] - 2, 'epilogue 0이 residual을 다 읽은 토큰', 12, MK['g'], 'start')
f.text(X + 480 * S + 10, ys[1] + 28, '끝 480', 13, MK['g'], 'start', True)
f.text(700, 560, '다음 tile의 LOAD(주황 · 노랑)가 앞 tile의 epilogue · STORE와 같은 시간에 돈다. compute는 STORE 대기만 남는다.', 14, INK)
f.text(700, 590, '시간은 설명용 비율이다 (실측: DeiT fc2 + residual add 543 → 470 us, Resnet50 68.85 → 65.41 ms)', 13, GREY)
f.text(700, 620, 'tile = 출력의 한 조각 (공간 조각 또는 출력 채널 묶음 = filter group). 모듈 셋은 각자 자기 큐의 명령을 순서대로 실행한다.', 13, GREY)
f.text(700, 650, 'run 하나의 첫 LOAD와 마지막 STORE는 겹칠 상대가 없어 그대로 남는다 (tile이 적은 conv일수록 이득이 작다).', 13, GREY)
f.save()

# ---------------------------------------------------------------- A2: modules, queues, tokens
f = Fig('evta_ahead_02_tokens', 1400, 640, '그림 A2. EVTA의 세 모듈과 토큰: 토큰에는 이름이 없고, n번째 pop은 n번째 push를 받는다')
f.box(60, 200, 260, 150, 'orange', ['load 모듈 (큐 1)', 'DRAM → inp / wgt', 'LOAD'], 15)
f.box(570, 200, 260, 150, 'blue', ['compute 모듈 (큐 2)', 'GEMM, ALU', 'acc / uop LOAD도 여기'], 15)
f.box(1080, 200, 260, 150, 'purple', ['store 모듈 (큐 3)', 'out → DRAM', 'STORE'], 15)
f.arrow([(320, 240), (570, 240)], 'o', 2.4)
f.text(445, 228, 'l2g: "다 실었다"', 13, MK['o'])
f.text(445, 262, 'VTADepPush(1, 2) / Pop(1, 2)', 12, GREY, mono=True)
f.arrow([(570, 310), (320, 310)], 'b', 2.4)
f.text(445, 298, 'g2l: "다 썼다 (덮어써도 됨)"', 13, MK['b'])
f.text(445, 332, 'VTADepPush(2, 1) / Pop(2, 1)', 12, GREY, mono=True)
f.arrow([(830, 240), (1080, 240)], 'b', 2.4)
f.text(955, 228, 'g2s: "계산 끝, 내보내라"', 13, MK['b'])
f.text(955, 262, 'VTADepPush(2, 3) / Pop(2, 3)', 12, GREY, mono=True)
f.arrow([(1080, 310), (830, 310)], 'p', 2.4)
f.text(955, 298, 's2g: "다 내보냄 (out 재사용)"', 13, MK['p'])
f.text(955, 332, 'VTADepPush(3, 2) / Pop(3, 2)', 12, GREY, mono=True)
f.box(60, 80, 1280, 90, 'grey', [], 14)
f.lines(80, 108, ['fetch 모듈이 DRAM의 명령열(VTAGenericInsn, 256 bit)을 읽어 명령마다 그 모듈의 큐로 보낸다. 세 모듈은 동시에 돈다.',
                  '명령마다 플래그 4개: pop_prev / pop_next (실행 전에 이웃 모듈의 토큰을 하나 꺼냄, 없으면 기다림),',
                  'push_prev / push_next (실행 후 이웃 모듈에 토큰 하나). 토큰 큐는 개수만 센다 (FIFO, 하드웨어 세마포어).'], 14, INK, 'start', 24)
f.box(60, 390, 1280, 220, 'white', [], 14)
f.text(80, 418, 'vtalib에서 쓰는 법 (VTADepPush(from, to) / VTADepPop(from, to); 1 = load, 2 = compute, 3 = store)', 15, INK, 'start', True)
f.lines(80, 450, ['VTADepPush(vtaCmdH, 2, 1);   // compute의 마지막 명령에 push_prev를 켠다 (그 명령이 끝나면 load에게 토큰)',
                  'VTADepPop(vtaCmdH, 2, 1);    // 다음에 만들 load 명령에 pop_next를 켠다 (토큰이 올 때까지 그 LOAD는 기다림)',
                  '                             // 같은 큐에 pop이 두 번 연속이면 첫 번째는 빈 명령(NOP)에 실린다',
                  '규칙 1. 각 큐 쌍에서 pop 순서 = push 순서. k번째 pop은 "k번째 push가 가리키는 일"이 끝났다는 뜻이다.',
                  '규칙 2. run 하나에서 push 수 = pop 수 (남으면 다음 run의 첫 pop이 엉뚱한 토큰을 받는다).',
                  '규칙 3. pop이 기다리는 push는 명령열에서 그 pop보다 앞에 있어야 한다 (뒤에 있으면 fetch가 막혀 멈춘다).'],
        13, INK, 'start', 25, mono=True)
f.save()

# ---------------------------------------------------------------- A3: which token each first LOAD takes
f = Fig('evta_ahead_03_pairs', 1400, 860, '그림 A3. group k → k+1: load 큐의 첫 pop들이 어느 push를 받나 (thread 2개, 입력 채널 블록 C개)')
def seq(f, x, y, items, w=118, h=50, gap=10):
    xs = []
    for t, c in items:
        f.box(x, y, w, h, c, t, 12, r=4)
        xs.append(x + w / 2)
        x += w + gap
    return xs
f.text(40, 80, '① 전: 다음 group의 첫 LOAD는 그 group의 reset 토큰을 받는다 → reset은 STORE 대기 뒤', 16, INK, 'start', True)
f.text(40, 120, 'compute', 14, INK, 'start', True)
cx = seq(f, 130, 100, [(['GEMM k', 'c=C-1 t0 ▲'], 'blue'), (['GEMM k', 'c=C-1 t1 ▲'], 'blue'), (['epilogue k', '(ALU)'], 'green'),
                       (['▼ STORE k', '대기'], 'purple'), (['reset k+1', 't0 ▲'], 'blue'), (['reset k+1', 't1 ▲'], 'blue'),
                       (['GEMM k+1', 'c=0 t0'], 'blue'), (['GEMM k+1', 'c=0 t1'], 'blue')])
f.text(40, 230, 'load', 14, INK, 'start', True)
lx = seq(f, 130, 210, [(['▼▼ 끝 pop', '(GEMM k 토큰)'], 'grey'), (['residual k', '→ inp 0/1024'], 'yellow'), (['▼ 입력 k+1', 'c=0 t0'], 'orange'),
                       (['▼ 가중치', 'c=0 t1'], 'orange'), (['▼ 입력', 'c=1 ...'], 'orange')], 150)
f.arrow([(cx[0], 150), (lx[0] - 20, 210)], 'b', 1.6)
f.arrow([(cx[1], 150), (lx[0] + 20, 210)], 'b', 1.6)
f.arrow([(cx[4], 150), (lx[2], 210)], 'r', 2)
f.arrow([(cx[5], 150), (lx[3], 210)], 'r', 2)
f.text(700, 300, '빨강: 입력 k+1은 reset k+1 뒤 = epilogue k와 STORE k가 끝난 뒤에야 DRAM에서 읽기 시작', 14, MK['r'])

f.text(40, 360, '② 후: 마지막 GEMM 토큰을 다음 group의 첫 LOAD가 받고, residual은 epilogue 토큰을 받는다', 16, INK, 'start', True)
f.text(40, 400, 'compute', 14, INK, 'start', True)
cx = seq(f, 130, 380, [(['GEMM k', 'c=C-1 t0 ▲'], 'blue'), (['GEMM k', 'c=C-1 t1 ▲'], 'blue'), (['epilogue k', 't0 ▲E'], 'green'),
                       (['epilogue k', 't1 ▲E'], 'green'), (['▼ STORE k', '대기'], 'purple'), (['reset k+1', '(토큰 없음)'], 'grey'),
                       (['GEMM k+1', 'c=0 t0 ▼'], 'blue'), (['GEMM k+1', 'c=0 t1 ▼'], 'blue'), (['▼▼ residual', '토큰 받기'], 'grey')], 112)
f.text(40, 510, 'load', 14, INK, 'start', True)
lx = seq(f, 130, 490, [(['▼ 입력 k+1', 'c=0 t0'], 'orange'), (['▼ 가중치', 'c=0 t1'], 'orange'), (['▼E residual', 'k+1 t0 ▲'], 'yellow'),
                       (['▼E residual', 'k+1 t1 ▲'], 'yellow'), (['▼ 입력', 'c=1 t0'], 'orange'), (['▼ 가중치', 'c=1 t1'], 'orange')], 150)
for a, b, c in [(0, 0, 'g'), (1, 1, 'g'), (2, 2, 'g'), (3, 3, 'g')]:
    f.arrow([(cx[a], 430), (lx[b], 490)], c, 2)
f.arrow([(lx[0], 540), (lx[0], 562), (cx[6], 562), (cx[6], 430)], 'o', 1.4, dash=True)
f.arrow([(lx[2], 540), (lx[2], 580), (cx[8], 580), (cx[8], 430)], 'y', 1.4, dash=True)
f.box(40, 610, 1320, 230, 'white', [], 14)
f.lines(60, 640, ['load 큐의 pop 순서 (group k+1):  c=0 t0, c=0 t1  →  residual t0, t1  →  c=1 t0, c=1 t1  →  …  →  c=C-1',
                  'compute 큐의 push 순서 (group k): GEMM c=0 ×2, …, GEMM c=C-1 ×2  →  epilogue t0, t1 (▲E)',
                  '→ group k+1의 c=0 pop 2개 = GEMM k c=C-1 (녹색)   residual pop 2개 = ▲E   c=1 pop = GEMM k+1 c=0 … (규칙 1)',
                  '',
                  'run의 첫 group: reset이 토큰 2개 + 2개를 더 낸다 (c=0용과 residual용).',
                  'run의 마지막 group: 끝 pop 2개(GEMM 토큰)를 그대로 하고 ▲E는 내지 않는다 → run 끝 상태가 전과 같다 (규칙 2).',
                  'compute 쪽: GEMM k+1 c=0 다음에 residual LOAD의 토큰 2개를 받는다 (점선). epilogue에서는 따로 pop하지 않는다.'],
        14, INK, 'start', 27)
f.save()

# ---------------------------------------------------------------- A4: SRAM layout
f = Fig('evta_ahead_04_sram', 1400, 600, '그림 A4. 단일 EVTA의 inp / wgt 배치 (thread 2개): 다음 tile의 LOAD가 앞 tile이 읽는 곳을 덮지 않는다')
def bar(f, x0, y, w, total, segs, label):
    f.text(x0 - 12, y + 30, label, 15, INK, 'end', True)
    for a, b, col, ls in segs:
        f.box(x0 + w * a / total, y, w * (b - a) / total, 52, col, ls, 12, r=2)
    last = -100
    for a in sorted({s[0] for s in segs} | {segs[-1][1]}):
        if w * a / total - last < 40:
            continue
        last = w * a / total
        f.text(x0 + w * a / total, y + 70, str(a), 11, GREY)
x0, w = 200, 1150
bar(f, x0, 80, w, 4096, [(0, 1024, 'orange', ['입력 반쪽 A', '홀수 채널']), (1024, 2048, 'orange', ['입력 반쪽 B', '짝수 채널']),
                         (2048, 3072, 'yellow', ['residual thread 0', '(epilogue ADD)']), (3072, 4096, 'yellow', ['residual thread 1', ''])], 'inp (16 B)')
bar(f, x0, 200, w, 1024, [(0, 512, 'blue', ['thread 0 가중치', '[kernel][channel]']), (512, 1023, 'blue', ['thread 1 가중치', '']),
                          (1023, 1024, 'red', [''])], 'wgt (256 B)')
f.text(x0 + w - 4, 196, 'identity 1023 (residual GEMM)', 12, MK['r'], 'end')
f.box(60, 320, 1290, 250, 'white', [], 14)
f.lines(80, 350, ['누가 덮어쓰고, 무엇을 기다리나 (group k+1의 LOAD)',
                  '• 입력 c=0 → 반쪽 하나: 그 반쪽을 마지막으로 읽은 것은 group k의 c=C-2 GEMM. thread 0의 c=C-1 GEMM 토큰이 오면 이미 끝났다',
                  '   (compute는 명령 순서대로: c=C-2 t0, t1 → c=C-1 t0). 반쪽 순서는 group을 넘어 이어 간다 (그림 A5).',
                  '• 가중치 thread t → 자기 영역: 읽는 것은 thread t의 GEMM뿐. c=0의 pop이 그 thread의 c=C-1 GEMM 토큰이다.',
                  '• residual thread t → 2048 + 1024 t: 읽는 것은 group k의 epilogue(residual GEMM). ▲E 토큰을 받고 LOAD한다.',
                  '• identity 1023: run 첫 group에서 한 번만 LOAD. 가중치는 1023 아래에 들어갈 때만 이 방식 (canUp).',
                  '• 2단계: 1x1 conv의 가중치가 한 번에 들어가고 filter group이 하나면, 같은 run의 다음 tile은 가중치 LOAD를 하지 않는다.',
                  '• multi-EVTA 비트스트림은 inp가 2048 entry라 residual 자리가 없다 → residual 있는 conv는 전과 같이 (ahead 끔).'],
        14, INK, 'start', 27, bold0=True)
f.save()

# ---------------------------------------------------------------- A5: odd block count
f = Fig('evta_ahead_05_parity', 1400, 700, '그림 A5. 처음 보드에서 틀렸던 이유: 입력 채널 블록이 홀수(13)일 때 반쪽 순서')
f.text(40, 84, '① 고치기 전: group마다 c=0을 반쪽 B에서 시작', 16, INK, 'start', True)
def chans(f, x, y, start, n=13, w=74, lab='k'):
    for c in range(n):
        h = 'B' if (c + start) % 2 == 0 else 'A'
        f.box(x + c * (w + 6), y, w, 44, 'orange' if h == 'B' else 'yellow', [f'{lab} c={c}', h], 11, r=3)
    return x + n * (w + 6)
e = chans(f, 60, 100, 0, 11)
f.text(e + 30, 128, '…', 16)
f.box(e + 60, 100, 74, 44, 'orange', ['k c=12', 'B'], 11, r=3)
f.box(e + 150, 100, 110, 44, 'red', ['k+1 c=0', 'B !'], 12, r=3)
f.lines(60, 180, ['group k의 c=12 = B.  compute: GEMM k c=12 t0 → GEMM k c=12 t1 (둘 다 B를 읽음)',
                  'load: 입력 k+1 c=0 → B.  이 LOAD가 받는 토큰은 GEMM k c=12 t0 것 → t1이 아직 B를 읽는 중에 덮어씀'], 14, MK['r'], 'start', 24)
f.text(40, 270, '시간 →', 13, GREY, 'start')
ys = lanes(f, 40, 280, ['compute', 'load'], 50, 1000, 120)
blk(f, 160, ys, 0, 0, 200, 'blue', 'GEMM k c=12 t0  (B)')
blk(f, 160, ys, 0, 200, 420, 'red', 'GEMM k c=12 t1  (B 읽는 중)')
blk(f, 160, ys, 1, 205, 420, 'red', '입력 k+1 c=0 → B 쓰기')
f.arrow([(360, ys[0] + 40), (365, ys[1] + 6)], 'r', 1.8)
f.text(600, ys[1] + 70, 'DeiT A x V (K 198 → 13 블록): 출력 783 / 12672 바이트가 달라짐, confidence 0.738 → 0.735. 시뮬레이터에서는 안 드러남', 13, MK['r'])
f.text(40, 470, '② 고친 뒤: 반쪽 순서를 group을 넘어 이어 감 (gAhead.inpPtr)', 16, INK, 'start', True)
e = chans(f, 60, 486, 0, 11)
f.text(e + 30, 514, '…', 16)
f.box(e + 60, 486, 74, 44, 'orange', ['k c=12', 'B'], 11, r=3)
f.box(e + 150, 486, 110, 44, 'green', ['k+1 c=0', 'A'], 12, r=3)
f.lines(60, 566, ['입력 k+1 c=0 → A: A를 마지막으로 읽은 것은 GEMM k c=11 (t0, t1). GEMM k c=12 t0의 토큰이 오면 둘 다 끝났다.',
                  '블록 수가 짝수면 전과 같은 배치다. run의 첫 group은 전처럼 B에서 시작한다.',
                  'uint32_t inputSramPtr = tokIn ? 0 : gAhead.inpPtr;   …   gAhead.inpPtr = inputSramPtr;'], 14, INK, 'start', 26)
f.save()
print('ok')
