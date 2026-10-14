#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

inline constexpr size_t kArenaFirstBlockBytes = 16 * 1024;
inline constexpr size_t kArenaMaxBlockBytes   = 1u << 20; // 1 MB

inline constexpr size_t kArenaAlignment = 8;

// Callers must only store trivially destructible data since the Arena never
// runs a destructor, only ::munmap
//
// Thread-safety: allocate() is safe to call concurrently. The fast path is a
// lock-free CAS on the current block's bump offset; the mutex is taken only
// when a block is exhausted and a new one has to be mapped.
class Arena {
public:
    explicit Arena(size_t first_block_bytes = kArenaFirstBlockBytes);
    ~Arena();

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

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
        char* base{ nullptr };
        size_t capacity{ 0 };
        std::atomic<size_t> offset{ 0 };
    };

    char* allocateFallback(size_t bytes);
    char* tryAllocate(Block* block, size_t bytes);
    Block* addBlock(size_t capacity);

    std::atomic<Block*> _current{ nullptr };
    std::mutex _blocks_mutex;
    std::vector<std::unique_ptr<Block>> _blocks;
    size_t _next_block_bytes;

    std::atomic<size_t> _bytes_allocated{ 0 };
    std::atomic<size_t> _bytes_reserved{ 0 };
};
