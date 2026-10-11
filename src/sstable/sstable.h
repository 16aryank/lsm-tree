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

// Immutable sorted table. The index stays in memory; checked data blocks are
// read on demand. pread() permits concurrent readers on one table.
class SSTable {
public:
    // A kDelete entry masks older versions in other tables.
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

    // Forward cursor in internal-key order. Views are invalidated by next(),
    // and the table must outlive the iterator.
    class Iterator {
    public:
        // _current contains views into _buffer.
        Iterator(const Iterator&) = delete;
        Iterator& operator=(const Iterator&) = delete;

        [[nodiscard]] bool valid() const noexcept { return _valid; }
        void next();

        // Positions at the first entry at or after (key, tag).
        void seek(std::string_view key, uint64_t tag);
        void seekToFirst() { loadBlock(0); }

        [[nodiscard]] std::string_view key() const noexcept { return _current.key; }
        // Packed sequence number and value type.
        [[nodiscard]] uint64_t tag() const noexcept { return _current.tag; }
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

    // Finds the newest entry visible at seq. Tombstones return kDelete.
    [[nodiscard]] std::optional<Entry> get(std::string_view key, uint64_t seq) const;

    [[nodiscard]] Iterator cursor() const { return Iterator(this); }

    [[nodiscard]] uint64_t numEntries() const noexcept { return _num_entries; }
    [[nodiscard]] uint64_t fileSize() const noexcept { return _file_size; }
    [[nodiscard]] size_t numBlocks() const noexcept { return _index.size(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return _path; }

    // Key range covered by this table, empty if it has no entries. User keys
    // only -- the bounds a level's overlap check needs.
    [[nodiscard]] const std::string& smallestKey() const noexcept { return _smallest_key; }
    [[nodiscard]] const std::string& largestKey() const noexcept { return _largest_key; }

    // Tags complete the table's internal-key bounds.
    [[nodiscard]] uint64_t smallestTag() const noexcept { return _smallest_tag; }
    [[nodiscard]] uint64_t largestTag() const noexcept { return _largest_tag; }

private:
    // One index block entry: the last internal key of a data block, and where
    // that block lives.
    struct IndexEntry {
        std::string key;
        uint64_t    tag = 0;
        BlockHandle handle;
    };

    SSTable(std::filesystem::path path, int fd) noexcept;

    // First block whose last internal key is at or after (key, tag).
    [[nodiscard]] size_t findBlock(std::string_view key, uint64_t tag) const noexcept;

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
    uint64_t                _smallest_tag = 0;
    uint64_t                _largest_tag  = 0;
};
