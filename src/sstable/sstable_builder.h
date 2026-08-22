#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "sstable/sstable_format.h"

// Writes one immutable SSTable, in a single forward pass, from entries handed
// to it in internal-key order -- exactly the order a MemTable iterator yields.
// Nothing is buffered beyond the current data block and the index.
//
// The file is only valid once finish() returns: it writes the footer last, so
// a crash mid-build leaves a file with no magic number at its tail, which the
// reader rejects. Anything that goes wrong throws std::system_error and leaves
// the partial file to be removed by abandon() (or by the destructor).
//
// Thread-safety: none. One builder belongs to one flush.
class SSTableBuilder {
public:
    // Creates path. Throws std::system_error if it cannot be opened.
    explicit SSTableBuilder(std::filesystem::path path, size_t block_size = kSSTableBlockSize);

    // Removes the file unless finish() completed.
    ~SSTableBuilder();

    // Delete copy constructors.
    SSTableBuilder(const SSTableBuilder&) = delete;
    SSTableBuilder& operator=(const SSTableBuilder&) = delete;

    // Appends one entry. Keys must arrive strictly after the previous one in
    // internal-key order; violating that asserts.
    void add(std::string_view key, uint64_t tag, std::string_view value);

    // Flushes the last data block, then the index and footer, and fsyncs both
    // the file and its directory so the table survives a crash. Returns the
    // file size in bytes. Throws std::system_error on I/O failure.
    uint64_t finish();

    // Closes and unlinks the partial file. Safe to call more than once, and
    // after finish() (where it does nothing).
    void abandon() noexcept;

    [[nodiscard]] inline uint64_t numEntries() const noexcept { return _num_entries; }
    [[nodiscard]] inline const std::filesystem::path& path() const noexcept { return _path; }

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

    // Last key added, which is also the index key for the block being built.
    std::string _last_key;
    uint64_t _last_tag = 0;

    bool _finished = false;
};
