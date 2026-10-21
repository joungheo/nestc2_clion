#!/usr/bin/env python3
# #### JO added (2026-10-21): pulse the PS->PL reset pl_resetn0 (ZynqMP: PS GPIO bank 5 bit 31 = EMIO 95 =
# gpiochip(zynqmp_gpio) base + 78 + 95). The vtahw designs reset their HLS blocks/FIFOs from it through
# proc_sys_reset. Xlnk().xlnk_reset() only frees CMA buffers, it does not touch the PL.
# usage: sudo python3 plreset.py [n]   (n = 0..3 for pl_resetn0..3, default 0)
import glob
import os
import sys
import time

n = int(sys.argv[1]) if len(sys.argv) > 1 else 0
base = None
for chip in glob.glob('/sys/class/gpio/gpiochip*'):
    label = open(chip + '/label').read().strip()
    ngpio = int(open(chip + '/ngpio').read())
    if label == 'zynqmp_gpio' or ngpio == 174:
        base = int(open(chip + '/base').read())
        break
if base is None:
    sys.exit('no zynqmp_gpio gpiochip found')
pin = base + 78 + 95 - n
path = '/sys/class/gpio/gpio%d' % pin
if not os.path.exists(path):
    with open('/sys/class/gpio/export', 'w') as f:
        f.write(str(pin))
with open(path + '/direction', 'w') as f:
    f.write('out')
for v, wait in (('0', 0.01), ('1', 0.01)):
    with open(path + '/value', 'w') as f:
        f.write(v)
    time.sleep(wait)
print('pl_resetn%d (gpio %d, chip base %d) pulsed low, now %s' % (n, pin, base, open(path + '/value').read().strip()))
