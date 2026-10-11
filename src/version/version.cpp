#include "version/version.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <utility>

#include "db/filename.h"
#include "iterator/concatenating_iterator.h"
#include "iterator/sstable_iterator.h"

uint64_t maxBytesForLevel(int level) noexcept {
    uint64_t result = kLevel1MaxBytes;
    for (int i = 1; i < level; ++i) {
        result *= 10;
    }
    return result;
}

const std::vector<FileHandle>& Version::files(int level) const {
    assert(level >= 0 && level < kNumLevels);
    return _files[static_cast<size_t>(level)];
}

std::optional<SSTable::Entry> Version::get(std::string_view user_key, uint64_t seq) const {
    // Level 0's files overlap, so every one whose range covers the key is a
    // candidate and they have to be probed newest first. _files[0] is already
    // in that order.
    for (const auto& file : _files[0]) {
        if (user_key < file.meta.smallest_key || user_key > file.meta.largest_key) {
            continue;
        }
        if (auto entry = file.table->get(user_key, seq)) {
            return entry;
        }
    }

    for (int level = 1; level < kNumLevels; ++level) {
        const std::vector<FileHandle>& files = _files[static_cast<size_t>(level)];
        if (files.empty()) {
            continue;
        }
        // Disjoint and sorted, so at most one file can hold the key: the first
        // whose largest key is >= it.
        const auto file = std::partition_point(
            files.begin(), files.end(), [&](const FileHandle& f) {
                return f.meta.largest_key < user_key;
            });
        if (file == files.end() || user_key < file->meta.smallest_key) {
            continue;
        }
        if (auto entry = file->table->get(user_key, seq)) {
            return entry;
        }
    }

    return std::nullopt;
}

std::vector<FileHandle> Version::overlappingInputs(
    int level, std::optional<std::string_view> begin,
    std::optional<std::string_view> end) const {
    assert(level >= 0 && level < kNumLevels);

    std::vector<FileHandle> result;
    for (const auto& file : _files[static_cast<size_t>(level)]) {
        if (begin && file.meta.largest_key < *begin) {
            continue; // entirely before the range
        }
        if (end && file.meta.smallest_key > *end) {
            continue; // entirely after it
        }
        result.push_back(file);
    }
    return result;
}

void Version::addIterators(std::vector<std::unique_ptr<InternalIterator>>& out) const {
    for (const auto& file : _files[0]) {
        out.push_back(std::make_unique<SSTableIterator>(file.table));
    }

    for (int level = 1; level < kNumLevels; ++level) {
        const std::vector<FileHandle>& files = _files[static_cast<size_t>(level)];
        if (files.empty()) {
            continue;
        }
        std::vector<std::shared_ptr<const SSTable>> tables;
        tables.reserve(files.size());
        for (const auto& file : files) {
            tables.push_back(file.table);
        }
        out.push_back(newConcatenatingIterator(std::move(tables)));
    }
}

uint64_t Version::levelBytes(int level) const noexcept {
    assert(level >= 0 && level < kNumLevels);
    uint64_t total = 0;
    for (const auto& file : _files[static_cast<size_t>(level)]) {
        total += file.meta.file_size;
    }
    return total;
}

size_t Version::numFiles() const noexcept {
    size_t total = 0;
    for (const auto& level : _files) {
        total += level.size();
    }
    return total;
}

void Version::computeCompactionScore() noexcept {
    double best_score = -1.0;
    int    best_level = -1;

    // The bottom level is skipped: there is nowhere below it to compact into.
    for (int level = 0; level < kNumLevels - 1; ++level) {
        const double score =
            level == 0
                ? static_cast<double>(_files[0].size()) / static_cast<double>(kL0CompactionTrigger)
                : static_cast<double>(levelBytes(level)) /
                      static_cast<double>(maxBytesForLevel(level));
        if (score > best_score) {
            best_score = score;
            best_level = level;
        }
    }

    _compaction_score = best_score;
    _compaction_level = best_level;
}

// ── builder ───────────────────────────────────────────────────────────────────

VersionBuilder::VersionBuilder(std::filesystem::path dir, std::shared_ptr<const Version> base)
    : _dir(std::move(dir)), _base(std::move(base)) {
    if (!_base) {
        return;
    }
    for (int level = 0; level < kNumLevels; ++level) {
        for (const auto& file : _base->files(level)) {
            _levels[static_cast<size_t>(level)].emplace(file.meta.number, file.meta);
        }
    }
}

void VersionBuilder::apply(const VersionEdit& edit) {
    for (const auto& [level, number] : edit.deletedFiles()) {
        assert(level >= 0 && level < kNumLevels);
        _levels[static_cast<size_t>(level)].erase(number);
    }
    for (const auto& [level, meta] : edit.addedFiles()) {
        assert(level >= 0 && level < kNumLevels);
        _levels[static_cast<size_t>(level)].insert_or_assign(meta.number, meta);
    }
}

std::shared_ptr<Version> VersionBuilder::build() {
    // Reuse table handles from the base Version.
    std::map<uint64_t, std::shared_ptr<const SSTable>> open;
    if (_base) {
        for (int level = 0; level < kNumLevels; ++level) {
            for (const auto& file : _base->files(level)) {
                open.emplace(file.meta.number, file.table);
            }
        }
    }

    auto version = std::make_shared<Version>();

    for (int level = 0; level < kNumLevels; ++level) {
        std::vector<FileHandle>& out = version->_files[static_cast<size_t>(level)];
        out.reserve(_levels[static_cast<size_t>(level)].size());

        for (const auto& [number, meta] : _levels[static_cast<size_t>(level)]) {
            FileHandle handle;
            handle.meta = meta;
            if (const auto it = open.find(number); it != open.end()) {
                handle.table = it->second;
            } else {
                handle.table = SSTable::open(tableFileName(_dir, number));
            }
            out.push_back(std::move(handle));
        }

        if (level == 0) {
            // Newest first. Level-0 files are numbered in the order they were
            // flushed, so a larger number is strictly newer -- the ordering a
            // lookup and a merge both depend on.
            std::sort(out.begin(), out.end(), [](const FileHandle& a, const FileHandle& b) {
                return a.meta.number > b.meta.number;
            });
        } else {
            // A sorted run, ordered by where each file sits in the key space.
            std::sort(out.begin(), out.end(), [](const FileHandle& a, const FileHandle& b) {
                return a.meta.smallest_key < b.meta.smallest_key;
            });
        }
    }

    version->computeCompactionScore();
    return version;
}
