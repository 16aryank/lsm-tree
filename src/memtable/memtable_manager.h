#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>

#include "key.h"
#include "memtable/memtable.h"
#include "wal/wal_writer.h"

// SSTable does not exist yet -- flushToSSTable() below is an intentional
// stub. Only a raw pointer to this incomplete type is ever used, so nothing
// here requires SSTable to be defined.
class SSTable;

// Owns the "live" write path for one column of an LSM engine: a mutable
// MemTable that absorbs new writes, and (at most) one immutable MemTable
// that a background thread is flushing to a Level-0 SSTable.

//   Thread-safety: the pointer swap in promote() and the pointer reads in add()/get()
//   go through the same boost::shared_mutex: reads take the shared lock,
//   promotion takes the exclusive lock, making promotion atomic with
//   respect to concurrent readers. The exclusive lock is held only for the swap itself,
//   not for the flush I/O that follows
template <typename UserKey_, typename Value_>
class MemTableManager {
public:
    using MemTablePtr = std::shared_ptr<MemTable<UserKey_, Value_>>;
    using MemTableFactory = std::function<MemTablePtr()>;
    using WalFactory = std::function<std::shared_ptr<WalWriter>()>;

    // wal_factory is optional: when supplied, it is invoked once at
    // construction and again on every promotion to obtain a fresh WAL to
    // pair with the fresh mutable MemTable, as currentWal().
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

    ~MemTableManager() {
        if (_flush_thread.joinable()) {
            _flush_thread.join();
        }
    }

    MemTableManager(const MemTableManager&) = delete;
    MemTableManager& operator=(const MemTableManager&) = delete;

    // Writes to the current mutable MemTable, promoting it to immutable
    // (and starting a background flush) if this write tips it over the
    // size threshold.
    void add(uint64_t seq, ValueType type, const UserKey_& key, Value_ value) {
        MemTablePtr target;
        {
            boost::shared_lock<boost::shared_mutex> lock(_pointer_mutex);
            target = _mutable;
        }
        target->add(seq, type, key, std::move(value));
        if (target->shouldFlush()) {
            promote();
        }
    }

    // Returns the value with the largest sequence number <= seq for key,
    // checking the mutable MemTable, then the immutable one, if present.
    // Does not consult any SSTable -- Level 0+ lookups land here once
    // SSTable exists.
    [[nodiscard]] std::optional<Value_> get(const UserKey_& key, uint64_t seq) const {
        MemTablePtr mutable_snapshot;
        MemTablePtr immutable_snapshot;
        {
            boost::shared_lock<boost::shared_mutex> lock(_pointer_mutex);
            mutable_snapshot = _mutable;
            immutable_snapshot = _immutable;
        }
        if (auto value = mutable_snapshot->get(key, seq)) {
            return value;
        }
        if (immutable_snapshot) {
            if (auto value = immutable_snapshot->get(key, seq)) {
                return value;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::shared_ptr<WalWriter> currentWal() const {
        boost::shared_lock<boost::shared_mutex> lock(_pointer_mutex);
        return _current_wal;
    }

    [[nodiscard]] bool hasPendingFlush() const {
        boost::shared_lock<boost::shared_mutex> lock(_pointer_mutex);
        return _immutable != nullptr;
    }

    // Test/debug helper: blocks until any in-flight background flush
    // completes. Not safe to call concurrently with another thread driving
    // promotion.
    void waitForPendingFlush() {
        if (_flush_thread.joinable()) {
            _flush_thread.join();
        }
    }

private:
    // Atomically swaps the mutable MemTable out for a fresh one, handing
    // the old one to a background thread to flush. If a previous flush is
    // still in flight (an immutable table is already pending), this is a
    // no-op: the caller stays over-threshold and will retry promotion on
    // its next write, once the in-flight flush clears _immutable.
    void promote() {
        MemTablePtr old_mutable;
        {
            boost::unique_lock<boost::shared_mutex> lock(_pointer_mutex);
            if (_immutable) {
                return;
            }
            old_mutable = _mutable;
            _immutable = old_mutable;
            _mutable = _memtable_factory();
            if (_wal_factory) {
                _current_wal = _wal_factory();
            }
        }

        // Don't hold lock during the flush process for increased concurrency
        // Need to have some sort of safegaurds against write failures / impelment retries

        if (_flush_thread.joinable()) {
            _flush_thread.join();
        }
        _flush_thread = std::thread([this, old_mutable] { flushWorker(old_mutable); });
    }

    // TODO: Implement
    void flushWorker(MemTablePtr table) {
        flushToSSTable(*table);
        boost::unique_lock<boost::shared_mutex> lock(_pointer_mutex);
        _immutable.reset();
    }

    // TODO: Implement
    SSTable* flushToSSTable(const MemTable<UserKey_, Value_>& table) {
        (void)table;
        return nullptr;
    }

    size_t _size_threshold_bytes;
    MemTableFactory _memtable_factory;
    WalFactory _wal_factory;

    mutable boost::shared_mutex _pointer_mutex;
    MemTablePtr _mutable;
    MemTablePtr _immutable;
    std::shared_ptr<WalWriter> _current_wal;

    std::thread _flush_thread;
};
