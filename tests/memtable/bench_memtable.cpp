// Compares arena and legacy memtable throughput and resident memory.
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include "src/deprecated/memtable.h"
#include "src/memtable/memtable.h"

#include <mach/mach.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

// Enough entries to measure sustained ingest beyond startup costs.
constexpr size_t kDefaultEntries = 250000;
constexpr size_t kKeyBytes       = 50;
constexpr size_t kValueBytes     = 100;

using Clock   = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

size_t residentBytes() {
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    const kern_return_t rc = task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                                       reinterpret_cast<task_info_t>(&info), &count);
    return rc == KERN_SUCCESS ? static_cast<size_t>(info.resident_size) : 0;
}

// Scatters sequential indices so inserts don't arrive in sorted order, which
// would be an unrealistically friendly traversal pattern.
uint64_t scatter(size_t i) {
    return static_cast<uint64_t>(i) * 0x9E3779B97F4A7C15ull;
}

// Overwrites the leading 16 bytes of `buf` with the hex digits of `v`, leaving
// the rest of the (already sized) buffer as filler. Identical work for both
// implementations, so it cancels out of the comparison.
void stampHex(std::string& buf, uint64_t v) {
    static constexpr char kDigits[] = "0123456789abcdef";
    for (int d = 0; d < 16; ++d) {
        buf[15 - static_cast<size_t>(d)] = kDigits[(v >> (4 * d)) & 0xF];
    }
}

struct Result {
    const char* name           = "";
    double insert_seconds      = 0;
    double lookup_seconds      = 0;
    double scan_seconds        = 0;
    size_t rss_delta           = 0;
    size_t reported_bytes      = 0;
    size_t entries             = 0;
    size_t lookups             = 0;
    size_t scanned             = 0;
    uint64_t checksum          = 0; // keeps the lookup loop from being optimized away
};

// kLeak avoids recursive destruction of the legacy shared_ptr node chain.
template <typename Table, bool kLeak, typename ScanFn>
Result runWorkload(const char* name, size_t entries, ScanFn scan) {
    Result result;
    result.name    = name;
    result.entries = entries;

    std::string key(kKeyBytes, 'k');
    std::string value(kValueBytes, 'v');
    stampHex(key, 0);
    stampHex(value, 0);

    // SIZE_MAX threshold: this measures footprint and speed, not flush policy.
    Table* table = new Table(SIZE_MAX);

    const size_t rss_before = residentBytes();

    const auto insert_start = Clock::now();
    for (size_t i = 0; i < entries; ++i) {
        const uint64_t h = scatter(i);
        stampHex(key, h);
        stampHex(value, h);
        table->add(static_cast<uint64_t>(i) + 1, ValueType::kValue, key, value);
    }
    result.insert_seconds = Seconds(Clock::now() - insert_start).count();

    const size_t rss_after = residentBytes();
    result.rss_delta       = rss_after > rss_before ? rss_after - rss_before : 0;
    result.reported_bytes  = table->approximateMemoryUsage();

    // Point lookups over a strided subset, so the probes jump around the
    // structure instead of walking it in order.
    const size_t lookups = entries < 100000 ? entries : 100000;
    const size_t stride  = entries / lookups;
    result.lookups       = lookups;

    const auto lookup_start = Clock::now();
    for (size_t n = 0; n < lookups; ++n) {
        const size_t i = n * stride;
        stampHex(key, scatter(i));
        if (auto found = table->get(key, entries)) {
            result.checksum += found->size();
        }
    }
    result.lookup_seconds = Seconds(Clock::now() - lookup_start).count();

    // Full ordered scan -- what a flush to an SSTable would do.
    uint64_t scanned_bytes = 0;
    const auto scan_start  = Clock::now();
    result.scanned         = scan(*table, scanned_bytes);
    result.scan_seconds    = Seconds(Clock::now() - scan_start).count();
    result.checksum += scanned_bytes;

    if constexpr (!kLeak) {
        delete table;
    }
    return result;
}

void printResult(const Result& r) {
    const double insert_ops = r.entries / r.insert_seconds;
    const double lookup_ops = r.lookups / r.lookup_seconds;
    const double scan_ops   = r.scanned / r.scan_seconds;

    std::printf("%-22s %10.3f %12.0f %10.1f %12.0f %10.1f %12.0f %10.1f\n",
                r.name,
                r.insert_seconds,
                insert_ops,
                1e9 / insert_ops,
                lookup_ops,
                1e9 / lookup_ops,
                scan_ops,
                1e9 / scan_ops);
}

void printMemory(const Result& r) {
    const double rss_per      = static_cast<double>(r.rss_delta) / r.entries;
    const double reported_per = static_cast<double>(r.reported_bytes) / r.entries;
    const double payload      = kKeyBytes + kValueBytes;

    std::printf("%-22s %12zu %10.1f %10.1f %12zu %10.1f %8.2fx\n",
                r.name,
                r.rss_delta,
                rss_per,
                rss_per - payload,
                r.reported_bytes,
                reported_per,
                rss_per / payload);
}

} // namespace

int main(int argc, char** argv) {
    size_t entries = kDefaultEntries;
    if (argc > 1) {
        const long long parsed = std::atoll(argv[1]);
        if (parsed > 0) {
            entries = static_cast<size_t>(parsed);
        }
    }

    std::printf("entries=%zu  key=%zuB  value=%zuB  payload=%zuB/entry\n",
                entries, kKeyBytes, kValueBytes, kKeyBytes + kValueBytes);
    if (entries > 131000) {
        std::printf("note: the deprecated table is intentionally leaked -- its shared_ptr node\n"
                    "      chain destructs recursively and overflows the stack past ~131k entries\n");
    }
    std::printf("\n");

    // Arena first: it releases its memory with munmap on destruction, so the
    // deprecated run that follows starts from a clean resident baseline.
    const Result arena = runWorkload<MemTable<std::string, std::string>, false>(
        "arena", entries,
        [](auto& table, uint64_t& bytes) {
            size_t count = 0;
            auto it = table.cursor();
            for (; it.valid(); it.next()) {
                bytes += it.value().size();
                ++count;
            }
            return count;
        });

    const Result legacy = runWorkload<deprecated::MemTable<std::string, std::string>, true>(
        "deprecated (pre-arena)", entries,
        [](auto& table, uint64_t& bytes) {
            size_t count = 0;
            auto* it = table.newIterator();
            for (; it->valid(); it->next()) {
                bytes += it->value().size();
                ++count;
            }
            delete it;
            return count;
        });

    std::printf("TIME\n");
    std::printf("%-22s %10s %12s %10s %12s %10s %12s %10s\n",
                "", "insert s", "insert/s", "ns/op", "lookup/s", "ns/op", "scan/s", "ns/op");
    printResult(arena);
    printResult(legacy);
    std::printf("%-22s %10s %11.2fx %10s %11.2fx %10s %11.2fx\n",
                "arena speedup", "",
                legacy.insert_seconds / arena.insert_seconds, "",
                legacy.lookup_seconds / arena.lookup_seconds, "",
                legacy.scan_seconds / arena.scan_seconds);

    std::printf("\nMEMORY\n");
    std::printf("%-22s %12s %10s %10s %12s %10s %9s\n",
                "", "RSS delta", "B/entry", "overhead", "reported", "B/entry", "amp");
    printMemory(arena);
    printMemory(legacy);
    std::printf("%-22s %11.2fx\n", "arena reduction",
                static_cast<double>(legacy.rss_delta) / static_cast<double>(arena.rss_delta));

    // Both implementations must have seen the same data.
    if (arena.scanned != legacy.scanned || arena.checksum != legacy.checksum) {
        std::printf("\nWARNING: workloads diverged (scan %zu vs %zu, checksum %llu vs %llu)\n",
                    arena.scanned, legacy.scanned,
                    static_cast<unsigned long long>(arena.checksum),
                    static_cast<unsigned long long>(legacy.checksum));
        return 1;
    }
    return 0;
}
