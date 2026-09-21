// FlexNPU cycle-accurate simulator — CLI entry point.
// Usage: flexnpu_sim <realization> [--trace file] [--json]
//
// Realizations available:
//   stage_b_chain          16x16 K=2048 2-chunk (STAGE B microbenchmark, host-free)
//   gpt2_ffn               A[512x3072]xB[3072x768] K=3072 (physical anchor)
//   mb_style               MB-style single-dispatch persist=1 hit path

#include "flexnpu_sim.h"
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

using namespace flexnpu_sim;

static void print_stats_json(const Simulator& sim, double pl_hz, std::ostream& out) {
    const auto& s = sim.stats();
    out << "{\n";
    out << "  \"events_processed\": "         << s.events_processed << ",\n";
    out << "  \"dispatches_run\": "           << s.dispatches_run << ",\n";
    out << "  \"wall_cycles\": "              << s.last_event_cycle << ",\n";
    out << "  \"wall_seconds\": "             << (static_cast<double>(s.last_event_cycle)/pl_hz) << ",\n";
    out << "  \"tile_done_events\": "         << s.tile_done_events << ",\n";
    out << "  \"bank_swaps_wr\": "            << s.bank_swaps_wr << ",\n";
    out << "  \"bank_swaps_rd\": "            << s.bank_swaps_rd << ",\n";
    out << "  \"compute_done_events\": "      << s.compute_done_events << ",\n";
    out << "  \"drain_done_events\": "        << s.drain_done_events << ",\n";
    out << "  \"total_useful_array_cycles_bytes\": " << s.total_useful_array_cycles << ",\n";
    out << "  \"total_bank_fill_cycles\": "   << s.total_bank_fill_cycles << ",\n";
    out << "  \"total_host_pack_cycles\": "   << s.total_host_pack_cycles << ",\n";
    out << "  \"total_host_mmio_cycles\": "   << s.total_host_mmio_cycles << ",\n";
    out << "  \"dma_bytes_act\": "            << s.dma_bytes_act << ",\n";
    out << "  \"dma_bytes_wgt\": "            << s.dma_bytes_wgt << ",\n";
    out << "  \"dma_bytes_out\": "            << s.dma_bytes_out << ",\n";
    out << "  \"persist_hits\": "             << s.persist_hits << ",\n";
    out << "  \"persist_misses\": "           << s.persist_misses << "\n";
    out << "}\n";
}

static void dump_trace(const Simulator& sim, std::ostream& out) {
    for (const auto& t : sim.trace()) {
        out << t.cycle << "\t" << event_name(t.type) << "\t"
            << t.dispatch_id << "\t" << t.aux << "\n";
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <realization> [--trace <path>] [--json] [--summary]\n"
                     "  realizations: stage_b_chain | gpt2_ffn | mb_style\n", argv[0]);
        return 1;
    }
    std::string realiz = argv[1];
    std::string trace_path;
    bool json = false;
    bool summary = true;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_path = argv[++i];
        } else if (std::strcmp(argv[i], "--json") == 0) {
            json = true;
        } else if (std::strcmp(argv[i], "--summary") == 0) {
            summary = true;
        } else if (std::strcmp(argv[i], "--no-summary") == 0) {
            summary = false;
        }
    }

    Arch arch;
    HostModel host;
    std::vector<Dispatch> dispatches;

    if (realiz == "stage_b_chain") {
        // Host-free: skip pack and mmio to match TB purely FPGA-event view.
        host.pack_us_per_dispatch = 0.0;
        host.mmio_overhead_us_per_dispatch = 0.0;
        dispatches = build_stage_b_two_chunk(1024, &arch);
    } else if (realiz == "gpt2_ffn") {
        arch = Arch{}; // defaults 16x16, MAX_K=1024, K_TILE=8
        host.pack_us_per_dispatch = 121.0;
        host.mmio_overhead_us_per_dispatch = 21.5;
        dispatches = build_chain_realization(512, 768, 3072, arch);
    } else if (realiz == "mb_style") {
        // Representative MB dispatch: N=16, K=384 (typical bert-tile), persist mostly hits.
        arch = Arch{};
        host.pack_us_per_dispatch = 8.7;   // MB per-dispatch host pack (from mb_full_reference.log)
        host.mmio_overhead_us_per_dispatch = 100.0; // includes usleep(76us) per dispatch
        dispatches = build_mb_style(1000, 384, 0.7);
    } else {
        std::fprintf(stderr, "unknown realization: %s\n", realiz.c_str());
        return 2;
    }

    bool enable_trace = !trace_path.empty();
    Simulator sim(arch, host, enable_trace);
    sim.load_dispatches(dispatches);

    auto t0 = std::chrono::steady_clock::now();
    cycle_t final_cycles = sim.run();
    auto t1 = std::chrono::steady_clock::now();
    double sim_wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (!trace_path.empty()) {
        std::ofstream f(trace_path);
        dump_trace(sim, f);
    }

    if (summary) {
        std::printf("== FlexNPU sim: %s ==\n", realiz.c_str());
        std::printf("  dispatches           = %llu\n", (unsigned long long)sim.stats().dispatches_run);
        std::printf("  events processed     = %llu\n", (unsigned long long)sim.stats().events_processed);
        std::printf("  simulated cycles     = %lld\n", (long long)final_cycles);
        std::printf("  simulated seconds    = %.6f\n", static_cast<double>(final_cycles)/arch.pl_clock_hz);
        std::printf("  sim wall (C++, ms)   = %.3f\n", sim_wall_ms);
        std::printf("  events/sec           = %.3g\n", sim.stats().events_processed/(sim_wall_ms*1e-3+1e-12));
        std::printf("  dma_bytes act/wgt/out= %llu / %llu / %llu\n",
                    (unsigned long long)sim.stats().dma_bytes_act,
                    (unsigned long long)sim.stats().dma_bytes_wgt,
                    (unsigned long long)sim.stats().dma_bytes_out);
        std::printf("  bank swaps wr/rd     = %llu / %llu\n",
                    (unsigned long long)sim.stats().bank_swaps_wr,
                    (unsigned long long)sim.stats().bank_swaps_rd);
        std::printf("  tile_done events     = %llu\n", (unsigned long long)sim.stats().tile_done_events);
        std::printf("  compute_done events  = %llu\n", (unsigned long long)sim.stats().compute_done_events);
        std::printf("  drain_done events    = %llu\n", (unsigned long long)sim.stats().drain_done_events);
        std::printf("  persist hits/misses  = %llu / %llu\n",
                    (unsigned long long)sim.stats().persist_hits,
                    (unsigned long long)sim.stats().persist_misses);
    }
    if (json) print_stats_json(sim, arch.pl_clock_hz, std::cout);
    return 0;
}
