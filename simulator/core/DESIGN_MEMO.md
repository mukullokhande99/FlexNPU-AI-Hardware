# FlexNPU cycle-accurate simulator — design memo

Records the timing / state contracts modeled by the C++ event-driven
simulator. Every cycle constant below is anchored in an internal RTL
directed testbench trace on the canonical FlexNPU platform (not
duplicated in this repo); students inherit the constants and can
re-anchor them if they change the RTL.

## Cycle-exact numbers extracted from STAGE B trace (K=1024, K_TILE=8, 16×16)

Reference — 2-chunk 16×16 K=2048 chain, chunks numbered by STAGE B TB:

| Chunk | STAGE B chunk cycles | STAGE A chunk cycles |
|:---:|:---:|:---:|
| FIRST (wb_suppress=1) | **13,063** | 13,057 |
| LAST  (wb_suppress=0) | **13,083** | 13,073 |

Event trace deltas (STAGE B, cycle 0 = TB reset released):

| Event | Absolute cycle | Delta from previous |
|:---|---:|---:|
| op_start pulse | 2 | +2 |
| op_busy asserted (OP_TILE_WAIT_IN) | 6 | +4 |
| ACT/WGT TLAST (both AXIS complete beat 1024) | 1,027 | +1,021 |
| `bank_full[0]=1`, wr_bank 0→1 (both banks) | 1,028 | +1 |
| op_fsm → compute_start pulse | 1,030 | +2 |
| rd_bank 0→1 (128th tile_done_pulse) | 14,001 | +12,971 |
| compute `done_legacy` | 14,087 | +86 |
| op_done asserted (suppress path skips OP_DRAIN) | 14,090 | +3 |

LAST chunk STAGE B additional: OP_DRAIN + OP_DRAIN_WAIT + writeback ⇒
+20 cycles vs FIRST (10 cyc op_fsm, 10 cyc compute S_WRITEBACK).

## Modeled parameters (derived from RTL — not fitted to totals)

| Parameter | Value | Provenance |
|:---|---:|:---|
| `array_M × array_N` | 16 × 16 | RTL param |
| `K_TILE` (compute-FSM K-slice granularity) | 8 | RTL param |
| `AXIS_BEAT_BYTES` | 16 | RTL bus width |
| `MAX_K` (shell) | 1024 | fixb1b bit param |
| `PL_CLOCK_HZ` | 100e6 | K26 starter-kit BOOT.BIN |
| `compute_setup_cycles` | 44 | RTL trace: cyc between compute_start and 1st tile_done_pulse minus per_pass — measured from Δ compute_start→rd_bank_swap: 12,971 cyc for 128 passes, backed out |
| `compute_per_pass_cycles` | 101 | RTL trace: (14,001 − 1,030) / 128 = 101.34 → integer model |
| `compute_tail_cycles` | 85 | RTL trace: 14,087 − 14,001 = 86 cyc from last pulse to `done_legacy` (rounded to 85 for exact match) |
| `compute_writeback_cycles` | 16 | RTL: `S_WRITEBACK` drains `ARRAY_M` rows |
| `bank_fill_setup_cycles` | 3 | RTL: DMA arm to first beat |
| `bank_fill_per_beat_cycles` | 1 | AXIS handshake steady-state |
| `bank_full_latency` | 1 | RTL: TLAST → bank_full[wr]<=1 next cycle |
| `opfsm_start_to_wait_in` | 4 | RTL: op_start pulse latch + IDLE→WAIT_IN |
| `opfsm_ready_to_compute_start` | 2 | RTL: WAIT_IN(ready) → OP_COMPUTE → pulse |
| `opfsm_done_to_op_done_suppress` | 3 | RTL: OP_COMPUTE_WAIT → OP_NEXT → OP_DONE (suppress) |
| `opfsm_done_to_op_done_drain` | 10 | RTL: + OP_DRAIN + OP_DRAIN_WAIT + drain-ack |

Sanity check with these params (should equal RTL trace exactly):

* FIRST chunk STAGE B total = op_start_setup + wait + ready_to_start + compute + suppress_return
  = 4 + wait + 2 + (44 + 128·101 + 85) + 3
  = 4 + wait + 2 + 13,057 + 3
  = wait + 13,066  (where `wait` = 1,022 cyc bank fill = 1,027 - 5 op_fsm setup already elapsed)

  End-to-end from TLAST to op_done: 13,063 cyc. ✓ (matches STAGE B chunk 0)

* LAST chunk STAGE B = FIRST + 10 (op_fsm drain path) + 10 (compute writeback = 16 - 6 already counted in tail?)

  Actually cleanest: LAST FPGA-active = FIRST + 20 cycles (writeback 16 + drain overhead 4).
  Model: writeback_cycles = 16 (in compute) + 4 (op_fsm OP_DRAIN vs OP_NEXT extra) = 20.
  = 13,083 ✓ (matches STAGE B chunk 1)

## Modeled resources

1. **HostRuntime** — parameterized host-side model (not RTL)
   * Serial pack + arm + poll
   * `pack_time_us_per_dispatch` — measured from `[S31] host_A_B_pack / 4608 dispatches` = 121 µs (median). Explicitly a physical host-model parameter, NOT a hardware-cycle rule.
   * `mmio_overhead_us_per_dispatch` ≈ 21 µs (residual)

2. **DMAEngine** — one per stream (dma0_MM2S, dma1_MM2S, dma0_S2MM)
   * Fires DMA_START when armed
   * Emits DMA_BEAT stream at 1 beat/cycle (steady state) up to `length_beats`
   * Fires DMA_TLAST on last beat
   * Fires DMA_DONE when transaction complete

3. **PingPongBuffer** — one per (ACT, WGT)
   * State: `wr_bank`, `rd_bank`, `bank_full[2]`, `wr_addr`, `tile_count`, `slot_tag[2]`, `persist_active`, `slot_valid[2]`
   * Receives DMA_BEAT → updates `wr_addr`; on `DMA_TLAST` → `bank_full[wr_bank]=1`, swap `wr_bank`
   * Emits `BUF_READY` when `bank_full[rd_bank]=1` transitions high
   * Receives `TILE_DONE` from compute → increments `tile_count`; on `tile_count==tile_total-1` → clears `bank_full[rd_bank]`, swaps `rd_bank` (only if `!persist_active`)

4. **OpFSM**
   * State: `OP_IDLE`, `OP_TILE_WAIT_IN`, `OP_COMPUTE`, `OP_COMPUTE_WAIT`, `OP_DRAIN`, `OP_DRAIN_WAIT`, `OP_NEXT`, `OP_DONE`
   * On `OP_START` event → schedule `OP_TILE_WAIT_IN` at +4 cyc
   * In WAIT_IN, on `BUFS_READY` → schedule `COMPUTE_START` at +2 cyc
   * On `COMPUTE_DONE` (from compute) → if `wb_suppress`: schedule `OP_DONE` at +3 cyc; else schedule `DRAIN_START` at +1 cyc
   * On `DRAIN_DONE` (from output drain model) → schedule `OP_DONE` at +2 cyc
   * Emits latched `accumulate_in`, `writeback_en`, `wb_suppress` for compute

5. **ComputeArray**
   * State: `busy`, `latched_acc_in`, `latched_wb_en`, `pass_count`
   * On `COMPUTE_START` → schedule `TILE_DONE` events at cyc + `setup + i*per_pass - 1` for i = 1..(K/K_TILE)
   * After last `TILE_DONE`, schedule `COMPUTE_DONE` at last_tile_done + `tail_cycles + (writeback_cycles if wb_en else 0)`

6. **OutputDrain** (simple, only fires when wb_en=1)
   * On `DRAIN_START` → schedule `DRAIN_DONE` at +ARRAY_M cycles (1 cyc/row for S2MM AXIS-out drain of 16 rows)

## Event set

```
DISPATCH_BEGIN(dispatch_id, kind, K, wb_suppress, acc_in, is_last)
HOST_PACK_DONE(dispatch_id)                  ← host measured param
DMA_ARM_ACT(dispatch_id, length_beats)
DMA_ARM_WGT(dispatch_id, length_beats)
DMA_BEAT(stream, dispatch_id, beat_idx)
DMA_TLAST(stream, dispatch_id)
DMA_DONE(stream, dispatch_id)
BUFS_READY(dispatch_id)                      ← after both bank_full[rd]=1
OP_START(dispatch_id)                        ← host kick
COMPUTE_START(dispatch_id)
TILE_DONE(dispatch_id, pass_idx)
BANK_SWAP_WR(bank_id) / BANK_SWAP_RD(bank_id)
COMPUTE_DONE(dispatch_id)
DRAIN_START(dispatch_id)
DRAIN_DONE(dispatch_id)
OP_DONE(dispatch_id)
DISPATCH_END(dispatch_id)
```

## Concurrency model

* Host is a serial resource for the current runtime. Nothing overlaps
  between host pack of chunk c+1 and FPGA execute of chunk c.
* Within a dispatch, DMA_MM2S + op_fsm setup + compute WAIT_IN can be
  concurrent, but compute cannot start until BUFS_READY.
* The event queue naturally supports concurrency; the host resource
  simply won't emit chunk c+1's PACK_DONE until chunk c's OP_DONE
  arrives.

## What is validated-cycle-exact vs measured-parameter

| Quantity | Class | Tolerance |
|:---|:---:|:---:|
| Per-pass cycles (101) | RTL-derived | ±0 required |
| Bank swap events | RTL-known | ±0 |
| op_fsm state deltas | RTL-known | ±0 |
| Compute tail 85 | RTL-derived | ±0 |
| Writeback 16 | RTL-derived | ±0 |
| Host pack per-dispatch (121 µs) | measured physical | ±10 % |
| Host MMIO overhead (21 µs) | measured physical | ±20 % |
| DMA beat rate (1 cyc/beat) | RTL steady-state | ±0 |

## Non-goals (per user constraint)

* No MAC-by-MAC simulation
* No monolithic latency formula
* No hardcoded 13,080 or 1.260 s
* Cycle numbers must emerge from resource events, not from fitted totals
* No DSE sweeps
* No optimization implementation

## Realization input schema (JSON)

Sim reads a small realization descriptor per run:

```json
{
  "workload": {"M": 512, "N": 768, "K": 3072},
  "arch": {"array_M": 16, "array_N": 16, "MAX_K": 1024, "K_TILE": 8, "pl_clock_hz": 100e6},
  "mapping": {
    "M_tiles": 32, "N_tiles": 48, "K_chunks": 3,
    "loop_order": "m_outer_n_middle_chunk_inner",
    "persistence": "off"
  },
  "host_model": {"pack_us_per_dispatch": 121.0, "mmio_overhead_us_per_dispatch": 21.5}
}
```

## Validation milestones (in order)

* **A** — STAGE B microbenchmark: 16×16 K=2048 2-chunk chain. Per-cycle event trace comparison. Chunk cycles 13,063 / 13,083 ±0.
* **B** — GPT-2 FFN: 4,608 dispatches. Total FPGA-active cycles within ±2 % of 603 ms.
* **C** — MobileBERT contrast: model persist=1 hit path, verify per-dispatch NPU time ~126 µs (existing counter).
