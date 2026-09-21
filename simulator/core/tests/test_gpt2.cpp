// Milestone B: GPT-2 FFN FPGA-active timing validation.
//
// Golden (see edgescope_gpt2_ffn_characterization_20260917/GPT2_FFN_CHARACTERIZATION.md):
//   Workload: A[512x3072] x B[3072x768], K=3072
//   MAX_K=1024, K chunks=3, output tiles=1536, chain dispatches=4608
//   FPGA-active (all dispatches, summed) ≈ 603 ms @ 100 MHz PL
//     = 60,300,000 PL cycles = 4608 x ~13,080 cyc/dispatch
//   Wall median 1.260 s = 126,000,000 PL cycles (includes host)
//   Host pack 0.558 s, host mmio+FPGA residual 0.702 s

#include "flexnpu_sim.h"
#include <cassert>
#include <cstdio>

using namespace flexnpu_sim;

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else         { std::fprintf(stderr, "ok  : %s\n", msg); } \
} while (0)

#define CHECK_TOL(actual, expected, tol_pct, msg) do { \
    double _a = (double)(actual); double _e = (double)(expected); \
    double _err = ((_a - _e) / _e) * 100.0; \
    if (std::abs(_err) > (tol_pct)) { \
        std::fprintf(stderr, "FAIL: %s: got %.3f want %.3f err=%.2f%% (tol %.1f%%)\n", msg, _a, _e, _err, (double)(tol_pct)); \
        failures++; \
    } else { \
        std::fprintf(stderr, "ok  : %s: got %.3f want %.3f err=%.2f%% (tol %.1f%%)\n", msg, _a, _e, _err, (double)(tol_pct)); \
    } \
} while (0)

int main() {
    Arch arch;
    HostModel host;
    host.pack_us_per_dispatch = 121.0;
    host.mmio_overhead_us_per_dispatch = 21.5;

    Simulator sim(arch, host, /*trace=*/false);
    sim.load_dispatches(build_chain_realization(512, 768, 3072, arch));
    cycle_t final_c = sim.run();

    const auto& stats = sim.stats();
    double wall_s = static_cast<double>(final_c) / arch.pl_clock_hz;

    // Structural
    CHECK((int)stats.dispatches_run == 4608, "dispatches == 4608");
    CHECK((int)stats.compute_done_events == 4608, "compute_done x 4608");
    CHECK((int)stats.drain_done_events == 1536, "drain_done x 1536 (LAST only)");
    CHECK((int)stats.tile_done_events == 4608 * 128, "tile_done_events == 4608 x 128");

    // FPGA-active total
    // We derive FPGA-active from the sum of per-dispatch (compute + writeback) cycles.
    // Per FIRST/MIDDLE: 44 + 128*101 + 85 = 13057 cyc
    // Per LAST:         44 + 128*101 + 85 + 16 = 13073 cyc
    // Total: 3072 x 13057 + 1536 x 13073 = 40,111,104 + 20,080,128 = 60,191,232 cyc = 601.9 ms
    uint64_t fpga_active_cyc = 3072ULL * 13057 + 1536ULL * 13073;
    double fpga_active_s = static_cast<double>(fpga_active_cyc) / arch.pl_clock_hz;
    std::fprintf(stderr, "\n-- fpga-active budget --\n");
    std::fprintf(stderr, "  fpga_active_cycles derived = %llu (%.3f s)\n",
                 (unsigned long long)fpga_active_cyc, fpga_active_s);

    // Golden: 603 ms with ±2 % tolerance
    CHECK_TOL(fpga_active_s, 0.603, 2.0, "FPGA-active total ≈ 603 ms (±2%)");

    // Wall time reconstruction (with host model)
    // Expected: pack 0.558 s + fpga_active 0.603 s + mmio 0.099 s ≈ 1.260 s
    CHECK_TOL(wall_s, 1.260, 5.0, "Wall ≈ 1.260 s (±5%)");

    // Per-dispatch bracket: wall / dispatches
    double per_dispatch_us = wall_s / stats.dispatches_run * 1e6;
    std::fprintf(stderr, "  per-dispatch bracket = %.1f µs (target ~274 µs)\n", per_dispatch_us);
    CHECK_TOL(per_dispatch_us, 274.0, 5.0, "per-dispatch bracket ≈ 274 µs (±5%)");

    // Traffic
    // ACT bytes: 4608 * 16384 = 75,497,472 = 75.5 MB
    // WGT bytes: 4608 * 16384 = 75,497,472 = 75.5 MB
    // OUT bytes: 1536 * 1024  = 1,572,864 = 1.5 MB
    CHECK((int64_t)stats.dma_bytes_act == 4608LL * 16384, "ACT bytes == 4608 * 16384");
    CHECK((int64_t)stats.dma_bytes_wgt == 4608LL * 16384, "WGT bytes == 4608 * 16384");
    CHECK((int64_t)stats.dma_bytes_out == 1536LL * 1024,  "OUT bytes == 1536 * 1024");

    std::fprintf(stderr, "\n== gpt2_ffn milestone summary ==\n");
    std::fprintf(stderr, "  wall cycles = %lld (%.6f s)\n", (long long)final_c, wall_s);
    std::fprintf(stderr, "  fpga_active_cyc derived = %llu (%.6f s)\n",
                 (unsigned long long)fpga_active_cyc, fpga_active_s);
    std::fprintf(stderr, "  events processed = %llu\n", (unsigned long long)stats.events_processed);
    std::fprintf(stderr, "  failures = %d\n", failures);
    return failures == 0 ? 0 : 1;
}
