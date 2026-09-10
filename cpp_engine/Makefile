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
BIN      := $(OBJ_DIR)/lob$(if $(filter Windows_NT,$(OS)),.exe,)

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
DEPS := $(OBJS:.o=.d)

.DEFAULT_GOAL := help
.PHONY: help lib app run release clean print-srcs

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

clean: ## Remove all build output
	rm -rf build

print-srcs: ## List the translation units make has picked up
	@printf '%s\n' $(SRCS)

-include $(DEPS)
