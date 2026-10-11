// Manifest replay, torn records, rotation, and live file tracking.
#include "src/db/filename.h"
#include "src/key.h"
#include "src/sstable/sstable_builder.h"
#include "src/util/file_util.h"
#include "src/version/version_set.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace {

std::filesystem::path testDir() {
    return std::filesystem::temp_directory_path() /
           ("lsm-versionset-test-" + std::to_string(::getpid()));
}

// Each case gets its own subdirectory: a database is a directory, and reusing
// one across cases would make every case depend on the last one's cleanup.
std::filesystem::path caseDir(const std::filesystem::path& root, const char* name) {
    const std::filesystem::path dir = root / name;
    std::filesystem::remove_all(dir);
    return dir;
}

// Writes a one-key table under `number` and returns the metadata for it.
FileMetaData writeTable(const std::filesystem::path& dir, uint64_t number,
                        const std::string& key) {
    const std::filesystem::path path = tableFileName(dir, number);
    SSTableBuilder builder(path);
    builder.add(key, packSeqAndType(number, ValueType::kValue), "value-" + key);
    const uint64_t size  = builder.finish();
    const auto     table = SSTable::open(path);

    FileMetaData meta;
    meta.number       = number;
    meta.file_size    = size;
    meta.smallest_key = table->smallestKey();
    meta.smallest_tag = table->smallestTag();
    meta.largest_key  = table->largestKey();
    meta.largest_tag  = table->largestTag();
    return meta;
}

// Adds one level-0 table, the way a flush does.
FileMetaData addTable(VersionSet& versions, const std::filesystem::path& dir,
                      const std::string& key) {
    const FileMetaData meta = writeTable(dir, versions.newFileNumber(), key);
    VersionEdit edit;
    edit.addFile(0, meta);
    versions.logAndApply(edit);
    return meta;
}

std::filesystem::path manifestFromCurrent(const std::filesystem::path& dir) {
    std::string name = fileutil::readFileToString(currentFileName(dir));
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
        name.pop_back();
    }
    return dir / name;
}

std::vector<uint64_t> level0Numbers(const Version& version) {
    std::vector<uint64_t> numbers;
    for (const auto& file : version.files(0)) {
        numbers.push_back(file.meta.number);
    }
    std::sort(numbers.begin(), numbers.end());
    return numbers;
}

size_t countManifests(const std::filesystem::path& dir) {
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        FileType type   = FileType::kUnknown;
        uint64_t number = 0;
        if (parseFileName(entry.path().filename().string(), type, number) &&
            type == FileType::kManifest) {
            ++count;
        }
    }
    return count;
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

    // A fresh database: CURRENT names a manifest that exists, and reopening it
    // yields the same empty tree.
    {
        const auto dir = caseDir(root, "fresh");
        {
            VersionSet versions(dir);
            versions.createNew();
            assert(versions.current()->numFiles() == 0);
            assert(std::filesystem::exists(manifestFromCurrent(dir)));
        }
        {
            VersionSet versions(dir);
            versions.recover();
            assert(versions.current()->numFiles() == 0);
            assert(versions.lastSequence() == 0);
        }
    }

    // createNew must refuse to overwrite an existing database: the file
    // numbers in a live directory are the only thing keeping its tables apart.
    {
        const auto dir = caseDir(root, "exists");
        VersionSet first(dir);
        first.createNew();

        VersionSet second(dir);
        assert(threw([&] { second.createNew(); }));
    }

    // Recovering a directory with no CURRENT is an error, not an empty tree --
    // silently starting fresh over someone's data is the one outcome that
    // loses it.
    {
        const auto dir = caseDir(root, "no-current");
        std::filesystem::create_directories(dir);
        VersionSet versions(dir);
        assert(threw([&] { versions.recover(); }));
    }

    // A CURRENT that does not name a manifest, and one that names a manifest
    // that is not there.
    {
        const auto dir = caseDir(root, "bad-current");
        std::filesystem::create_directories(dir);
        fileutil::writeFileAtomic(currentFileName(dir), "not-a-manifest\n", dir / "t.dbtmp");
        VersionSet garbage(dir);
        assert(threw([&] { garbage.recover(); }));

        fileutil::writeFileAtomic(currentFileName(dir), "MANIFEST-000777\n", dir / "t.dbtmp");
        VersionSet missing(dir);
        assert(threw([&] { missing.recover(); }));
    }

    // The ordinary path: several edits, then a reopen that reconstructs the
    // same file set and the same counters.
    {
        const auto dir = caseDir(root, "replay");
        std::vector<uint64_t> expected;
        uint64_t next_file = 0;

        {
            VersionSet versions(dir);
            versions.createNew();
            for (const char* key : { "alpha", "bravo", "charlie", "delta" }) {
                expected.push_back(addTable(versions, dir, key).number);
            }
            versions.setLastSequence(4242);
            VersionEdit seq_only;
            versions.logAndApply(seq_only);

            assert(level0Numbers(*versions.current()) == expected);
            next_file = versions.nextFileNumber();
        }

        {
            VersionSet versions(dir);
            versions.recover();
            assert(level0Numbers(*versions.current()) == expected);
            assert(versions.lastSequence() == 4242);
            // A recovered database never hands out a number it already used;
            // rotating the manifest consumes one, so it only ever moves up.
            assert(versions.nextFileNumber() >= next_file);
            // Every table is readable through the recovered version.
            assert(versions.current()->get("alpha", 100).has_value());
            assert(versions.current()->get("delta", 100).has_value());
        }
    }

    // Deletions replay too, so a compaction's swap survives a restart.
    {
        const auto dir = caseDir(root, "delete");
        uint64_t kept = 0;
        {
            VersionSet versions(dir);
            versions.createNew();
            const uint64_t dropped = addTable(versions, dir, "gone").number;
            kept = addTable(versions, dir, "stays").number;

            VersionEdit edit;
            edit.deleteFile(0, dropped);
            versions.logAndApply(edit);
            assert(level0Numbers(*versions.current()) == (std::vector<uint64_t>{ kept }));
        }
        {
            VersionSet versions(dir);
            versions.recover();
            assert(level0Numbers(*versions.current()) == (std::vector<uint64_t>{ kept }));
        }
    }

    // A torn tail. Cutting the manifest mid-record is what a crash during an
    // append leaves behind: every complete edit must still replay, and the
    // partial one must vanish without taking the rest with it.
    {
        const auto dir = caseDir(root, "torn");
        uint64_t   first_number = 0;
        uint64_t   complete_through = 0;

        {
            VersionSet versions(dir);
            versions.createNew();
            first_number = addTable(versions, dir, "first").number;

            // Everything through the first table's edit is durable here.
            complete_through = std::filesystem::file_size(manifestFromCurrent(dir));
            addTable(versions, dir, "second");
        }

        // Cut partway into the second edit's record: past its header, short of
        // its payload, which is a torn record rather than a clean boundary.
        const std::filesystem::path manifest = manifestFromCurrent(dir);
        assert(std::filesystem::file_size(manifest) > complete_through + 8);
        {
            const int fd = ::open(manifest.c_str(), O_WRONLY);
            assert(fd >= 0);
            assert(::ftruncate(fd, static_cast<off_t>(complete_through + 8)) == 0);
            ::close(fd);
        }

        VersionSet versions(dir);
        versions.recover();
        assert(level0Numbers(*versions.current()) == (std::vector<uint64_t>{ first_number }));
    }

    // A manifest cut exactly on a record boundary is a clean truncation rather
    // than a torn one, and must replay to precisely the edits that survived.
    {
        const auto dir = caseDir(root, "clean-cut");
        uint64_t   first_number = 0;
        uint64_t   complete_through = 0;

        {
            VersionSet versions(dir);
            versions.createNew();
            first_number = addTable(versions, dir, "first").number;
            complete_through = std::filesystem::file_size(manifestFromCurrent(dir));
            addTable(versions, dir, "second");
        }

        const std::filesystem::path manifest = manifestFromCurrent(dir);
        {
            const int fd = ::open(manifest.c_str(), O_WRONLY);
            assert(fd >= 0);
            assert(::ftruncate(fd, static_cast<off_t>(complete_through)) == 0);
            ::close(fd);
        }

        VersionSet versions(dir);
        versions.recover();
        assert(level0Numbers(*versions.current()) == (std::vector<uint64_t>{ first_number }));
    }

    // A manifest whose very first record is destroyed has nothing to replay.
    // That is corruption, not an empty database.
    {
        const auto dir = caseDir(root, "empty-manifest");
        {
            VersionSet versions(dir);
            versions.createNew();
        }
        const std::filesystem::path manifest = manifestFromCurrent(dir);
        {
            const int fd = ::open(manifest.c_str(), O_WRONLY | O_TRUNC);
            assert(fd >= 0);
            ::close(fd);
        }
        VersionSet versions(dir);
        assert(threw([&] { versions.recover(); }));
    }

    // Rolling. With a threshold of one byte every commit rotates, which is the
    // rotation path run over and over: state has to survive each one, and the
    // manifest that CURRENT stopped naming has to be cleaned up rather than
    // accumulating.
    {
        const auto dir = caseDir(root, "roll");
        std::vector<uint64_t> expected;
        {
            VersionSet versions(dir, /*max_manifest_file_size=*/1);
            versions.createNew();
            for (const char* key : { "a", "b", "c", "d", "e" }) {
                expected.push_back(addTable(versions, dir, key).number);
            }
            assert(level0Numbers(*versions.current()) == expected);
            // One live manifest, not one per commit.
            assert(countManifests(dir) == 1);
            assert(std::filesystem::exists(manifestFromCurrent(dir)));
        }
        {
            VersionSet versions(dir, 1);
            versions.recover();
            assert(level0Numbers(*versions.current()) == expected);
            assert(countManifests(dir) == 1);
        }
    }

    // liveFiles is what tells the obsolete-file sweep which tables it must not
    // delete. A version a reader is still holding keeps its files live even
    // after the current version has dropped them.
    {
        const auto dir = caseDir(root, "live");
        VersionSet versions(dir);
        versions.createNew();

        const uint64_t dropped = addTable(versions, dir, "old").number;
        const uint64_t kept    = addTable(versions, dir, "new").number;

        // A reader pins the version that still names both files.
        std::shared_ptr<const Version> pinned = versions.current();

        VersionEdit edit;
        edit.deleteFile(0, dropped);
        versions.logAndApply(edit);

        assert(level0Numbers(*versions.current()) == (std::vector<uint64_t>{ kept }));
        // Still live: the pinned version can still read it.
        assert(versions.liveFiles().contains(dropped));
        assert(versions.liveFiles().contains(kept));

        // Once the reader lets go, the file becomes collectable.
        pinned.reset();
        assert(!versions.liveFiles().contains(dropped));
        assert(versions.liveFiles().contains(kept));
    }

    // A manifest referencing a table that is not on disk cannot be recovered:
    // the version it describes is not one the engine can actually serve.
    {
        const auto dir = caseDir(root, "missing-table");
        uint64_t number = 0;
        {
            VersionSet versions(dir);
            versions.createNew();
            number = addTable(versions, dir, "vanishing").number;
        }
        std::filesystem::remove(tableFileName(dir, number));

        VersionSet versions(dir);
        assert(threw([&] { versions.recover(); }));
    }

    std::filesystem::remove_all(root);
    return 0;
}
