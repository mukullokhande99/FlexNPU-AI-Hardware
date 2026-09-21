# FlexNPU-AI-Hardware

[![CI](https://github.com/YiminGao0113/FlexNPU-AI-Hardware/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/YiminGao0113/FlexNPU-AI-Hardware/actions/workflows/ci.yml)

Student release of the FlexNPU research platform, used for the AI Hardware
course. Runs entirely on a Linux host — no FPGA, no board, no vendor
toolchain — and pairs a mechanistic Python simulator with the actual
SystemVerilog RTL under automated regression.

The platform is set up for open-ended work on accelerator architecture,
resource provisioning, performance modeling, mapping, and dataflow. You
get real cycle-accurate RTL, a fast mechanistic model that can be swept
over thousands of design points in seconds, and operator traces
extracted from real MobileBERT and MobileNetV2 models.

## What you'll do

You'll be assigned to a project group. Each group:

1. Forks this repo and works in the fork.
2. Picks a research question from `PROJECT_IDEAS.md` (or proposes an
   adjacent one).
3. Writes down a concrete hypothesis or prediction before touching code.
4. Explores broadly in the mechanistic simulator — dozens or hundreds
   of points is normal.
5. Validates a small, well-chosen subset in RTL and against the provided
   workload traces.
6. Writes up *why* the observed behavior happens — the story matters
   more than the number.
7. Opens a pull request from the fork back to this course repo for
   the final submission.

`PROJECT_GUIDE.md` walks through how the tools compose and how to
run this kind of project end to end.

## Quick start

```
pip install -r requirements.txt
make onboarding
```

`make onboarding` runs:

- the mechanistic simulator's regression tests (`simulator/ctb_mech/`);
- byte-exact Verilator GEMM correctness for `rtl/compute_tile_bram.sv`;
- an Icarus Verilog run of the CTB regime testbench;
- the C++ event-driven simulator's `ctest` suite (`simulator/core/`);
- simulator-vs-RTL parity for a canonical `(K, D)` point.

If all five are green you're ready to work.

## Project directions

Two broad lanes; concrete starter questions live in `PROJECT_IDEAS.md`.

- **NPU realization and resource co-design** — decoupling depth, FIFO
  capacity, drain policy, array shape, and the interactions between
  them. Predict from the mechanistic model, then falsify in RTL.
- **Performance modeling, mapping, and dataflow** — turn a workload's
  operator/tile trace into an execution schedule, model the resulting
  service and stall structure, and evaluate on real MobileBERT /
  MobileNetV2 shape distributions.

You are not restricted to these; adjacent questions grounded in the
same platform are welcome. Talk to us first if you're unsure.

## Workloads

Under `workloads/`:

- **synthetic** — parameterized shape generators for controlled sweeps.
- **mobilebert** — operator traces extracted from the real INT8
  MobileBERT model via the canonical extraction flow.
- **mobilenetv2** — operator traces extracted from the real INT8
  MobileNetV2 model, same flow.

These let you evaluate the model against realistic tile/site shape
distributions rather than synthetic ones alone.

## Where to look next

- `PROJECT_GUIDE.md` — research workflow and how the tools compose.
- `PROJECT_IDEAS.md` — Lane A / Lane B expanded, with starter
  questions and weak-vs-strong examples.
- `docs/01_architecture_overview.md` — the platform at a glance.
- `docs/02_ctb_mech_and_regimes.md` — the mechanistic model and what
  each regime means.
- `docs/03_rtl_and_testbenches.md` — the RTL and how the testbenches
  drive it.
- `docs/04_ppa_flow.md` — optional Yosys + open-cell-library flow
  for reporting relative hardware cost.
- `simulator/core/SIMULATOR.md` — the C++ event-driven simulator.

## License

Apache-2.0 (see `LICENSE`).
