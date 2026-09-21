# FlexNPU-AI-Hardware (v0.1)

A minimal, real-hardware student platform for the "AI Hardware" course.
Extracted from the canonical FlexNPU research repo. Runs entirely on a
Linux host — no FPGA, no board, no vendor toolchain.

## What you get

- **Real RTL** — the FlexNPU CTB compute path (`rtl/`), simulated with
  iverilog and Verilator. Byte-exact GEMM correctness gate.
- **A cycle-accurate Python mechanism model** (`simulator/ctb_mech/`) of
  the CTB service pipeline (producer FSM, credit, per-row FIFOs, drain
  FSM).
- **A minimal roofline layer** (`analysis/roofline.py`) for analytical
  compute-vs-bandwidth bounds.
- **A one-YAML config bridge** (`tools/config_bridge.py`) that
  translates one `configs/ctb/*.yaml` into both iverilog `-P` overrides
  and Python `CTBParams`.
- **Prepared workload traces** for MobileBERT and MobileNetV2 (shape
  metadata only; INT8-quantized site descriptors).
- **Bounded-freedom project workflow** — see `PROJECT_GUIDE.md`.

## Prerequisites

```bash
sudo apt install iverilog verilator
python3 -m pip install -r requirements.txt   # onnx not required in v0.1
```

Any Linux host works. No `/dev/mem`, no root, no board.

## Quickstart

```bash
make check              # tools present + 8/8 ctb_mech regressions pass
make ctb_correctness    # Verilator: byte-exact GEMM matches numpy reference
make ctb_eval           # iverilog: run regime TB at K=1024, D=3
make regime_walk        # Python: sweep K over the four service regimes
make parity             # Python + iverilog: sim vs RTL, side by side
make workload_replay    # Python: replay MobileBERT trace through ctb_mech
```

## Layout

- `rtl/` — 5 SystemVerilog files (mac, fifo, systolic, mm, compute_tile_bram).
- `tb/` — regime TB (iverilog) + correctness TB (Verilator).
- `configs/arch/` — architectural manifests (16x16 baseline + 8x8 variant).
- `configs/ctb/baseline.yaml` — the CTB runtime knobs a student edits.
- `simulator/ctb_mech/` — Python mechanistic simulator + 8 regression tests.
- `analysis/roofline.py` — analytical compute/bandwidth bound.
- `tools/config_bridge.py` — YAML → both simulators, one source of truth.
- `workloads/` — synthetic generator + MobileBERT + MobileNetV2 traces.
- `examples/` — four short scripts wiring the pieces together.
- `docs/` — architecture overview, mechanism story, RTL/TB walkthrough.
- `PROJECT_GUIDE.md` — student workflow, onboarding exercise, rubric.
- `PROJECT_IDEAS.md` — Lane A and Lane B seed questions.

- 
