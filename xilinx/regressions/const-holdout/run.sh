#!/usr/bin/env bash
# Build the const-holdout case and check that every GND-tied LUTRAM address
# pin was routed.  Exit 0 = pass, 1 = bug present or the flow failed.
#
# Environment:
#   NPNR      nextpnr-xilinx binary (default: nextpnr-xilinx on PATH)
#   YOSYS     yosys binary (default: yosys on PATH)
#   CHIPDB    required: <part>.bin, or a directory holding <part>.bin
#   PART      device (default: part.txt)
#   N         number of FIFO stages (default: the swept value)
#   SEED      nextpnr seed (default: the swept value)
#   OUT       output directory (default: ./build next to this script)
#   NPNR_EXTRA        extra nextpnr arguments, e.g. --allow-const-holdouts
#   REQUIRE_STIMULUS  1: also fail when the first constant fill pass had no
#                     holdout, i.e. the stimulus no longer exercises the fix
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
NPNR=${NPNR:-nextpnr-xilinx}
YOSYS=${YOSYS:-yosys}
PART=${PART:-$(cat "$HERE/part.txt")}
N=${N:-1}
SEED=${SEED:-32}
OUT=${OUT:-$HERE/build}
CHIPDB=${CHIPDB:?set CHIPDB to <part>.bin or a directory of chipdb .bin files}
if [ -d "$CHIPDB" ]; then
    for c in "$CHIPDB/$PART.bin" "$CHIPDB/${PART%-*}.bin"; do
        [ -f "$c" ] && CHIPDB=$c && break
    done
fi
[ -f "$CHIPDB" ] || { echo "run.sh: chipdb $CHIPDB not found"; exit 1; }
mkdir -p "$OUT"

"$YOSYS" -q -l "$OUT/yosys.log" -p "read_verilog $HERE/top.v; chparam -set N $N top;
    synth_xilinx $(cat "$HERE/synth_flags") -family xc7 -top top;
    write_json $OUT/top.json" || { echo "run.sh: yosys failed"; exit 1; }
rams=$(grep -c '"type": "RAM32M"' "$OUT/top.json")
[ "$rams" -gt 0 ] || { echo "run.sh: yosys inferred no RAM32M"; exit 1; }
echo "yosys: N=$N stages, $rams RAM32M"

rm -f "$OUT/gnd_holdouts.txt"
NEXTPNR_LOG_CONST_HOLDOUTS=1 NEXTPNR_GND_HOLDOUT_FILE="$OUT/gnd_holdouts.txt" \
"$NPNR" --chipdb "$CHIPDB" --xdc "$HERE/top.xdc" --json "$OUT/top.json" \
    --write "$OUT/top_routed.json" --fasm "$OUT/top.fasm" \
    --seed "$SEED" --freq 100 --timing-allow-fail ${NPNR_EXTRA:-} \
    > "$OUT/nextpnr.log" 2>&1
rc=$?

# "(N left to main router" is the unfixed wording, "(N holdouts" the fixed one.
counts=$(grep -oE '\$PACKER_GND_NET: [0-9]+/[0-9]+ sinks bridged \([0-9]+ (left to main router|holdouts)' \
    "$OUT/nextpnr.log" | sed -E 's/.*\(([0-9]+) .*/\1/')
first=$(echo "$counts" | head -1)
last=$(echo "$counts" | tail -1)
rampins=$(grep -cE ' (A|WA)[1-6]$' "$OUT/gnd_holdouts.txt" 2>/dev/null)
rampins=${rampins:-0}
echo "nextpnr: seed=$SEED rc=$rc GND holdouts first pass=${first:-?} final=${last:-?}, LUTRAM pins in holdout file=$rampins"
grep -E "Constant holdouts:|constant sink\(s\) (could not be routed|left unrouted)" "$OUT/nextpnr.log" || true

if [ $rc -ne 0 ]; then
    echo "run.sh: nextpnr failed (rc=$rc)"
    grep -E "^ERROR" "$OUT/nextpnr.log" | tail -5
    exit 1
fi
[ -s "$OUT/top.fasm" ] || { echo "run.sh: empty fasm"; exit 1; }
python3 "$HERE/check_const_pins.py" "$OUT/top_routed.json" || exit 1
if [ "${REQUIRE_STIMULUS:-0}" = 1 ] && [ "${first:-0}" -eq 0 ]; then
    echo "run.sh: the first fill pass had no holdout; the stimulus drifted, re-run sweep.sh"
    exit 1
fi
echo PASS
