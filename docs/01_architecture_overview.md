# FlexNPU compute core — architectural overview

The FlexNPU compute core is a 2D int8 systolic array with:

- **PE (`rtl/mac.sv`)** — one signed-int8 x signed-int8 -> signed-int32
  MAC per cycle. Wrapper adds systolic pass-through of `a`, `b`,
  and `en` with one register of delay.
- **Systolic array (`rtl/systolic.sv`)** — `ARRAY_M x ARRAY_N`
  generate-block instantiation of `mac`. Per-row `row_done_pulse`
  signals when a row's accumulator has completed a K-tile.
- **Array + FIFOs (`rtl/mm.sv`)** — wraps the systolic array with
  ARRAY_M A-side and ARRAY_N B-side input FIFOs, plus ARRAY_M
  per-row output FIFOs. Exposes `o_perf_mm_active` and
  `o_all_streaming_done`.
- **Compute-tile-BRAM top (`rtl/compute_tile_bram.sv`)** — the
  outer dataflow controller. Owns the 7-state main FSM
  (`S_IDLE / S_WAIT_TILE / S_CLEAR / S_EXECUTE / S_WAIT / S_NEXT /
  S_WRITEBACK`), a separate drain FSM, ping-pong ACT/WGT buffers,
  the credit register (`r_pending`), and a small K-tile counter.

## Why a scalar per-pass latency is insufficient

A first-pass performance model might say: "the array does K MACs per
K-tile, cycles per K-tile = some anchor." That collapses too much.
The real service time of one dispatch depends on the interaction of
six mechanism layers:

1. **Producer / pass execution FSM.** Each K_TILE pass moves through
   IDLE -> EXECUTE -> WAIT -> NEXT with fitted cycle constants
   (T_execute, T_wait, T_next). Not all of them are always visible;
   `SWAIT_EARLY_EXIT` shortcuts the wait state under specific
   conditions.
2. **Outstanding-work credit `r_pending`.** Bounded by `DECOUPLE_MAX`
   (D). The producer stalls in S_NEXT whenever
   `r_pending == D` — a *credit stall*, distinct from a compute stall.
3. **Progressive per-row materialization.** After a pass ends, rows
   push into their per-row output FIFOs on a systolic schedule (row 0
   arrives ARRAY_M-1 cycles before row 15).
4. **Per-row FIFO occupancy `Q_r(t)`.** Depth `CAP = OUT_FIFO_DEPTH`.
   Overflow means data is lost — the drain FSM must fire fast enough.
5. **Drain FSM (D_IDLE, D_ACTIVE).** Fires one drain per `drain_ii`
   cycles, drains each row's oldest word. Its own two-slot buffer
   (q0/q1) smooths the accumulate path.
6. **Credit-admission backpressure.** The producer's S_NEXT is gated
   on `r_pending < D`. When the drain FSM lags, `r_pending` piles up,
   credit stalls dominate.

Which layer *dominates* on a given workload is the "service regime."

## The five regimes

The mechanism model classifies each run into one of:

| Regime | Witness (roughly) | What to do about it |
|---|---|---|
| **dormant** | `rp_peak < D` and workload short | Over-provisioned; can shrink D or CAP. |
| **producer-limited** | `wait + next` dominates; drain idle | Speed up producer or accept the cost. |
| **drain-limited** | Drain saturated; `credit_stall_cyc` small | Producer is fine; the drain policy caps throughput. |
| **credit-limited** | `credit_stall_cyc` large; `rp_peak == D` | Add credit (raise D) or drain faster. |
| **storage-unsafe** | any `Q_r(t) > CAP` | Infeasible; will lose data in RTL. |

The regime that dominates depends on **workload shape** (K, N, count
distribution), **realization parameters** (D, CAP, drain policy), and
**geometry** (array_m, array_n, K_TILE). The mechanism model predicts
which regime each cell is in *before* running RTL.

## What is anchored, what is analytical

The compute-per-pass cycle constant is measured against RTL at
`(ARRAY_M=16, ARRAY_N=16, K_TILE=16)`. Changing any of those three
without re-anchoring will silently mis-predict cycle counts (Lane A
seed questions A1 and A4 address this).

Everything else in the mechanism model is analytical — small integer
recurrences over the six layers above.
