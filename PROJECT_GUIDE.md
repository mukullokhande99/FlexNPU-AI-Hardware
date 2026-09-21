# FlexNPU-AI-Hardware — Project Guide

This document is the student handbook for the AI Hardware final project.

## Philosophy

**Bounded freedom.** You pick a research question inside a labeled lane
(see `PROJECT_IDEAS.md`). We supply the tooling, the RTL, the traces,
and the workflow. You supply the hypothesis, the experiment, and the
causal explanation.

The workflow is:

```
question -> hypothesis -> prediction ->
simulator exploration -> <=5 RTL validation points ->
real-workload replay -> causal explanation -> PR
```

Sweeps without a preceding prediction are penalized in review. Causal
explanation is weighted more than raw numbers. Real workload relevance
is required unless you justify otherwise.

## Onboarding exercise

Everyone completes this in the first week. It is one PR containing your
`projects/<gh-handle>/lab1.md` file and the four artifacts below.

**Time budget:** ~60-90 minutes.

Steps:

1. **Environment check.** Run `make check`. All targets return exit 0.
   Paste the output.
2. **Byte-exact RTL correctness.** Run `make ctb_correctness`. Confirm
   `ALL CASES PASS`. Paste the tail of the log.
3. **Regime walk.** Run `make regime_walk`. In one sentence per row,
   annotate *which* CTB regime dominates each K and *why*.
4. **Change one YAML parameter, predict, verify.**
   - Edit `configs/ctb/baseline.yaml`: change `decouple_max: 3` to
     `decouple_max: 1`.
   - **Before running anything**, write in your `lab1.md` what you
     predict will happen to `credit_stall_cyc` at K=1024 relative to
     the baseline value.
   - Run `make parity K=1024`. Paste the parity table. State whether
     the prediction held.
5. **Restore** `decouple_max: 3` and confirm the parity table returns
   to the baseline numbers.

Pass criteria (auto-checked where possible):
- `make onboarding` returns exit 0.
- The `lab1.md` file contains the four artifacts above and the
  annotations in your own words.

## Project workflow

After onboarding is merged:

1. **Choose a lane and question** from `PROJECT_IDEAS.md`. Open a
   GitHub issue titled `research_question: <lane> <slug>` with the
   fields below. Approved variants are allowed.

   ```
   Lane: A | B
   Seed question: A1 | A2 | ... | B6 (or "variant of Ax/Bx")
   Question (1 short paragraph):
   Why it matters (1-2 sentences):
   Expected effort: low | medium | high
   ```

   TA approves within 48h.

2. **Create your project directory:** `projects/<gh-handle>/`. This is
   yours; all your artifacts live here.

3. **Baseline commit:** run the untouched simulator + a chosen RTL
   configuration on your workload of interest. Commit
   `projects/<gh-handle>/baseline.md` + raw CSVs. Baseline lands
   *before* any code modification.

4. **Hypothesis + prediction commit:** write
   `projects/<gh-handle>/hypothesis.md` with a **quantitative**
   prediction. Commit *before* editing `rtl/`, `simulator/`,
   `analysis/`, or `workloads/`.

5. **Simulator exploration.** Use `simulator/ctb_mech/` + roofline.
   Sweep only what the prediction requires.

6. **RTL validation:** rebuild RTL via `make ctb_eval CONFIG=...`
   (or the correctness gate for numerical-correctness changes) at
   at most 5 chosen (K, D, CAP, ...) cells. Log the parity table.

7. **Real workload replay:** replay a `workloads/mobilebert/` or
   `workloads/mobilenetv2/` trace through the modified simulator.

8. **Causal explanation:** `projects/<gh-handle>/explanation.md`,
   <= 600 words. Which of the six CTB mechanism layers moved? Which
   regime changed? What surprised you?

9. **Open PR** using `.github/pull_request_template.md`. Fill out
   every section. Attach graphs. Link the hypothesis commit.

10. **Review:** one TA + one peer student. Merge on green CI +
    two thumbs-up.

## Grading rubric

Five axes, each scored 0-4.

| Axis | Weight |
|---|---|
| Question quality — falsifiable, scoped, engages a real FlexNPU concept | 15% |
| Prediction discipline — hypothesis commit predates the modification; prediction is quantitative | 20% |
| Simulator/RTL parity rigor — meaningful validation points; parity analyzed | 25% |
| Causal explanation — mechanism / regime attribution, not a plot dump | 25% |
| Real-workload relevance — transfer to at least one provided trace | 15% |

Bonus (up to +10%):
- Discovering a framework bug with a fix PR.
- Contributing a reusable tool or trace.
- Independently reproducing another student's finding.

Deductions:
- Sweeps without a preceding prediction: -5% each.
- Modifying `rtl/`, `simulator/ctb_mech/ctb_mech.py`, or
  `tools/config_bridge.py` without TA approval: PR bounced.

## Upstreaming to canonical FlexNPU

Student PRs never auto-flow to canonical.

- Merged student PRs live in this repo only.
- Once per term, TAs nominate a subset. Instructor + canonical
  maintainer independently reproduce the finding in a canonical
  checkout, without copying student code.
- If reproducible, canonical implements the change; student is credited
  in the canonical release notes.

## Guardrails (v0.1)

- No absolute paths (`/home/...`) in student files. CI grep checks.
- No secrets (`.env`, `*_key`, `*.pem`, `.claude*`). CI grep checks.
- No `rtl/`, `simulator/ctb_mech/ctb_mech.py`, `tools/config_bridge.py`
  edits without TA approval. Convention-locked in v0.1 (CODEOWNERS is
  a v0.2 concern).
- Sweep gate: >500 config runs prints a warning; keep runs small.

## Known scope of v0.1

- No compiler / ONNX / IREE integration. Traces ship pre-generated.
- No FPGA / Kria / Vivado / ORT runtime.
- No physical hardware perf counters — students see simulator counters.
- No C++ event-driven simulator — the mechanistic Python model plus
  RTL testbenches cover both lanes.
- MobileBERT and MobileNetV2 traces are illustrative subsets in v0.1;
  full regeneration from public HF checkpoints is a v0.2 deliverable.
