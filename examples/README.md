# FlexNPU Examples

These examples provide a short path through the main FlexNPU workflow:

```text
local mechanism
    ↓
parameter sweep
    ↓
RTL validation
    ├── correctness / cycle behavior
    └── optional synthesis / PPA
    ↓
real workload replay
    ↓
analytical bound vs realized behavior
    ↓
mapping / dataflow comparison
```

The goal is not to cover the full FlexNPU design space. The examples show how to form a prediction, explore it with models and simulators, and validate important points with RTL.

## A few terms used in the examples

The CTB examples frequently use `(K, D, CAP)`:

- **`K`** — the GEMM reduction dimension in `M × K` by `K × N`. Larger `K` generally means more accumulation work and more execution passes through the tile.
- **`D`** — the producer/drain decoupling depth (`DECOUPLE_MAX`). It limits how much completed or outstanding producer work can get ahead of the drain before backpressure occurs.
- **`CAP`** — the per-row output FIFO capacity (`OUT_FIFO_DEPTH`). It determines how much produced output can be buffered while the drain consumes it.

Together, `(K, D, CAP)` describe an interaction between **workload depth**, **producer-consumer decoupling**, and **output-buffer capacity**.

`CAP` should not be interpreted as the total SRAM size of the accelerator. FIFO depth is only one storage-related design knob. Other local storage resources, such as activation buffering, output buffering, and SRAM capacities exposed by the architecture configuration, are additional Lane A design variables but are not explicitly swept in these onboarding examples.

Other architectural and mapping parameters are introduced as they appear in the later examples.

---

## 01 — One CTB design point

```bash
python3 examples/01_hello_ctb.py
```

Runs the CTB mechanistic model at one `(K, D, CAP)` point.

Look at quantities such as:

- service cycles
- peak FIFO occupancy
- active service regime

This is the smallest useful FlexNPU experiment: one workload point, one hardware realization, and one predicted execution behavior.

The main purpose is to become familiar with the mechanistic model and the quantities it exposes.

---

## 02 — Walk across service regimes

```bash
python3 examples/02_regime_walk.py
```

Sweeps `K` while keeping the hardware realization fixed.

Instead of assuming execution time simply scales with workload size, watch how the active constraint changes as the workload moves through different producer, drain, credit, and buffering conditions.

The lesson is:

> Changing a workload parameter can change **which mechanism limits execution**, not only how much work is performed.

---

## 03 — Model prediction vs RTL

```bash
python3 examples/03_rtl_vs_model_parity.py
```

Runs the mechanistic model and RTL at the same design point and compares their behavior.

This demonstrates a core workflow:

```text
model a design point
    ↓
make a prediction
    ↓
run RTL
    ↓
compare prediction and implementation
```

The simulator is useful for exploring many points quickly. RTL provides a lower-level check of timing, control, buffering, and correctness for selected points.

This is particularly important when aggressive resource settings appear attractive in a higher-level model but may cause stalls, overflow, or unsafe execution in the implementation.

RTL is also the implementation boundary for evaluating **hardware cost**. Interesting architecture variants can be taken through synthesis/PPA to compare area, timing, register count, memory resources, and related implementation overheads.

The optional PPA flow is documented in [`docs/04_ppa_flow.md`](../docs/04_ppa_flow.md). It is not part of the onboarding examples and should be treated as an optional extension for projects that need an implementation-cost dimension.

---

## 04 — Replay a real workload

```bash
python3 examples/04_workload_replay.py
```

Replays the shipped MobileBERT operator trace through the CTB mechanistic model.

The first three examples focus on individual synthetic design points. This example applies the same mechanism to a distribution of operator shapes extracted from a real model.

The question becomes:

> Does a mechanism observed on one GEMM remain important across a real workload?

This is the bridge from local architectural reasoning to workload-level evaluation.

---

## 05 — Analytical compute bound vs realized execution

```bash
python3 examples/05_workload_analytical_vs_sim.py
```

Runs the MobileBERT trace through the C++ event-driven simulator and compares the result with an analytical peak-compute lower bound.

The analytical bound asks:

> If useful MAC execution were the only cost, how quickly could this workload complete?

The event-driven simulator then exposes additional service costs that appear in the realized execution.

The example reports directly counted simulator quantities such as:

- host packing cycles
- host MMIO cycles
- DMA bank-fill cycles

and an **accounting residual** for work that is not separately attributed to those buckets.

The residual is not a measured decomposition of compute, drain, and serialization effects. Likewise, the host/MMIO contributions are properties of this simulator realization and configuration, not a universal physical FlexNPU result.

The broader lesson is:

> Peak arithmetic throughput alone may be a poor predictor of realized accelerator performance.

That gap is often the starting point for a research question: which additional architectural, mapping, storage, or service constraint is actually necessary to explain the execution?

For richer analytical exploration, see [`analysis/roofline.py`](../analysis/roofline.py).

---

## 06 — Compare two mapping realizations

```bash
python3 examples/06_mapping_compare.py
```

Runs the same MobileBERT workload under two persistence choices:

```text
persist = off
persist = hit
```

The useful work is the same, but the realized execution structure is different.

In the shipped configuration, persistence removes the weight-DMA stream and reduces bank-fill work, while the final wall time remains unchanged.

That means the two cases are **wall-time equivalent in this configuration**, but they are **not realization equivalent**.

This illustrates an important FlexNPU idea:

> The same system-level performance outcome does not necessarily imply the same realized execution behavior.

A mapping change can therefore matter even when the top-level latency does not move. It may change traffic, local-storage pressure, energy, scalability, or which constraint becomes active under another workload or architecture.

---

## What to explore next

The examples are starting points, not project templates.

See [`PROJECT_IDEAS.md`](../PROJECT_IDEAS.md) for the two main research directions.
