#pragma once
#include <cstdint>
#include <vector>
#include "key.h"

enum class WalRecordType : uint8_t {
    kPadding    = 0, // sentinel when writer pads kHeaderSize bytes at a block boundary
    kFullType   = 1, // entire record fits in one fragment
    kFirstType  = 2, // first fragment of a multi-block record
    kMiddleType = 3, // interior fragment(s)
    kLastType   = 4, // final fragment
};

constexpr uint32_t kBlockSize  = 32768;           // 32 KB
constexpr uint32_t kHeaderSize = 7;               // 4 (CRC) + 2 (length) + 1 (type)
constexpr uint32_t kMaxPayload = kBlockSize - kHeaderSize;

// On-disk record header — no padding between fields.
#pragma pack(push, 1)
struct WriteAheadLogRecord {
    uint32_t      _crc;    // CRC-32 over [type byte | payload bytes]
    uint16_t      _length; // payload length in bytes for this fragment
    WalRecordType _type;
};
#pragma pack(pop)

static_assert(sizeof(WriteAheadLogRecord) == kHeaderSize);

// In-memory representation of a complete (possibly reassembled) WAL entry.
struct WalEntry {
    WriteAheadLogRecord  header;
    std::vector<uint8_t> payload; // header._length bytes
};
