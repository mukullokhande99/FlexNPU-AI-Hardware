# FlexNPU-AI-Hardware

[![CI](https://github.com/YiminGao0113/FlexNPU-AI-Hardware/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/YiminGao0113/FlexNPU-AI-Hardware/actions/workflows/ci.yml)

FlexNPU is a full-stack accelerator research platform: real ONNX
models go through graph and operator analysis, workload realization,
analytical and cycle-level modeling, RTL, an FPGA implementation, and
end-to-end execution on an FPGA prototype under a real
host runtime. This repo is the **student slice** of that stack —
enough to do real architecture- and mapping-aware research on the
same RTL and workload shapes the platform runs.

```
    Real models / ONNX
       │
       ▼
    operator + graph analysis                          ┐
       │  (quantization, operator coverage, shapes)    │
       ▼                                               │  research
    workload realization                               │  repo
       │  (mapping, tiling, persistence, split-K)      │
       ▼                                               ┘
    analytical / roofline modeling                     ┐
       │                                               │
       ▼                                               │
    event-driven C++ simulator                         │
       │  (architecture + mapping + resource behavior) │  student
       ▼                                               │  release
    CTB mechanistic model                              │  (this repo)
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
