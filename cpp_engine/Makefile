# Build for the C++ matching engine.
#
# Expects GNU make + g++. CLion bundles both; add them to PATH once:
#   export PATH="/c/Program Files/JetBrains/CLion 2026.2/bin/mingw/bin:$PATH"
# and invoke as `mingw32-make` (MinGW ships no plain `make`).
#
# Recipes assume a POSIX shell, so run from Git Bash / MSYS, not cmd.exe.
SHELL := /bin/sh

CXX ?= g++
AR  ?= ar

# debug (default) or release; object trees are kept separate so switching
# between them never needs a clean.
BUILD ?= debug

CXXSTD   := c++20
SRC_DIRS := . orderbook continuous_auction call_auction call_auction/rules
OBJ_DIR  := build/$(BUILD)
LIB      := $(OBJ_DIR)/liblob.a
EXE      := $(if $(filter Windows_NT,$(OS)),.exe,)
BIN      := $(OBJ_DIR)/lob$(EXE)
TEST_BIN := $(OBJ_DIR)/lob_tests$(EXE)
BENCH_BIN := build/release/lob_bench$(EXE)
REPLAY_BIN := build/release/lob_replay$(EXE)
FUZZ_BIN := build/release/lob_fuzz$(EXE)
LATENCY_BIN := build/release/lob_latency$(EXE)
BACKTEST_BIN := build/release/lob_backtest$(EXE)
LOBSTER_CHECK_BIN := build/release/lob_lobster_check$(EXE)
# libstdc++ static on MinGW so its own allocations also hit replay.cpp's
# counting operator new; psapi provides the peak working set.
REPLAY_LIBS := $(if $(filter Windows_NT,$(OS)),-static-libstdc++ -static-libgcc -lpsapi,)

WARN := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
        -Wold-style-cast -Wnon-virtual-dtor

CXXFLAGS_debug   := -O0 -g3
CXXFLAGS_release := -O2 -DNDEBUG

# Opt in with `SANITIZE=1`. MinGW ships no libasan, so this needs clang++ or
# a Linux/macOS gcc: SANITIZE=1 CXX=clang++ mingw32-make
ifeq ($(SANITIZE),1)
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer
endif

CXXFLAGS := -std=$(CXXSTD) $(WARN) $(CXXFLAGS_$(BUILD)) $(SAN) \
            $(addprefix -I,$(SRC_DIRS)) -MMD -MP
LDFLAGS  := $(SAN)

SRCS := $(foreach d,$(SRC_DIRS),$(wildcard $(d:.=)*.cpp $(d)/*.cpp))
SRCS := $(sort $(patsubst ./%,%,$(SRCS)))
MAIN := $(filter main.cpp,$(SRCS))
OBJS := $(SRCS:%.cpp=$(OBJ_DIR)/%.o)
LIB_OBJS := $(filter-out $(OBJ_DIR)/main.o,$(OBJS))
TEST_SRCS := $(sort $(wildcard tests/*.cpp))
TEST_OBJS := $(TEST_SRCS:%.cpp=$(OBJ_DIR)/%.o)
BENCH_OBJ := build/release/bench/bench.o
REPLAY_OBJ := build/release/bench/replay.o
FUZZ_OBJ := build/release/fuzz/fuzz.o
LATENCY_OBJ := build/release/bench/latency.o
BACKTEST_OBJ := build/release/backtest/run_backtest.o
LOBSTER_CHECK_OBJ := build/release/backtest/lobster_check.o
DEPS := $(OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(BENCH_OBJ:.o=.d) $(REPLAY_OBJ:.o=.d) \
        $(FUZZ_OBJ:.o=.d) $(LATENCY_OBJ:.o=.d) $(BACKTEST_OBJ:.o=.d) $(LOBSTER_CHECK_OBJ:.o=.d)

# Fuzzer knobs: `make fuzz OPS=5000000 SEEDS=10`
OPS ?= 1000000
SEEDS ?= 6

.DEFAULT_GOAL := help
.PHONY: help lib app run test bench replay fuzz mutants latency backtest ui lobster-check profile release clean print-srcs

help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$'  $(firstword $(MAKEFILE_LIST)) | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-12s\033[0m %s\n", $$1, $$2}'

lib: $(LIB) ## Compile everything into build/$(BUILD)/liblob.a

release: ## Build the optimised library
	@$(MAKE) --no-print-directory BUILD=release lib

$(LIB): $(LIB_OBJS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

ifneq ($(MAIN),)
app: $(BIN) ## Link the executable (needs cpp_engine/main.cpp)

$(BIN): $(OBJ_DIR)/main.o $(LIB)
	$(CXX) $(LDFLAGS) $^ -o $@

run: $(BIN) ## Build and run the executable
	./$(BIN)
else
app run: ## Link the executable (needs cpp_engine/main.cpp)
	@echo "no main.cpp in cpp_engine/ yet -- nothing to link. 'make lib' builds $(LIB)."
endif

test: $(TEST_BIN) ## Build and run the test suite (FILTER=name runs a subset)
	./$(TEST_BIN) $(FILTER)

$(TEST_BIN): $(TEST_OBJS) $(LIB)
	$(CXX) $(LDFLAGS) $^ -o $@

# Always an optimised build: timing a -O0 binary measures nothing useful.
bench: ## Run the benchmark (release build) and write ../benchmark/results_cpp.json
	@$(MAKE) --no-print-directory BUILD=release $(BENCH_BIN)
	./$(BENCH_BIN) ../benchmark/results_cpp.json

$(BENCH_BIN): $(BENCH_OBJ) build/release/liblob.a
	$(CXX) $(LDFLAGS) $^ -o $@

replay: ## Build the replay binary used by benchmark/compare.py (release)
	@$(MAKE) --no-print-directory BUILD=release $(REPLAY_BIN)

$(REPLAY_BIN): $(REPLAY_OBJ) build/release/liblob.a
	$(CXX) $(LDFLAGS) $^ -o $@ $(REPLAY_LIBS)

fuzz: ## Differential fuzz every book against the reference (OPS=, SEEDS=)
	@$(MAKE) --no-print-directory BUILD=release $(FUZZ_BIN)
	./$(FUZZ_BIN) --ops $(OPS) --seeds $(SEEDS)

$(FUZZ_BIN): $(FUZZ_OBJ) build/release/liblob.a
	$(CXX) $(LDFLAGS) $^ -o $@

mutants: ## Inject bugs into FastBook one at a time; the fuzzer must catch each
	@$(MAKE) --no-print-directory BUILD=release lib
	python fuzz/mutation_check.py

latency: ## Per-op latency distributions (p50..p99.99) for every book; writes ../benchmark/latency_results.json
	@$(MAKE) --no-print-directory BUILD=release $(LATENCY_BIN)
	./$(LATENCY_BIN) ../benchmark/latency_results.json

$(LATENCY_BIN): $(LATENCY_OBJ) build/release/liblob.a
	$(CXX) $(LDFLAGS) $^ -o $@ $(REPLAY_LIBS)

# Backtester knobs: `make backtest BT_ARGS="--profile volatile --latency-us 200"`
BT_ARGS ?=
backtest: ## Run the market-making backtester on a synthetic feed (BT_ARGS= passes options)
	@$(MAKE) --no-print-directory BUILD=release $(BACKTEST_BIN)
	./$(BACKTEST_BIN) $(BT_ARGS)

$(BACKTEST_BIN): $(BACKTEST_OBJ)
	$(CXX) $(LDFLAGS) $^ -o $@

# The localhost viewer: runs the backtester from a web page and plots the result.
UI_PORT ?= 8765
ui: ## Open the backtest viewer on http://127.0.0.1:8765 (UI_PORT= changes it)
	@$(MAKE) --no-print-directory BUILD=release $(BACKTEST_BIN)
	python ../ui/server.py --port $(UI_PORT)

# LOBSTER sample files unzip into ../data/lobster (git-ignored).
LOBSTER_DIR ?= ../data/lobster
LOBSTER_DAY ?= AMZN_2012-06-21_34200000_57600000
LOBSTER_LEVELS ?= 10
lobster-check: ## Rebuild a LOBSTER day and check it against its orderbook file after every message
	@$(MAKE) --no-print-directory BUILD=release $(LOBSTER_CHECK_BIN)
	./$(LOBSTER_CHECK_BIN) $(LOBSTER_DIR)/$(LOBSTER_DAY)_message_$(LOBSTER_LEVELS).csv $(LOBSTER_DIR)/$(LOBSTER_DAY)_orderbook_$(LOBSTER_LEVELS).csv --price-divisor 100

$(LOBSTER_CHECK_BIN): $(LOBSTER_CHECK_OBJ)
	$(CXX) $(LDFLAGS) $^ -o $@

# perf on Linux. Elsewhere bench/sampler.h samples the thread (MinGW has no
# perf, and its gprof records call counts but no time) and bench/symbolize.py
# resolves the samples with addr2line. PROFILE_IMPL=engine|map|fast.
PROFILE_IMPL ?= fast
profile: ## Profile the latency replay (perf on Linux, a sampling profiler on Windows)
ifeq ($(shell uname -s 2>/dev/null),Linux)
	@$(MAKE) --no-print-directory BUILD=release $(LATENCY_BIN)
	perf record -g -o build/perf.data ./$(LATENCY_BIN) --profile $(PROFILE_IMPL)
	perf report -i build/perf.data --stdio --no-children | head -60
else
	@mkdir -p build/profile
	$(CXX) -std=$(CXXSTD) -O2 -DNDEBUG -g $(addprefix -I,$(SRC_DIRS)) \
		bench/latency.cpp $(filter-out main.cpp,$(SRCS)) -o build/profile/lob_latency_prof$(EXE) \
		$(REPLAY_LIBS) -Wl,--disable-dynamicbase
	./build/profile/lob_latency_prof$(EXE) --profile $(PROFILE_IMPL) build/profile/samples_$(PROFILE_IMPL).txt
	python bench/symbolize.py build/profile/lob_latency_prof$(EXE) build/profile/samples_$(PROFILE_IMPL).txt
endif

clean: ## Remove all build output
	rm -rf build

print-srcs: ## List the translation units make has picked up
	@printf '%s\n' $(SRCS)

-include $(DEPS)
