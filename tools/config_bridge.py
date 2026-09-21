"""
config_bridge — one YAML pair (arch + ctb) into either iverilog -P
overrides or a Python CTBParams instance.

Usage:
    # emit iverilog args
    python3 tools/config_bridge.py --ctb configs/ctb/baseline.yaml --emit iverilog

    # emit Python source for manual inspection
    python3 tools/config_bridge.py --ctb configs/ctb/baseline.yaml --emit python

Programmatic:
    from tools.config_bridge import load_configs, to_ctb_params, to_iverilog_args
    arch, ctb = load_configs("configs/ctb/baseline.yaml")
    params    = to_ctb_params(arch, ctb, K=1024)
    argv      = to_iverilog_args(arch, ctb, K=1024)
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path
from typing import Any, Dict, List, Tuple

try:
    import yaml
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "pyyaml is required. Install with:  pip install pyyaml"
    ) from exc


REPO_ROOT = Path(__file__).resolve().parents[1]


def _read_yaml(path: Path) -> Dict[str, Any]:
    with path.open() as fh:
        return yaml.safe_load(fh)


def load_configs(ctb_path: str | os.PathLike) -> Tuple[Dict, Dict]:
    """Load a CTB YAML plus the arch YAML it references."""
    ctb_path = Path(ctb_path)
    if not ctb_path.is_absolute():
        ctb_path = (REPO_ROOT / ctb_path).resolve()
    ctb = _read_yaml(ctb_path)

    arch_ref = ctb.get("arch")
    if not arch_ref:
        raise ValueError(f"{ctb_path}: missing 'arch:' key")
    arch_path = Path(arch_ref)
    if not arch_path.is_absolute():
        arch_path = (REPO_ROOT / arch_path).resolve()
    arch = _read_yaml(arch_path)
    return arch, ctb


def _canonical(arch: Dict, ctb: Dict, K: int | None) -> Dict[str, Any]:
    """Pack the shared parameter surface into one dict."""
    g   = arch["geometry"]
    d   = arch["datapath"]
    s   = arch["storage"]
    dsp = ctb.get("dispatch", {})
    stg = ctb.get("storage", {})
    return dict(
        ARRAY_M=int(g["array_m"]),
        ARRAY_N=int(g["array_n"]),
        ACT_WIDTH=int(d["act_width_bits"]),
        ACC_WIDTH=int(d["acc_width_bits"]),
        MAX_K=int(s["act_buffer"]["capacity_words"]),
        K_TILE=int(dsp.get("k_tile", 16)),
        DECOUPLE_MAX=int(dsp.get("decouple_max", 3)),
        DRAIN_ISSUE_ON_RETURN=int(dsp.get("drain_issue_on_return", 1)),
        OUT_FIFO_DEPTH=int(stg.get("out_fifo_depth", 32)),
        ACT_FIFO_DEPTH=int(stg.get("act_fifo_depth", 16)),
        SWAIT_EARLY_EXIT=int(ctb.get("swait_early_exit", 1)),
        SWAIT_EARLY_THRESHOLD=int(ctb.get("swait_early_threshold", 11)),
        K_UNDER_TEST=int(K if K is not None else ctb.get("k_under_test", 1024)),
    )


def to_iverilog_args(arch: Dict, ctb: Dict, K: int | None = None,
                     top: str = "ctb_regime_tb") -> List[str]:
    """List of ['-Ptop.PARAM=value', ...] arguments for iverilog."""
    cn = _canonical(arch, ctb, K)
    return [f"-P{top}.{k}={v}" for k, v in cn.items()]


def to_ctb_params(arch: Dict, ctb: Dict, K: int | None = None):
    """Return a simulator.ctb_mech.CTBParams instance."""
    # Local import so `--emit iverilog` doesn't require importing the sim.
    sys.path.insert(0, str(REPO_ROOT))
    from simulator.ctb_mech.ctb_mech import CTBParams  # type: ignore
    cn = _canonical(arch, ctb, K)
    # drain_ii is not directly a YAML knob today: DRAIN_ISSUE_ON_RETURN=1
    # means Y2 with drain_ii=2; DRAIN_ISSUE_ON_RETURN=0 uses drain_ii=3
    # (the classic policy; empirically slower per fire).
    drain_ii = 2 if cn["DRAIN_ISSUE_ON_RETURN"] else 3
    return CTBParams(
        K=cn["K_UNDER_TEST"],
        K_TILE=cn["K_TILE"],
        ARRAY_M=cn["ARRAY_M"],
        ARRAY_N=cn["ARRAY_N"],
        D=cn["DECOUPLE_MAX"],
        drain_ii=drain_ii,
        CAP=cn["OUT_FIFO_DEPTH"],
    )


def _emit_python(cn: Dict[str, Any]) -> str:
    body = ",\n    ".join(f"{k}={v!r}" for k, v in cn.items())
    return f"CTBParams(\n    {body},\n)"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ctb", default="configs/ctb/baseline.yaml",
                    help="path to a configs/ctb/*.yaml file")
    ap.add_argument("--K", type=int, default=None,
                    help="override K_UNDER_TEST for this invocation")
    ap.add_argument("--emit", choices=("iverilog", "python", "json"),
                    default="iverilog")
    ap.add_argument("--top", default="ctb_regime_tb",
                    help="testbench top module name (for iverilog -P prefix)")
    args = ap.parse_args()

    arch, ctb = load_configs(args.ctb)
    cn = _canonical(arch, ctb, args.K)

    if args.emit == "iverilog":
        print(" ".join(f"-P{args.top}.{k}={v}" for k, v in cn.items()))
    elif args.emit == "python":
        print(_emit_python(cn))
    else:
        import json
        print(json.dumps(cn, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
