#!/usr/bin/env python3
"""Figures of docs/mobilenetv2_evta_paper.md (measured 2026-11-27, vtabundles_xcmpl/ablation_mnv2.sh, 3 rounds averaged).

python3 docs/figs/src/make_mnv2_paper_figs.py   -> docs/figs/mnv2_paper_01_ladder.svg, mnv2_paper_02_loo.svg
"""
import math
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
COL = {"start": "#6b7280", "comp": "#2563eb", "rt": "#16a34a", "hw": "#dc2626", "in": "#9333ea"}
NAME = {"comp": "컴파일러", "rt": "런타임", "hw": "하드웨어", "in": "입력"}
FONT = "font-family='Noto Sans CJK KR, Noto Sans KR, sans-serif'"

# (label, ms, kind) in the order of the ladder (mobilenetv2_evta_paper.md table 3)
LADDER = [
    ("c0 모두 끔", 487.02, "start"),
    ("+ 24채널을 32로", 224.44, "comp"),
    ("+ 전처리 합치기", 224.43, "comp"),
    ("+ conv0 묶기", 77.17, "comp"),
    ("+ EVTA layout 유지", 59.84, "comp"),
    ("+ logits를 EVTA fc로", 54.59, "comp"),
    ("+ depthwise를 EVTA로", 43.51, "comp"),
    ("+ residual add 합치기", 47.78, "comp"),
    ("+ invalidate 빼기", 47.28, "comp"),
    ("+ buffer pool", 40.60, "rt"),
    ("+ record / replay", 35.00, "rt"),
    ("+ conv0 입력 stream", 34.25, "rt"),
    ("+ DMA 묶기", 29.27, "rt"),
    ("+ LOAD 앞당기기", 27.42, "rt"),
    ("+ 명령 사본, thread별 pool", 27.21, "rt"),
    ("+ 1×1 weight 한 LOAD", 27.03, "rt"),
    ("ALU imm II 1", 23.71, "hw"),
    ("RMW bias reset", 21.57, "hw"),
    ("out 쓰기 requantize", 13.90, "hw"),
    ("ALU 확장 1", 13.42, "hw"),
    ("ALU 확장 2", 13.08, "hw"),
    ("ALU 확장 3", 13.08, "hw"),
    ("depthwise 창 장치 (387.5 MHz)", 11.71, "hw"),
    ("+ 다음 그림 quantize 겹치기", 11.62, "in"),
]

# leave one out on _alux3_dw 387.5 MHz (all on: 11.70 ms): (label, ms with it off, kind)
LOO_ALL = 11.70
LOO = [
    ("24채널을 32로", 279.54, "comp"),
    ("conv0 묶기", 165.04, "comp"),
    ("EVTA layout 유지", 47.06, "comp"),
    ("depthwise를 EVTA로", 31.19, "comp"),
    ("record / replay", 27.03, "rt"),
    ("out 쓰기 requantize", 19.87, "hw"),
    ("buffer pool", 18.92, "rt"),
    ("DMA 묶기", 17.18, "rt"),
    ("logits를 EVTA fc로", 17.02, "comp"),
    ("depthwise 창 장치", 13.38, "hw"),
    ("전처리 합치기", 13.32, "comp"),
    ("LOAD 앞당기기", 13.05, "rt"),
    ("RMW bias reset", 12.43, "hw"),
    ("residual add 합치기", 12.42, "comp"),
    ("conv0 입력 stream", 12.33, "rt"),
    ("ALU 확장", 12.25, "hw"),
    ("invalidate 빼기", 12.17, "comp"),
    ("명령 사본", 11.90, "rt"),
    ("1×1 weight 한 LOAD", 11.86, "rt"),
]


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;")


def bars(rows, title, xlabel, value_of, label_of, lo, hi, ticks, fname, legend):
    W, left, right, top, rowh = 860, 250, 120, 64, 22
    H = top + rowh * len(rows) + 70
    pw = W - left - right
    x = lambda v: left + pw * (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo))
    o = [f"<svg xmlns='http://www.w3.org/2000/svg' width='{W}' height='{H}' viewBox='0 0 {W} {H}' {FONT}>",
         f"<rect width='{W}' height='{H}' fill='#ffffff'/>",
         f"<text x='{W / 2}' y='26' font-size='17' font-weight='bold' text-anchor='middle' fill='#111827'>{esc(title)}</text>"]
    for t in ticks:
        o.append(f"<line x1='{x(t):.1f}' y1='{top - 6}' x2='{x(t):.1f}' y2='{top + rowh * len(rows)}' stroke='#e5e7eb'/>")
        o.append(f"<text x='{x(t):.1f}' y='{top + rowh * len(rows) + 16}' font-size='11' text-anchor='middle' fill='#4b5563'>{t:g}</text>")
    o.append(f"<text x='{left + pw / 2}' y='{top + rowh * len(rows) + 34}' font-size='12' text-anchor='middle' fill='#374151'>{esc(xlabel)}</text>")
    for i, r in enumerate(rows):
        y = top + i * rowh
        v = value_of(r)
        o.append(f"<text x='{left - 8}' y='{y + 15}' font-size='12' text-anchor='end' fill='#111827'>{esc(r[0])}</text>")
        o.append(f"<rect x='{left}' y='{y + 4}' width='{max(1.0, x(v) - left):.1f}' height='{rowh - 7}' fill='{COL[r[2]]}' rx='2'/>")
        o.append(f"<text x='{x(v) + 5:.1f}' y='{y + 15}' font-size='11' fill='#374151'>{esc(label_of(r))}</text>")
    lx = left
    for k in legend:
        o.append(f"<rect x='{lx}' y='38' width='12' height='12' fill='{COL[k]}' rx='2'/>")
        o.append(f"<text x='{lx + 17}' y='48' font-size='12' fill='#374151'>{NAME.get(k, '출발점')}</text>")
        lx += 90
    o.append("</svg>")
    with open(os.path.join(OUT, fname), "w") as f:
        f.write("\n".join(o) + "\n")


bars(LADDER, "MobileNetV2Tf 1장 시간: 하나씩 켜기 (EVTA 1개, 같은 때 3번 평균)", "1장 시간 (ms, log)",
     lambda r: r[1], lambda r: f"{r[1]:.2f} ms", 10, 600, [10, 20, 50, 100, 200, 500], "mnv2_paper_01_ladder.svg",
     ["start", "comp", "rt", "hw", "in"])
bars(LOO, f"다 켠 상태({LOO_ALL:.2f} ms)에서 하나만 끄면 늘어나는 시간 (_alux3_dw 387.5 MHz)", "늘어난 시간 (ms, log)",
     lambda r: r[1] - LOO_ALL, lambda r: f"+{r[1] - LOO_ALL:.2f} ms", 0.1, 400, [0.1, 1, 10, 100], "mnv2_paper_02_loo.svg",
     ["comp", "rt", "hw"])
