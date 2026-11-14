# Area–latency design study: shared SIMD MAC V0 versus V1

This document describes the proposed area–latency tradeoff experiments for the exploratory SQuARE/FlexNPU arithmetic replacement. **There are no independently measured PPA results yet.** The scripts below generate generic synthesized gate-count and logic-path proxies, not physical area, clock period, or post-layout power.

## Objective
Replace the existing signed INT8 x INT8 -> INT32 PE datapath with a restricted-E4M3-compatible shared low-precision fabric supporting:
- one FP4 E2M1 product/issue;
- one Posit(4,1) product/issue;
- two independent FP2 E1M0 or E0M1 products/issue, **summed before one accumulation**;
- exactly one physical signed INT32 accumulator register per PE;
- no fixed Q8.8 intermediate, no floating-point accumulator, no SQuARE legacy 16-bit FP2 accumulator.

This is *not* full E4M3 FP8 multiplication; all finite values of the supported input formats can be represented exactly in E4M3, but the implementation exploits the much smaller native format sets. Signed INT32 accumulation necessarily quantizes fractional products. The `acc_unit_exp` input specifies the integer-unit exponent, and the current numerical policy is **truncation toward zero of each signed lane-reduced product contribution**, with modulo-2^32 accumulation overflow. FP2 lane cancellation is performed before truncation. No fractional residue persists between operations.

## Designs
| Variant | RTL source | Multiplier hardware | Scaling | INT32 register |
|---|---|---|---|---|
| INT8 | `rtl/mac.sv`, `mac_comp` | signed 8x8 | none | 1 |
| V0 | `rtl/square_partitioned_mac.sv` | 2 x 2x2 narrow cells | programmable signed unit exponent, 32-bit bidirectional shift representation | 1 |
| V1 | `rtl/square_partitioned_mac_v1.sv` | same two narrow cells | programmable unit; smaller exp logic, 6-bit right shift, 32-bit left shift | 1 |
| V1-static | V1 with `STATIC_UNIT=1`, `STATIC_UNIT_EXP=0` | same | compile-time integer unit (no runtime unit input cone) | 1 |

**The two multiplication cells are part of one reconfigurable MAC module.** Their logic is *not* claimed to be a single indivisible multiplier that does two products with zero overhead.

## V1 optimization hypotheses
1. **Narrower exponent control:** Posit exponents use signed 4-bit storage, FP4 3-bit, total exponent 5-bit, final exponent difference 7-bit instead of V0's 6/7/8-bit chain.
2. **Narrow right alignment:** signed FP2/FP4/Posit coefficient lies in [-18,18]. Right shifts use only six magnitude bits. Removes the V0 general 32-bit right-shift data cone.
3. **Sign handling after magnitude alignment:** shift unsigned magnitude, restore sign once. Compare whether the removed wide absolute-value path compensates for the output negation logic.
4. **Compile-time unit option:** V1-static permits constant propagation through the entire exponent-alignment network, at the expense of runtime scaling flexibility.
5. **Single accumulation path:** both scalar and dual-lane FP2 feed one INT32 adder/register.

The 32-bit left-shift output is still required for large positive unit-adjusted exponents. Therefore, we should not claim to have *eliminated* all barrel-shifter logic or proven an area/delay improvement without synthesis.

## Run the study locally / in CI
```bash
make square_mac_test       # V0 directed numerical test
make square_array_test     # 2x2 V0 systolic integration
make square_v1_equiv       # V0/V1 exhaustive encoded inputs + 10 unit scales
make square_v1_oracle      # V1 against independent exact-rational oracle
make square_ppa            # requires yosys; produces build/square_ppa logs/report
```
`make square_v1_equiv` compares 256 Posit code pairs, 256 FP4 code pairs, and all 64x64 packed independent dual FP2 code pairs for each of E1M0/E0M1 at ten unit exponents, giving 87,040 comparisons. The separate `make square_v1_oracle` target generates 87,040 oracle vectors using exact Python rational arithmetic and compares V1's RTL output to them. This checks the specified per-issue truncation policy independently, but does not establish application-level neural-network accuracy.

`make square_ppa` maps each top module to Yosys generic Boolean cells and obtains the `ltp -noff` longest topological path. Both are **architecture proxies**, not µm², ns, MHz, power, or energy. The signed INT8 baseline includes an 8x8 multiplier and so is not numerically equivalent to FP2/FP4/Posit modes: compare it as a resource reference, not bit-exact arithmetic competitor. For credible hardware PPA, use an identical target technology, constraints, clock, power methodology, and reporting conventions.

## Reading the area/delay Pareto result
For actual target-specific physical metrics, normalize to V0:
`area_ratio = Area(V1)/Area(V0)`,
`delay_ratio = Delay(V1)/Delay(V0)`,
`ADP_ratio = area_ratio * delay_ratio`.
If V1 has lower area but longer delay, it is an area/latency tradeoff; if both are lower, V1 dominates V0 on those metrics. V1-static is only directly useful where its unit exponent is appropriate for the layer.

## Integration
At the `compute_tile_bram` instance use `SQUARE_ENABLE=1`, `SQUARE_V1=1`, `ACT_WIDTH=8`, `ACC_WIDTH=32`. New `i_square_mode`, `i_square_unit_exp` ports latch at dispatch start and must remain consistent across K-chain dispatches. Include both V0/V1 files in source lists when selecting alternate versions.

The default `SQUARE_ENABLE=0` keeps the original INT8 MAC implementation. The new RTL shares the legacy systolic forwarding interface; a complete CTB regression with precision-encoded data and cross-dispatch accumulation is still necessary before production use. The original FIFO simultaneous push/pop and non-power-of-two depth corner cases also remain outside this optimization.

## Remaining priority work
- Validate all modes against an **independent real-number reference**, including NaR, zeros, negative truncation, and overflow.
- Confirm V1 integration in a 2x2 array, then 8x8 and 16x16 CTB, with mode/scale latching and correct skew.
- Obtain cell-library mapped ASIC area and STA, optionally FPGA LUT/DSP/fMAX/power, at equal constraints.
- Measure active switching under realistic CNN/Transformer streams and INT32 accumulation ranges.
- Explore Boolean-direct FP2 partial products, fixed-unit specialization, and optional pipelining if target frequency demands it.
