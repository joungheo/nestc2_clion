#!/usr/bin/env python3
# #### JO added (2026-10-25): the INT-tile PIPs that need a stuck-at-0 configuration cell of the lab ZCU102.
# The cells (vtabundles_xcmpl/hwtest/brambist/stuckchk.py) are bit offsets 2, 16-19, 28-30 of the 48-bit slot of tile
# row Y219 in every frame of clock-region row 3. A PIP of an INT tile in that row is lost on the board only if one of
# the bits it sets is at such an offset; the bits of each PIP come from Project U-Ray (prjuray-db, zynqusp/segbits_int.db,
# the same for every UltraScale+ INT tile). On the multi4 design blk_f this predicted every one of the 85 cells
# stuckchk found set (460 PIPs in the row, 85 of them on a stuck cell). Prints "<from wire> <to wire>" per PIP.
# usage: python3 stuckpips.py segbits_int.db > stuck_int_pips.txt
#        (curl -LO https://raw.githubusercontent.com/f4pga/prjuray-db/master/zynqusp/segbits_int.db)
import sys

STUCK = {2, 16, 17, 18, 19, 28, 29, 30}

for line in open(sys.argv[1]):
    f = line.split()
    if not f or not f[0].startswith('INT.PIP.'):
        continue
    _, _, dst, src = f[0].split('.', 3)
    if any(int(b.split('_')[1]) in STUCK for b in f[1:] if not b.startswith('!')):
        print(src, dst)
