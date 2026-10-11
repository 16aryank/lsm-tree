CXX      := g++
# Override OPT to change optimization or enable a sanitizer. Tests use assert(),
# so keep NDEBUG unset.
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

# Mirror source paths under build/ to keep same-named files distinct.
SRC_BUILD  := $(BUILD_DIR)/$(SRC_DIR)
TEST_BUILD := $(BUILD_DIR)/$(TEST_DIR)

SRCS     := $(shell find $(SRC_DIR) -name '*.cpp')
SRC_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(SRC_BUILD)/%.o,$(SRCS))

TEST_SRCS := $(shell find $(TEST_DIR) -name '*.cpp')
TEST_OBJS := $(patsubst $(TEST_DIR)/%.cpp,$(TEST_BUILD)/%.o,$(TEST_SRCS))
ALL_BINS  := $(patsubst $(TEST_DIR)/%.cpp,$(BIN_DIR)/%,$(TEST_SRCS))

# Exclude benchmarks from the assertion suite.
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

# Link each test with its matching source directory and any listed dependencies.
SRC_DIRS_wal/wal                   := wal/ util/
SRC_DIRS_sstable/sstable           := sstable/ util/
SRC_DIRS_memtable/memtable_manager := memtable/ wal/ util/
SRC_DIRS_iterator/merging_iterator := iterator/ sstable/ memtable/ util/
# Version tests share the same source dependencies.
SRC_DIRS_version/version_edit      := version/ sstable/ iterator/ wal/ util/ db/
SRC_DIRS_version/version           := version/ sstable/ iterator/ wal/ util/ db/
SRC_DIRS_version/version_set       := version/ sstable/ iterator/ wal/ util/ db/
SRC_DIRS_iterator/seek             := iterator/ sstable/ memtable/ util/
SRC_DIRS_db/db                     := db/ version/ memtable/ sstable/ wal/ iterator/ util/

define TEST_LINK_RULE
$(BIN_DIR)/$(1): $(TEST_BUILD)/$(1).o \
                 $(patsubst $(SRC_DIR)/%.cpp,$(SRC_BUILD)/%.o,$(foreach d,$(or $(SRC_DIRS_$(1)),$(dir $(1))),$(wildcard $(SRC_DIR)/$(d)*.cpp)))
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

# Compare arena and legacy memtables. Override the entry count with ARGS.
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
