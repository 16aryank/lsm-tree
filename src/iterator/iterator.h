#pragma once
#include <cstdint>
#include <string_view>

// Every implementation yields entries in internal-key order: user key
// ascending, tag descending, so the newest version of a key comes first. 
//
// key() and value() point into storage the iterator owns and are invalidated
// by next().
//
// seek() is deliberately absent. Compaction scans its inputs whole, and adding
// a positioning operation would mean implementing it for every source before
// anything could use it.
class InternalIterator {
public:
    virtual ~InternalIterator() = default;

    InternalIterator(const InternalIterator&) = delete;
    InternalIterator& operator=(const InternalIterator&) = delete;

    [[nodiscard]] virtual bool valid() const noexcept = 0;
    virtual void next() = 0;

    [[nodiscard]] virtual std::string_view key() const noexcept = 0;
    [[nodiscard]] virtual uint64_t tag() const noexcept = 0;

    [[nodiscard]] virtual std::string_view value() const noexcept = 0;

protected:
    InternalIterator() = default;
};
