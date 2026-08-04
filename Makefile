CXX      := g++
CXXFLAGS := -std=c++23 -Wall -Wextra -g
INCLUDES := -I. -Isrc -I/opt/homebrew/opt/boost/include

SRC_DIR   := src
TEST_DIR  := tests
BUILD_DIR := build
BIN_DIR   := bin

SRCS     := $(shell find $(SRC_DIR) -name '*.cpp')
SRC_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
DEPS     := $(SRC_OBJS:.o=.d) $(patsubst $(TEST_DIR)/%.cpp,$(BUILD_DIR)/%.d,$(shell find $(TEST_DIR) -name '*.cpp'))

TEST_SRCS := $(shell find $(TEST_DIR) -name '*.cpp')
TEST_BINS := $(patsubst $(TEST_DIR)/%.cpp,$(BIN_DIR)/%,$(TEST_SRCS))

.PHONY: all test clean check run
.PRECIOUS: $(BUILD_DIR)/%.o

all: test

# Compile any single .cpp for a sanity check without linking, e.g.:
#   make check FILE=src/wal/wal_reader.cpp
check:
ifndef FILE
	$(error Usage: make check FILE=path/to/file.cpp)
endif
	$(CXX) $(CXXFLAGS) $(INCLUDES) -fsyntax-only $(FILE)

# Build any object under src/ or tests/, e.g. `make build/wal/wal_reader.o`.
# Doubles as a per-file compilation sanity check that leaves the .o behind.
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: $(TEST_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# Link each test against the src objects in its matching subdirectory only
# (tests/foo/bar.cpp <-> src/foo/*.cpp), so an unrelated broken src file
# doesn't block tests that don't need it.
.SECONDEXPANSION:
$(BIN_DIR)/%: $(BUILD_DIR)/%.o $$(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$$(wildcard $(SRC_DIR)/$$(dir %)*.cpp))
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@

# Build and run every test, e.g. `make test`.
test: $(TEST_BINS)
	@for bin in $(TEST_BINS); do \
		echo "== running $$bin =="; \
		./$$bin || exit 1; \
	done

# Build and run a single test, e.g. `make run TEST=memtable/skiplist`.
run: $(BIN_DIR)/$(TEST)
ifndef TEST
	$(error Usage: make run TEST=memtable/skiplist)
endif
	./$(BIN_DIR)/$(TEST)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

-include $(DEPS)
