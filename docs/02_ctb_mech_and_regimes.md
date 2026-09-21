# CTB mechanistic simulator + regime classification

The Python simulator at `simulator/ctb_mech/ctb_mech.py` models the six
mechanism layers from `docs/01_architecture_overview.md` cycle-by-cycle,
without simulating RTL. It's used for breadth: sweeps over K, D, CAP,
drain policy, geometry are cheap; the RTL is used only for validation
at selected points.

## Data classes

- `CTBParams` (`ctb_mech.py:31`) — all inputs. Workload (K, K_TILE),
  geometry (ARRAY_M, ARRAY_N), FSM cycle constants (T_execute, T_wait,
  T_next_unblocked), credit (D), drain (drain_ii, drain_tail,
  drain_start_delay), storage (CAP), row-materialization schedule
  (pipe_row15, row_skew).
- `CTBResult` (`ctb_mech.py:87`) — every observable: service, exec,
  wait, next, credit_stall_cyc, rp_peak, rp_hist, fifo_peak_all,
  fifo_peak_per_row, overflow, L_first, L_last, regime, and both
  pass_end and drain_end timestamp lists.
- `CTBMechanistic` (`ctb_mech.py:112`) — the simulator itself.
  `.run()` returns a `CTBResult`.

## Regime classification

Implemented at the bottom of `CTBMechanistic.run()`. The rough decision
tree:

1. If any per-row FIFO ever exceeds CAP -> `storage-unsafe`.
2. If `rp_peak < D` and the workload is short (a "dormant" heuristic
   involving `num_passes`) -> `dormant`.
3. If `credit_stall_cyc` is a large fraction of service and
   `rp_peak == D` -> `credit-limited`.
4. If drain-side handshake accounts for most of service -> `drain-limited`.
5. Otherwise -> `producer-limited`.

The exact thresholds are in the code; students inspecting the code may
notice the classifier is deliberately conservative — it will call
"drain-limited" what an intuitive reading might call "credit-limited"
at the boundary. Do not use the label alone; look at the counters.

## Regression tests

`simulator/ctb_mech/test_regimes.py` locks in 8 canonical points:

- K=1024, D=3 (baseline).
- K=1024, D=1 (credit tight).
- K=32 (dormant).
- Storage-unsafe boundary probes at K=608 (safe) and K=624 (overflow)
  at D=4.
- L_first / L_last constants.
- rp_peak invariants across drain_ii.

Run: `python3 -m pytest simulator/ctb_mech/test_regimes.py`. Expected:
8/8 pass.

## Calibration data

`simulator/ctb_mech/data/regime_sweep_reference.csv` is a K x D
sweep captured against iverilog runs of `tb/ctb_regime_tb.sv` — the
same TB you drive via `make ctb_eval`. Rows: 30 cells (5 K values x
6 D values) plus a K=608 overflow probe. Columns include `service`
(RTL cycles), `rp_peak`, `fifo_peak_all`, `credit_stall_cyc`,
`overflow_hazard`.

Students may compare their modified mechanism model against this
reference to see whether their changes improved or regressed parity.

## Extending the model

The mechanism model is deliberately readable. Common extensions
students may need:

- **Re-anchoring** `T_execute`, `T_wait`, `pipe_row15` for a new
  geometry or K_TILE — Lane A A4.
- **Adding a static-power surrogate** — Lane A A9-style: sum
  resident-word cycles.
- **Modeling a mapping change** — Lane B B3: change the K_chunk split
  and re-run.

Anything more invasive (new FSM state, new mechanism layer) requires
TA approval.
