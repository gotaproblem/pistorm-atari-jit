#!/bin/sh
# PSP2 verification gate.  Run before EVERY synthesis.  Exit 0 = may flash.
# Needs: pip install amaranth-yosys yowasp-yosys   (WASM, no root needed)
set -e
cd "$(dirname "$0")"
export PATH="$HOME/.local/bin:$PATH"

yowasp-yosys -Q -p \
  "read_verilog ../rtl/pistorm_psp2.v; hierarchy -top pistorm_psp2_core; \
   proc; opt_clean; check -assert; write_cxxrtl psp2.cc"

RT=$(python3 -c "import amaranth_yosys,os;print(os.path.dirname(amaranth_yosys.__file__)+'/share/include/backends/cxxrtl/runtime')")
g++ -O1 -std=c++17 -I"$RT" -o tb_psp2 tb_psp2.cc
./tb_psp2
