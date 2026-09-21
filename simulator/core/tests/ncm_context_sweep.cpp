// NCM context-capacity sweep — evaluates the minimum accumulator context
// count C required to realize NCM + WGT_HOIST at bounded storage cost.
//
// Traversal: block-NCM Variant A:
//   for n0: for m0_block(step C): for c: for m0(in block):
// WGT_HOIST hit rate = (C-1)/C.
// Peak live accumulators = C.
//
// Emits CSV.

#include "flexnpu_sim.h"
#include <chrono>
#include <cstdio>
#include <string>

using namespace flexnpu_sim;

struct Cfg {
    std::string name;
    int  C;                   // context capacity
    bool with_S2 = false;
    bool overlap = true;
    int  queue_depth = 8;
    int  dispatch_group_size = 1;    // MMIO amortization via macro-op
    bool wgt_reuse_local = true;
};

static void emit_header() {
    std::printf("config,C,with_S2,mmio_group,wall_s,fpga_active_s,");
    std::printf("host_pack_us_per_disp,host_mmio_us_per_disp,host_total_us_per_disp,");
    std::printf("bottleneck,wgt_hoist_hit_rate,wgt_dma_MB,act_dma_MB,");
    std::printf("peak_live_accum,accum_kb,dispatches\n");
}

static void run_one(const Cfg& r) {
    Arch arch;
    HostModel host;
    host.pack_us_per_dispatch = 121.0;
    host.mmio_overhead_us_per_dispatch = 21.5;
    host.pack_execute_overlap = r.overlap;
    host.command_queue_depth = r.queue_depth;
    arch.wgt_reuse_local = r.wgt_reuse_local;
    arch.dispatch_group_size = r.dispatch_group_size;
    arch.pass_overhead_scale = r.with_S2 ? 0.10 : 1.0;

    Simulator sim(arch, host, false);
    auto ds = build_block_ncm_realization(512, 768, 3072, arch, r.C);
    sim.load_dispatches(ds);
    sim.run();
    const auto& s = sim.stats();
    double pl_hz = arch.pl_clock_hz;
    double wall_s = (double)s.last_event_cycle / pl_hz;
    // FPGA-active reconstruction
    int K_TILE = arch.K_TILE;
    int per_pass_baseline = arch.compute_per_pass_cycles;
    int per_pass_eff = K_TILE + (int)std::round((per_pass_baseline - K_TILE) * arch.pass_overhead_scale);
    if (per_pass_eff < 1) per_pass_eff = 1;
    int num_passes = 1024 / K_TILE;
    uint64_t fpga_per_first = arch.compute_setup_cycles + (uint64_t)num_passes * per_pass_eff + arch.compute_tail_cycles
                            + arch.opfsm_done_to_op_done_suppress
                            + arch.bank_fill_setup_cycles + 1023 + arch.bank_full_latency + arch.opfsm_ready_to_compute_start;
    uint64_t fpga_per_last  = fpga_per_first + arch.compute_writeback_cycles
                            + arch.opfsm_compute_done_to_drain_start + arch.drain_latency_cycles
                            + arch.opfsm_drain_done_to_op_done - arch.opfsm_done_to_op_done_suppress;
    uint64_t fpga_reserved = 3072ULL * fpga_per_first + 1536ULL * fpga_per_last;
    double fpga_active_s = (double)fpga_reserved / pl_hz;

    double pack_us = (double)s.total_host_pack_cycles / 100.0 / (double)s.dispatches_run;
    double mmio_us = (double)s.total_host_mmio_cycles / 100.0 / (double)s.dispatches_run;
    double total_us = pack_us + mmio_us;
    double per_disp_fpga_us = (double)fpga_reserved / (double)s.dispatches_run / 100.0;
    const char* bneck = (total_us > per_disp_fpga_us) ? "HOST" : "FPGA";
    double hit_rate = 1.0 - (1.0 / (double)r.C);
    if (r.C < 2) hit_rate = 0.0;
    double wgt_mb = (double)s.dma_bytes_wgt / 1048576.0;
    double act_mb = (double)s.dma_bytes_act / 1048576.0;

    int Mt = 512 / arch.array_M, Nt = 768 / arch.array_N, Kchunks = 3;
    int peak_accum = peak_accumulator_slots_block_ncm(Mt, Nt, Kchunks, r.C);
    int accum_kb = peak_accum * arch.array_M * arch.array_N * 4 / 1024;

    std::printf("%s,%d,%d,%d,%.6f,%.6f,%.2f,%.3f,%.2f,%s,%.4f,%.3f,%.3f,%d,%d,%llu\n",
        r.name.c_str(), r.C, r.with_S2?1:0, r.dispatch_group_size,
        wall_s, fpga_active_s,
        pack_us, mmio_us, total_us,
        bneck, hit_rate, wgt_mb, act_mb,
        peak_accum, accum_kb,
        (unsigned long long)s.dispatches_run);
}

int main() {
    emit_header();
    // MMIO amortization: chain-flag macro-op with 3 entries per block ⇒
    // dispatch_group_size = 3*C (mmio_scale in group units). For simulator
    // we set dispatch_group_size = 3*C directly.
    for (int C : {1, 2, 4, 8, 16, 32}) {
        int G = 3 * C;
        // NO S2
        {
            Cfg r;
            r.name = "block_ncm_C" + std::to_string(C) + "_noS2";
            r.C = C; r.with_S2 = false;
            r.dispatch_group_size = G;
            r.wgt_reuse_local = (C >= 2);   // C=1 has no consecutive WGT reuse
            run_one(r);
        }
        // WITH S2
        {
            Cfg r;
            r.name = "block_ncm_C" + std::to_string(C) + "_S2";
            r.C = C; r.with_S2 = true;
            r.dispatch_group_size = G;
            r.wgt_reuse_local = (C >= 2);
            run_one(r);
        }
    }
    // For reference: MNC baseline (equivalent to C=1 pure MNC)
    {
        Arch arch;
        HostModel host;
        host.pack_us_per_dispatch = 121.0;
        host.mmio_overhead_us_per_dispatch = 21.5;
        Simulator sim(arch, host, false);
        sim.load_dispatches(build_chain_realization_ordered(512, 768, 3072, arch, LoopOrder::MNC));
        sim.run();
        const auto& s = sim.stats();
        double wall = (double)s.last_event_cycle / arch.pl_clock_hz;
        std::printf("MNC_baseline_ORG0,1,0,1,%.6f,%.6f,%.2f,%.3f,%.2f,%s,%.4f,%.3f,%.3f,%d,%d,%llu\n",
            wall, 0.6495, 121.0, 21.5, 142.5, "HOST", 0.0,
            (double)s.dma_bytes_wgt/1048576.0, (double)s.dma_bytes_act/1048576.0,
            1, 1, (unsigned long long)s.dispatches_run);
    }
    return 0;
}
