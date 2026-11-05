// Exercise the output writer with exact internal-key ties, which ordinary
// writes cannot create across tables, and binary keys and values.
#include "src/db/db.h"
#include "src/db/filename.h"
#include "src/sstable/sstable_builder.h"
#include "src/version/version_set.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

namespace {

using Engine = DB<std::string, std::string>;

struct Entry {
    std::string key;
    uint64_t seq;
    ValueType type;
    std::string value;

    bool operator==(const Entry&) const = default;
};

FileMetaData writeTable(const std::filesystem::path& dir, uint64_t number,
                        std::vector<Entry> entries) {
    const InternalKeyComparator compare;
    std::sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
        return compare(a.key, packSeqAndType(a.seq, a.type),
                       b.key, packSeqAndType(b.seq, b.type)) < 0;
    });
    SSTableBuilder builder(tableFileName(dir, number), 128);
    for (const auto& entry : entries) {
        builder.add(entry.key, packSeqAndType(entry.seq, entry.type), entry.value);
    }

    FileMetaData meta;
    meta.number = number;
    meta.file_size = builder.finish();
    meta.smallest_key = builder.smallestKey();
    meta.smallest_tag = builder.smallestTag();
    meta.largest_key = builder.largestKey();
    meta.largest_tag = builder.largestTag();
    return meta;
}

std::vector<Entry> readEntries(const SSTable& table) {
    std::vector<Entry> entries;
    auto it = table.cursor();
    for (it.seekToFirst(); it.valid(); it.next()) {
        entries.push_back({ std::string(it.key()), it.sequenceNumber(), it.valueType(),
                            std::string(it.value()) });
    }
    return entries;
}

void sameRead(const Version& before, const Version& after,
              std::string_view key, uint64_t seq) {
    const auto left = before.get(key, seq);
    const auto right = after.get(key, seq);
    assert(left.has_value() == right.has_value());
    if (left) {
        assert(left->seq == right->seq);
        assert(left->type == right->type);
        assert(left->value == right->value);
    }
}

} // namespace

int main() {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lsm-compaction-output-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);

    const std::string binary_key{ 'b', 'i', 'n', '\0', static_cast<char>(0xff) };
    const std::string binary_value{ '\0', static_cast<char>(0xff), 'A' };
    const std::string wide10(220, 'q');
    const std::string wide9(220, 'x');
    const std::string wide8(220, 'y');
    const std::string wide7(220, 'z');

    std::shared_ptr<const Version> before;
    {
        VersionSet versions(dir);
        versions.createNew();
        VersionEdit edit;
        // Level 0 is searched in descending file-number order. Three files
        // and the level-1 input deliberately disagree on the exact dup tag.
        edit.addFile(0, writeTable(dir, versions.newFileNumber(), {
            { "a", 4, ValueType::kValue, "A4" },
            { "dup", 8, ValueType::kValue, "old" },
            { "keep", 2, ValueType::kValue, "K2" },
            { "wide", 7, ValueType::kValue, wide7 },
        }));
        edit.addFile(0, writeTable(dir, versions.newFileNumber(), {
            { "dup", 8, ValueType::kValue, "middle" },
            { "multi", 6, ValueType::kValue, "M6" },
            { "wide", 8, ValueType::kValue, wide8 },
        }));
        edit.addFile(0, writeTable(dir, versions.newFileNumber(), {
            { "gone", 5, ValueType::kDelete, "" },
            { "multi", 7, ValueType::kValue, "M7" },
            { "wide", 9, ValueType::kValue, wide9 },
        }));
        edit.addFile(0, writeTable(dir, versions.newFileNumber(), {
            { binary_key, 6, ValueType::kValue, binary_value },
            { "dup", 8, ValueType::kValue, "new" },
            { "multi", 8, ValueType::kValue, "M8" },
            { "wide", 10, ValueType::kValue, wide10 },
        }));
        edit.addFile(1, writeTable(dir, versions.newFileNumber(), {
            { "deep", 2, ValueType::kDelete, "" },
            { "dup", 8, ValueType::kValue, "destination" },
            { "gone", 3, ValueType::kValue, "G3" },
            { "multi", 3, ValueType::kValue, "M3" },
            { "wide", 3, ValueType::kValue, "W3" },
        }));
        // The deeper value forces the level-1 tombstone to survive.
        edit.addFile(2, writeTable(dir, versions.newFileNumber(), {
            { "deep", 1, ValueType::kValue, "buried" },
        }));
        // Keep the writer's cutoff at 3 so the merge must preserve every
        // distinct newer version supplied by this synthetic table fixture.
        versions.setLastSequence(3);
        versions.logAndApply(edit);
        before = versions.current();
        assert(before->files(0).size() == 4);
    }

    Options options;
    options.dir = dir;
    options.memtable_size_bytes = 1 << 20;
    options.sstable_block_size = 128;
    options.compaction_output_file_size = 180;
    {
        auto db = Engine::open(options);
        db->waitForPendingCompaction();
        assert(db->lastCompactionError().empty());
        assert(db->compactionStats().runs_by_level[0] == 1);
        const auto after = db->currentVersion();
        assert(after->files(0).empty());
        assert(after->files(1).size() > 1);
        assert(after->files(2).size() == 1);

        std::vector<Entry> actual;
        size_t wide_files = 0;
        for (const auto& file : after->files(1)) {
            const auto entries = readEntries(*file.table);
            actual.insert(actual.end(), entries.begin(), entries.end());
            if (file.meta.smallest_key <= "wide" && file.meta.largest_key >= "wide") {
                ++wide_files;
                assert(file.meta.file_size > options.compaction_output_file_size);
                assert(entries.size() == 5);
            }
        }
        assert(wide_files == 1); // even five oversized versions stay together
        for (size_t i = 1; i < after->files(1).size(); ++i) {
            assert(after->files(1)[i - 1].meta.largest_key <
                   after->files(1)[i].meta.smallest_key);
        }
        const std::vector<Entry> expected{
            { "a", 4, ValueType::kValue, "A4" },
            { binary_key, 6, ValueType::kValue, binary_value },
            { "deep", 2, ValueType::kDelete, "" },
            { "dup", 8, ValueType::kValue, "new" },
            { "gone", 5, ValueType::kDelete, "" },
            { "gone", 3, ValueType::kValue, "G3" },
            { "keep", 2, ValueType::kValue, "K2" },
            { "multi", 8, ValueType::kValue, "M8" },
            { "multi", 7, ValueType::kValue, "M7" },
            { "multi", 6, ValueType::kValue, "M6" },
            { "multi", 3, ValueType::kValue, "M3" },
            { "wide", 10, ValueType::kValue, wide10 },
            { "wide", 9, ValueType::kValue, wide9 },
            { "wide", 8, ValueType::kValue, wide8 },
            { "wide", 7, ValueType::kValue, wide7 },
            { "wide", 3, ValueType::kValue, "W3" },
        };
        assert(actual == expected);

        for (const auto& key : { std::string("a"), binary_key, std::string("deep"),
                                 std::string("dup"), std::string("gone"),
                                 std::string("keep"), std::string("multi"),
                                 std::string("wide"), std::string("missing") }) {
            for (uint64_t seq : std::array<uint64_t, 8>{ 3, 4, 5, 6, 7, 8, 9, 10 }) {
                sameRead(*before, *after, key, seq);
            }
        }
        assert(db->get("keep") == std::optional<std::string>("K2"));
        assert(!db->get("deep"));
    }
    std::filesystem::remove_all(dir);
}
