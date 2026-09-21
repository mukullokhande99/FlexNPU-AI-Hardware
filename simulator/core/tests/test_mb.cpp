// Milestone C: MobileBERT contrast.
//
// Purpose: demonstrate the simulator can model persist=1 hit path (K ≤ MAX_K,
// non-chain, WGT bank retained) — NOT hardcoded to chain semantics.
//
// Existing counters (mb_full_reference.log, tile16_generic vmfb):
//   Total dispatches: 198,912
//   Total NPU time: 25.04 s → 126 µs/dispatch median
//   host_A_B_pack:   1.74 s → 8.7 µs/dispatch
//   tile_pack+sync+arm: 18.83 s → 94.6 µs/dispatch (dominated by usleep=15.1s)
//   DMA_MM2S:        0.17 s → 0.85 µs/dispatch (negligible; persist reuse)
//   compute_wait:    2.13 s → 10.7 µs/dispatch  (32-dummy fence + pure_hw_wait)
//   readback:        1.68 s → 8.4  µs/dispatch  (sync_OUT + memcpy)
//
// We simulate a representative MB-style batch (small K, persist=1 hit) to
// confirm the sim's persist-hit path produces the expected structure.

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
    // MB has usleep(76 µs) per dispatch baked into tile_pack+sync+arm bucket
    // + host pack 8.7 µs + other ~10 µs = ~95 µs of host overhead per dispatch.
    host.pack_us_per_dispatch = 8.7;
    host.mmio_overhead_us_per_dispatch = 95.0;

    int N = 1000;             // subset (full 198,912 too long for a unit test)
    int K_per_dispatch = 384; // typical MB bert-tile K
    double hit_rate = 0.70;   // conservative Fix-B hit rate

    Simulator sim(arch, host, /*trace=*/false);
    sim.load_dispatches(build_mb_style(N, K_per_dispatch, hit_rate));
    cycle_t final_c = sim.run();

    const auto& stats = sim.stats();
    double wall_s = static_cast<double>(final_c) / arch.pl_clock_hz;
    double per_disp_us = wall_s / N * 1e6;

    std::fprintf(stderr, "\n-- MB-style simulation --\n");
    std::fprintf(stderr, "  N=%d K_per_dispatch=%d hit_rate=%.2f\n", N, K_per_dispatch, hit_rate);
    std::fprintf(stderr, "  wall = %.6f s\n", wall_s);
    std::fprintf(stderr, "  per-dispatch = %.1f µs\n", per_disp_us);
    std::fprintf(stderr, "  persist hits/misses = %llu / %llu (%.1f%% hit)\n",
                 (unsigned long long)stats.persist_hits,
                 (unsigned long long)stats.persist_misses,
                 100.0 * stats.persist_hits / (stats.persist_hits + stats.persist_misses));
    std::fprintf(stderr, "  DMA bytes ACT/WGT/OUT = %llu / %llu / %llu\n",
                 (unsigned long long)stats.dma_bytes_act,
                 (unsigned long long)stats.dma_bytes_wgt,
                 (unsigned long long)stats.dma_bytes_out);

    // Structural: persist hits should skip WGT DMA
    // For hits: WGT bytes contributed = 0 (per hit). For misses: full WGT stream.
    uint64_t expected_hits = stats.persist_hits;
    uint64_t expected_misses = stats.persist_misses;
    uint64_t expected_act_bytes = (uint64_t)N * K_per_dispatch * arch.AXIS_BEAT_BYTES;
    uint64_t expected_wgt_bytes = expected_misses * K_per_dispatch * arch.AXIS_BEAT_BYTES;
    (void)expected_hits;
    CHECK(stats.dma_bytes_act == expected_act_bytes, "MB: ACT bytes = N * K * beat_bytes (all dispatches)");
    CHECK(stats.dma_bytes_wgt == expected_wgt_bytes, "MB: WGT bytes = misses * K * beat_bytes (persist hits skip WGT DMA)");

    // Target per-dispatch NPU time: 126 µs. With our host_model of ~104 µs host
    // + FPGA per-dispatch (K=384: setup 44 + 48*101 + 85 = 4977 cyc = 49.77 µs
    // + writeback 16 = 4993 cyc = 49.93 µs FPGA-active per dispatch on LAST-only path)
    // Total per-dispatch: ~104 + ~50 = ~154 µs.
    //
    // But: MB's real 126 µs/dispatch already includes all host+FPGA overhead
    // in the flat 25 s / 198,912 amortized average. Our sub-simulation with
    // K=384 gets a similar order-of-magnitude result, not an exact match, since
    // MB's actual per-dispatch K varies. This test is illustrative, not a
    // strict apples-to-apples reproduction.
    CHECK(per_disp_us > 100 && per_disp_us < 250, "MB per-dispatch in 100-250 µs range (order-of-magnitude sanity)");

    // The critical structural claim: persist hits reduce WGT DMA proportionally
    double persist_hit_rate_actual = static_cast<double>(stats.persist_hits) /
                                     (stats.persist_hits + stats.persist_misses);
    CHECK_TOL(persist_hit_rate_actual, hit_rate, 5.0, "persist hit-rate matches config within 5%");

    std::fprintf(stderr, "\n== MB milestone summary ==\n");
    std::fprintf(stderr, "  failures = %d\n", failures);
    return failures == 0 ? 0 : 1;
}
