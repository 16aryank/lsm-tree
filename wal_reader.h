#pragma once
#include <cstdint>
#include <optional>
#include <vector>
#include "wal_entry.h"

class WalReader {
public:
    explicit WalReader(int fd, uint64_t initial_offset = 0);

    // Reads and reassembles the next complete logical record from the WAL.
    // Returns std::nullopt on clean EOF.
    // Throws std::system_error on I/O failure, std::runtime_error on corruption.
    std::optional<std::vector<uint8_t>> readRecord();

    uint64_t fileOffset() const { return _file_offset; }

private:
    // Reads one physical fragment into out_type and out_payload.
    // Returns false on clean EOF before the first byte of a header.
    bool readFragment(WalRecordType& out_type, std::vector<uint8_t>& out_payload);

    // Wrappers around pread: readAll throws on EOF, tryRead returns bytes read (0 = EOF).
    void   readAll(void* buf, size_t n);
    size_t tryRead(void* buf, size_t n);
    void   skipBytes(uint32_t n);

    uint32_t blockOffset() const { return static_cast<uint32_t>(_file_offset % kBlockSize); }

    int      _fd;
    uint64_t _file_offset;
};
