#include "src/sstable/bloom_filter.h"

#include <cassert>
#include <cstdio>
#include <string>

#include "src/coding.h"

namespace {

std::string keyAt(size_t i, char prefix) {
    char digits[24];
    std::snprintf(digits, sizeof(digits), "%08zu", i);
    return prefix + std::string(digits);
}

void rejects(std::string payload) {
    assert(!BloomFilter::decode(payload).has_value());
}

} // namespace

int main() {
    BloomFilterBuilder empty_builder;
    const BloomFilter empty = empty_builder.finish();
    assert(empty_builder.distinctKeys() == 0);
    assert(empty.bitCount() == 0);
    assert(empty.probeCount() == kBloomProbes);
    assert(!empty.mayContain(""));
    assert(!empty.mayContain("missing"));
    assert(empty.encode().size() == kBloomHeaderSize);
    const auto empty_copy = BloomFilter::decode(empty.encode());
    assert(empty_copy && !empty_copy->mayContain("anything"));

    BloomFilterBuilder tiny_builder;
    const std::string binary_key("a\0\xff", 3);
    tiny_builder.addUserKey(binary_key);
    tiny_builder.addUserKey(binary_key); // two sequence versions of one user key
    assert(tiny_builder.distinctKeys() == 1);
    const BloomFilter tiny = tiny_builder.finish();
    assert(tiny.bitCount() == 64);
    assert(tiny.mayContain(binary_key));
    assert(BloomFilter::decode(tiny.encode())->mayContain(binary_key));

    BloomFilterBuilder once;
    once.addUserKey(binary_key);
    assert(once.finish().encode() == tiny.encode());

    // The encoded bytes pin the seeds, double-hash step, and low-bit-first
    // numbering. Mutating any one of those changes this fixed fixture.
    std::string expected_tiny("\x01\x07", 2);
    putFixed64(expected_tiny, 64);
    expected_tiny.append("\x00\x00\x00\x00\x00\x80\x3f\x00", 8);
    assert(tiny.encode() == expected_tiny);

    BloomFilterBuilder builder;
    for (size_t i = 0; i < 2000; ++i) {
        builder.addUserKey(keyAt(i, 'p'));
        builder.addUserKey(keyAt(i, 'p'));
    }
    builder.addUserKey(std::string("\x80\0\xff", 3));
    const BloomFilter filter = builder.finish();
    assert(builder.distinctKeys() == 2001);
    assert(filter.bitCount() == 20016);
    assert(filter.probeCount() == kBloomProbes);
    assert(filter.encode() == builder.finish().encode());
    const auto decoded = BloomFilter::decode(filter.encode());
    assert(decoded && decoded->encode() == filter.encode());
    for (size_t i = 0; i < 2000; ++i) {
        assert(decoded->mayContain(keyAt(i, 'p')));
    }
    assert(decoded->mayContain(std::string("\x80\0\xff", 3)));

    size_t false_positives = 0;
    for (size_t i = 0; i < 10000; ++i) {
        false_positives += decoded->mayContain(keyAt(i, 'q'));
    }
    std::printf("Bloom primitive: %zu false positives / 10000 absent keys\n", false_positives);
    assert(false_positives <= 300); // catches a broken distribution

    std::string payload = tiny.encode();
    rejects("");
    rejects(payload.substr(0, kBloomHeaderSize - 1));
    rejects(payload.substr(0, payload.size() - 1));
    rejects(payload + "x");
    payload[0] = 2;
    rejects(payload); // unknown version
    payload = tiny.encode();
    payload[1] = 0;
    rejects(payload); // zero probes
    payload[1] = 31;
    rejects(payload); // unreasonable probe count
    payload = tiny.encode();
    payload[2] = 63;
    rejects(payload); // bit count not byte aligned
    payload[2] = 56;
    rejects(payload); // nonempty filter smaller than minimum
    payload = tiny.encode();
    payload[2] = 0;
    rejects(payload); // empty bit count with a nonempty bit array
    payload = empty.encode();
    payload[2] = 64;
    rejects(payload); // bit count requires absent bytes
}
