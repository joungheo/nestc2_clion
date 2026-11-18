#!/usr/bin/env python3
# JO added (2026-11-19): the figures of docs/evta_conv_pool_guide.md. usage: python3 docs/figs/gen_evta_pool_figs.py docs/figs
# (style and helpers of docs/figs/gen_evta_ahead_figs.py)
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






# ---------------------------------------------------------------- P1: data flow before / after
f = Fig('evta_pool_01_flow', 1400, 560, '그림 P1. VGG16 첫 pool: conv 출력이 DRAM을 오가던 왕복이 없어진다')
f.text(40, 84, '① 전: conv 호출과 pool 호출이 따로 (maxpool_evta)', 16, INK, 'start', True)
f.box(40, 104, 220, 80, 'blue', ['EVTA conv', 'convolution_wo_tr'], 14)
f.box(370, 104, 200, 80, 'grey', ['DRAM', '32×32×64 = 64 KB'], 14)
f.box(680, 104, 220, 80, 'blue', ['EVTA pool', 'maxpool_evta'], 14)
f.box(1010, 104, 200, 80, 'grey', ['DRAM', '16×16×64 = 16 KB'], 14)
f.arrow([(260, 144), (370, 144)], 'r', 2.4); f.text(315, 134, 'STORE 64 KB', 12, MK['r'])
f.arrow([(570, 144), (680, 144)], 'r', 2.4); f.text(625, 134, 'LOAD 64 KB', 12, MK['r'])
f.arrow([(900, 144), (1010, 144)], 'k', 2); f.text(955, 134, 'STORE 16 KB', 12, INK)
f.text(1230, 140, 'EVTA run 2개', 14, MK['r'], 'start', True)
f.text(1230, 162, '+0.30 ms', 14, MK['r'], 'start')
f.text(40, 270, '② 후: 한 호출 (convolution_wo_tr_pool) 안에서 tile마다 conv → 2x2 MAX → STORE', 16, INK, 'start', True)
f.box(40, 290, 520, 150, 'blue', [], 14)
f.text(300, 316, 'EVTA conv + pool (tile 하나, acc 안)', 14, INK, 'middle', True)
f.box(60, 334, 140, 86, 'white', ['GEMM', 'conv 결과', '(int32)'], 13)
f.box(230, 334, 140, 86, 'green', ['ALU MAX ①', '가로 2개'], 13)
f.box(400, 334, 140, 86, 'green', ['ALU MAX ②', '세로 2개', '+ requantize'], 13)
f.arrow([(200, 377), (230, 377)], 'k', 1.6); f.arrow([(370, 377), (400, 377)], 'k', 1.6)
f.box(1010, 325, 200, 80, 'grey', ['DRAM', '16×16×64 = 16 KB'], 14)
f.arrow([(560, 365), (1010, 365)], 'g', 2.4); f.text(785, 355, 'STORE 16 KB (pool 영역만)', 13, MK['g'])
f.text(1230, 360, 'EVTA run 1개', 14, MK['g'], 'start', True)
f.text(700, 490, 'conv 출력 64 KB의 STORE · LOAD와 pool 호출 하나가 빠지고, conv의 STORE도 1/4이 된다. VGG16 OMP 1 7.01 → 6.75 ms.', 14, INK)
f.text(700, 520, '크기는 VGG16 첫 pool(MaxPool_4) 자리의 값이다. EVTA에서 돌던 나머지 pool 셋도 같은 방식이다.', 13, GREY)
f.save()

# ---------------------------------------------------------------- P2: finder rule before / after
f = Fig('evta_pool_02_rule', 1400, 740, '그림 P2. findPoolFusions의 조건: 전에는 Resnet conv0 하나만 맞았다')
def checks(f, x, y, title, col, items, w=630):
    f.box(x, y, w, 44, col, [title], 15)
    for i, (ok, t) in enumerate(items):
        yy = y + 60 + i * 46
        f.box(x, yy, w, 38, 'white', [], 13, r=4, sw=1)
        mark, c = {1: ('✔', 'g'), 0: ('✘', 'r'), 2: ('＋', 'b')}[ok]
        f.text(x + 22, yy + 25, mark, 16, MK[c], 'middle', True)
        f.text(x + 44, yy + 25, t, 13, INK, 'start')
checks(f, 40, 70, '전', 'grey', [
    (1, 'conv가 ConvPath::FirstBNN (conv0 pack 경로, 모델의 첫 conv)'),
    (1, 'relu가 붙어 있음'),
    (1, 'conv 출력을 읽는 것이 pool 하나뿐, pool이 EVTA maxpool'),
    (0, 'pool이 3x3 / stride 2 / pad 1  ← VGG16의 2x2 / 2 / 0은 여기서 탈락'),
    (1, 'schedule의 tile 높이 · 너비가 짝수'),
    (1, 'pool 출력 버퍼가 아직 할당 전이 아님'),
])
checks(f, 730, 70, '후', 'green', [
    (2, '첫 conv(위와 같음)  또는  일반 EVTA conv (ConvPath::VTA)'),
    (2, '  일반 conv: relu, 위쪽 clip 없음 (clipHi = 127)'),
    (2, '  일반 conv: residual / epilogue / 출력 LUT 융합이 없음'),
    (1, 'conv 출력을 읽는 것이 pool 하나뿐, pool이 EVTA maxpool'),
    (2, 'pool이 3x3 / 2 / 1  또는  2x2 / 2 / 0  (첫 conv는 3x3만)'),
    (2, 'tile이 짝수이고 출력 높이 · 너비를 나누어떨어뜨림'),
    (2, 'pool 출력이 EVTA layout (isPacked)'),
], 630)
f.box(40, 480, 1320, 230, 'white', [], 14)
f.lines(60, 510, ['왜 이 조건들인가',
                  '· relu: pool 영역을 0으로 채우고 MAX를 시작한다. 0과의 MAX = relu이므로 conv 출력이 relu를 거쳐야 같은 값이 된다.',
                  '· clipHi = 127: relu6처럼 위를 자르는 clip이면 requantize(0..127)만으로는 같지 않다.',
                  '· 다른 융합 없음: residual add · 출력 LUT도 tile 끝(STORE 전)에 붙는 일이라 한 tile에 둘을 섞지 않는다.',
                  '· tile 짝수 · 나누어떨어짐: 2x2 묶음 하나가 tile 두 개에 걸치지 않아야 tile 안에서 끝난다.',
                  '· isPacked: 출력을 EVTA layout(N{C/16}HW16)으로 쓴다. 다음 conv가 그대로 읽는다.',
                  '조건이 하나라도 안 맞으면 예전처럼 conv 호출 + maxpool_evta 호출이다 (vtalib도 한 번 더 확인하고 안 맞으면 둘로 나눈다).'],
        14, INK, 'start', 28, bold0=True)
f.save()

# ---------------------------------------------------------------- P3: the two MAX passes on one tile
f = Fig('evta_pool_03_passes', 1400, 760, '그림 P3. tile 안의 2x2 max pool: ALU MAX 두 번 (예: tile 4×4, 채널 블록 하나)')
L = 'abcdefghijklmnop'
def grid(f, x, y, cells, cw=78, ch=50, col='white', size=13, title=None):
    if title: f.text(x + cw * len(cells[0]) / 2, y - 14, title, 14, INK, 'middle', True)
    for i, row in enumerate(cells):
        for j, t in enumerate(row):
            c = col(i, j) if callable(col) else col
            f.box(x + j * cw, y + i * ch, cw, ch, c, [t] if t else [], size, r=0, sw=1, bold0=False)
grid(f, 60, 110, [[L[4 * i + j] for j in range(4)] for i in range(4)], title='conv 결과 (acc, int32, bias 포함)',
     col=lambda i, j: 'blue')
f.arrow([(390, 210), (450, 210)], 'k', 2)
f.text(420, 190, '①', 16, INK, 'middle', True)
grid(f, 460, 110, [[('%s∨%s' % (L[4 * i + j], L[4 * i + j + 1]) if j % 2 == 0 else L[4 * i + j]) for j in range(4)] for i in range(4)],
     title='① 가로: 열 2j ← max(열 2j, 열 2j+1)', col=lambda i, j: 'green' if j % 2 == 0 else 'grey', size=12)
f.arrow([(790, 210), (850, 210)], 'k', 2)
f.text(820, 190, '②', 16, INK, 'middle', True)
grid(f, 880, 135, [['a∨b∨e∨f', 'c∨d∨g∨h'], ['i∨j∨m∨n', 'k∨l∨o∨p']], 120, 76, 'orange', 13,
     title='② 세로 → pool 영역 (2×2)')
f.text(1120, 320, '∨ = max', 13, GREY, 'start')
f.box(40, 350, 1320, 380, 'white', [], 14)
f.lines(60, 380, ['ALU 명령 (pushPoolMax2: acc[out + i·outRow + j·outCol] = max(그 자리, acc[in + i·inRow + j·inCol]), 채널 블록 nF개)',
                  '',
                  '① 가로 1번:   out = base,      in = base + 1,       i < 4 행, j < 2,  outRow = inRow = TW,  outCol = inCol = 2',
                  '      → 각 행의 짝수 열이 오른쪽 이웃과의 max가 된다 (홀수 열은 그대로, 이제 안 쓴다)',
                  '② 세로 2번 (dy = 0, 1):  out = pooled,  in = base + dy·TW,  i < 2, j < 2,  outRow = 2, outCol = 1,  inRow = 2·TW, inCol = 2',
                  '      → pool[i][j] = max(pool[i][j], 행 2i+dy의 열 2j).  pool 영역은 먼저 0으로 reset되어 있다',
                  '',
                  '0에서 시작하므로 결과 = max(0, 네 값) = relu(max(네 값)).  relu 전의 int32 acc에 바로 해도 된다.',
                  '② 두 번에 requantize(rq: >> shift, 0..127로 clip, int8)를 켜 둔다. max와 requantize는 순서를 바꿔도 같으므로',
                  '      (둘 다 커지는 쪽을 지키는 함수) 마지막 값 = int8(max).  rq는 out 버퍼에 쓰고, STORE는 그 pool 영역만 내보낸다.',
                  '3x3 pool이면 가로 2번 + 세로 3번이고, tile 경계 밖 한 줄(halo)이 필요하다 (그림 P4).'],
        14, INK, 'start', 30, mono=False)
f.save()

# ---------------------------------------------------------------- P4: halo for 3x3, none for 2x2
f = Fig('evta_pool_04_halo', 1400, 600, '그림 P4. 2x2 / stride 2는 tile 안에서 끝나고, 3x3 / stride 2 / pad 1은 이웃 tile의 한 줄(halo)이 필요하다')
def strip(f, x, y, n, cw, ch, colf, label):
    f.text(x - 10, y + ch / 2 + 5, label, 14, INK, 'end', True)
    for j in range(n):
        f.box(x + j * cw, y, cw, ch, colf(j), [str(j)], 12, r=0, sw=1, bold0=False)
cw, ch = 56, 44
f.text(40, 90, '한 행만 보기: conv 출력 열 0..11, tile 너비 4 (tile 경계는 열 4와 8 앞)', 15, INK, 'start', True)
X0 = 260
strip(f, X0, 120, 12, cw, ch, lambda j: 'blue' if (j // 4) % 2 == 0 else 'purple', 'conv 출력')
for b in (4, 8):
    f.line(X0 + b * cw, 105, X0 + b * cw, 470, 'r', 2, True)
f.text(40, 230, '2x2 / s2 / p0', 15, INK, 'start', True)
for p in range(6):
    x = X0 + 2 * p * cw
    f.box(x + 3, 190, 2 * cw - 6, 50, 'green', ['pool %d' % p], 12, r=4)
f.text(X0 + 12 * cw + 20, 220, '창 = 열 2p, 2p+1: 늘 한 tile 안', 13, MK['g'], 'start')
f.text(40, 330, '3x3 / s2 / p1', 15, INK, 'start', True)
for p in range(6):
    a = 2 * p - 1
    x = X0 + max(a, 0) * cw
    w = (2 * p + 2 - max(a, 0)) * cw
    cross = p in (2, 4)
    y = 280 + (p % 2) * 58
    f.box(x + 3, y, w - 6, 48, 'red' if cross else 'orange', ['pool %d' % p + (' ← 열 %d 필요' % a if cross else '')], 12, r=4)
f.text(X0 + 12 * cw + 20, 330, '창 = 열 2p-1 .. 2p+1:', 13, MK['r'], 'start')
f.text(X0 + 12 * cw + 20, 352, '왼쪽 tile의 마지막 열이 필요', 13, MK['r'], 'start')
f.box(40, 420 + 40, 1320, 110, 'white', [], 14)
f.lines(60, 490, ['3x3: 둘째 tile부터 conv tile을 한 줄 · 한 열 더 계산한다 (haloH / haloW = tile 번호 > 0).',
                  '2x2: halo 없음.  코드: tp.haloH = pool->k == 3 && tileHIdx > 0;   (전: tp.haloH = tileHIdx > 0;)',
                  '맞는지 확인하는 최소 tile 조건도 halo 줄을 3x3일 때만 더한다: tileHSize + (k3 ? 1 : 0) >= evtaMinTile(MIN_H_SIZE).'],
        14, INK, 'start', 30)
f.save()

# ---------------------------------------------------------------- P5: acc entries per thread, pickNF
f = Fig('evta_pool_05_acc', 1400, 560, '그림 P5. thread 하나의 acc 자리: pool 영역도 채널 블록 수(nF)를 정할 때 세야 한다')
X0, W = 120, 1160
tot = 1024
def seg(a, b, col, ls):
    f.box(X0 + W * a / tot, 110, W * (b - a) / tot, 70, col, ls, 13, r=2)
nF, T, P = 2, 256, 64
seg(0, T * nF, 'blue', ['conv tile  T × nF = 256 × 2 = 512'])
seg(T * nF, T * nF + nF, 'yellow', [])
seg(T * nF + nF, T * nF + nF + P * nF, 'orange', ['pool  P × nF = 128'])
seg(T * nF + nF + P * nF, tot, 'grey', ['비어 있음'])
f.text(X0 + W * (T * nF + 1) / tot, 200, 'bias nF', 12, MK['y'])
f.text(X0 - 10, 152, 'acc', 15, INK, 'end', True)
for v in (0, 512, 642, 1024):
    f.text(X0 + W * v / tot, 100, str(v), 12, GREY)
f.text(700, 240, 'VGG16 첫 pool 자리의 conv: tile 8×32 (T = 256 행), thread 2개 → thread당 acc 1024 entry, pool 영역 P = 4×16 = 64', 13, GREY)
f.box(40, 280, 1320, 250, 'white', [], 14)
f.lines(60, 312, ['pickNF(rows): 한 번에 처리할 출력 채널 블록 수 nF (16 채널 = 1 블록)',
                  '전:  n = min(KN/vt/16, 1024 / (padT + yT + rows))           ← pool 영역이 빠짐',
                  '후:  n = min(KN/vt/16, 1024 / (padT + yT + rows + poolP))   poolP = pth × ptw (tile의 1/4)',
                  '      그 뒤 (KN/16)이 n × vt로 나누어떨어질 때까지 n을 줄인다.',
                  'VGG16 이 자리: 전 1024 / 257 = 3, 후 1024 / 321 = 3 → 둘 다 나누어떨어짐 때문에 2.  결과는 같았다.',
                  '하지만 다른 모양에서는 pool 영역이 thread 자리를 넘어 다음 thread의 acc를 덮을 수 있었다 (Resnet conv0도 그대로 2).'],
        14, INK, 'start', 34, bold0=True, mono=False)
f.save()
