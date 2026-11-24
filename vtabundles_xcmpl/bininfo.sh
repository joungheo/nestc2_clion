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
  #### JO added (2026-11-22): the ALU extension the systolic vtalib reads from "_alux<k>" (VTABundle.cpp VTAHasAluExt / 2 / 3,
  #### docs/evta_isa_ext.md 5.2): 1 = MUL..CLZ, LUT (layer norm / softmax / GELU table / avgpool on the EVTA), 2 = + II 1 slot loop,
  #### MADD / MULSHR / SHRV / SHRR, 3 = + SQACC / LUTC
  if [[ $b =~ _alux([0-9]*) ]]; then
    case ${BASH_REMATCH[1]:-1} in 1) f="$f ALU-ext1";; 2) f="$f ALU-ext2(+slot-II1,MADD..SHRR)";; *) f="$f ALU-ext${BASH_REMATCH[1]}(+slot-II1,MADD..SHRR,SQACC,LUTC)";; esac
  fi
  #### JO added (2026-11-23): "_amul" = ALU MUL with an immediate only (HLS ALUMUL=1; avgpool on the EVTA, VTAHasAluMul)
  case $b in *_amul*) f="$f ALU-MUL-imm";; esac
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

#### JO added (2026-11-24, docs/evta_user_manual.md "빌드 흔적"): exe_info <executable> <bitstream name>: who built the executable
#### and from what. The build stamp (build_stamp.cmake: vtalib's last commit, dirty, host, build dir), the model-compiler line of
#### the Dir's bundle .cpp (VTASave.cpp compilerStamp) and a warning when the bitstream has a feature the executable's vtalib
#### predates (the commit where vtalib-zcu102-systolic started to use it, docs/evta_isa_ext.md section 5). Runs in hw*/ (git -C ..).
exe_info() {
  local e=$1 b=$2 st c have need="" x w=""
  st=$(LC_ALL=C grep -a -o -m1 'NESTC_BUILD [ -~]*' "$e" 2>/dev/null | head -1 | sed 's/".*//')
  echo "  exe       : $e   built $(stat -c '%y' "$e" | cut -c1-16)"
  if [ -n "$st" ]; then echo "              ${st#NESTC_BUILD }"
  else echo "              no build stamp (linked before 2026-11-24, or not by vtabundles_xcmpl)"; fi
  c=$(for f in $(dirname "$e")/*.cpp; do case $f in *[Mm]ain*.cpp) ;; *) grep -h -m1 '^// model-compiler:' "$f";; esac; done 2>/dev/null | head -1)
  [ -n "$c" ] && c=${c#// model-compiler: } || c=".cpp without a model-compiler line (generated before 2026-11-24)"
  echo "  bundle    : $c"
  [[ $st =~ vtalib=vtalib-zcu102-systolic\ ([0-9a-f]+) ]] || return 0
  have=${BASH_REMATCH[1]}
  case $b in *_sa_*) need="$need 3982d46";; esac
  case $b in *bias*|*_rq*) need="$need 76aa60e";; esac
  case $b in *_rq*) need="$need d26c3e3";; esac
  if [[ $b =~ _alux([0-9]*) ]]; then
    case ${BASH_REMATCH[1]:-1} in 1) need="$need e1a7924";; 2) need="$need c50e968";; *) need="$need 8191320";; esac
  fi
  case $b in *_amul*) need="$need 398f358";; esac
  for x in $need; do git -C .. merge-base --is-ancestor $x $have 2>/dev/null; [ $? = 1 ] && w="$w $x"; done
  [ -n "$w" ] && echo "  WARNING   : vtalib $have predates$w that $b needs: rebuild the executable (the bitstream's features stay unused)"
  return 0
}
