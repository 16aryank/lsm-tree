#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <boost/crc.hpp>

#include "key.h"

// The on-disk shape of an SSTable, shared by the builder and the reader.
//
//     +---------------------+
//     | data block 0        |  entries in internal-key order, + CRC trailer
//     | data block 1        |
//     | ...                 |
//     +---------------------+
//     | index block         |  one entry per data block, + CRC trailer
//     +---------------------+
//     | footer (32 bytes)   |  where the index lives, how many entries, magic
//     +---------------------+
//

// Bytes of entry data the builder packs into a data block before starting a
// new one. A single entry larger than this gets a block to itself.
inline constexpr size_t kSSTableBlockSize = 4096;

// Every block is followed by a CRC-32 over its
// contents. Same checksum the WAL uses, so corruption is caught on read
// rather than surfacing as a garbage value.
inline constexpr size_t kBlockTrailerSize = 4;

// Entry header: key_len uint32 | value_len uint32 | tag uint64.
inline constexpr size_t kEntryHeaderSize = 16;

// Block handle: offset uint64 | size uint32, where size counts block contents
// only and excludes the trailer.
inline constexpr size_t kBlockHandleSize = 12;

// Footer: index_offset uint64 | index_size uint64 | num_entries uint64 | magic
// uint64. Fixed width and last in the file, so a reader starts by seeking to
// (file_size - kFooterSize) and works backwards.
inline constexpr size_t kFooterSize = 32;

// "LSMSST01" in ASCII. Identifies the file and pins the format version.
inline constexpr uint64_t kSSTableMagic = 0x4c534d5353543031ULL;

// ── little-endian fixed-width primitives ──────────────────────────────────────

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

[[nodiscard]] inline uint32_t blockChecksum(std::string_view contents) noexcept {
    boost::crc_32_type crc;
    if (!contents.empty()) {
        crc.process_bytes(contents.data(), contents.size());
    }
    return crc.checksum();
}

// ── entries ───────────────────────────────────────────────────────────────────

// One decoded entry. key and value are views into the block buffer they were
// decoded from and stay valid only as long as it does.
struct SSTableEntryView {
    std::string_view key;
    uint64_t         tag = 0;
    std::string_view value;
};

inline void appendEntry(std::string& dst, std::string_view key, uint64_t tag,
                        std::string_view value) {
    putFixed32(dst, static_cast<uint32_t>(key.size()));
    putFixed32(dst, static_cast<uint32_t>(value.size()));
    putFixed64(dst, tag);
    dst.append(key);
    dst.append(value);
}

// Decodes the entry starting at `offset`. Returns the offset just past it, or
// 0 if the block is truncated there or the lengths run off its end -- 0 is
// never a valid answer, since an entry is at least kEntryHeaderSize bytes.
[[nodiscard]] inline size_t decodeEntry(std::string_view block, size_t offset,
                                        SSTableEntryView& out) noexcept {
    if (offset + kEntryHeaderSize > block.size()) {
        return 0;
    }
    const char*    header    = block.data() + offset;
    const uint32_t key_len   = decodeFixed32(header);
    const uint32_t value_len = decodeFixed32(header + 4);
    const uint64_t tag       = decodeFixed64(header + 8);

    const size_t body = offset + kEntryHeaderSize;
    // Widened before adding: two uint32 lengths can overflow size_t only on a
    // 32-bit host, but the sum is compared against a size_t either way.
    if (static_cast<uint64_t>(body) + key_len + value_len > block.size()) {
        return 0;
    }
    out.key = block.substr(body, key_len);
    out.value = block.substr(body + key_len, value_len);
    out.tag = tag;
    return body + key_len + value_len;
}

// ── block handles ─────────────────────────────────────────────────────────────

// Where a block lives in the file. `size` excludes the CRC trailer, so the
// bytes to read are size + kBlockTrailerSize.
struct BlockHandle {
    uint64_t offset = 0;
    uint32_t size   = 0;
};

inline void appendBlockHandle(std::string& dst, const BlockHandle& handle) {
    putFixed64(dst, handle.offset);
    putFixed32(dst, handle.size);
}

[[nodiscard]] inline bool decodeBlockHandle(std::string_view src, BlockHandle& out) noexcept {
    if (src.size() != kBlockHandleSize) {
        return false;
    }
    out.offset = decodeFixed64(src.data());
    out.size   = decodeFixed32(src.data() + 8);
    return true;
}

// ── footer ────────────────────────────────────────────────────────────────────

struct SSTableFooter {
    uint64_t index_offset = 0;
    uint64_t index_size   = 0; // index block contents, trailer excluded
    uint64_t num_entries  = 0;
};

inline void appendFooter(std::string& dst, const SSTableFooter& footer) {
    putFixed64(dst, footer.index_offset);
    putFixed64(dst, footer.index_size);
    putFixed64(dst, footer.num_entries);
    putFixed64(dst, kSSTableMagic);
}

// Returns false if `src` is too short or does not end in the magic number,
// which is the cheap test for "this is not one of our files".
[[nodiscard]] inline bool decodeFooter(std::string_view src, SSTableFooter& out) noexcept {
    if (src.size() < kFooterSize) {
        return false;
    }
    if (decodeFixed64(src.data() + 24) != kSSTableMagic) {
        return false;
    }
    out.index_offset = decodeFixed64(src.data());
    out.index_size   = decodeFixed64(src.data() + 8);
    out.num_entries  = decodeFixed64(src.data() + 16);
    return true;
}
