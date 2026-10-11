#pragma once
#include <cstdint>
#include <string_view>

// Every implementation yields entries in internal-key order: user key
// ascending, tag descending, so the newest version of a key comes first. 
//
// key() and value() point into storage the iterator owns and are invalidated
// by next() and by seek().
//
// A new iterator starts at its first entry.
class InternalIterator {
public:
    virtual ~InternalIterator() = default;

    InternalIterator(const InternalIterator&) = delete;
    InternalIterator& operator=(const InternalIterator&) = delete;

    [[nodiscard]] virtual bool valid() const noexcept = 0;
    virtual void next() = 0;

    // Positions at the first entry at or after (key, tag) in internal-key
    // order, leaving the iterator invalid if there is none. Note that tags
    // sort *descending* within a user key, so seeking (key, packSeqAndType(seq,
    // kValue)) lands on the newest version of `key` visible at `seq`.
    virtual void seek(std::string_view key, uint64_t tag) = 0;

    // Back to the first entry, the position a fresh iterator starts at.
    virtual void seekToFirst() = 0;

    [[nodiscard]] virtual std::string_view key() const noexcept = 0;
    [[nodiscard]] virtual uint64_t tag() const noexcept = 0;

    [[nodiscard]] virtual std::string_view value() const noexcept = 0;

protected:
    InternalIterator() = default;
};
