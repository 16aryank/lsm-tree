#include "version/version_set.h"

#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <unistd.h>

#include "coding.h"
#include "db/filename.h"
#include "util/file_util.h"
#include "wal/wal_reader.h"

namespace {

[[noreturn]] void corrupt(const std::filesystem::path& path, const std::string& what) {
    throw std::runtime_error("manifest: " + path.string() + ": " + what);
}

int openForAppend(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "manifest create: " + path.string());
    }
    return fd;
}

// CURRENT holds one manifest base name, with optional trailing whitespace.
std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

VersionSet::VersionSet(std::filesystem::path dir, uint64_t max_manifest_file_size)
    : _dir(std::move(dir)), _max_manifest_file_size(max_manifest_file_size) { }

VersionSet::~VersionSet() {
    closeManifest();
}

void VersionSet::advanceFileNumberPast(uint64_t number) noexcept {
    uint64_t next = _next_file_number.load();
    while (next <= number &&
           !_next_file_number.compare_exchange_weak(next, number + 1)) { }
}

void VersionSet::closeManifest() noexcept {
    _manifest_log.reset();
    if (_manifest_fd >= 0) {
        ::close(_manifest_fd);
        _manifest_fd = -1;
    }
}

std::shared_ptr<const Version> VersionSet::current() const {
    boost::shared_lock<boost::shared_mutex> lock(_mutex);
    return _current;
}

void VersionSet::trackVersion(const std::shared_ptr<const Version>& version) {
    std::erase_if(_versions, [](const std::weak_ptr<const Version>& v) { return v.expired(); });
    _versions.push_back(version);
}

std::set<uint64_t> VersionSet::liveFiles() const {
    boost::shared_lock<boost::shared_mutex> lock(_mutex);

    std::set<uint64_t> live;
    for (const auto& weak : _versions) {
        const std::shared_ptr<const Version> version = weak.lock();
        if (!version) {
            continue;
        }
        for (int level = 0; level < kNumLevels; ++level) {
            for (const auto& file : version->files(level)) {
                live.insert(file.meta.number);
            }
        }
    }
    return live;
}

VersionEdit VersionSet::snapshotEdit(const Version& version, uint64_t log_number) const {
    VersionEdit edit;
    edit.setLogNumber(log_number);
    edit.setNextFileNumber(_next_file_number.load());
    edit.setLastSequence(_last_sequence.load());
    for (int level = 0; level < kNumLevels; ++level) {
        for (const auto& file : version.files(level)) {
            edit.addFile(level, file.meta);
        }
    }
    return edit;
}

void VersionSet::installNewManifest(const Version& version, uint64_t log_number) {
    const uint64_t              number = newFileNumber();
    const std::filesystem::path path   = manifestFileName(_dir, number);

    const int fd = openForAppend(path);
    auto      log = std::make_unique<WalWriter>(fd);

    try {
        // Persist the snapshot before switching CURRENT to it.
        const std::string record = snapshotEdit(version, log_number).encode();
        log->addRecord(byteSpan(record));
        log->sync();

        // The rename is the commit point for the whole rotation.
        fileutil::writeFileAtomic(currentFileName(_dir), manifestBaseName(number) + "\n",
                                  tempFileName(_dir, number));
    } catch (...) {
        ::close(fd);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        throw;
    }

    const uint64_t previous = _manifest_number.exchange(number);
    closeManifest();
    _manifest_fd  = fd;
    _manifest_log = std::move(log);

    if (previous != 0 && previous != number) {
        std::error_code ec;
        std::filesystem::remove(manifestFileName(_dir, previous), ec);
    }
}

void VersionSet::createNew() {
    std::filesystem::create_directories(_dir);

    if (std::filesystem::exists(currentFileName(_dir))) {
        throw std::runtime_error("database already exists: " + _dir.string());
    }

    _next_file_number.store(1);
    _last_sequence.store(0);
    _log_number.store(0);
    _manifest_failed = false;

    VersionBuilder builder(_dir, nullptr);
    std::shared_ptr<const Version> empty = builder.build();

    installNewManifest(*empty, 0);

    boost::unique_lock<boost::shared_mutex> lock(_mutex);
    _current = empty;
    trackVersion(empty);
}

void VersionSet::recover() {
    const std::filesystem::path current_path = currentFileName(_dir);
    if (!std::filesystem::exists(current_path)) {
        throw std::runtime_error("no database in " + _dir.string() + ": CURRENT is missing");
    }

    const std::string      contents = fileutil::readFileToString(current_path);
    const std::string_view name     = trim(contents);

    FileType type   = FileType::kUnknown;
    uint64_t number = 0;
    if (!parseFileName(name, type, number) || type != FileType::kManifest) {
        corrupt(current_path, "does not name a manifest");
    }

    const std::filesystem::path manifest_path = _dir / name;
    const int fd = ::open(manifest_path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "manifest open: " + manifest_path.string());
    }

    VersionBuilder builder(_dir, nullptr);
    uint64_t       log_number       = 0;
    uint64_t       next_file_number = 0;
    uint64_t       last_sequence    = 0;
    uint64_t       highest_file     = 0;
    bool           any_record       = false;

    try {
        // Recovery keeps the complete prefix before a damaged record.
        WalReader reader(fd, 0, WalReadMode::kRecovery);
        while (const auto record = reader.readRecord()) {
            VersionEdit edit;
            if (!edit.decode(byteView(*record))) {
                corrupt(manifest_path, "undecodable version edit");
            }
            builder.apply(edit);

            if (edit.hasLogNumber())      { log_number = edit.logNumber(); }
            if (edit.hasNextFileNumber()) { next_file_number = edit.nextFileNumber(); }
            if (edit.hasLastSequence())   { last_sequence = edit.lastSequence(); }

            for (const auto& [level, meta] : edit.addedFiles()) {
                (void)level;
                highest_file = std::max(highest_file, meta.number);
            }
            any_record = true;
        }
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);

    if (!any_record) {
        corrupt(manifest_path, "no readable records");
    }

    _log_number.store(log_number);
    _last_sequence.store(last_sequence);
    // Resume above the manifest and every table it names.
    _next_file_number.store(std::max({ next_file_number, highest_file + 1, number + 1 }));
    _manifest_number.store(number);

    std::shared_ptr<const Version> recovered = builder.build();

    // A fresh snapshot keeps new edits reachable after a damaged old tail.
    installNewManifest(*recovered, log_number);

    boost::unique_lock<boost::shared_mutex> lock(_mutex);
    _current = recovered;
    trackVersion(recovered);
    _manifest_failed = false;
}

void VersionSet::logAndApply(VersionEdit& edit) {
    if (_manifest_failed) {
        throw std::runtime_error("manifest needs recovery after a failed append");
    }
    if (!edit.hasLogNumber()) {
        edit.setLogNumber(_log_number.load());
    }
    edit.setNextFileNumber(_next_file_number.load());
    edit.setLastSequence(_last_sequence.load());

    // Validate the next file set before persisting its edit.
    VersionBuilder builder(_dir, current());
    builder.apply(edit);
    std::shared_ptr<const Version> next = builder.build();

    const uint64_t log_number = edit.logNumber();

    if (_manifest_log == nullptr || _manifest_log->fileOffset() >= _max_manifest_file_size) {
        // Rolling writes a snapshot of the *new* version, which already
        // includes this edit -- so the edit itself is never appended.
        installNewManifest(*next, log_number);
    } else {
        const std::string record = edit.encode();
        try {
            _manifest_log->addRecord(byteSpan(record));
            // Durable before the version is installed.
            _manifest_log->sync();
        } catch (...) {
            _manifest_failed = true;
            throw;
        }
    }

    boost::unique_lock<boost::shared_mutex> lock(_mutex);
    _current = next;
    _log_number.store(log_number);
    trackVersion(next);
}
