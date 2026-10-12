// End-to-end writes, flushes, restarts, and manifest-based recovery.
#include "src/db/db.h"
#include "src/db/filename.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

using Engine = DB<std::string, std::string>;

std::filesystem::path testDir() {
    return std::filesystem::temp_directory_path() /
           ("lsm-db-test-" + std::to_string(::getpid()));
}

std::filesystem::path caseDir(const std::filesystem::path& root, const char* name) {
    const std::filesystem::path dir = root / name;
    std::filesystem::remove_all(dir);
    return dir;
}

Options optionsFor(const std::filesystem::path& dir, size_t memtable_bytes = 1024) {
    Options options;
    options.dir                 = dir;
    options.memtable_size_bytes = memtable_bytes;
    return options;
}

std::string got(const Engine& db, const std::string& key) {
    const auto value = db.get(key);
    return value ? *value : ".";
}

std::string keyFor(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%05d", i);
    return buf;
}

std::string valueFor(int i) {
    return "value-" + std::to_string(i) + std::string(80, 'p');
}

std::vector<uint64_t> filesOfType(const std::filesystem::path& dir, FileType wanted) {
    std::vector<uint64_t> numbers;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        FileType type   = FileType::kUnknown;
        uint64_t number = 0;
        if (parseFileName(entry.path().filename().string(), type, number) && type == wanted) {
            numbers.push_back(number);
        }
    }
    return numbers;
}

bool threw(auto&& fn) {
    try {
        fn();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    const std::filesystem::path root = testDir();
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    // Basic reads and writes, and the layout a fresh database leaves behind.
    {
        const auto dir = caseDir(root, "basic");
        auto db = Engine::open(optionsFor(dir));

        db->put("a", "one");
        db->put("b", "two");
        assert(got(*db, "a") == "one");
        assert(got(*db, "b") == "two");
        assert(got(*db, "missing") == ".");

        // An overwrite is a new version, and the newest one wins.
        db->put("a", "one-updated");
        assert(got(*db, "a") == "one-updated");

        db->del("b");
        assert(got(*db, "b") == ".");
        // Deleting something absent is not an error and stays absent.
        db->del("never-existed");
        assert(got(*db, "never-existed") == ".");

        assert(std::filesystem::exists(currentFileName(dir)));
        assert(filesOfType(dir, FileType::kManifest).size() == 1);
        assert(filesOfType(dir, FileType::kLog).size() == 1);
    }

    // A snapshot pins what a read can see, regardless of what is written after
    // it -- which is the whole point of assigning sequence numbers centrally.
    {
        const auto dir = caseDir(root, "snapshot");
        auto db = Engine::open(optionsFor(dir));

        db->put("k", "first");
        const uint64_t snapshot = db->snapshot();
        db->put("k", "second");
        db->put("fresh", "new");

        assert(got(*db, "k") == "second");
        assert(*db->get("k", snapshot) == "first");
        // A key written after the snapshot is invisible to it.
        assert(!db->get("fresh", snapshot).has_value());

        // A delete after the snapshot likewise does not reach back.
        db->del("k");
        assert(got(*db, "k") == ".");
        assert(*db->get("k", snapshot) == "first");
    }

    // Everything still in memory at close time is recovered from the log. No
    // flush happens here at all -- this is the pure replay path, and the one a
    // crash takes.
    {
        const auto dir = caseDir(root, "replay");
        {
            auto db = Engine::open(optionsFor(dir, 1 << 20)); // never flushes
            db->put("alpha", "one");
            db->put("beta", "two");
            db->put("alpha", "one-again");
            db->del("beta");
            assert(db->currentVersion()->numFiles() == 0);
        }
        {
            auto db = Engine::open(optionsFor(dir, 1 << 20));
            assert(got(*db, "alpha") == "one-again");
            assert(got(*db, "beta") == "."); // the tombstone survived
            // Replay flushed what the log held, so the tree now has a table.
            assert(db->currentVersion()->numFiles() == 1);

            // Writes after recovery are numbered above everything recovered,
            // so they win over what came before.
            db->put("alpha", "post-recovery");
            assert(got(*db, "alpha") == "post-recovery");
        }
        {
            // And a third open sees the second one's writes too.
            auto db = Engine::open(optionsFor(dir, 1 << 20));
            assert(got(*db, "alpha") == "post-recovery");
            assert(got(*db, "beta") == ".");
        }
    }

    // The main event: enough writes to force many flushes, then a reopen that
    // has to reconstruct the whole file set from the manifest.
    {
        const auto dir = caseDir(root, "flushes");
        constexpr int kCount = 300;

        std::map<std::string, std::string> expected;
        {
            auto db = Engine::open(optionsFor(dir));
            for (int i = 0; i < kCount; ++i) {
                db->put(keyFor(i), valueFor(i));
                expected[keyFor(i)] = valueFor(i);
            }
            // Rewrite a slice so the newest version has to win across tables.
            for (int i = 0; i < kCount; i += 7) {
                db->put(keyFor(i), "rewritten-" + std::to_string(i));
                expected[keyFor(i)] = "rewritten-" + std::to_string(i);
            }
            // Delete a different slice.
            for (int i = 3; i < kCount; i += 11) {
                db->del(keyFor(i));
                expected.erase(keyFor(i));
            }

            db->waitForPendingFlush();
            assert(db->lastFlushError().empty());
            // Several flushes happened, so this is genuinely a multi-table read.
            assert(db->currentVersion()->numFiles() > 1);

            for (int i = 0; i < kCount; ++i) {
                const auto found = expected.find(keyFor(i));
                assert(got(*db, keyFor(i)) == (found == expected.end() ? "." : found->second));
            }
        }

        // Reopened from the manifest alone.
        {
            auto db = Engine::open(optionsFor(dir));
            assert(db->currentVersion()->numFiles() > 1);
            for (int i = 0; i < kCount; ++i) {
                const auto found = expected.find(keyFor(i));
                assert(got(*db, keyFor(i)) == (found == expected.end() ? "." : found->second));
            }
        }
    }

    // Obsolete files are swept: the logs a flush retired, the manifests a
    // rotation replaced, and any table no live version names.
    {
        const auto dir = caseDir(root, "sweep");
        {
            auto db = Engine::open(optionsFor(dir));
            for (int i = 0; i < 200; ++i) {
                db->put(keyFor(i), valueFor(i));
            }
            db->waitForPendingFlush();
            assert(db->lastFlushError().empty());

            // One live manifest and one live log, however many flushes ran.
            assert(filesOfType(dir, FileType::kManifest).size() == 1);
            assert(filesOfType(dir, FileType::kLog).size() == 1);
            // No staging files left over from a CURRENT rename.
            assert(filesOfType(dir, FileType::kTemp).empty());

            // Every table on disk is one the current version names.
            const auto tables = filesOfType(dir, FileType::kTable);
            assert(tables.size() == db->currentVersion()->numFiles());
        }
        {
            auto db = Engine::open(optionsFor(dir));
            assert(filesOfType(dir, FileType::kManifest).size() == 1);
            assert(got(*db, keyFor(0)) == valueFor(0));
            assert(got(*db, keyFor(199)) == valueFor(199));
        }
    }

    // Four overlapping flushes compact into level 1. The merge keeps old
    // versions and tombstones, including versions visible to an old sequence.
    {
        const auto dir = caseDir(root, "compaction");
        Options options = optionsFor(dir, 512);
        options.compaction_output_file_size = 350;
        uint64_t old_sequence = 0;
        {
            auto db = Engine::open(options);
            std::shared_ptr<const Version> held_version;
            std::vector<uint64_t> held_tables;
            db->put("anchor", "first");
            old_sequence = db->snapshot();
            for (int round = 0; round < 5; ++round) {
                db->put("anchor", "round-" + std::to_string(round));
                db->put("doomed", "value");
                db->del("doomed");
                for (int i = 0; i < 40; ++i) {
                    db->put(keyFor(i), valueFor(round * 40 + i));
                }
                db->waitForPendingFlush();
                db->waitForPendingCompaction();
                assert(db->lastFlushError().empty());
                assert(db->lastCompactionError().empty());
                if (round == 0) {
                    held_version = db->currentVersion();
                    for (int level = 0; level < kNumLevels; ++level) {
                        for (const auto& file : held_version->files(level)) {
                            held_tables.push_back(file.meta.number);
                        }
                    }
                }
            }
            assert(!db->currentVersion()->files(1).empty());
            assert(db->currentVersion()->files(1).size() > 1);
            const auto stats = db->compactionStats();
            assert(stats.runs > 0 && stats.runs_by_level[0] > 0);
            assert(stats.input_bytes > 0 && stats.output_bytes > 0);
            assert(got(*db, "anchor") == "round-4");
            assert(*db->get("anchor", old_sequence) == "first");
            assert(got(*db, "doomed") == ".");
            for (int i = 0; i < 40; ++i) {
                assert(got(*db, keyFor(i)) == valueFor(160 + i));
            }
            for (const uint64_t number : held_tables) {
                assert(std::filesystem::exists(tableFileName(dir, number)));
            }
            held_version.reset();
            for (int i = 40; i < 80; ++i) {
                db->put(keyFor(i), valueFor(i));
            }
            db->waitForPendingCompaction();
            for (const uint64_t number : held_tables) {
                bool current = false;
                for (int level = 0; level < kNumLevels; ++level) {
                    for (const auto& file : db->currentVersion()->files(level)) {
                        current |= file.meta.number == number;
                    }
                }
                if (!current) {
                    assert(!std::filesystem::exists(tableFileName(dir, number)));
                }
            }
        }
        {
            auto db = Engine::open(options);
            db->waitForPendingCompaction();
            assert(db->lastCompactionError().empty());
            assert(*db->get("anchor", old_sequence) == "first");
            assert(got(*db, "anchor") == "round-4");
            assert(got(*db, "doomed") == ".");
        }
    }

    // The byte budget at level 1 also schedules a merge into level 2.
    {
        const auto dir = caseDir(root, "deeper-compaction");
        Options options = optionsFor(dir, 1 << 19);
        const std::string large_value(1 << 20, 'v');
        {
            auto db = Engine::open(options);
            for (int i = 0; i < 16; ++i) {
                db->put(keyFor(i), large_value);
                db->waitForPendingFlush();
            }
            db->waitForPendingCompaction();
            assert(db->lastFlushError().empty());
            assert(db->lastCompactionError().empty());
            assert(!db->currentVersion()->files(2).empty());
            assert(db->compactionStats().runs_by_level[1] > 0);
            for (int i = 0; i < 16; ++i) {
                assert(got(*db, keyFor(i)) == large_value);
            }
        }
        {
            auto db = Engine::open(options);
            for (int i = 0; i < 16; ++i) {
                assert(got(*db, keyFor(i)) == large_value);
            }
        }
    }

    // Opening uses the manifest's file set, ignoring and sweeping a stray SSTable.
    {
        const auto dir = caseDir(root, "stray");
        {
            auto db = Engine::open(optionsFor(dir));
            db->put("real", "data");
        }

        const std::filesystem::path stray = tableFileName(dir, 999999);
        std::ofstream(stray) << "not a table at all";
        assert(std::filesystem::exists(stray));

        {
            auto db = Engine::open(optionsFor(dir));
            // Opening did not try to read it, and the sweep removed it.
            assert(!std::filesystem::exists(stray));
            assert(got(*db, "real") == "data");

            // The counter did not jump past the stray file's number, so the
            // manifest -- not the directory -- is what numbering follows.
            for (int i = 0; i < 50; ++i) {
                db->put(keyFor(i), valueFor(i));
            }
            db->waitForPendingFlush();
            assert(db->lastFlushError().empty());
            for (const uint64_t number : filesOfType(dir, FileType::kTable)) {
                assert(number < 999999);
            }
        }
    }

    // Files this engine did not write are left strictly alone: an unrecognized
    // name is not ours to reason about, let alone unlink.
    {
        const auto dir = caseDir(root, "foreign");
        auto db = Engine::open(optionsFor(dir));
        std::ofstream(dir / "notes.txt") << "keep me";
        std::ofstream(dir / "backup.sst.bak") << "keep me too";

        for (int i = 0; i < 100; ++i) {
            db->put(keyFor(i), valueFor(i));
        }
        db->waitForPendingFlush();

        assert(std::filesystem::exists(dir / "notes.txt"));
        assert(std::filesystem::exists(dir / "backup.sst.bak"));
    }

    // Open-time policy.
    {
        const auto missing = caseDir(root, "missing");
        std::filesystem::create_directories(missing);
        Options no_create = optionsFor(missing);
        no_create.create_if_missing = false;
        assert(threw([&] { (void)Engine::open(no_create); }));

        const auto existing = caseDir(root, "existing");
        { auto db = Engine::open(optionsFor(existing)); db->put("k", "v"); }
        Options must_not_exist = optionsFor(existing);
        must_not_exist.error_if_exists = true;
        assert(threw([&] { (void)Engine::open(must_not_exist); }));

        // A directory is required; there is no implicit default.
        Options no_dir;
        assert(threw([&] { (void)Engine::open(no_dir); }));
    }

    // Sequence numbers keep climbing across restarts. If they restarted, a
    // write after a reopen would be numbered below one before it and would be
    // invisible to a read at the newest sequence.
    {
        const auto dir = caseDir(root, "sequence");
        uint64_t after_first = 0;
        {
            auto db = Engine::open(optionsFor(dir));
            for (int i = 0; i < 20; ++i) {
                db->put(keyFor(i), valueFor(i));
            }
            after_first = db->snapshot();
            assert(after_first >= 20);
        }
        {
            auto db = Engine::open(optionsFor(dir));
            assert(db->snapshot() >= after_first);
            db->put("newest", "wins");
            assert(db->snapshot() > after_first);
            assert(got(*db, "newest") == "wins");
            assert(got(*db, keyFor(0)) == valueFor(0));
        }
    }

    // A key deleted before a flush stays deleted after one, and after a
    // restart. The tombstone has to reach the table and be honoured there.
    {
        const auto dir = caseDir(root, "tombstone");
        {
            auto db = Engine::open(optionsFor(dir));
            db->put("doomed", "value");
            db->del("doomed");
            // Push the tombstone out of memory and into a table.
            for (int i = 0; i < 100; ++i) {
                db->put(keyFor(i), valueFor(i));
            }
            db->waitForPendingFlush();
            assert(got(*db, "doomed") == ".");
        }
        {
            auto db = Engine::open(optionsFor(dir));
            assert(got(*db, "doomed") == ".");
            assert(got(*db, keyFor(0)) == valueFor(0));
        }
    }

    // Concurrent writers driving repeated flushes while readers race them.
    //
    // The window this is aimed at: a flush installs a new Version and then
    // retires the frozen memtable. A reader searching the memtables first and
    // the Version second must never fall between the two and conclude a key is
    // absent. The ordering that rules it out is that the install happens
    // before the drop, and the reader's Version read happens after its
    // memtable read -- so a reader that sees the drop must also see the
    // install. Run this under -fsanitize=thread.
    {
        const auto dir = caseDir(root, "concurrent");
        constexpr int kThreads   = 4;
        constexpr int kPerThread = 150;

        const auto concurrentKey = [](int thread, int i) {
            return "t" + std::to_string(thread) + "-" + std::to_string(i);
        };

        auto db = Engine::open(optionsFor(dir));

        std::atomic<bool> writing{ true };
        std::atomic<bool> vanished{ false };

        std::vector<std::thread> writers;
        for (int t = 0; t < kThreads; ++t) {
            writers.emplace_back([&, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    db->put(concurrentKey(t, i), valueFor(i));
                }
            });
        }

        // Reads a key thread 0 wrote first. Once it has been seen, it must
        // never stop being visible -- not while it moves from the mutable
        // table to the frozen one to a table on disk.
        std::thread reader([&] {
            bool seen = false;
            while (writing.load()) {
                const auto value = db->get(concurrentKey(0, 0));
                if (value) {
                    seen = true;
                } else if (seen) {
                    vanished.store(true);
                }
                (void)db->get("absent-key");
                (void)db->currentVersion();
            }
        });

        for (auto& writer : writers) {
            writer.join();
        }
        writing.store(false);
        reader.join();

        assert(!vanished.load()); // a key was never invisible mid-flush

        db->waitForPendingFlush();
        assert(db->lastFlushError().empty());

        for (int t = 0; t < kThreads; ++t) {
            for (int i = 0; i < kPerThread; ++i) {
                assert(got(*db, concurrentKey(t, i)) == valueFor(i));
            }
        }

        // And all of it survives a restart.
        db.reset();
        auto reopened = Engine::open(optionsFor(dir));
        for (int t = 0; t < kThreads; ++t) {
            for (int i = 0; i < kPerThread; ++i) {
                assert(got(*reopened, concurrentKey(t, i)) == valueFor(i));
            }
        }
    }

    // A snapshot captured while writers overwrite one key must keep returning
    // the same value after flushes, compactions, and restart.
    {
        const auto dir = caseDir(root, "concurrent-snapshots");
        const Options options = optionsFor(dir, 256);
        auto db = Engine::open(options);
        std::atomic<bool> writing{ true };
        std::vector<std::pair<uint64_t, std::string>> observed;
        std::thread sampler([&] {
            while (writing.load() && observed.size() < 1000) {
                const uint64_t sequence = db->snapshot();
                const auto value = db->get("shared", sequence);
                observed.emplace_back(sequence, value.value_or("."));
                std::this_thread::yield();
            }
        });
        std::vector<std::thread> writers;
        for (int thread = 0; thread < 2; ++thread) {
            writers.emplace_back([&, thread] {
                for (int i = 0; i < 150; ++i) {
                    db->put("shared", std::to_string(thread) + ":" + std::to_string(i) +
                                       std::string(100, 'x'));
                }
            });
        }
        for (auto& writer : writers) {
            writer.join();
        }
        writing.store(false);
        sampler.join();
        observed.emplace_back(db->snapshot(), got(*db, "shared"));
        db->waitForPendingCompaction();
        assert(db->lastFlushError().empty());
        assert(db->lastCompactionError().empty());
        for (const auto& [sequence, value] : observed) {
            assert(db->get("shared", sequence).value_or(".") == value);
        }
        db.reset();
        auto reopened = Engine::open(options);
        reopened->waitForPendingCompaction();
        for (const auto& [sequence, value] : observed) {
            assert(reopened->get("shared", sequence).value_or(".") == value);
        }
    }

    std::filesystem::remove_all(root);
    return 0;
}
