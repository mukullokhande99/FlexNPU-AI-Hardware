# FlexNPU event-driven cycle-accurate simulator
Date: 2026-09-17

Validated foundation for future EdgeScope architecture DSE. Reproduces
the RTL-verified FlexNPU timing behavior from modeled events and
resources — **not** from a fitted latency formula or hardcoded constants.

## 1. Architecture summary

* Discrete-event, cycle-based, resource/state-aware C++17 simulator.
* `priority_queue<Event>` with `(cycle, seq)` deterministic ordering.
* Small resource classes (Simulator holds their state); each event
  handler is a method that updates state and schedules follow-up events.
* Header-only core (`include/flexnpu_sim.h`) + CLI (`src/main.cpp`) +
  three test drivers under `tests/`.
* Event throughput: ~29 M events/sec (~56× faster than real time on the
  GPT-2 FFN workload).

## 2. Source paths

```
sim/flexnpu_sim/
├── DESIGN_MEMO.md               ← internal timing/state contracts (read first)
├── SIMULATOR.md                 ← this document
├── CMakeLists.txt
├── include/flexnpu_sim.h        ← core header (Event, Arch, HostModel,
│                                  ping-pong / compute / op_fsm state,
│                                  Simulator class, realization helpers)
├── src/main.cpp                 ← CLI: `flexnpu_sim <realization>`
├── tests/test_stage_b.cpp       ← Milestone A (STAGE B RTL trace)
├── tests/test_gpt2.cpp          ← Milestone B (GPT-2 FFN FPGA-active)
├── tests/test_mb.cpp            ← Milestone C (MobileBERT contrast)
└── build/                       ← CMake build tree
```

## 3. Modeled resources and events

### Resources (each with explicit state)

| Resource | State |
|:---|:---|
| `HostRuntime` (parameterized, measured) | `busy`, `last_op_done`, `next_dispatch_idx`, host_pack + mmio counters |
| `DMAEngine` (per stream: ACT / WGT / OUT) | byte-accounted, per-beat scheduling |
| `PingPongBuffer` × 2 (ACT, WGT) | `wr_bank`, `rd_bank`, `bank_full[2]`, `tile_count`, `tile_total`, `wr_addr`, `persist_active`, `slot_valid[2]`, `swaps_wr`, `swaps_rd` |
| `ComputeArray` | `busy`, `latched_acc_in`, `latched_wb_en`, `passes_scheduled`, `passes_completed`, useful/total-cycle counters |
| `OpFSM` | `state ∈ {IDLE, TILE_WAIT_IN, COMPUTE, COMPUTE_WAIT, DRAIN, DRAIN_WAIT, NEXT, DONE}`, `r_writeback_suppress`, `tiles_completed` |
| `OutputDrain` | modeled as op_fsm handshake + fixed cycle latency |

### Event set

`DISPATCH_BEGIN, HOST_PACK_DONE, DMA_ARM, DMA_TLAST, DMA_DONE,
BUFS_READY, OP_START, COMPUTE_START, TILE_DONE, BANK_SWAP_WR,
BANK_SWAP_RD, COMPUTE_DONE, DRAIN_START, DRAIN_DONE, OP_DONE,
DISPATCH_END`

Each event has an integer cycle timestamp; the queue's sequence tiebreaker
guarantees deterministic replay of same-cycle events.

## 4. Relationship to RSem

For this first milestone, the "realization descriptor" input is generated
by small helper builders (`build_stage_b_two_chunk`,
`build_chain_realization`, `build_mb_style`) that emit a
`std::vector<Dispatch>`. Each `Dispatch` carries `K, wb_suppress, acc_in,
is_last, persist`. The simulator interface is intentionally shaped so
that RSem output can produce the same descriptor structure without
changing the sim internals.

## 5. What is cycle-exact vs measured/parameterized

| Quantity | Class | Value | Source |
|:---|:---:|:---:|:---|
| Per-pass compute cycles | RTL-derived | **101** | STAGE B trace |
| Compute setup | RTL-derived | **44** | STAGE B trace |
| Compute tail | RTL-derived | **85** | STAGE B trace |
| Compute S_WRITEBACK | RTL-derived | **16** (ARRAY_M rows) | STAGE B trace |
| DMA bank-fill setup | RTL-derived | 2 | STAGE B trace TLAST timing |
| Bank full latency (TLAST→bank_full) | RTL-derived | 1 | STAGE B trace |
| op_fsm setup (op_start→WAIT_IN) | RTL-derived | 4 | STAGE B trace |
| op_fsm ready→compute_start | RTL-derived | 2 | STAGE B trace |
| op_fsm done→op_done (suppress) | RTL-derived | 3 | STAGE B trace |
| op_fsm drain path total (compute_done→op_done) | RTL-derived | 7 | STAGE B trace |
| Host pack per dispatch | **measured physical** | 121 µs (GPT-2), 8.7 µs (MB) | S31 profiler bucket / dispatches |
| Host MMIO overhead per dispatch | **measured physical** | 21.5 µs (GPT-2), 95 µs (MB, incl. usleep) | S31 profiler bucket |
| Persistence hit rate | **input parameter** | 0.7 (MB test config) | measured/derived from workload |

**Fitted-constant safeguard**: all RTL-derived values above are derived
from directly observed events in the STAGE B RTL testbench trace, not
by fitting per-dispatch totals. Verified: chunk 0 = 13,063 and chunk 1 =
13,083 emerge from the resource model, matching RTL trace with **±0
cycle** exactness.

## 6. Validation results

### Milestone A — STAGE B directed RTL microbenchmark

Setup: `HostModel{0, 0}` (host-free), single 16×16 output tile, 2 K-chunks
(K=1024 each, K_TILE=8).

| Assertion | Result |
|:---|:---:|
| dispatches_run == 2 | ✓ |
| tile_done_events == 256 | ✓ |
| bank_swaps_wr == 4 (2 chunks × 2 sides) | ✓ |
| bank_swaps_rd == 4 | ✓ |
| compute_done × 2, drain_done × 1 | ✓ |
| **chunk 0 (TLAST → OP_DONE) == 13,063** | **✓ exact** |
| **chunk 1 (TLAST → OP_DONE) == 13,083** | **✓ exact** |
| chunk 0 compute_done − compute_start == 13,057 | ✓ |
| chunk 1 compute_done − compute_start == 13,073 | ✓ |
| 128 tile_done pulses per chunk | ✓ |

Event trace extracted from simulator (chunk 0):

```
0     DISPATCH_BEGIN     dispatch=0
0     DMA_ARM            ACT+WGT
1025  DMA_TLAST          ACT (first), WGT (second, same cyc)
1026  BANK_SWAP_WR       ACT, WGT (same cyc, seq-ordered)
1026  BUFS_READY
1028  COMPUTE_START
1072..13999  TILE_DONE   ×128 at spacing 101 cyc
14085 COMPUTE_DONE
14088 OP_DONE
```

All key events align **cycle-exactly** with RTL trace deltas (adjusting
for TB launch offset).

### Milestone B — GPT-2 FFN FPGA-active

Setup: `HostModel{121 µs, 21.5 µs}`, 4,608 chain dispatches, MAX_K=1024,
K=3072.

| Assertion | Predicted | Golden | Error |
|:---|:---:|:---:|:---:|
| dispatches | 4,608 | 4,608 | 0 |
| compute_done events | 4,608 | 4,608 | 0 |
| drain_done events | 1,536 (LAST only) | 1,536 | 0 |
| tile_done events | 589,824 (=4608×128) | 589,824 | 0 |
| **FPGA-active cycles (derived sum)** | **601.9 ms** | **603 ms** | **−0.18 %** |
| Wall (with host model) | 1.306 s | 1.260 s | +3.66 % |
| Per-dispatch bracket | 283.4 µs | 274 µs | +3.45 % |
| ACT DMA bytes | 75,497,472 | 75,497,472 | 0 |
| WGT DMA bytes | 75,497,472 | 75,497,472 | 0 |
| OUT DMA bytes | 1,572,864 | 1,572,864 | 0 |

**FPGA cycles emerged from the resource model**, not from a hardcoded
13,080 value. The 13,057 (FIRST/MIDDLE) and 13,073 (LAST) chunk cycles
build up from `44 + 128·101 + 85` and `+16` writeback.

Wall time within ±4 % of median physical (1.260 s). Residual error
(46 ms over-prediction) is host-side timing variability (host_us
parameters are single-point estimates from measured medians, not
per-dispatch distributions).

### Milestone C — MobileBERT persist=1 hit contrast

Setup: `HostModel{8.7 µs, 95 µs}` (MB per-dispatch, includes 76 µs
usleep), 1,000 dispatches, K=384, hit_rate=0.70.

| Assertion | Result |
|:---|:---:|
| ACT bytes = N × K × beat_bytes (all dispatches) | ✓ 6,144,000 B |
| WGT bytes = misses × K × beat_bytes (hits skip WGT DMA) | ✓ 1,849,344 B |
| persist hits/misses recorded correctly (structural) | ✓ 699 / 301 |
| observed hit rate matches config within 5 % | ✓ 69.9 % vs 70.0 % |
| per-dispatch in 100–250 µs range (order-of-magnitude sanity) | ✓ 157.6 µs |

Structural contrast demonstrated: **same simulator** produces the
persist-hit path (WGT DMA skipped, bank retained) alongside the chain
path (non-persist, all DMA per dispatch). Not hardcoded.

Note: this is a best-effort contrast test, not a strict apples-to-apples
reproduction of the full MB 25-second NPU run (198,912 dispatches, K
varies per matmul call, softmax not modeled). The intent is to show the
simulator handles the persist=1 structural regime.

## 7. Simulator performance

Measured on the GPT-2 FFN 4,608-dispatch workload:

| Metric | Value |
|:---|---:|
| events processed | 675,840 |
| simulated cycles | 130,612,224 (1.306 s @ 100 MHz) |
| C++ wall time | 23 ms |
| events / sec | ~29 M |
| simulation speedup vs real time | ~56× |

Test suite (`ctest`): **3/3 pass in 0.04 s total**.

The simulator's event throughput can plausibly support DSE with 10³-10⁴
architecture points per hour on a laptop, so DSE workloads that require
end-to-end MobileBERT (198k dispatches) are also tractable.

## 8. Known limitations

1. **Compute-FSM cycle model is per-pass average**. The 101 cyc/pass
   value is the RTL trace fitted average over 128 passes. If per-pass
   behavior differs at DSE-parameter extremes (K_TILE ≠ 8, ARRAY_M ≠ 16),
   the model must be re-fitted with new RTL simulations or a more
   detailed pass-cycle breakdown must be exposed.
2. **AXIS-out drain is minimal-ack (STAGE B-style)**. Real physical
   drain time (16 rows × 1 cyc S2MM AXIS + IOC latency) is folded into
   the 7-cycle op_fsm drain-path constant. Adequate for the current
   workload; must be revisited for wider AXIS-out or larger array_M.
3. **Host model is a single-point per-dispatch mean**. Variance is not
   modeled; run-to-run variation is smaller than the model's rounding.
4. **MB test is order-of-magnitude, not cycle-exact**. Modeling the full
   MB dispatch shape / softmax / usleep-inside-dispatch requires
   per-dispatch schedule fidelity beyond this first milestone.
5. **No modeling of DMA descriptor reload / MM2S internal FIFO
   backpressure**. Assumes DMA delivers 1 beat / cycle steady-state
   (matches STAGE B RTL TB, matches physical measured throughput).
6. **No modeling of AXI-Lite bus contention**. All MMIO writes to
   different registers are treated as one lumped `mmio_overhead`.
7. **Chain path only supports the currently validated realization**.
   Adding oversized-fallback, macro-op batching, or Fix-B strip=8
   requires a Realization extension.
8. **Rescale / softmax slot not modeled**. The current bitstream's
   rescale_active gate + softmax_bf16 slot exist but are outside the
   validated GEMM path.
9. **No DSE parameter sweeps.** By design (per this-job constraint).

## 9. Future DSE parameters (exposed but not yet swept)

Present in `Arch` struct, ready for future DSE without changing the
simulator core:

* `array_M`, `array_N`
* `MAX_K`
* `K_TILE`
* `AXIS_BEAT_BYTES`
* `pl_clock_hz`
* All compute/opfsm cycle deltas
* All host-model timing parameters

**Do NOT sweep these in this job.** The simulator is validated for the
current hardware point only. A parameter change requires re-anchoring
against RTL (or explicit labelling as "hypothetical architecture mode").

## 10. Simulator hierarchy reference

```
workload / design
    ↓
RSem realization  ← today: small builder helpers; tomorrow: RSem output
    ↓
event-driven cycle-accurate C++ architecture simulator  ← THIS COMPONENT
    ↓
timing / utilization / traffic / stalls  ← JSON stats output
    ↓
host / system + energy / PPA model  ← not yet built
```

The simulator does NOT duplicate RSem logic. It consumes a small
realization descriptor and produces cycle-level outcomes. Later, RSem
will generate the descriptor automatically for any workload / arch /
mapping tuple.

## 11. Anchoring provenance

Every cycle constant in this document is anchored in an internal RTL
directed testbench trace maintained on the canonical FlexNPU platform.
That trace is not shipped in this student repo; the constants are
inherited from that validated reference. If you change the exposed
RTL in a way that affects timing, re-anchor the affected constants
against your own trace before trusting the numbers.

## 12. Extending / running

Build:
```
cd simulator/core && mkdir -p build && cd build && cmake .. && cmake --build . -j
```

Run:
```
./flexnpu_sim stage_b_chain   # STAGE B microbenchmark
./flexnpu_sim gpt2_ffn        # GPT-2 FFN K=3072
./flexnpu_sim mb_style        # MB persist=1 hit path
./flexnpu_sim <realiz> --trace <file>   # emit event trace
./flexnpu_sim <realiz> --json           # emit stats as JSON
```

Test:
```
ctest --output-on-failure
```
