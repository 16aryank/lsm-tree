// Insertion, lookup, ordering, shadowing, removal, and node-layout checks for
// the arena-backed skip list.

#include "src/memtable/skiplist.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Entries order by key ascending, then tag descending.
void testBasicLookup() {
    Arena arena;
    SkipList<> list(arena);

    list.insert("five", 5, "FIVE");
    list.insert("three", 3, "THREE");
    list.insert("eight", 8, "EIGHT");

    assert(list.search("five", 5) == "FIVE");
    assert(list.search("three", 3) == "THREE");
    assert(list.search("eight", 8) == "EIGHT");

    // Right key, wrong tag is not a match.
    assert(!list.search("five", 4).has_value());
    // Absent key.
    assert(!list.search("four", 4).has_value());
}

// seek() is a lower_bound over (key, tag).
void testSeekOrdering() {
    Arena arena;
    SkipList<> list(arena);

    list.insert("b", 2, "b2");
    list.insert("a", 1, "a1");
    list.insert("c", 3, "c3");

    auto it = list.seek("a", 1);
    assert(it.valid() && it.key() == "a");

    // Lands on the first entry at or after the probe.
    it = list.seek("aa", 0);
    assert(it.valid() && it.key() == "b");

    // Past the end.
    it = list.seek("z", 0);
    assert(!it.valid());
}

// Higher tags sort first within one key, so the newest version is found first.
void testTagDescendingWithinKey() {
    Arena arena;
    SkipList<> list(arena);

    list.insert("k", 1, "v1");
    list.insert("k", 9, "v9");
    list.insert("k", 5, "v5");

    std::vector<uint64_t> tags;
    for (auto it = list.begin(); it.valid(); it.next()) {
        assert(it.key() == "k");
        tags.push_back(it.tag());
    }
    const std::vector<uint64_t> expected = { 9, 5, 1 };
    assert(tags == expected);

    // A probe at a given tag lands on that version, not a newer one.
    auto it = list.seek("k", 5);
    assert(it.valid() && it.value() == "v5");
}

// An identical (key, tag) is shadowed, not overwritten: the newer node is
// linked ahead of the older one and wins every lookup.
void testDuplicateShadowing() {
    Arena arena;
    SkipList<> list(arena);

    list.insert("k", 7, "first");
    assert(list.search("k", 7) == "first");

    list.insert("k", 7, "second");
    assert(list.search("k", 7) == "second");

    // Both nodes are still present -- nothing was reclaimed or replaced.
    size_t count = 0;
    for (auto it = list.begin(); it.valid(); it.next()) {
        ++count;
    }
    assert(count == 2);
    assert(list.begin().value() == "second");
}

void testRemove() {
    Arena arena;
    SkipList<> list(arena);

    list.insert("a", 1, "a1");
    list.insert("b", 2, "b2");
    list.insert("c", 3, "c3");

    list.remove("b", 2);
    assert(!list.search("b", 2).has_value());
    assert(list.search("a", 1) == "a1");
    assert(list.search("c", 3) == "c3");

    // Removing something absent is a no-op.
    list.remove("zzz", 0);
    list.remove("a", 999); // right key, wrong tag
    assert(list.search("a", 1) == "a1");

    std::vector<std::string> keys;
    for (auto it = list.begin(); it.valid(); it.next()) {
        keys.emplace_back(it.key());
    }
    const std::vector<std::string> expected = { "a", "c" };
    assert(keys == expected);
}

// Iteration visits every entry in ascending key order.
void testIterationOrder() {
    Arena arena;
    SkipList<> list(arena);

    // Insert scattered so the list can't be accidentally sorted by arrival.
    const std::vector<std::string> input = { "m", "d", "z", "a", "q", "f" };
    for (const std::string& key : input) {
        list.insert(key, 1, key + "-value");
    }

    std::vector<std::string> seen;
    for (auto it = list.begin(); it.valid(); it.next()) {
        seen.emplace_back(it.key());
        assert(it.value() == seen.back() + "-value");
    }

    const std::vector<std::string> expected = { "a", "d", "f", "m", "q", "z" };
    assert(seen == expected);
}

// Empty keys and values round-trip, and byte-comparison handles a key that is
// a strict prefix of another (shorter sorts first).
void testEdgeCaseKeys() {
    Arena arena;
    SkipList<> list(arena);

    list.insert("", 1, "");
    list.insert("ab", 1, "ab-value");
    list.insert("a", 1, "a-value");

    assert(list.search("", 1) == "");
    assert(list.search("a", 1) == "a-value");
    assert(list.search("ab", 1) == "ab-value");

    std::vector<std::string> seen;
    for (auto it = list.begin(); it.valid(); it.next()) {
        seen.emplace_back(it.key());
    }
    const std::vector<std::string> expected = { "", "a", "ab" };
    assert(seen == expected);

    // Keys with high bytes compare unsigned, matching std::string's ordering.
    Arena bytes_arena;
    SkipList<> bytes(bytes_arena);
    const std::string low(1, '\x01');
    const std::string high(1, '\xff');
    bytes.insert(high, 1, "high");
    bytes.insert(low, 1, "low");
    auto it = bytes.begin();
    assert(it.valid() && it.value() == "low");
    it.next();
    assert(it.valid() && it.value() == "high");
}

// Values far larger than a block still work, taking the arena's dedicated
// mapping path, and their bytes survive intact.
void testLargeValue() {
    Arena arena;
    SkipList<> list(arena);

    const std::string big(3u << 20, 'x'); // 3 MB
    list.insert("big", 1, big);
    list.insert("small", 1, "s");

    auto found = list.search("big", 1);
    assert(found.has_value());
    assert(found->size() == big.size());
    assert(*found == big);
    assert(list.search("small", 1) == "s");
}

// One entry is a single allocation: a 24-byte header, 8 bytes per level, then
// the key and value bytes inline. This is the property the whole rewrite is
// for, so pin it down.
void testNodeFootprint() {
    Arena arena;
    SkipList<> list(arena);

    const std::string key(50, 'k');
    const std::string value(100, 'v');

    const size_t before = arena.bytesAllocated();
    list.insert(key, 1, value);
    const size_t entry_bytes = arena.bytesAllocated() - before;

    constexpr size_t kHeader = 24;
    constexpr size_t kPtr    = 8;
    const size_t payload     = key.size() + value.size();

    // Height is random in [1, kDefaultSkipListLevel], so bound it.
    assert(entry_bytes >= kHeader + 1 * kPtr + payload);
    assert(entry_bytes <= kHeader + kDefaultSkipListLevel * kPtr + payload);
}

// A large batch stays correct and keeps per-entry cost near the expected
// height-2 figure of header + 2 pointers + payload.
void testBulkInsertFootprint() {
    Arena arena;
    SkipList<> list(arena);

    constexpr size_t kCount = 20000;
    const std::string filler(100, 'v');

    for (size_t i = 0; i < kCount; ++i) {
        list.insert(std::to_string(i * 2), i, filler);
    }

    // Every entry is findable.
    for (size_t i = 0; i < kCount; ++i) {
        auto found = list.search(std::to_string(i * 2), i);
        assert(found.has_value() && *found == filler);
    }

    size_t count = 0;
    for (auto it = list.begin(); it.valid(); it.next()) {
        ++count;
    }
    assert(count == kCount);

    // Expected: 24 header + ~16 forward + ~5 key + 100 value ~= 145.
    const size_t per_entry = arena.bytesAllocated() / kCount;
    assert(per_entry < 200);
}

} // namespace

int main() {
    testBasicLookup();
    testSeekOrdering();
    testTagDescendingWithinKey();
    testDuplicateShadowing();
    testRemove();
    testIterationOrder();
    testEdgeCaseKeys();
    testLargeValue();
    testNodeFootprint();
    testBulkInsertFootprint();
    return 0;
}
