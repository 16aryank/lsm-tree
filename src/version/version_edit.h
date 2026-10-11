#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Level 0 holds flushed memtables and its files
// overlap; every level below it is a sorted run of disjoint files.
inline constexpr int kNumLevels = 7;

// What the manifest records about one table: enough to place it in a level and
// decide whether a lookup or a compaction needs to touch it, without opening
// it. The bounds are *internal* keys -- two tables can share a user key at the
// edge and only the tag says which side is newer.
struct FileMetaData {
    uint64_t number = 0;
    uint64_t file_size = 0;
    std::string smallest_key;
    uint64_t smallest_tag = 0;
    std::string largest_key;
    uint64_t largest_tag = 0;
};

// One atomic change to the set of files that make up the tree: a flush adding
// a level-0 table, a compaction swapping N inputs for M outputs, or a bare
// counter bump. The manifest is a log of these, and replaying every one of
// them from an empty tree reconstructs the current Version.
//
// Wire format -- a sequence of tagged fields, each a varint32 tag followed by
// that tag's payload:
//
//     kLogNumber       varint64
//     kNextFileNumber  varint64
//     kLastSequence    varint64
//     kDeletedFile     varint32 level, varint64 number
//     kNewFile         varint32 level, varint64 number, varint64 file size,
//                      length-prefixed smallest key, varint64 smallest tag,
//                      length-prefixed largest key,  varint64 largest tag
//
// Scalars appear at most once; file fields may repeat. Unknown tags fail
// decoding because their payload boundaries are unknown.
enum class EditTag : uint32_t {
    kLogNumber      = 1,
    kNextFileNumber = 2,
    kLastSequence   = 3,
    kDeletedFile    = 4,
    kNewFile        = 5,
};

class VersionEdit {
public:
    void setLogNumber(uint64_t number) { _log_number = number; }
    void setNextFileNumber(uint64_t number) { _next_file_number = number; }
    void setLastSequence(uint64_t sequence) { _last_sequence = sequence; }

    void addFile(int level, FileMetaData meta) {
        _added.emplace_back(level, std::move(meta));
    }
    void deleteFile(int level, uint64_t number) {
        _deleted.emplace_back(level, number);
    }

    void clear() {
        _log_number.reset();
        _next_file_number.reset();
        _last_sequence.reset();
        _added.clear();
        _deleted.clear();
    }

    [[nodiscard]] bool hasLogNumber() const noexcept { return _log_number.has_value(); }
    [[nodiscard]] bool hasNextFileNumber() const noexcept { return _next_file_number.has_value(); }
    [[nodiscard]] bool hasLastSequence() const noexcept { return _last_sequence.has_value(); }

    // Reading one of these without its has* companion returning true is a
    // caller bug, not a defaulting question: 0 is a legal value for all three.
    [[nodiscard]] uint64_t logNumber() const noexcept { return *_log_number; }
    [[nodiscard]] uint64_t nextFileNumber() const noexcept { return *_next_file_number; }
    [[nodiscard]] uint64_t lastSequence() const noexcept { return *_last_sequence; }

    [[nodiscard]] const std::vector<std::pair<int, FileMetaData>>& addedFiles() const noexcept {
        return _added;
    }
    [[nodiscard]] const std::vector<std::pair<int, uint64_t>>& deletedFiles() const noexcept {
        return _deleted;
    }

    [[nodiscard]] std::string encode() const;

    // Replaces this edit's contents with what `src` describes. Returns false if
    // `src` is truncated, names an unknown tag, repeats a scalar, carries an
    // out-of-range level, or has bytes left over -- trailing bytes mean the
    // record is not the single edit it claims to be. A false return leaves the
    // edit cleared, never half-populated.
    [[nodiscard]] bool decode(std::string_view src) noexcept;

private:
    std::optional<uint64_t> _log_number;
    std::optional<uint64_t> _next_file_number;
    std::optional<uint64_t> _last_sequence;

    std::vector<std::pair<int, FileMetaData>> _added;
    std::vector<std::pair<int, uint64_t>>     _deleted;
};
