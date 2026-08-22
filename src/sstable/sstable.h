#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "key.h"
#include "sstable/sstable_format.h"

// A read-only, immutable table on disk: the thing a MemTable becomes when it
// is flushed. Written by SSTableBuilder, never modified afterwards.
//
// open() pulls the footer and the whole index block into memory and keeps the
// file open; data blocks are read on demand and not cached, so the resident
// cost of a table is its index plus one file descriptor. Every read verifies
// the block's CRC before looking at it.
//
// Thread-safety: get() and iteration are const and use pread(), which carries
// its own offset, so any number of threads may read one table concurrently.
class SSTable {
public:
    // What a lookup found. `type` matters as much as the value: a kDelete hit
    // is a tombstone, meaning the key is absent as of `seq` and older tables
    // must not be consulted.
    struct Entry {
        uint64_t    seq  = 0;
        ValueType   type = ValueType::kValue;
        std::string value;
    };

    // Opens and validates `path`. Throws std::system_error if the file cannot
    // be read, std::runtime_error if it is not a well-formed SSTable.
    [[nodiscard]] static std::shared_ptr<SSTable> open(std::filesystem::path path);

    ~SSTable();

    SSTable(const SSTable&)            = delete;
    SSTable& operator=(const SSTable&) = delete;

    // Forward-only cursor over every version of every key, in internal-key
    // order (user key ascending, newest version of each key first) -- the same
    // order MemTable::Iterator yields, which is what lets a merge walk both.
    //
    // key() and value() point into a block this iterator holds and are
    // invalidated by next(). The SSTable must outlive the iterator.
    class Iterator {
    public:
        [[nodiscard]] bool valid() const noexcept { return _valid; }
        void next();

        [[nodiscard]] std::string_view key() const noexcept { return _current.key; }
        [[nodiscard]] uint64_t sequenceNumber() const noexcept {
            return unpackSeqNumber(_current.tag);
        }
        [[nodiscard]] ValueType valueType() const noexcept {
            return unpackValueType(_current.tag);
        }
        [[nodiscard]] std::string_view value() const noexcept { return _current.value; }

    private:
        friend class SSTable;
        explicit Iterator(const SSTable* table);

        // Loads block `index` and positions on its first entry. Marks the
        // iterator exhausted once there are no blocks left.
        void loadBlock(size_t index);

        const SSTable*   _table;
        size_t           _block_index = 0;
        std::string      _buffer;
        size_t           _offset = 0;
        SSTableEntryView _current;
        bool             _valid = false;
    };

    // Returns the entry with the largest sequence number <= seq for key, or
    // nullopt if the table has no version of that key at or below seq. A
    // tombstone is returned as an Entry with type kDelete, not as nullopt:
    // telling those apart is the caller's job.
    [[nodiscard]] std::optional<Entry> get(std::string_view key, uint64_t seq) const;

    [[nodiscard]] Iterator newIterator() const { return Iterator(this); }

    [[nodiscard]] uint64_t numEntries() const noexcept { return _num_entries; }
    [[nodiscard]] uint64_t fileSize() const noexcept { return _file_size; }
    [[nodiscard]] size_t numBlocks() const noexcept { return _index.size(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return _path; }

    // Key range covered by this table, empty if it has no entries. User keys
    // only -- the bounds a level's overlap check needs.
    [[nodiscard]] const std::string& smallestKey() const noexcept { return _smallest_key; }
    [[nodiscard]] const std::string& largestKey() const noexcept { return _largest_key; }

private:
    // One index block entry: the last internal key of a data block, and where
    // that block lives.
    struct IndexEntry {
        std::string key;
        uint64_t    tag = 0;
        BlockHandle handle;
    };

    SSTable(std::filesystem::path path, int fd) noexcept;

    // Reads the footer and index block, then the first data block to learn the
    // smallest key. Throws on anything malformed.
    void loadIndex();

    // Reads a block, checks its CRC, and returns its contents without the
    // trailer. Throws std::runtime_error if the checksum does not match.
    [[nodiscard]] std::string readBlock(const BlockHandle& handle) const;

    // pread loop. Throws std::system_error on failure, std::runtime_error if
    // the file ends early.
    void readAll(void* buf, size_t n, uint64_t offset) const;

    std::filesystem::path   _path;
    int                     _fd;
    uint64_t                _file_size   = 0;
    uint64_t                _num_entries = 0;
    std::vector<IndexEntry> _index;
    std::string             _smallest_key;
    std::string             _largest_key;
};
