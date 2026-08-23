#pragma once
#include <cstdint>
#include <mutex>
#include <span>
#include "wal_entry.h"

// Appends logical records to one write-ahead log, fragmenting them across 32 KB
// blocks so a torn tail costs at most the record being written.
//
// Thread-safety: addRecord() is safe to call concurrently. It holds an internal
// mutex, which is what keeps one record's header and payload contiguous and
// _file_offset consistent while several writers append. The lock lives here
// rather than in the caller because _file_offset is this class's invariant --
// and because a WalWriter is handed around by shared_ptr, so a caller-held lock
// would be a contract no holder of that pointer could discover.
//
// sync() deliberately does not take that lock; see its comment below.
class WalWriter {
public:
    explicit WalWriter(int fd, uint64_t initial_offset = 0);

    // Holding a mutex, so neither copyable nor movable. Callers that need to
    // share one hold it by shared_ptr, which is how MemTableManager passes it.
    WalWriter(const WalWriter&)            = delete;
    WalWriter& operator=(const WalWriter&) = delete;

    // Appends a complete logical record, fragmenting across 32 KB blocks as needed.
    // Throws std::system_error on I/O failure.
    void addRecord(std::span<const uint8_t> data);

    // Flushes written data to stable storage.
    //
    // Does not take the append lock, which is a deliberate choice rather than an
    // oversight: this touches no member state, and holding the lock across an
    // fsync would stall every appender behind a syscall three to five orders of
    // magnitude slower than the append itself. The guarantee callers need still
    // holds -- any record whose addRecord() has already returned is durable when
    // this returns -- and sweeping up a concurrent record too is harmless.
    void sync();

    // Returns the current file offset. Under concurrent appends this is a
    // snapshot that is stale the moment it is returned; it exists for
    // single-writer bookkeeping, such as resuming a log or recording where a
    // record landed.
    [[nodiscard]] uint64_t fileOffset() const {
        std::lock_guard<std::mutex> guard(_mutex);
        return _file_offset;
    }

private:
    // The three helpers below all run with _mutex already held by addRecord.

    // Writes a WAL fragment
    void emitFragment(WalRecordType type, std::span<const uint8_t> payload);

    // Pads out the remainder of the current block so the next record starts on
    // a block boundary
    void padToNextBlock();

    // Writes n bytes from buf.
    // Throws std::system_error if unable
    void writeAll(const void* buf, size_t n);

    // Returns the offset within the current block
    uint32_t blockOffset() const noexcept {
        return static_cast<uint32_t>(_file_offset % kBlockSize);
    }

    mutable std::mutex _mutex; // serializes appends and guards _file_offset
    int _fd;
    uint64_t _file_offset;
};
