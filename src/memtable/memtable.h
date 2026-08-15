#pragma once
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>

#include "key.h"
#include "memtable/arena.h"
#include "memtable/skiplist.h"

inline constexpr size_t kDefaultMemTableSizeBytes = 4ull << 20; // 4 MB

// A memtable can store a contiguous run of bytes it can copy into its
// arena. Satisfied by std::string, std::string_view, std::vector<char>.
template <typename T>
concept ByteRange = requires(const T& value) {
    { value.data() } -> std::convertible_to<const char*>;
    { value.size() } -> std::convertible_to<size_t>;
};

// Values have to be rebuildable from those bytes on the way back
// out, since get() hands the caller an owning Value_ rather than a view into
// the arena.
template <typename T>
concept ByteStorable = ByteRange<T> && std::constructible_from<T, const char*, size_t>;

// Entries are copied into an Arena as raw bytes, one allocation per entry,
// with the key and value stored inline in the node. Nothing is freed
// individually -- the arena releases everything at once when the table is
// destroyed after its flush.
//
// Thread-safety: guarded internally by a boost::shared_mutex. add() takes the
// exclusive lock; get() and newIterator() take the shared lock.
template <typename UserKey_, typename Value_>
class MemTable {
private:
    using Table = SkipList<>;

    static std::string_view bytesOf(const ByteRange auto& value) noexcept {
        return { value.data(), value.size() };
    }

public:
    // Forward-only cursor over all versions of all keys, in internal-key
    // order (user key ascending, newest version of each key first).
    //
    // key() and value() are views into the arena, valid only while the
    // MemTable that produced this iterator is alive.
    class Iterator {
    public:
        [[nodiscard]] bool valid() const noexcept { return _it.valid(); }
        void next() { _it.next(); }

        [[nodiscard]] std::string_view key() const noexcept { return _it.key(); }
        [[nodiscard]] uint64_t sequenceNumber() const noexcept { return unpackSeqNumber(_it.tag()); }
        [[nodiscard]] ValueType valueType() const noexcept { return unpackValueType(_it.tag()); }
        [[nodiscard]] std::string_view value() const noexcept { return _it.value(); }

    private:
        friend class MemTable;
        explicit Iterator(typename Table::Iterator it) noexcept : _it(it) { }

        typename Table::Iterator _it;
    };

    explicit MemTable(size_t size_threshold_bytes = kDefaultMemTableSizeBytes)
        : _size_threshold_bytes(size_threshold_bytes), _table(_arena)
        {
        if constexpr (!ByteRange<UserKey_>) {
            throw std::invalid_argument(
                "MemTable: key type does not satisfy the ByteRange concept required by the underlying SkipList");
        }
        _base_bytes = _arena.bytesAllocated();
    }

    // Records a versioned write. type == ValueType::kDelete stores a
    // tombstone for `key`; the accompanying value is ignored by get() but
    // still stored. Both key and value are copied into the arena.
    void add(uint64_t seq, ValueType type, const UserKey_& key, Value_ value) {
        boost::unique_lock<boost::shared_mutex> lock(_mutex);
        _table.insert(bytesOf(key), packSeqAndType(seq, type), bytesOf(value));
    }

    // Returns the value with the largest sequence number <= seq for key, or
    // std::nullopt if not found or the newest is a tombstone.
    [[nodiscard]] std::optional<Value_> get(const UserKey_& key, uint64_t seq) const
        requires ByteStorable<Value_>
    {
        boost::shared_lock<boost::shared_mutex> lock(_mutex);
        const std::string_view key_bytes = bytesOf(key);

        // Tags sort descending within a key, so lower_bound on
        // (key, seq|kValue) lands on the newest version at or below seq.
        typename Table::Iterator it = _table.seek(key_bytes, packSeqAndType(seq, ValueType::kValue));
        if (!it.valid() || it.key() != key_bytes) {
            return std::nullopt;
        }
        if (unpackValueType(it.tag()) == ValueType::kDelete) {
            return std::nullopt;
        }
        const std::string_view value_bytes = it.value();
        return Value_(value_bytes.data(), value_bytes.size());
    }

    // Caller owns the returned Iterator, which must not outlive this MemTable.
    [[nodiscard]] Iterator* newIterator() const {
        boost::shared_lock<boost::shared_mutex> lock(_mutex);
        return new Iterator(_table.begin());
    }

    // Exact count of entry bytes handed out by the arena, excluding the skip
    // list's head sentinel. Every byte an entry costs comes from one 
    // arena allocation, so this accurate.
    [[nodiscard]] size_t approximateMemoryUsage() const noexcept {
        return _arena.bytesAllocated() - _base_bytes;
    }

    [[nodiscard]] size_t arenaMemoryUsage() const noexcept { return _arena.bytesReserved(); }

    [[nodiscard]] bool shouldFlush() const noexcept {
        return approximateMemoryUsage() >= _size_threshold_bytes;
    }

private:
    mutable boost::shared_mutex _mutex;
    size_t _size_threshold_bytes;

    // Declared before _table: the skip list holds a reference to this arena
    // and allocates its head sentinel from it during construction.
    Arena _arena;
    Table _table;

    size_t _base_bytes{ 0 };
};
