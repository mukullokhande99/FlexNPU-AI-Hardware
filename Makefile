# FlexNPU-AI-Hardware — v0.1 Makefile
#
# Host-only. No FPGA, no Kria, no Vivado. Requires:
#   apt install iverilog verilator
#   pip install -r requirements.txt

REPO       := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD_DIR  := $(REPO)/build
RTL        := rtl/compute_tile_bram.sv rtl/mm.sv rtl/systolic.sv rtl/mac.sv rtl/fifo.sv
RTL_ABS    := $(patsubst %,$(REPO)/%,$(RTL))
PY         ?= python3
CONFIG     ?= configs/ctb/baseline.yaml
K          ?= 1024

.PHONY: help check onboarding ctb_correctness ctb_eval regime_walk parity workload_replay pytest sim_core sim_core_test clean

help:
	@echo "FlexNPU-AI-Hardware — student targets"
	@echo ""
	@echo "  make check              Quick tool + regression check (fast)."
	@echo "  make onboarding         Full onboarding gate (~1 min)."
	@echo "  make ctb_correctness    Verilator byte-exact GEMM (~15 s)."
	@echo "  make ctb_eval           iverilog run of the regime TB (~2 s)."
	@echo "                          Args: CONFIG=configs/ctb/baseline.yaml K=1024"
	@echo "  make regime_walk        Python: walk K to hit each regime."
	@echo "  make parity             Python + iverilog: sim-vs-RTL side-by-side."
	@echo "  make workload_replay    Replay a MobileBERT trace through ctb_mech."
	@echo "  make sim_core           Build the C++ event-driven simulator."
	@echo "  make sim_core_test      Build + ctest the C++ simulator (6 tests)."
	@echo "  make pytest             Run smoke tests + ctb_mech regressions."
	@echo "  make clean              Remove $(BUILD_DIR)."

check:
	@which iverilog >/dev/null || (echo "iverilog missing"; exit 1)
	@which verilator >/dev/null || (echo "verilator missing"; exit 1)
	@which $(PY) >/dev/null || (echo "python3 missing"; exit 1)
	@$(PY) -c "import yaml" 2>/dev/null || (echo "pip install pyyaml"; exit 1)
	@$(PY) -m pytest simulator/ctb_mech/test_regimes.py -q
	@echo "check: OK"

onboarding: check ctb_correctness ctb_eval sim_core_test regime_walk parity
	@echo ""
	@echo "===================================================="
	@echo "Onboarding gate: all steps completed."
	@echo "See PROJECT_GUIDE.md for the research workflow."
	@echo "===================================================="

$(BUILD_DIR):
	@mkdir -p $@

# --- RTL: byte-exact correctness gate via Verilator ---
ctb_correctness: | $(BUILD_DIR)
	@rm -rf $(BUILD_DIR)/ctb_correctness
	@mkdir -p $(BUILD_DIR)/ctb_correctness
	@verilator --cc --exe --build \
	  -Wno-fatal -Wno-WIDTHTRUNC -Wno-WIDTHEXPAND -Wno-UNOPTFLAT \
	  -Wno-CASEINCOMPLETE -Wno-INITIALDLY -Wno-UNUSEDSIGNAL \
	  -Wno-UNUSEDPARAM -Wno-VARHIDDEN -Wno-PINMISSING \
	  --top-module compute_tile_bram \
	  -Mdir $(BUILD_DIR)/ctb_correctness -j 4 \
	  $(RTL_ABS) $(REPO)/tb/ctb_correctness.cpp \
	  -CFLAGS "-O2" --no-timing
	@$(BUILD_DIR)/ctb_correctness/Vcompute_tile_bram

# --- RTL: one regime TB run with parameters from config_bridge ---
ctb_eval: | $(BUILD_DIR)
	@ARGS=$$($(PY) tools/config_bridge.py --ctb $(CONFIG) --K $(K) --emit iverilog); \
	 iverilog -g2012 $$ARGS -o $(BUILD_DIR)/ctb_regime.vvp \
	   tb/ctb_regime_tb.sv $(RTL)
	@vvp $(BUILD_DIR)/ctb_regime.vvp

# --- Python: examples ---
regime_walk:
	@$(PY) examples/02_regime_walk.py

parity:
	@$(PY) examples/03_rtl_vs_model_parity.py --config $(CONFIG) --K $(K)

workload_replay:
	@$(PY) examples/04_workload_replay.py \
	  --trace workloads/mobilebert/mb_flex_sites.json \
	  --config $(CONFIG)

pytest:
	@$(PY) -m pytest -q simulator/ctb_mech/test_regimes.py tests/

# --- C++ event-driven simulator (simulator/core/) ---
sim_core:
	@cmake -S simulator/core -B simulator/core/build -DCMAKE_BUILD_TYPE=Release >/dev/null
	@cmake --build simulator/core/build -j4

sim_core_test: sim_core
	@cd simulator/core/build && ctest --output-on-failure

clean:
	@rm -rf $(BUILD_DIR) simulator/core/build
