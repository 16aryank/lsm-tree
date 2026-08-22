// Round-trip through SSTableBuilder and SSTable: sequence-number and tombstone
// semantics on get(), multi-block seeks, iteration order, checksum enforcement,
// and rejection of files that aren't well-formed tables.

#include "src/sstable/sstable.h"
#include "src/sstable/sstable_builder.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

constexpr size_t kKeys = 200;

// Small enough that a few hundred entries span many blocks, which is what
// makes the index and the binary search over it worth testing.
constexpr size_t kTestBlockSize = 256;

// The key with an extra tombstone on top of its two values.
const std::string kDeletedKey = "key000010";

std::string keyAt(size_t i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "key%06zu", i);
    return buf;
}

std::filesystem::path testDir() {
    return std::filesystem::temp_directory_path() /
           ("lsm-sstable-test-" + std::to_string(::getpid()));
}

// Writes the fixture table: kKeys keys, two versions each (seq 2 then seq 1),
// plus a tombstone at seq 3 on kDeletedKey.
void buildFixture(const std::filesystem::path& path) {
    SSTableBuilder builder(path, kTestBlockSize);
    for (size_t i = 0; i < kKeys; ++i) {
        const std::string key = keyAt(i);
        if (key == kDeletedKey) {
            builder.add(key, packSeqAndType(3, ValueType::kDelete), "");
        }
        builder.add(key, packSeqAndType(2, ValueType::kValue), "vB-" + key);
        builder.add(key, packSeqAndType(1, ValueType::kValue), "vA-" + key);
    }
    assert(builder.numEntries() == kKeys * 2 + 1);
    const uint64_t size = builder.finish();
    assert(size > 0);
    assert(std::filesystem::file_size(path) == size);
}

// Flips one byte wherever `marker` appears in the file.
void corruptByte(const std::filesystem::path& path, const std::string& marker) {
    std::string contents;
    {
        std::ifstream in(path, std::ios::binary);
        contents.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const size_t at = contents.find(marker);
    assert(at != std::string::npos);

    std::fstream out(path, std::ios::binary | std::ios::in | std::ios::out);
    out.seekp(static_cast<std::streamoff>(at));
    const char flipped = static_cast<char>(contents[at] ^ 0x40);
    out.write(&flipped, 1);
}

} // namespace

int main() {
    const std::filesystem::path dir = testDir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    const std::filesystem::path path = dir / "000001.sst";
    buildFixture(path);

    {
        auto table = SSTable::open(path);
        assert(table->numEntries() == kKeys * 2 + 1);
        // The point of the block size chosen above.
        assert(table->numBlocks() > 1);
        assert(table->smallestKey() == keyAt(0));
        assert(table->largestKey() == keyAt(kKeys - 1));

        // Every key is reachable at every sequence number, including ones in
        // the middle and last blocks, which only the index can find.
        for (size_t i = 0; i < kKeys; ++i) {
            const std::string key = keyAt(i);
            auto newest = table->get(key, 100);
            assert(newest.has_value());
            if (key == kDeletedKey) {
                // A tombstone is a hit, not a miss: the caller needs to see it
                // to know the key is gone rather than merely absent here.
                assert(newest->type == ValueType::kDelete);
                assert(newest->seq == 3);
            } else {
                assert(newest->type == ValueType::kValue);
                assert(newest->seq == 2);
                assert(newest->value == "vB-" + key);
            }

            // Reads at an older snapshot see the older version.
            auto older = table->get(key, 1);
            assert(older.has_value());
            assert(older->seq == 1);
            assert(older->value == "vA-" + key);

            // Nothing was written at or below seq 0.
            assert(!table->get(key, 0).has_value());
        }

        // The version under the tombstone is still visible to an older read.
        auto under = table->get(kDeletedKey, 2);
        assert(under.has_value());
        assert(under->type == ValueType::kValue);
        assert(under->value == "vB-" + kDeletedKey);

        // Misses before, after, and between the keys that exist.
        assert(!table->get("aaa", 100).has_value());
        assert(!table->get("zzz", 100).has_value());
        assert(!table->get("key000000x", 100).has_value());

        // Iteration walks internal-key order across block boundaries: user key
        // ascending, newest version of each key first.
        auto it = table->newIterator();
        const InternalKeyComparator compare;
        std::string previous_key;
        uint64_t previous_tag = 0;
        size_t seen = 0;
        while (it.valid()) {
            const auto tag = packSeqAndType(it.sequenceNumber(), it.valueType());
            if (seen > 0) {
                assert(compare(previous_key, previous_tag, it.key(), tag) < 0);
            }
            previous_key.assign(it.key());
            previous_tag = tag;
            ++seen;
            it.next();
        }
        assert(seen == table->numEntries());
        assert(previous_key == keyAt(kKeys - 1));
    }

    // An empty table is still a valid file: a footer, and nothing to find.
    {
        const std::filesystem::path empty_path = dir / "empty.sst";
        SSTableBuilder builder(empty_path);
        assert(builder.finish() == kFooterSize + kBlockTrailerSize);

        auto table = SSTable::open(empty_path);
        assert(table->numEntries() == 0);
        assert(table->numBlocks() == 0);
        assert(table->smallestKey().empty());
        assert(!table->get("anything", 100).has_value());
        assert(!table->newIterator().valid());
    }

    // abandon() leaves nothing behind, which is what a failed flush relies on.
    {
        const std::filesystem::path abandoned = dir / "abandoned.sst";
        {
            SSTableBuilder builder(abandoned);
            builder.add("k", packSeqAndType(1, ValueType::kValue), "v");
            assert(std::filesystem::exists(abandoned));
            builder.abandon();
        }
        assert(!std::filesystem::exists(abandoned));
    }

    // The destructor abandons a builder that was never finished, so a throw
    // mid-flush cannot leave a torso of a file behind.
    {
        const std::filesystem::path dropped = dir / "dropped.sst";
        {
            SSTableBuilder builder(dropped);
            builder.add("k", packSeqAndType(1, ValueType::kValue), "v");
        }
        assert(!std::filesystem::exists(dropped));
    }

    // A flipped byte in a data block is caught by its checksum rather than
    // handed back as a value. The corrupted key lives past the first block, so
    // open() -- which reads the index and block 0 -- still succeeds.
    {
        const std::filesystem::path damaged = dir / "damaged.sst";
        buildFixture(damaged);
        corruptByte(damaged, "vB-" + keyAt(150));

        auto table = SSTable::open(damaged);
        bool threw = false;
        try {
            (void)table->get(keyAt(150), 100);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);

        // Blocks are verified independently, so the rest of the table is fine.
        assert(table->get(keyAt(0), 100).has_value());
    }

    // Files that aren't tables are rejected at open() rather than misread.
    {
        const std::filesystem::path junk = dir / "junk.sst";
        {
            std::ofstream out(junk, std::ios::binary);
            out << std::string(128, 'x');
        }
        bool threw = false;
        try {
            (void)SSTable::open(junk);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);
    }

    // A build that died before its footer landed looks like a truncated file.
    {
        const std::filesystem::path truncated = dir / "truncated.sst";
        buildFixture(truncated);
        std::filesystem::resize_file(truncated, std::filesystem::file_size(truncated) - 8);

        bool threw = false;
        try {
            (void)SSTable::open(truncated);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);
    }

    // A file too short to hold even a footer.
    {
        const std::filesystem::path tiny = dir / "tiny.sst";
        {
            std::ofstream out(tiny, std::ios::binary);
            out << "abc";
        }
        bool threw = false;
        try {
            (void)SSTable::open(tiny);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);
    }

    // Opening something that isn't there is an errno failure, not corruption.
    {
        bool threw = false;
        try {
            (void)SSTable::open(dir / "does-not-exist.sst");
        } catch (const std::system_error&) {
            threw = true;
        }
        assert(threw);
    }

    std::filesystem::remove_all(dir);
    return 0;
}
