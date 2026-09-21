# FlexNPU-AI-Hardware — Project Ideas

Two lanes, six seed questions each. Pick one, or propose an approved
variant. Each is *research-relevant* to FlexNPU beyond the course.

## Lane A — NPU realization / resource co-design

Given the same array datapath, how does the surrounding realization
(buffering, credit, FIFO depth, drain policy, per-op FSM) determine
which service regime the accelerator lives in on a given workload, and
what does the realization frontier look like?

- **A1.** How does array geometry (16x16 vs 16x32 vs 32x16) move the
  active bottleneck across the MobileNetV2 and MobileBERT trace mixes?
  Call out where the mechanism model's compute-per-pass anchor breaks.
- **A2.** How should `OUT_FIFO_DEPTH` (CAP) and `DECOUPLE_MAX` (D) be
  co-designed? Find a Pareto frontier for one real workload. When
  does extra buffering become **dormant** — provisioned but unused?
- **A3.** For a chosen real workload, predict — before any RTL run —
  the exact K value at which storage-unsafe overflow occurs at
  (D=4, CAP=24). Confirm with iverilog. Repeat for a second workload.
- **A4.** How does `K_TILE` in {8, 16, 32} affect producer-vs-drain
  service balance? Empirically extract the new per-pass cycle
  constant from RTL and update the mechanism model.
- **A5.** What is the smallest safe realization (D, CAP, drain policy)
  that keeps **both** MobileBERT and MobileNetV2 out of the
  credit-limited and storage-unsafe regimes? Justify with the model,
  validate at <=5 RTL cells.
- **A6.** Can the mechanism model predict an *unseen* realization
  (D, CAP, drain_ii triple not in `regime_sweep_reference.csv`)
  before RTL confirms it? Pick two unseen cells, register the
  prediction as a commit, then measure.

## Lane B — performance modeling / mapping / dataflow

Given a workload and a fixed accelerator, which mapping (loop order,
tile order, split-K, reuse strategy) works best, and why?

- **B1.** For each provided real workload, when does the roofline
  bound *fail* to predict realized service? Attribute the gap to the
  four regimes; identify the sites where the gap is largest.
- **B2.** Which mapping changes actually change *behavior*, vs.
  changing only *nominal configuration*? Prove one behavioral-
  equivalence class exists.
- **B3.** How do tiling and reuse/persistence choices affect per-row
  FIFO pressure on MobileBERT's Q/K/V/proj sites? Recommend a mapping.
- **B4.** For a large-K matmul, sweep `K_chunk` and identify the
  sweet spot. Predict the sweet spot from producer/drain amortization
  *before* the sweep.
- **B5.** Which workload features (K/N ratio, N_padded slack, site
  count, count-per-shape distribution) predict *mapping sensitivity*?
  Rank MobileBERT and MobileNetV2 sites by predicted sensitivity,
  then measure.
- **B6.** Can profiling (per-state cycle counts, credit_stall_cyc,
  fifo_peak_all, rp_hist) explain outliers between the analytical
  roofline prediction and the RTL cycle count? Pick 3 outlier sites,
  explain each in mechanism terms.

## Common project methodology

Every project must show:

1. A **research question** and a **quantitative hypothesis**.
2. A **prediction** committed *before* the modification.
3. **Simulator exploration** using `simulator/ctb_mech/` (and
   optionally `analysis/roofline.py`).
4. **RTL validation** of at most 5 selected points.
5. A **real-workload replay** on one of the provided traces.
6. A **causal explanation** in mechanism terms (which of the six CTB
   layers, which regime).

Sweeps for their own sake are not encouraged. Grading weights causal
explanation and prediction discipline over raw plot count.

## Approved variants

If none of the seed questions fits your interest, open a
`research_question` issue proposing a variant, with:

- Which seed question it derives from (name it).
- What is different from the seed question.
- Why the variant is still tractable in the project period.
- Which of the six methodology gates you will hit.

TA approves within 48h.
