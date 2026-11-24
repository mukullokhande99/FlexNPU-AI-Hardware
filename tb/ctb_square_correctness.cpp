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

static int square_mode = 0;
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
    dut->i_square_mode = 0;
    dut->i_square_unit_exp = 0;
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

// Exact integer numerator/denominator, followed by C++ signed division
// (truncation toward zero), for a unit exponent of -4.
static int64_t scaled_product(uint8_t a, uint8_t b, int mode) {
    static const int posit16[16] = {0,1,4,8,16,32,64,256,0,-256,-64,-32,-16,-8,-4,-1};
    auto fp4_2 = [](int x) { int e=(x>>1)&3, f=x&1; int v=e ? ((2+f) << (e-1)) : f; return (x&8) ? -v : v; };
    auto lane2 = [](int x, int lane, bool e0m1) { if (x & (1 << (4+lane))) return 0; int c=(x>>(2*lane))&3; int v=e0m1 ? (c&1 ? 3 : 2) : (c&1 ? 1 : 2); return c&2 ? -v : v; };
    if (mode==0) return (int64_t(posit16[a&15])*posit16[b&15])/16;
    if (mode==1) return int64_t(fp4_2(a&15))*fp4_2(b&15)*4;
    int sum=lane2(a,0,mode==3)*lane2(b,0,mode==3)+lane2(a,1,mode==3)*lane2(b,1,mode==3);
    return int64_t(sum)*4;
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
                s += scaled_product(uint8_t(A_full[m * K_full + k]), uint8_t(B_full[k * NN + n]), square_mode);
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
    dut->i_square_mode = square_mode;
    dut->i_square_unit_exp = uint8_t(-4) & 63;
    dut->start = 1;
    tick(dut);
    dut->start = 0;
    // Change inputs after start to check the latched configuration.
    dut->i_square_mode = (square_mode + 1) & 3;
    dut->i_square_unit_exp = 0;

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
    std::uniform_int_distribution<int> d(0, square_mode < 2 ? 15 : 63);
    std::vector<int8_t> v(size_t(rows) * cols);
    for (auto& x : v) x = int8_t(d(rng));
    return v;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto* dut = new Vcompute_tile_bram;
    int fails=0;
    for (square_mode=0; square_mode<4; ++square_mode) {
        for (int chained=0; chained<2; ++chained) {
            const int K=32;
            auto A=gen_matrix(N,K,0xA100 + square_mode);
            auto B=gen_matrix(K,N,0xB200 + square_mode);
            auto gold=golden_pack(A,B,N,K,N);
            reset(dut); reset_capture();
            if (!chained) {
                load_slice(A,B,N,K,N,0,K);
                if (run_dispatch(dut,K,false,true)<0) ++fails;
            } else {
                load_slice(A,B,N,K,N,0,16);
                if (run_dispatch(dut,16,false,false)<0) ++fails;
                if (out_enb_count_since_dispatch != 0) ++fails;
                load_slice(A,B,N,K,N,16,16);
                if (run_dispatch(dut,16,true,true)<0) ++fails;
            }
            char name[80]; std::snprintf(name,sizeof(name),"mode=%d chained=%d",square_mode,chained);
            fails += verify(gold,name);
        }
    }
    delete dut;
    if (fails) { std::fprintf(stderr,"SQuARE CTB failures: %d\n",fails); return 1; }
    std::puts("PASS SQuARE CTB encoded GEMM"); return 0;
}
