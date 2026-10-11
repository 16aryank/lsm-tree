#pragma once
#include <cstdint>
#include <mutex>
#include <span>
#include "wal_entry.h"

// Appends checked logical records across 32 KiB blocks. addRecord() serializes
// complete records across concurrent callers.
class WalWriter {
public:
    explicit WalWriter(int fd, uint64_t initial_offset = 0);

    WalWriter(const WalWriter&)            = delete;
    WalWriter& operator=(const WalWriter&) = delete;

    // Appends a complete logical record, fragmenting across 32 KB blocks as needed.
    // Throws std::system_error on I/O failure.
    void addRecord(std::span<const uint8_t> data);

    // Flushes written data to stable storage.
    //
    // Does not take the append lock. Records appended before sync() starts are
    // durable when it returns; concurrent appends may be included as well.
    void sync();

    // Snapshot of the append offset, primarily for single-writer bookkeeping.
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
