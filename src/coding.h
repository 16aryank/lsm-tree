#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Little-endian fixed-width and varint primitives, shared by every on-disk
// format in the engine: SSTable blocks and footers, WAL payloads, and the
// manifest

// ── fixed width ───────────────────────────────────────────────────────────────

inline void putFixed32(std::string& dst, uint32_t value) {
    const char buf[4] = {
        static_cast<char>(value & 0xFF),
        static_cast<char>((value >> 8) & 0xFF),
        static_cast<char>((value >> 16) & 0xFF),
        static_cast<char>((value >> 24) & 0xFF),
    };
    dst.append(buf, sizeof(buf));
}

inline void putFixed64(std::string& dst, uint64_t value) {
    putFixed32(dst, static_cast<uint32_t>(value & 0xFFFFFFFFULL));
    putFixed32(dst, static_cast<uint32_t>(value >> 32));
}

[[nodiscard]] inline uint32_t decodeFixed32(const char* src) noexcept {
    const auto* p = reinterpret_cast<const unsigned char*>(src);
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

[[nodiscard]] inline uint64_t decodeFixed64(const char* src) noexcept {
    return static_cast<uint64_t>(decodeFixed32(src)) |
           (static_cast<uint64_t>(decodeFixed32(src + 4)) << 32);
}

// ── varint ────────────────────────────────────────────────────────────────────

// Seven payload bits per byte, least significant group first, with the high bit
// set on every byte but the last. Values below 128 cost one byte, which is what
// makes it worth using for the key and value lengths that dominate a WAL
// payload or a manifest record.
inline constexpr size_t kMaxVarint64Bytes = 10;

inline void putVarint64(std::string& dst, uint64_t value) {
    while (value >= 0x80) {
        dst.push_back(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    dst.push_back(static_cast<char>(value));
}

inline void putVarint32(std::string& dst, uint32_t value) {
    putVarint64(dst, value);
}

// Decodes a varint from the front of `src` and advances it past the bytes
// consumed. Returns false if `src` ends mid-varint or the encoding runs longer
// than a uint64 can hold, both of which mean the input is malformed.
[[nodiscard]] inline bool getVarint64(std::string_view& src, uint64_t& out) noexcept {
    uint64_t result = 0;
    for (size_t i = 0; i < kMaxVarint64Bytes; ++i) {
        if (i >= src.size()) {
            return false;
        }
        const uint64_t byte = static_cast<unsigned char>(src[i]);
        result |= (byte & 0x7F) << (7 * i);
        if ((byte & 0x80) == 0) {
            src.remove_prefix(i + 1);
            out = result;
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool getVarint32(std::string_view& src, uint32_t& out) noexcept {
    uint64_t wide = 0;
    if (!getVarint64(src, wide) || wide > UINT32_MAX) {
        return false;
    }
    out = static_cast<uint32_t>(wide);
    return true;
}

// ── length-prefixed byte strings ──────────────────────────────────────────────

inline void putLengthPrefixed(std::string& dst, std::string_view value) {
    putVarint64(dst, value.size());
    dst.append(value);
}

// On success `out` views into `src`'s buffer, so it stays valid only as long as
// that buffer does.
[[nodiscard]] inline bool getLengthPrefixed(std::string_view& src,
                                            std::string_view& out) noexcept {
    uint64_t length = 0;
    if (!getVarint64(src, length) || length > src.size()) {
        return false;
    }
    out = src.substr(0, length);
    src.remove_prefix(length);
    return true;
}
