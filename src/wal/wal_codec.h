#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coding.h"
#include "key.h"

// One logical write, as it appears inside a WAL record's payload:
//
//     varint64   tag        (sequence number << 8 | ValueType)
//     varint64   key length, then the key bytes
//     varint64   value length, then the value bytes
//
// A tombstone is a kDelete tag with an empty value. The framing around this --
// CRC, fragmentation, block alignment -- belongs to WalWriter/WalReader; this
// header is only concerned with what one record says.

struct WalEntryView {
    uint64_t         tag = 0;
    std::string_view key;
    std::string_view value;

    [[nodiscard]] uint64_t  sequenceNumber() const noexcept { return unpackSeqNumber(tag); }
    [[nodiscard]] ValueType valueType() const noexcept { return unpackValueType(tag); }
};

inline void appendWalEntry(std::string& dst, uint64_t seq, ValueType type,
                           std::string_view key, std::string_view value) {
    putVarint64(dst, packSeqAndType(seq, type));
    putLengthPrefixed(dst, key);
    putLengthPrefixed(dst, value);
}

[[nodiscard]] inline std::string encodeWalEntry(uint64_t seq, ValueType type,
                                                std::string_view key, std::string_view value) {
    std::string out;
    appendWalEntry(out, seq, type, key, value);
    return out;
}

// Returns false if `src` is truncated, malformed, or has bytes left over --
// trailing bytes mean the record is not the single entry it claims to be. On
// success the views point into `src`'s buffer and share its lifetime.
[[nodiscard]] inline bool decodeWalEntry(std::string_view src, WalEntryView& out) noexcept {
    return getVarint64(src, out.tag) && getLengthPrefixed(src, out.key) &&
           getLengthPrefixed(src, out.value) && src.empty();
}

// ── adapters between the codec's bytes and the framing layer's ────────────────

[[nodiscard]] inline std::span<const uint8_t> byteSpan(const std::string& bytes) noexcept {
    return { reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size() };
}

[[nodiscard]] inline std::string_view byteView(const std::vector<uint8_t>& bytes) noexcept {
    return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
}
