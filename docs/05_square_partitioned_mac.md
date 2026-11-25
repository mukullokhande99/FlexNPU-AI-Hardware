# Exploratory SQuARE / FlexNPU partitioned MAC

Branch: `explore/square-partitioned-int32-mac`. The default (`SQUARE_ENABLE=0`) remains the original signed INT8 x INT8 -> INT32 design.

## What is implemented
- One physical signed 32-bit accumulator register per processing element, across all precision modes.
- Two **2x2-bit significand multiply cells**: cell P0 is reused for scalar FP4, Posit4 and SIMD FP2 lane 0; P1 is used only for FP2 lane 1. This is ONE reconfigurable *arithmetic block*, **not** a claim that one indivisible multiplier produces two independent results without extra hardware.
- FP4 E2M1 and Posit(4,1) decode into a restricted E4M3-compatible sign/significand/power-of-two representation; no actual 8-bit floating-point multiplication or FP8 rounding is performed.
- FP2 SIMD E1M0 and E0M1 produces two products per PE issue, reduced into **one** dot-product contribution.
- No Q8.8 product format, no independent lane accumulator, no fp32 accumulator.
- Signed integer result: `increment = trunc_toward_zero((A0*B0+A1*B1) / 2**acc_unit_exp)`, where `acc_unit_exp` is a **signed integer unit exponent**. Default `0` gives true integer units. In scalar mode lane 1 contributes zero. The integer dot-product register wraps on overflow.

## Important limitation (numerical semantics)
Fractional terms cannot be stored exactly in an unscaled INT32 integer accumulator. With `acc_unit_exp=0`, a contribution of 0.5 truncates to zero. Repeated subunit contributions do not recover the fraction. The control-side unit exponent allows a layer to choose a finer integer unit, but it is **not a hidden Q8.8 accumulator**, and it must stay constant throughout a logical dot product (including K-chain dispatches). No rounding-nearest, stochastic rounding, or dynamic range guard implemented. Lane reduction occurs before truncation in FP2, to retain cancellation.

## Encoding
`mode[1:0]`:
- `00`: scalar Posit(4,1), low nibble of each 8-bit operand; code 8 is NaR. NaR product is zeroed and `nar` asserted in the combinational product module; higher levels do not yet export this status.
- `01`: scalar FP4 E2M1, low nibble of each operand; `{sign,exp[1:0],fraction}`, including subnormals and zero.
- `10`: two FP2 E1M0 lanes.
- `11`: two FP2 E0M1 lanes.

For dual FP2: `[1:0]` code of lane 0, `[3:2]` code of lane 1, `[4]` lane 0 zero, `[5]` lane 1 zero. Both operands follow the same encoding. FP2 nonzero magnitudes: E1M0 = {1,0.5}, E0M1 = {1,1.5}. No packed-pair sharing/zero compression or block-scale support is claimed.

## Integration points
- `rtl/square_partitioned_mac.sv`: shared product generator and 32-bit accumulator.
- `rtl/mac.sv`: generate-select replacement under `SQUARE_ENABLE`.
- `rtl/systolic.sv`: passes precision mode and unit exponent to PEs.
- `rtl/mm.sv`: passes configuration through while preserving existing FIFO widths.
- `rtl/compute_tile_bram.sv`: latches `i_square_mode` and `i_square_unit_exp` on `start`, propagates them to `mm`.

For the alternate implementation set `SQUARE_ENABLE=1` at `compute_tile_bram`, use `ACT_WIDTH=8` and `ACC_WIDTH=32`, add `rtl/square_partitioned_mac.sv` to compilation, and connect both new input ports. The new configuration must remain unchanged across chained dispatches. External memories must hold encoded FP2/FP4/Posit values—not raw signed INT8 tensors.

## Verification
```bash
make square_mac_test
```
This runs a directed standalone Icarus testbench for scalar, SIMD, NaR and accumulator reset. **This is an exploratory implementation, not validated as full end-to-end FlexNPU GEMM.** The original compute pipeline uses an enable delay and a conservative drain timing model, and the new precision path needs integration-level skew/flow tests and cross-dispatch numerical checks.

## Research next steps
1. Maintain the exhaustive rational oracle and expand system-level data patterns.
2. Extend FP2 zero-flag and NaR coverage at array and CTB writeback; NaR status is not exported by the CTB.
3. Compare against an exact separated-format design under identical synthesis/STA constraints. Two 2x2 multiplier cells and control logic have measurable cost; **zero SIMD overhead is not assumed**.
4. Test overflow and numerical loss for layer-dependent unit scaling.
5. Re-anchor C++ / Python timing models after changes to PE critical path or pipeline latency; no automatic speedup claim.
6. Expand FIFO stress testing beyond the directed boundary and non-power-of-two depth regression.

## Scope
Experimental hardware research only; no modifications to the default branch or legacy MAC numerical behavior.


## Integration regression (draft)
`make square_gate` adds FIFO boundary, streaming MAC, and encoded 2x2, 8x8 and 16x16 CTB
writeback comparisons for V0 and V1 to the existing arithmetic checks.
The CTB reference exercises all four modes at unit exponents -8, -4, 0 and 4, including
single and chained dispatches, and changes the input controls after start to
check latching, including ignored start pulses while busy. Tests must pass on the actual PR head;
they do not establish target PPA, application accuracy, arbitrary backpressure,
or correctness for all sizes and scales. The FIFO accepts only a write on a
simultaneous request when empty, and only a read when full (no bypass).

For SQuARE, `ACT_FIFO_DEPTH` is a minimum: the actual input FIFO depth is
`max(ACT_FIFO_DEPTH, K_TILE)`. This preserves continuous issue for a whole K
tile and avoids the known deadlock when a smaller FIFO is requested. It uses
additional physical storage and does not implement arbitrary backpressure or
an exact-depth-two FIFO. The legacy path retains its configured depth.
