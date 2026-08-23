#pragma once
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "iterator/iterator.h"

// Yields the union of its children in internal-key order. Each child must
// already be in that order -- this merges, it does not sort.
//
// Duplicate internal keys are all yielded, never deduplicated: deciding which
// version of a key survives is a compaction's drop rule. When two children sit on the same 
// (key, tag) the earlier child wins, so callers pass their newest input first -- which is the
// case that arises between overlapping Level-0 files.
class MergingIterator final : public InternalIterator {
public:
    explicit MergingIterator(std::vector<std::unique_ptr<InternalIterator>> children);

    [[nodiscard]] bool valid() const noexcept override { return _current != nullptr; }
    void next() override;

    [[nodiscard]] inline std::string_view key() const noexcept override { return _current->key(); }
    [[nodiscard]] inline uint64_t tag() const noexcept override { return _current->tag(); }
    [[nodiscard]] inline std::string_view value() const noexcept override { return _current->value(); }

private:
    // Points _current at the smallest-keyed valid child, or null if there is
    // none left.
    void findSmallest();

    std::vector<std::unique_ptr<InternalIterator>> _children;
    InternalIterator* _current{ nullptr };
};

[[nodiscard]] std::unique_ptr<InternalIterator> newMergingIterator(
    std::vector<std::unique_ptr<InternalIterator>> children);