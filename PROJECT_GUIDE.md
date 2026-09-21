# Project Guide

This guide describes how to do a project on FlexNPU-AI-Hardware.
It is not a syllabus. Grading, deadlines, and administrative details
live wherever the course itself lives.

## The stack

FlexNPU exposes several layers of description at different fidelity /
speed points. Choose the layer that answers your question, not the
one that has the most detail.

| Layer | Where it lives | Cost | Good for |
|:---|:---|:---|:---|
| Analytical / roofline | `analysis/roofline.py` | milliseconds | intuition, order-of-magnitude bounds, mapping first-pass |
| Event-driven C++ simulator | `simulator/core/` | seconds per point, sweep-friendly | architecture / mapping DSE, dispatch scheduling, host/DMA modeling |
| CTB mechanistic model | `simulator/ctb_mech/` | ms per point | producer / drain / credit / per-row FIFO service questions |
| RTL testbench | `rtl/` + `tb/` under iverilog / Verilator | seconds–minutes | validation of a selected point |
| Optional PPA sketch | `docs/04_ppa_flow.md` | minutes | rough area / cell-count comparison across configs |

The examples under `examples/` show how these compose end-to-end.

## Two lanes

Most projects fall into one of two lanes. `PROJECT_IDEAS.md` expands
each of them with concrete starter questions.

- **Lane A — NPU realization and resource co-design.** How should the
  accelerator itself be provisioned and organized? What is dormant,
  what is limiting, what is unsafe?
- **Lane B — performance modeling, mapping, and dataflow.** For a
  fixed accelerator, why does a workload run the way it does, and
  which mapping choice actually changes behavior?

Cross-lane work is welcome as long as the question is well-defined.

## The research process we expect

Not a checklist. This is roughly the shape of a project that produces
a useful answer.

1. **Pick a question** — from `PROJECT_IDEAS.md` or an adjacent
   variant. Prefer a question with a *falsifiable* prediction over
   an open-ended sweep.
2. **State a hypothesis / prediction** — write it down before you
   modify anything. A quantitative claim is stronger than a
   directional one.
3. **Explore with the appropriate FlexNPU tool** — usually one of
   the top three layers above. Sweep broadly; there is no cost to
   the mechanistic model.
4. **Validate the important claims** — a handful of RTL runs at
   carefully chosen points is worth more than dozens of casual ones.
5. **Evaluate on real workloads where relevant** — the
   `workloads/mobilebert/` and `workloads/mobilenetv2/` traces let
   you check whether the effect you found matters for a real
   operator distribution.
6. **Explain the mechanism** — which model layer moves? Which
   regime? What surprised you? A causal explanation is worth
   more than a table of numbers.
7. **Submit through the fork / PR workflow** — fork this repo,
   work in the fork, open a PR back to the course repo with your
   writeup and code.

If your project falsifies one of our assumptions, that is a
positive outcome, not a failure. Write it up.

## Simulator-vs-RTL parity

Predictions matter more than model-vs-RTL cycle-count agreement.
The mechanistic model is not cycle-exact; agreement to within a few
percent on relevant observables (service, stall, peak FIFO,
overflow) is normal and sufficient for most projects.

When it *disagrees* by more than that on an observable central to
your claim, treat the disagreement itself as the finding. Diagnose
it. Ask what state / event the model is missing.

## Cross-cutting: PPA

Hardware cost is a legitimate dimension of any Lane A comparison and
sometimes shows up in Lane B mapping choices. FlexNPU-AI-Hardware
ships a lightweight open-flow PPA sketch (see `docs/04_ppa_flow.md`).
Report relative area / cell count / register count for the
configurations you compare, and be honest about what those numbers
do and do not tell you. This is not ASIC signoff; interesting
designs can be evaluated with the full ASIC flow separately.

## Writeup

A short writeup that clearly states the question, the prediction,
what you found, and *why* is the right output. There is no fixed
template. Diagrams help. Raw sweep CSVs and plots can live alongside
your code in the PR.

## Where to look

- `README.md` — quick start and orientation.
- `PROJECT_IDEAS.md` — starter questions per lane.
- `docs/01_architecture_overview.md` — the platform at a glance.
- `docs/02_ctb_mech_and_regimes.md` — the mechanistic model.
- `docs/03_rtl_and_testbenches.md` — the RTL and how the TBs
  drive it.
- `docs/04_ppa_flow.md` — optional open-flow synthesis sketch.
- `simulator/core/SIMULATOR.md` — the event-driven C++ simulator.
- `simulator/core/DESIGN_MEMO.md` — the timing contracts it models.
