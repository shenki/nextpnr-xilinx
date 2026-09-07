# const-holdout: a GND-tied LUTRAM address pin left unrouted

## What this case is

A single 16-deep, 6-bit FIFO in distributed RAM (`top.v`, parameter `N`
sets the number of chained FIFO stages, default 1). Yosys maps each stage
to one RAM32M with address bit 4 tied to GND on every lane (site pins A5,
B5, C5 and D5 of the SLICEM; D5 is the write address shared by all lanes).

On xc7 the GND net enters an INT tile only through `GFAN0` or `GFAN1`, and
each LUT input mux hangs off exactly one of them: A5 and B5 (IMUX8, IMUX24)
on `GFAN0`, C5 and D5 (IMUX31, IMUX47) on `GFAN1`. Both GFANs are also
ordinary fan-out wires for signals. `Arch::routeVcc()` fills the constant
sinks after the main router has finished and may only use what the signal
nets left free. Before the fix, a sink it could not reach was counted in
the log line

    $PACKER_GND_NET: 7/19 sinks bridged (12 left to main router; max BFS 50000)

and then dropped: nextpnr exited 0, the fasm had no entry for the IMUX, and
an unprogrammed xc7 input mux reads 1. Here the FIFO's own read-enable net
takes `GFAN1` of the RAM32M's tile (`INT_R_X3Y108` with the default seed),
the pointer and data nets hold the bounce wires, and C5 and D5 get nothing:
writes land at entries 16..31 while lanes A and B read entries 0..15.

The fix (`xilinx/route_const.cc`) drives every unreached constant sink from
a local constant LUT and re-runs the router, and any sink still unreached
is an error unless `--allow-const-holdouts` is given.

## Files

| file | purpose |
|---|---|
| `top.v` | the design, `N` chained 16x6 LUTRAM FIFOs |
| `top.xdc` | Arty A7 pins (xc7a35tcsg324 and xc7a100tcsg324 share them) |
| `part.txt`, `synth_flags` | device and `synth_xilinx` flags (`-nocarry` keeps CARRY4 DI pins out of the GND sink list, so every GND holdout is a LUTRAM pin) |
| `run.sh` | yosys, nextpnr, log summary, checker |
| `check_const_pins.py` | reads the `--write` JSON and requires every constant-0 LUTRAM address pin to be reached by its net's routing |
| `check.sh` | wrapper in the openXC7 demo-projects regression contract (`CASE_DIR`) |
| `sweep.sh` | the N x seed sweep used to pick the defaults |

## Running

    CHIPDB=/path/to/xc7a100tcsg324.bin ./run.sh
    NPNR=/path/to/nextpnr-xilinx CHIPDB=/path/to/chipdb-dir N=6 SEED=2 ./run.sh

Exit 0 means every constant LUTRAM address pin was delivered. Exit 1 means
the bug is present (the checker prints the broken slice and the missing
pins), or nextpnr itself failed.

| binary | result |
|---|---|
| unfixed (openXC7 bd9c74c5b90b and earlier) | nextpnr exits 0, log says `12 left to main router`, checker: `SLICE_X2Y108 A5=gnd B5=gnd C5=NO D5=NO -> BROKEN`, run.sh exits 1 |
| fixed | first fill pass reports 12 holdouts, `Constant holdouts: 1 local constant LUT(s) added, 1 re-route pass(es)`, checker: `C5=lut D5=lut -> OK`, run.sh exits 0 |
| fixed, `NEXTPNR_NO_CONST_LUT_DRIVERS=1` | `ERROR: 12 constant sink(s) could not be routed; the bitstream would be wrong.`, run.sh exits 1 |
| fixed, `NEXTPNR_NO_CONST_LUT_DRIVERS=1 NEXTPNR_ALLOW_CONST_HOLDOUTS=1` | nextpnr exits 0 with 12 warnings, checker BROKEN, run.sh exits 1 |

In the unfixed fasm the victim tile has `INT_R_X3Y108.IMUX8.GFAN0` and
`INT_R_X3Y108.IMUX24.GFAN0` but no `IMUX31` or `IMUX47` feature, the same
signature as the original failing designs.

## Cross-check with fasm2bels

`fasm2bels/run.sh` uses
[f4pga-xc-fasm2bels](https://github.com/chipsalliance/f4pga-xc-fasm2bels)
to recover, from each FASM, the netlist the bitstream really implements,
then simulates both recovered netlists next to the RTL with yosys's `sim`
pass (`fasm2bels/tb.v`, `fasm2bels/compare_vcd.py`). fasm2bels needs the
small patch in `fasm2bels/fasm2bels-nextpnr-fasm.patch`: it expects the
zero-bit `PRECYINIT.C0` and `DI1MUX` features that symbiflow always emits
and nextpnr leaves out, and it has no entry for the `IO_INT_INTERFACE`
tiles nextpnr routes through.

The two recovered netlists differ in eight lines, all in the RAM32M at
SLICE_X2Y108:

    unfixed:  .ADDRA({1'b0, ...})  .ADDRB({1'b0, ...})  .ADDRC({1'b1, ...})  .ADDRD({1'b1, ...})
    fixed:    .ADDRA({1'b0, ...})  .ADDRB({1'b0, ...})  .ADDRC({CLBLM_R_X3Y108_SLICE_X3Y108_BO6, ...})  .ADDRD({... same ...})

`SLICE_X3Y108_BO6` is the INIT-0 LUT the fix inserted. In the unfixed
bitstream the unprogrammed C5 and D5 input muxes read as 1, which is what
fasm2bels models them as.

Simulating 4000 cycles with the same stimulus:

| | unfixed netlist vs RTL | fixed netlist vs RTL |
|---|---|---|
| led[0] (empty), led[1] (full), led[2] (valid) | equal every cycle | equal every cycle |
| led[3] (xor of FIFO data) | differs on 2056 cycles, first at cycle 7 | equal every cycle |
| RAM32M lane A and B outputs (data bits 3:0) | never toggle | about 1000 toggles each |
| RAM32M lane C outputs (data bits 5:4) | identical to fixed | |

The flow control is intact and the data is wrong in the way the address
pins predict: writes go to entries 16..31, lanes A and B read entries
0..15 that were never written, lane C reads 16..31.

The comparison RTL is `top.v` with the LFSR seed's bit 10 cleared. Yosys
folds three LFSR stages into an SRL16E with INIT 100, and nextpnr-xilinx
writes an all-zero LUT INIT for that SRL, so on the chip (and in the
recovered netlist) that stage starts at 0. That is a second, unrelated
defect (SRL INIT lost) and is not part of this case.

## How the defaults were chosen

`sweep.sh` runs `run.sh` over a grid of `N` and seeds against an unfixed
binary (`NPNR=.../nextpnr-xilinx.old`). Hit rates on nextpnr-xilinx
0.9.3-10-g7cfd1e906823 with the xc7a100tcsg324 chipdb, one run per seed:

| N | seeds tried | runs with a broken RAM32M |
|---|---|---|
| 1 | 1..32 | 1 (seed 32) |
| 2 | 1..32 | 3 |
| 3 | 1..32 | 7 |
| 4 | 1..32 | 13 |
| 6 | 1..32 | 21 |
| 8 | 1..16 | 8 |
| 16 | 1..16 | 10 |
| 32 | 1..16 | 13 |
| 48 | 1..16 | 16 |
| 64 | 1..16 | 16 |

`N=1 SEED=32` is the committed default because it is the smallest design
that shows the failure. `N=48` reproduces on every seed tried and is the
choice when a robust demonstration matters more than a small one.

## Caveats

- The outcome depends on the part, the chipdb (prjxray-db revision and the
  bbaexport of the tree that built it), the nextpnr-xilinx version, the
  yosys version (0.68 here) and the seed. A placer or router change can
  move the FIFO to a tile where GND still fits, in which case the first
  fill pass has 0 holdouts and the run passes without exercising the fix.
  That is still a pass for the property under test. Set
  `REQUIRE_STIMULUS=1` to turn it into a failure, and re-run `sweep.sh` to
  find a new N and seed.
- Any extra nextpnr command-line option (even one that does not affect
  placement, such as `--allow-const-holdouts`) can change the result for
  the same seed, because option parsing interns new identifiers and shifts
  the hash order of everything created after it. Use the environment
  variables (`NEXTPNR_ALLOW_CONST_HOLDOUTS`, `NEXTPNR_NO_CONST_LUT_DRIVERS`)
  to compare modes on the same placement.
- Do not add `--verbose`: on this design both the fixed and the unfixed
  binaries abort in the verbose critical-path report
  (`common/timing.cc:1022`, `Assertion it != net->wires.end()`). That is a
  separate, pre-existing problem.
- For a port to openXC7/demo-projects: their runner passes no `--seed` and
  builds for other parts, so re-sweep `N` there and swap `top.xdc` and
  `part.txt`.
- The design runs on an Arty (LEDs show FIFO activity), but the test stops
  at the routed JSON and fasm on purpose.
