#pragma once
#include <cstdint>
#include <span>
#include "wal_entry.h"

class WalWriter {
public:
    explicit WalWriter(int fd, uint64_t initial_offset = 0);

    // Appends a complete logical record, fragmenting across 32 KB blocks as needed.
    // Throws std::system_error on I/O failure.
    void addRecord(std::span<const uint8_t> data);

    // Flushes written data to stable storage.
    void sync();

    // Returns the current file offset
    uint64_t fileOffset() const noexcept { return _file_offset; }

private:
    // Writes a WAL fragmenet
    void emitFragment(WalRecordType type, std::span<const uint8_t> payload);
    
    // Writes junk into to pad the block size
    void padToNextBlock();

    // Writes n bytes into buff.
    // Throws std::system_error if unable
    void writeAll(const void* buf, size_t n);

    // Returns the block offet
    uint32_t blockOffset() const noexcept {
        return static_cast<uint32_t>(_file_offset % kBlockSize);
    }

    int _fd;
    uint64_t _file_offset;
};
