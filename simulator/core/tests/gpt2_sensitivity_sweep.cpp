// GPT-2 FFN mechanism sensitivity + bottleneck-transition sweep.
//
// This is the FIRST RESEARCH EXPERIMENT using the validated simulator.
// It does not modify the validated baseline path; every sensitivity run
// is an OPT-IN idealized capability mode (except S1/S3/S6 which are
// clean parameter scales of measured host values).
//
// Emits CSV to stdout with columns:
//   config_name, pack_scale, mmio_scale, pass_overhead_scale, overlap,
//   wgt_reuse_ideal, dispatch_group_size, wall_cycles, wall_seconds,
//   fpga_active_cycles, useful_mac_cycles, pass_overhead_cycles,
//   host_pack_cycles, host_mmio_cycles, dma_bytes_act, dma_bytes_wgt,
//   dma_bytes_out, dispatches_run, sim_wall_ms
//
// Every row is a full GPT-2 FFN K=3072 run.

#include "flexnpu_sim.h"
#include <chrono>
#include <cstdio>
#include <string>

using namespace flexnpu_sim;

struct Run {
    std::string name;
    double pack_scale = 1.0;
    double mmio_scale = 1.0;
    double pass_overhead_scale = 1.0;
    bool   overlap = false;
    bool   wgt_reuse_ideal = false;
    int    dispatch_group_size = 1;
    std::string type;  // "baseline" or "idealized_bound"
};

static void emit_header() {
    std::printf("config_name,type,pack_scale,mmio_scale,pass_overhead_scale,overlap,wgt_reuse_ideal,dispatch_group_size,");
    std::printf("wall_cycles,wall_seconds,fpga_active_cycles,useful_mac_cycles,pass_overhead_cycles,");
    std::printf("host_pack_cycles,host_mmio_cycles,dma_bytes_act,dma_bytes_wgt,dma_bytes_out,dispatches_run,");
    std::printf("array_utilization_e2e,array_utilization_fpga,sim_wall_ms\n");
}

static void emit_row(const Run& r,
                     const Simulator& sim,
                     uint64_t useful,
                     uint64_t pass_overhead,
                     double sim_wall_ms) {
    const auto& s = sim.stats();
    const double pl_hz = sim.arch().pl_clock_hz;
    double wall_s = static_cast<double>(s.last_event_cycle) / pl_hz;
    // FPGA-active from resource model: compute + writeback per dispatch
    // FIRST/MIDDLE: (setup + K/K_TILE * per_pass_eff + tail) with wb=0
    // LAST: + writeback
    // With pass_overhead_scale, per_pass_eff changes; use recorded counters.
    uint64_t fpga_active = useful + pass_overhead
        + 4608ULL * (sim.arch().compute_setup_cycles + sim.arch().compute_tail_cycles)
        + 1536ULL * sim.arch().compute_writeback_cycles;
    double fpga_active_s = static_cast<double>(fpga_active) / pl_hz;
    double util_e2e = static_cast<double>(useful) / static_cast<double>(s.last_event_cycle);
    double util_fpga = fpga_active > 0 ? static_cast<double>(useful) / static_cast<double>(fpga_active) : 0.0;
    std::printf("%s,%s,%.3f,%.3f,%.3f,%d,%d,%d,%lld,%.6f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.6f,%.6f,%.3f\n",
        r.name.c_str(), r.type.c_str(),
        r.pack_scale, r.mmio_scale, r.pass_overhead_scale,
        r.overlap ? 1 : 0, r.wgt_reuse_ideal ? 1 : 0, r.dispatch_group_size,
        (long long)s.last_event_cycle, wall_s,
        (unsigned long long)fpga_active, (unsigned long long)useful,
        (unsigned long long)pass_overhead,
        (unsigned long long)s.total_host_pack_cycles,
        (unsigned long long)s.total_host_mmio_cycles,
        (unsigned long long)s.dma_bytes_act,
        (unsigned long long)s.dma_bytes_wgt,
        (unsigned long long)s.dma_bytes_out,
        (unsigned long long)s.dispatches_run,
        util_e2e, util_fpga,
        sim_wall_ms);
}

static void run_one(const Run& r) {
    Arch arch;
    HostModel host;
    host.pack_us_per_dispatch = 121.0;
    host.mmio_overhead_us_per_dispatch = 21.5;
    host.pack_scale = r.pack_scale;
    host.mmio_scale = r.mmio_scale;
    host.pack_execute_overlap = r.overlap;
    arch.pass_overhead_scale = r.pass_overhead_scale;
    arch.wgt_reuse_ideal = r.wgt_reuse_ideal;
    arch.dispatch_group_size = r.dispatch_group_size;

    Simulator sim(arch, host, /*trace=*/false);
    sim.load_dispatches(build_chain_realization(512, 768, 3072, arch));
    auto t0 = std::chrono::steady_clock::now();
    sim.run();
    auto t1 = std::chrono::steady_clock::now();
    double sim_wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Reach into the sim for detailed counters via public trace/stats.
    // The public stats give us useful_mac indirectly via useful_MACs / 256.
    // Better: expose via friend? Simpler: recompute here.
    // Useful array cycles = K_TILE * sum_dispatches(K/K_TILE) = sum_dispatches(K)
    // For GPT-2: 4608 dispatches * K=1024 each = 4,718,592 (matches 1.21 G MAC / 256 MAC/cyc)
    uint64_t useful = 4608ULL * 1024;
    // Pass overhead per dispatch = (per_pass_eff - K_TILE) * (K/K_TILE)
    int K_TILE = arch.K_TILE;
    int per_pass_baseline = arch.compute_per_pass_cycles;
    int per_pass_overhead_baseline = per_pass_baseline - K_TILE;
    int per_pass_eff = K_TILE + static_cast<int>(std::round(per_pass_overhead_baseline * arch.pass_overhead_scale));
    if (per_pass_eff < 1) per_pass_eff = 1;
    int pass_overhead_per_dispatch = (per_pass_eff - K_TILE) * (1024 / K_TILE);
    uint64_t pass_overhead = 4608ULL * pass_overhead_per_dispatch;
    emit_row(r, sim, useful, pass_overhead, sim_wall_ms);
}

int main() {
    emit_header();

    // -------- Baseline reconstruction --------
    Run baseline{"baseline"};
    baseline.type = "baseline";
    run_one(baseline);

    // -------- S1: host pack cost sensitivity --------
    for (double s : {1.00, 0.75, 0.50, 0.25, 0.00}) {
        Run r{"S1_pack_" + std::to_string(s)};
        r.pack_scale = s;
        r.type = "idealized_bound";
        run_one(r);
    }

    // -------- S2: compute-FSM per-pass overhead scaling --------
    for (double s : {1.00, 0.75, 0.50, 0.25, 0.10, 0.00}) {
        Run r{"S2_pass_" + std::to_string(s)};
        r.pass_overhead_scale = s;
        r.type = "idealized_bound";
        run_one(r);
    }

    // -------- S3: host runtime/mmio overhead scaling --------
    for (double s : {1.00, 0.50, 0.25, 0.00}) {
        Run r{"S3_mmio_" + std::to_string(s)};
        r.mmio_scale = s;
        r.type = "idealized_bound";
        run_one(r);
    }

    // -------- S4: pack / FPGA execution overlap --------
    {
        Run r{"S4_overlap_on"};
        r.overlap = true;
        r.type = "idealized_bound";
        run_one(r);
    }

    // -------- S5: WGT reuse ceiling --------
    {
        Run r{"S5_wgt_reuse_ideal"};
        r.wgt_reuse_ideal = true;
        r.type = "idealized_bound";
        run_one(r);
    }

    // -------- S6: dispatch granularity / grouping amortization --------
    for (int g : {1, 2, 4, 8, 16, 32, 1536}) {
        Run r{"S6_group_" + std::to_string(g)};
        r.dispatch_group_size = g;
        r.type = "idealized_bound";
        run_one(r);
    }

    // -------- Interaction studies --------
    {
        Run r{"IA_S2_S4"};
        r.pass_overhead_scale = 0.10; r.overlap = true;
        r.type = "idealized_bound";
        run_one(r);
    }
    {
        Run r{"IA_S2_S5"};
        r.pass_overhead_scale = 0.10; r.wgt_reuse_ideal = true;
        r.type = "idealized_bound";
        run_one(r);
    }
    {
        Run r{"IA_S5_S6"};
        r.wgt_reuse_ideal = true; r.dispatch_group_size = 32;
        r.type = "idealized_bound";
        run_one(r);
    }
    {
        Run r{"IA_S2_S4_S5"};
        r.pass_overhead_scale = 0.10; r.overlap = true; r.wgt_reuse_ideal = true;
        r.type = "idealized_bound";
        run_one(r);
    }
    {
        Run r{"IA_ALL_IDEAL"};
        r.pack_scale = 0.0; r.mmio_scale = 0.0;
        r.pass_overhead_scale = 0.0;
        r.overlap = true; r.wgt_reuse_ideal = true;
        r.dispatch_group_size = 1;
        r.type = "idealized_bound";
        run_one(r);
    }

    return 0;
}
