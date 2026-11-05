#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// File-wide filter payload: version uint8 | probes uint8 | bit_count uint64
// (little endian) | bit array. Bit zero is the low bit of the first byte.
// The enclosing SSTable block supplies the checksum.
inline constexpr uint8_t kBloomPayloadVersion = 1;
inline constexpr uint8_t kBloomProbes = 7;
inline constexpr size_t kBloomHeaderSize = 10;

struct BloomHashes {
    uint64_t first;
    uint64_t second;
};

[[nodiscard]] BloomHashes bloomHashes(std::string_view key) noexcept;

class BloomFilter {
public:
    [[nodiscard]] static BloomFilter build(std::span<const BloomHashes> hashes);
    [[nodiscard]] static std::optional<BloomFilter> decode(std::string_view payload);

    [[nodiscard]] std::string encode() const;
    [[nodiscard]] bool mayContain(std::string_view key) const noexcept;
    [[nodiscard]] uint64_t bitCount() const noexcept { return _bit_count; }
    [[nodiscard]] uint8_t probeCount() const noexcept { return _probes; }

private:
    uint64_t _bit_count = 0;
    uint8_t _probes = kBloomProbes;
    std::string _bits;
};

// SSTableBuilder streams entries in internal-key order. Consecutive versions
// of one user key contribute only one hash pair to the filter.
class BloomFilterBuilder {
public:
    void addUserKey(std::string_view key);
    [[nodiscard]] size_t distinctKeys() const noexcept { return _hashes.size(); }
    [[nodiscard]] BloomFilter finish() const { return BloomFilter::build(_hashes); }

private:
    bool _has_last_key = false;
    std::string _last_key;
    std::vector<BloomHashes> _hashes;
};
