// Focused tests for the ORG family execution semantics.
// * ORG-0 baseline unchanged (already covered by test_gpt2).
// * ORG-1 queue depth 1 == ORG-0 (no overlap).
// * ORG-2 NCM traversal + wgt_reuse_local: WGT DMA bytes drop to
//   Nt*Kchunks unique-panel bytes.
// * ORG-2 MCN traversal + act_reuse_local: ACT DMA bytes drop
//   symmetrically.
// * Traversal reordering alone (no reuse) preserves DMA totals.

#include "flexnpu_sim.h"
#include <cstdio>

using namespace flexnpu_sim;

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else         { std::fprintf(stderr, "ok  : %s\n", msg); } \
} while (0)

#define CHECK_EQ(a, e, msg) do { \
    auto _a = (a); auto _e = (e); \
    if (_a != _e) { std::fprintf(stderr, "FAIL: %s: got %llu want %llu\n", msg, (unsigned long long)_a, (unsigned long long)_e); failures++; } \
    else         { std::fprintf(stderr, "ok  : %s = %llu\n", msg, (unsigned long long)_a); } \
} while (0)

static Simulator run_gpt2(const Arch& arch, const HostModel& host, LoopOrder ord) {
    Simulator sim(arch, host, /*trace=*/false);
    auto ds = build_chain_realization_ordered(512, 768, 3072, arch, ord);
    sim.load_dispatches(ds);
    sim.run();
    return sim;
}

int main() {
    const uint64_t EXPECT_ACT = 4608ULL * 16384;  // 75.5 MB (no reuse)
    const uint64_t EXPECT_WGT = 4608ULL * 16384;  // 75.5 MB (no reuse)
    const uint64_t EXPECT_OUT = 1536ULL * 1024;   // 1.5 MB

    // -------- Test 1: ORG-0 baseline (mnc, no overlap, no reuse) -------
    {
        Arch arch; HostModel host;
        host.pack_us_per_dispatch = 121.0;
        host.mmio_overhead_us_per_dispatch = 21.5;
        auto sim = run_gpt2(arch, host, LoopOrder::MNC);
        const auto& s = sim.stats();
        std::fprintf(stderr, "\n-- ORG-0 baseline --\n");
        CHECK_EQ(s.dispatches_run, 4608u, "ORG-0 dispatches == 4608");
        CHECK_EQ(s.dma_bytes_act, EXPECT_ACT, "ORG-0 ACT DMA == 75.5 MB");
        CHECK_EQ(s.dma_bytes_wgt, EXPECT_WGT, "ORG-0 WGT DMA == 75.5 MB");
        CHECK_EQ(s.dma_bytes_out, EXPECT_OUT, "ORG-0 OUT DMA == 1.5 MB");
    }

    // -------- Test 2: ORG-1 D=1 (should equal ORG-0 exactly) --------
    // (overlap enabled but queue depth 1 forces serialization)
    {
        Arch arch; HostModel host;
        host.pack_us_per_dispatch = 121.0;
        host.mmio_overhead_us_per_dispatch = 21.5;
        host.pack_execute_overlap = true;
        host.command_queue_depth = 1;
        auto sim = run_gpt2(arch, host, LoopOrder::MNC);
        const auto& s = sim.stats();
        std::fprintf(stderr, "\n-- ORG-1 D=1 (should ≈ ORG-0) --\n");
        CHECK_EQ(s.dma_bytes_wgt, EXPECT_WGT, "ORG-1 D=1 WGT DMA == 75.5 MB");
        CHECK_EQ(s.dispatches_run, 4608u, "ORG-1 D=1 dispatches == 4608");
    }

    // -------- Test 3: traversal reorder alone preserves DMA totals ------
    for (auto ord : {LoopOrder::NCM, LoopOrder::MCN, LoopOrder::CNM}) {
        Arch arch; HostModel host;
        host.pack_us_per_dispatch = 121.0;
        host.mmio_overhead_us_per_dispatch = 21.5;
        auto sim = run_gpt2(arch, host, ord);
        const auto& s = sim.stats();
        std::fprintf(stderr, "\n-- traversal-only reorder (order=%d): DMA preserved --\n", (int)ord);
        CHECK_EQ(s.dma_bytes_act, EXPECT_ACT, "reorder-only ACT DMA unchanged");
        CHECK_EQ(s.dma_bytes_wgt, EXPECT_WGT, "reorder-only WGT DMA unchanged");
        CHECK_EQ(s.dispatches_run, 4608u, "reorder-only dispatches == 4608");
    }

    // -------- Test 4: ORG-2 NCM + wgt_reuse_local: WGT DMA collapses --
    {
        Arch arch; HostModel host;
        host.pack_us_per_dispatch = 121.0;
        host.mmio_overhead_us_per_dispatch = 21.5;
        host.pack_execute_overlap = true;
        host.command_queue_depth = 8;
        arch.wgt_reuse_local = true;
        auto sim = run_gpt2(arch, host, LoopOrder::NCM);
        const auto& s = sim.stats();
        // Under NCM, consecutive dispatches share (n0, c) for the inner
        // m0-loop of length Mt=32. So WGT reuse fires (Mt-1)=31 times
        // per (n0, c) group. Total unique loads = Nt*Kchunks = 48*3 = 144.
        uint64_t expect_wgt = 144ULL * 16384;
        std::fprintf(stderr, "\n-- ORG-2 NCM + wgt_reuse_local: WGT DMA collapses --\n");
        CHECK_EQ(s.dma_bytes_wgt, expect_wgt, "ORG-2 NCM+wgt_reuse_local WGT DMA == 144×16KB");
        CHECK_EQ(s.dma_bytes_act, EXPECT_ACT, "ORG-2 NCM ACT DMA unchanged (48×amp)");
        double amp = static_cast<double>(s.dma_bytes_wgt) / (2.25 * 1024 * 1024);
        std::fprintf(stderr, "     WGT amplification vs raw B: %.2fx (was 32x on ORG-0)\n", amp);
    }

    // -------- Test 5: ORG-2 MCN + act_reuse_local: ACT DMA collapses ---
    {
        Arch arch; HostModel host;
        host.pack_us_per_dispatch = 121.0;
        host.mmio_overhead_us_per_dispatch = 21.5;
        host.pack_execute_overlap = true;
        host.command_queue_depth = 8;
        arch.act_reuse_local = true;
        auto sim = run_gpt2(arch, host, LoopOrder::MCN);
        const auto& s = sim.stats();
        // Under MCN: for m0: for c: for n0. Consecutive share (m0, c).
        // ACT reuse fires (Nt-1)=47 times per (m0, c) group.
        // Unique ACT loads = Mt*Kchunks = 32*3 = 96.
        uint64_t expect_act = 96ULL * 16384;
        std::fprintf(stderr, "\n-- ORG-2 MCN + act_reuse_local: ACT DMA collapses --\n");
        CHECK_EQ(s.dma_bytes_act, expect_act, "ORG-2 MCN+act_reuse_local ACT DMA == 96×16KB");
        CHECK_EQ(s.dma_bytes_wgt, EXPECT_WGT, "ORG-2 MCN WGT DMA unchanged (32×amp)");
        double amp = static_cast<double>(s.dma_bytes_act) / (1.5 * 1024 * 1024);
        std::fprintf(stderr, "     ACT amplification vs raw A: %.2fx (was 48x on ORG-0)\n", amp);
    }

    // -------- Test 6: peak accumulator storage varies with traversal --
    {
        int Mt = 512/16, Nt = 768/16, Kchunks = 3;
        int p_mnc = peak_accumulator_slots(Mt, Nt, Kchunks, LoopOrder::MNC);
        int p_ncm = peak_accumulator_slots(Mt, Nt, Kchunks, LoopOrder::NCM);
        int p_mcn = peak_accumulator_slots(Mt, Nt, Kchunks, LoopOrder::MCN);
        int p_cnm = peak_accumulator_slots(Mt, Nt, Kchunks, LoopOrder::CNM);
        std::fprintf(stderr, "\n-- peak accumulator slots per traversal order --\n");
        std::fprintf(stderr, "  MNC (baseline): %d slots (= %d KB)\n", p_mnc, p_mnc*1024/1024);
        std::fprintf(stderr, "  NCM:            %d slots (= %d KB)\n", p_ncm, p_ncm*1024/1024);
        std::fprintf(stderr, "  MCN:            %d slots (= %d KB)\n", p_mcn, p_mcn*1024/1024);
        std::fprintf(stderr, "  CNM:            %d slots (= %d KB)\n", p_cnm, p_cnm*1024/1024);
        // MNC = 1, NCM = Mt (=32), MCN = Nt (=48), CNM = Mt*Nt (=1536).
        CHECK_EQ(p_mnc, 1, "MNC peak accum == 1 slot");
        CHECK_EQ(p_ncm, 32, "NCM peak accum == Mt = 32 slots");
        CHECK_EQ(p_mcn, 48, "MCN peak accum == Nt = 48 slots");
        CHECK_EQ(p_cnm, 1536, "CNM peak accum == Mt*Nt = 1536 slots (unrealizable)");
    }

    std::fprintf(stderr, "\n== org_family tests failures: %d ==\n", failures);
    return failures == 0 ? 0 : 1;
}
