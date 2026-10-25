#!/usr/bin/env python3
# #### JO added (2026-10-25): floorplan figures of the multi4 build for docs/vta_synth_notes.md.
# input: CSV of FloorDump.java (type,site,tile column,tile row,user: EVTA index / s(hared) / -)
# The die is drawn with one bar per site column (CLB, BRAM, DSP; the routing columns between them are left out) and the
# SLICE Y coordinate upwards (a RAMB36 spans 5 SLICE rows, a DSP48E2 2.5).
# usage: python3 floorplan_svg.py <floor.csv> <out_dir>   -> zcu102_y219.svg (device, row Y219, pblocks),
#                                                            multi4_placement.svg (sites used by each EVTA)
import collections
import csv
import re
import sys

CW, RH = 6, 2             # px per site column, per SLICE row
BIN = 5                   # SLICE rows per drawn cell
COL = {'0': '#1f77b4', '1': '#ff7f0e', '2': '#2ca02c', '3': '#9467bd', 's': '#333333'}
TYPE = {'L': '#d0d0d0', 'B': '#e8a0a0', 'D': '#a0b8e8'}
NY = 420


def yslice(site):
    t = site.split('_X')[0]
    y = int(re.search(r'Y(\d+)$', site).group(1))
    return y * 5 if t == 'RAMB36' else y * 5 // 2 if t == 'DSP48E2' else y


def main():
    sites = [(t, n, int(c), int(r), u) for t, n, c, r, u in csv.reader(open(sys.argv[1])) if t in 'LBD']
    out = sys.argv[2]
    cols = sorted({s[2] for s in sites})
    cx = {c: i for i, c in enumerate(cols)}
    W, H = len(cols) * CW, NY * RH
    ctype = {s[2]: s[0] for s in sites}
    cmin = collections.defaultdict(lambda: NY)
    cells = collections.defaultdict(collections.Counter)   # (col, bin) -> users
    for t, n, c, r, u in sites:
        y = yslice(n)
        cmin[c] = min(cmin[c], y)
        cells[(c, y // BIN)][u] += 1

    def Y(y):              # SLICE y -> svg y (top of that row)
        return H - (y + 1) * RH

    def rect(x, y, w, h, fill, stroke='none'):
        return '<rect x="%d" y="%d" width="%d" height="%d" fill="%s" stroke="%s"/>' % (x, y, w, h, fill, stroke)

    def page(title, by_user):
        o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" font-family="sans-serif" font-size="12">'
             % (W + 300, H + 30, W + 300, H + 30), '<title>%s</title>' % title, rect(0, 0, W + 300, H + 30, 'white')]
        for c in cols:
            x = cx[c] * CW
            o.append(rect(x, Y(NY - 1), CW - 1, (NY - cmin[c]) * RH, TYPE[ctype[c]]))
            if by_user:
                for b in range(NY // BIN):
                    u = cells.get((c, b))
                    if not u:
                        continue
                    k, v = max(((k, v) for k, v in u.items() if k != '-'), key=lambda kv: kv[1], default=('-', 0))
                    if k != '-':
                        o.append(rect(x, Y(b * BIN + BIN - 1), CW - 1, BIN * RH, COL[k]))
        # PS: the columns that start above Y0
        ps = [cx[c] for c in cols if cmin[c] > 0]
        top = min(cmin[c] for c in cols if cmin[c] > 0)
        o.append(rect(min(ps) * CW, Y(top - 1), (max(ps) - min(ps) + 1) * CW, top * RH, '#f4f4f4', '#888'))
        o.append('<text x="%d" y="%d" font-size="16" fill="#444">PS (Zynq MPSoC)</text>' % (min(ps) * CW + 20, Y(top // 2)))
        o.append(rect(0, Y(219), W, RH, 'red'))
        o.append('<text x="%d" y="%d" fill="red" font-weight="bold">row Y219 (stuck-at-0 cells)</text>' % (W + 10, Y(219) + 5))
        for y in (0, 100, 200, 300, 419):
            o.append('<text x="%d" y="%d" fill="#666" font-size="10">Y%d</text>' % (W + 10, max(Y(y) + 4, 12), y))
        o.append('<text x="0" y="%d" fill="#666" font-size="10">X0 (left edge) ... one bar per CLB / BRAM / DSP column ... right edge</text>' % (H + 20))
        return o

    o = page('ZCU102 row Y219 and pblocks', False)
    allx = (0, W)
    for (y0, y1), name, col in (((0, 218), 'pb_evta_0, pb_evta_1: Y0..218', '#1f77b4'), ((220, 419), 'pb_evta_2, pb_evta_3: Y220..419', '#2ca02c')):
        o.append('<rect x="%d" y="%d" width="%d" height="%d" fill="none" stroke="%s" stroke-width="3" stroke-dasharray="10 5"/>'
                 % (allx[0] + 2, Y(y1) + 2, W - 4, (y1 - y0 + 1) * RH - 4, col))
        o.append('<text x="%d" y="%d" fill="%s" font-size="14" font-weight="bold">%s</text>' % (W - 260, Y(y1) + 20, col, name))
    lx = W + 10
    for i, (t, name) in enumerate((('L', 'CLB (SLICE) column'), ('B', 'BRAM (RAMB36) column'), ('D', 'DSP48E2 column'))):
        o.append(rect(lx, 20 + 22 * i, 14, 14, TYPE[t]) + '<text x="%d" y="%d">%s</text>' % (lx + 20, 32 + 22 * i, name))
    o.append('<text x="%d" y="110">pb_fence = both rectangles;</text>' % lx)
    o.append('<text x="%d" y="126">pblocks span the whole die width</text>' % lx)
    o.append('</svg>')
    open(out + '/zcu102_y219.svg', 'w').write('\n'.join(o))

    o = page('multi4 placement per EVTA', True)
    cnt = collections.Counter((u, t) for t, n, c, r, u in sites if u != '-')
    lx = W + 10
    for i, u in enumerate('0123s'):
        name = 'EVTA %s' % u if u != 's' else 'shared (AXI interconnect, reset)'
        o.append(rect(lx, 20 + 40 * i, 14, 14, COL[u]) + '<text x="%d" y="%d">%s</text>' % (lx + 20, 32 + 40 * i, name))
        o.append('<text x="%d" y="%d" font-size="11" fill="#444">%d SLICE, %d RAMB36, %d DSP</text>' % (lx + 20, 47 + 40 * i, cnt[(u, 'L')], cnt[(u, 'B')], cnt[(u, 'D')]))
    o.append('</svg>')
    open(out + '/multi4_placement.svg', 'w').write('\n'.join(o))


if __name__ == '__main__':
    main()
