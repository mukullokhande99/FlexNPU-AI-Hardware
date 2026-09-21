# Project Ideas

FlexNPU currently has two main areas where broader exploration would
be especially valuable. Adjacent questions grounded in the same
platform are welcome. The existing D=3 / CAP=24 style results in the
research repo are best treated as methodology anchors — they show
what a good prediction-then-validation loop looks like. What we
actually need is coverage of the parts of the design space they did
not touch.

## Lane A — NPU realization and resource co-design

I want to understand how a given realization of the accelerator (array
geometry, tile granularity, FIFO / SRAM capacity, credit depth, drain
policy, flow control) interacts with a workload to determine which
constraint binds — and how much of the provisioning is actually
active vs dormant.

Interesting axes:

- array geometry (16×16, 16×32, 32×16, 8×32, …);
- `K_TILE` and other execution-granularity knobs;
- FIFO / SRAM capacity;
- outstanding-work credit depth;
- drain / service organization and flow control;
- producer vs consumer service-rate balance;
- performance vs implementation cost (relative area / cell count).

Starter questions:

- **A1** Which realization axes actually move the active constraint
  on the MobileBERT / MobileNetV2 shape distributions?
- **A2** Does array geometry change *which* constraint binds, or
  only how tightly?
- **A3** How does `K_TILE` in {8, 16, 32} change producer vs drain
  service balance? Re-anchor the per-pass cycle constant from RTL
  and update the mechanistic model.
- **A4** Co-design `OUT_FIFO_DEPTH` and `DECOUPLE_MAX` for one real
  workload: smallest safe combination, at what hardware cost.
- **A5** Track how the active constraint moves as array geometry
  and `K_TILE` change. Does a provisioning rule (e.g. a credit /
  storage relation) derived under one realization transfer to
  another? Where does it break, and why?
- **A6** Build a performance–cost frontier across a small but
  meaningful design space — for example `{array geometry, K_TILE,
  D, CAP}`. Identify strictly dominated designs, behaviorally
  equivalent designs, and points where the active bottleneck
  changes.
- **A7** Turn any of the above into a PPA comparison via the
  optional flow in `docs/04_ppa_flow.md`.

Tools: `simulator/ctb_mech/` for fast sweeps; `simulator/core/` when
the change should propagate beyond the CTB; RTL for validation;
PPA sketch for hardware-cost comparisons under identical open-flow
synthesis assumptions.

## Lane B — Mapping and dataflow realization

I want to understand how mapping decisions (loop order, tiling,
split-K, persistence / reuse, dispatch order) become realized
execution — and where the analytical bound diverges from measured
behavior.

Interesting axes:

- tiling and tile order;
- loop order;
- split-K;
- persistence / reuse and materialization behavior;
- mapping-dependent buffering and traffic;
- bottleneck migration under mapping change;
- behavioral equivalence between nominally different mappings;
- roofline prediction vs realized cycle counts.

Starter questions:

- **B1** For each real workload, where does the roofline bound
  fail, and which realized service or resource effect explains the
  gap? The explanation is not required to be a CTB-regime label —
  it can be an outer-pipeline effect, a mapping effect, a
  buffering / materialization effect, a CTB service effect, a
  dispatch / service-organization effect, or another measured
  mechanism. Pick the level of the stack that actually explains
  the gap.
- **B2** Find a pair of mappings that are nominally different but
  behaviorally identical in the C++ simulator; give a mechanism
  for the equivalence.
- **B3** Sweep `K_chunk` on a large-K matmul; predict the sweet
  spot from producer / drain amortization first, then measure.
- **B4** Which workload features (K/N ratio, N_padded slack, site
  count, count-per-shape distribution) predict mapping
  sensitivity? Rank MobileBERT and MobileNetV2 sites, then measure.
- **B5** On the repeated projection shapes in MobileBERT, which
  mapping / reuse choices materially change realized service,
  traffic, or buffering — and why? A good answer names the
  mechanism, not just the winner.
- **B6** Show a workload where a different dispatch-order decision
  materially changes realized service.
- **B7** Decompose realized service on a real workload into an
  analytical compute bound, a mapping / outer-pipeline overhead,
  and a CTB service component. Determine which of those three
  abstractions is actually needed to explain each portion of the
  gap.

Tools: `analysis/roofline.py` for the analytical baseline;
`simulator/core/` as the workhorse for mapping / dispatch modeling;
`simulator/ctb_mech/` for CTB-service detail when the outer layer
is too coarse; RTL for chosen validation points; real traces under
`workloads/mobilebert/` and `workloads/mobilenetv2/` for evaluation.
