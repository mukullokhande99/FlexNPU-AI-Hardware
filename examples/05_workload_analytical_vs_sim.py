"""
05_workload_analytical_vs_sim — real workload trace: analytical bound
vs realized C++ event-driven simulator.

Reads a shipped workload trace (MobileBERT by default), computes the
analytical compute-bound cycle count from `2 · M · K · N / (array_M
· array_N)` per site, then runs the C++ event-driven simulator on
the same set of shapes via the trace-driven CLI. Prints the gap
between the analytical expectation and realized simulator behavior,
and decomposes the realized wall cycles into the components the
simulator exposes (host pack, host MMIO, DMA bank fill).

Lane B onboarding: the question this example poses is what
additional service / architecture effects account for the gap
between the analytical roofline and the realized simulator, not
whether either number matches a specific target.

Usage:
  python3 examples/05_workload_analytical_vs_sim.py \\
      [--trace workloads/mobilebert/mb_flex_sites.json] \\
      [--arch configs/arch/npu_core_16x16.yaml]
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SIM_BIN = os.path.join(REPO, "simulator", "core", "build", "flexnpu_sim")


def _load_arch_geom(arch_path: str) -> tuple[int, int]:
    """Read array_m / array_n from an arch YAML."""
    import yaml
    with open(arch_path) as f:
        arch = yaml.safe_load(f)
    g = arch["geometry"]
    return int(g["array_m"]), int(g["array_n"])


def _load_trace(trace_path: str) -> list[dict]:
    with open(trace_path) as f:
        return json.load(f)


def _write_shapes_tsv(sites: list[dict], out_path: str) -> int:
    """Write `M K N count` per line. Return number of rows."""
    rows = 0
    with open(out_path, "w") as f:
        f.write("# M K N count\n")
        for s in sites:
            f.write(f"{s['M']}\t{s['K']}\t{s['N']}\t{s.get('count', 1)}\n")
            rows += 1
    return rows


def _run_sim(shapes_path: str) -> dict:
    if not os.path.exists(SIM_BIN):
        raise SystemExit(
            f"C++ simulator not built at {SIM_BIN}. Run `make sim_core` first."
        )
    r = subprocess.run(
        [SIM_BIN, "trace", "--from-shapes", shapes_path, "--no-summary", "--json"],
        capture_output=True, text=True, check=True,
    )
    return json.loads(r.stdout)


def _analytical(sites: list[dict], array_m: int, array_n: int) -> dict:
    """Compute-bound cycles under perfect utilization of the array."""
    peak_macs_per_cycle = array_m * array_n
    total_macs = 0
    total_bound_cycles = 0
    for s in sites:
        M, K, N = s["M"], s["K"], s["N"]
        count = s.get("count", 1)
        macs = 2 * M * K * N * count
        bound_cyc = macs // peak_macs_per_cycle
        total_macs += macs
        total_bound_cycles += bound_cyc
    return dict(total_macs=total_macs, total_bound_cycles=total_bound_cycles)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", default=os.path.join(
        REPO, "workloads", "mobilebert", "mb_flex_sites.json"))
    ap.add_argument("--arch", default=os.path.join(
        REPO, "configs", "arch", "npu_core_16x16.yaml"))
    args = ap.parse_args()

    array_m, array_n = _load_arch_geom(args.arch)
    sites = _load_trace(args.trace)
    workload = os.path.basename(os.path.dirname(args.trace))
    print(f"workload: {workload}   sites: {len(sites)}   "
          f"array: {array_m}x{array_n}")

    ana = _analytical(sites, array_m, array_n)

    with tempfile.NamedTemporaryFile("w", suffix=".tsv", delete=False) as f:
        shapes_path = f.name
    try:
        _write_shapes_tsv(sites, shapes_path)
        realized = _run_sim(shapes_path)
    finally:
        os.unlink(shapes_path)

    wall = realized["wall_cycles"]
    bank_fill = realized["total_bank_fill_cycles"]
    host_pack = realized["total_host_pack_cycles"]
    host_mmio = realized["total_host_mmio_cycles"]
    bound = ana["total_bound_cycles"]

    gap = wall - bound
    residual = wall - bank_fill - host_pack - host_mmio  # ≈ compute + drain

    print()
    print(f"{'quantity':<40} {'cycles':>14}")
    print("-" * 56)
    print(f"{'analytical compute bound (Σ macs / peak)':<40} {bound:>14,}")
    print(f"{'realized wall (event-driven sim)':<40} {wall:>14,}")
    print(f"{'gap (wall − bound)':<40} {gap:>+14,}")
    print()
    print(f"{'realized decomposition':<40} {'cycles':>14} {'share':>8}")
    print("-" * 65)
    for name, cyc in [
        ("host pack cycles",  host_pack),
        ("host mmio cycles",  host_mmio),
        ("DMA bank-fill cycles", bank_fill),
        ("residual (compute + drain + serial gaps)", residual),
    ]:
        share = 100.0 * cyc / max(wall, 1)
        print(f"{name:<40} {cyc:>14,} {share:>7.1f}%")

    print()
    print("The gap you see is what the analytical model cannot explain.")
    print("The decomposition tells you which real service term dominates.")
    print("Lane B project: pick a workload region where the residual is")
    print("large and give a mechanism for it.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
