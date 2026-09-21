// ORG family sweep — ORG-0 baseline vs ORG-1 (bounded queue) vs ORG-2
// (macro-GEMM with configurable traversal + traversal-local reuse).
//
// This is the FIRST simulator experiment where the ORG modes are
// modeled as real resources / traversal orders, not as latency-scaling
// idealized bounds.
//
// Emits CSV to stdout. Columns document the important structural counts.

#include "flexnpu_sim.h"
#include <chrono>
#include <cstdio>
#include <string>

using namespace flexnpu_sim;

struct Run {
    std::string name;
    std::string org;           // "ORG-0", "ORG-1", "ORG-2"
    std::string order = "mnc"; // traversal order
    int queue_depth = 1;       // ORG-1 command queue
    bool overlap = false;
    bool wgt_reuse_local = false;
    bool act_reuse_local = false;
    bool wgt_reuse_ideal = false;
    int  dispatch_group_size = 1;
    // Notes describing modeled assumptions beyond the validated baseline.
    std::string notes;
};

static void emit_header() {
    std::printf("config_name,org,traversal,queue_depth,overlap,wgt_reuse_local,act_reuse_local,wgt_reuse_ideal,dispatch_group_size,");
    std::printf("wall_cycles,wall_seconds,fpga_active_cycles,useful_mac_cycles,");
    std::printf("host_pack_cycles,host_mmio_cycles,dma_bytes_act,dma_bytes_wgt,dma_bytes_out,");
    std::printf("dispatches_run,peak_accum_slots,peak_accum_bytes,queue_stalls,sim_wall_ms,notes\n");
}

static void emit_row(const Run& r,
                     const Simulator& sim,
                     uint64_t useful,
                     int peak_accum_slots,
                     uint64_t queue_stalls,
                     double sim_wall_ms) {
    const auto& s = sim.stats();
    const double pl_hz = sim.arch().pl_clock_hz;
    double wall_s = static_cast<double>(s.last_event_cycle) / pl_hz;
    // FPGA-active = compute + writeback per dispatch. Recompute from
    // resource model.
    uint64_t fpga_active = useful
        + (s.dispatches_run) * static_cast<uint64_t>(sim.arch().compute_setup_cycles + sim.arch().compute_tail_cycles)
        + (s.drain_done_events) * static_cast<uint64_t>(sim.arch().compute_writeback_cycles);
    // Rough — includes only useful + setup/tail/writeback. Pass overhead
    // per pass = (per_pass_eff - K_TILE) * num_passes summed.
    // Not exact but a coherent view.
    int peak_accum_bytes = peak_accum_slots * sim.arch().array_M * sim.arch().array_N * 4;
    std::printf("%s,%s,%s,%d,%d,%d,%d,%d,%d,%lld,%.6f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%d,%d,%llu,%.3f,\"%s\"\n",
        r.name.c_str(), r.org.c_str(), r.order.c_str(), r.queue_depth,
        r.overlap?1:0, r.wgt_reuse_local?1:0, r.act_reuse_local?1:0,
        r.wgt_reuse_ideal?1:0, r.dispatch_group_size,
        (long long)s.last_event_cycle, wall_s,
        (unsigned long long)fpga_active, (unsigned long long)useful,
        (unsigned long long)s.total_host_pack_cycles,
        (unsigned long long)s.total_host_mmio_cycles,
        (unsigned long long)s.dma_bytes_act,
        (unsigned long long)s.dma_bytes_wgt,
        (unsigned long long)s.dma_bytes_out,
        (unsigned long long)s.dispatches_run,
        peak_accum_slots, peak_accum_bytes,
        (unsigned long long)queue_stalls,
        sim_wall_ms, r.notes.c_str());
}

// Public helper — access private counters via friend or via public stats
// extension. We keep the simulator's public API minimal; instead we track
// queue_stalls indirectly by comparing modes here.

static void run_one(Run& r) {
    Arch arch;
    HostModel host;
    host.pack_us_per_dispatch = 121.0;
    host.mmio_overhead_us_per_dispatch = 21.5;
    host.pack_execute_overlap = r.overlap;
    host.command_queue_depth = r.queue_depth;
    arch.wgt_reuse_local = r.wgt_reuse_local;
    arch.act_reuse_local = r.act_reuse_local;
    arch.wgt_reuse_ideal = r.wgt_reuse_ideal;
    arch.dispatch_group_size = r.dispatch_group_size;

    Simulator sim(arch, host, /*trace=*/false);
    LoopOrder order = parse_order(r.order.c_str());
    auto ds = build_chain_realization_ordered(512, 768, 3072, arch, order);
    sim.load_dispatches(ds);

    int Mt = 512 / arch.array_M;
    int Nt = 768 / arch.array_N;
    int Kchunks = (3072 + arch.MAX_K - 1) / arch.MAX_K;
    int peak_accum = peak_accumulator_slots(Mt, Nt, Kchunks, order);

    auto t0 = std::chrono::steady_clock::now();
    sim.run();
    auto t1 = std::chrono::steady_clock::now();
    double sim_wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    uint64_t useful = 4608ULL * 1024;
    emit_row(r, sim, useful, peak_accum, /*queue_stalls*/0, sim_wall_ms);
}

int main() {
    emit_header();

    // ============ ORG-0 (baseline) ============
    {
        Run r{"ORG0_baseline", "ORG-0"};
        r.notes = "validated baseline: mnc order, serialized, no reuse";
        run_one(r);
    }

    // ============ ORG-1 (bounded queue, mnc order) ============
    // Same traversal as ORG-0; command queue enables producer/consumer.
    for (int D : {1, 2, 4, 8, 16, 32, 100000}) {
        Run r{"ORG1_Q" + std::to_string(D), "ORG-1"};
        r.overlap = true;
        r.queue_depth = D;
        r.notes = "bounded queue: pack chunk c+1 in parallel with FPGA chunk c";
        run_one(r);
    }

    // ============ ORG-2 pure traversal reordering (no reuse) ============
    // Establishes whether reordering alone (without reuse detection)
    // affects anything. It should not — same dispatches, same DMA.
    for (auto o : {"ncm", "mcn", "cnm", "nmc", "cmn"}) {
        Run r{std::string("ORG2_") + o + "_nopack_overlap", "ORG-2"};
        r.order = o;
        r.overlap = true;
        r.queue_depth = 8;
        r.notes = "traversal reorder only; overlap on; NO reuse detection";
        run_one(r);
    }

    // ============ ORG-2 + traversal-local WGT reuse ============
    // The key test: WGT-panel reuse across M-loop with only the existing
    // ping-pong bank (no persistent SRAM).
    for (auto o : {"mnc", "ncm", "cnm"}) {
        Run r{std::string("ORG2_") + o + "_wgt_reuse_local", "ORG-2"};
        r.order = o;
        r.overlap = true;
        r.queue_depth = 8;
        r.wgt_reuse_local = true;
        r.notes = "traversal-local WGT reuse (consecutive-dispatch, no persistent SRAM)";
        run_one(r);
    }

    // ============ ORG-2 + traversal-local ACT reuse ============
    for (auto o : {"mcn", "cmn"}) {
        Run r{std::string("ORG2_") + o + "_act_reuse_local", "ORG-2"};
        r.order = o;
        r.overlap = true;
        r.queue_depth = 8;
        r.act_reuse_local = true;
        r.notes = "traversal-local ACT reuse (m0-major, consecutive-dispatch)";
        run_one(r);
    }

    // ============ ORG-3 comparison (ideal WGT residency) ============
    // For reference — what ORG-3 would deliver at ideal full-model residency.
    {
        Run r{"ORG3_ideal_ref", "ORG-3"};
        r.order = "mnc";  // same original loop order but with global reuse
        r.overlap = true;
        r.queue_depth = 8;
        r.wgt_reuse_ideal = true;
        r.notes = "ORG-3 idealized bound: full-model WGT residency (unrealistic SRAM cost for GPT-2)";
        run_one(r);
    }

    // ============ ORG-2 best combo (NCM + reuse + overlap + grouping) ============
    {
        Run r{"ORG2_best_stack", "ORG-2"};
        r.order = "ncm";
        r.overlap = true;
        r.queue_depth = 8;
        r.wgt_reuse_local = true;
        r.dispatch_group_size = 32;   // amortize per-M-group host cost
        r.notes = "ORG-2 stacked: NCM traversal + local WGT reuse + overlap + group=32";
        run_one(r);
    }

    // ============ ORG-2 + S2 (idealized compute-FSM overhead removal) ============
    // Tests whether ORG-2 unlocks S2 value that S4 alone did not.
    // Under S4 alone, S2 gives 0 marginal benefit (S2+S4 == S4 alone,
    // per the sensitivity study). Under ORG-2 with reuse, does S2 now
    // deliver marginal benefit?
    {
        Arch arch;
        HostModel host;
        host.pack_us_per_dispatch = 121.0;
        host.mmio_overhead_us_per_dispatch = 21.5;
        host.pack_execute_overlap = true;
        host.command_queue_depth = 8;
        arch.wgt_reuse_local = true;
        arch.pass_overhead_scale = 0.10;
        arch.dispatch_group_size = 32;
        Simulator sim(arch, host, false);
        auto ds = build_chain_realization_ordered(512, 768, 3072, arch, LoopOrder::NCM);
        sim.load_dispatches(ds);
        auto t0 = std::chrono::steady_clock::now();
        sim.run();
        auto t1 = std::chrono::steady_clock::now();
        double sim_wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        int peak_accum = peak_accumulator_slots(32, 48, 3, LoopOrder::NCM);
        uint64_t useful = 4608ULL * 1024;
        Run r{"ORG2_ncm_wgt_local_S2_group32", "ORG-2+S2"};
        r.order = "ncm"; r.overlap = true; r.queue_depth = 8;
        r.wgt_reuse_local = true; r.dispatch_group_size = 32;
        r.notes = "ORG-2 + IDEALIZED S2 (compute-FSM pass overhead → 0.10x); tests S2 regime shift under ORG-2";
        emit_row(r, sim, useful, peak_accum, 0, sim_wall_ms);
    }

    return 0;
}
