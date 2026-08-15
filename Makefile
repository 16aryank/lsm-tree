CXX      := g++
# Optimization/instrumentation level, overridable per invocation, e.g.:
#   make OPT=-O0 test                          # faster builds, easier stepping
#   make OPT="-O1 -fsanitize=thread" test      # race detection
#   make OPT="-O1 -fsanitize=address,undefined" test
# NDEBUG is deliberately never set: the test suite is built entirely on
# assert(), so defining it would silently turn every test into a no-op.
OPT      ?= -O2
# Recursively expanded so a target-specific override (see memtable_benchmark)
# still reaches the compile rules.
CXXFLAGS  = -std=c++23 -Wall -Wextra -g $(OPT)
INCLUDES := -I. -Isrc -I/opt/homebrew/opt/boost/include
LDFLAGS  := -L/opt/homebrew/opt/boost/lib -lboost_thread

SRC_DIR   := src
TEST_DIR  := tests
BUILD_DIR := build
BIN_DIR   := bin

# Objects mirror their source tree under build/, so a test named after the
# unit it covers (tests/memtable/arena.cpp <-> src/memtable/arena.cpp) doesn't
# collide on the same .o.
SRC_BUILD  := $(BUILD_DIR)/$(SRC_DIR)
TEST_BUILD := $(BUILD_DIR)/$(TEST_DIR)

SRCS     := $(shell find $(SRC_DIR) -name '*.cpp')
SRC_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(SRC_BUILD)/%.o,$(SRCS))

TEST_SRCS := $(shell find $(TEST_DIR) -name '*.cpp')
TEST_OBJS := $(patsubst $(TEST_DIR)/%.cpp,$(TEST_BUILD)/%.o,$(TEST_SRCS))
ALL_BINS  := $(patsubst $(TEST_DIR)/%.cpp,$(BIN_DIR)/%,$(TEST_SRCS))

# Anything named bench_* is a benchmark, not a test: it takes far longer than
# an assertion suite and its output is a measurement rather than a pass/fail.
# `make` and `make test` skip them entirely; each has its own explicit target.
BENCH_SRCS := $(shell find $(TEST_DIR) -name 'bench_*.cpp')
BENCH_BINS := $(patsubst $(TEST_DIR)/%.cpp,$(BIN_DIR)/%,$(BENCH_SRCS))
TEST_BINS  := $(filter-out $(BENCH_BINS),$(ALL_BINS))

DEPS := $(SRC_OBJS:.o=.d) $(TEST_OBJS:.o=.d)

.PHONY: all test memtable_benchmark clean check run
.PRECIOUS: $(SRC_BUILD)/%.o $(TEST_BUILD)/%.o

all: test

# Compile any single .cpp for a sanity check without linking, e.g.:
#   make check FILE=src/wal/wal_reader.cpp
check:
ifndef FILE
	$(error Usage: make check FILE=path/to/file.cpp)
endif
	$(CXX) $(CXXFLAGS) $(INCLUDES) -fsyntax-only $(FILE)

# Build any object under src/ or tests/, e.g. `make build/src/wal/wal_reader.o`.
# Doubles as a per-file compilation sanity check that leaves the .o behind.
$(SRC_BUILD)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(TEST_BUILD)/%.o: $(TEST_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# Link each test against the src objects in its matching subdirectory only
# (tests/foo/bar.cpp <-> src/foo/*.cpp), so an unrelated broken src file
# doesn't block tests that don't need it. Generated per test rather than done
# with one pattern rule, because the src-object list depends on the test's own
# subdirectory and `%` isn't available inside a secondary expansion.
define TEST_LINK_RULE
$(BIN_DIR)/$(1): $(TEST_BUILD)/$(1).o \
                 $(patsubst $(SRC_DIR)/%.cpp,$(SRC_BUILD)/%.o,$(wildcard $(SRC_DIR)/$(dir $(1))*.cpp))
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(CXXFLAGS) $$^ -o $$@ $$(LDFLAGS)
endef

$(foreach t,$(patsubst $(TEST_DIR)/%.cpp,%,$(TEST_SRCS)),$(eval $(call TEST_LINK_RULE,$(t))))

# Build and run every test, e.g. `make test`. Benchmarks are excluded.
test: $(TEST_BINS)
	@for bin in $(TEST_BINS); do \
		echo "== running $$bin =="; \
		./$$bin || exit 1; \
	done

# Build and run the memtable benchmark: the arena-backed MemTable against the
# pre-arena one in src/deprecated/. Never part of `make` or `make test`.
# Override the entry count with ARGS, e.g.
#   make memtable_benchmark ARGS=1000000
# Pinned to -O2 so the numbers stay meaningful even if OPT is lowered
# project-wide; an explicit `make OPT=... memtable_benchmark` still wins.
memtable_benchmark: OPT := -O2
memtable_benchmark: $(BIN_DIR)/memtable/bench_memtable
	./$< $(ARGS)

# Build and run a single test, e.g. `make run TEST=memtable/skiplist`.
run: $(BIN_DIR)/$(TEST)
ifndef TEST
	$(error Usage: make run TEST=memtable/skiplist)
endif
	./$(BIN_DIR)/$(TEST) $(ARGS)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

-include $(DEPS)
