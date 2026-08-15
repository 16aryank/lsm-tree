#pragma once
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>

#include "key.h"
#include "deprecated/skiplist.h"

// DEPRECATED -- the pre-arena memtable, kept only so the benchmark can measure
// the arena rewrite against it. Superseded by src/memtable/memtable.h.
//
// Its size accounting is the reason the rewrite happened: kEntryOverheadBytes
// models only the skip list's own bookkeeping and ignores the std::string heap
// buffers each entry actually owns, so it reports roughly 254 B/entry for a
// 50-byte key and 100-byte value that really costs ~384 B resident. This is
// an unacceptable memory overhead cost, making ~66% of the overhead from bookkeeping.
//
// Everything lives in namespace deprecated so this header can be included
// alongside the current one in a single translation unit.
namespace deprecated {

inline constexpr size_t kDefaultMemTableSizeBytes = 4ull << 20; // 4 MB

// Orders by user key ascending, then by (sequence number, type) descending
template <typename UserKey_>
struct InternalKey {
    Key<UserKey_> tagged_key;

    [[deprecated("pre-arena internal key; ordering now lives in ::InternalKeyComparator")]]
    bool operator<(const InternalKey& other) const
        requires requires(const UserKey_& a, const UserKey_& b) {
            { a < b } -> std::convertible_to<bool>;
        }
    {
        const UserKey_& a = tagged_key._user_key;
        const UserKey_& b = other.tagged_key._user_key;
        if (a < b) return true;
        if (b < a) return false;
        return tagged_key._seqnum_type > other.tagged_key._seqnum_type;
    }

    [[deprecated("pre-arena internal key; ordering now lives in ::InternalKeyComparator")]]
    bool operator==(const InternalKey& other) const
        requires requires(const UserKey_& a, const UserKey_& b) {
            { a == b } -> std::convertible_to<bool>;
        }
    {
        return tagged_key._seqnum_type == other.tagged_key._seqnum_type &&
               tagged_key._user_key == other.tagged_key._user_key;
    }
};

// Substituted for InternalKey<UserKey_> as the skip list's key type when
// UserKey_ does not satisfy Orderable. This keeps SkipList<TableKey, Value_>
// and therefore MemTable<UserKey_, Value_> itself always a valid
// instantiation, so construction fails with a catchable std::invalid_argument
// from the MemTable constructor rather than a compile error.
struct UnorderableKeyPlaceholder {
    [[deprecated("pre-arena placeholder key")]]
    bool operator<(const UnorderableKeyPlaceholder&) const { return false; }

    [[deprecated("pre-arena placeholder key")]]
    bool operator==(const UnorderableKeyPlaceholder&) const { return true; }
};

namespace memtable_detail {

// Best-effort byte-size estimate for the memtable's size accounting.
// Falls back to sizeof(T) for fixed-size/POD-ish types; uses .size() for
// container-like types since sizeof() only reports the control-block size.
template <typename T>
[[deprecated("pre-arena size estimate; the arena now reports exact bytes")]]
size_t approxByteSize(const T& value) {
    if constexpr (requires { { value.size() } -> std::convertible_to<size_t>; }) {
        return static_cast<size_t>(value.size());
    } else {
        return sizeof(T);
    }
}

// SkipList uses p=0.5 for level promotion; a node's expected number of forward pointers is 1/(1-p) = 2.
inline constexpr size_t kExpectedForwardLevels = 2;

// Per-entry bookkeeping the skip list adds on top of the raw key/value
// bytes, derived from SkipList::Node's layout:
//   - the std::shared_ptr<Value_> handle stored inline in Node
//   - std::vector<shared_ptr<Node>> forward's inline {begin,end,cap} control
//     block
//   - forward's heap-allocated backing array, sized to the node's expected
//     level (kExpectedForwardLevels shared_ptr<Node> slots)
//   - the two heap allocations' control blocks from make_shared<Node> and
//     make_shared<Value_> (refcounts + deleter, ~2 pointers each)
//
// Known to undercount by more than 2x: it omits the key's and value's own
// string buffers, std::optional's engaged flag, and malloc bucket slack.
inline constexpr size_t kEntryOverheadBytes =
    sizeof(std::shared_ptr<void>) +
    sizeof(std::vector<std::shared_ptr<void>>) +
    kExpectedForwardLevels * sizeof(std::shared_ptr<void>) +
    2 * (2 * sizeof(void*));

} // namespace memtable_detail

// An in-memory table of versioned key/value entries backed by a SkipList.
// Thread-safety: guarded internally by a boost::shared_mutex. add() takes the
// exclusive lock; get() and newIterator() take the shared lock
template <typename UserKey_, typename Value_>
class MemTable {
private:
    using VersionedKey = InternalKey<UserKey_>;
    using TableKey = std::conditional_t<Orderable<VersionedKey>, VersionedKey, UnorderableKeyPlaceholder>;
    using Table = SkipList<TableKey, Value_>;

public:
    // Forward-only cursor over all versions of all keys, in InternalKey
    // order (user key ascending, newest version of each key first).
    class Iterator {
    public:
        [[deprecated("pre-arena iterator")]]
        [[nodiscard]] bool valid() const noexcept { return _it.valid(); }

        [[deprecated("pre-arena iterator")]]
        void next() { _it.next(); }

        [[deprecated("pre-arena iterator")]]
        [[nodiscard]] const UserKey_& key() const { return _it.key().tagged_key._user_key; }

        [[deprecated("pre-arena iterator")]]
        [[nodiscard]] uint64_t sequenceNumber() const { return unpackSeqNumber(_it.key().tagged_key._seqnum_type); }

        [[deprecated("pre-arena iterator")]]
        [[nodiscard]] ValueType valueType() const { return unpackValueType(_it.key().tagged_key._seqnum_type); }

        [[deprecated("pre-arena iterator")]]
        [[nodiscard]] const Value_& value() const { return _it.value(); }

    private:
        friend class MemTable;
        explicit Iterator(typename Table::Iterator it) : _it(std::move(it)) {}

        typename Table::Iterator _it;
    };

    [[deprecated("pre-arena memtable; use ::MemTable from memtable/memtable.h")]]
    explicit MemTable(size_t size_threshold_bytes = kDefaultMemTableSizeBytes)
        : _size_threshold_bytes(size_threshold_bytes) {
        if constexpr (!Orderable<VersionedKey>) {
            throw std::invalid_argument(
                "MemTable: key type does not satisfy the Orderable concept required by the underlying SkipList");
        }
    }

    // Records a versioned write. type == ValueType::kDelete stores a
    // tombstone for `key`; the accompanying value is ignored by get() but still stored.
    [[deprecated("pre-arena memtable; use ::MemTable from memtable/memtable.h")]]
    void add(uint64_t seq, ValueType type, const UserKey_& key, Value_ value) {
        boost::unique_lock<boost::shared_mutex> lock(_mutex);
        TableKey internal_key{ Key<UserKey_>{ key, packSeqAndType(seq, type) } };
        _approx_size_bytes.fetch_add(
            memtable_detail::approxByteSize(key) +
                memtable_detail::approxByteSize(value) +
                memtable_detail::kEntryOverheadBytes,
            std::memory_order_relaxed
        );
        _table.insert(internal_key, std::move(value));
    }

    // Returns the value with the largest sequence number <= seq for key, or
    // std::nullopt not found or newest is a tombstone.
    [[deprecated("pre-arena memtable; use ::MemTable from memtable/memtable.h")]]
    [[nodiscard]] std::optional<Value_> get(const UserKey_& key, uint64_t seq) const {
        boost::shared_lock<boost::shared_mutex> lock(_mutex);
        TableKey lookup{ Key<UserKey_>{ key, packSeqAndType(seq, ValueType::kValue) } };
        typename Table::Iterator it = _table.seek(lookup);
        if (!it.valid() || it.key().tagged_key._user_key != key) {
            return std::nullopt;
        }
        if (unpackValueType(it.key().tagged_key._seqnum_type) == ValueType::kDelete) {
            return std::nullopt;
        }
        return it.value();
    }

    // Caller owns the returned Iterator.
    [[deprecated("pre-arena memtable; use ::MemTable from memtable/memtable.h")]]
    [[nodiscard]] Iterator* newIterator() const {
        boost::shared_lock<boost::shared_mutex> lock(_mutex);
        return new Iterator(_table.begin());
    }

    [[deprecated("pre-arena size estimate; the arena now reports exact bytes")]]
    [[nodiscard]] size_t approximateMemoryUsage() const noexcept {
        return _approx_size_bytes.load(std::memory_order_relaxed);
    }

    [[deprecated("pre-arena memtable; use ::MemTable from memtable/memtable.h")]]
    [[nodiscard]] bool shouldFlush() const noexcept {
        return approximateMemoryUsage() >= _size_threshold_bytes;
    }

private:
    mutable boost::shared_mutex _mutex;
    std::atomic<size_t> _approx_size_bytes{ 0 };
    size_t _size_threshold_bytes;
    Table _table;
};

} // namespace deprecated
