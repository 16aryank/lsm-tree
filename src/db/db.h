#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include "db/filename.h"
#include "db/options.h"
#include "key.h"
#include "memtable/memtable.h"
#include "memtable/memtable_manager.h"
#include "sstable/sstable_builder.h"
#include "util/file_util.h"
#include "version/version.h"
#include "version/version_edit.h"
#include "version/version_set.h"
#include "wal/wal_codec.h"
#include "wal/wal_reader.h"
#include "wal/wal_writer.h"

// How many times a flush is retried before it is left for a later write to
// pick up again.
inline constexpr size_t kFlushMaxAttempts = 3;

// Waited between attempts, multiplied by the attempt number.
inline constexpr std::chrono::milliseconds kFlushRetryBackoff{ 50 };

// Coordinates the memtable, WAL, SSTables, and manifest. WAL appends precede
// memtable inserts; ordinary writes do not sync the WAL before returning.
template <typename UserKey_, typename Value_>
    requires ByteStorable<UserKey_> && ByteStorable<Value_>
class DB {
public:
    using Manager = MemTableManager<UserKey_, Value_>;
    using Table   = MemTable<UserKey_, Value_>;

    // Opens or creates the database described by `options`. Throws if the
    // directory holds no database and create_if_missing is false, if it holds
    // one and error_if_exists is true, or if recovery finds the durable state
    // unreadable.
    [[nodiscard]] static std::unique_ptr<DB> open(Options options) {
        if (options.dir.empty()) {
            throw std::invalid_argument("DB::open: options.dir is required");
        }
        std::unique_ptr<DB> db(new DB(std::move(options)));
        db->initialize();
        return db;
    }

    ~DB() {
        // Recovery replays any writes remaining in the WAL after close.
        std::lock_guard<std::mutex> guard(_flush_mutex);
        if (_flush_thread.joinable()) {
            _flush_thread.join();
        }
    }

    DB(const DB&)            = delete;
    DB& operator=(const DB&) = delete;

    void put(const UserKey_& key, Value_ value) {
        write(ValueType::kValue, key, std::move(value));
    }

    // Records a tombstone for the key.
    void del(const UserKey_& key) {
        write(ValueType::kDelete, key, Value_());
    }

    // Reads at the newest sequence number, i.e. sees every write that has
    // returned.
    [[nodiscard]] std::optional<Value_> get(const UserKey_& key) const {
        return get(key, _versions.lastSequence());
    }

    // Reads as of `snapshot`, seeing no write numbered above it.
    [[nodiscard]] std::optional<Value_> get(const UserKey_& key, uint64_t snapshot) const {
        Value_ value;
        switch (_memtables->lookup(key, snapshot, value)) {
        case LookupStatus::kFound:
            return value;
        case LookupStatus::kDeleted:
            // A tombstone masks older versions on disk.
            return std::nullopt;
        case LookupStatus::kNotFound:
            break;
        }

        const std::shared_ptr<const Version> version = _versions.current();
        const std::optional<SSTable::Entry>  entry =
            version->get(std::string_view(key.data(), key.size()), snapshot);
        if (!entry || entry->type == ValueType::kDelete) {
            return std::nullopt;
        }
        return Value_(entry->value.data(), entry->value.size());
    }

    // A sequence number to read at later, so a series of get()s can see one
    // consistent state regardless of what is written in between.
    [[nodiscard]] uint64_t snapshot() const { return _versions.lastSequence(); }

    // Test and shutdown helper: blocks until any in-flight flush finishes. A
    // finished flush is not necessarily a successful one -- check
    // lastFlushError().
    void waitForPendingFlush() {
        std::lock_guard<std::mutex> guard(_flush_mutex);
        if (_flush_thread.joinable()) {
            _flush_thread.join();
        }
    }

    // Why the last flush failed, or empty if it succeeded. A non-empty value
    // means the frozen memtable is still in memory awaiting a retry; its
    // writes are in the log either way.
    [[nodiscard]] std::string lastFlushError() const {
        std::lock_guard<std::mutex> guard(_error_mutex);
        return _flush_error;
    }

    [[nodiscard]] const Options& options() const noexcept { return _options; }
    [[nodiscard]] std::shared_ptr<const Version> currentVersion() const {
        return _versions.current();
    }

private:
    explicit DB(Options options)
        : _options(std::move(options)),
          _versions(_options.dir, _options.max_manifest_file_size) { }

    // ── startup ───────────────────────────────────────────────────────────────

    void initialize() {
        const bool exists = std::filesystem::exists(currentFileName(_options.dir));

        if (!exists) {
            if (!_options.create_if_missing) {
                throw std::runtime_error("no database in " + _options.dir.string());
            }
            _versions.createNew();
        } else {
            if (_options.error_if_exists) {
                throw std::runtime_error("database already exists: " + _options.dir.string());
            }
            _versions.recover();
            replayLogs();
        }

        _memtables = std::make_unique<Manager>(_options.memtable_size_bytes,
                                               [this] { return createLog(); });

        VersionEdit edit;
        edit.setLogNumber(_memtables->currentWal().number);
        {
            std::lock_guard<std::mutex> guard(_manifest_mutex);
            _versions.logAndApply(edit);
        }

        removeObsoleteFiles();
    }

    // Rebuilds whatever the logs still hold that no table does, and flushes it
    // so the next open has nothing to replay.
    void replayLogs() {
        std::vector<uint64_t> numbers;
        for (const auto& entry : std::filesystem::directory_iterator(_options.dir)) {
            FileType type   = FileType::kUnknown;
            uint64_t number = 0;
            if (!parseFileName(entry.path().filename().string(), type, number)) {
                continue;
            }
            // Anything numbered below the manifest's log number is already in
            // a table; replaying it would resurrect deleted keys.
            if (type == FileType::kLog && number >= _versions.logNumber()) {
                numbers.push_back(number);
            }
        }
        // Replay logs in file-number order.
        std::sort(numbers.begin(), numbers.end());

        auto     table   = std::make_shared<Table>(_options.memtable_size_bytes);
        uint64_t max_seq = _versions.lastSequence();
        bool     any     = false;

        for (const uint64_t number : numbers) {
            const std::filesystem::path path = logFileName(_options.dir, number);
            const int fd = ::open(path.c_str(), O_RDONLY);
            if (fd < 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "log open: " + path.string());
            }

            try {
                WalReader reader(fd, 0, WalReadMode::kRecovery);
                while (const auto record = reader.readRecord()) {
                    WalEntryView entry;
                    if (!decodeWalEntry(byteView(*record), entry)) {
                        throw std::runtime_error("log: " + path.string() +
                                                 ": undecodable entry");
                    }
                    table->add(entry.sequenceNumber(), entry.valueType(),
                               UserKey_(entry.key.data(), entry.key.size()),
                               Value_(entry.value.data(), entry.value.size()));
                    max_seq = std::max(max_seq, entry.sequenceNumber());
                    any     = true;
                }
            } catch (...) {
                ::close(fd);
                throw;
            }
            ::close(fd);
        }

        // Keep new writes above the sequence numbers found in the logs.
        _versions.setLastSequence(max_seq);

        if (!any) {
            return;
        }

        VersionEdit  edit;
        FileMetaData meta;
        if (buildTable(*table, meta)) {
            edit.addFile(0, meta);
        }
        std::lock_guard<std::mutex> guard(_manifest_mutex);
        _versions.logAndApply(edit);
    }

    // ── write path ────────────────────────────────────────────────────────────

    void write(ValueType type, const UserKey_& key, Value_ value) {
        const uint64_t seq = _versions.newSequence();
        _memtables->add(seq, type, key, std::move(value));

        if (_memtables->shouldPromote()) {
            scheduleFlush();
        }
    }

    void scheduleFlush() {
        // Serializes promotion so two writers cannot both touch _flush_thread.
        std::lock_guard<std::mutex> guard(_flush_mutex);

        typename Manager::Promotion promotion = _memtables->promote();
        if (!promotion.table) {
            return;
        }

        // A successful promotion means the previous worker has finished.
        if (_flush_thread.joinable()) {
            _flush_thread.join();
        }
        _flush_thread = std::thread([this, promotion] { flushWorker(promotion); });
    }

    // Flushes the frozen table and commits its file metadata. A failed flush
    // leaves the table in memory for a later retry.
    void flushWorker(typename Manager::Promotion promotion) {
        std::string error;

        for (size_t attempt = 1; attempt <= kFlushMaxAttempts; ++attempt) {
            try {
                VersionEdit  edit;
                FileMetaData meta;
                if (buildTable(*promotion.table, meta)) {
                    edit.addFile(0, meta);
                }
                // The new log number retires logs covered by this flush.
                edit.setLogNumber(promotion.log_number);

                {
                    std::lock_guard<std::mutex> guard(_manifest_mutex);
                    _versions.logAndApply(edit);
                    removeObsoleteFiles();
                }

                // Publish the version before releasing the in-memory copy.
                _memtables->dropImmutable();
                error.clear();
                break;
            } catch (const std::exception& e) {
                error = e.what();
                if (attempt < kFlushMaxAttempts) {
                    std::this_thread::sleep_for(kFlushRetryBackoff * attempt);
                }
            }
        }

        if (!error.empty()) {
            _memtables->abandonFlush();
        }
        std::lock_guard<std::mutex> guard(_error_mutex);
        _flush_error = std::move(error);
    }

    // Writes every version of every key in `table` to a new table file and
    // fills in `meta`. Returns false when there is nothing to write.
    //
    // The frozen memtable is already in internal-key order.
    bool buildTable(const Table& table, FileMetaData& meta) {
        typename Table::Iterator it = table.cursor();
        if (!it.valid()) {
            return false;
        }

        const uint64_t              number = _versions.newFileNumber();
        const std::filesystem::path path   = tableFileName(_options.dir, number);

        SSTableBuilder builder(path, _options.sstable_block_size);
        for (; it.valid(); it.next()) {
            builder.add(it.key(), it.tag(), it.value());
        }
        const uint64_t size = builder.finish();

        meta.number       = number;
        meta.file_size    = size;
        meta.smallest_key = builder.smallestKey();
        meta.smallest_tag = builder.smallestTag();
        meta.largest_key  = builder.largestKey();
        meta.largest_tag  = builder.largestTag();
        return true;
    }

    // ── files ─────────────────────────────────────────────────────────────────

    WalHandle createLog() {
        const uint64_t              number = _versions.newFileNumber();
        const std::filesystem::path path   = logFileName(_options.dir, number);

        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "log create: " + path.string());
        }
        fileutil::syncDirectory(path);

        // WalWriter does not own its descriptor, so the shared_ptr's deleter
        // does: the log stays open exactly as long as something can still
        // write to it, and closes when the last writer lets go.
        std::shared_ptr<WalWriter> writer(new WalWriter(fd), [fd](WalWriter* w) {
            delete w;
            ::close(fd);
        });
        return { number, std::move(writer) };
    }

    // Removes obsolete recognized files while holding _manifest_mutex.
    void removeObsoleteFiles() {
        const std::set<uint64_t> live            = _versions.liveFiles();
        const uint64_t           log_number      = _versions.logNumber();
        const uint64_t           manifest_number = _versions.manifestNumber();

        std::error_code ec;
        std::filesystem::directory_iterator entries(_options.dir, ec);
        if (ec) {
            return;
        }

        for (const auto& entry : entries) {
            FileType type   = FileType::kUnknown;
            uint64_t number = 0;
            if (!parseFileName(entry.path().filename().string(), type, number)) {
                continue;
            }

            bool keep = true;
            switch (type) {
            case FileType::kCurrent:
                break;
            case FileType::kManifest:
                keep = number >= manifest_number;
                break;
            case FileType::kLog:
                // Everything below the recorded log number is in a table.
                keep = number >= log_number;
                break;
            case FileType::kTable:
                // Live means some Version still names it -- the current one, or
                // an older one a reader has not let go of yet.
                keep = live.contains(number);
                break;
            case FileType::kTemp:
                // Only ever a staging file for a CURRENT rename that did not
                // complete.
                keep = false;
                break;
            case FileType::kUnknown:
                break;
            }

            if (!keep) {
                std::error_code remove_ec;
                std::filesystem::remove(entry.path(), remove_ec);
            }
        }
    }

    Options    _options;
    VersionSet _versions;
    // Built during initialize(), once VersionSet can hand out file numbers.
    std::unique_ptr<Manager> _memtables;

    // Serializes manifest commits, which VersionSet requires to be
    // single-writer, and the obsolete-file sweep that reads the state they
    // install.
    std::mutex _manifest_mutex;

    std::mutex  _flush_mutex;
    std::thread _flush_thread;

    mutable std::mutex _error_mutex;
    std::string        _flush_error;
};
