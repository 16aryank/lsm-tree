#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>

#include "key.h"
#include "memtable/memtable.h"
#include "sstable/sstable.h"
#include "sstable/sstable_builder.h"
#include "wal/wal_writer.h"

// How many times a flush is retried before the manager gives up on it for now
// and leaves the table for a later write to retry.
inline constexpr size_t kFlushMaxAttempts = 3;

// Waited between attempts, multiplied by the attempt number.
inline constexpr std::chrono::milliseconds kFlushRetryBackoff{ 50 };

// Owns the "live" write path for one column of an LSM engine: a mutable
// MemTable that absorbs new writes, and (at most) one immutable MemTable
// that a background thread is flushing to a Level-0 SSTable.

// Thread-safety: promote() takes exclusive lock, add()/get() take shared
// lock, and the I/O flush during promote() does not hold a lock
template <typename UserKey_, typename Value_>
class MemTableManager {
public:
    using MemTablePtr = std::shared_ptr<MemTable<UserKey_, Value_>>;
    using MemTableFactory = std::function<MemTablePtr()>;
    using WalFactory = std::function<std::shared_ptr<WalWriter>()>;

    // `sstable_dir` is where flushed tables are written, and is created if it
    // does not exist. Leaving it empty puts the manager in a memory-only mode
    // where a promoted table is dropped once it is no longer reachable rather
    // than persisted -- useful for benchmarks, never for real data.
    explicit MemTableManager(size_t size_threshold_bytes = kDefaultMemTableSizeBytes,
                              WalFactory wal_factory = nullptr,
                              std::filesystem::path sstable_dir = {})
        : _size_threshold_bytes(size_threshold_bytes),
          _memtable_factory([size_threshold_bytes] {
              return std::make_shared<MemTable<UserKey_, Value_>>(size_threshold_bytes);
          }),
          _wal_factory(std::move(wal_factory)),
          _sstable_dir(std::move(sstable_dir)) {
        if (!_sstable_dir.empty()) {
            std::filesystem::create_directories(_sstable_dir);
        }
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

    void add(uint64_t seq, ValueType type, const UserKey_& key, Value_ value) {
        MemTablePtr target;
        {
            // The lock covers the write, not just the pointer read. promote()
            // needs the exclusive lock, so holding it here is what stops a
            // write from landing in a table that has already been handed to a
            // flush -- which the flush would then be iterating underneath it.
            boost::shared_lock<boost::shared_mutex> lock(_pointer_mutex);
            target = _mutable;
            target->add(seq, type, key, std::move(value));
        }
        // Checked outside the lock because promote() needs the exclusive one.
        // By then `target` may already have been promoted by another writer,
        // which promote() sorts out.
        if (target->shouldFlush()) {
            promote();
        }
    }

    // Returns the value with the largest sequence number <= seq for key,
    // checking the mutable MemTable, then the immutable one, then the flushed
    // SSTables from newest to oldest. The first table holding any version of
    // the key decides the answer -- including a tombstone, which stops the
    // search rather than falling through to an older table.
    [[nodiscard]] std::optional<Value_> get(const UserKey_& key, uint64_t seq) const {
        MemTablePtr mutable_snapshot;
        MemTablePtr immutable_snapshot;
        std::vector<std::shared_ptr<SSTable>> sstables;
        {
            boost::shared_lock<boost::shared_mutex> lock(_pointer_mutex);
            mutable_snapshot = _mutable;
            immutable_snapshot = _immutable;
            sstables = _sstables;
        }
        if (auto value = mutable_snapshot->get(key, seq)) {
            return value;
        }
        if (immutable_snapshot) {
            if (auto value = immutable_snapshot->get(key, seq)) {
                return value;
            }
        }
        // Level 0 tables overlap, so they have to be walked newest first.
        const std::string_view key_bytes{ key.data(), key.size() };
        for (auto table = sstables.rbegin(); table != sstables.rend(); ++table) {
            std::optional<SSTable::Entry> entry = (*table)->get(key_bytes, seq);
            if (!entry) {
                continue;
            }
            if (entry->type == ValueType::kDelete) {
                return std::nullopt;
            }
            return Value_(entry->value.data(), entry->value.size());
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

    // Level-0 tables, oldest first.
    [[nodiscard]] std::vector<std::shared_ptr<SSTable>> sstables() const {
        boost::shared_lock<boost::shared_mutex> lock(_pointer_mutex);
        return _sstables;
    }

    // Why the last flush failed, or empty if the last one succeeded. A
    // non-empty value alongside hasPendingFlush() means the immutable table is
    // still in memory waiting to be retried.
    [[nodiscard]] std::string lastFlushError() const {
        boost::shared_lock<boost::shared_mutex> lock(_pointer_mutex);
        return _flush_error;
    }

    // Test/debug helper: blocks until any in-flight background flush
    // completes. Note that a completed flush is not necessarily a successful
    // one -- check lastFlushError().
    void waitForPendingFlush() {
        std::lock_guard<std::mutex> flush_guard(_flush_mutex);
        if (_flush_thread.joinable()) {
            _flush_thread.join();
        }
    }

private:
    // Atomically swaps the mutable MemTable out for a fresh one, handing the
    // old one to a background thread to flush.
    //
    // If a flush is already in flight this is a no-op: the caller stays
    // over-threshold and retries on its next write. If an earlier flush failed
    // and left its table behind, this re-arms that table instead of promoting
    // a new one -- writes must not stack up more than one immutable table, and
    // the stale one has to reach disk first anyway.
    void promote() {
        // Serializes promotion, so concurrent writers can't both touch
        // _flush_thread. Held across the join below, never during the flush.
        std::lock_guard<std::mutex> flush_guard(_flush_mutex);

        MemTablePtr to_flush;
        {
            boost::unique_lock<boost::shared_mutex> lock(_pointer_mutex);
            if (_immutable) {
                if (_flush_in_flight) {
                    return;
                }
                to_flush = _immutable; // previous attempt failed; try again
            } else if (_mutable->shouldFlush()) {
                to_flush = _mutable;
                _immutable = to_flush;
                _mutable = _memtable_factory();
                if (_wal_factory) {
                    _current_wal = _wal_factory();
                }
            } else {
                // Another writer rotated the table this caller saw, and the
                // fresh one has room; nothing to do.
                return;
            }
            _flush_in_flight = true;
        }

        // _flush_in_flight was false above, so any previous worker has already
        // run its last statement; this only reaps the thread object.
        if (_flush_thread.joinable()) {
            _flush_thread.join();
        }
        _flush_thread = std::thread([this, to_flush] { flushWorker(to_flush); });
    }

    // Writes `table` out, retrying a few times before giving up. A failed
    // flush is not data loss: the table stays reachable as _immutable and its
    // writes are still in the WAL, so the next promote() retries it.
    void flushWorker(MemTablePtr table) {
        std::shared_ptr<SSTable> flushed;
        std::string error;

        for (size_t attempt = 1; attempt <= kFlushMaxAttempts; ++attempt) {
            try {
                flushed = flushToSSTable(*table);
                error.clear();
                break;
            } catch (const std::exception& e) {
                // Typically a transient I/O error (a full or briefly
                // unavailable disk), which is what makes a retry worth trying.
                error = e.what();
                if (attempt < kFlushMaxAttempts) {
                    std::this_thread::sleep_for(kFlushRetryBackoff * attempt);
                }
            }
        }

        boost::unique_lock<boost::shared_mutex> lock(_pointer_mutex);
        if (error.empty()) {
            if (flushed) {
                _sstables.push_back(std::move(flushed));
            }
            // Published before the table is dropped, so a concurrent get()
            // sees the key in one place or the other, never neither.
            _immutable.reset();
        }
        _flush_error = std::move(error);
        _flush_in_flight = false;
    }

    // Writes every version of every key in `table` to a new SSTable and opens
    // it for reading. Returns nullptr when there is nothing to write, or when
    // no sstable directory was configured.
    //
    // The memtable iterator already yields entries in internal-key order, so
    // this is a straight copy with no sorting or buffering. It also walks the
    // table without holding its lock, which is only sound because a promoted
    // table is frozen: add() writes under the shared pointer lock that
    // promote() takes exclusively, so no write can still be in flight here.
    std::shared_ptr<SSTable> flushToSSTable(const MemTable<UserKey_, Value_>& table) {
        if (_sstable_dir.empty()) {
            return nullptr;
        }
        std::unique_ptr<typename MemTable<UserKey_, Value_>::Iterator> it(table.newIterator());
        if (!it->valid()) {
            return nullptr;
        }

        const std::filesystem::path path = nextSSTablePath();
        SSTableBuilder builder(path);
        for (; it->valid(); it->next()) {
            builder.add(it->key(), packSeqAndType(it->sequenceNumber(), it->valueType()),
                        it->value());
        }
        // A throw anywhere above unwinds through ~SSTableBuilder, which
        // removes the half-written file before the retry starts a new one.
        builder.finish();
        return SSTable::open(path);
    }

    [[nodiscard]] std::filesystem::path nextSSTablePath() {
        char name[32];
        std::snprintf(name, sizeof(name), "%06llu.sst",
                      static_cast<unsigned long long>(_next_file_number.fetch_add(1)));
        return _sstable_dir / name;
    }

    size_t _size_threshold_bytes;
    MemTableFactory _memtable_factory;
    WalFactory _wal_factory;
    std::filesystem::path _sstable_dir;

    mutable boost::shared_mutex _pointer_mutex;
    MemTablePtr _mutable;
    MemTablePtr _immutable;
    std::shared_ptr<WalWriter> _current_wal;
    std::vector<std::shared_ptr<SSTable>> _sstables;
    std::string _flush_error;
    bool _flush_in_flight{ false };

    std::mutex _flush_mutex;
    std::thread _flush_thread;
    std::atomic<uint64_t> _next_file_number{ 1 };
};
