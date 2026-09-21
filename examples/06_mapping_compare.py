"""
06_mapping_compare — same workload, two mapping / dataflow realizations.

Reads a workload trace and runs the C++ event-driven simulator twice
on the same shape set:
  A) persist=off — every dispatch does its own WGT DMA (baseline).
  B) persist=hit — WGT residency is treated as ideal (WGT DMA elided).

The two runs differ only in the mapping / dataflow realization
(persistence policy). Everything else — the array, the shapes, the
dispatches — is identical. The example prints wall cycles, WGT DMA
bytes, host cycles, and persist counters side by side.

The mechanism this exercises is real: `PersistPolicy` gates whether
the WGT-DMA stream fires per dispatch. Comparing (A) and (B) shows
which realized quantity actually moves when you change a mapping /
dataflow decision — and which quantity stays fixed because it lives
on a different resource path.

Usage:
  python3 examples/06_mapping_compare.py \\
      [--trace workloads/mobilebert/mb_flex_sites.json]
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


def _load_trace(trace_path: str) -> list[dict]:
    with open(trace_path) as f:
        return json.load(f)


def _write_shapes_tsv(sites: list[dict], out_path: str) -> None:
    with open(out_path, "w") as f:
        f.write("# M K N count\n")
        for s in sites:
            f.write(f"{s['M']}\t{s['K']}\t{s['N']}\t{s.get('count', 1)}\n")


def _run_sim(shapes_path: str, persist: str) -> dict:
    if not os.path.exists(SIM_BIN):
        raise SystemExit(
            f"C++ simulator not built at {SIM_BIN}. Run `make sim_core` first."
        )
    r = subprocess.run(
        [SIM_BIN, "trace", "--from-shapes", shapes_path,
         "--persist", persist, "--no-summary", "--json"],
        capture_output=True, text=True, check=True,
    )
    return json.loads(r.stdout)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", default=os.path.join(
        REPO, "workloads", "mobilebert", "mb_flex_sites.json"))
    args = ap.parse_args()

    sites = _load_trace(args.trace)
    workload = os.path.basename(os.path.dirname(args.trace))
    print(f"workload: {workload}   sites: {len(sites)}")

    with tempfile.NamedTemporaryFile("w", suffix=".tsv", delete=False) as f:
        shapes_path = f.name
    try:
        _write_shapes_tsv(sites, shapes_path)
        a = _run_sim(shapes_path, "off")
        b = _run_sim(shapes_path, "hit")
    finally:
        os.unlink(shapes_path)

    def fmt_bytes(n: int) -> str:
        for u in ("B", "KB", "MB", "GB"):
            if n < 1024 or u == "GB":
                return f"{n:.1f} {u}"
            n /= 1024
        return f"{n} B"

    def delta_pct(new: float, base: float) -> str:
        if base == 0:
            return "n/a"
        return f"{(new - base) / base * 100:+.1f}%"

    print()
    print(f"{'quantity':<28} {'A: persist=off':>18} {'B: persist=hit':>18} {'Δ':>10}")
    print("-" * 78)
    rows = [
        ("dispatches_run",         a["dispatches_run"],     b["dispatches_run"]),
        ("wall_cycles",            a["wall_cycles"],        b["wall_cycles"]),
        ("wall_seconds",           a["wall_seconds"],       b["wall_seconds"]),
        ("dma_bytes_act (B)",      a["dma_bytes_act"],      b["dma_bytes_act"]),
        ("dma_bytes_wgt (B)",      a["dma_bytes_wgt"],      b["dma_bytes_wgt"]),
        ("dma_bytes_out (B)",      a["dma_bytes_out"],      b["dma_bytes_out"]),
        ("total_bank_fill_cycles", a["total_bank_fill_cycles"], b["total_bank_fill_cycles"]),
        ("total_host_pack_cycles", a["total_host_pack_cycles"], b["total_host_pack_cycles"]),
        ("total_host_mmio_cycles", a["total_host_mmio_cycles"], b["total_host_mmio_cycles"]),
        ("persist_hits",           a["persist_hits"],       b["persist_hits"]),
        ("persist_misses",         a["persist_misses"],     b["persist_misses"]),
    ]
    for name, va, vb in rows:
        if isinstance(va, float) or isinstance(vb, float):
            print(f"{name:<28} {va:>18.6f} {vb:>18.6f} {delta_pct(vb, va):>10}")
        else:
            print(f"{name:<28} {va:>18,} {vb:>18,} {delta_pct(vb, va):>10}")

    print()
    print("The mapping choice moves what you'd expect it to move")
    print("(WGT DMA bytes, persist counters). Whether it also moves")
    print("wall cycles depends on whether WGT DMA is on the critical")
    print("path for this workload — which is exactly the Lane B")
    print("question the example is designed to prompt.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
