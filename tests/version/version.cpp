// The per-level file set: how a lookup descends it, how a builder derives one
// version from another, and the range queries a compaction picker needs.
//
// This is where "later in the vector is newer" stops being the rule. Level 0
// still works that way because its files overlap, but every level below is a
// sorted run searched by key, and a lookup has to stop at the first answer it
// finds -- tombstones included -- or a deleted key comes back from an older
// level.
#include "src/db/filename.h"
#include "src/iterator/merging_iterator.h"
#include "src/key.h"
#include "src/sstable/sstable_builder.h"
#include "src/version/version.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

std::filesystem::path testDir() {
    return std::filesystem::temp_directory_path() /
           ("lsm-version-test-" + std::to_string(::getpid()));
}

struct Entry {
    std::string key;
    uint64_t    seq;
    ValueType   type;
    std::string value;
};

// Writes a table as <dir>/<number>.sst and returns the metadata a manifest
// edit would carry for it -- read back off the finished table, which is what
// the real flush path does.
FileMetaData writeTable(const std::filesystem::path& dir, uint64_t number,
                        std::vector<Entry> entries) {
    const InternalKeyComparator compare;
    std::sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
        return compare(a.key, packSeqAndType(a.seq, a.type),
                       b.key, packSeqAndType(b.seq, b.type)) < 0;
    });

    const std::filesystem::path path = tableFileName(dir, number);
    SSTableBuilder builder(path);
    for (const auto& e : entries) {
        builder.add(e.key, packSeqAndType(e.seq, e.type), e.value);
    }
    const uint64_t size  = builder.finish();
    const auto     table = SSTable::open(path);

    FileMetaData meta;
    meta.number       = number;
    meta.file_size    = size;
    meta.smallest_key = table->smallestKey();
    meta.smallest_tag = table->smallestTag();
    meta.largest_key  = table->largestKey();
    meta.largest_tag  = table->largestTag();
    return meta;
}

std::shared_ptr<Version> buildFrom(const std::filesystem::path& dir,
                                   std::shared_ptr<const Version> base,
                                   const VersionEdit& edit) {
    VersionBuilder builder(dir, std::move(base));
    builder.apply(edit);
    return builder.build();
}

std::vector<uint64_t> numbersAt(const Version& version, int level) {
    std::vector<uint64_t> numbers;
    for (const auto& file : version.files(level)) {
        numbers.push_back(file.meta.number);
    }
    return numbers;
}

// What a lookup found, flattened so a test can assert on it in one line:
// "." for absent, "x" for a tombstone, otherwise the value.
std::string lookup(const Version& version, std::string_view key, uint64_t seq) {
    const auto entry = version.get(key, seq);
    if (!entry) {
        return ".";
    }
    return entry->type == ValueType::kDelete ? "x" : entry->value;
}

} // namespace

int main() {
    const std::filesystem::path dir = testDir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    // An empty tree answers every lookup with "absent" and asks for no
    // compaction it cannot do.
    {
        VersionEdit nothing;
        const auto  version = buildFrom(dir, nullptr, nothing);
        assert(version->numFiles() == 0);
        assert(lookup(*version, "anything", 100) == ".");
        assert(version->files(0).empty());
        assert(version->levelBytes(3) == 0);
    }

    // Level 0 overlaps, so a key present in two files is answered by the newer
    // one -- and "newer" is the larger file number, which is the order the
    // builder puts them in regardless of the order the edit listed them.
    {
        const FileMetaData older = writeTable(dir, 1, {
            { "k", 10, ValueType::kValue, "old" },
            { "m", 10, ValueType::kValue, "m-old" },
        });
        const FileMetaData newer = writeTable(dir, 2, {
            { "k", 20, ValueType::kValue, "new" },
        });

        VersionEdit edit;
        edit.addFile(0, older);   // deliberately listed oldest first
        edit.addFile(0, newer);
        const auto version = buildFrom(dir, nullptr, edit);

        assert(numbersAt(*version, 0) == (std::vector<uint64_t>{ 2, 1 }));
        assert(lookup(*version, "k", 100) == "new");
        // A snapshot below the newer write still sees the older one, since the
        // newer file simply has no version at or below that sequence number.
        assert(lookup(*version, "k", 15) == "old");
        // A key only the older file has is still reachable past the newer one.
        assert(lookup(*version, "m", 100) == "m-old");
        assert(lookup(*version, "absent", 100) == ".");
    }

    // A shallow tombstone masks an older value in a deeper level.
    {
        const FileMetaData deep = writeTable(dir, 10, {
            { "gone", 5, ValueType::kValue, "still-here" },
            { "kept", 5, ValueType::kValue, "kept-value" },
        });
        const FileMetaData shallow = writeTable(dir, 11, {
            { "gone", 50, ValueType::kDelete, "" },
        });

        VersionEdit edit;
        edit.addFile(2, deep);
        edit.addFile(0, shallow);
        const auto version = buildFrom(dir, nullptr, edit);

        assert(lookup(*version, "gone", 100) == "x");   // tombstone wins
        assert(lookup(*version, "kept", 100) == "kept-value");
        // Below the tombstone's sequence number the older value is visible
        // again -- the delete simply has not happened yet at that snapshot.
        assert(lookup(*version, "gone", 20) == "still-here");
    }

    // A level below 0 is a sorted run of disjoint files, so a lookup probes at
    // most one of them, and a key in the gap between two files finds nothing.
    {
        const FileMetaData a = writeTable(dir, 20, {
            { "a", 5, ValueType::kValue, "A" }, { "c", 5, ValueType::kValue, "C" },
        });
        const FileMetaData b = writeTable(dir, 21, {
            { "m", 5, ValueType::kValue, "M" }, { "p", 5, ValueType::kValue, "P" },
        });
        const FileMetaData c = writeTable(dir, 22, {
            { "x", 5, ValueType::kValue, "X" },
        });

        VersionEdit edit;
        edit.addFile(1, c); // out of key order on purpose
        edit.addFile(1, a);
        edit.addFile(1, b);
        const auto version = buildFrom(dir, nullptr, edit);

        // A run is stored ordered by key, not by file number or insert order.
        assert(numbersAt(*version, 1) == (std::vector<uint64_t>{ 20, 21, 22 }));

        assert(lookup(*version, "a", 100) == "A");
        assert(lookup(*version, "c", 100) == "C");
        assert(lookup(*version, "m", 100) == "M");
        assert(lookup(*version, "x", 100) == "X");

        // Gaps: before the first file, between two files, and past the last.
        assert(lookup(*version, "A", 100) == ".");
        assert(lookup(*version, "e", 100) == ".");
        assert(lookup(*version, "zz", 100) == ".");
    }

    // overlappingInputs is how a compaction picks its inputs, so its bounds
    // have to be inclusive and an unbounded side has to mean unbounded rather
    // than "the empty key".
    {
        const FileMetaData a = writeTable(dir, 30, {
            { "a", 5, ValueType::kValue, "A" }, { "c", 5, ValueType::kValue, "C" },
        });
        const FileMetaData b = writeTable(dir, 31, {
            { "m", 5, ValueType::kValue, "M" }, { "p", 5, ValueType::kValue, "P" },
        });
        const FileMetaData c = writeTable(dir, 32, {
            { "x", 5, ValueType::kValue, "X" },
        });

        VersionEdit edit;
        edit.addFile(1, a);
        edit.addFile(1, b);
        edit.addFile(1, c);
        const auto version = buildFrom(dir, nullptr, edit);

        const auto numbersOf = [](const std::vector<FileHandle>& files) {
            std::vector<uint64_t> out;
            for (const auto& f : files) { out.push_back(f.meta.number); }
            return out;
        };

        // Fully unbounded picks up the whole level.
        assert(numbersOf(version->overlappingInputs(1, std::nullopt, std::nullopt)) ==
               (std::vector<uint64_t>{ 30, 31, 32 }));

        // A range inside one file picks only that file.
        assert(numbersOf(version->overlappingInputs(1, "n", "o")) ==
               (std::vector<uint64_t>{ 31 }));

        // Bounds are inclusive: a range touching exactly one file's edge key
        // still selects it.
        assert(numbersOf(version->overlappingInputs(1, "c", "c")) ==
               (std::vector<uint64_t>{ 30 }));
        assert(numbersOf(version->overlappingInputs(1, "x", "x")) ==
               (std::vector<uint64_t>{ 32 }));

        // A range spanning a gap picks up the files on both sides but not the
        // one beyond it.
        assert(numbersOf(version->overlappingInputs(1, "b", "n")) ==
               (std::vector<uint64_t>{ 30, 31 }));

        // A range falling entirely in a gap picks up nothing.
        assert(version->overlappingInputs(1, "d", "l").empty());

        // One-sided bounds.
        assert(numbersOf(version->overlappingInputs(1, "q", std::nullopt)) ==
               (std::vector<uint64_t>{ 32 }));
        assert(numbersOf(version->overlappingInputs(1, std::nullopt, "b")) ==
               (std::vector<uint64_t>{ 30 }));
    }

    // Deriving a Version reuses surviving open table handles.
    {
        const FileMetaData one = writeTable(dir, 40, { { "a", 5, ValueType::kValue, "A" } });
        const FileMetaData two = writeTable(dir, 41, { { "b", 5, ValueType::kValue, "B" } });

        VersionEdit first;
        first.addFile(1, one);
        const auto v1 = buildFrom(dir, nullptr, first);

        VersionEdit second;
        second.addFile(1, two);
        const auto v2 = buildFrom(dir, v1, second);

        assert(numbersAt(*v2, 1) == (std::vector<uint64_t>{ 40, 41 }));
        assert(v2->files(1)[0].table == v1->files(1)[0].table); // carried forward
        assert(lookup(*v2, "a", 100) == "A");
        assert(lookup(*v2, "b", 100) == "B");

        // A deletion removes the file from the new version while the old one
        // keeps working -- which is what makes a compaction safe to install
        // while a reader is mid-lookup.
        VersionEdit third;
        third.deleteFile(1, 40);
        const auto v3 = buildFrom(dir, v2, third);

        assert(numbersAt(*v3, 1) == (std::vector<uint64_t>{ 41 }));
        assert(lookup(*v3, "a", 100) == ".");
        assert(lookup(*v1, "a", 100) == "A"); // the older version is untouched
    }

    // A file moving between levels in one edit: dropped from the old level and
    // added to the new, which is exactly the shape a compaction writes.
    {
        const FileMetaData meta = writeTable(dir, 50, { { "q", 5, ValueType::kValue, "Q" } });

        VersionEdit place;
        place.addFile(0, meta);
        const auto v1 = buildFrom(dir, nullptr, place);
        assert(v1->files(0).size() == 1 && v1->files(1).empty());

        VersionEdit move;
        move.deleteFile(0, 50);
        move.addFile(1, meta);
        const auto v2 = buildFrom(dir, v1, move);

        assert(v2->files(0).empty());
        assert(numbersAt(*v2, 1) == (std::vector<uint64_t>{ 50 }));
        assert(lookup(*v2, "q", 100) == "Q");
        assert(v2->files(1)[0].table == v1->files(0)[0].table); // still not reopened
    }

    // Iterating a whole version: level 0 first and newest first within it, so
    // the merge's earlier-child-wins tie-break yields the newest version of a
    // key duplicated across levels.
    {
        const FileMetaData deep = writeTable(dir, 60, {
            { "k", 5, ValueType::kValue, "deep" },
            { "z", 5, ValueType::kValue, "Z" },
        });
        const FileMetaData shallow = writeTable(dir, 61, {
            { "k", 50, ValueType::kValue, "shallow" },
        });

        VersionEdit edit;
        edit.addFile(1, deep);
        edit.addFile(0, shallow);
        const auto version = buildFrom(dir, nullptr, edit);

        std::vector<std::unique_ptr<InternalIterator>> children;
        version->addIterators(children);
        assert(children.size() == 2); // one L0 file, one concatenated level

        MergingIterator it(std::move(children));
        std::vector<std::string> seen;
        for (; it.valid(); it.next()) {
            seen.push_back(std::string(it.key()) + "=" + std::string(it.value()));
        }
        // Both versions of "k" are yielded, newest first: deduplication is a
        // compaction's drop rule, not the iterator's job.
        assert(seen == (std::vector<std::string>{ "k=shallow", "k=deep", "z=Z" }));
    }

    // Level 0 reaches its compaction threshold at the configured file count.
    {
        VersionEdit edit;
        for (uint64_t i = 0; i < kL0CompactionTrigger; ++i) {
            edit.addFile(0, writeTable(dir, 70 + i, {
                { "k" + std::to_string(i), 5, ValueType::kValue, "v" },
            }));
        }
        const auto version = buildFrom(dir, nullptr, edit);

        assert(version->compactionLevel() == 0);
        assert(version->compactionScore() >= 1.0);

        // One file short of the trigger is under budget.
        VersionEdit fewer;
        for (uint64_t i = 0; i + 1 < kL0CompactionTrigger; ++i) {
            fewer.addFile(0, writeTable(dir, 80 + i, {
                { "k" + std::to_string(i), 5, ValueType::kValue, "v" },
            }));
        }
        const auto under = buildFrom(dir, nullptr, fewer);
        assert(under->compactionScore() < 1.0);

        // The bottom level is never chosen: there is nowhere below it to go.
        assert(version->compactionLevel() < kNumLevels - 1);
    }

    // Level budgets grow by ten per level, which is what bounds the tree's
    // depth. Level 0 is scored by file count, so it has no byte budget here.
    {
        assert(maxBytesForLevel(1) == kLevel1MaxBytes);
        assert(maxBytesForLevel(2) == kLevel1MaxBytes * 10);
        assert(maxBytesForLevel(3) == kLevel1MaxBytes * 100);
    }

    std::filesystem::remove_all(dir);
    return 0;
}
