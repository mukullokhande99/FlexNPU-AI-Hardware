# Project PR — <lane>: <short title>

**Student:** <name>, <gh handle>
**Lane:** A | B
**Seed question (or approved variant):** A# / B# / "variant of ..."
**Branch:** student/<handle>/<slug>
**Project dir:** projects/<handle>/

## 1. Research question
One paragraph.

## 2. Hypothesis
Link to the commit that added `hypothesis.md`. That commit must
predate any change to `rtl/`, `simulator/`, `configs/`, `analysis/`,
or `workloads/`.

## 3. Baseline
- Config used:
- Baseline artifacts (paths):

## 4. Prediction (quantitative, made before the experiment)
Example: "At K=1024, halving DECOUPLE_MAX from 3 to 1 will increase
credit_stall_cyc by >= 30x; fifo_peak_all should not change."

## 5. What changed
File list, one line each.

## 6. Simulator results
- ctb_mech sweep result path:
- (optional) roofline analysis path:

## 7. RTL validation
Table of <=5 cells. sim service vs iverilog service. If any
|delta%| > 5%, explain in one paragraph.

## 8. Real-workload result
Which trace (MobileBERT or MobileNetV2, or justified other):
Per-site delta or workload-level delta:
Which sites benefited; which did not:

## 9. Causal explanation
Link to explanation.md (<=600 words).
Which regime moved? Which of the six CTB mechanism layers is
responsible?

## 10. Reproduction instructions
Exact commands to reproduce the key finding from a fresh clone.

## 11. Limitations
What you did not test; where your prediction did not cover.

## 12. Follow-up
Concrete next question(s).

## Checklist
- [ ] `make onboarding` passes on my branch
- [ ] CI green
- [ ] `hypothesis.md` commit predates any modification to `rtl/`,
      `simulator/`, `configs/`, `analysis/`, `workloads/`
- [ ] All raw results committed under `projects/<handle>/`
- [ ] Peer reviewer assigned in GitHub UI
