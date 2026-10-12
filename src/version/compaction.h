#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "version/version.h"

struct CompactionPlan {
    std::shared_ptr<const Version> base;
    int source_level = 0;
    std::vector<FileHandle> source;
    std::vector<FileHandle> destination;
    std::string smallest_key;
    std::string largest_key;
};

// Chooses inputs without writing files. One picker is owned by each DB worker.
class CompactionPicker {
public:
    [[nodiscard]] std::optional<CompactionPlan> pick(std::shared_ptr<const Version> version);
    [[nodiscard]] std::optional<CompactionPlan> pickAtLevel(
        std::shared_ptr<const Version> version, int level);

private:
    std::array<uint64_t, kNumLevels> _cursors{};
};
