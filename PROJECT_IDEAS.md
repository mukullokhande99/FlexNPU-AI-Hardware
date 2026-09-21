# Project Ideas

FlexNPU exposes a fairly broad accelerator design space across both **hardware realization** and **mapping/dataflow choices**. The directions below are intended as possible research areas, not fixed assignments. Students can choose one question, combine several related ideas, or propose an adjacent direction using the same framework.

The goal is to use the analytical models, simulators, RTL, and real workload traces to understand how a design or mapping choice changes the realized execution behavior — not just to sweep parameters and report the fastest point.

## Lane A — NPU realization and resource co-design

This direction explores how hardware design choices interact with workload structure.

Possible design axes include:

- array geometry;
- `K_TILE` and execution granularity;
- FIFO depth;
- SRAM / local-buffer size and partitioning;
- outstanding-work / credit depth;
- drain and service organization;
- flow-control policy;
- producer vs consumer service balance;
- implementation cost such as area, registers, memory resources, and timing.

Possible directions include:

- how array shape changes the active bottleneck;
- how execution granularity changes producer/drain balance;
- how much FIFO, SRAM, or credit capacity is actually useful;
- when additional provisioning becomes dormant;
- when reducing a resource becomes unsafe;
- whether resource-sizing rules transfer across workloads or architectures;
- performance–cost frontiers across architecture choices;
- hardware-cost evaluation of new architectural mechanisms.

## Lane B — Mapping and dataflow realization

This direction explores how mapping choices become actual execution behavior on the accelerator.

Possible design axes include:

- tiling and tile order;
- loop order;
- split-K / K-chunking;
- persistence and reuse;
- materialization;
- dispatch order and grouping;
- mapping-dependent buffering;
- local-storage pressure;
- traffic and data movement.

Possible directions include:

- where analytical or roofline predictions diverge from realized execution;
- when two different mappings produce similar realized behavior;
- when two mappings have the same latency but very different traffic or buffering behavior;
- how persistence or reuse changes data movement and service;
- when K-chunking changes the active bottleneck;
- which workload characteristics make mapping choices important;
- when dispatch order changes execution behavior;
- how mapping interacts with architecture choices such as array shape or SRAM capacity.

A good project does not need to cover the whole space. A narrow question is useful if it can establish:

```text
design or mapping choice
        ↓
predicted mechanism
        ↓
measured realized behavior
        ↓
explanation
```

The important result is usually not just which configuration is faster, but **why the behavior changes and whether that explanation generalizes**.
