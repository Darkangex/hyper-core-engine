# ═══════════════════════════════════════════════════════════════════════
#   Hyper-Core HFT Matching Engine — Build System
#   CMake 3.20+ | C++20 | FetchContent for Catch2
# ═══════════════════════════════════════════════════════════════════════

cmake_minimum_required(VERSION 3.20)
project(HyperCoreEngine
    VERSION 1.0.0
    LANGUAGES CXX
    DESCRIPTION "Ultra-low latency HFT Matching Engine"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Compiler flags ───
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -O2)
endif()

# ═══════════════════════════════════════════════════════════════════════
#  1. Main Engine Executable
# ═══════════════════════════════════════════════════════════════════════

add_executable(hyper_core_engine hyper_core_engine.cpp)
target_link_libraries(hyper_core_engine PRIVATE Threads::Threads)
find_package(Threads REQUIRED)

# Platform-specific settings
if(WIN32)
    target_compile_definitions(hyper_core_engine PRIVATE _WIN32)
endif()


# ═══════════════════════════════════════════════════════════════════════
#  2. Unit Tests (with Catch2 via FetchContent)
# ═══════════════════════════════════════════════════════════════════════

# Tests use a self-contained minimal framework (zero dependencies).
# Catch2 is available as an OPTIONAL dependency for future expansion.

add_executable(test_hyper_core tests/test_hyper_core.cpp)
target_link_libraries(test_hyper_core PRIVATE Threads::Threads)
target_compile_definitions(test_hyper_core PRIVATE HYPER_CORE_NO_MAIN)

if(WIN32)
    target_compile_definitions(test_hyper_core PRIVATE _WIN32)
endif()

# Register with CTest
enable_testing()
add_test(NAME HyperCoreTests COMMAND test_hyper_core)


# ═══════════════════════════════════════════════════════════════════════
#  3. Latency Benchmark
# ═══════════════════════════════════════════════════════════════════════

add_executable(benchmark_latency benchmarks/benchmark_latency.cpp)
target_link_libraries(benchmark_latency PRIVATE Threads::Threads)
target_compile_definitions(benchmark_latency PRIVATE HYPER_CORE_NO_MAIN)

if(WIN32)
    target_compile_definitions(benchmark_latency PRIVATE _WIN32)
endif()


# ═══════════════════════════════════════════════════════════════════════
#  Custom Targets (convenience)
# ═══════════════════════════════════════════════════════════════════════

# Run tests:     cmake --build build --target run_tests
add_custom_target(run_tests
    COMMAND test_hyper_core
    DEPENDS test_hyper_core
    COMMENT "Running unit tests..."
)

# Run benchmark:  cmake --build build --target run_benchmark
add_custom_target(run_benchmark
    COMMAND benchmark_latency
    DEPENDS benchmark_latency
    COMMENT "Running latency benchmark..."
)
