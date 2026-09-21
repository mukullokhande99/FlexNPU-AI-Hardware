cmake_minimum_required(VERSION 3.13)
project(flexnpu_sim CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wno-unused-parameter -O2)

include_directories(include)

add_executable(flexnpu_sim src/main.cpp)

enable_testing()

add_executable(test_stage_b tests/test_stage_b.cpp)
add_test(NAME stage_b_chain COMMAND test_stage_b)

add_executable(test_gpt2 tests/test_gpt2.cpp)
add_test(NAME gpt2_ffn COMMAND test_gpt2)

add_executable(test_mb tests/test_mb.cpp)
add_test(NAME mb_style COMMAND test_mb)

# Sensitivity sweep tool (not a test — emits CSV to stdout).
add_executable(gpt2_sensitivity_sweep tests/gpt2_sensitivity_sweep.cpp)

# ORG family sweep tool (CSV output).
add_executable(org_family_sweep tests/org_family_sweep.cpp)

# ORG family focused test.
add_executable(test_org_family tests/test_org_family.cpp)
add_test(NAME org_family COMMAND test_org_family)

# S2-regime causal-claim regression test.
add_executable(test_s2_regime tests/test_s2_regime.cpp)
add_test(NAME s2_regime COMMAND test_s2_regime)

# NCM context-capacity sweep + correctness test.
add_executable(ncm_context_sweep tests/ncm_context_sweep.cpp)
add_executable(test_ncm_context tests/test_ncm_context.cpp)
add_test(NAME ncm_context COMMAND test_ncm_context)
