#!/usr/bin/env python3
"""Compare the LEDs of the RTL and the two recovered netlists in tb.vcd, and
count activity on the RAM32M lane outputs. Exit 1 if the fixed netlist ever
disagrees with the RTL, or if the unfixed one never does."""
import sys

LEDS = ("led_rtl", "led_old", "led_fixed")
LANES = {f"CLBLM_R_X3Y108_SLICE_X2Y108_{l}O{o}": f"lane {l} O{o}" for l in "ABCD" for o in (5, 6)}


def main(path):
    ids, scope, vals, rows, t = {}, [], {}, [], 0
    lane_last, lane_toggles = {}, {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("$scope"):
                scope.append(line.split()[2])
            elif line.startswith("$upscope"):
                scope.pop()
            elif line.startswith("$var"):
                p = line.split()
                code, name = p[3], p[4]
                if scope == ["tb"] and name in LEDS + ("clk",):
                    ids[code] = ("tb", name)
                elif len(scope) == 2 and scope[1] in ("u_old", "u_fixed") and name in LANES:
                    ids[code] = (scope[1], name)
                    lane_toggles[(scope[1], name)] = 0
            elif line[0] == "#":
                nt = int(line[1:])
                if vals.get("clk") == "0" and nt != t and all(k in vals for k in LEDS):
                    rows.append(tuple(int(vals[k], 2) for k in LEDS))
                t = nt
            else:
                if line[0] == "b":
                    v, code = line[1:].split()
                else:
                    v, code = line[0], line[1:]
                if code not in ids:
                    continue
                inst, name = ids[code]
                if inst == "tb":
                    vals[name] = v
                else:
                    k = (inst, name)
                    if k in lane_last and lane_last[k] != v:
                        lane_toggles[k] += 1
                    lane_last[k] = v

    n = len(rows)
    old_bad = sum(r[1] != r[0] for r in rows)
    fixed_bad = sum(r[2] != r[0] for r in rows)
    print(f"{n} cycles: unfixed netlist != RTL on {old_bad}, fixed netlist != RTL on {fixed_bad}")
    names = ["sw[2]^empty", "|full", "data valid", "^data"]
    for bit, name in enumerate(names):
        a = sum(((r[2] >> bit) & 1) == ((r[0] >> bit) & 1) for r in rows) / n
        b = sum(((r[1] >> bit) & 1) == ((r[0] >> bit) & 1) for r in rows) / n
        print(f"  led[{bit}] {name:12s} fixed==rtl {a:.3f}  unfixed==rtl {b:.3f}")
    print("RAM32M lane output toggles (lanes A/B carry data bits 3:0, C bits 5:4):")
    for k in sorted(lane_toggles):
        print(f"  {k[0]:8s} {LANES[k[1]]}: {lane_toggles[k]}")
    return 1 if fixed_bad or not old_bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
