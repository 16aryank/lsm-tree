// The in-memory write path: promotion (mutable -> immutable), the log rotation
// that goes with it, and the three-valued lookup that lets a caller tell a
// tombstone from a miss.
//
// MemTableManager no longer writes SSTables or knows what a level is -- a flush
// is DB's job, and the end-to-end behaviour lives in tests/db/db.cpp. What is
// tested here is the handoff: promote() freezes a table and hands it back, and
// nothing is lost or double-frozen while that is in flight.
#include "src/memtable/memtable_manager.h"
#include "src/wal/wal_codec.h"
#include "src/wal/wal_reader.h"
#include <atomic>
#include <cassert>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace {

using Manager = MemTableManager<std::string, std::string>;

std::filesystem::path testDir() {
    return std::filesystem::temp_directory_path() /
           ("lsm-manager-test-" + std::to_string(::getpid()));
}

// Flattens lookup() for assertions: "." absent, "x" tombstone, else the value.
std::string lookup(const Manager& manager, const std::string& key, uint64_t seq) {
    std::string value;
    switch (manager.lookup(key, seq, value)) {
    case LookupStatus::kFound:    return value;
    case LookupStatus::kDeleted:  return "x";
    case LookupStatus::kNotFound: return ".";
    }
    return ".";
}

// Opens a log under `dir`, numbering them in creation order. WalWriter does
// not own its fd, so the caller keeps them open and closes them at the end.
struct LogSet {
    std::filesystem::path              dir;
    std::vector<int>                   fds;
    std::vector<std::filesystem::path> paths;

    WalHandle create() {
        const auto path = dir / (std::to_string(paths.size()) + ".log");
        const int  fd   = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        assert(fd >= 0);
        fds.push_back(fd);
        paths.push_back(path);
        return { paths.size(), std::make_shared<WalWriter>(fd) };
    }

    void closeAll() {
        for (const int fd : fds) { ::close(fd); }
        fds.clear();
    }
};

} // namespace

int main() {
    const std::filesystem::path dir = testDir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    // A miss and a tombstone are different answers. Collapsing them is what
    // would let a deleted key come back from a table below the memtable.
    {
        Manager manager(4096);

        manager.add(1, ValueType::kValue, "a", "one");
        manager.add(2, ValueType::kValue, "gone", "doomed");
        manager.add(3, ValueType::kDelete, "gone", "");

        assert(lookup(manager, "a", 10) == "one");
        assert(lookup(manager, "gone", 10) == "x");    // tombstone, stop here
        assert(lookup(manager, "gone", 2) == "doomed"); // older snapshot
        assert(lookup(manager, "missing", 10) == ".");  // keep looking below
    }

    // Promotion freezes the mutable table and hands it back. Until the caller
    // retires it, its keys are still served -- there is no window where a
    // written key is in neither table.
    {
        Manager manager(512);
        manager.add(1, ValueType::kValue, "small", "v");

        // Under threshold: nothing to do, and no table handed out.
        assert(!manager.shouldPromote());
        assert(!manager.promote().table);
        assert(!manager.hasPendingFlush());

        manager.add(2, ValueType::kValue, "big", std::string(512, 'x'));
        assert(manager.shouldPromote());

        const auto promotion = manager.promote();
        assert(promotion.table != nullptr);
        assert(manager.hasPendingFlush());

        // Both keys are still readable through the frozen table.
        assert(lookup(manager, "small", 10) == "v");
        assert(lookup(manager, "big", 10) == std::string(512, 'x'));

        // A second promote while the first is in flight must not freeze
        // another table: writes may not stack up more than one.
        assert(!manager.promote().table);

        // Writes after the freeze land in the fresh mutable table.
        manager.add(3, ValueType::kValue, "after", "fresh");
        assert(lookup(manager, "after", 10) == "fresh");

        // Retiring the frozen table drops its keys -- which is only correct
        // because the caller has by then installed them somewhere else.
        manager.dropImmutable();
        assert(!manager.hasPendingFlush());
        assert(lookup(manager, "small", 10) == ".");
        assert(lookup(manager, "after", 10) == "fresh");
    }

    // A flush that failed leaves its table in place; the next promote() hands
    // back the same one rather than freezing a second.
    {
        Manager manager(512);
        manager.add(1, ValueType::kValue, "keep", "me");
        manager.add(2, ValueType::kValue, "big", std::string(512, 'x'));

        const auto first = manager.promote();
        assert(first.table != nullptr);

        manager.abandonFlush(); // the flush failed
        assert(manager.hasPendingFlush());

        const auto retry = manager.promote();
        assert(retry.table == first.table); // the same table, not a new one
        assert(lookup(manager, "keep", 10) == "me");

        manager.dropImmutable();
        assert(!manager.hasPendingFlush());
    }

    // Every write reaches the log before the memtable, and a promotion rotates
    // onto a fresh one. Replaying the logs in order reproduces exactly the
    // writes that were issued -- which is what recovery depends on.
    {
        LogSet logs{ dir / "rotate", { }, { } };
        std::filesystem::create_directories(logs.dir);

        struct Write {
            uint64_t    seq;
            ValueType   type;
            std::string key;
            std::string value;
        };
        const std::vector<Write> writes = {
            { 1, ValueType::kValue, "alpha", "one" },
            { 2, ValueType::kValue, "beta", "two" },
            { 3, ValueType::kDelete, "alpha", "" },
            // Clears the 512-byte threshold and triggers promotion.
            { 4, ValueType::kValue, "bulk", std::string(512, 'x') },
            { 5, ValueType::kValue, "gamma", "three" },
        };

        {
            Manager manager(512, [&] { return logs.create(); });
            const uint64_t first_log = manager.currentWal().number;

            for (const auto& w : writes) {
                manager.add(w.seq, w.type, w.key, w.value);
                if (manager.shouldPromote()) {
                    const auto promotion = manager.promote();
                    if (promotion.table) {
                        // The log now taking writes is the one a manifest edit
                        // would record: everything frozen is numbered below it.
                        assert(promotion.log_number > first_log);
                        manager.dropImmutable();
                    }
                }
            }
            assert(manager.currentWal().number > first_log);
        }

        assert(logs.paths.size() == 2); // rotated exactly once
        logs.closeAll();

        std::vector<Write> replayed;
        for (const auto& path : logs.paths) {
            const int fd = ::open(path.c_str(), O_RDONLY);
            assert(fd >= 0);
            WalReader reader(fd);
            while (const auto record = reader.readRecord()) {
                WalEntryView entry;
                assert(decodeWalEntry(byteView(*record), entry));
                replayed.push_back({ entry.sequenceNumber(), entry.valueType(),
                                     std::string(entry.key), std::string(entry.value) });
            }
            assert(!reader.stoppedEarly());
            ::close(fd);
        }

        assert(replayed.size() == writes.size());
        for (size_t i = 0; i < writes.size(); ++i) {
            assert(replayed[i].seq == writes[i].seq);
            assert(replayed[i].type == writes[i].type);
            assert(replayed[i].key == writes[i].key);
            assert(replayed[i].value == writes[i].value);
        }
    }

    // Concurrent writers drive promotion repeatedly while a reader races them.
    // Nothing may be lost: every key written is still readable at the end,
    // whether it landed in the mutable table or the frozen one, and every one
    // of them also reached a log intact.
    {
        constexpr int      kThreads   = 4;
        constexpr int      kPerThread = 200;
        constexpr uint64_t kReadSeq   = kThreads * kPerThread + 10;

        const auto keyFor = [](int thread, int i) {
            return "t" + std::to_string(thread) + "-k" + std::to_string(i);
        };

        // A real log here, not nullptr: this is the only case in the suite
        // where several threads append to one WalWriter at once, so it is what
        // exercises the writer's internal locking.
        LogSet logs{ dir / "concurrent", { }, { } };
        std::filesystem::create_directories(logs.dir);

        // promote() serializes its calls to the factory, so LogSet needs no
        // lock of its own.
        Manager manager(4096, [&] { return logs.create(); });

        std::atomic<uint64_t> next_seq{ 1 };
        std::atomic<bool>     writing{ true };

        std::vector<std::thread> writers;
        for (int t = 0; t < kThreads; ++t) {
            writers.emplace_back([&, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    manager.add(next_seq.fetch_add(1), ValueType::kValue, keyFor(t, i),
                                std::string(64, static_cast<char>('a' + t)));
                    // Freeze but never retire, so every key stays reachable
                    // through the mutable or the immutable table -- there is
                    // nowhere else for one to go without a flush.
                    if (manager.shouldPromote()) {
                        (void)manager.promote();
                    }
                }
            });
        }
        std::thread reader([&] {
            while (writing.load()) {
                (void)lookup(manager, keyFor(0, 0), kReadSeq);
                (void)lookup(manager, "absent", kReadSeq);
            }
        });

        for (auto& writer : writers) {
            writer.join();
        }
        writing.store(false);
        reader.join();

        for (int t = 0; t < kThreads; ++t) {
            for (int i = 0; i < kPerThread; ++i) {
                assert(lookup(manager, keyFor(t, i), kReadSeq) ==
                       std::string(64, static_cast<char>('a' + t)));
            }
        }

        // Every write also reached a log, intact. Records arrive in whatever
        // order the threads won the writer's lock, so check each is present
        // exactly once with the right value rather than checking the order.
        logs.closeAll();
        std::unordered_map<std::string, std::string> outstanding;
        for (int t = 0; t < kThreads; ++t) {
            for (int i = 0; i < kPerThread; ++i) {
                outstanding.emplace(keyFor(t, i),
                                    std::string(64, static_cast<char>('a' + t)));
            }
        }
        size_t replayed = 0;
        for (const auto& path : logs.paths) {
            const int fd = ::open(path.c_str(), O_RDONLY);
            assert(fd >= 0);
            WalReader log_reader(fd);
            while (const auto record = log_reader.readRecord()) {
                WalEntryView entry;
                assert(decodeWalEntry(byteView(*record), entry));
                const auto found = outstanding.find(std::string(entry.key));
                assert(found != outstanding.end()); // unknown or duplicated key
                assert(entry.value == found->second);
                outstanding.erase(found);
                ++replayed;
            }
            assert(!log_reader.stoppedEarly());
            ::close(fd);
        }
        assert(replayed == static_cast<size_t>(kThreads * kPerThread));
        assert(outstanding.empty());
    }

    std::filesystem::remove_all(dir);
    return 0;
}
