#!/usr/bin/env bash
# Sweep N (FIFO stages) and the nextpnr seed to find a small design that
# leaves a GND-tied LUTRAM address pin unrouted on an unfixed nextpnr-xilinx.
# Writes one line per run to $OUT/summary.tsv:
#   N seed rc first_pass_holdouts ram_pins_in_holdout_file broken_slices slices
#
#   NPNR=/path/to/unfixed/nextpnr-xilinx CHIPDB=/path/to/xc7a100tcsg324.bin ./sweep.sh
#   NS="8 16 32" SEEDS="1 2 3" JOBS=8 ./sweep.sh
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT=${OUT:-$HERE/build/sweep}
NS=${NS:-8 12 16 24 32 48 64}
SEEDS=${SEEDS:-$(seq 1 16)}
JOBS=${JOBS:-32}
export NPNR YOSYS CHIPDB PART
mkdir -p "$OUT"
: > "$OUT/summary.tsv"

one() {
    n=$1; seed=$2
    dir="$OUT/N${n}_S${seed}"
    N=$n SEED=$seed OUT="$dir" "$HERE/run.sh" > "$dir.log" 2>&1
    rc=$?
    first=$(sed -nE 's/.*holdouts first pass=([0-9?]+).*/\1/p' "$dir.log" | head -1)
    rampins=$(sed -nE 's/.*holdout file=([0-9]+).*/\1/p' "$dir.log" | head -1)
    read -r slices _ _ broken _ < <(grep -E '^[0-9]+ RAM32M slices, [0-9]+ broken' "$dir.log" || echo "? x x ? x")
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$n" "$seed" "$rc" "${first:-?}" "${rampins:-?}" "${broken:-?}" "${slices:-?}"
}
export -f one
export HERE OUT
for n in $NS; do for s in $SEEDS; do echo "$n $s"; done; done \
    | xargs -P "$JOBS" -n 2 bash -c 'one "$@"' _ >> "$OUT/summary.tsv"
sort -n -k1,1 -k2,2 "$OUT/summary.tsv" -o "$OUT/summary.tsv"
echo "N	seed	rc	first_holdouts	ram_pins	broken	slices"
cat "$OUT/summary.tsv"
echo
echo "hits per N (runs with a broken slice):"
awk -F'\t' '$6 ~ /^[0-9]+$/ { t[$1]++; if ($6 > 0) h[$1]++ } END { for (n in t) printf "  N=%s: %d/%d\n", n, h[n]+0, t[n] }' "$OUT/summary.tsv" | sort -t= -k2 -n
