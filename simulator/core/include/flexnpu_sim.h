// FlexNPU event-driven cycle-accurate simulator — public header.
// See DESIGN_MEMO.md for the timing contracts modeled here.
//
// Everything here is header-only for the MVP; resources are small structs
// with explicit state + on_event methods. main.cpp + test drivers include
// this header directly.

#pragma once

#include <cstdint>
#include <cstddef>
#include <queue>
#include <vector>
#include <string>
#include <functional>
#include <optional>
#include <map>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <deque>

namespace flexnpu_sim {

using cycle_t = int64_t;

enum class EventType {
    DISPATCH_BEGIN,
    HOST_PACK_DONE,
    DMA_ARM,
    DMA_TLAST,
    DMA_DONE,
    BUFS_READY,
    OP_START,
    COMPUTE_START,
    TILE_DONE,
    BANK_SWAP_WR,
    BANK_SWAP_RD,
    COMPUTE_DONE,
    DRAIN_START,
    DRAIN_DONE,
    OP_DONE,
    DISPATCH_END
};

inline const char* event_name(EventType t) {
    switch (t) {
        case EventType::DISPATCH_BEGIN:  return "DISPATCH_BEGIN";
        case EventType::HOST_PACK_DONE:  return "HOST_PACK_DONE";
        case EventType::DMA_ARM:         return "DMA_ARM";
        case EventType::DMA_TLAST:       return "DMA_TLAST";
        case EventType::DMA_DONE:        return "DMA_DONE";
        case EventType::BUFS_READY:      return "BUFS_READY";
        case EventType::OP_START:        return "OP_START";
        case EventType::COMPUTE_START:   return "COMPUTE_START";
        case EventType::TILE_DONE:       return "TILE_DONE";
        case EventType::BANK_SWAP_WR:    return "BANK_SWAP_WR";
        case EventType::BANK_SWAP_RD:    return "BANK_SWAP_RD";
        case EventType::COMPUTE_DONE:    return "COMPUTE_DONE";
        case EventType::DRAIN_START:     return "DRAIN_START";
        case EventType::DRAIN_DONE:      return "DRAIN_DONE";
        case EventType::OP_DONE:         return "OP_DONE";
        case EventType::DISPATCH_END:    return "DISPATCH_END";
    }
    return "?";
}

// Which AXIS stream (for DMA_* events).
enum class Stream { ACT, WGT, OUT };

inline const char* stream_name(Stream s) {
    switch (s) {
        case Stream::ACT: return "ACT";
        case Stream::WGT: return "WGT";
        case Stream::OUT: return "OUT";
    }
    return "?";
}

struct Event {
    cycle_t  cycle;
    uint64_t seq;           // tiebreaker for deterministic order at same cycle
    EventType type;
    int      dispatch_id;   // -1 if not applicable
    int      aux;           // per-event: stream_id / bank_id / pass_idx
};

// Reverse-order comparator: smallest cycle first, then smallest seq.
struct EventCmp {
    bool operator()(const Event& a, const Event& b) const {
        if (a.cycle != b.cycle) return a.cycle > b.cycle;
        return a.seq > b.seq;
    }
};

using EventQueue = std::priority_queue<Event, std::vector<Event>, EventCmp>;

// ============================================================
// Architecture parameters (RTL-derived unless marked)
// ============================================================

struct Arch {
    int      array_M = 16;
    int      array_N = 16;
    int      MAX_K   = 1024;
    int      K_TILE  = 8;
    int      AXIS_BEAT_BYTES = 16;
    double   pl_clock_hz     = 100e6;

    // Compute FSM cycle model (STAGE A/B RTL trace derived).
    int compute_setup_cycles      = 44;
    int compute_per_pass_cycles   = 101;
    int compute_tail_cycles       = 85;
    int compute_writeback_cycles  = 16;   // ARRAY_M rows drained (S_WRITEBACK)

    // ------------------------------------------------------------
    // OPT-IN SENSITIVITY KNOBS (all default to validated baseline).
    // Any deviation from default = 1.0 is IDEALIZED CAPABILITY MODE,
    // NOT a validated hardware mapping. Runs must be labelled accordingly.
    // ------------------------------------------------------------
    //
    // pass_overhead_scale ∈ [0, 1]: scales the NON-useful part of per-pass
    //   compute cycles. The "useful" part per pass is K_TILE (dense MAC).
    //   effective_per_pass = K_TILE + (compute_per_pass_cycles - K_TILE) * scale
    //   scale=1.0 → validated baseline (101 cyc/pass)
    //   scale=0.0 → ideal array-only floor (K_TILE=8 cyc/pass, all overhead removed)
    double pass_overhead_scale = 1.0;

    // wgt_reuse_ideal: idealized WGT residency across the FULL matmul (any
    //   (n0_group, k_chunk_idx) seen before ⇒ reuse). Represents ORG-3
    //   persistence-style capability. Bounded by tag SRAM in physical realizations.
    bool wgt_reuse_ideal = false;

    // wgt_reuse_local: TRAVERSAL-LOCAL WGT reuse. Reuse fires only when the
    //   IMMEDIATELY PREVIOUS dispatch shares (n0_group, k_chunk_idx) — i.e.
    //   the WGT bank is still warm from the last iteration. Requires only
    //   the existing ping-pong bank storage. Represents ORG-2's inner-loop
    //   reuse capability WITHOUT model-lifetime persistence.
    bool wgt_reuse_local = false;

    // Similarly for ACT (used only under ORG-2 traversal orders where
    // consecutive dispatches share (m0_group, k_chunk_idx)).
    bool act_reuse_local = false;

    // dispatch_group_size: host/runtime per-group amortization (idealized).
    //   Per-dispatch host_pack + host_mmio cost charged once per group.
    //   Different from command_queue_depth (below) which models a real
    //   producer/consumer resource. Value 1 = validated baseline.
    int dispatch_group_size = 1;

    // Ping-pong / DMA / handshake (RTL-derived).
    // With setup=2, K_beats=1024 → TLAST at cycle 0+2+1024-1=1025, matches
    // RTL trace Δ(op_start → TLAST) = 1025.
    int bank_fill_setup_cycles = 2;
    int bank_full_latency      = 1;

    // op_fsm state deltas (RTL trace).
    int opfsm_start_to_wait_in         = 4;
    int opfsm_ready_to_compute_start   = 2;
    int opfsm_done_to_op_done_suppress = 3;
    // Drain path: compute_done → OP_DRAIN (1) → out_reader_trigger + drain_ack
    // → OP_DRAIN_WAIT → OP_NEXT → OP_DONE. STAGE B TB observed 7 cycles total.
    // Physical S_WRITEBACK drain of ARRAY_M rows happens INSIDE compute cycles
    // (compute_writeback_cycles=16 above), not in this op_fsm drain path.
    int opfsm_compute_done_to_drain_start = 1;
    int drain_latency_cycles              = 1;  // out_reader_trigger + drain_ack (STAGE B minimal-ack)
    int opfsm_drain_done_to_op_done       = 5;  // DRAIN_WAIT → OP_NEXT → OP_DONE
};

// Host runtime model (physical measured, NOT hardware-cycle rules).
struct HostModel {
    double pack_us_per_dispatch     = 121.0;
    double mmio_overhead_us_per_dispatch = 21.5;

    // Pack cost decomposition (measured — see AUDIT_GROUPED_DISPATCH_REALIZABILITY.md).
    // MB pack (WGT_HOIST hitting) ≈ 8.7 µs/dispatch; GPT-2 chain pack ≈ 121 µs
    // (WGT_HOIST never fires). Implies wgt-half is ~93% of pack, act-half is ~7%.
    // When wgt_reuse_local fires, follower dispatches save the WGT half only.
    // This replaces the earlier (incorrect) 0.5× approximation.
    double wgt_pack_fraction = 0.925;    // wgt_pack ≈ 112 µs, act_pack ≈ 9 µs

    // OPT-IN sensitivity knobs (default preserves validated baseline):
    //   pack_scale, mmio_scale: multiplicative — reduce pack/mmio cost.
    //   pack_execute_overlap: enable pipeline overlap of host pack (dispatch i+1)
    //     with FPGA execution (dispatch i). See DESIGN_MEMO.md for the two-
    //     resource model used.
    //   command_queue_depth: bounded producer/consumer command queue depth.
    //     Depth 1 = serialized (ORG-0). Depth ∞ = pure overlap. Depth D>1
    //     with overlap models a real bounded FIFO between host and NPU.
    double pack_scale             = 1.0;
    double mmio_scale             = 1.0;
    bool   pack_execute_overlap   = false;
    int    command_queue_depth    = 1;   // 1 = serialized; INT_MAX = unbounded

    // Convert to cycles at the given PL clock.
    cycle_t pack_cycles(double pl_hz) const {
        return static_cast<cycle_t>((pack_us_per_dispatch * pack_scale * 1e-6) * pl_hz + 0.5);
    }
    cycle_t mmio_cycles(double pl_hz) const {
        return static_cast<cycle_t>((mmio_overhead_us_per_dispatch * mmio_scale * 1e-6) * pl_hz + 0.5);
    }
};

// Persistence policy (per dispatch)
enum class PersistPolicy { OFF, FIX_B_HIT, FIX_B_MISS_FILL };

// Per-dispatch descriptor (produced by the realization layer for now).
struct Dispatch {
    int   id;
    int   K;              // K bytes streamed per dispatch (in ACT/WGT dims)
    int   K_beats;        // ACT beats = WGT beats = K (with K_TILE align)
    bool  wb_suppress;    // 1 for FIRST/MIDDLE, 0 for LAST
    bool  acc_in;         // 0 for FIRST, 1 for MIDDLE/LAST
    bool  is_last;        // triggers S2MM output drain
    PersistPolicy persist = PersistPolicy::OFF;
    // If persist=FIX_B_HIT, no DMA WGT stream fires (WGT bank retained).
    // If persist=FIX_B_MISS_FILL, DMA WGT stream fires as normal.
    // WGT reuse context (used by wgt_reuse_ideal/local mode).
    int   n0_group_id = -1;
    int   k_chunk_idx = -1;
    // ACT reuse context (used by act_reuse_local mode). m0_group_id
    // groups dispatches sharing an ACT panel (row range).
    int   m0_group_id = -1;
};

// ============================================================
// Modeled state per resource
// ============================================================

struct PingPongState {
    int  wr_bank = 0;
    int  rd_bank = 0;
    int  bank_full[2] = {0, 0};
    int  tile_count = 0;
    int  tile_total = 0;    // set at compute start (K/K_TILE)
    int  wr_addr = 0;
    bool persist_active = false;
    int  slot_valid[2] = {0, 0};
    uint64_t swaps_wr = 0;
    uint64_t swaps_rd = 0;

    // Reset atomic (equivalent of matmul_reset_pulse when persist=1).
    void matmul_reset() {
        wr_bank = 0; rd_bank = 0;
        bank_full[0] = bank_full[1] = 0;
        tile_count = 0;
        wr_addr = 0;
        slot_valid[0] = slot_valid[1] = 0;
    }
};

struct ComputeState {
    bool busy = false;
    bool latched_acc_in = false;
    bool latched_wb_en  = true;
    int  passes_scheduled = 0;
    int  passes_completed = 0;

    // Counters for stats
    uint64_t total_useful_cycles = 0;   // K_TILE * num_passes (dense array work)
    uint64_t total_fpga_cycles   = 0;   // compute+drain per dispatch
};

struct OpFSMState {
    enum S { IDLE, TILE_WAIT_IN, COMPUTE, COMPUTE_WAIT, DRAIN, DRAIN_WAIT, NEXT, DONE };
    S state = IDLE;
    bool r_writeback_suppress = false;
    int  tiles_completed = 0;
    int  r_tile_count = 0;
};

struct HostState {
    bool busy = false;
    cycle_t last_op_done = 0;
    int    next_dispatch_idx = 0;
    uint64_t total_pack_cycles = 0;
    uint64_t total_mmio_cycles = 0;
};

struct SimStats {
    uint64_t events_processed = 0;
    cycle_t  last_event_cycle = 0;
    uint64_t total_useful_array_cycles = 0;
    uint64_t total_fpga_active_cycles = 0;  // sum of compute+writeback+drain per dispatch
    uint64_t total_bank_fill_cycles = 0;
    uint64_t total_host_pack_cycles = 0;
    uint64_t total_host_mmio_cycles = 0;
    uint64_t dma_bytes_act = 0;
    uint64_t dma_bytes_wgt = 0;
    uint64_t dma_bytes_out = 0;
    uint64_t dispatches_run = 0;
    uint64_t bank_swaps_wr = 0;
    uint64_t bank_swaps_rd = 0;
    uint64_t tile_done_events = 0;
    uint64_t compute_done_events = 0;
    uint64_t drain_done_events = 0;
    uint64_t persist_hits = 0;
    uint64_t persist_misses = 0;

    cycle_t wall_cycles() const { return last_event_cycle; }
    double  wall_seconds(double pl_hz) const { return static_cast<double>(last_event_cycle) / pl_hz; }
};

// ============================================================
// Trace (for debugging + RTL comparison)
// ============================================================

struct TraceEntry {
    cycle_t cycle;
    EventType type;
    int dispatch_id;
    int aux;
};

// ============================================================
// Simulator
// ============================================================

class Simulator {
public:
    Simulator(Arch arch, HostModel host, bool enable_trace = false)
        : arch_(arch), host_(host), enable_trace_(enable_trace) {
        act_pp_.tile_total = 0;
        wgt_pp_.tile_total = 0;
    }

    void load_dispatches(std::vector<Dispatch> ds) {
        dispatches_ = std::move(ds);
    }

    // Run to completion. Returns the last event cycle.
    cycle_t run() {
        if (dispatches_.empty()) return 0;
        // Seed with first DISPATCH_BEGIN at cycle 0.
        schedule(0, EventType::DISPATCH_BEGIN, dispatches_[0].id, 0);
        while (!eq_.empty()) {
            Event e = eq_.top();
            eq_.pop();
            if (e.cycle < clock_) {
                // Sanity: never move backwards.
                std::fprintf(stderr, "[sim] backward event cycle=%lld < clock=%lld type=%s\n",
                             (long long)e.cycle, (long long)clock_, event_name(e.type));
                assert(false);
            }
            clock_ = e.cycle;
            stats_.events_processed++;
            stats_.last_event_cycle = e.cycle;
            if (enable_trace_) trace_.push_back({e.cycle, e.type, e.dispatch_id, e.aux});
            handle(e);
        }
        return clock_;
    }

    const SimStats& stats() const { return stats_; }
    const std::vector<TraceEntry>& trace() const { return trace_; }
    const Arch& arch() const { return arch_; }

private:
    Arch      arch_;
    HostModel host_;
    bool      enable_trace_;

    EventQueue eq_;
    cycle_t    clock_ = 0;
    uint64_t   seq_counter_ = 0;

    std::vector<Dispatch> dispatches_;
    HostState     host_state_;
    PingPongState act_pp_;
    PingPongState wgt_pp_;
    ComputeState  compute_;
    OpFSMState    opfsm_;
    SimStats      stats_;
    std::vector<TraceEntry> trace_;

    // Cross-resource latch buffers (per dispatch)
    int  current_dispatch_idx_ = -1;
    bool wgt_dma_active_ = true;   // false when persist FIX_B_HIT skips WGT stream
    bool act_ready_ = false;
    bool wgt_ready_ = false;

    // Sensitivity-mode state:
    //   host_free_at_ : when host resource is free to start next pack (overlap mode)
    //   fpga_free_at_ : when FPGA resource is free to start next op (overlap mode)
    //   prev_dispatch_n0_group_ / k_chunk_idx_ : for wgt_reuse_ideal book-keeping
    cycle_t host_free_at_ = 0;
    cycle_t fpga_free_at_ = 0;
    int     prev_n0_group_ = -1;
    int     prev_k_chunk_idx_ = -1;
    int     prev_m0_group_ = -1;
    int     prev_k_chunk_idx_for_act_ = -1;
    int     dispatch_group_counter_ = 0;   // for dispatch_group_size amortization
    // For wgt_reuse_ideal: set of already-seen (n0_group, k_chunk_idx) pairs.
    // Represents ORG-3 persistence: full-lifetime residency.
    std::map<int, std::map<int, bool>> wgt_seen_;
    uint64_t dma_skipped_act_ = 0;   // ORG-2 traversal-local ACT reuse counter

    // Bounded command queue: ring of the OP_DONE times of the last D
    // dispatches. When full, the next pack must wait for the oldest to
    // complete FPGA — that dequeues one slot.
    std::deque<cycle_t> fpga_op_done_ring_;
    uint64_t queue_stall_events_ = 0;
    uint64_t queue_starve_events_ = 0;

    // Additional per-dispatch stats (used by decomposition)
    uint64_t total_useful_mac_cycles_ = 0;    // K_TILE * num_passes summed
    uint64_t total_pass_overhead_cycles_ = 0; // (per_pass_effective - K_TILE) * num_passes
    uint64_t total_setup_tail_cycles_ = 0;    // setup + tail per dispatch
    uint64_t total_writeback_cycles_ = 0;
    uint64_t total_drain_handshake_cycles_ = 0;
    uint64_t dma_skipped_wgt_ = 0;             // wgt_reuse_ideal counter

    void schedule(cycle_t at_cycle, EventType type, int dispatch_id, int aux) {
        Event e;
        e.cycle = at_cycle;
        e.seq = seq_counter_++;
        e.type = type;
        e.dispatch_id = dispatch_id;
        e.aux = aux;
        eq_.push(e);
    }

    const Dispatch& disp_by_id(int id) const {
        for (auto& d : dispatches_) if (d.id == id) return d;
        assert(false && "dispatch id not found");
        return dispatches_[0];
    }

    void handle(const Event& e) {
        switch (e.type) {
            case EventType::DISPATCH_BEGIN:  on_dispatch_begin(e);  break;
            case EventType::HOST_PACK_DONE:  on_host_pack_done(e);  break;
            case EventType::DMA_ARM:         on_dma_arm(e);         break;
            case EventType::DMA_TLAST:       on_dma_tlast(e);       break;
            case EventType::DMA_DONE:        on_dma_done(e);        break;
            case EventType::BUFS_READY:      on_bufs_ready(e);      break;
            case EventType::OP_START:        on_op_start(e);        break;
            case EventType::COMPUTE_START:   on_compute_start(e);   break;
            case EventType::TILE_DONE:       on_tile_done(e);       break;
            case EventType::BANK_SWAP_WR:    on_bank_swap_wr(e);    break;
            case EventType::BANK_SWAP_RD:    on_bank_swap_rd(e);    break;
            case EventType::COMPUTE_DONE:    on_compute_done(e);    break;
            case EventType::DRAIN_START:     on_drain_start(e);     break;
            case EventType::DRAIN_DONE:      on_drain_done(e);      break;
            case EventType::OP_DONE:         on_op_done(e);         break;
            case EventType::DISPATCH_END:    on_dispatch_end(e);    break;
        }
    }

    void on_dispatch_begin(const Event& e) {
        // Host starts packing this dispatch's ACT/WGT.
        // Sensitivity modes:
        //   * wgt_reuse_ideal: if same (n0_group, k_chunk_idx) as previous
        //     dispatch, WGT pack is elided (pack cost reduced proportionally).
        //   * dispatch_group_size > 1: pack + mmio cost amortized over G
        //     consecutive dispatches (only counted once per group).
        //   * pack_execute_overlap: host resource is a separate timeline;
        //     pack_start = max(e.cycle, host_free_at_).
        const auto& d = disp_by_id(e.dispatch_id);
        current_dispatch_idx_ = e.dispatch_id;

        // Determine whether this dispatch's pack is elided by WGT reuse:
        // this is a REDUCTION in pack cost proportional to WGT/total contribution.
        // pack_op_strip in runtime does ACT + WGT together; ACT pack is O(M*K),
        // WGT pack is O(N*K). For 16x16, we split 50/50 for the reuse bound.
        double pack_scale_effective = 1.0;
        bool wgt_reused_this_dispatch = false;
        // WGT reuse decision. Precedence: local > ideal (if both set,
        // local applies as the tighter capability).
        //
        // wgt_reuse_local (ORG-2 capability): reuse iff the IMMEDIATELY
        //   PREVIOUS dispatch shares (n0_group, k_chunk_idx). Requires only
        //   the existing ping-pong bank remaining warm; no persistent SRAM.
        //
        // wgt_reuse_ideal (ORG-3 capability): reuse iff (n0_group, k_chunk_idx)
        //   has EVER been observed before. Requires SRAM sized to the
        //   working set of unique (n0_group, k_chunk_idx) pairs.
        // Corrected reuse cost model (see AUDIT_GROUPED_DISPATCH_REALIZABILITY.md).
        // Real WGT_HOIST in the runtime (pack_op_strip's static cache) skips
        // pack_wgt_tile ENTIRELY on hits, saving ~93% of the total pack cost —
        // not 50% as the earlier heuristic assumed.
        double pack_wgt_frac = host_.wgt_pack_fraction;   // e.g., 0.925
        double pack_act_frac = 1.0 - pack_wgt_frac;       // e.g., 0.075
        if (arch_.wgt_reuse_local) {
            int n0_group = d.n0_group_id;
            int k_chunk_idx = d.k_chunk_idx;
            if (n0_group == prev_n0_group_ && k_chunk_idx == prev_k_chunk_idx_
                && prev_n0_group_ >= 0) {
                // WGT_HOIST hit: pay only act_pack (~9 µs), skip wgt_pack (~112 µs).
                pack_scale_effective = pack_act_frac;
                wgt_reused_this_dispatch = true;
                dma_skipped_wgt_++;
            }
            prev_n0_group_    = n0_group;
            prev_k_chunk_idx_ = k_chunk_idx;
        } else if (arch_.wgt_reuse_ideal) {
            int n0_group = d.n0_group_id;
            int k_chunk_idx = d.k_chunk_idx;
            auto& row = wgt_seen_[n0_group];
            if (row.count(k_chunk_idx)) {
                pack_scale_effective = pack_act_frac;
                wgt_reused_this_dispatch = true;
                dma_skipped_wgt_++;
            } else {
                row[k_chunk_idx] = true;
            }
        }

        // ACT reuse — mirror of WGT reuse. If ACT reused: subtract act_frac
        // from pack_scale_effective (pay only pack_wgt cost). Correct
        // physically-grounded model — see AUDIT_GROUPED_DISPATCH_REALIZABILITY.md.
        bool act_reused_this_dispatch = false;
        if (arch_.act_reuse_local) {
            int m0_group = d.m0_group_id;
            int k_chunk_idx = d.k_chunk_idx;
            if (m0_group == prev_m0_group_ && k_chunk_idx == prev_k_chunk_idx_for_act_
                && prev_m0_group_ >= 0) {
                pack_scale_effective = std::max(0.0, pack_scale_effective - pack_act_frac);
                act_reused_this_dispatch = true;
                dma_skipped_act_++;
            }
            prev_m0_group_ = m0_group;
            prev_k_chunk_idx_for_act_ = k_chunk_idx;
        }

        // Dispatch grouping: amortize MMIO ONLY (not pack) across a group.
        // This reflects the physical macro-op path: one macro submission
        // shares one dma_reset + reg-write + arm + kick MMIO burst across
        // its constituent tiles. Pack is NOT amortized because ACT
        // differs per tile — the host must construct each tile's ACT
        // layout individually (see runtime pack_op_strip).
        //
        // If a workload can pre-pack A/B once per inference (e.g. via a
        // model-load preprocessing step or on-chip layout hardware),
        // it must set pack_scale directly rather than expecting
        // dispatch_group_size to elide pack.
        bool group_leader = (dispatch_group_counter_ == 0);
        dispatch_group_counter_++;
        if (dispatch_group_counter_ >= arch_.dispatch_group_size) {
            dispatch_group_counter_ = 0;
        }

        // Pack ALWAYS runs (per dispatch); it is NOT gated by group_leader.
        cycle_t pack_c = static_cast<cycle_t>(host_.pack_cycles(arch_.pl_clock_hz) * pack_scale_effective + 0.5);

        cycle_t arrival = e.cycle;
        cycle_t pack_start;
        if (host_.pack_execute_overlap) {
            pack_start = std::max(arrival, host_free_at_);
            // Bounded queue: if the queue is at depth D and we would enqueue
            // another dispatch, we must wait until the OLDEST in-flight
            // dispatch's OP_DONE clears a slot.
            int D = host_.command_queue_depth;
            if (D >= 1 && (int)fpga_op_done_ring_.size() >= D) {
                cycle_t oldest_done = fpga_op_done_ring_.front();
                if (pack_start < oldest_done) {
                    queue_stall_events_++;
                    pack_start = oldest_done;
                }
                fpga_op_done_ring_.pop_front();
            }
        } else {
            pack_start = arrival;
        }
        cycle_t pack_done = pack_start + pack_c;
        host_free_at_ = pack_done;  // host busy until pack done
        stats_.total_host_pack_cycles += pack_c;
        host_state_.busy = true;

        // Store per-dispatch state via aux — reuse flag encoded in aux.
        // Aux bit 0 : group_leader
        // Aux bit 1 : wgt_reused (skip WGT DMA)
        // Aux bit 2 : act_reused (skip ACT DMA — traversal-local ACT reuse)
        int aux = (group_leader ? 1 : 0)
                | (wgt_reused_this_dispatch ? 2 : 0)
                | (act_reused_this_dispatch ? 4 : 0);
        schedule(pack_done, EventType::HOST_PACK_DONE, d.id, aux);
    }

    void on_host_pack_done(const Event& e) {
        // Host writes MMIO: reset DMAs + reg writes + kick op_start.
        // Model as fixed host MMIO overhead then OP_START + DMA_ARM (both streams).
        const auto& d = disp_by_id(e.dispatch_id);
        bool group_leader = (e.aux & 1) != 0;
        bool wgt_reused = (e.aux & 2) != 0;
        bool act_reused = (e.aux & 4) != 0;
        cycle_t mmio_c = host_.mmio_cycles(arch_.pl_clock_hz);
        if (!group_leader) mmio_c = 0;   // amortize per-group
        stats_.total_host_mmio_cycles += mmio_c;

        // Now host mmio also uses the host resource sequentially.
        cycle_t mmio_start = e.cycle;
        cycle_t after_mmio = mmio_start + mmio_c;
        // Update host free time (mmio done)
        host_free_at_ = std::max(host_free_at_, after_mmio);

        // In overlap mode, kick off the NEXT dispatch's pack ASAP (concurrent
        // with FPGA execution of this dispatch).
        if (host_.pack_execute_overlap) {
            int next_idx = host_state_.next_dispatch_idx + 1;
            if (next_idx < static_cast<int>(dispatches_.size())) {
                // Mark that we've pre-scheduled it; on_dispatch_end will skip.
                host_state_.next_dispatch_idx = next_idx;
                schedule(after_mmio, EventType::DISPATCH_BEGIN,
                         dispatches_[next_idx].id, 0);
            }
        }

        // FPGA start: gated by fpga_free_at_ (for overlap) or immediate (for serial)
        cycle_t op_start_at = after_mmio;
        if (host_.pack_execute_overlap) {
            op_start_at = std::max(after_mmio, fpga_free_at_);
        }
        // Eagerly RESERVE the FPGA for this dispatch's full path so that
        // subsequent dispatches whose HOST_PACK_DONE fires before this
        // dispatch's OP_DONE cannot double-book the compute resource.
        // Path = bank-fill wait + op_fsm handshake + compute + drain.
        {
            int K_TILE = arch_.K_TILE;
            int per_pass_baseline = arch_.compute_per_pass_cycles;
            int per_pass_overhead = per_pass_baseline - K_TILE;
            int per_pass_eff = K_TILE + static_cast<int>(std::round(per_pass_overhead * arch_.pass_overhead_scale));
            if (per_pass_eff < 1) per_pass_eff = 1;
            int num_passes = d.K / K_TILE;
            // Bank fill: DMA_ARM(op_start_at) → DMA_TLAST at +setup+K_beats-1 →
            //            BANK_SWAP_WR at +1 → BUFS_READY at same → COMPUTE_START
            //            at +opfsm_ready_to_compute_start.
            // If BOTH ACT and WGT banks are already warm (traversal-local
            // reuse), bank_fill is skipped entirely.
            bool both_reused = wgt_reused && act_reused;
            cycle_t bank_fill_cyc = both_reused ? 0
                                  : (arch_.bank_fill_setup_cycles + d.K_beats - 1
                                    + arch_.bank_full_latency
                                    + arch_.opfsm_ready_to_compute_start);
            cycle_t compute_cyc = arch_.compute_setup_cycles
                                + static_cast<cycle_t>(num_passes) * per_pass_eff
                                + arch_.compute_tail_cycles
                                + (d.wb_suppress ? 0 : arch_.compute_writeback_cycles);
            // Drain path for LAST adds op_fsm overhead too.
            cycle_t drain_cyc = d.wb_suppress
                ? arch_.opfsm_done_to_op_done_suppress
                : (arch_.opfsm_compute_done_to_drain_start
                   + arch_.drain_latency_cycles
                   + arch_.opfsm_drain_done_to_op_done);
            cycle_t total_fpga_cyc = bank_fill_cyc + compute_cyc + drain_cyc;
            // If FPGA already busy past op_start_at, push op_start_at.
            op_start_at = std::max(op_start_at, fpga_free_at_);
            fpga_free_at_ = op_start_at + total_fpga_cyc;
            // Enqueue this dispatch's OP_DONE time so the bounded queue can
            // gate the future host pack start. Only meaningful under overlap.
            if (host_.pack_execute_overlap) {
                fpga_op_done_ring_.push_back(fpga_free_at_);
            }
        }

        // Fire DMA_ARM and OP_START at op_start_at
        // Skip ACT DMA if ACT reused (traversal-local; consecutive dispatch
        // shares (m0_group, k_chunk_idx) so ACT bank is still warm).
        if (!act_reused) {
            schedule(op_start_at, EventType::DMA_ARM, d.id, static_cast<int>(Stream::ACT));
        } else {
            // Force ACT bank ready to model bank-still-warm.
            act_pp_.bank_full[act_pp_.rd_bank] = 1;
            act_ready_ = true;
        }
        // wgt_dma_active_ is now local per-dispatch: skip WGT DMA if reused
        // or if persist=FIX_B_HIT.
        bool wgt_dma_this = (d.persist != PersistPolicy::FIX_B_HIT) && !wgt_reused;
        if (wgt_dma_this) {
            schedule(op_start_at, EventType::DMA_ARM, d.id, static_cast<int>(Stream::WGT));
        }
        // Track persistence counters
        if (d.persist == PersistPolicy::FIX_B_HIT) stats_.persist_hits++;
        else if (d.persist == PersistPolicy::FIX_B_MISS_FILL) stats_.persist_misses++;
        // op_start kick fires at same cycle as DMA_ARM (runtime writes are close).
        schedule(op_start_at, EventType::OP_START, d.id, 0);
        // Store wgt_reused hint for on_op_start (fake bank_full for WGT if reused)
        if (wgt_reused) {
            // Force WGT ping-pong ready for compute — modeled like FIX_B_HIT
            wgt_pp_.bank_full[wgt_pp_.rd_bank] = 1;
            wgt_ready_ = true;
        }
        host_state_.busy = false;
    }

    void on_dma_arm(const Event& e) {
        // Model DMA as: after setup + K_beats*1cycle, TLAST fires; +1 cycle DMA_DONE.
        const auto& d = disp_by_id(e.dispatch_id);
        Stream s = static_cast<Stream>(e.aux);
        cycle_t tlast_at = e.cycle + arch_.bank_fill_setup_cycles + d.K_beats - 1;
        schedule(tlast_at, EventType::DMA_TLAST, d.id, e.aux);
        // Count bytes
        uint64_t bytes = static_cast<uint64_t>(d.K_beats) * arch_.AXIS_BEAT_BYTES;
        if      (s == Stream::ACT) stats_.dma_bytes_act += bytes;
        else if (s == Stream::WGT) stats_.dma_bytes_wgt += bytes;
        else                       stats_.dma_bytes_out += bytes;
        stats_.total_bank_fill_cycles += static_cast<uint64_t>(d.K_beats);
    }

    void on_dma_tlast(const Event& e) {
        // TLAST → bank_full[wr]<=1 next cycle → BANK_SWAP_WR.
        Stream s = static_cast<Stream>(e.aux);
        cycle_t bank_full_at = e.cycle + arch_.bank_full_latency;
        schedule(bank_full_at, EventType::BANK_SWAP_WR, e.dispatch_id, e.aux);
        // For MM2S streams also emit DMA_DONE at bank_full_at.
        if (s != Stream::OUT) schedule(bank_full_at, EventType::DMA_DONE, e.dispatch_id, e.aux);
    }

    void on_dma_done(const Event& e) {
        // No-op accounting: DMA_DONE per stream (already counted).
        (void)e;
    }

    void on_bank_swap_wr(const Event& e) {
        Stream s = static_cast<Stream>(e.aux);
        PingPongState& pp = (s == Stream::ACT) ? act_pp_ : wgt_pp_;
        pp.bank_full[pp.wr_bank] = 1;
        int old_bank = pp.wr_bank;
        pp.wr_bank = 1 - pp.wr_bank;
        pp.swaps_wr++;
        stats_.bank_swaps_wr++;

        // Non-persist: rd_bank stays pointing at old_bank until 128 tile_done_pulses.
        // Signal buf_ready condition.
        bool this_ready = (pp.bank_full[pp.rd_bank] == 1);
        if (s == Stream::ACT) act_ready_ = this_ready;
        else                  wgt_ready_ = this_ready;

        // If persist=FIX_B_HIT for WGT, wgt_ready_ was already true (bank retained).
        // For ACT never persist.

        // Check if both are now ready → schedule BUFS_READY.
        if (act_ready_ && wgt_ready_) {
            schedule(e.cycle, EventType::BUFS_READY, e.dispatch_id, 0);
        }
    }

    void on_bank_swap_rd(const Event& e) {
        Stream s = static_cast<Stream>(e.aux);
        PingPongState& pp = (s == Stream::ACT) ? act_pp_ : wgt_pp_;
        pp.bank_full[pp.rd_bank] = 0;
        pp.rd_bank = 1 - pp.rd_bank;
        pp.tile_count = 0;
        pp.swaps_rd++;
        stats_.bank_swaps_rd++;
    }

    void on_op_start(const Event& e) {
        // OP_IDLE → OP_TILE_WAIT_IN at +opfsm_start_to_wait_in
        cycle_t at = e.cycle + arch_.opfsm_start_to_wait_in;
        // Latch chain flags: sample from the dispatch descriptor.
        const auto& d = disp_by_id(e.dispatch_id);
        opfsm_.r_writeback_suppress = d.wb_suppress;
        opfsm_.state = OpFSMState::TILE_WAIT_IN;
        // Configure ping-pong tile_total for this dispatch (K/K_TILE)
        int tile_total = d.K / arch_.K_TILE;
        act_pp_.tile_total = tile_total;
        wgt_pp_.tile_total = tile_total;
        // WGT persist behavior (Fix-B model). This replicates the RTL
        // wgt_wr_bank_select + slot_cmd contract seen in ping_pong_buffer.v.
        if (d.persist == PersistPolicy::FIX_B_HIT) {
            // Runtime writes REUSE_x → rd_bank routed to persistent bank.
            // Bank is already full from a prior FIX_B_MISS_FILL.
            wgt_pp_.persist_active = true;
            wgt_pp_.bank_full[wgt_pp_.rd_bank] = 1;
            wgt_ready_ = true;
        } else if (d.persist == PersistPolicy::FIX_B_MISS_FILL) {
            // Runtime writes wr_bank_select + NORMAL cmd. Route rd_bank at
            // the target bank; clear its bank_full so incoming DMA fills it.
            wgt_pp_.persist_active = true;
            wgt_pp_.rd_bank = wgt_pp_.wr_bank;
            wgt_pp_.bank_full[wgt_pp_.wr_bank] = 0;
            wgt_ready_ = false;
        } else {
            wgt_pp_.persist_active = false;
            wgt_ready_ = (wgt_pp_.bank_full[wgt_pp_.rd_bank] == 1);
        }
        act_ready_ = (act_pp_.bank_full[act_pp_.rd_bank] == 1);
        // If already ready, we can fire BUFS_READY when WAIT_IN entered.
        if (act_ready_ && wgt_ready_) {
            schedule(at, EventType::BUFS_READY, d.id, 0);
        }
    }

    void on_bufs_ready(const Event& e) {
        // Do not gate on opfsm_.state — in overlap mode the shared state
        // register may be transiently set by a later dispatch. Schedule
        // COMPUTE_START unconditionally (guarded by fpga_free_at_ reservation).
        opfsm_.state = OpFSMState::COMPUTE;
        cycle_t at = e.cycle + arch_.opfsm_ready_to_compute_start;
        schedule(at, EventType::COMPUTE_START, e.dispatch_id, 0);
    }

    void on_compute_start(const Event& e) {
        // Compute FSM begins.
        const auto& d = disp_by_id(e.dispatch_id);
        compute_.busy = true;
        compute_.latched_acc_in = d.acc_in;
        compute_.latched_wb_en  = !d.wb_suppress;
        int num_passes = d.K / arch_.K_TILE;
        compute_.passes_scheduled = num_passes;
        compute_.passes_completed = 0;

        // pass_overhead_scale: split per-pass into useful (K_TILE) + overhead
        // Effective per-pass cycles under the sensitivity scale.
        int K_TILE = arch_.K_TILE;
        int per_pass_baseline = arch_.compute_per_pass_cycles;
        int per_pass_overhead = per_pass_baseline - K_TILE;
        // Round to int so cycle math stays integral.
        int per_pass_eff = K_TILE + static_cast<int>(std::round(per_pass_overhead * arch_.pass_overhead_scale));
        if (per_pass_eff < 1) per_pass_eff = 1;

        // Schedule TILE_DONE events. tile_done_pulse i fires at
        // compute_start + setup - 1 + i * per_pass_eff  (i=1..num_passes)
        cycle_t base = e.cycle + arch_.compute_setup_cycles - 1;
        for (int i = 1; i <= num_passes; ++i) {
            schedule(base + static_cast<cycle_t>(i) * per_pass_eff,
                     EventType::TILE_DONE, d.id, i);
        }
        // Compute + tail + optional writeback → COMPUTE_DONE
        cycle_t compute_end = e.cycle + arch_.compute_setup_cycles +
                              static_cast<cycle_t>(num_passes) * per_pass_eff +
                              arch_.compute_tail_cycles;
        cycle_t writeback_cyc = 0;
        if (compute_.latched_wb_en) writeback_cyc = arch_.compute_writeback_cycles;
        cycle_t compute_done_at = compute_end + writeback_cyc;
        schedule(compute_done_at, EventType::COMPUTE_DONE, d.id, 0);
        // Track useful array cycles (dense MAC work): K_TILE * num_passes = K
        compute_.total_useful_cycles += static_cast<uint64_t>(d.K);
        total_useful_mac_cycles_    += static_cast<uint64_t>(K_TILE) * num_passes;
        total_pass_overhead_cycles_ += static_cast<uint64_t>(per_pass_eff - K_TILE) * num_passes;
        total_setup_tail_cycles_    += static_cast<uint64_t>(arch_.compute_setup_cycles) + arch_.compute_tail_cycles;
        total_writeback_cycles_     += static_cast<uint64_t>(writeback_cyc);
        compute_.total_fpga_cycles += static_cast<uint64_t>(compute_done_at - e.cycle);
    }

    void on_tile_done(const Event& e) {
        stats_.tile_done_events++;
        // Ping-pong sides: on tile_done, increment tile_count; if tile_count == tile_total,
        // swap rd_bank (non-persist only). Both ACT and WGT share this pulse.
        auto step = [&](PingPongState& pp, Stream s) {
            pp.tile_count++;
            if (pp.tile_count == pp.tile_total) {
                // wrap
                pp.tile_count = 0;
                if (!pp.persist_active) {
                    schedule(e.cycle, EventType::BANK_SWAP_RD, e.dispatch_id, static_cast<int>(s));
                }
            }
        };
        step(act_pp_, Stream::ACT);
        step(wgt_pp_, Stream::WGT);
        compute_.passes_completed++;
    }

    void on_compute_done(const Event& e) {
        stats_.compute_done_events++;
        compute_.busy = false;
        opfsm_.state = OpFSMState::COMPUTE_WAIT;
        // Now op_fsm branches on suppress vs drain.
        // Read from the DISPATCH descriptor rather than the shared opfsm
        // register, so overlap-mode with in-flight dispatches from different
        // (wb_suppress) categories doesn't corrupt the branch decision.
        const auto& d = disp_by_id(e.dispatch_id);
        if (d.wb_suppress) {
            // Suppress: OP_NEXT → OP_DONE at +opfsm_done_to_op_done_suppress
            schedule(e.cycle + arch_.opfsm_done_to_op_done_suppress,
                     EventType::OP_DONE, e.dispatch_id, 0);
        } else {
            // Drain path: OP_DRAIN fires out_reader_trigger next cycle
            schedule(e.cycle + arch_.opfsm_compute_done_to_drain_start,
                     EventType::DRAIN_START, e.dispatch_id, 0);
        }
        // Account FPGA-active cycles per dispatch (compute + writeback)
        // Already counted via compute_.total_fpga_cycles.
    }

    void on_drain_start(const Event& e) {
        opfsm_.state = OpFSMState::DRAIN;
        // The physical drain of ARRAY_M output rows happens INSIDE compute's
        // S_WRITEBACK state (already accounted for in compute_writeback_cycles).
        // Here we only model the op_fsm handshake with axis_out_reader.
        cycle_t drain_done_at = e.cycle + arch_.drain_latency_cycles;
        // S2MM DMA bytes for OUT (ARRAY_M rows * ARRAY_N lanes * 4 bytes)
        uint64_t out_bytes = static_cast<uint64_t>(arch_.array_M) * arch_.array_N * 4;
        stats_.dma_bytes_out += out_bytes;
        schedule(drain_done_at, EventType::DRAIN_DONE, e.dispatch_id, 0);
    }

    void on_drain_done(const Event& e) {
        stats_.drain_done_events++;
        opfsm_.state = OpFSMState::DRAIN_WAIT;
        // opfsm handles remaining OP_DRAIN_WAIT → OP_NEXT → OP_DONE
        schedule(e.cycle + arch_.opfsm_drain_done_to_op_done,
                 EventType::OP_DONE, e.dispatch_id, 0);
    }

    void on_op_done(const Event& e) {
        opfsm_.state = OpFSMState::IDLE;
        opfsm_.tiles_completed++;
        stats_.dispatches_run++;
        // FPGA free at op_done cycle.
        // In overlap mode, fpga_free_at_ is pre-reserved in on_host_pack_done
        // for future dispatches. Only monotonically advance here.
        if (e.cycle > fpga_free_at_) fpga_free_at_ = e.cycle;
        host_state_.last_op_done = e.cycle;
        schedule(e.cycle, EventType::DISPATCH_END, e.dispatch_id, 0);
    }

    void on_dispatch_end(const Event& e) {
        // Advance to next dispatch (host is serial).
        // In overlap mode, on_host_pack_done already pre-schedules the next
        // DISPATCH_BEGIN, so we must not double-schedule here.
        if (host_.pack_execute_overlap) return;
        host_state_.next_dispatch_idx++;
        if (host_state_.next_dispatch_idx < static_cast<int>(dispatches_.size())) {
            const auto& nd = dispatches_[host_state_.next_dispatch_idx];
            schedule(e.cycle, EventType::DISPATCH_BEGIN, nd.id, 0);
        }
    }
};

// ============================================================
// Realization helpers (small; RSem will supersede later)
// ============================================================

// Build a list of dispatches for a chained matmul.
// non_persist = true means chain path (persist off).
inline std::vector<Dispatch> build_chain_realization(
        int M, int N, int K,
        const Arch& arch,
        bool non_persist = true) {
    std::vector<Dispatch> ds;
    int Mt = M / arch.array_M;
    int Nt = N / arch.array_N;
    int Kchunks = (K + arch.MAX_K - 1) / arch.MAX_K;
    int per_chunk_K = arch.MAX_K;
    int id = 0;
    // Loop order: for m0 in Mt: for n0 in Nt: for c in Kchunks
    for (int m0 = 0; m0 < Mt; ++m0) {
        for (int n0 = 0; n0 < Nt; ++n0) {
            for (int c = 0; c < Kchunks; ++c) {
                Dispatch d;
                d.id = id++;
                int klen = std::min(per_chunk_K, K - c * per_chunk_K);
                int k_fpga = ((klen + arch.K_TILE - 1) / arch.K_TILE) * arch.K_TILE;
                d.K = k_fpga;
                d.K_beats = k_fpga; // 1 beat per K in the AXIS packing
                d.wb_suppress = (c + 1 != Kchunks);
                d.acc_in      = (c != 0);
                d.is_last     = (c + 1 == Kchunks);
                d.persist = PersistPolicy::OFF;
                // WGT reuse context: WGT depends on (n0, c). Two dispatches
                // sharing the same (n0, c) can reuse WGT.
                d.n0_group_id = n0;
                d.k_chunk_idx = c;
                (void)non_persist;
                ds.push_back(d);
            }
        }
    }
    return ds;
}

// ORG-2 style realization builder — configurable loop order.
// Loop-order encoding: string of 3 chars from {m,n,c}. Leftmost is
// outermost. Examples:
//   "mnc"  → for m0: for n0: for c   (ORG-0 baseline order)
//   "ncm"  → for n0: for c: for m0   (WGT-panel reuse across M loop)
//   "mcn"  → for m0: for c: for n0   (ACT-panel reuse across N loop)
//   "cnm"  → for c:  for n0: for m0   (WGT-panel reuse, K-outer variant)
enum class LoopOrder { MNC, NCM, MCN, CNM, NMC, CMN };

inline LoopOrder parse_order(const char* s) {
    std::string x(s);
    if (x == "mnc") return LoopOrder::MNC;
    if (x == "ncm") return LoopOrder::NCM;
    if (x == "mcn") return LoopOrder::MCN;
    if (x == "cnm") return LoopOrder::CNM;
    if (x == "nmc") return LoopOrder::NMC;
    if (x == "cmn") return LoopOrder::CMN;
    return LoopOrder::MNC;
}

// Peak accumulator slots required for a chain matmul under a traversal
// order = max over any moment of #(output tiles whose K-chunks are
// partially accumulated). Under baseline MNC order the accumulator can
// be a single tile; NCM keeps Mt tiles partially-accumulated across the
// (n0, c) group; CNM keeps Mt*Nt in flight. Report this as an
// architectural cost of the ordering choice.
inline int peak_accumulator_slots(int Mt, int Nt, int Kchunks, LoopOrder order) {
    // Simulate the ordered dispatch stream, track (m0, n0) → chunk progress,
    // count active tiles.
    std::vector<std::vector<int>> chunks_seen(Mt, std::vector<int>(Nt, 0));
    int peak = 0, active = 0;
    auto step = [&](int m0, int n0, int c) {
        if (chunks_seen[m0][n0] == 0) { active++; if (active > peak) peak = active; }
        chunks_seen[m0][n0]++;
        if (chunks_seen[m0][n0] == Kchunks) active--;
    };
    auto loop3 = [&](int Amax, int Bmax, int Cmax,
                     auto m_pick, auto n_pick, auto c_pick) {
        for (int a = 0; a < Amax; ++a)
            for (int b = 0; b < Bmax; ++b)
                for (int c = 0; c < Cmax; ++c)
                    step(m_pick(a,b,c), n_pick(a,b,c), c_pick(a,b,c));
    };
    switch (order) {
    case LoopOrder::MNC: loop3(Mt,Nt,Kchunks, [](int a,int,int){return a;}, [](int,int b,int){return b;}, [](int,int,int c){return c;}); break;
    case LoopOrder::NCM: loop3(Nt,Kchunks,Mt, [](int,int,int c){return c;}, [](int a,int,int){return a;}, [](int,int b,int){return b;}); break;
    case LoopOrder::MCN: loop3(Mt,Kchunks,Nt, [](int a,int,int){return a;}, [](int,int,int c){return c;}, [](int,int b,int){return b;}); break;
    case LoopOrder::CNM: loop3(Kchunks,Nt,Mt, [](int,int,int c){return c;}, [](int,int b,int){return b;}, [](int a,int,int){return a;}); break;
    case LoopOrder::NMC: loop3(Nt,Mt,Kchunks, [](int,int b,int){return b;}, [](int a,int,int){return a;}, [](int,int,int c){return c;}); break;
    case LoopOrder::CMN: loop3(Kchunks,Mt,Nt, [](int,int b,int){return b;}, [](int,int,int c){return c;}, [](int a,int,int){return a;}); break;
    }
    return peak;
}

inline std::vector<Dispatch> build_chain_realization_ordered(
        int M, int N, int K,
        const Arch& arch,
        LoopOrder order) {
    std::vector<Dispatch> ds;
    int Mt = M / arch.array_M;
    int Nt = N / arch.array_N;
    int Kchunks = (K + arch.MAX_K - 1) / arch.MAX_K;
    int per_chunk_K = arch.MAX_K;
    int id = 0;
    // Track per-tile chunk-progress so chain flags (FIRST/MIDDLE/LAST) fire
    // correctly regardless of traversal order.
    std::vector<std::vector<int>> chunks_done(Mt, std::vector<int>(Nt, 0));
    // Enumerate all (m0, n0, c) tuples in the specified order.
    auto emit = [&](int m0_idx, int n0_idx, int c_idx) {
        Dispatch d;
        d.id = id++;
        int klen = std::min(per_chunk_K, K - c_idx * per_chunk_K);
        int k_fpga = ((klen + arch.K_TILE - 1) / arch.K_TILE) * arch.K_TILE;
        d.K = k_fpga;
        d.K_beats = k_fpga;
        int chunk_ord = chunks_done[m0_idx][n0_idx];   // 0..Kchunks-1
        d.wb_suppress = (chunk_ord + 1 != Kchunks);    // suppress unless LAST
        d.acc_in      = (chunk_ord != 0);              // false unless MIDDLE/LAST
        d.is_last     = (chunk_ord + 1 == Kchunks);
        chunks_done[m0_idx][n0_idx]++;
        d.persist = PersistPolicy::OFF;
        d.n0_group_id = n0_idx;
        d.k_chunk_idx = c_idx;
        d.m0_group_id = m0_idx;
        ds.push_back(d);
    };
    auto loop = [&](int a_max, int b_max, int c_max,
                    auto a_pick, auto b_pick, auto c_pick) {
        for (int a = 0; a < a_max; ++a)
            for (int b = 0; b < b_max; ++b)
                for (int c = 0; c < c_max; ++c) {
                    int m = a_pick(a, b, c);
                    int n = b_pick(a, b, c);
                    int cc = c_pick(a, b, c);
                    emit(m, n, cc);
                }
    };
    // Simple explicit expansion (readable + independent of parameter names).
    switch (order) {
    case LoopOrder::MNC:
        loop(Mt, Nt, Kchunks,
             [](int a,int,int){return a;}, [](int,int b,int){return b;}, [](int,int,int c){return c;});
        break;
    case LoopOrder::NCM:
        loop(Nt, Kchunks, Mt,
             [](int,int,int c){return c;}, [](int a,int,int){return a;}, [](int,int b,int){return b;});
        break;
    case LoopOrder::MCN:
        loop(Mt, Kchunks, Nt,
             [](int a,int,int){return a;}, [](int,int,int c){return c;}, [](int,int b,int){return b;});
        break;
    case LoopOrder::CNM:
        loop(Kchunks, Nt, Mt,
             [](int,int,int c){return c;}, [](int,int b,int){return b;}, [](int a,int,int){return a;});
        break;
    case LoopOrder::NMC:
        loop(Nt, Mt, Kchunks,
             [](int,int b,int){return b;}, [](int a,int,int){return a;}, [](int,int,int c){return c;});
        break;
    case LoopOrder::CMN:
        loop(Kchunks, Mt, Nt,
             [](int,int b,int){return b;}, [](int,int,int c){return c;}, [](int a,int,int){return a;});
        break;
    }
    return ds;
}

// ============================================================
// Block-NCM realization builder (bounded-context NCM traversal).
// See AUDIT_NCM_CONTEXT_CAPACITY.md.
//
// Sequence:
//   for n0 in 0..Nt:
//     for m0_block in 0..Mt step C:            <-- C = block/context capacity
//       for c in 0..Kchunks:
//         for m0 in [m0_block, m0_block+C):
//           emit(m0, n0, c)
//
// Peak simultaneously-live accumulators = C.
// WGT_HOIST hits within each (n0, m0_block, c) sub-group of C dispatches.
// WGT_HOIST misses at each c-transition inside a block AND at each block
// boundary (m0_block change).
// Miss rate = 1/C; hit rate = (C-1)/C.
//
// Correctness: each output tile (m0, n0) receives exactly 3 chunks in
// order c=0..Kchunks-1, with correct FIRST/MIDDLE/LAST chain_flags.
// ============================================================
inline std::vector<Dispatch> build_block_ncm_realization(
        int M, int N, int K,
        const Arch& arch,
        int C) {
    if (C < 1) C = 1;
    std::vector<Dispatch> ds;
    int Mt = M / arch.array_M;
    int Nt = N / arch.array_N;
    int Kchunks = (K + arch.MAX_K - 1) / arch.MAX_K;
    int per_chunk_K = arch.MAX_K;
    int id = 0;
    // Per-tile chunk-progress tracker so chain flags fire correctly under
    // block-NCM's interleaved sequence.
    std::vector<std::vector<int>> chunks_done(Mt, std::vector<int>(Nt, 0));
    for (int n0 = 0; n0 < Nt; ++n0) {
        for (int m0_block = 0; m0_block < Mt; m0_block += C) {
            int m0_end = std::min(m0_block + C, Mt);
            for (int c = 0; c < Kchunks; ++c) {
                for (int m0 = m0_block; m0 < m0_end; ++m0) {
                    Dispatch d;
                    d.id = id++;
                    int klen = std::min(per_chunk_K, K - c * per_chunk_K);
                    int k_fpga = ((klen + arch.K_TILE - 1) / arch.K_TILE) * arch.K_TILE;
                    d.K = k_fpga;
                    d.K_beats = k_fpga;
                    int chunk_ord = chunks_done[m0][n0];
                    d.wb_suppress = (chunk_ord + 1 != Kchunks);
                    d.acc_in      = (chunk_ord != 0);
                    d.is_last     = (chunk_ord + 1 == Kchunks);
                    chunks_done[m0][n0]++;
                    d.persist = PersistPolicy::OFF;
                    d.n0_group_id = n0;
                    d.k_chunk_idx = c;
                    d.m0_group_id = m0;
                    ds.push_back(d);
                }
            }
        }
    }
    return ds;
}

// Peak live-accumulator count under block-NCM Variant A.
// Simulates the sequence and counts max simultaneously-live output tiles.
inline int peak_accumulator_slots_block_ncm(int Mt, int Nt, int Kchunks, int C) {
    if (C < 1) C = 1;
    std::vector<std::vector<int>> chunks_seen(Mt, std::vector<int>(Nt, 0));
    int peak = 0, active = 0;
    for (int n0 = 0; n0 < Nt; ++n0) {
        for (int m0_block = 0; m0_block < Mt; m0_block += C) {
            int m0_end = std::min(m0_block + C, Mt);
            for (int c = 0; c < Kchunks; ++c) {
                for (int m0 = m0_block; m0 < m0_end; ++m0) {
                    if (chunks_seen[m0][n0] == 0) { active++; if (active > peak) peak = active; }
                    chunks_seen[m0][n0]++;
                    if (chunks_seen[m0][n0] == Kchunks) active--;
                }
            }
        }
    }
    return peak;
}

// STAGE B microbenchmark realization: single output tile, 2 K-chunks.
inline std::vector<Dispatch> build_stage_b_two_chunk(int K_chunk = 1024, const Arch* archp = nullptr) {
    Arch arch = archp ? *archp : Arch{};
    std::vector<Dispatch> ds;
    Dispatch d0;
    d0.id = 0; d0.K = K_chunk; d0.K_beats = K_chunk;
    d0.wb_suppress = true; d0.acc_in = false; d0.is_last = false; d0.persist = PersistPolicy::OFF;
    Dispatch d1;
    d1.id = 1; d1.K = K_chunk; d1.K_beats = K_chunk;
    d1.wb_suppress = false; d1.acc_in = true; d1.is_last = true; d1.persist = PersistPolicy::OFF;
    ds.push_back(d0);
    ds.push_back(d1);
    return ds;
}

// MobileBERT-style realization: single dispatch, K<=MAX_K, persist=FIX_B_HIT.
// hit_rate in [0, 1]: fraction of dispatches that hit persist cache.
inline std::vector<Dispatch> build_mb_style(int n_dispatches, int K_per_dispatch,
                                             double hit_rate = 0.7) {
    std::vector<Dispatch> ds;
    for (int i = 0; i < n_dispatches; ++i) {
        Dispatch d;
        d.id = i;
        d.K = K_per_dispatch;
        d.K_beats = K_per_dispatch;
        d.wb_suppress = false;   // MB single-dispatch has full writeback
        d.acc_in = false;
        d.is_last = true;
        bool is_hit = (static_cast<double>((i * 1103515245u + 12345u) % 65535u) / 65535.0) < hit_rate;
        d.persist = is_hit ? PersistPolicy::FIX_B_HIT : PersistPolicy::FIX_B_MISS_FILL;
        ds.push_back(d);
    }
    return ds;
}

} // namespace flexnpu_sim
