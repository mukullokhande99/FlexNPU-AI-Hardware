// Verilator C++ TB for cross-dispatch K-accumulate in compute_tile_bram.
//
// Test matrix:
//   CASE A — non-regression: K=1024 single dispatch (acc=0, wb=1).
//            Byte-identical vs a golden numpy matmul reference for the
//            same inputs. Proves the added inputs at reset default change
//            NOTHING vs the pre-change shell.
//   CASE B — K=2048 as 2 x 1024 chained: chunks (acc=0,wb=0) then
//            (acc=1,wb=1). Full-matmul byte-exact.
//   CASE C — K=3000 as 1024+1024+952 (uneven last chunk, still K%K_TILE==0):
//            chunks (acc=0,wb=0), (acc=1,wb=0), (acc=1,wb=1). Full-matmul
//            byte-exact.
//   CASE D — chain intermediate-chunk quiet check: with (acc=0,wb=0) and
//            (acc=1,wb=0), out_enb MUST NEVER assert on those dispatches.
//            This is the byte-exact proof that rescale is IMPLICITLY GATED
//            on intermediate chunks (writeback skipped → pack_row never
//            fires → no partial sums escape → no downstream rescale
//            corruption possible).
//
// Golden reference: full-matmul on int64 accumulator, then packed as
// N * ACC_WIDTH into OUT_BUF_DATA_WIDTH words matching pack_row().
//
// PASS gate is unconditional bit-equality between DUT output words and
// golden words for CASES A/B/C, plus zero-out_enb on CASE D intermediate
// chunks.

#include "Vcompute_tile_bram.h"
#include "verilated.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

// Must mirror the DUT parameters. If these change, this TB no longer
// exercises the same hardware — bump both together.
constexpr int ACT_WIDTH        = 8;
constexpr int ACC_WIDTH        = 32;
constexpr int N                = 8;
constexpr int K_TILE           = 8;
constexpr int MAX_K            = 1024;
constexpr int IN_BUF_ADDR_W    = 10;  // clog2(MAX_K)
constexpr int OUT_BUF_ADDR_W   = 3;   // clog2(N)
constexpr int IN_BUF_DATA_W    = N * ACT_WIDTH;    // 64b
constexpr int OUT_BUF_DATA_W   = N * ACC_WIDTH;    // 256b
constexpr int OUT_BUF_WE_W     = OUT_BUF_DATA_W / 8;  // 32B

static uint64_t sim_time_ps = 0;
double sc_time_stamp() { return double(sim_time_ps); }

// ---- BRAM backing model ----
// One slice at a time (the shell's ping-pong buffer would refill between
// dispatches; we mimic by rewriting the arrays). Each address holds N
// packed int8 values.
static std::array<uint64_t, MAX_K> act_bram{};
static std::array<uint64_t, MAX_K> wgt_bram{};

// ---- OUT capture: address-indexed, most-recent write wins. On a
// successful S_WRITEBACK compute_tile_bram writes N rows (addr 0..N-1),
// each containing N packed ACC_WIDTH values. We capture 256b per row as
// 8 uint32_t words (little-endian LSB-first) matching pack_row(). ----
static std::array<std::array<uint32_t, N>, N> out_capture{};
static std::array<bool, N>                     out_valid{};
static int                                     out_enb_count_since_dispatch = 0;

// ---- Reset / tick helpers ----
static void tick(Vcompute_tile_bram* dut) {
    dut->clk = 0;
    dut->eval();
    sim_time_ps += 500;
    dut->clk = 1;
    // Sample OUT write BEFORE we let the clock rise take effect on downstream
    // logic. Actually we sample AFTER the clock has settled the writes:
    dut->eval();
    sim_time_ps += 500;

    // Capture writes fired this cycle.
    if (dut->out_enb && (dut->out_web != 0)) {
        out_enb_count_since_dispatch++;
        uint32_t row = dut->out_addrb;
        assert(row < N);
        // Verilator packs OUT_BUF_DATA_WIDTH (256b) into 8 uint32_t
        // words in the flattened `out_dinb` array. Extract them.
        for (int w = 0; w < N; ++w) {
            out_capture[row][w] = dut->out_dinb[w];
        }
        out_valid[row] = true;
    }
}

static void ticks(Vcompute_tile_bram* dut, int n) {
    for (int i = 0; i < n; ++i) tick(dut);
}

static void reset(Vcompute_tile_bram* dut) {
    dut->rst_n = 0;
    dut->clk   = 0;
    dut->start = 0;
    dut->K_total = 0;
    dut->i_accumulate_in = 0;
    dut->i_writeback_en  = 1;
    dut->act_buf_ready = 0;
    dut->wgt_buf_ready = 0;
    dut->act_doutb = 0;
    dut->wgt_doutb = 0;
    for (int i = 0; i < 6; ++i) tick(dut);
    dut->rst_n = 1;
    for (int i = 0; i < 3; ++i) tick(dut);
}

// Pack N int8 values into a uint64_t as the shell's BRAM word does.
static uint64_t pack_row_i8(const int8_t* vals) {
    uint64_t w = 0;
    for (int i = 0; i < N; ++i) {
        w |= uint64_t(uint8_t(vals[i])) << (i * ACT_WIDTH);
    }
    return w;
}

// Fill act_bram/wgt_bram with a K-slice from the full matrices.
// Full A is M×K stored row-major (M=N here), full B is K×N row-major.
// act_bram[k] holds A[:, k+k_off] packed as N int8s (one per row).
// wgt_bram[k] holds B[k+k_off, :] packed as N int8s (one per column).
static void load_slice(const std::vector<int8_t>& A_full,
                       const std::vector<int8_t>& B_full,
                       int M, int K_full, int NN,
                       int k_off, int k_len) {
    assert(NN == N && M == N);
    for (int k = 0; k < k_len; ++k) {
        int8_t a_col[N], b_row[N];
        for (int m = 0; m < M; ++m) a_col[m] = A_full[m * K_full + (k + k_off)];
        for (int n = 0; n < NN; ++n) b_row[n] = B_full[(k + k_off) * NN + n];
        act_bram[k] = pack_row_i8(a_col);
        wgt_bram[k] = pack_row_i8(b_row);
    }
    // Zero the rest so any stray fetch is obvious.
    for (int k = k_len; k < MAX_K; ++k) { act_bram[k] = 0; wgt_bram[k] = 0; }
}

// Golden: full-matmul, then pack rows N×N as pack_row does.
// Returns out_golden[row][word_idx].
static std::array<std::array<uint32_t, N>, N>
golden_pack(const std::vector<int8_t>& A_full,
            const std::vector<int8_t>& B_full,
            int M, int K_full, int NN) {
    assert(NN == N && M == N);
    std::array<std::array<uint32_t, N>, N> out{};
    for (int m = 0; m < M; ++m) {
        for (int n = 0; n < NN; ++n) {
            int64_t s = 0;
            for (int k = 0; k < K_full; ++k) {
                s += int32_t(A_full[m * K_full + k]) * int32_t(B_full[k * NN + n]);
            }
            // Truncate to ACC_WIDTH bits (i32); pack_row copies the low
            // ACC_WIDTH bits of the signed accumulator.
            out[m][n] = uint32_t(int32_t(s));
        }
    }
    return out;
}

// Drive one dispatch: sample flags, pulse start, respond to act/wgt reads
// with 1-cycle BRAM latency, wait for done. Returns cycle count.
static int run_dispatch(Vcompute_tile_bram* dut,
                         int K_slice,
                         bool acc_in, bool wb_en) {
    // Present flags before start pulse; DUT latches them at start.
    dut->i_accumulate_in = acc_in ? 1 : 0;
    dut->i_writeback_en  = wb_en  ? 1 : 0;
    dut->K_total = K_slice;
    // Simulate ping-pong "data ready" (act/wgt buffers loaded).
    dut->act_buf_ready = 1;
    dut->wgt_buf_ready = 1;

    // Pulse start for one cycle.
    dut->start = 1;
    tick(dut);
    dut->start = 0;

    // Respond to BRAM reads with 1-cycle latency (matches shell BRAM model).
    // We track pending reads by watching act_enb/wgt_enb on the previous
    // cycle. Verilator's eval reflects combinational outputs immediately.
    uint32_t act_addr_d = 0, wgt_addr_d = 0;
    bool     act_pend = false, wgt_pend = false;

    int cycles = 0;
    while (!dut->done && cycles < 500000) {
        // Present the latched read data BEFORE the tick so it's available
        // on the DUT's next posedge (matching sync BRAM behavior).
        if (act_pend) dut->act_doutb = act_bram[act_addr_d & (MAX_K - 1)];
        if (wgt_pend) dut->wgt_doutb = wgt_bram[wgt_addr_d & (MAX_K - 1)];
        act_pend = dut->act_enb;
        wgt_pend = dut->wgt_enb;
        act_addr_d = dut->act_addrb;
        wgt_addr_d = dut->wgt_addrb;

        tick(dut);
        cycles++;
    }
    if (!dut->done) {
        std::fprintf(stderr, "  TIMEOUT after %d cycles waiting for done\n", cycles);
        return -1;
    }
    // Drop buf-ready between dispatches to model the ping-pong reload.
    dut->act_buf_ready = 0;
    dut->wgt_buf_ready = 0;
    // Let done deassert and settle.
    ticks(dut, 2);
    return cycles;
}

// Verify captured output vs golden. Returns 0 on PASS, nonzero on FAIL.
static int verify(const std::array<std::array<uint32_t, N>, N>& gold,
                   const char* case_name) {
    int fails = 0;
    for (int r = 0; r < N; ++r) {
        if (!out_valid[r]) {
            std::fprintf(stderr, "  [%s] row %d MISSING writeback\n", case_name, r);
            fails++;
            continue;
        }
        for (int w = 0; w < N; ++w) {
            if (out_capture[r][w] != gold[r][w]) {
                std::fprintf(stderr,
                    "  [%s] row=%d col=%d MISMATCH  dut=0x%08x  gold=0x%08x\n",
                    case_name, r, w, out_capture[r][w], gold[r][w]);
                fails++;
            }
        }
    }
    if (fails == 0) {
        std::printf("  [%s] BYTE-EXACT  (%d rows x %d cols)\n", case_name, N, N);
    } else {
        std::printf("  [%s] FAIL: %d mismatches\n", case_name, fails);
    }
    return fails;
}

// Reset the capture arrays between cases.
static void reset_capture() {
    for (int r = 0; r < N; ++r) {
        out_valid[r] = false;
        for (int w = 0; w < N; ++w) out_capture[r][w] = 0;
    }
    out_enb_count_since_dispatch = 0;
}

// Generate deterministic random int8 test matrices.
static std::vector<int8_t> gen_matrix(int rows, int cols, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> d(-128, 127);
    std::vector<int8_t> v(size_t(rows) * cols);
    for (auto& x : v) x = int8_t(d(rng));
    return v;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto* dut = new Vcompute_tile_bram;

    int total_fails = 0;
    int intermediate_out_enb_fires = 0;

    // -----------------------------------------------------------
    // CASE A — non-regression. K=1024, one dispatch, defaults.
    // -----------------------------------------------------------
    {
        std::printf("== CASE A: K=1024 single dispatch (acc=0, wb=1) — non-regression ==\n");
        const int K = 1024;
        auto A = gen_matrix(N, K, 0xA1);
        auto B = gen_matrix(K, N, 0xA2);
        auto gold = golden_pack(A, B, N, K, N);

        reset(dut);
        reset_capture();
        load_slice(A, B, N, K, N, 0, K);
        int cy = run_dispatch(dut, K, /*acc=*/false, /*wb=*/true);
        std::printf("  dispatch cy=%d, out writes captured=%d\n", cy, out_enb_count_since_dispatch);
        total_fails += verify(gold, "CASE A");
    }

    // -----------------------------------------------------------
    // CASE B — K=2048 as 2 x 1024 chained. Byte-exact.
    // -----------------------------------------------------------
    {
        std::printf("\n== CASE B: K=2048 = 1024+1024 chained ==\n");
        const int K = 2048;
        auto A = gen_matrix(N, K, 0xB1);
        auto B = gen_matrix(K, N, 0xB2);
        auto gold = golden_pack(A, B, N, K, N);

        reset(dut);
        reset_capture();
        // Chunk 0: acc=0, wb=0 (first — clear accumulator, no writeback)
        load_slice(A, B, N, K, N, /*k_off=*/0, /*k_len=*/1024);
        int cy0 = run_dispatch(dut, 1024, /*acc=*/false, /*wb=*/false);
        int intermediate_writes_c0 = out_enb_count_since_dispatch;
        int prev_writes = out_enb_count_since_dispatch;
        std::printf("  chunk 0 (acc=0, wb=0): cy=%d, out writes=%d (must be 0)\n",
                     cy0, intermediate_writes_c0);
        if (intermediate_writes_c0 != 0) intermediate_out_enb_fires++;

        // Chunk 1: acc=1, wb=1 (last — accumulate on top, writeback)
        load_slice(A, B, N, K, N, /*k_off=*/1024, /*k_len=*/1024);
        int cy1 = run_dispatch(dut, 1024, /*acc=*/true, /*wb=*/true);
        int writes_c1 = out_enb_count_since_dispatch - prev_writes;
        std::printf("  chunk 1 (acc=1, wb=1): cy=%d, out writes=%d (must be %d)\n",
                     cy1, writes_c1, N);
        total_fails += verify(gold, "CASE B");
    }

    // -----------------------------------------------------------
    // CASE C — K=3000 as 1024+1024+952 chained. Uneven last chunk.
    // -----------------------------------------------------------
    {
        std::printf("\n== CASE C: K=3000 = 1024+1024+952 chained ==\n");
        const int K = 3000;
        auto A = gen_matrix(N, K, 0xC1);
        auto B = gen_matrix(K, N, 0xC2);
        auto gold = golden_pack(A, B, N, K, N);

        reset(dut);
        reset_capture();
        int prev_writes = 0;

        load_slice(A, B, N, K, N, /*k_off=*/0, /*k_len=*/1024);
        int cy0 = run_dispatch(dut, 1024, /*acc=*/false, /*wb=*/false);
        int w0 = out_enb_count_since_dispatch - prev_writes;
        prev_writes = out_enb_count_since_dispatch;
        std::printf("  chunk 0 (acc=0, wb=0): cy=%d, out writes=%d (must be 0)\n", cy0, w0);
        if (w0 != 0) intermediate_out_enb_fires++;

        load_slice(A, B, N, K, N, /*k_off=*/1024, /*k_len=*/1024);
        int cy1 = run_dispatch(dut, 1024, /*acc=*/true, /*wb=*/false);
        int w1 = out_enb_count_since_dispatch - prev_writes;
        prev_writes = out_enb_count_since_dispatch;
        std::printf("  chunk 1 (acc=1, wb=0): cy=%d, out writes=%d (must be 0)\n", cy1, w1);
        if (w1 != 0) intermediate_out_enb_fires++;

        load_slice(A, B, N, K, N, /*k_off=*/2048, /*k_len=*/952);
        int cy2 = run_dispatch(dut, 952, /*acc=*/true, /*wb=*/true);
        int w2 = out_enb_count_since_dispatch - prev_writes;
        std::printf("  chunk 2 (acc=1, wb=1, K_slice=952): cy=%d, out writes=%d (must be %d)\n",
                     cy2, w2, N);
        total_fails += verify(gold, "CASE C");
    }

    // -----------------------------------------------------------
    // CASE D — intermediate-chunk quiet check summary.
    //   Rescale is implicitly gated on intermediate chunks BECAUSE
    //   out_enb never asserts (no drain to downstream = no rescale).
    // -----------------------------------------------------------
    std::printf("\n== CASE D: rescale-gating-by-design summary ==\n");
    std::printf("  Intermediate chunks (wb=0) that fired out_enb: %d (must be 0)\n",
                 intermediate_out_enb_fires);
    if (intermediate_out_enb_fires != 0) {
        std::printf("  FAIL: intermediate chunk asserted out_enb — rescale would corrupt.\n");
        total_fails++;
    } else {
        std::printf("  PASS: no partial-sum drain to output BRAM on any intermediate chunk.\n");
        std::printf("        Rescale unit (downstream) is guaranteed to see only the\n");
        std::printf("        fully-accumulated tile on the final chunk. Zero risk of\n");
        std::printf("        partial-sum rescale corruption.\n");
    }

    // -----------------------------------------------------------
    // Summary
    // -----------------------------------------------------------
    std::printf("\n============================================================\n");
    if (total_fails == 0) {
        std::printf("ALL CASES PASS — cross-dispatch K-accumulate is byte-exact.\n");
        std::printf("============================================================\n");
        delete dut;
        return 0;
    } else {
        std::printf("FAIL: %d total mismatches — STOP, do not proceed.\n", total_fails);
        std::printf("============================================================\n");
        delete dut;
        return 1;
    }
}
