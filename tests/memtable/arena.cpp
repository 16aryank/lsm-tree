// Alignment, block growth, oversized-allocation handling, byte accounting,
// and concurrent non-overlap for the Arena bump allocator.

#include "src/memtable/arena.h"

#include <unistd.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

bool isAligned(const char* p) {
    return reinterpret_cast<uintptr_t>(p) % kArenaAlignment == 0;
}

size_t pageSize() {
    const long v = ::sysconf(_SC_PAGESIZE);
    return v > 0 ? static_cast<size_t>(v) : 4096;
}

// A fresh arena maps nothing until it is actually used.
void testLazyMapping() {
    Arena arena;
    assert(arena.bytesAllocated() == 0);
    assert(arena.bytesReserved() == 0);

    char* p = arena.allocate(1);
    assert(p != nullptr);
    assert(arena.bytesAllocated() == 1);
    assert(arena.bytesReserved() >= pageSize());
}

// Every hand-out is 8-byte aligned regardless of the sizes requested before
// it, and bytesAllocated() is the exact sum of what was asked for.
void testAlignmentAndAccounting() {
    Arena arena;
    size_t requested = 0;

    // Deliberately odd sizes so each allocation forces realignment.
    for (size_t bytes = 1; bytes <= 200; ++bytes) {
        char* p = arena.allocate(bytes);
        assert(p != nullptr);
        assert(isAligned(p));
        requested += bytes;
        assert(arena.bytesAllocated() == requested);
    }

    assert(arena.bytesReserved() >= arena.bytesAllocated());
    assert(arena.bytesReserved() % pageSize() == 0);
}

// A zero-byte request still yields a usable, distinct pointer.
void testZeroSized() {
    Arena arena;
    char* a = arena.allocate(0);
    char* b = arena.allocate(0);
    assert(a != nullptr && b != nullptr);
    assert(a != b);
}

// Allocations that outgrow the first block keep working, the arena maps more
// blocks, and nothing previously handed out is disturbed.
void testBlockGrowth() {
    // Block sizes are floored at one page, so cross boundaries by volume.
    Arena arena;

    constexpr size_t kCount = 4000;
    constexpr size_t kChunk = 64;

    std::vector<char*> pointers;
    pointers.reserve(kCount);

    for (size_t i = 0; i < kCount; ++i) {
        char* p = arena.allocate(kChunk);
        assert(isAligned(p));
        std::memset(p, static_cast<int>(i & 0xFF), kChunk);
        pointers.push_back(p);
    }

    // Growth must have crossed at least one block boundary.
    assert(arena.bytesReserved() > pageSize());

    // Earlier regions survived every subsequent block map.
    for (size_t i = 0; i < kCount; ++i) {
        const auto expected = static_cast<unsigned char>(i & 0xFF);
        for (size_t b = 0; b < kChunk; ++b) {
            assert(static_cast<unsigned char>(pointers[i][b]) == expected);
        }
    }

    assert(arena.bytesAllocated() == kCount * kChunk);
}

// A request larger than a quarter of the next block size gets its own
// dedicated mapping, and the block in use keeps serving small requests after.
void testOversizedAllocation() {
    Arena arena;

    char* small_before = arena.allocate(32);
    std::memset(small_before, 'a', 32);

    constexpr size_t kBig = 2u << 20; // 2 MB, larger than kArenaMaxBlockBytes
    char* big = arena.allocate(kBig);
    assert(big != nullptr);
    assert(isAligned(big));
    std::memset(big, 'b', kBig);

    char* small_after = arena.allocate(32);
    std::memset(small_after, 'c', 32);

    // The oversized region did not overlap either neighbour.
    for (size_t i = 0; i < 32; ++i) {
        assert(small_before[i] == 'a');
        assert(small_after[i] == 'c');
    }
    for (size_t i = 0; i < kBig; ++i) {
        assert(big[i] == 'b');
    }

    assert(arena.bytesAllocated() == 32 + kBig + 32);
    assert(arena.bytesReserved() >= kBig);
}

// Concurrent allocate() must never hand the same bytes to two threads. Each
// thread stamps its regions with its own id; if any two regions overlapped,
// one thread's stamp would be clobbered by the other's.
void testConcurrentNonOverlap() {
    constexpr size_t kThreads         = 8;
    constexpr size_t kPerThread       = 2000;
    constexpr size_t kMaxAllocation   = 64;

    Arena arena; // default growth, so threads race across several block swaps

    struct Region { char* base; size_t size; };
    std::vector<std::vector<Region>> regions(kThreads);
    std::vector<std::thread> threads;

    for (size_t t = 0; t < kThreads; ++t) {
        regions[t].reserve(kPerThread);
        threads.emplace_back([&arena, &regions, t]() {
            const auto stamp = static_cast<int>(t + 1);
            for (size_t i = 0; i < kPerThread; ++i) {
                // Vary sizes so threads contend at different alignments.
                const size_t bytes = 1 + ((i * 7 + t) % kMaxAllocation);
                char* p = arena.allocate(bytes);
                assert(p != nullptr);
                assert(isAligned(p));
                std::memset(p, stamp, bytes);
                regions[t].push_back({ p, bytes });
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    size_t total = 0;
    for (size_t t = 0; t < kThreads; ++t) {
        const auto stamp = static_cast<unsigned char>(t + 1);
        assert(regions[t].size() == kPerThread);
        for (const Region& region : regions[t]) {
            for (size_t b = 0; b < region.size; ++b) {
                // A mismatch here means two threads were handed overlapping
                // bytes by the bump path.
                assert(static_cast<unsigned char>(region.base[b]) == stamp);
            }
            total += region.size;
        }
    }

    assert(arena.bytesAllocated() == total);
}

} // namespace

int main() {
    testLazyMapping();
    testAlignmentAndAccounting();
    testZeroSized();
    testBlockGrowth();
    testOversizedAllocation();
    testConcurrentNonOverlap();
    return 0;
}
