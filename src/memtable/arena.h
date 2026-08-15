#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

inline constexpr size_t kArenaFirstBlockBytes = 16 * 1024;
inline constexpr size_t kArenaMaxBlockBytes   = 1u << 20; // 1 MB

// Every allocation is aligned to this. It's the alignment SkipList's node
// header needs for its uint64_t tag and its trailing array of Node*.
inline constexpr size_t kArenaAlignment = 8;

// A bump allocator over anonymous mmap'd blocks. Memory frees after flush.
//
// Callers must only store trivially destructible data here; the Arena never
// runs a destructor on anything it hands out.
//
// Thread-safety: allocate() is safe to call concurrently. The fast path is a
// lock-free CAS on the current block's bump offset; the mutex is taken only
// when a block is exhausted and a new one has to be mapped.
class Arena {
public:
    explicit Arena(size_t first_block_bytes = kArenaFirstBlockBytes);
    ~Arena();

    Arena(const Arena&)            = delete;
    Arena& operator=(const Arena&) = delete;

    // Returns kArenaAlignment-aligned storage for `bytes`. Never returns
    // null; throws std::bad_alloc if the underlying mmap fails. A zero-byte
    // request still yields a distinct, non-null pointer and updates the offset.
    char* allocate(size_t bytes);

    // Bytes handed out to callers.
    [[nodiscard]] size_t bytesAllocated() const noexcept {
        return _bytes_allocated.load(std::memory_order_relaxed);
    }

    // Bytes actually mapped from the OS: always a multiple of the page size,
    // and the real ceiling on this arena's resident footprint. Exceeds
    // bytesAllocated() by the unused tails of retired blocks.
    [[nodiscard]] size_t bytesReserved() const noexcept {
        return _bytes_reserved.load(std::memory_order_relaxed);
    }

private:
    struct Block {
        char*  base     = nullptr;
        size_t capacity = 0;
        std::atomic<size_t> offset{ 0 };
    };

    // Slow path: block exhausted (or none mapped yet). Takes _blocks_mutex.
    char* allocateFallback(size_t bytes);

    // Maps a new block of at least `capacity` bytes and records it for
    // teardown. Caller must hold _blocks_mutex.
    Block* addBlock(size_t capacity);

    // The block new allocations bump through. Null until the first allocate().
    std::atomic<Block*> _current{ nullptr };

    std::mutex _blocks_mutex;

    // Owns every block ever mapped, including retired ones and the dedicated
    // mappings handed to oversized requests. Guarded by _blocks_mutex.
    std::vector<std::unique_ptr<Block>> _blocks;

    // Size of the next block to map; doubles up to kArenaMaxBlockBytes.
    // Guarded by _blocks_mutex.
    size_t _next_block_bytes;

    std::atomic<size_t> _bytes_allocated{ 0 };
    std::atomic<size_t> _bytes_reserved{ 0 };
};
