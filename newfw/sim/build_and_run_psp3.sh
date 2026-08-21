#!/bin/sh
# PSP3 sim gate - nothing flashes unless this prints ALL PASS.
set -e
export PATH="$HOME/.local/bin:$PATH"
yowasp-yosys -q -p \
  "read_verilog ../rtl/pistorm_psp3.v; hierarchy -top pistorm_psp3_core; \
   proc; opt_clean; check -assert; write_cxxrtl psp3.cc"
RT=$(python3 -c "import amaranth_yosys,os;print(os.path.dirname(amaranth_yosys.__file__)+'/share/include/backends/cxxrtl/runtime')")
g++ -O1 -std=c++17 -I"$RT" -o tb_psp3 tb_psp3.cc
./tb_psp3
