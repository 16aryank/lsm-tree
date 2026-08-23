// The merge over mixed sources: ordering across memtables and SSTables, how
// duplicates between overlapping inputs are resolved, the degenerate child
// sets, and the ownership that lets a merge outlive its caller's handles.
#include "src/iterator/memtable_iterator.h"
#include "src/iterator/merging_iterator.h"
#include "src/iterator/sstable_iterator.h"
#include "src/key.h"
#include "src/memtable/memtable.h"
#include "src/sstable/sstable.h"
#include "src/sstable/sstable_builder.h"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

using Table = MemTable<std::string, std::string>;

std::filesystem::path testDir() {
    return std::filesystem::temp_directory_path() /
           ("lsm-merge-test-" + std::to_string(::getpid()));
}

struct Entry {
    std::string key;
    uint64_t    tag;
    std::string value;
};

// Entries must reach a builder in internal-key order, so sort before writing.
std::shared_ptr<const SSTable> buildTable(const std::filesystem::path& path,
                                          std::vector<Entry> entries) {
    const InternalKeyComparator compare;
    std::sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
        return compare(a.key, a.tag, b.key, b.tag) < 0;
    });
    SSTableBuilder builder(path);
    for (const auto& e : entries) {
        builder.add(e.key, e.tag, e.value);
    }
    builder.finish();
    return SSTable::open(path);
}

std::shared_ptr<Table> buildMemTable(const std::vector<Entry>& entries) {
    auto table = std::make_shared<Table>();
    for (const auto& e : entries) {
        table->add(unpackSeqNumber(e.tag), unpackValueType(e.tag), e.key, e.value);
    }
    return table;
}

// Drains an iterator, asserting strict internal-key ordering as it goes.
std::vector<Entry> drain(InternalIterator& it) {
    const InternalKeyComparator compare;
    std::vector<Entry> out;
    for (; it.valid(); it.next()) {
        if (!out.empty()) {
            // Non-decreasing: equal internal keys are legal across inputs, so
            // the merge may not reorder but may repeat.
            assert(compare(out.back().key, out.back().tag, it.key(), it.tag()) <= 0);
        }
        out.push_back({ std::string(it.key()), it.tag(), std::string(it.value()) });
    }
    return out;
}

uint64_t tagOf(uint64_t seq, ValueType type = ValueType::kValue) {
    return packSeqAndType(seq, type);
}

} // namespace

int main() {
    const std::filesystem::path dir = testDir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    // Two SSTables and a memtable with deliberately interleaved keys. The merge
    // must produce one globally ordered stream, with every version of every key
    // present -- nothing deduplicated.
    {
        auto older = buildTable(dir / "older.sst", {
            { "a", tagOf(1), "a@1" },
            { "c", tagOf(1), "c@1" },
            { "e", tagOf(1), "e@1" },
        });
        auto newer = buildTable(dir / "newer.sst", {
            { "b", tagOf(5), "b@5" },
            { "c", tagOf(5), "c@5" },
            { "d", tagOf(5), "d@5" },
        });
        auto live = buildMemTable({
            { "a", tagOf(9), "a@9" },
            { "f", tagOf(9), "f@9" },
        });

        std::vector<std::unique_ptr<InternalIterator>> children;
        children.push_back(std::make_unique<MemTableIterator<std::string, std::string>>(live));
        children.push_back(std::make_unique<SSTableIterator>(newer));
        children.push_back(std::make_unique<SSTableIterator>(older));

        auto merged = newMergingIterator(std::move(children));
        const auto got = drain(*merged);

        const std::vector<std::pair<std::string, uint64_t>> expected = {
            { "a", tagOf(9) }, { "a", tagOf(1) },   // newest version of a key first
            { "b", tagOf(5) },
            { "c", tagOf(5) }, { "c", tagOf(1) },
            { "d", tagOf(5) },
            { "e", tagOf(1) },
            { "f", tagOf(9) },
        };
        assert(got.size() == expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            assert(got[i].key == expected[i].first);
            assert(got[i].tag == expected[i].second);
        }
    }

    // Tombstones travel through the merge like any other version -- a
    // compaction has to see them to apply its drop rule.
    {
        auto table = buildTable(dir / "tombstones.sst", {
            { "gone", tagOf(4, ValueType::kDelete), "" },
            { "gone", tagOf(2), "doomed" },
            { "kept", tagOf(3), "here" },
        });
        std::vector<std::unique_ptr<InternalIterator>> children;
        children.push_back(std::make_unique<SSTableIterator>(table));
        auto merged = newMergingIterator(std::move(children));
        const auto got = drain(*merged);

        assert(got.size() == 3);
        assert(got[0].key == "gone" && unpackValueType(got[0].tag) == ValueType::kDelete);
        assert(got[1].key == "gone" && got[1].value == "doomed");
        assert(got[2].key == "kept");
    }

    // Identical internal keys in two inputs: both are yielded, and the earlier
    // child comes first. This is the Level-0 overlap case, where the caller
    // passes its newest table first and relies on that ordering.
    {
        auto first  = buildTable(dir / "dup-a.sst", { { "k", tagOf(7), "from-first" } });
        auto second = buildTable(dir / "dup-b.sst", { { "k", tagOf(7), "from-second" } });

        std::vector<std::unique_ptr<InternalIterator>> children;
        children.push_back(std::make_unique<SSTableIterator>(first));
        children.push_back(std::make_unique<SSTableIterator>(second));
        auto merged = newMergingIterator(std::move(children));
        const auto got = drain(*merged);

        assert(got.size() == 2);
        assert(got[0].value == "from-first");
        assert(got[1].value == "from-second");
    }

    // Degenerate child sets: none, one, all-empty, and one that runs out while
    // others still have entries.
    {
        auto empty_merge = newMergingIterator({});
        assert(!empty_merge->valid());

        auto solo = buildTable(dir / "solo.sst", { { "only", tagOf(1), "v" } });
        std::vector<std::unique_ptr<InternalIterator>> one;
        one.push_back(std::make_unique<SSTableIterator>(solo));
        auto single = newMergingIterator(std::move(one));
        assert(drain(*single).size() == 1);

        auto empty_table = buildTable(dir / "empty.sst", {});
        std::vector<std::unique_ptr<InternalIterator>> empties;
        empties.push_back(std::make_unique<SSTableIterator>(empty_table));
        empties.push_back(std::make_unique<MemTableIterator<std::string, std::string>>(
            buildMemTable({})));
        auto all_empty = newMergingIterator(std::move(empties));
        assert(!all_empty->valid());
        assert(drain(*all_empty).empty());

        // One child holds a single early key and is exhausted long before the
        // other, which must keep producing.
        auto shorty = buildTable(dir / "short.sst", { { "aaa", tagOf(1), "s" } });
        auto lengthy = buildTable(dir / "long.sst", {
            { "bbb", tagOf(1), "l1" }, { "ccc", tagOf(1), "l2" }, { "ddd", tagOf(1), "l3" },
        });
        std::vector<std::unique_ptr<InternalIterator>> uneven;
        uneven.push_back(std::make_unique<SSTableIterator>(shorty));
        uneven.push_back(std::make_unique<SSTableIterator>(lengthy));
        auto merged = newMergingIterator(std::move(uneven));
        const auto got = drain(*merged);
        assert(got.size() == 4);
        assert(got[0].key == "aaa" && got[3].key == "ddd");
    }

    // Entries spanning many blocks, so the merge crosses block boundaries in
    // its children rather than staying inside one buffer.
    {
        std::vector<Entry> evens;
        std::vector<Entry> odds;
        for (int i = 0; i < 600; ++i) {
            char key[16];
            std::snprintf(key, sizeof(key), "key%06d", i);
            (i % 2 == 0 ? evens : odds).push_back({ key, tagOf(1), std::string(64, 'v') });
        }
        auto even_table = buildTable(dir / "evens.sst", evens);
        auto odd_table  = buildTable(dir / "odds.sst", odds);
        assert(even_table->numBlocks() > 1 && odd_table->numBlocks() > 1);

        std::vector<std::unique_ptr<InternalIterator>> children;
        children.push_back(std::make_unique<SSTableIterator>(even_table));
        children.push_back(std::make_unique<SSTableIterator>(odd_table));
        auto merged = newMergingIterator(std::move(children));
        const auto got = drain(*merged);

        assert(got.size() == 600);
        for (int i = 0; i < 600; ++i) {
            char key[16];
            std::snprintf(key, sizeof(key), "key%06d", i);
            assert(got[static_cast<size_t>(i)].key == key);
        }
    }

    // The adapters own their sources. A merge keeps working after the caller
    // drops every handle it had -- which is what lets a compaction run against
    // files a newer version has already stopped referencing.
    {
        std::vector<std::unique_ptr<InternalIterator>> children;
        {
            auto table = buildTable(dir / "owned.sst", {
                { "x", tagOf(2), "x@2" }, { "y", tagOf(2), "y@2" },
            });
            auto live = buildMemTable({ { "z", tagOf(3), "z@3" } });
            children.push_back(std::make_unique<SSTableIterator>(table));
            children.push_back(
                std::make_unique<MemTableIterator<std::string, std::string>>(live));
            // table and live go out of scope here; only the adapters hold them.
        }
        auto merged = newMergingIterator(std::move(children));
        const auto got = drain(*merged);
        assert(got.size() == 3);
        assert(got[0].key == "x" && got[1].key == "y" && got[2].key == "z");
        assert(got[2].value == "z@3");
    }

    std::filesystem::remove_all(dir);
    return 0;
}
