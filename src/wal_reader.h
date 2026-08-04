#pragma once
#include <cstdint>
#include <optional>
#include <vector>
#include "wal_entry.h"

class WalReader {
public:
    explicit WalReader(int fd, uint64_t initial_offset = 0);

    // Reads and reassembles the next complete logical record from the WAL.
    // Returns constructed record if partial. Returns std::nullopt on clean EOF.
    // Throws std::system_error on I/O failure, std::runtime_error on corruption.
    std::optional<std::vector<uint8_t>> readRecord();

    // Returns the current file offset
    inline uint64_t fileOffset() const noexcept { return _file_offset; }

private:
    // Reads one physical fragment into out_type and out_payload.
    // Returns false on clean EOF before the first byte of a header.
    // Throws std::runtime_error on corrupted fragment.
    bool readFragment(WalRecordType& out_type, std::vector<uint8_t>& out_payload);

    // ── pread wrappers ─────────────────────────────────────────────────────────────

    // Reads all bytes from buf and checks that it matches the size of the expected payload.
    // Throws a runtime error if unexpected EOF. 
    void readAll(void* buf, size_t n);

    // Tries to read up to n bytes.
    // Returns number of bytes read.
    // Throws std::system_error if pread fails.
    size_t tryRead(void* buf, size_t n);

    // Skips up to n bytes by reading from trash buffer.
    void skipBytes(uint32_t n);

    // Returns the offset of the current block
    inline uint32_t blockOffset() const noexcept {
        return static_cast<uint32_t>(_file_offset % kBlockSize);
    }

    int _fd;
    uint64_t _file_offset;
};
