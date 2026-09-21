# RTL and testbenches

## Files

- `rtl/mac.sv` — PE (mac_comp + systolic-forwarding wrapper).
- `rtl/systolic.sv` — `systolic_array` (generate block ARRAY_M x ARRAY_N).
- `rtl/mm.sv` — `mm` (array + input/output FIFOs, per-row output FIFO).
- `rtl/fifo.sv` — parameterizable synchronous FIFO.
- `rtl/compute_tile_bram.sv` — CTB top: 7-state main FSM, drain FSM,
  ping-pong ACT/WGT buffers, credit register, K-tile counter,
  `SWAIT_EARLY_EXIT`, `DRAIN_ISSUE_ON_RETURN`, per-K-tile drain.

Compile-time parameters exposed on `compute_tile_bram`:

| Parameter | Default | Meaning |
|---|---|---|
| `N` | 8 | Legacy shorthand; ARRAY_M/ARRAY_N default to this. |
| `ARRAY_M` | N | Systolic rows. |
| `ARRAY_N` | N | Systolic cols. |
| `ACT_WIDTH` | 8 | Activation datapath width. |
| `ACC_WIDTH` | 32 | Accumulator width (frozen). |
| `K_TILE` | 8 | K per pass. |
| `MAX_K` | 1024 | ACT buffer depth. |
| `ACT_FIFO_DEPTH` | 8 | A-side input FIFO depth. |
| `OUT_FIFO_DEPTH` | 32 | Per-row output FIFO depth (CAP). |
| `DECOUPLE_MAX` | 3 | Credit depth D. |
| `SWAIT_EARLY_EXIT` | 1 | Enable S_WAIT shortcut. |
| `SWAIT_EARLY_THRESHOLD` | 11 | S_WAIT shortcut trigger. |

## The two testbenches

### `tb/ctb_regime_tb.sv` — regime probe (iverilog)

Drives one dispatch and emits one line at end-of-simulation:

```
REGIME_JSON {"K": 1024, "D": 3, "service": 2316, "exec": 1344,
             "wait": 768, "next": 186, ..., "overflow_hazard": 0}
```

Parsed by `examples/03_rtl_vs_model_parity.py` for the sim-vs-RTL
comparison.

**Run via make:**
```bash
make ctb_eval K=1024 CONFIG=configs/ctb/baseline.yaml
```

The Makefile calls `tools/config_bridge.py --emit iverilog`, which
turns the YAML into `-P` overrides, then invokes iverilog + vvp.

**Run manually:**
```bash
ARGS=$(python3 tools/config_bridge.py --K 1024 --emit iverilog)
iverilog -g2012 $ARGS -o build/regime.vvp \
    tb/ctb_regime_tb.sv rtl/*.sv
vvp build/regime.vvp
```

### `tb/ctb_correctness.cpp` — byte-exact correctness (Verilator)

C++ testbench that drives four cases through the DUT and compares to
a numpy-style int64 golden matmul:

- **A** — K=1024, single dispatch.
- **B** — K=2048 as two chained 1024s (`acc=0,wb=0` then `acc=1,wb=1`).
- **C** — K=3000 as 1024+1024+952 (uneven last chunk).
- **D** — verifies `out_enb` is quiet on intermediate wb=0 chunks
  (no partial-sum leaks to downstream rescale).

Uses N=8 (an 8x8 array). Pass criterion: byte-equality on A/B/C and
zero out_enb pulses on D.

**Run:**
```bash
make ctb_correctness
```

Prints `ALL CASES PASS` on success.

## Numerical vs cycle observables — different testbenches

- The regime TB (`ctb_regime_tb.sv`) reports **cycle** observables:
  service, exec, wait, next, credit_stall_cyc, rp_peak, fifo_peak.
  It does not check output data.
- The correctness TB (`ctb_correctness.cpp`) checks **numerical**
  correctness against a golden matmul. It does not report per-state
  cycle counts.

Both are needed. `make ctb_correctness` is the onboarding numerical
gate; `make ctb_eval` and `make parity` are the cycle-level cross-check.
