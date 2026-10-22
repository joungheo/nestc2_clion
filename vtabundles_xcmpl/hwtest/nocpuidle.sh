#!/bin/bash
# #### JO added (2026-10-22): disable the A53 power-down idle states (keep WFI = state0) before using JTAG/ILA.
# With hw_server attached (its DAP polls the cores), a core woken from cpuidle power-down stalls at the ATF
# warm entry (0xfffea120) and Linux hangs silently within minutes. Lasts until reboot.
# usage: sudo ./nocpuidle.sh
for f in /sys/devices/system/cpu/cpu*/cpuidle/state[1-9]*/disable; do echo 1 > "$f"; done
grep -H . /sys/devices/system/cpu/cpu*/cpuidle/state*/disable
