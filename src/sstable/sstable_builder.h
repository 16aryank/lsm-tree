#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "sstable/bloom_filter.h"
#include "sstable/sstable_format.h"

// Writes sorted entries in one pass. finish() writes the footer last and syncs
// the file; abandon() or the destructor removes an unfinished file.
class SSTableBuilder {
public:
    // Creates path. Throws std::system_error if it cannot be opened.
    explicit SSTableBuilder(std::filesystem::path path, size_t block_size = kSSTableBlockSize);

    // Removes the file unless finish() completed.
    ~SSTableBuilder();

    SSTableBuilder(const SSTableBuilder&) = delete;
    SSTableBuilder& operator=(const SSTableBuilder&) = delete;

    // Appends one entry. Keys must arrive strictly after the previous one in
    // internal-key order; violating that asserts.
    void add(std::string_view key, uint64_t tag, std::string_view value);

    // Flushes the last data block, filter, index, and footer, then syncs both
    // the file and its directory so the table survives a crash. Returns the
    // file size in bytes. Throws std::system_error on I/O failure.
    uint64_t finish();

    // Closes and unlinks the partial file. Safe to call more than once, and
    // after finish() (where it does nothing).
    void abandon() noexcept;

    [[nodiscard]] inline uint64_t numEntries() const noexcept { return _num_entries; }

    // Bytes written, the current data block, and the pending filter block.
    // Saturates if an unfinished table grows beyond the representable range.
    [[nodiscard]] inline uint64_t estimatedSize() const noexcept {
        if (_offset > UINT64_MAX - _block.size()) {
            return UINT64_MAX;
        }
        const uint64_t base = _offset + _block.size();
        if (_num_entries == 0) {
            return base;
        }
        const uint64_t keys = _filter_builder.distinctKeys();
        if (keys > (UINT64_MAX - 7) / 10) {
            return UINT64_MAX;
        }
        const uint64_t filter_bytes = std::max<uint64_t>(8, (keys * 10 + 7) / 8);
        const uint64_t pending = kBloomHeaderSize + filter_bytes + kBlockTrailerSize;
        return base > UINT64_MAX - pending ? UINT64_MAX : base + pending;
    }
    [[nodiscard]] inline const std::filesystem::path& path() const noexcept { return _path; }

    // Internal-key bounds, valid once at least one entry has been added.
    [[nodiscard]] inline const std::string& smallestKey() const noexcept { return _first_key; }
    [[nodiscard]] inline uint64_t smallestTag() const noexcept { return _first_tag; }
    [[nodiscard]] inline const std::string& largestKey() const noexcept { return _last_key; }
    [[nodiscard]] inline uint64_t largestTag() const noexcept { return _last_tag; }

private:
    // Writes `contents` plus its CRC trailer at the current offset and fills
    // in where it landed.
    void writeBlock(const std::string& contents, BlockHandle& handle);

    // Writes the pending data block, if any, and adds its index entry.
    void flushDataBlock();

    void writeAll(const void* buf, size_t n);

    std::filesystem::path _path;
    int _fd;
    size_t _block_size;

    uint64_t _offset{ 0 }; // bytes written so far
    uint64_t _num_entries{ 0 };

    std::string _block; // data block under construction
    std::string _index; // index block, one entry per finished data block
    BloomFilterBuilder _filter_builder;

    // First key added. Entries arrive in internal-key order, so this is the
    // table's lower bound.
    std::string _first_key;
    uint64_t _first_tag = 0;

    // Last key added, which is also the index key for the block being built.
    std::string _last_key;
    uint64_t _last_tag = 0;

    bool _finished = false;
};
