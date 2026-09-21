# Project Guide

This repo exposes the FlexNPU research platform so students can do
architecture-aware research on it. The course instructors and TAs
decide how projects are structured, delivered, and graded. What
this document does is describe the tools and point at the research
directions I would like student help on.

## Tools

| Layer | Where | Cost | Good for |
|:---|:---|:---|:---|
| Analytical / roofline | `analysis/roofline.py` | ms | intuition, first-pass bounds |
| C++ event-driven simulator | `simulator/core/` | seconds/point; sweep-friendly | architecture / mapping / dispatch |
| CTB mechanistic model | `simulator/ctb_mech/` | ms/point | producer / drain / credit / FIFO service |
| RTL testbenches | `rtl/` + `tb/` | seconds–minutes | validation of chosen points |
| Optional PPA sketch | `docs/04_ppa_flow.md` | minutes | relative area / cell count |

The mechanistic layer and the C++ simulator are the exploration
surface. RTL is where you confirm a chosen point.

## Research directions

Two, described in `PROJECT_IDEAS.md`:

- **Lane A** — NPU realization and resource co-design.
- **Lane B** — mapping and dataflow realization.

## Workloads

`workloads/` ships a synthetic shape generator and operator traces
extracted from INT8 MobileBERT and MobileNetV2. Real-workload
transfer is where a Lane A or Lane B result becomes interesting.
