// Iterator positioning across present keys, gaps, and file boundaries.
#include "src/iterator/concatenating_iterator.h"
#include "src/iterator/memtable_iterator.h"
#include "src/iterator/merging_iterator.h"
#include "src/iterator/sstable_iterator.h"
#include "src/key.h"
#include "src/memtable/memtable.h"
#include "src/sstable/sstable.h"
#include "src/sstable/sstable_builder.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

using Table = MemTable<std::string, std::string>;

std::filesystem::path testDir() {
    return std::filesystem::temp_directory_path() /
           ("lsm-seek-test-" + std::to_string(::getpid()));
}

struct Entry {
    std::string key;
    uint64_t    tag;
    std::string value;
};

uint64_t tagOf(uint64_t seq) { return packSeqAndType(seq, ValueType::kValue); }

std::shared_ptr<const SSTable> buildTable(const std::filesystem::path& path,
                                          std::vector<Entry> entries,
                                          size_t block_size = kSSTableBlockSize) {
    const InternalKeyComparator compare;
    std::sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
        return compare(a.key, a.tag, b.key, b.tag) < 0;
    });
    SSTableBuilder builder(path, block_size);
    for (const auto& e : entries) {
        builder.add(e.key, e.tag, e.value);
    }
    builder.finish();
    return SSTable::open(path);
}

// The key an iterator is sitting on, or "." for an exhausted one -- shorter to
// assert against than a valid()/key() pair at every call site.
std::string at(const InternalIterator& it) {
    return it.valid() ? std::string(it.key()) : ".";
}

std::vector<std::string> drainKeys(InternalIterator& it) {
    std::vector<std::string> keys;
    for (; it.valid(); it.next()) {
        keys.emplace_back(it.key());
    }
    return keys;
}

} // namespace

int main() {
    const std::filesystem::path dir = testDir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    const std::vector<Entry> letters = {
        { "b", tagOf(10), "B" }, { "d", tagOf(10), "D" },
        { "f", tagOf(10), "F" }, { "h", tagOf(10), "H" },
    };

    // An SSTable spanning several blocks, so a seek has to pick the right one
    // rather than always landing in the only block there is.
    {
        std::vector<Entry> many;
        for (int i = 0; i < 400; ++i) {
            char key[16];
            std::snprintf(key, sizeof(key), "key%04d", i);
            many.push_back({ key, tagOf(5), std::string(64, 'x') });
        }
        const auto table = buildTable(dir / "many.sst", many, 256);
        assert(table->numBlocks() > 1); // otherwise this proves nothing

        SSTableIterator it(table);
        it.seek("key0200", tagOf(5));
        assert(at(it) == "key0200");

        // Between keys: "key0200a" sorts after key0200 and before key0201.
        it.seek("key0200a", tagOf(5));
        assert(at(it) == "key0201");

        // Before everything, and past everything.
        it.seek("", 0);
        assert(at(it) == "key0000");
        it.seek("zzzz", tagOf(5));
        assert(!it.valid());

        // seekToFirst undoes a seek, including one that exhausted the cursor.
        it.seekToFirst();
        assert(at(it) == "key0000");

        // Seeking to the last key leaves exactly one entry to walk.
        it.seek("key0399", tagOf(5));
        assert(at(it) == "key0399");
        it.next();
        assert(!it.valid());
    }

    // Tags sort descending within a user key, so a seek at a sequence number
    // lands on the newest version at or below it -- the property Version::get
    // and every compaction drop rule rest on.
    {
        const auto table = buildTable(dir / "versions.sst", {
            { "k", tagOf(30), "v30" },
            { "k", tagOf(20), "v20" },
            { "k", tagOf(10), "v10" },
        });
        SSTableIterator it(table);

        it.seek("k", tagOf(25));
        assert(at(it) == "k" && it.value() == "v20");

        it.seek("k", tagOf(30));
        assert(at(it) == "k" && it.value() == "v30");

        // Older than every version present: the cursor moves past this key
        // entirely rather than returning a version the caller may not see.
        it.seek("k", tagOf(5));
        assert(!it.valid());
    }

    // The memtable iterator answers the same questions off the skip list.
    {
        auto table = std::make_shared<Table>();
        for (const auto& e : letters) {
            table->add(unpackSeqNumber(e.tag), unpackValueType(e.tag), e.key, e.value);
        }
        MemTableIterator<std::string, std::string> it(table);

        it.seek("d", tagOf(10));
        assert(at(it) == "d");
        it.seek("c", tagOf(10));   // between b and d
        assert(at(it) == "d");
        it.seek("a", tagOf(10));   // before the first
        assert(at(it) == "b");
        it.seek("z", tagOf(10));   // past the last
        assert(!it.valid());
        it.seekToFirst();
        assert(at(it) == "b");
    }

    // A merge seeks every child and re-picks the smallest, so a target that
    // only one child can satisfy still positions the merge correctly.
    {
        const auto evens = buildTable(dir / "evens.sst", {
            { "b", tagOf(10), "B" }, { "d", tagOf(10), "D" },
        });
        const auto odds = buildTable(dir / "odds.sst", {
            { "a", tagOf(10), "A" }, { "c", tagOf(10), "C" },
        });

        std::vector<std::unique_ptr<InternalIterator>> children;
        children.push_back(std::make_unique<SSTableIterator>(evens));
        children.push_back(std::make_unique<SSTableIterator>(odds));
        MergingIterator it(std::move(children));

        assert(at(it) == "a"); // constructed already positioned
        it.seek("bb", tagOf(10));
        assert(at(it) == "c"); // the odd child wins even though it is second
        assert(drainKeys(it) == (std::vector<std::string>{ "c", "d" }));

        it.seekToFirst();
        assert(drainKeys(it) == (std::vector<std::string>{ "a", "b", "c", "d" }));
    }

    // The concatenating iterator over a sorted, disjoint run.
    {
        const auto first  = buildTable(dir / "run1.sst", {
            { "a", tagOf(10), "A" }, { "c", tagOf(10), "C" },
        });
        const auto second = buildTable(dir / "run2.sst", {
            { "m", tagOf(10), "M" }, { "p", tagOf(10), "P" },
        });
        const auto third  = buildTable(dir / "run3.sst", {
            { "x", tagOf(10), "X" },
        });

        ConcatenatingIterator it({ first, second, third });

        // A full scan crosses every file boundary in order.
        assert(drainKeys(it) == (std::vector<std::string>{ "a", "c", "m", "p", "x" }));

        it.seekToFirst();
        assert(at(it) == "a");

        // Seeks landing inside each file.
        it.seek("c", tagOf(10));
        assert(at(it) == "c");
        it.seek("m", tagOf(10));
        assert(at(it) == "m");
        it.seek("x", tagOf(10));
        assert(at(it) == "x");

        // The case the binary search alone gets wrong: "e" is past every entry
        // in the first file but before the second's range, so the file whose
        // largest key is >= the target is already the second one. "h" instead
        // falls in the gap after the first file's last key, and the answer is
        // the second file's first entry -- reached only by rolling forward.
        it.seek("e", tagOf(10));
        assert(at(it) == "m");
        it.seek("h", tagOf(10));
        assert(at(it) == "m");

        // Before everything and past everything.
        it.seek("", 0);
        assert(at(it) == "a");
        it.seek("zz", tagOf(10));
        assert(!it.valid());

        // Walking on from a seek crosses the remaining boundaries.
        it.seek("p", tagOf(10));
        assert(drainKeys(it) == (std::vector<std::string>{ "p", "x" }));
    }

    // Empty tables carry no key range to binary search against, so they are
    // dropped rather than left to answer a seek with nothing.
    {
        SSTableBuilder builder(dir / "empty.sst");
        builder.finish();
        const auto empty = SSTable::open(dir / "empty.sst");
        assert(empty->numEntries() == 0);

        const auto real = buildTable(dir / "real.sst", {
            { "k", tagOf(10), "V" },
        });

        ConcatenatingIterator it({ empty, real, empty });
        assert(drainKeys(it) == (std::vector<std::string>{ "k" }));
        it.seek("a", tagOf(10));
        assert(at(it) == "k");
    }

    // A run with nothing in it is valid and simply never positions.
    {
        ConcatenatingIterator it({ });
        assert(!it.valid());
        it.seek("a", tagOf(10));
        assert(!it.valid());
        it.seekToFirst();
        assert(!it.valid());
        it.next(); // must not crash on an exhausted cursor
        assert(!it.valid());
    }

    std::filesystem::remove_all(dir);
    return 0;
}
