// Head-to-head benchmark: the arena-backed MemTable against the pre-arena one
// kept in src/deprecated/.
//
// Measures, for each implementation:
//   - insert throughput
//   - point-lookup throughput
//   - full-scan (flush-path) throughput
//   - resident bytes per entry, and what the table itself reports
//
// Run it with its own target, which pins -O2 so the numbers stay meaningful:
//     make memtable_benchmark                  # default entry count
//     make memtable_benchmark ARGS=1000000     # override the entry count
//
// It is deliberately excluded from `make` and `make test`.
//
// This translation unit exists to exercise the deprecated implementation, so
// the deprecation warnings it would otherwise emit on every call are turned
// off for the whole file.

// ------Results-------------------------------------------------------------
// Head-to-head at 1M entries (-O2)

//                          insert s     insert/s      ns/op     lookup/s      ns/op       scan/s      ns/op
// arena                       0.901      1109445      901.4       584965     1709.5     13134333       76.1
// deprecated (pre-arena)      1.826       547618     1826.1       298135     3354.2      7103894      140.8
// arena speedup                            2.03x                   1.96x                   1.85x

//                           RSS delta    B/entry   overhead     reported    B/entry       amp
// arena                     192315392      192.3       42.3    189981832      190.0     1.28x
// deprecated (pre-arena)    382025728      382.0      232.0    254000000      254.0     2.55x
// arena reduction               1.99x

// These tests also surfaced a bug in the old implementation:
// The pre-arena skiplist cannot be destroyed past ~131k entries
// It segfaults because _head owns the level-0 chain through nested shared_ptrs, 
// so ~SkipList recurses once per node and overflows the 8 MB main-thread stack. 

// Tests compare against deprecated version
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

// One 4 MB memtable holds roughly 22k entries at this shape, so the default is
// about a dozen memtables' worth of sustained ingest -- enough for the timing
// to be dominated by the data structure rather than by start-up noise.
constexpr size_t kDefaultEntries = 250000;
constexpr size_t kKeyBytes       = 50;
constexpr size_t kValueBytes     = 100;

// Both implementations use the same skip-list height cap (12), so the
// comparison is apples-to-apples even where that cap is limiting.
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

// Runs the same workload against whichever MemTable type is passed in.
// `Table` differs between the two, and so do the iterator's accessors, so the
// scan is handed in as a lambda.
//
// kLeak exists because the deprecated skip list owns its nodes through a
// shared_ptr chain, so destroying it recurses once per node and overflows the
// 8 MB main-thread stack at roughly 131k entries. Deliberately leaking it is
// the only way to benchmark it at a realistic size; the process is about to
// exit anyway.
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
            auto* it = table.newIterator();
            for (; it->valid(); it->next()) {
                bytes += it->value().size();
                ++count;
            }
            delete it;
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
