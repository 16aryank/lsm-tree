#pragma once
#include <memory>
#include <string_view>
#include <utility>

#include "iterator/iterator.h"
#include "memtable/memtable.h"

// Adapts MemTable<K,V>::Iterator to InternalIterator. The keys and values are views 
// into the table's arena, so the cursor must not outlive it.
template <typename UserKey_, typename Value_>
class MemTableIterator final : public InternalIterator {
public:
    using Table = MemTable<UserKey_, Value_>;

    explicit MemTableIterator(std::shared_ptr<const Table> table)
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
    std::shared_ptr<const Table> _table;
    typename Table::Iterator _it;
};
