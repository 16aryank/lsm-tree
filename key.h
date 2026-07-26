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
