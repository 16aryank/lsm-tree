#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "iterator/iterator.h"
#include "sstable/sstable.h"
#include "version/version_edit.h"

// Level 0's score is based on overlapping file count.
inline constexpr size_t kL0CompactionTrigger = 4;

// Byte budget for level 1; each deeper level has ten times this budget.
inline constexpr uint64_t kLevel1MaxBytes = 10ull << 20;

// Level 0's budget is a file count, so this is meaningful only for level >= 1.
[[nodiscard]] uint64_t maxBytesForLevel(int level) noexcept;

// Metadata and an open table retained for the Version's lifetime.
struct FileHandle {
    FileMetaData                   meta;
    std::shared_ptr<const SSTable> table;
};

// Immutable file set. Level 0 may overlap and is newest first; deeper levels
// contain disjoint files in key order. Readers retain a shared_ptr during use.
class Version {
public:
    // The newest version of `user_key` at or below `seq`, searched level by
    // level from the top. A tombstone is returned as an Entry with type
    // kDelete, not as nullopt: it means the key is absent as of `seq`, and
    // older levels must not be consulted.
    [[nodiscard]] std::optional<SSTable::Entry> get(std::string_view user_key, uint64_t seq) const;

    [[nodiscard]] const std::vector<FileHandle>& files(int level) const;

    // Every file in `level` whose key range intersects [begin, end], inclusive.
    // A nullopt bound is unbounded on that side -- distinct from an empty
    // string, which is a legal key.
    [[nodiscard]] std::vector<FileHandle> overlappingInputs(
        int level, std::optional<std::string_view> begin,
        std::optional<std::string_view> end) const;

    // Appends level-0 iterators newest first, then one iterator per deeper level.
    void addIterators(std::vector<std::unique_ptr<InternalIterator>>& out) const;

    [[nodiscard]] uint64_t levelBytes(int level) const noexcept;
    [[nodiscard]] size_t numFiles() const noexcept;

    // A score >= 1 means the selected level is over budget.
    [[nodiscard]] double compactionScore() const noexcept { return _compaction_score; }
    [[nodiscard]] int compactionLevel() const noexcept { return _compaction_level; }

private:
    friend class VersionBuilder;

    void computeCompactionScore() noexcept;

    std::array<std::vector<FileHandle>, kNumLevels> _files;
    double _compaction_score{ -1.0 };
    int    _compaction_level{ -1 };
};

// Applies edits to a base Version and opens each surviving table at build time.
class VersionBuilder {
public:
    // `base` may be null, meaning start from an empty tree, which is what
    // recovery does before replaying the first record.
    VersionBuilder(std::filesystem::path dir, std::shared_ptr<const Version> base);

    // Deletions are applied before additions, matching the order the edit
    // encodes them in.
    void apply(const VersionEdit& edit);

    // Reuses open tables from the base Version. Throws for a missing or
    // malformed table referenced by the edits.
    [[nodiscard]] std::shared_ptr<Version> build();

private:
    std::filesystem::path                          _dir;
    std::shared_ptr<const Version>                 _base;
    // Keyed by file number so a deletion is a lookup rather than a scan; the
    // key ordering is irrelevant, build() sorts each level properly.
    std::array<std::map<uint64_t, FileMetaData>, kNumLevels> _levels;
};
