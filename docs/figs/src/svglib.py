#!/usr/bin/env python3
# Small SVG writer for the EVTA figures (docs/figs/evta_*.svg): boxes, arrows, text, a title block.
from xml.sax.saxutils import escape

FONT = "'Noto Sans CJK KR', 'Noto Sans KR', 'Malgun Gothic', 'Apple SD Gothic Neo', sans-serif"
MONO = "'DejaVu Sans Mono', Consolas, monospace"

# colors: fill (light) / stroke (dark)
C = {
    "evta": ("#dce8f5", "#2f5f98"),     # EVTA hardware / EVTA call
    "cpu": ("#fde5cc", "#c06a10"),      # CPU work
    "mem": ("#eeeeee", "#777777"),      # buffers, DRAM
    "new": ("#dcefd6", "#3c8a35"),      # new / changed
    "gone": ("#fbdcdc", "#c0392b"),     # removed
    "hls": ("#ece2f4", "#6d4596"),      # HLS logic
    "rtl": ("#fff3c4", "#9a7b00"),      # RTL logic
    "note": ("#ffffff", "#999999"),
    "dram": ("#efe6d6", "#8a6d3b"),
}


def tw(s, size):
    """rough text width in px"""
    w = 0.0
    for ch in s:
        o = ord(ch)
        if o > 0x2e80:
            w += size * 1.0
        elif ch in "il.,:;|!'":
            w += size * 0.3
        elif ch.isupper() or ch in "mwMW%@":
            w += size * 0.68
        else:
            w += size * 0.56
    return w


class Svg:
    def __init__(self, w, h, title, subtitle=None):
        self.w, self.h = w, h
        self.el = []
        self.defs = []
        self.title = title
        self.rect(0, 0, w, h, fill="#ffffff", stroke="none")
        self.text(24, 38, title, 22, weight="bold")
        if subtitle:
            for i, line in enumerate(subtitle if isinstance(subtitle, list) else [subtitle]):
                self.text(24, 64 + 20 * i, line, 13.5, color="#444444")
        for name, color in [("a", "#333333"), ("b", "#2f5f98"), ("o", "#c06a10"), ("g", "#3c8a35"), ("r", "#c0392b"),
                            ("p", "#6d4596"), ("y", "#9a7b00"), ("k", "#888888")]:
            self.defs.append(f'<marker id="ar{name}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" '
                             f'orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="{color}"/></marker>')

    def add(self, s):
        self.el.append(s)

    def rect(self, x, y, w, h, fill="#ffffff", stroke="#333333", sw=1.2, rx=0, dash=None, opacity=None):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        o = f' fill-opacity="{opacity}"' if opacity is not None else ""
        self.add(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{rx}" fill="{fill}"{o} '
                 f'stroke="{stroke}" stroke-width="{sw}"{d}/>')

    def text(self, x, y, s, size=13, anchor="start", weight="normal", color="#222222", mono=False, italic=False):
        f = MONO if mono else FONT
        n = len(s) - len(s.lstrip(' '))
        s = '\u00a0' * n + s[n:]  # keep leading indentation (SVG collapses spaces)
        st = ' font-style="italic"' if italic else ""
        self.add(f'<text x="{x:.1f}" y="{y:.1f}" font-family="{f}" font-size="{size}" text-anchor="{anchor}" '
                 f'font-weight="{weight}" fill="{color}"{st} xml:space="preserve">{escape(s)}</text>')

    def lines(self, x, y, lines, size=13, anchor="start", color="#222222", lh=None, weight="normal", mono=False):
        lh = lh or size * 1.35
        for i, s in enumerate(lines):
            if isinstance(s, tuple):  # (text, weight)
                self.text(x, y + i * lh, s[0], size, anchor, s[1], color, mono)
            else:
                self.text(x, y + i * lh, s, size, anchor, weight, color, mono)

    def box(self, x, y, w, h, kind="evta", lines=(), size=13, title=None, tsize=None, rx=6, dash=None, sw=1.4,
            align="middle", lh=None, cross=False):
        fill, stroke = C[kind] if isinstance(kind, str) else kind
        self.rect(x, y, w, h, fill, stroke, sw, rx, dash)
        lh = lh or size * 1.35
        all_lines = ([(title, "bold")] if title else []) + list(lines)
        n = len(all_lines)
        if n:
            ty = y + h / 2 - (n - 1) * lh / 2 + size * 0.35
            tx = x + w / 2 if align == "middle" else x + 8
            for i, s in enumerate(all_lines):
                if isinstance(s, tuple):
                    self.text(tx, ty + i * lh, s[0], tsize or size, align, s[1], "#111111")
                else:
                    self.text(tx, ty + i * lh, s, size, align, "normal", "#111111")
        if cross:
            self.line(x + 4, y + 4, x + w - 4, y + h - 4, color="#c0392b", sw=2)
            self.line(x + w - 4, y + 4, x + 4, y + h - 4, color="#c0392b", sw=2)

    def line(self, x1, y1, x2, y2, color="#333333", sw=1.4, dash=None, arrow=None, arrow_start=False):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        m = ""
        if arrow:
            m += f' marker-end="url(#ar{arrow})"'
            if arrow_start:
                m += f' marker-start="url(#ar{arrow})"'
        self.add(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{color}" stroke-width="{sw}"{d}{m}/>')

    def arrow(self, x1, y1, x2, y2, color="a", sw=1.6, dash=None, both=False):
        col = {"a": "#333333", "b": "#2f5f98", "o": "#c06a10", "g": "#3c8a35", "r": "#c0392b", "p": "#6d4596",
               "y": "#9a7b00", "k": "#888888"}[color]
        self.line(x1, y1, x2, y2, col, sw, dash, color, both)

    def path(self, d, color="#333333", sw=1.4, fill="none", arrow=None, dash=None):
        m = f' marker-end="url(#ar{arrow})"' if arrow else ""
        da = f' stroke-dasharray="{dash}"' if dash else ""
        col = {"a": "#333333", "b": "#2f5f98", "o": "#c06a10", "g": "#3c8a35", "r": "#c0392b", "p": "#6d4596",
               "y": "#9a7b00", "k": "#888888"}.get(color, color)
        self.add(f'<path d="{d}" fill="{fill}" stroke="{col}" stroke-width="{sw}"{m}{da}/>')

    def section(self, y, label, x=24, w=None, color="#2f5f98"):
        w = w or self.w - 48
        self.rect(x, y, w, 30, "#f3f6fa", "#c8d3e0", 1, 4)
        self.text(x + 12, y + 21, label, 15.5, weight="bold", color=color)

    def legend(self, x, y, items, size=12.5):
        cx = x
        for kind, label in items:
            fill, stroke = C[kind]
            self.rect(cx, y - 11, 18, 14, fill, stroke, 1.2, 3)
            self.text(cx + 24, y, label, size)
            cx += 24 + tw(label, size) + 22

    def save(self, path):
        with open(path, "w") as f:
            f.write(f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" '
                    f'viewBox="0 0 {self.w} {self.h}" font-family="{FONT}">\n')
            f.write(f"<title>{escape(self.title)}</title>\n<defs>\n" + "\n".join(self.defs) + "\n</defs>\n")
            f.write("\n".join(self.el))
            f.write("\n</svg>\n")
