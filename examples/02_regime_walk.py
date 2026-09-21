"""
02_regime_walk — walk K to observe each CTB regime.

Runs the mechanistic simulator at a small ladder of K values and prints
one row per K with the regime label. A student should be able to read
this table and say, in one sentence per row, which mechanism dominates.
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from tools.config_bridge import load_configs, to_ctb_params  # noqa: E402
from simulator.ctb_mech.ctb_mech import CTBMechanistic       # noqa: E402


K_LADDER = (32, 128, 512, 1024, 2048)


def main() -> int:
    arch, ctb = load_configs("configs/ctb/baseline.yaml")
    header = f"{'K':>6}  {'service':>8}  {'exec':>6}  {'wait':>6}  {'next':>6}  {'stall':>6}  {'rp_pk':>5}  {'fifo_pk':>7}  regime"
    print(header)
    print("-" * len(header))
    for K in K_LADDER:
        p = to_ctb_params(arch, ctb, K=K)
        r = CTBMechanistic(p).run()
        print(f"{K:>6}  {r.service_cycles:>8}  {r.exec_cycles:>6}  {r.wait_cycles:>6}"
              f"  {r.next_cycles:>6}  {r.credit_stall_cyc:>6}  {r.rp_peak:>5}"
              f"  {r.fifo_peak_all:>7}  {r.regime}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
