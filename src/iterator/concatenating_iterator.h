#pragma once
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "iterator/iterator.h"
#include "sstable/sstable.h"

// One sorted run of non-overlapping tables, presented as a single iterator.
//
// The caller supplies disjoint tables in key order. Only one table iterator
// exists at a time.
class ConcatenatingIterator final : public InternalIterator {
public:
    // Tables with no entries are dropped on the way in: they have no key range
    // to binary search against, and they can never satisfy a seek.
    explicit ConcatenatingIterator(std::vector<std::shared_ptr<const SSTable>> tables);

    [[nodiscard]] bool valid() const noexcept override;
    void next() override;

    void seek(std::string_view key, uint64_t tag) override;
    void seekToFirst() override;

    [[nodiscard]] std::string_view key() const noexcept override;
    [[nodiscard]] uint64_t tag() const noexcept override;
    [[nodiscard]] std::string_view value() const noexcept override;

private:
    // Opens the first table at or after `index` that yields an entry, and
    // positions on it. Leaves the iterator invalid if there is none.
    void openFrom(size_t index);

    std::vector<std::shared_ptr<const SSTable>> _tables;
    size_t                                     _index{ 0 };
    std::unique_ptr<InternalIterator>          _it;
};

[[nodiscard]] std::unique_ptr<InternalIterator> newConcatenatingIterator(
    std::vector<std::shared_ptr<const SSTable>> tables);
