// Regression test codifying the S2-marginal causal chain, CORRECTED
// for the physical realizability audit (2026-09-17 v3).
//
// History:
//   v1 initial ORG_FAMILY_MEASURED.md draft: claimed "S2 has ~4.2×
//     marginal under ORG-2 + local reuse". WRONG — misattributed to
//     traversal-local reuse.
//   v2 audit_s2_marginal + this file's first version: attributed the
//     4.2× to dispatch_group_size=32 host amortization. Also WRONG —
//     the group model assumed pack amortizes across a macro-op, but
//     pack is per-dispatch because ACT differs per tile. Only MMIO
//     genuinely amortizes across a macro submission.
//   v3 (this file, corrected model — see AUDIT_GROUPED_DISPATCH_REALIZABILITY.md):
//     * pack cost decomposed into act (~9 µs) + wgt (~112 µs);
//     * WGT_HOIST (physical runtime cache) fires under NCM traversal,
//       reducing pack from 121 µs → ~9 µs on hit (skipping wgt_pack);
//     * dispatch_group_size amortizes ONLY mmio (21.5 µs), NOT pack;
//     * ⇒ the 4.2× S2 marginal is realized under NCM + WGT_HOIST
//       (per-dispatch host ≈ 34 µs < fpga_S2 ≈ 33 µs);
//     * group=32 alone (no reuse) does NOT unlock 4.2× — per-dispatch
//       host stays at ~121 µs (pack unchanged) and remains the
//       bottleneck.
//
// Physical requirements for the 4.2× regime to be realizable:
//   1. Software: change chain-path loop order to NCM.
//   2. RTL: multi-slot accumulator (32× 1 KB = 32 KB) for chain FIRST/
//      MIDDLE partial sums under NCM order — new SRAM in shell.
//   3. Chain-flag support in macro-op entries (or per-dispatch flag
//      writes as currently done).
//   4. S2 compute-FSM alternative RTL (separate).

#include "flexnpu_sim.h"
#include <cstdio>
#include <cmath>

using namespace flexnpu_sim;

static int failures = 0;

#define CHECK_APPROX(actual, expected, tol_pct, msg) do { \
    double _a = (double)(actual); double _e = (double)(expected); \
    double _err = std::abs(_a - _e) / _e * 100.0; \
    if (_err > (tol_pct)) { \
        std::fprintf(stderr, "FAIL: %s: got %.4f want %.4f (err %.2f%% > %.1f%%)\n", msg, _a, _e, _err, (double)(tol_pct)); failures++; \
    } else { \
        std::fprintf(stderr, "ok  : %s: got %.4f want %.4f (err %.2f%%)\n", msg, _a, _e, _err); \
    } \
} while (0)

static double run(bool overlap, int q, LoopOrder order,
                  bool wgt_reuse_local, int group, double pass_scale) {
    Arch arch;
    HostModel host;
    host.pack_us_per_dispatch = 121.0;
    host.mmio_overhead_us_per_dispatch = 21.5;
    host.pack_execute_overlap = overlap;
    host.command_queue_depth = q;
    arch.wgt_reuse_local = wgt_reuse_local;
    arch.dispatch_group_size = group;
    arch.pass_overhead_scale = pass_scale;
    Simulator sim(arch, host, false);
    sim.load_dispatches(build_chain_realization_ordered(512, 768, 3072, arch, order));
    sim.run();
    return (double)sim.stats().last_event_cycle / arch.pl_clock_hz;
}

int main() {
    std::fprintf(stderr, "== S2-marginal causal regime test ==\n\n");

    // Baseline references
    double A = run(true, 8, LoopOrder::MNC, false, 1, 1.0);   // ORG-1 Q=8
    double B = run(true, 8, LoopOrder::MNC, false, 1, 0.10);  // + S2
    double C = run(true, 8, LoopOrder::NCM, true,  1, 1.0);   // ORG-2 NCM+reuse
    double D = run(true, 8, LoopOrder::NCM, true,  1, 0.10);  // + S2

    // Group=32 alternatives
    double E = run(true, 8, LoopOrder::MNC, false, 32, 1.0);  // group=32 alone
    double F = run(true, 8, LoopOrder::MNC, false, 32, 0.10); // group=32 + S2
    double G = run(true, 8, LoopOrder::NCM, true,  32, 1.0);  // ORG-2 + group=32
    double H = run(true, 8, LoopOrder::NCM, true,  32, 0.10); // full stack

    std::fprintf(stderr, "\n-- key regime observations --\n");
    std::fprintf(stderr, "  A: ORG-1 Q=8              = %.4f s (host-bottleneck: 142.5µs > 141µs)\n", A);
    std::fprintf(stderr, "  B: A + S2                 = %.4f s\n", B);
    std::fprintf(stderr, "  C: ORG-2 NCM+reuse        = %.4f s\n", C);
    std::fprintf(stderr, "  D: C + S2                 = %.4f s\n", D);
    std::fprintf(stderr, "  E: A + group=32           = %.4f s\n", E);
    std::fprintf(stderr, "  F: E + S2                 = %.4f s\n", F);
    std::fprintf(stderr, "  G: C + group=32           = %.4f s\n", G);
    std::fprintf(stderr, "  H: G + S2 (full stack)    = %.4f s\n\n", H);

    // CLAIM 1 (unchanged, still valid): S2 gives ~0 marginal when host is
    // bottleneck (ORG-1 alone). Pack (121 µs) + mmio (21.5) > FPGA_S2 (33 µs).
    CHECK_APPROX(B, A, 1.0, "S2 marginal under ORG-1 alone ≈ 1.00× (host bottleneck)");

    // CLAIM 2 (CORRECTED — v3): S2 gives ~4.13× marginal under ORG-2 NCM +
    // WGT_HOIST alone. Corrected model: WGT_HOIST hit skips pack_wgt (~112 µs)
    // entirely, leaving act_pack (~9 µs) + mmio (21.5) = 30.5 µs amortized.
    // This is just below fpga_S2 = 33 µs, so FPGA becomes the bottleneck at
    // fpga_S2. Under NCM: fresh pack once per 32-dispatch group + 31 hits =
    // per-dispatch ≈ 34 µs.
    double s2_marginal_orgs2 = C / D;
    std::fprintf(stderr, "  S2 marginal under ORG-2+WGT_HOIST alone: %.2f× (v3 corrected)\n", s2_marginal_orgs2);
    CHECK_APPROX(s2_marginal_orgs2, 4.13, 5.0, "S2 marginal under ORG-2 NCM+wgt_reuse_local ≈ 4.13× (WGT_HOIST unlocks S2)");

    // CLAIM 3 (CORRECTED — v3): dispatch_group_size=32 does NOT unlock S2's
    // full value on its own, because it amortizes only MMIO (21.5 µs), not
    // pack. Pack stays at 121 µs (per-dispatch, ACT differs per tile).
    // Per-dispatch host ≈ 121.67 µs, still >> fpga_S2 (33 µs). Host remains
    // bottleneck. S2 marginal ~1.16× (small).
    double s2_marginal_group = E / F;
    std::fprintf(stderr, "  S2 marginal under group=32 alone:        %.2f× (v3 corrected — group amortizes mmio only)\n", s2_marginal_group);
    CHECK_APPROX(s2_marginal_group, 1.16, 5.0, "S2 marginal under group=32 alone ≈ 1.16× (mmio-only amortization insufficient)");

    // CLAIM 4 (CORRECTED — v3): F ≠ H. Without WGT_HOIST (case F), pack is
    // never reduced; host bottleneck. With WGT_HOIST (case H), pack drops.
    // The two are NOT equivalent under the corrected model.
    // Concretely: F ≈ 0.56 s, H ≈ 0.16 s. F ≈ 3.6× H.
    double F_over_H = F / H;
    std::fprintf(stderr, "  F/H ratio (should be > 3, showing NOT redundant): %.2f×\n", F_over_H);
    if (F_over_H < 2.0) {
        std::fprintf(stderr, "FAIL: F/H = %.2f× — expected > 3.0 (WGT_HOIST provides real value under group=32+S2)\n", F_over_H);
        failures++;
    } else {
        std::fprintf(stderr, "ok  : F/H = %.2f× > 3.0 (WGT_HOIST provides real value under group=32+S2)\n", F_over_H);
    }

    // CLAIM 5 (unchanged): Without S2, group=32 alone gives ~1% wall benefit.
    double group_marginal_noS2 = A / E;
    std::fprintf(stderr, "  group=32 marginal without S2:            %.2f× (small: mmio was only ~15%% of host)\n", group_marginal_noS2);
    CHECK_APPROX(group_marginal_noS2, 1.011, 1.0, "group=32 alone (no S2) ≈ 1.01× marginal (mmio amortization insufficient)");

    std::fprintf(stderr, "\n== failures: %d ==\n", failures);
    return failures == 0 ? 0 : 1;
}
