# Project Ideas

Two lanes. Each lane says what kinds of questions belong there, why
they matter, which tools help, and gives starter questions with weak
vs strong examples. You can propose adjacent questions in either
lane — the starter list is not a menu you're stuck with.

---

## Lane A — NPU realization and resource co-design

### The research target

```
workload + realization
    ->
active constraint
    ->
performance / safety / cost
```

You are not trying to "find the best D" or "sweep parameters." You
are trying to understand how a specific realization interacts with a
specific workload to produce a specific *active constraint*, and how
architectural moves shift that constraint. When you can predict
which constraint is binding — and be right — you have Lane A working.

### What belongs here

Questions about how the accelerator itself should be provisioned and
organized. You keep the compute datapath; you vary the resources
and control around it. Interesting axes:

- array geometry (16×16 vs 16×32 vs 32×16 vs 8×32);
- `K_TILE` and other execution-granularity knobs;
- FIFO and SRAM capacity;
- outstanding-work credit depth;
- drain / service organization and flow control;
- producer vs consumer service-rate balance;
- how architectural changes migrate regime boundaries;
- performance vs implementation cost (relative area / cell count).

The answers you're after describe when a resource is **limiting**,
**sufficient**, **dormant**, or **unsafe** for a given workload
envelope — and *why*, in mechanism terms.

### Tools

- The **CTB mechanistic model** (`simulator/ctb_mech/`) is the fast
  first stop: producer, credit, per-row FIFOs, drain, backpressure.
  Sweep here freely; RTL is not the exploration surface.
- The **C++ event-driven simulator** (`simulator/core/`) folds the
  CTB into the outer dispatch / host / DMA / ping-pong pipeline —
  useful when a Lane A change should propagate beyond the CTB.
- **RTL under iverilog** validates a small, deliberately chosen set
  of points.
- The optional **PPA sketch** (`docs/04_ppa_flow.md`) reports
  relative hardware cost across the same configurations under
  identical open-flow synthesis assumptions.

### Starter questions

- **A1** Which realization axes actually move the *active
  constraint* on the MobileBERT and MobileNetV2 shape distributions?
  Rank them by predicted regime-boundary sensitivity from the
  mechanistic model.
- **A2** Does array geometry (16×16 vs 16×32 vs 32×16) change
  *which* constraint binds, or only *how tightly*? Predict from the
  model, then confirm with a few RTL points.
- **A3** How does `K_TILE` in {8, 16, 32} change producer vs drain
  service balance? Re-anchor the per-pass cycle constant from RTL
  and update the mechanistic model — does the model's regime label
  stay honest?
- **A4** Co-design `OUT_FIFO_DEPTH` and `DECOUPLE_MAX` for one real
  workload: identify the smallest safe combination that keeps the
  design out of *credit-limited* and *storage-unsafe*. Trace what
  the sacrificed capacity costs in the PPA sketch.
- **A5** For a chosen realization, predict — before running any
  RTL — the exact K at which storage-unsafe overflow first occurs.
  Commit the prediction, then measure.
- **A6** Take one dormant-provisioning claim from the mechanistic
  model and falsify it (or fail to falsify it) with RTL + PPA.
  Which direction did the mechanism actually go?
- **A7** Predict an *unseen* realization triple `(D, CAP,
  drain_policy)` before RTL confirms — two cells, prediction as a
  commit, then measure. Weight the causal explanation over the
  cycle-count match.

### Weak vs strong Lane A questions

- **Weak** — "How much faster is CAP=64 than CAP=32?"
  (No mechanism, no workload envelope, no prediction, no cost.)
- **Better** — "For the MobileBERT shape distribution under Y2+D3,
  is CAP=32 behaviorally equivalent to CAP=24? Predict from the
  mechanistic model, validate at ≤ 4 RTL points."
- **Stronger** — same as Better, plus a named mechanism for the
  equivalence (the natural per-row FIFO peak at `D = D_min`), a
  demonstration that the prediction transfers to a workload not used
  to fit it, and a PPA comparison showing what the smaller CAP buys.

---

## Lane B — Mapping and dataflow realization

### The research target

```
nominal mapping choice
    ->
realized execution structure
    ->
performance / bottleneck / resource behavior
```

You are not trying to find "the fastest mapping." You are trying to
understand which mapping *decisions* actually change realized
execution, which are nominal-only, and where the analytical
prediction fails.

### What belongs here

Questions about how loop / tile / dispatch choices become realized
execution on a fixed accelerator. Interesting axes:

- tiling and tile order;
- loop order;
- split-K;
- persistence / reuse and materialization behavior;
- mapping-dependent per-row FIFO pressure;
- bottleneck migration under mapping change;
- when nominally different mappings behave identically;
- roofline analytical bounds vs realized cycle counts.

The answers you're after describe *which* mapping change moves
*which* mechanism layer, and where the roofline gap becomes
mechanistic rather than analytical.

### Tools

- The **roofline layer** (`analysis/roofline.py`) is the analytical
  baseline you compare against.
- The **C++ event-driven simulator** (`simulator/core/`) is the
  Lane B workhorse: dispatch order, tile / bank behavior, host and
  DMA pipeline, end-to-end wall-clock modeling.
- The **CTB mechanistic model** (`simulator/ctb_mech/`) drops in
  when a mapping question is really about CTB service and the
  event-driven layer treats the CTB too coarsely.
- **RTL** validates a small subset — a mapping-behavior claim
  usually doesn't need many RTL runs to confirm.
- Real traces under `workloads/mobilebert/` and
  `workloads/mobilenetv2/` are the evaluation set.

### Starter questions

- **B1** For each provided real workload, where does the roofline
  bound fail? Attribute the residual gap to the four CTB regimes
  and name the operator sites where the gap is largest.
- **B2** Find a pair of mappings that are nominally different but
  behaviorally equivalent in the C++ simulator. Prove the
  equivalence; give a mechanism for it.
- **B3** For a large-K matmul, sweep K_chunk; predict the sweet
  spot from producer / drain amortization *before* the sweep.
  Attribute residual error.
- **B4** Which workload features (K/N ratio, N_padded slack, site
  count, count-per-shape distribution) predict *mapping sensitivity*?
  Rank MobileBERT and MobileNetV2 sites by predicted sensitivity,
  then measure.
- **B5** On MobileBERT's Q / K / V / proj sites, which reuse /
  persistence choice most changes per-row FIFO pressure? Recommend
  a mapping and back it up.
- **B6** Take one dispatch-order decision the outer simulator
  makes today and show a workload where a different decision
  changes realized service.
- **B7** Combine the roofline, the C++ simulator, and the CTB
  model into a *decomposition* of realized service on a real
  workload: how many cycles are pure compute, drain-side, and
  host / DMA?

### Weak vs strong Lane B questions

- **Weak** — "Which loop order is fastest?"
  (No workload, no bottleneck target, no prediction.)
- **Better** — "For MobileBERT FFN sites, does reordering the tile
  loop over N vs K change per-row FIFO peak? Predict from the
  mechanistic model."
- **Stronger** — same as Better, plus explains which mechanism
  layer moves, shows whether the effect transfers to
  MobileNetV2's Conv-as-matmul sites, and identifies which sites
  cluster into a behavioral-equivalence class.

---

## Common methodology

Whichever lane, a project that produces a useful answer will
usually:

1. State a **question** and a **quantitative prediction**.
2. Commit the prediction *before* running the modification.
3. Explore with the **appropriate FlexNPU tool** (not necessarily
   RTL).
4. Validate the important claims with **a small number of
   deliberately chosen RTL points**.
5. Evaluate on **real workload traces** where the effect should
   transfer.
6. Explain **which mechanism moved**, not just what changed.

Sweeps for their own sake are not particularly valuable. Sweeps
that test a specific prediction, or that map a boundary you
already understand mechanistically, are.

## Adjacent variants

If your question doesn't fit any starter above, propose a variant.
A good proposal states:

- which lane it lives in;
- what is falsifiable about it;
- which FlexNPU tool it depends on and why;
- what a positive result would look like, and what a negative
  result would look like.

Both outcomes are interesting.
