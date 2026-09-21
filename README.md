# FlexNPU-AI-Hardware

[![CI](https://github.com/YiminGao0113/FlexNPU-AI-Hardware/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/YiminGao0113/FlexNPU-AI-Hardware/actions/workflows/ci.yml)

FlexNPU is a full-stack accelerator research platform: real ONNX
models go through graph and operator analysis, workload realization,
analytical and cycle-level modeling, RTL, an FPGA implementation, and
end-to-end execution on an FPGA prototype under a real host runtime.
This repo is the **student-facing release** of that stack — enough
to explore architecture / resource and mapping / dataflow questions
using the same simulator models, RTL, and workload shapes used by
FlexNPU.

```
    Real models / ONNX
       │
       ▼
    operator + graph analysis                          ┐  research
       │  (quantization, operator coverage, shapes)    │  repo
       ▼                                               ┘
    real workload traces
       │
       ▼                                               ┐
    workload realization                               │
       │  (mapping, tiling, persistence, split-K)      │
       ▼                                               │
    analytical / roofline modeling                     │
       │                                               │
       ▼                                               │  student
    event-driven C++ simulator                         │  release
       │  (architecture + mapping + resource behavior) │  (this repo)
       ▼                                               │
    CTB mechanistic model                              │
       │  (service / buffering / bottleneck behavior)  │
       ▼                                               │
    RTL  ─────────►  optional synthesis / PPA          │
       │  (correctness + cycle-level validation)       │
       ▼                                               ┘
    FPGA implementation                                ┐
       │                                               │
       ▼                                               │  research
    FPGA prototype                                     │  repo
       │                                               │
       ▼                                               │
    host runtime + end-to-end workloads                │
       (counters, latency, realized system behavior)   ┘
```

## The design space

FlexNPU is exploring interactions across:

    workload × architecture × mapping / dataflow
    × resource provisioning × operator realization
    × runtime / system realization

Concrete axes include array geometry, `K_TILE` and other execution-
granularity knobs, FIFO / SRAM capacity, credit / outstanding-work
depth, service organization, tiling, loop order, split-K,
persistence / reuse, materialization, and precision. Student
projects here focus on the **architecture / resource** and
**mapping / dataflow** axes on the middle of the stack; interesting
findings can be taken further through the FPGA and runtime layers
in the research repo. Students work in their own fork of this repo
and open a PR back when they're done.

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

`examples/` contains six short scripts that wire the pieces together
into a progression from local mechanism to real-workload mapping
research:

- `01_hello_ctb.py` — mechanistic CTB model at one `(K, D, CAP)`
  point; prints regime, service cycles, FIFO peak.
- `02_regime_walk.py` — sweep K and watch the model move through
  the service regimes.
- `03_rtl_vs_model_parity.py` — mechanistic model and iverilog RTL
  side by side at a chosen point.
- `04_workload_replay.py` — replay the MobileBERT trace through
  the mechanistic model.
- `05_workload_analytical_vs_sim.py` — the same trace through the
  analytical roofline and the C++ event-driven simulator; prints
  the gap and attributes it to the simulator's directly counted
  buckets plus an accounting residual.
- `06_mapping_compare.py` — the same trace under two mapping /
  dataflow realizations (persist off vs hit); prints which
  realized quantities move and which stay fixed.

`analysis/roofline.py` also runs as a standalone script:

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
