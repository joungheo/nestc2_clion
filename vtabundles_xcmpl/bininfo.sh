# #### JO added (2026-11-04): sourced by hw*/script/run_prof.sh, run_all.sh and load_bit.sh (they run in the hw dir: . ../bininfo.sh).
# bin_info <file in /lib/firmware>: what the bitstream is, read from its name the way vtalib reads it (pynq_driver.cc
# CheckLoadedBitstream: EVTA count; VTABundle.cpp evtaBiasReset / evtaRequant: features), see docs/evta_isa_ext.md.
bin_info() {
  local b=$1 n=1 f="" clk="" gemm vt
  [[ $b =~ ([0-9]+)evta ]] && n=${BASH_REMATCH[1]}
  [[ $b =~ multi([0-9]+) ]] && n=${BASH_REMATCH[1]}
  [[ $b =~ _([0-9]+)mhz ]] && clk="${BASH_REMATCH[1]} MHz"
  case $b in *_sa_*) gemm="systolic (RTL gemm_sa, vtalib-zcu102-systolic executables: -s _sys*)";;
             *) gemm="HLS (vta/vtalib executables: -s _prof / _timed)";; esac
  case $b in *_sa_*alu1*|*_sa_*bias*|*_sa_*_rq*) f="$f ALU-imm-II1";; esac
  case $b in *bias*|*_rq*) f="$f bias-reset";; esac
  case $b in *_rq*) f="$f requantize";; esac
  case $b in *_prof*) f="$f AXI-perf-monitor";; esac
  case $b in *hp333*) f="$f HP-333MHz";; esac
  echo "  bitstream : /lib/firmware/$b"
  if [ -f /lib/firmware/$b ]; then
    echo "              $(stat -c '%y' /lib/firmware/$b | cut -c1-16), md5 $(md5sum < /lib/firmware/$b | cut -c1-8)"
  else echo "              (not found)"; fi
  echo "  EVTAs     : $n   clock: ${clk:-not in the name}   GEMM: $gemm"
  echo "  features  :${f:- none beyond the base ISA}"
  # the switches vtalib reads at run time (EVTA_BIAS_RESET / EVTA_REQUANT override the name, docs/evta_user_manual.md)
  vt=$(env | grep -E '^(EVTA_|VTA_|MULTI_|BUNDLE_)' | sort | tr '\n' ' ')
  echo "  env       : ${vt:-no EVTA_* / MULTI_* / BUNDLE_* variables}"
}
