#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>

#include "key.h"
#include "memtable/memtable.h"
#include "wal/wal_codec.h"
#include "wal/wal_writer.h"

// A write-ahead log and the number that identifies it. The number is what a
// manifest edit records, so rotation has to hand both back together.
struct WalHandle {
    uint64_t                   number{ 0 };
    std::shared_ptr<WalWriter> writer;
};

// Owns one mutable memtable, at most one immutable memtable, and the current
// WAL. Callers commit a flushed table before calling dropImmutable().
// Pointer swaps use an exclusive lock; add() and lookup() use a shared lock.
template <typename UserKey_, typename Value_>
class MemTableManager {
public:
    using MemTablePtr     = std::shared_ptr<MemTable<UserKey_, Value_>>;
    using MemTableFactory = std::function<MemTablePtr()>;
    using WalFactory      = std::function<WalHandle()>;

    // What promote() froze.
    struct Promotion {
        // The table to flush, or null when there is nothing to do.
        MemTablePtr table;
        // The log now taking writes. Everything in `table` was written to a
        // log numbered below this one, so once the flush is recorded, those
        // logs are deletable -- which is exactly what a manifest edit's log
        // number means.
        uint64_t log_number{ 0 };
    };

private:
    static std::string_view bytesOf(const auto& value) noexcept {
        return { value.data(), value.size() };
    }

public:
    // A null wal_factory supports in-memory tests and benchmarks.
    explicit MemTableManager(size_t size_threshold_bytes = kDefaultMemTableSizeBytes,
                             WalFactory wal_factory = nullptr)
        : _size_threshold_bytes(size_threshold_bytes),
          _memtable_factory([size_threshold_bytes] {
              return std::make_shared<MemTable<UserKey_, Value_>>(size_threshold_bytes);
          }),
          _wal_factory(std::move(wal_factory)) {
        _mutable = _memtable_factory();
        if (_wal_factory) {
            _current_wal = _wal_factory();
        }
    }

    MemTableManager(const MemTableManager&)            = delete;
    MemTableManager& operator=(const MemTableManager&) = delete;

    void add(uint64_t seq, ValueType type, const UserKey_& key, Value_ value) {
        boost::shared_lock<boost::shared_mutex> lock(_pointer_mutex);

        // Hold the pointer lock across WAL append and insert so rotation cannot
        // separate a write from the log covering its memtable.
        if (_current_wal.writer) {
            const std::string record =
                encodeWalEntry(seq, type, bytesOf(key), bytesOf(value));
            _current_wal.writer->addRecord(byteSpan(record));
        }

        _mutable->add(seq, type, key, std::move(value));
    }

    // Searches the mutable table and then the immutable one. kNotFound means
    // neither had anything to say and the caller should go on to the tables on
    // disk; kDeleted means a tombstone covers the key and it must not.
    [[nodiscard]] LookupStatus lookup(const UserKey_& key, uint64_t seq, Value_& out) const {
        // Pin both tables across lookup; the sequence number bounds visibility.
        MemTablePtr mutable_snapshot;
        MemTablePtr immutable_snapshot;
        {
            boost::shared_lock<boost::shared_mutex> lock(_pointer_mutex);
            mutable_snapshot   = _mutable;
            immutable_snapshot = _immutable;
        }

        const LookupStatus status = mutable_snapshot->lookup(key, seq, out);
        if (status != LookupStatus::kNotFound) {
            return status;
        }
        if (immutable_snapshot) {
            return immutable_snapshot->lookup(key, seq, out);
        }
        return LookupStatus::kNotFound;
    }

    // True once the mutable table is over its size threshold, so the caller
    // knows to promote. Checked outside add() because promote() needs the
    // exclusive lock that add() holds shared.
    [[nodiscard]] bool shouldPromote() const {
        boost::shared_lock<boost::shared_mutex> lock(_pointer_mutex);
        return _mutable->shouldFlush();
    }

    [[nodiscard]] WalHandle currentWal() const {
        boost::shared_lock<boost::shared_mutex> lock(_pointer_mutex);
        return _current_wal;
    }

    [[nodiscard]] bool hasPendingFlush() const {
        boost::shared_lock<boost::shared_mutex> lock(_pointer_mutex);
        return _immutable != nullptr;
    }

    // Freezes the mutable table, installs a fresh one and a fresh log, and
    // returns the frozen table to be flushed.
    //
    // Returns an empty Promotion if the table has room or a flush is active.
    // A failed flush is retried before another table is frozen.
    [[nodiscard]] Promotion promote() {
        boost::unique_lock<boost::shared_mutex> lock(_pointer_mutex);

        MemTablePtr to_flush;
        if (_immutable) {
            if (_flush_in_flight) {
                return { };
            }
            to_flush = _immutable;
        } else if (_mutable->shouldFlush()) {
            to_flush   = _mutable;
            _immutable = to_flush;
            _mutable   = _memtable_factory();
            if (_wal_factory) {
                _current_wal = _wal_factory();
            }
        } else {
            return { };
        }

        _flush_in_flight = true;
        return { to_flush, _current_wal.number };
    }

    // Retires the frozen table after its SSTable is committed to the manifest.
    void dropImmutable() {
        boost::unique_lock<boost::shared_mutex> lock(_pointer_mutex);
        _immutable.reset();
        _flush_in_flight = false;
    }

    // Keeps the frozen table for a later retry.
    void abandonFlush() {
        boost::unique_lock<boost::shared_mutex> lock(_pointer_mutex);
        _flush_in_flight = false;
    }

private:
    size_t                _size_threshold_bytes;
    MemTableFactory       _memtable_factory;
    WalFactory            _wal_factory;

    mutable boost::shared_mutex _pointer_mutex;
    MemTablePtr                 _mutable;
    MemTablePtr                 _immutable;
    WalHandle                   _current_wal;
    bool                        _flush_in_flight{ false };
};
