// Milestone A: STAGE B two-chunk chain — event-level RTL trace validation.
//
// Golden values from STAGE B RTL trace (see
// docs/simulator_frontend/edgescope_chain_correctness_20260916/
// CHAIN_CORRECTNESS_v6_op_fsm_stage2.md):
//
//   Chunk 0 (FIRST):     13,063 cycles from TLAST to op_done
//   Chunk 1 (LAST):      13,083 cycles from TLAST to op_done
//   Tile_done_pulses:    128 per chunk
//   Bank swaps wr:       2 total (one per chunk)
//   Bank swaps rd:       2 total (one per chunk, after 128 pulses each)
//
// Tolerance: ±0 required for the two chunk cycle totals.

#include "flexnpu_sim.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>

using namespace flexnpu_sim;

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        failures++; \
    } else { \
        std::fprintf(stderr, "ok  : %s\n", msg); \
    } \
} while (0)

#define CHECK_EQ(actual, expected, msg) do { \
    auto _a = (actual); auto _e = (expected); \
    if (_a != _e) { \
        std::fprintf(stderr, "FAIL: %s: got %lld want %lld (%s:%d)\n", msg, (long long)_a, (long long)_e, __FILE__, __LINE__); \
        failures++; \
    } else { \
        std::fprintf(stderr, "ok  : %s = %lld\n", msg, (long long)_a); \
    } \
} while (0)

int main() {
    Arch arch;
    HostModel host{0.0, 0.0};   // STAGE B is host-free — pure FPGA event trace
    Simulator sim(arch, host, /*trace=*/true);
    sim.load_dispatches(build_stage_b_two_chunk(1024, &arch));
    cycle_t final_c = sim.run();

    const auto& stats = sim.stats();

    // Structural counts
    CHECK_EQ(stats.dispatches_run, 2u, "dispatches_run == 2");
    CHECK_EQ(stats.tile_done_events, 256u, "tile_done_events == 256 (128 x 2 chunks)");
    CHECK_EQ(stats.bank_swaps_wr, 4u, "bank_swaps_wr == 4 (2 chunks x 2 sides ACT/WGT)");
    CHECK_EQ(stats.bank_swaps_rd, 4u, "bank_swaps_rd == 4");
    CHECK_EQ(stats.compute_done_events, 2u, "compute_done x 2");
    CHECK_EQ(stats.drain_done_events, 1u, "drain_done x 1 (LAST only)");

    // Locate per-chunk key events in trace.
    // Chunk 0 dispatch_id=0, chunk 1 dispatch_id=1.
    cycle_t tlast_c0 = -1, opdone_c0 = -1;
    cycle_t tlast_c1 = -1, opdone_c1 = -1;
    cycle_t compute_start_c0 = -1, compute_done_c0 = -1;
    cycle_t compute_start_c1 = -1, compute_done_c1 = -1;
    cycle_t last_tile_done_c0 = -1, last_tile_done_c1 = -1;
    int tile_done_count_c0 = 0, tile_done_count_c1 = 0;

    for (const auto& t : sim.trace()) {
        if (t.dispatch_id == 0) {
            if (t.type == EventType::DMA_TLAST && tlast_c0 < 0) tlast_c0 = t.cycle;
            if (t.type == EventType::COMPUTE_START) compute_start_c0 = t.cycle;
            if (t.type == EventType::COMPUTE_DONE)  compute_done_c0  = t.cycle;
            if (t.type == EventType::TILE_DONE)     { tile_done_count_c0++; last_tile_done_c0 = t.cycle; }
            if (t.type == EventType::OP_DONE)       opdone_c0 = t.cycle;
        } else if (t.dispatch_id == 1) {
            if (t.type == EventType::DMA_TLAST && tlast_c1 < 0) tlast_c1 = t.cycle;
            if (t.type == EventType::COMPUTE_START) compute_start_c1 = t.cycle;
            if (t.type == EventType::COMPUTE_DONE)  compute_done_c1  = t.cycle;
            if (t.type == EventType::TILE_DONE)     { tile_done_count_c1++; last_tile_done_c1 = t.cycle; }
            if (t.type == EventType::OP_DONE)       opdone_c1 = t.cycle;
        }
    }

    std::fprintf(stderr, "\n-- extracted events --\n");
    std::fprintf(stderr, "  chunk 0: TLAST=%lld  compute_start=%lld  last_tile_done=%lld  compute_done=%lld  op_done=%lld  tile_pulses=%d\n",
        (long long)tlast_c0, (long long)compute_start_c0,
        (long long)last_tile_done_c0, (long long)compute_done_c0,
        (long long)opdone_c0, tile_done_count_c0);
    std::fprintf(stderr, "  chunk 1: TLAST=%lld  compute_start=%lld  last_tile_done=%lld  compute_done=%lld  op_done=%lld  tile_pulses=%d\n",
        (long long)tlast_c1, (long long)compute_start_c1,
        (long long)last_tile_done_c1, (long long)compute_done_c1,
        (long long)opdone_c1, tile_done_count_c1);
    std::fprintf(stderr, "\n");

    // GOLDEN: chunk cycles = op_done - tlast
    CHECK_EQ(tile_done_count_c0, 128, "tile_done pulses chunk 0 == 128");
    CHECK_EQ(tile_done_count_c1, 128, "tile_done pulses chunk 1 == 128");
    CHECK_EQ(opdone_c0 - tlast_c0, 13063, "chunk 0 (TLAST→OP_DONE) == 13063 (STAGE B FIRST)");
    CHECK_EQ(opdone_c1 - tlast_c1, 13083, "chunk 1 (TLAST→OP_DONE) == 13083 (STAGE B LAST)");

    // Compute-relative deltas match RTL trace
    CHECK_EQ(compute_done_c0 - compute_start_c0, 13057, "chunk 0 compute_done - compute_start == 13057 (RTL FIRST inner)");
    CHECK_EQ(compute_done_c1 - compute_start_c1, 13073, "chunk 1 compute_done - compute_start == 13073 (RTL LAST inner + wb)");

    // Simulator wall cycles > 0
    CHECK(final_c > 0, "simulator progressed");

    // Report per-milestone summary
    std::fprintf(stderr, "\n== stage_b_chain milestone summary ==\n");
    std::fprintf(stderr, "  events processed = %llu\n", (unsigned long long)stats.events_processed);
    std::fprintf(stderr, "  wall cycles      = %lld\n", (long long)final_c);
    std::fprintf(stderr, "  failures         = %d\n", failures);
    return failures == 0 ? 0 : 1;
}
