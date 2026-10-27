# Project model for the IDE (CLion indexes and debugs through this).
# cpp_engine/Makefile is the command-line build; keep the two in sync.
cmake_minimum_required(VERSION 3.20)
project(limit_order_book CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # lets clangd/other editors see the flags

file(GLOB_RECURSE LOB_SOURCES CONFIGURE_DEPENDS cpp_engine/*.cpp)
file(GLOB_RECURSE LOB_HEADERS CONFIGURE_DEPENDS cpp_engine/*.h)
# The executables' sources are not part of the library.
list(FILTER LOB_SOURCES EXCLUDE REGEX "cpp_engine/(main\.cpp|tests/|bench/|fuzz/|backtest/)")
list(FILTER LOB_HEADERS EXCLUDE REGEX "cpp_engine/tests/")
file(GLOB LOB_TEST_SOURCES CONFIGURE_DEPENDS cpp_engine/tests/*.cpp)

add_library(lob ${LOB_SOURCES} ${LOB_HEADERS})

target_include_directories(lob PUBLIC
        cpp_engine
        cpp_engine/orderbook
        cpp_engine/continuous_auction
        cpp_engine/call_auction
        cpp_engine/call_auction/rules)

add_executable(lob_app cpp_engine/main.cpp)
target_link_libraries(lob_app PRIVATE lob)

add_executable(lob_bench cpp_engine/bench/bench.cpp)
target_link_libraries(lob_bench PRIVATE lob)

add_executable(lob_replay cpp_engine/bench/replay.cpp)
target_link_libraries(lob_replay PRIVATE lob)
if (WIN32)
    target_link_libraries(lob_replay PRIVATE psapi)
endif ()
if (MINGW)
    # every allocation, libstdc++'s included, must reach the counting operator new
    target_link_options(lob_replay PRIVATE -static-libstdc++ -static-libgcc)
endif ()

# Differential fuzzer and latency distributions (FastBook vs the engine).
add_executable(lob_fuzz cpp_engine/fuzz/fuzz.cpp)
target_link_libraries(lob_fuzz PRIVATE lob)

add_executable(lob_latency cpp_engine/bench/latency.cpp)
target_link_libraries(lob_latency PRIVATE lob)
if (MINGW)
    # the counting operator new must see libstdc++'s allocations too
    target_link_options(lob_latency PRIVATE -static-libstdc++ -static-libgcc)
endif ()

# Market-making backtester (header-only; Part 4 of ROADMAP.md).
add_executable(lob_backtest cpp_engine/backtest/run_backtest.cpp)
target_link_libraries(lob_backtest PRIVATE lob)
add_executable(lob_lobster_check cpp_engine/backtest/lobster_check.cpp)
target_link_libraries(lob_lobster_check PRIVATE lob)

# Google Benchmark suite. Off by default: it downloads google/benchmark.
option(LOB_GBENCH "Build lob_gbench (fetches google/benchmark)" OFF)
if (LOB_GBENCH)
    include(FetchContent)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googlebenchmark
            GIT_REPOSITORY https://github.com/google/benchmark.git
            GIT_TAG v1.9.1
            GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(googlebenchmark)
    add_executable(lob_gbench cpp_engine/bench/gbench.cpp)
    target_link_libraries(lob_gbench PRIVATE lob benchmark::benchmark)
endif ()

add_executable(lob_tests ${LOB_TEST_SOURCES})
target_link_libraries(lob_tests PRIVATE lob)

enable_testing()
add_test(NAME lob_tests COMMAND lob_tests)

foreach (target lob lob_app lob_bench lob_replay lob_fuzz lob_latency lob_backtest lob_lobster_check lob_tests)
    if (MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else ()
        target_compile_options(${target} PRIVATE
                -Wall -Wextra -Wpedantic -Wshadow
                -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor)
    endif ()
endforeach ()
