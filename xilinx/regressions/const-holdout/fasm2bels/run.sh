#!/usr/bin/env bash
# Independent check of the const-holdout case with fasm2bels
# (https://github.com/chipsalliance/f4pga-xc-fasm2bels): recover the netlist
# each bitstream really implements from its FASM, then simulate the recovered
# netlists next to the RTL with yosys's sim pass.
#
#   FASM2BELS=/path/to/f4pga-xc-fasm2bels   (with fasm2bels-nextpnr-fasm.patch applied)
#   PYTHON=/path/to/venv/bin/python         (fasm2bels installed)
#   DB_ROOT=/path/to/prjxray-db/artix7
#   OLD=/path/to/unfixed/build/top.fasm  FIXED=/path/to/fixed/build/top.fasm
#   ./run.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CASE="$HERE/.."
PYTHON=${PYTHON:-python3}
DB_ROOT=${DB_ROOT:?prjxray-db artix7 directory}
CONNDB=${CONNDB:-$HERE/build/xc7a100tcsg324.db}   # built on the first run, ~2 min
OLD=${OLD:-$CASE/build/final-old/top.fasm}
FIXED=${FIXED:-$CASE/build/final-fixed/top.fasm}
YOSYS=${YOSYS:-yosys}
OUT=$HERE/build
mkdir -p "$OUT"

recover() { # name fasm
    mkdir -p "$OUT/$1"
    "$PYTHON" -m fasm2bels --connection_database "$CONNDB" --db_root "$DB_ROOT" \
        --part xc7a100tcsg324-1 --top top --iostandard LVCMOS33 \
        --input_xdc "$CASE/top.xdc" --fasm_file "$2" --allow_orphan_sinks \
        --verilog_file "$OUT/$1/top.v" --xdc_file "$OUT/$1/top.xdc" > "$OUT/$1/fasm2bels.log" 2>&1
    echo "recovered $1 -> $OUT/$1/top.v"
}
recover old "$OLD"
recover fixed "$FIXED"

# The RAM32M as each bitstream implements it: ADDRC[4]/ADDRD[4] are the pins.
for n in old fixed; do
    echo "== $n"; grep -E "^\.ADDR[A-D]\(" "$OUT/$n/top.v" | sed -E 's/, CLBL.*//; s/\(\{/(/'
done

# RTL copy seeded as the bitstream is: nextpnr-xilinx drops the SRL16E INIT
# that yosys gives the three LFSR stages it folds into an SRL, so bit 10 of
# the seed reads 0 on the chip (a separate bug).
sed 's/16.hACE1/16'"'"'hA8E1/' "$CASE/top.v" > "$OUT/top_rtl.v"
CELLS=$("$YOSYS" -q -p 'help' >/dev/null 2>&1; dirname "$(command -v "$YOSYS")")/../share/yosys/xilinx/cells_sim.v
cat > "$OUT/sim.ys" <<EOS
read_verilog $CELLS
read_verilog $OUT/top_rtl.v
chparam -set N 1 top
rename top top_rtl
read_verilog $OUT/old/top.v
rename top top_old
read_verilog $OUT/fixed/top.v
rename top top_fixed
read_verilog $HERE/tb.v
hierarchy -top tb
proc
sim -clock clk -n 4000 -zinit -vcd $OUT/tb.vcd
EOS
"$YOSYS" -q -l "$OUT/sim.log" "$OUT/sim.ys" > /dev/null
"$PYTHON" "$HERE/compare_vcd.py" "$OUT/tb.vcd"
