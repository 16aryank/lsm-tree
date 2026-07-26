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
    // On macOS, prefer fcntl(fd, F_FULLFSYNC) for true durability on SSDs.
    void sync();

    uint64_t fileOffset() const { return _file_offset; }

private:
    void emitFragment(WalRecordType type, std::span<const uint8_t> payload);
    void padToNextBlock();
    void writeAll(const void* buf, size_t n);

    uint32_t blockOffset() const { return static_cast<uint32_t>(_file_offset % kBlockSize); }

    int      _fd;
    uint64_t _file_offset;
};
