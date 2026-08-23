#pragma once
#include <cstdint>
#include <optional>
#include <vector>
#include "wal_entry.h"

// How the reader treats a damaged or truncated record.
enum class WalReadMode {
    kRecovery,
    kStrict,
};

// Reads logical records back out of a write-ahead log, reassembling any that
// the writer fragmented across block boundaries.
//
// Thread-safety: none. One reader belongs to one replay. 
class WalReader {
public:
    explicit WalReader(int fd, uint64_t initial_offset = 0,
                       WalReadMode mode = WalReadMode::kRecovery);

    // Reads and reassembles the next complete logical record from the WAL.
    // Returns std::nullopt at the end of the log -- whether that end is clean
    // or torn; stoppedEarly() tells the two apart.
    // Throws std::system_error on I/O failure, and std::runtime_error on
    // corruption when the mode is kStrict.
    std::optional<std::vector<uint8_t>> readRecord();

    // Returns the current file offset
    [[nodiscard]] inline uint64_t fileOffset() const noexcept { return _file_offset; }

    // True if the last readRecord() stopped on damage rather than on a clean
    // end of file. Only ever set in kRecovery mode -- kStrict throws instead.
    [[nodiscard]] inline bool stoppedEarly() const noexcept { return _stopped_early; }

private:
    // Outcome of reading one physical fragment.
    enum class FragmentStatus {
        kOk,      // out_type and out_payload are filled in
        kEof,     // clean end of file, on a record boundary
        kCorrupt, // bad CRC, or a header/payload the file ends in the middle of
    };

    // Reads one physical fragment into out_type and out_payload, stepping over
    // any block padding on the way.
    FragmentStatus readFragment(WalRecordType& out_type, std::vector<uint8_t>& out_payload);

    // Ends the record stream: throws in kStrict mode, otherwise just records
    // that the stop was not clean.
    void reportCorruption(const char* what);

    // ── pread wrappers ─────────────────────────────────────────────────────────

    // Tries to read up to n bytes.
    // Returns number of bytes read.
    // Throws std::system_error if pread fails.
    size_t tryRead(void* buf, size_t n);

    // Skips up to n bytes by reading into a trash buffer.
    // Returns false if the file ended before n bytes had been skipped.
    bool skipBytes(uint32_t n);

    // Returns the offset within the current block
    inline uint32_t blockOffset() const noexcept {
        return static_cast<uint32_t>(_file_offset % kBlockSize);
    }

    int         _fd;
    uint64_t    _file_offset;
    WalReadMode _mode;
    bool        _stopped_early{ false };
};
