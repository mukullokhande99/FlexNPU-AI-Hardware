"""Smoke tests for the v0.1 tree. Kept deliberately tiny."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))


def test_configs_load():
    from tools.config_bridge import load_configs
    arch, ctb = load_configs("configs/ctb/baseline.yaml")
    assert arch["config_name"] == "npu_core_16x16"
    assert ctb["dispatch"]["decouple_max"] == 3


def test_ctb_params_construction():
    from tools.config_bridge import load_configs, to_ctb_params
    arch, ctb = load_configs("configs/ctb/baseline.yaml")
    p = to_ctb_params(arch, ctb, K=1024)
    assert p.K == 1024
    assert p.D == 3
    assert p.CAP == 32
    assert p.ARRAY_M == 16 and p.ARRAY_N == 16


def test_iverilog_args_shape():
    from tools.config_bridge import load_configs, to_iverilog_args
    arch, ctb = load_configs("configs/ctb/baseline.yaml")
    args = to_iverilog_args(arch, ctb, K=1024)
    joined = " ".join(args)
    assert "ARRAY_M=16" in joined
    assert "DECOUPLE_MAX=3" in joined
    assert "K_UNDER_TEST=1024" in joined


def test_roofline_runs_on_synthetic():
    from analysis.roofline import roofline, _load_arch
    from workloads.synthetic.kchain_bank import kchain
    arch = _load_arch("configs/arch/npu_core_16x16.yaml")
    sites = kchain(M=1, N=16, K_values=(64, 128, 256))
    records = roofline(arch, sites)
    assert len(records) == 4  # 3 sites + 1 summary
    assert records[-1]["name"] == "__WORKLOAD_TOTAL__"
    assert records[-1]["macs"] > 0


def test_ctb_mech_runs_at_default_config():
    from tools.config_bridge import load_configs, to_ctb_params
    from simulator.ctb_mech.ctb_mech import CTBMechanistic
    arch, ctb = load_configs("configs/ctb/baseline.yaml")
    r = CTBMechanistic(to_ctb_params(arch, ctb, K=1024)).run()
    assert r.service_cycles > 0
    assert r.regime in {"dormant", "producer-limited", "drain-limited",
                        "credit-limited", "storage-unsafe", "credit-limited-drain-limited"}


def test_workload_traces_parse():
    for path in ("workloads/mobilebert/mb_flex_sites.json",
                 "workloads/mobilenetv2/mnv2_flex_sites.json"):
        data = json.loads((REPO / path).read_text())
        assert isinstance(data, list) and len(data) > 0
        for rec in data:
            assert "K" in rec and "N" in rec and "N_padded" in rec
            assert rec["N_padded"] >= rec["N"]
