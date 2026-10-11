#pragma once
#include <memory>
#include <string_view>
#include <utility>

#include "iterator/iterator.h"
#include "sstable/sstable.h"

// SSTable::Iterator holds a bare const SSTable* and does nothing to keep the
// table alive. Owning the shared_ptr here means a merge can safely outlive
// whatever handed it the table.
class SSTableIterator final : public InternalIterator {
public:
    explicit SSTableIterator(std::shared_ptr<const SSTable> table)
        : _table(std::move(table)), _it(_table->cursor()) { }

    [[nodiscard]] bool valid() const noexcept override { return _it.valid(); }
    void next() override { _it.next(); }

    void seek(std::string_view key, uint64_t tag) override { _it.seek(key, tag); }
    void seekToFirst() override { _it.seekToFirst(); }

    [[nodiscard]] std::string_view key() const noexcept override { return _it.key(); }
    [[nodiscard]] uint64_t tag() const noexcept override { return _it.tag(); }
    [[nodiscard]] std::string_view value() const noexcept override { return _it.value(); }

private:
    // Declaration order is load-bearing: _table is destroyed last, so the raw
    // table pointer inside _it stays valid for the whole of _it's lifetime.
    std::shared_ptr<const SSTable> _table;
    SSTable::Iterator _it;
};
