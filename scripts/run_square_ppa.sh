#!/usr/bin/env bash
# Area/depth experiment using generic Yosys synthesis.
# Requires yosys on PATH. No PDK or timing library is assumed.
set -euo pipefail
mkdir -p build/square_ppa
run() {
  local name="$1" input="$2" top="$3" setup="${4:-}"
  yosys -Q -T -l "build/square_ppa/${name}.log" -p "
      read_verilog -sv ${input};
      ${setup}
      hierarchy -check -top ${top};
      synth -top ${top};
      abc -g simple;
      clean;
      stat;
      ltp -noff;
  " > "build/square_ppa/${name}.stdout"
}
run int8 rtl/mac.sv mac_comp "chparam -set D_W 8 -set ACC_WIDTH 32 mac_comp;"
run square_v0 rtl/square_partitioned_mac.sv square_partitioned_accumulator
run square_v1 rtl/square_partitioned_mac_v1.sv square_partitioned_accumulator_v1
# Fix all unit exponents at compile-time: zero means genuine integer units.
# Removes the runtime unit-exponent input cone, may reduce gate count/depth.
run square_v1_static rtl/square_partitioned_mac_v1.sv square_partitioned_accumulator_v1 \
  "chparam -set STATIC_UNIT 1 -set STATIC_UNIT_EXP 0 square_partitioned_accumulator_v1;"
python3 scripts/square_ppa_summary.py
