"""Regression tests for the CTB mechanistic model.

Each test asserts REGIME agreement, not exact cycle counts. Cycle
counts are asserted with slack because the model's small-K overhead
and drain-tail approximations are deliberately imperfect (see
DESIGN_NOTE.md §"Approximations flagged for later diagnosis").

Run:
    python3 -m pytest simulator/ctb_mech/test_regimes.py -v
    (or)
    python3 simulator/ctb_mech/test_regimes.py
"""

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

from ctb_mech import CTBMechanistic, CTBParams


def _run(K: int, D: int) -> "CTBResult":
    return CTBMechanistic(CTBParams(K=K, D=D)).run()


# ---------------------------------------------------------------------------
# 1. Dormant regime: workload too short for D to matter.
# ---------------------------------------------------------------------------

def test_dormant_regime_k32():
    """K=32 (2 passes): D=2, 3, 4 must give identical service and be classified dormant."""
    r_d2 = _run(K=32, D=2)
    r_d3 = _run(K=32, D=3)
    r_d4 = _run(K=32, D=4)
    assert r_d2.regime == "dormant"
    assert r_d3.regime == "dormant"
    assert r_d4.regime == "dormant"
    assert r_d2.service_cycles == r_d3.service_cycles == r_d4.service_cycles
    assert r_d2.credit_stall_cyc == 0


# ---------------------------------------------------------------------------
# 2. Credit-limited regime: D below the natural r_pending peak.
# ---------------------------------------------------------------------------

def test_credit_limited_k1024_d2():
    """K=1024, D=2: producer paced by drain pair-oscillation."""
    r = _run(K=1024, D=2)
    assert r.regime == "credit-limited"
    assert r.credit_stall_cyc > 500
    assert r.rp_peak == 2                 # clamped at D
    # Per-pass 46 ± 2 cyc → service in [2900, 3200]
    per_pass = r.service_cycles / r.num_passes
    assert 45 <= per_pass <= 50, f"per-pass {per_pass}"


def test_credit_limited_k1024_d1():
    """K=1024, D=1: strictly serialized pipeline; MUST classify credit-limited.

    Previously misclassified as drain-limited because the drain_end
    spacing (which the classifier had used as T_d_measured) is inflated
    to the credit-serialized 95 cyc/pass, defeating the drain-throttle
    check. The corrected classifier compares per-pass cadence to the
    intrinsic no-credit bottleneck max(T_p_native, drain_ii*ARRAY_N).
    """
    r = _run(K=1024, D=1)
    assert r.regime == "credit-limited", (
        f"expected credit-limited, got {r.regime} "
        f"(service={r.service_cycles}, stall={r.credit_stall_cyc}, "
        f"rp_peak={r.rp_peak})")
    # Overwhelming stall: nearly two-thirds of service is credit stall.
    assert r.credit_stall_cyc > 0.5 * r.service_cycles
    assert r.rp_peak == 1
    # Per-pass 95 cyc for the strictly-serialized pipeline.
    per_pass = r.service_cycles / r.num_passes
    assert 90 <= per_pass <= 100, f"per-pass {per_pass}"


def test_credit_limited_dominates_drain_when_d_small():
    """Sanity: at D=1 the observed cadence must exceed the intrinsic
    no-credit bottleneck by enough that the classifier calls it
    credit-limited, not drain-limited via credit-throttling."""
    for K in (256, 512, 1024):
        r = _run(K=K, D=1)
        assert r.regime == "credit-limited", (
            f"K={K} D=1: got {r.regime}")


# ---------------------------------------------------------------------------
# 3. Drain-limited regime: D >= D_min = ceil(L/T_p) = 3.
# ---------------------------------------------------------------------------

def test_drain_limited_k1024_d3():
    """K=1024, D=3: drain-limited, tiny credit stall, per-pass ~35 cyc."""
    r = _run(K=1024, D=3)
    assert r.regime == "drain-limited"
    assert r.credit_stall_cyc <= 0.05 * r.service_cycles
    assert r.rp_peak == 3
    assert not r.overflow
    per_pass = r.service_cycles / r.num_passes
    assert 34 <= per_pass <= 37, f"per-pass {per_pass}"


# ---------------------------------------------------------------------------
# 4. Storage-unsafe regime: D >= D_min AND workload long enough to overflow.
# ---------------------------------------------------------------------------

def test_storage_unsafe_k1024_d4():
    """K=1024, D=4: producer unstalled → per-row FIFO grows past CAP=32."""
    r = _run(K=1024, D=4)
    assert r.regime == "storage-unsafe"
    assert r.overflow
    assert r.fifo_peak_all > 32
    assert r.overflow_row in range(16)
    # RTL observation: this deadlocks. Model reports overflow midway.


def test_storage_unsafe_boundary_k608_safe():
    """Held-out point K=608, D=4: last safe before overflow onset."""
    r = _run(K=608, D=4)
    assert not r.overflow
    assert r.fifo_peak_all == 32          # exactly at CAP
    assert r.rp_peak == 4                 # r_pending briefly hits D at this N


def test_storage_unsafe_boundary_k624_overflow():
    """Held-out point K=624, D=4: first overflow past K=608."""
    r = _run(K=624, D=4)
    assert r.overflow
    assert r.fifo_peak_all > 32


# ---------------------------------------------------------------------------
# 5. Behavioral equivalence: D beyond D_min gives no throughput improvement
#    (until it overflows).
# ---------------------------------------------------------------------------

def test_dormant_d_beyond_dmin_at_k512():
    """K=512, D=3 vs D=4 must give same service (D=4 still safe)."""
    r3 = _run(K=512, D=3)
    r4 = _run(K=512, D=4)
    assert r3.regime == "drain-limited"
    assert r4.regime == "drain-limited"
    assert r3.service_cycles == r4.service_cycles


# ---------------------------------------------------------------------------
# 6. Derived L matches audit-measured L=71 (± 2).
# ---------------------------------------------------------------------------

def test_L_first_matches_audit():
    """First-pass pass_end -> same-pass drain_end latency."""
    for K, D in [(256, 3), (512, 3), (1024, 3), (128, 4)]:
        r = _run(K=K, D=D)
        assert 65 <= r.L_first <= 75, (
            f"K={K} D={D}: L_first={r.L_first} outside [65,75]")


# ---------------------------------------------------------------------------
# Simple test runner if executed directly (no pytest).
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import traceback
    tests = [t for name, t in list(globals().items()) if name.startswith("test_")]
    passed = failed = 0
    for t in tests:
        try:
            t()
            print(f"PASS  {t.__name__}")
            passed += 1
        except Exception as e:
            print(f"FAIL  {t.__name__}: {e}")
            traceback.print_exc()
            failed += 1
    print(f"\n{passed}/{passed+failed} passed")
    sys.exit(0 if failed == 0 else 1)
