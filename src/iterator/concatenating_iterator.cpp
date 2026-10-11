#include "iterator/concatenating_iterator.h"

#include <algorithm>
#include <utility>

#include "iterator/sstable_iterator.h"
#include "key.h"

ConcatenatingIterator::ConcatenatingIterator(std::vector<std::shared_ptr<const SSTable>> tables)
    : _tables(std::move(tables)) {
    std::erase_if(_tables, [](const std::shared_ptr<const SSTable>& table) {
        return table == nullptr || table->numEntries() == 0;
    });
    openFrom(0);
}

void ConcatenatingIterator::openFrom(size_t index) {
    for (size_t i = index; i < _tables.size(); ++i) {
        auto it = std::make_unique<SSTableIterator>(_tables[i]);
        if (it->valid()) {
            _index = i;
            _it    = std::move(it);
            return;
        }
        // A table that reported entries but yields none is degenerate rather
        // than fatal; skipping it keeps the run readable.
    }
    _index = _tables.size();
    _it.reset();
}

bool ConcatenatingIterator::valid() const noexcept {
    return _it != nullptr && _it->valid();
}

void ConcatenatingIterator::next() {
    if (!valid()) {
        return;
    }
    _it->next();
    if (!_it->valid()) {
        openFrom(_index + 1);
    }
}

void ConcatenatingIterator::seek(std::string_view key, uint64_t tag) {
    const InternalKeyComparator compare;

    // Tables are keyed by their largest entry, so the first table whose
    // largest key is >= the target is the only one that can hold it -- the
    // same argument the block index inside a table rests on, one level up.
    const auto table = std::partition_point(
        _tables.begin(), _tables.end(), [&](const std::shared_ptr<const SSTable>& t) {
            return compare(t->largestKey(), t->largestTag(), key, tag) < 0;
        });

    const size_t index = static_cast<size_t>(table - _tables.begin());
    if (index == _tables.size()) {
        _index = _tables.size();
        _it.reset();
        return;
    }

    auto it = std::make_unique<SSTableIterator>(_tables[index]);
    it->seek(key, tag);
    if (it->valid()) {
        _index = index;
        _it    = std::move(it);
        return;
    }
    // The target sorts after everything in the table the index picked, which
    // happens when it falls in the gap between this table's range and the
    // next's. The answer is then the next table's first entry.
    openFrom(index + 1);
}

void ConcatenatingIterator::seekToFirst() {
    openFrom(0);
}

std::string_view ConcatenatingIterator::key() const noexcept { return _it->key(); }
uint64_t ConcatenatingIterator::tag() const noexcept { return _it->tag(); }
std::string_view ConcatenatingIterator::value() const noexcept { return _it->value(); }

std::unique_ptr<InternalIterator> newConcatenatingIterator(
    std::vector<std::shared_ptr<const SSTable>> tables) {
    return std::make_unique<ConcatenatingIterator>(std::move(tables));
}
