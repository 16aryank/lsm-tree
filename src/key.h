#pragma once
#include <cstdint>
#include <bit>

// Stored in the low 8 bits of the packed seqnum_type uint64_t.
// kDelete < kValue so deletions sort before insertions at equal seqnum.
enum class ValueType : uint8_t {
    kDelete = 0,
    kValue  = 1,
};

template <typename T>
struct Key {
    T        _user_key;
    uint64_t _seqnum_type; // bits 63:8 = sequence number, bits 7:0 = ValueType
};

// Packs a sequence number and value type into the combined field stored in
// Key<T>::_seqnum_type.
inline constexpr uint64_t packSeqAndType(uint64_t seq, ValueType type) noexcept {
    return (seq << 8) | static_cast<uint8_t>(type);
}

inline constexpr uint64_t unpackSeqNumber(uint64_t seqnum_type) noexcept {
    return seqnum_type >> 8;
}

inline constexpr ValueType unpackValueType(uint64_t seqnum_type) noexcept {
    return static_cast<ValueType>(seqnum_type & 0xFF);
}
