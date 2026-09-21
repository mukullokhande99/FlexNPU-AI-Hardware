"""
03_rtl_vs_model_parity — side-by-side comparison of ctb_mech vs iverilog.

At one (K, D, CAP, drain_ii) point (via configs/ctb/baseline.yaml), this
runs both:
  * ctb_mech.CTBMechanistic (Python)
  * ctb_regime_tb.sv  (RTL, compiled with iverilog using overrides from
                       tools/config_bridge.py)

and prints a small parity table. If the two disagree by >5% on any
observable, the script highlights that row.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from tools.config_bridge import load_configs, to_ctb_params, to_iverilog_args  # noqa: E402
from simulator.ctb_mech.ctb_mech import CTBMechanistic                          # noqa: E402


def run_rtl(K: int, config_path: str) -> dict:
    arch, ctb = load_configs(config_path)
    args = to_iverilog_args(arch, ctb, K=K)
    if shutil.which("iverilog") is None:
        raise RuntimeError("iverilog not on PATH. Install with: apt install iverilog")
    if shutil.which("vvp") is None:
        raise RuntimeError("vvp not on PATH. Install with: apt install iverilog")

    rtl = [str(REPO / f) for f in (
        "rtl/compute_tile_bram.sv",
        "rtl/mm.sv",
        "rtl/systolic.sv",
        "rtl/mac.sv",
        "rtl/fifo.sv",
    )]
    tb = str(REPO / "tb/ctb_regime_tb.sv")

    with tempfile.TemporaryDirectory() as td:
        vvp = str(Path(td) / "regime.vvp")
        cmd = ["iverilog", "-g2012", *args, "-o", vvp, tb, *rtl]
        subprocess.run(cmd, check=True)
        out = subprocess.check_output(["vvp", vvp], text=True)
    m = re.search(r"REGIME_JSON\s+(\{.*\})", out)
    if not m:
        raise RuntimeError(f"RTL run produced no REGIME_JSON line:\n{out}")
    return json.loads(m.group(1))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--config", default="configs/ctb/baseline.yaml")
    ap.add_argument("--K", type=int, default=1024)
    args = ap.parse_args()

    arch, ctb = load_configs(args.config)
    p = to_ctb_params(arch, ctb, K=args.K)
    sim_r = CTBMechanistic(p).run()

    rtl = run_rtl(args.K, args.config)

    rows = [
        ("service_cycles",   sim_r.service_cycles,   rtl["service"]),
        ("exec_cycles",      sim_r.exec_cycles,      rtl["exec"]),
        ("wait_cycles",      sim_r.wait_cycles,      rtl["wait"]),
        ("next_cycles",      sim_r.next_cycles,      rtl["next"]),
        ("credit_stall_cyc", sim_r.credit_stall_cyc, rtl["credit_stall_cyc"]),
        ("rp_peak",          sim_r.rp_peak,          rtl["rp_peak"]),
        ("fifo_peak_all",    sim_r.fifo_peak_all,    rtl["fifo_peak_all"]),
    ]
    hdr = f"{'observable':<20} {'sim':>10} {'rtl':>10} {'delta%':>8}"
    print(hdr); print("-" * len(hdr))
    worst = 0.0
    for name, sim, rtl_v in rows:
        if rtl_v == 0 and sim == 0:
            delta = 0.0
        elif rtl_v == 0:
            delta = float("inf")
        else:
            delta = 100.0 * (sim - rtl_v) / rtl_v
        flag = "  <<<" if abs(delta) > 5.0 and rtl_v != 0 else ""
        print(f"{name:<20} {sim:>10} {rtl_v:>10} {delta:>7.2f}{flag}")
        if rtl_v != 0:
            worst = max(worst, abs(delta))
    print()
    print(f"regime (sim): {sim_r.regime}")
    print(f"overflow_hazard (rtl): {rtl['overflow_hazard']}")
    print(f"worst |delta%| across observables: {worst:.2f}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
