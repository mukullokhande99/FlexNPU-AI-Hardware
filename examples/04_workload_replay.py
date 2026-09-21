"""
04_workload_replay — replay a prepared workload trace through ctb_mech.

Reads a workload trace JSON (from workloads/mobilenetv2/, workloads/mobilebert/,
or workloads/synthetic/) and runs ctb_mech per site. Emits a CSV with per-site
service cycles, regime, credit-stall, FIFO peak, and overflow flag; also prints
a short summary table.

To keep the example self-contained, sites with K > MAX_K (the arch's
act_buffer capacity) are skipped and reported at the end. This is a real
architectural constraint students will need to reason about (Lane B B3
touches on split-K).
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from tools.config_bridge import load_configs, to_ctb_params  # noqa: E402
from simulator.ctb_mech.ctb_mech import CTBMechanistic       # noqa: E402
from analysis.roofline import _load_trace                    # noqa: E402


REGIME_COUNTS: dict = {}


def run_trace(trace_path: str, config_path: str, out_csv: str | None) -> None:
    arch, ctb = load_configs(config_path)
    sites = _load_trace(trace_path)
    max_k = int(arch["storage"]["act_buffer"]["capacity_words"])

    rows = []
    skipped = []
    total_service = 0
    for s in sites:
        K = int(s["K"])
        name = s.get("name", f"site_{s.get('idx', '?')}")
        if K > max_k:
            skipped.append((name, K))
            continue
        p = to_ctb_params(arch, ctb, K=K)
        r = CTBMechanistic(p).run()
        REGIME_COUNTS[r.regime] = REGIME_COUNTS.get(r.regime, 0) + 1
        rows.append(dict(
            name=name, K=K, N=s.get("N", ""), N_padded=s.get("N_padded", ""),
            service_cycles=r.service_cycles,
            credit_stall_cyc=r.credit_stall_cyc,
            fifo_peak_all=r.fifo_peak_all,
            rp_peak=r.rp_peak,
            regime=r.regime,
            overflow=int(r.overflow),
        ))
        total_service += r.service_cycles

    if out_csv:
        with open(out_csv, "w", newline="") as fh:
            wr = csv.DictWriter(fh, fieldnames=list(rows[0].keys()) if rows else
                                ["name","K","service_cycles","regime"])
            wr.writeheader()
            wr.writerows(rows)
        print(f"wrote {len(rows)} rows to {out_csv}")

    print()
    print(f"total sites replayed : {len(rows)}")
    print(f"total service cycles : {total_service}")
    print(f"regime distribution  : {REGIME_COUNTS}")
    if skipped:
        print(f"skipped (K > MAX_K={max_k}):")
        for name, K in skipped[:10]:
            print(f"  {name}: K={K}")
        if len(skipped) > 10:
            print(f"  ... {len(skipped) - 10} more")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--trace", required=True,
                    help="path to a workload trace JSON")
    ap.add_argument("--config", default="configs/ctb/baseline.yaml")
    ap.add_argument("--out-csv", default=None)
    args = ap.parse_args()
    run_trace(args.trace, args.config, args.out_csv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
