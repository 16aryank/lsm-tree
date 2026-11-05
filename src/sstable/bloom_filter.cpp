#include "sstable/bloom_filter.h"

#include <algorithm>
#include <stdexcept>

#include "coding.h"

namespace {

constexpr uint64_t kFnvPrime = 1099511628211ULL;
constexpr uint64_t kFirstSeed = 0x9ae16a3b2f90404fULL;
constexpr uint64_t kSecondSeed = 0xc3a5c85c97cb3127ULL;
constexpr uint64_t kMinBits = 64;
constexpr uint64_t kBitsPerKey = 10;
constexpr uint8_t kMaxProbes = 30;

uint64_t hashWithSeed(std::string_view key, uint64_t seed) noexcept {
    uint64_t hash = seed;
    for (unsigned char byte : key) {
        hash = (hash ^ byte) * kFnvPrime;
    }
    // Diffuse the seeded FNV state, including for short and empty keys.
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    return hash ^ (hash >> 33);
}

uint64_t position(BloomHashes hashes, uint8_t probe, uint64_t bit_count) noexcept {
    return (hashes.first + static_cast<uint64_t>(probe) * hashes.second) % bit_count;
}

} // namespace

BloomHashes bloomHashes(std::string_view key) noexcept {
    return {hashWithSeed(key, kFirstSeed), hashWithSeed(key, kSecondSeed) | 1ULL};
}

BloomFilter BloomFilter::build(std::span<const BloomHashes> hashes) {
    BloomFilter result;
    if (hashes.empty()) {
        return result;
    }

    // Block handles store payload lengths in uint32. Check before multiplying,
    // rounding, or allocating the bit array.
    constexpr uint64_t max_bytes = UINT32_MAX - kBloomHeaderSize;
    if (hashes.size() > max_bytes * 8 / kBitsPerKey) {
        throw std::length_error("Bloom filter exceeds SSTable block size limit");
    }
    const uint64_t wanted_bits = static_cast<uint64_t>(hashes.size()) * kBitsPerKey;
    result._bit_count = ((std::max(wanted_bits, kMinBits) + 7) / 8) * 8;
    if (result._bit_count / 8 > max_bytes) {
        throw std::length_error("Bloom filter exceeds SSTable block size limit");
    }
    result._bits.assign(static_cast<size_t>(result._bit_count / 8), '\0');
    for (const BloomHashes hashes_for_key : hashes) {
        for (uint8_t i = 0; i < result._probes; ++i) {
            const uint64_t bit = position(hashes_for_key, i, result._bit_count);
            result._bits[bit / 8] = static_cast<char>(
                static_cast<unsigned char>(result._bits[bit / 8]) | (1U << (bit % 8)));
        }
    }
    return result;
}

std::optional<BloomFilter> BloomFilter::decode(std::string_view payload) {
    if (payload.size() < kBloomHeaderSize || payload.size() > UINT32_MAX ||
        static_cast<uint8_t>(payload[0]) != kBloomPayloadVersion) {
        return std::nullopt;
    }
    const uint8_t probes = static_cast<uint8_t>(payload[1]);
    const uint64_t bit_count = decodeFixed64(payload.data() + 2);
    if (probes == 0 || probes > kMaxProbes || (bit_count != 0 && bit_count < kMinBits) ||
        bit_count % 8 != 0 || bit_count / 8 != payload.size() - kBloomHeaderSize) {
        return std::nullopt;
    }
    BloomFilter result;
    result._probes = probes;
    result._bit_count = bit_count;
    result._bits.assign(payload.substr(kBloomHeaderSize));
    return result;
}

std::string BloomFilter::encode() const {
    std::string payload;
    payload.reserve(kBloomHeaderSize + _bits.size());
    payload.push_back(static_cast<char>(kBloomPayloadVersion));
    payload.push_back(static_cast<char>(_probes));
    putFixed64(payload, _bit_count);
    payload.append(_bits);
    return payload;
}

bool BloomFilter::mayContain(std::string_view key) const noexcept {
    if (_bit_count == 0) {
        return false;
    }
    const BloomHashes hashes = bloomHashes(key);
    for (uint8_t i = 0; i < _probes; ++i) {
        const uint64_t bit = position(hashes, i, _bit_count);
        if ((static_cast<unsigned char>(_bits[bit / 8]) & (1U << (bit % 8))) == 0) {
            return false;
        }
    }
    return true;
}

void BloomFilterBuilder::addUserKey(std::string_view key) {
    if (_has_last_key && key == _last_key) {
        return;
    }
    _last_key.assign(key);
    _has_last_key = true;
    _hashes.push_back(bloomHashes(key));
}
