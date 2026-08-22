// Promotion (mutable -> immutable), background-flush handoff, mutable ->
// immutable -> SSTable read ordering, and recovery from a failed flush for
// MemTableManager.
#include "src/memtable/memtable_manager.h"
#include <atomic>
#include <cassert>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::filesystem::path testDir() {
    return std::filesystem::temp_directory_path() /
           ("lsm-manager-test-" + std::to_string(::getpid()));
}

} // namespace

int main() {
    // Without an sstable directory the manager is memory-only: a promoted
    // table is flushed nowhere and simply dropped.
    {
        MemTableManager<std::string, std::string> mgr(256);

        mgr.add(1, ValueType::kValue, "a", "small");
        assert(!mgr.hasPendingFlush());
        assert(mgr.get("a", 10) == "small");
        assert(!mgr.get("missing", 10).has_value());

        // Push the mutable table over its threshold; this promotes it to
        // immutable and kicks off a background flush.
        mgr.add(2, ValueType::kValue, "b", std::string(512, 'x'));

        // Deterministically wait for the background flush instead of racing it.
        mgr.waitForPendingFlush();
        assert(!mgr.hasPendingFlush());
        assert(mgr.lastFlushError().empty());
        assert(mgr.sstables().empty());

        // Nothing persisted the promoted table, so its keys are gone.
        assert(!mgr.get("a", 10).has_value());

        // Writes after promotion land in the fresh mutable table.
        mgr.add(3, ValueType::kValue, "c", "fresh");
        assert(mgr.get("c", 10) == "fresh");
    }

    const std::filesystem::path dir = testDir();
    std::filesystem::remove_all(dir);

    // With a directory, a promotion flushes to a Level-0 SSTable and reads
    // fall through to it once the memtable is gone.
    {
        // 512-byte threshold against 512-byte values: three small entries can
        // never reach it whatever heights the skip list rolls for them, and
        // the fourth always clears it. Keeps which entries land in which table
        // deterministic.
        MemTableManager<std::string, std::string> mgr(512, nullptr, dir);
        assert(std::filesystem::exists(dir));

        mgr.add(1, ValueType::kValue, "a", "a-v1");
        mgr.add(2, ValueType::kValue, "gone", "doomed");
        mgr.add(3, ValueType::kDelete, "gone", "");
        mgr.add(4, ValueType::kValue, "big", std::string(512, 'x'));

        mgr.waitForPendingFlush();
        assert(!mgr.hasPendingFlush());
        assert(mgr.lastFlushError().empty());
        assert(mgr.sstables().size() == 1);

        auto table = mgr.sstables().front();
        assert(std::filesystem::exists(table->path()));
        assert(table->numEntries() == 4);
        assert(table->smallestKey() == "a");
        assert(table->largestKey() == "gone");

        // Served from the SSTable now, not from any memtable.
        assert(mgr.get("a", 10) == "a-v1");
        assert(mgr.get("big", 10) == std::string(512, 'x'));

        // A tombstone in the table reads as absent, and an older snapshot
        // still sees the value it covers.
        assert(!mgr.get("gone", 10).has_value());
        assert(mgr.get("gone", 2) == "doomed");
        assert(!mgr.get("missing", 10).has_value());

        // A second flush stacks another Level-0 table, and the newer one wins
        // for keys both of them hold.
        mgr.add(5, ValueType::kValue, "a", "a-v5");
        mgr.add(6, ValueType::kValue, "big", std::string(512, 'y'));
        mgr.waitForPendingFlush();
        assert(mgr.sstables().size() == 2);

        assert(mgr.get("a", 10) == "a-v5");
        assert(mgr.get("a", 4) == "a-v1"); // older snapshot, older table
        assert(mgr.get("big", 10) == std::string(512, 'y'));
    }

    // A flush that can't write keeps its data in memory and retries later,
    // rather than dropping the memtable on the floor.
    //
    // Skipped for root, which writes through the permission bits this relies
    // on.
    if (::geteuid() != 0) {
        const std::filesystem::path locked = dir / "locked";
        std::filesystem::create_directories(locked);

        MemTableManager<std::string, std::string> mgr(256, nullptr, locked);
        mgr.add(1, ValueType::kValue, "keep", "me");

        // Read-only directory: creating the .sst file fails.
        std::filesystem::permissions(locked, std::filesystem::perms::owner_read |
                                                 std::filesystem::perms::owner_exec);

        mgr.add(2, ValueType::kValue, "big", std::string(512, 'x'));
        mgr.waitForPendingFlush();

        // The attempt failed, so the immutable table is still pending and the
        // error is visible -- but every key is still readable.
        assert(mgr.hasPendingFlush());
        assert(!mgr.lastFlushError().empty());
        assert(mgr.sstables().empty());
        assert(mgr.get("keep", 10) == "me");
        assert(mgr.get("big", 10) == std::string(512, 'x'));

        // The next write over the threshold retries the same table.
        std::filesystem::permissions(locked, std::filesystem::perms::owner_all);
        mgr.add(3, ValueType::kValue, "later", std::string(400, 'z'));
        mgr.waitForPendingFlush();

        assert(!mgr.hasPendingFlush());
        assert(mgr.lastFlushError().empty());
        assert(mgr.sstables().size() == 1);
        assert(mgr.get("keep", 10) == "me");
        assert(mgr.get("later", 10) == std::string(400, 'z'));
    }

    // Concurrent writers drive promotion repeatedly while a reader races them.
    // Nothing may be lost on the way to disk: every key written is still
    // readable at the end, whether it landed in the mutable table, the one
    // pending flush, or an SSTable.
    {
        constexpr int kThreads   = 4;
        constexpr int kPerThread = 200;
        constexpr uint64_t kReadSeq = kThreads * kPerThread + 10;

        const auto keyFor = [](int thread, int i) {
            return "t" + std::to_string(thread) + "-k" + std::to_string(i);
        };

        MemTableManager<std::string, std::string> mgr(4096, nullptr, dir / "concurrent");
        std::atomic<uint64_t> next_seq{ 1 };
        std::atomic<bool> writing{ true };

        std::vector<std::thread> writers;
        for (int t = 0; t < kThreads; ++t) {
            writers.emplace_back([&, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    mgr.add(next_seq.fetch_add(1), ValueType::kValue, keyFor(t, i),
                            std::string(64, static_cast<char>('a' + t)));
                }
            });
        }
        std::thread reader([&] {
            while (writing.load()) {
                (void)mgr.get(keyFor(0, 0), kReadSeq);
                (void)mgr.get("absent", kReadSeq);
            }
        });

        for (auto& writer : writers) {
            writer.join();
        }
        writing.store(false);
        reader.join();
        mgr.waitForPendingFlush();

        assert(mgr.lastFlushError().empty());
        for (int t = 0; t < kThreads; ++t) {
            for (int i = 0; i < kPerThread; ++i) {
                auto value = mgr.get(keyFor(t, i), kReadSeq);
                assert(value.has_value());
                assert(*value == std::string(64, static_cast<char>('a' + t)));
            }
        }
    }

    std::filesystem::remove_all(dir);
    return 0;
}
