"""
roofline — minimal analytical bound for FlexNPU workloads.

Per site inputs are (K, N, N_padded) — matching the workload trace
schema in workloads/. M is provided by the caller (or defaults to 1
for the attention/QKV-style single-token case). MACs per site are
2*M*K*N; bytes moved use the arch YAML's activation/accumulator
widths and the AXIS stream widths.

Two bounds:
  compute_bound_macs_per_s   = array_m * array_n * clock_hz
  bandwidth_bound_bytes_per_s = out_stream_width_bits/8 * clock_hz
                              + wgt_stream_width_bits/8 * clock_hz
                              + act_stream_width_bits/8 * clock_hz

Per-site sustained throughput is min(compute, bandwidth * intensity).
The report DataFrame has one row per site plus a workload-level summary
row. A crude PNG scatter plot is also emitted.

The math is deliberately simple; students are expected to extend it
(distinguishing bandwidth by stream, adding drain overhead, etc.).
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from pathlib import Path
from typing import Any, Dict, List, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.config_bridge import _read_yaml, REPO_ROOT  # noqa: E402


CLOCK_HZ_DEFAULT = 100_000_000  # 100 MHz — canonical FlexNPU FPGA prototype.


def _load_arch(arch_path: str | os.PathLike) -> Dict[str, Any]:
    p = Path(arch_path)
    if not p.is_absolute():
        p = (REPO_ROOT / p).resolve()
    return _read_yaml(p)


def _load_trace(trace_path: str | os.PathLike) -> List[Dict[str, Any]]:
    p = Path(trace_path)
    if not p.is_absolute():
        p = (REPO_ROOT / p).resolve()
    with p.open() as fh:
        data = json.load(fh)
    if isinstance(data, dict) and "sites" in data:
        data = data["sites"]
    return list(data)


def roofline(arch: Dict[str, Any], sites: Sequence[Dict[str, Any]],
             M: int = 1, clock_hz: float = CLOCK_HZ_DEFAULT) -> List[Dict[str, Any]]:
    """Return one report record per site plus a summary record."""
    g   = arch["geometry"]
    d   = arch["datapath"]
    ifc = arch["interface"]
    array_macs = int(g["array_m"]) * int(g["array_n"])

    peak_macs_per_s = array_macs * clock_hz
    act_bpb = int(ifc["act_stream_width_bits"]) // 8
    wgt_bpb = int(ifc["wgt_stream_width_bits"]) // 8
    out_bpb = int(ifc["out_stream_width_bits"]) // 8
    total_stream_bytes_per_s = (act_bpb + wgt_bpb + out_bpb) * clock_hz
    act_width_bytes = int(d["act_width_bits"]) // 8
    acc_width_bytes = int(d["acc_width_bits"]) // 8

    records: List[Dict[str, Any]] = []
    tot_macs = 0
    tot_bytes = 0
    tot_ceiling_cycles = 0.0
    for i, s in enumerate(sites):
        K = int(s.get("K", 0))
        N = int(s.get("N", 0))
        Npad = int(s.get("N_padded", N))
        count = int(s.get("count", 1))
        Mi = int(s.get("M", M))

        macs = 2 * Mi * K * N * count
        # Coarse per-site bytes: act (M*K) + wgt (K*N_padded) + out (M*N)
        b_act = Mi * K * act_width_bytes * count
        b_wgt = K * Npad * act_width_bytes * count
        b_out = Mi * N * acc_width_bytes * count
        b_tot = b_act + b_wgt + b_out
        arith_intensity = (macs / b_tot) if b_tot else float("inf")

        # Per-site sustained-throughput ceiling (macs/s), then translate
        # to cycles at clock_hz.
        bw_bound_macs_per_s = arith_intensity * total_stream_bytes_per_s
        ceiling_macs_per_s = min(peak_macs_per_s, bw_bound_macs_per_s)
        ceiling_cycles = macs / (ceiling_macs_per_s / clock_hz) if ceiling_macs_per_s else float("inf")
        bound = "compute" if peak_macs_per_s <= bw_bound_macs_per_s else "bandwidth"

        records.append(dict(
            idx=i, name=s.get("name", f"site_{i}"),
            M=Mi, K=K, N=N, N_padded=Npad, count=count,
            macs=macs, bytes_moved=b_tot, arith_intensity=arith_intensity,
            bound=bound, ceiling_macs_per_s=ceiling_macs_per_s,
            ceiling_cycles=ceiling_cycles,
        ))
        tot_macs += macs
        tot_bytes += b_tot
        tot_ceiling_cycles += ceiling_cycles

    records.append(dict(
        idx=-1, name="__WORKLOAD_TOTAL__",
        M=None, K=None, N=None, N_padded=None,
        count=len(sites),
        macs=tot_macs, bytes_moved=tot_bytes,
        arith_intensity=(tot_macs / tot_bytes) if tot_bytes else float("inf"),
        bound="mixed",
        ceiling_macs_per_s=peak_macs_per_s,   # informational
        ceiling_cycles=tot_ceiling_cycles,
    ))
    return records


def format_table(records: Sequence[Dict[str, Any]], limit: int = 20) -> str:
    if not records:
        return "(empty)"
    hdr = ["idx", "name", "K", "N", "macs", "intensity", "bound", "ceil_cyc"]
    rows = [hdr]
    for r in records[:limit]:
        rows.append([
            str(r["idx"]),
            str(r["name"])[:24],
            str(r["K"]),
            str(r["N"]),
            f"{r['macs']:.2e}",
            f"{r['arith_intensity']:.2f}",
            r["bound"],
            f"{r['ceiling_cycles']:.2e}",
        ])
    widths = [max(len(row[c]) for row in rows) for c in range(len(hdr))]
    out = []
    for row in rows:
        out.append("  ".join(row[c].ljust(widths[c]) for c in range(len(hdr))))
    if len(records) > limit:
        out.append(f"... ({len(records) - limit} more sites hidden)")
    return "\n".join(out)


def try_plot(records, out_path: str) -> bool:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return False
    site_recs = [r for r in records if r["name"] != "__WORKLOAD_TOTAL__"]
    if not site_recs:
        return False
    xs = [r["arith_intensity"] for r in site_recs]
    ys = [r["ceiling_macs_per_s"] / 1e9 for r in site_recs]
    plt.figure(figsize=(6, 4))
    plt.scatter(xs, ys, s=20)
    plt.xscale("log")
    plt.yscale("log")
    plt.xlabel("Arithmetic intensity (MAC / byte)")
    plt.ylabel("Ceiling throughput (GMAC/s)")
    plt.title("Roofline — per-site ceiling")
    plt.grid(True, which="both", alpha=0.3)
    plt.tight_layout()
    plt.savefig(out_path, dpi=100)
    plt.close()
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--arch", default="configs/arch/npu_core_16x16.yaml")
    ap.add_argument("--trace", required=True,
                    help="path to a workload trace JSON")
    ap.add_argument("--M", type=int, default=1)
    ap.add_argument("--clock-hz", type=float, default=CLOCK_HZ_DEFAULT)
    ap.add_argument("--out-plot", default=None,
                    help="if set and matplotlib is available, write PNG here")
    ap.add_argument("--limit", type=int, default=20)
    args = ap.parse_args()

    arch = _load_arch(args.arch)
    trace = _load_trace(args.trace)
    records = roofline(arch, trace, M=args.M, clock_hz=args.clock_hz)
    print(format_table(records, limit=args.limit))
    if args.out_plot:
        ok = try_plot(records, args.out_plot)
        print(f"plot written: {args.out_plot}" if ok
              else "(matplotlib not available or no sites — plot skipped)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
