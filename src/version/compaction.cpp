#include "version/compaction.h"

#include <algorithm>
#include <utility>

namespace {

void widen(CompactionPlan& plan) {
    bool first = true;
    for (const auto* inputs : { &plan.source, &plan.destination }) {
        for (const auto& file : *inputs) {
            if (first) {
                plan.smallest_key = file.meta.smallest_key;
                plan.largest_key = file.meta.largest_key;
                first = false;
            } else {
                plan.smallest_key = std::min(plan.smallest_key, file.meta.smallest_key);
                plan.largest_key = std::max(plan.largest_key, file.meta.largest_key);
            }
        }
    }
}

} // namespace

std::optional<CompactionPlan> CompactionPicker::pick(std::shared_ptr<const Version> version) {
    if (!version || version->compactionScore() < 1.0) {
        return std::nullopt;
    }
    const int level = version->compactionLevel();
    return pickAtLevel(std::move(version), level);
}

std::optional<CompactionPlan> CompactionPicker::pickAtLevel(
    std::shared_ptr<const Version> version, int level) {
    if (!version || level < 0 || level >= kNumLevels - 1) {
        return std::nullopt;
    }
    CompactionPlan plan;
    plan.base = std::move(version);
    plan.source_level = level;
    const auto& files = plan.base->files(plan.source_level);
    if (files.empty() || plan.source_level >= kNumLevels - 1) {
        return std::nullopt;
    }

    if (plan.source_level == 0) {
        plan.source = files;
    } else {
        const uint64_t cursor = _cursors[static_cast<size_t>(plan.source_level)];
        const FileHandle* next = nullptr;
        for (const auto& file : files) {
            if (file.meta.number > cursor &&
                (!next || file.meta.number < next->meta.number)) {
                next = &file;
            }
        }
        if (!next) {
            next = &*std::min_element(files.begin(), files.end(),
                                      [](const FileHandle& a, const FileHandle& b) {
                return a.meta.number < b.meta.number;
            });
        }
        plan.source.push_back(*next);
        _cursors[static_cast<size_t>(plan.source_level)] = next->meta.number;
    }

    for (;;) {
        widen(plan);
        auto source = plan.base->overlappingInputs(plan.source_level, plan.smallest_key,
                                                   plan.largest_key);
        auto destination = plan.base->overlappingInputs(plan.source_level + 1,
                                                        plan.smallest_key, plan.largest_key);
        if (source.size() == plan.source.size() && destination.size() == plan.destination.size()) {
            break;
        }
        plan.source = std::move(source);
        plan.destination = std::move(destination);
    }
    widen(plan);
    return plan;
}
