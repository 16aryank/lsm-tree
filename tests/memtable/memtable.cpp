// Add/Get sequence-number semantics, tombstones, iteration order, size
// tracking, and Orderable-concept enforcement for MemTable.

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
        seen.push_back({ it->key(), it->sequenceNumber() });
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

    // A key type without operator< / operator== can't satisfy the
    // SkipList's Orderable concept, so the constructor throws instead of
    // failing to compile.
    {
        struct NotOrderable { int x; };
        bool threw = false;
        try {
            MemTable<NotOrderable, std::string> bad;
            (void)bad;
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    return 0;
}
