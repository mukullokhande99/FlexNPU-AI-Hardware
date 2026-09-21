# FlexNPU-AI-Hardware

[![CI](https://github.com/YiminGao0113/FlexNPU-AI-Hardware/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/YiminGao0113/FlexNPU-AI-Hardware/actions/workflows/ci.yml)

This repo is a **student slice of an ongoing FlexNPU research
project**, packaged for the AI Hardware course. It exposes the parts
of the platform students can use for architecture-aware research on
a Linux host — the mechanistic simulator, the C++ event-driven
simulator, the RTL, real workload traces, and a lightweight roofline
model. Everything runs without an FPGA, a board, or a vendor toolchain.

## The larger platform and where you plug in

The full FlexNPU pipeline goes from an ONNX model through a compiler
and runtime and executes on a physical Kria FPGA prototype. The
compiler, the runtime, and the physical deployment path stay in the
research repo. This student release ships everything else — the RTL,
the mechanistic and C++ simulators, the roofline model, and the
operator traces — so you can answer real questions using the same RTL
and the same workload shapes that end up on the physical prototype.

Students plug into the pipeline by forking this repo, working in
their fork, and opening a PR back:

```
FlexNPU research pipeline (owned by the research repo)
──────────────────────────────────────────────────────

    ONNX model
       │  operator extraction / site metadata
       ▼
    trace files ─────────────────────────────┐
       │                                     │
       ▼                                     │
    mechanistic sim + C++ sim + roofline     │
       │                                     │
       ▼                                     │      What students do
    RTL ─────────────────────────────────────┤      ────────────────
       │                                     │      1. fork this repo
       │  bitstream + runtime                │      2. work in your fork
       ▼                                     │      3. open a PR back
    physical Kria FPGA prototype ────────────┘      4. results we can
                                                       reproduce may
                                                       flow into the
                                                       research repo
```

The middle band — traces + simulators + RTL — is what you see and
work with in this repo. The compiler and runtime on either side are
what turn your work into (and out of) something running on the
physical prototype.

## Two research directions

Details in `PROJECT_IDEAS.md`.

- **Lane A** — NPU realization and resource co-design.
- **Lane B** — mapping and dataflow realization.

## Quick start

```
pip install -r requirements.txt
make onboarding
```

`make onboarding` runs the mechanistic-simulator regressions, the
Verilator byte-exact GEMM correctness check, the Icarus CTB regime
testbench, the C++ event-driven simulator's ctest suite, and a
simulator-vs-RTL parity point.

## Examples

`examples/` contains four short scripts that wire the pieces together:

- `01_hello_ctb.py` — run the mechanistic CTB model at one `(K, D, CAP)`
  point and print the regime, service cycles, and FIFO peak.
- `02_regime_walk.py` — sweep K and watch the model move through
  the four service regimes.
- `03_rtl_vs_model_parity.py` — the mechanistic model and iverilog
  RTL side by side at a chosen point.
- `04_workload_replay.py` — replay the MobileBERT trace (361
  operator sites) through the mechanistic model.

`analysis/roofline.py` prints a per-site roofline table for either
of the shipped traces:

```
python3 analysis/roofline.py \
    --arch configs/arch/npu_core_16x16.yaml \
    --trace workloads/mobilebert/mb_flex_sites.json
```

## Layout

- `simulator/ctb_mech/` — Python mechanistic CTB model.
- `simulator/core/` — C++ event-driven simulator.
- `analysis/roofline.py` — analytical baseline.
- `rtl/`, `tb/` — RTL and testbenches.
- `tools/config_bridge.py` — one YAML → both simulators + iverilog.
- `workloads/` — synthetic shape generator, MobileBERT trace,
  MobileNetV2 trace.
- `examples/`, `docs/`.

## Docs

- PROJECT_GUIDE.md, PROJECT_IDEAS.md
- docs/01_architecture_overview.md, docs/02_ctb_mech_and_regimes.md,
  docs/03_rtl_and_testbenches.md, docs/04_ppa_flow.md
- simulator/core/SIMULATOR.md

## License

Apache-2.0 (see LICENSE).
