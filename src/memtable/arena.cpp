#include "memtable/arena.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <limits>
#include <new>

namespace {

size_t pageSize() {
    static const size_t kPageSize = []() -> size_t {
        const long v = ::sysconf(_SC_PAGESIZE);
        // 16 KB on Apple Silicon, 4 KB on x86_64.
        return v > 0 ? static_cast<size_t>(v) : 4096;
    }();
    return kPageSize;
}

// Rounds `n` up to a multiple of nonzero `to` without wrapping size_t.
size_t roundUp(size_t n, size_t to) {
    const size_t remainder = n % to;
    if (remainder == 0) {
        return n;
    }
    const size_t padding = to - remainder;
    if (n > std::numeric_limits<size_t>::max() - padding) {
        throw std::bad_alloc();
    }
    return n + padding;
}

} // namespace

Arena::Arena(size_t first_block_bytes)
    : _next_block_bytes(std::max(first_block_bytes, pageSize())) {
    // Blocks are mapped lazily, so an arena that is never written to costs
    // nothing beyond this object.
}

Arena::~Arena() {
    // Single-threaded teardown: no other thread may hold a pointer into this
    // arena by the time it is destroyed.
    for (const std::unique_ptr<Block>& block : _blocks) {
        ::munmap(block->base, block->capacity);
    }
}

char* Arena::allocate(size_t bytes) {
    // Hand out a distinct pointer even for an empty request, so callers can
    // compare returned addresses without aliasing surprises.
    if (bytes == 0) {
        bytes = 1;
    }

    if (char* result = tryAllocate(_current.load(std::memory_order_acquire), bytes)) {
        return result;
    }

    return allocateFallback(bytes);
}

char* Arena::allocateFallback(size_t bytes) {
    std::lock_guard<std::mutex> lock(_blocks_mutex);

    // Another thread may have mapped a fresh block while we waited for the
    // lock, so retry the fast path before mapping one of our own.
    if (char* result = tryAllocate(_current.load(std::memory_order_acquire), bytes)) {
        return result;
    }

    // A request large relative to the block size gets its own right-sized
    // mapping. It deliberately does not become _current: the block everyone
    // else is bumping through keeps whatever room it has left, instead of
    // being retired with a large unused tail.
    if (bytes > _next_block_bytes / 4) {
        Block* dedicated = addBlock(bytes);
        dedicated->offset.store(bytes, std::memory_order_relaxed);
        _bytes_allocated.fetch_add(bytes, std::memory_order_relaxed);
        return dedicated->base;
    }

    Block* fresh = addBlock(_next_block_bytes);
    _next_block_bytes = _next_block_bytes >= kArenaMaxBlockBytes / 2
                            ? kArenaMaxBlockBytes
                            : _next_block_bytes * 2;

    // Take our slice before publishing, so the block is never visible to
    // another thread in a state where this allocation hasn't been reserved.
    fresh->offset.store(bytes, std::memory_order_relaxed);
    _current.store(fresh, std::memory_order_release);
    _bytes_allocated.fetch_add(bytes, std::memory_order_relaxed);
    return fresh->base;
}

char* Arena::tryAllocate(Block* block, size_t bytes) {
    if (block == nullptr) {
        return nullptr;
    }

    size_t offset = block->offset.load(std::memory_order_relaxed);
    for (;;) {
        const size_t aligned = roundUp(offset, kArenaAlignment);
        // Recheck after a failed CAS: another thread may have filled the block.
        // Subtraction avoids overflowing when the requested size is large.
        if (aligned > block->capacity || bytes > block->capacity - aligned) {
            return nullptr;
        }
        if (block->offset.compare_exchange_weak(offset, aligned + bytes,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
            _bytes_allocated.fetch_add(bytes, std::memory_order_relaxed);
            return block->base + aligned;
        }
    }
}

Arena::Block* Arena::addBlock(size_t capacity) {
    const size_t mapped = roundUp(capacity, pageSize());

    void* address = ::mmap(nullptr, mapped, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (address == MAP_FAILED) {
        throw std::bad_alloc();
    }

    try {
        auto block = std::make_unique<Block>();
        block->base = static_cast<char*>(address);
        block->capacity = mapped;

        Block* raw = block.get();
        _blocks.push_back(std::move(block));
        _bytes_reserved.fetch_add(mapped, std::memory_order_relaxed);
        return raw;
    } catch (...) {
        ::munmap(address, mapped);
        throw;
    }
}
