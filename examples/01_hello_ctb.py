"""
01_hello_ctb — run the CTB mechanistic simulator once and print the result.

Reads the default configs/ctb/baseline.yaml, builds a CTBParams from it,
and runs one dispatch. Prints the compact result summary. Runs in <100ms.
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from tools.config_bridge import load_configs, to_ctb_params  # noqa: E402
from simulator.ctb_mech.ctb_mech import CTBMechanistic       # noqa: E402


def main() -> int:
    arch, ctb = load_configs("configs/ctb/baseline.yaml")
    params = to_ctb_params(arch, ctb)
    result = CTBMechanistic(params).run()

    print(f"K={params.K}  D={params.D}  CAP={params.CAP}  drain_ii={params.drain_ii}")
    print(f"  regime           : {result.regime}")
    print(f"  service_cycles   : {result.service_cycles}")
    print(f"  exec_cycles      : {result.exec_cycles}")
    print(f"  wait_cycles      : {result.wait_cycles}")
    print(f"  next_cycles      : {result.next_cycles}")
    print(f"  credit_stall_cyc : {result.credit_stall_cyc}")
    print(f"  rp_peak          : {result.rp_peak}")
    print(f"  fifo_peak_all    : {result.fifo_peak_all}")
    print(f"  overflow         : {result.overflow}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
