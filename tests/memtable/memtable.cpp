// Add/Get sequence-number semantics, tombstones, iteration order, size
// tracking, and ByteRange-concept enforcement for MemTable.

#include "src/memtable/memtable.h"
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    MemTable<std::string, std::string> mt;

    mt.add(1, ValueType::kValue, "a", "a-v1");
    mt.add(3, ValueType::kValue, "a", "a-v3");
    mt.add(2, ValueType::kValue, "b", "b-v2");

    // Get returns the value with the largest sequence number <= seq.
    assert(mt.get("a", 10) == "a-v3");
    assert(mt.get("a", 2) == "a-v1");
    assert(!mt.get("a", 0).has_value());
    assert(mt.get("b", 5) == "b-v2");
    assert(!mt.get("missing", 100).has_value());

    // A tombstone hides earlier values from seq >= the tombstone's seq, but
    // not from a seq before it.
    mt.add(5, ValueType::kDelete, "b", "");
    assert(mt.get("b", 4) == "b-v2");
    assert(!mt.get("b", 5).has_value());
    assert(!mt.get("b", 100).has_value());

    // A later value un-deletes the key.
    mt.add(6, ValueType::kValue, "b", "b-v6");
    assert(mt.get("b", 100) == "b-v6");

    // Iterator walks keys in ascending order, newest version of each key
    // first.
    auto* it = mt.newIterator();
    struct Seen { std::string key; uint64_t seq; };
    std::vector<Seen> seen;
    while (it->valid()) {
        // key() is a view into the arena; copy it out to keep it past `it`.
        seen.push_back({ std::string(it->key()), it->sequenceNumber() });
        it->next();
    }
    delete it;

    std::vector<Seen> expected = {
        { "a", 3 }, { "a", 1 }, { "b", 6 }, { "b", 5 }, { "b", 2 },
    };
    assert(seen.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        assert(seen[i].key == expected[i].key);
        assert(seen[i].seq == expected[i].seq);
    }

    // Size threshold.
    {
        MemTable<std::string, std::string> small(10);
        assert(!small.shouldFlush());
        small.add(1, ValueType::kValue, "k", "0123456789012345");
        assert(small.shouldFlush());
    }

    // Per-entry footprint. One arena allocation per entry: a 24-byte header,
    // 8 bytes per level (expected height 2 at p=0.5), then the key and value
    // bytes inline. For a 50-byte key and 100-byte value that is ~190 bytes.
    // This is the whole point of the arena rewrite, so guard it against
    // regressions.
    {
        constexpr size_t kCount      = 20000;
        constexpr size_t kKeyBytes   = 50;
        constexpr size_t kValueBytes = 100;

        MemTable<std::string, std::string> sized(SIZE_MAX);
        std::string value(kValueBytes, 'v');

        for (size_t i = 0; i < kCount; ++i) {
            std::string key = std::to_string(i);
            key.resize(kKeyBytes, 'k');
            sized.add(i + 1, ValueType::kValue, key, value);
        }

        const size_t per_entry = sized.approximateMemoryUsage() / kCount;
        assert(per_entry >= kKeyBytes + kValueBytes);   // can't beat the payload
        assert(per_entry <= 210);                       // ~190 expected, 10% headroom

        // Reserved memory is the real ceiling and stays close to what was
        // handed out -- block tails and page rounding only, no malloc slack.
        assert(sized.arenaMemoryUsage() >= sized.approximateMemoryUsage());
        assert(sized.arenaMemoryUsage() < sized.approximateMemoryUsage() * 3 / 2);
    }

    // A key type that isn't a contiguous run of bytes can't be copied into
    // the arena, so the constructor throws instead of failing to compile.
    {
        struct NotByteRange { int x; };
        bool threw = false;
        try {
            MemTable<NotByteRange, std::string> bad;
            (void)bad;
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    return 0;
}
