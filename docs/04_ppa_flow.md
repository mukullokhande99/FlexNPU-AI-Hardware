# Optional PPA sketch — proposed open-flow synthesis

This is a **proposed** minimal flow, not a shipped one. It is written
so that a project group who wants to add relative hardware-cost
numbers to a Lane A comparison can turn it on with a small, honest
amount of tooling. It is deliberately not part of `make onboarding`,
because it needs external tools that not every student will install.

## Goal

Provide *relative* PPA proxies — cell count, sequential vs
combinational logic, register count, and a rough timing slack —
across CTB realizations synthesized under identical open-flow
assumptions. That is enough to compare, e.g., CAP=32 vs CAP=24 or
16×16 vs 8×16, and to say *smaller* / *larger* / *tied* honestly.

This is **not** ASIC signoff. It does not model wire load, does not
close timing in a real PDK context, and is not directly transferable
to silicon area or power numbers. Interesting designs can be
evaluated with the full ASIC flow separately.

## Tooling

Two candidate combinations, both open and reproducible:

- **Yosys + Nangate 45 nm** open standard-cell library. Nangate 45
  is a synthetic library often used for teaching / benchmarking;
  it is compact, quick to synthesize against, and does not require
  a full PDK.
- **Yosys + SkyWater sky130 (`sky130_fd_sc_hd`)**. Real open PDK,
  slower to install, but supported by a wide open flow (OpenROAD,
  OpenLane) if a group later wants elaboration beyond synthesis.

For an in-class flow, **Yosys + Nangate 45** is the smallest,
easiest-to-install choice. `sky130` is available as a fallback for
groups that want it.

Install:

```
sudo apt install yosys                      # 0.10 or newer
```

Nangate 45 `.lib` files are downloadable in a few MB and referenced
by path in the synthesis script.

## The proposed flow

A single Yosys script under `synth/synth_ctb.ys` (not yet shipped):

```
read_verilog -sv rtl/mac.sv
read_verilog -sv rtl/fifo.sv
read_verilog -sv rtl/systolic.sv
read_verilog -sv rtl/mm.sv
read_verilog -sv rtl/compute_tile_bram.sv
hierarchy -check -top compute_tile_bram
synth -flatten -top compute_tile_bram
dfflibmap  -liberty NangateOpenCellLibrary_typical.lib
abc -liberty NangateOpenCellLibrary_typical.lib
stat -liberty NangateOpenCellLibrary_typical.lib -tech cmos
tee -o synth_ctb.rpt stat
```

A Makefile target (also not yet shipped):

```
synth_ctb:
	@yosys -q -c 'source synth/synth_ctb.ys'
```

Overrides for a Lane A comparison would pass different parameter
values on the Yosys command line so that CAP, DECOUPLE_MAX, array
dimensions, etc. can be swept under identical library assumptions,
then diffed:

```
make synth_ctb ARRAY_M=16 ARRAY_N=16 OUT_FIFO_DEPTH=32   # baseline
make synth_ctb ARRAY_M=16 ARRAY_N=16 OUT_FIFO_DEPTH=24   # candidate
diff synth_ctb_baseline.rpt synth_ctb_candidate.rpt
```

## What we honestly report

Under this flow a Lane A writeup can report:

- **Cell count** (`Number of cells`) — total and by cell type.
- **Sequential vs combinational split** (`Number of wires`,
  `Sequential elements`).
- **Register count** — the direct proxy for storage cost.
- **Chip-area estimate** in library-normalized µm² (Yosys `stat -tech
  cmos` gives this against a `.lib`) — comparable *between*
  configurations under the same library; not a real ASIC number.
- **A very rough timing slack** using `abc -liberty` with a target
  clock; usable for cross-configuration comparison, not for signoff.

## What we do not claim

- No power estimate (needs a real PDK and activity).
- No wire-load / place-and-route effect (needs full flow).
- No absolute silicon area — only relative µm² under the chosen
  open library.
- No claim of transferability to a specific ASIC node.

Write these caveats down in the project writeup.

## Sanity checks before trusting anything

1. Yosys must elaborate `compute_tile_bram` cleanly with no unresolved
   references. A first pass on the shipped RTL is expected to work
   because the exposed modules do not use SystemVerilog features
   Yosys refuses to synthesize (all four exposed modules are already
   iverilog- and Verilator-clean). If it does fail, log which
   construct and either downgrade the construct or exclude the file
   from the synth flow.
2. Cell counts and register counts across two configurations should
   *differ in the direction the mechanism predicts*. If they don't,
   suspect the flow before the mechanism.
3. Sanity-check by turning a Lane A dormant-provisioning question
   into a PPA question: does the register count actually decrease
   when the mechanistic model says the FIFO can be smaller?

## Adding the flow to your fork

If a group decides to enable this flow, do it in the fork:

- Add `synth/synth_ctb.ys` with the script above.
- Add a `synth_ctb` target to the Makefile.
- Commit a `synth/README.md` documenting which library file / version
  you used, and the exact hash of the RTL you synthesized against.

Then the PPA numbers in your writeup are reproducible.
