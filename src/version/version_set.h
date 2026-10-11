#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <boost/thread/shared_mutex.hpp>

#include "version/version.h"
#include "version/version_edit.h"
#include "wal/wal_writer.h"

// Threshold for rolling the manifest to a file-set snapshot.
inline constexpr uint64_t kDefaultMaxManifestFileSize = 64ull << 20;

// Owns the current immutable file set and its manifest. Manifest payloads are
// VersionEdits in WAL framing. Commits build a Version, sync its edit, then
// publish it. createNew(), recover(), and logAndApply() require one writer;
// current() and the counters support concurrent readers.
class VersionSet {
public:
    explicit VersionSet(std::filesystem::path dir,
                        uint64_t max_manifest_file_size = kDefaultMaxManifestFileSize);
    ~VersionSet();

    VersionSet(const VersionSet&)            = delete;
    VersionSet& operator=(const VersionSet&) = delete;

    // Initializes an empty database: creates the directory, writes a manifest
    // holding an empty snapshot, and points CURRENT at it. Throws if the
    // directory already holds a database.
    void createNew();

    // Replays CURRENT's manifest and rolls it to a fresh snapshot.
    //
    // Throws if CURRENT is missing or unreadable, if it names a manifest that
    // is not there, or if a table the manifest references cannot be opened.
    void recover();

    // Appends `edit` to the manifest, makes it durable, and installs the
    // Version it produces. `edit` is stamped with the current counters on the
    // way through, so a caller only fills in what it is actually changing.
    //
    // On failure, the current Version remains unchanged. A partial manifest
    // append requires recovery before another commit.
    void logAndApply(VersionEdit& edit);

    [[nodiscard]] std::shared_ptr<const Version> current() const;

    // Reserves a unique file number.
    [[nodiscard]] uint64_t newFileNumber() noexcept { return _next_file_number.fetch_add(1); }
    [[nodiscard]] uint64_t nextFileNumber() const noexcept { return _next_file_number.load(); }

    [[nodiscard]] uint64_t lastSequence() const noexcept { return _last_sequence.load(); }
    void setLastSequence(uint64_t sequence) noexcept { _last_sequence.store(sequence); }
    // Reserves the next sequence number for a write.
    [[nodiscard]] uint64_t newSequence() noexcept { return _last_sequence.fetch_add(1) + 1; }

    // The WAL whose contents are not yet in any table. Everything numbered
    // below it has been flushed and its log is deletable.
    [[nodiscard]] uint64_t logNumber() const noexcept { return _log_number.load(); }

    [[nodiscard]] uint64_t manifestNumber() const noexcept { return _manifest_number.load(); }

    // Every file number referenced by any Version still alive -- the current
    // one and any older one a reader is still holding. Anything else in the
    // directory is unreachable and safe to delete.
    [[nodiscard]] std::set<uint64_t> liveFiles() const;

private:
    // Writes the whole current file set as one edit, so a manifest can be read
    // without the history that produced it.
    [[nodiscard]] VersionEdit snapshotEdit(const Version& version, uint64_t log_number) const;

    // Creates a manifest holding `version` as a snapshot, points CURRENT at it,
    // and retires the previous one. On a throw the previous manifest and
    // CURRENT are left exactly as they were.
    void installNewManifest(const Version& version, uint64_t log_number);

    void closeManifest() noexcept;
    void trackVersion(const std::shared_ptr<const Version>& version);

    std::filesystem::path _dir;
    uint64_t              _max_manifest_file_size;

    std::atomic<uint64_t> _next_file_number{ 1 };
    std::atomic<uint64_t> _last_sequence{ 0 };
    std::atomic<uint64_t> _log_number{ 0 };
    std::atomic<uint64_t> _manifest_number{ 0 };

    int                        _manifest_fd{ -1 };
    std::unique_ptr<WalWriter> _manifest_log;

    mutable boost::shared_mutex    _mutex;
    std::shared_ptr<const Version> _current;
    // Readers pin files through their live Versions.
    mutable std::vector<std::weak_ptr<const Version>> _versions;
};
