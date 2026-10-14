#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <map>
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
#include "iterator/concatenating_iterator.h"
#include "iterator/merging_iterator.h"
#include "iterator/sstable_iterator.h"
#include "key.h"
#include "memtable/memtable.h"
#include "memtable/memtable_manager.h"
#include "sstable/sstable_builder.h"
#include "util/file_util.h"
#include "version/version.h"
#include "version/compaction.h"
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

struct CompactionStats {
    uint64_t input_bytes = 0;
    uint64_t output_bytes = 0;
    uint64_t input_files = 0;
    uint64_t runs = 0;
    std::array<uint64_t, kNumLevels> runs_by_level{};
    std::chrono::nanoseconds elapsed{};
};

// Coordinates the memtable, WAL, SSTables, and manifest. WAL appends precede
// memtable inserts; ordinary writes do not sync the WAL before returning.
template <typename UserKey_, typename Value_>
    requires ByteStorable<UserKey_> && ByteStorable<Value_>
class DB {
    struct SnapshotRegistry {
        std::mutex mutex;
        std::map<uint64_t, size_t> active;
    };

public:
    using Manager = MemTableManager<UserKey_, Value_>;
    using Table   = MemTable<UserKey_, Value_>;

    // A registered read sequence. Releasing the last handle for a sequence
    // lets later compactions discard versions it alone needed.
    class Snapshot {
    public:
        Snapshot() = default;
        Snapshot(const Snapshot&) = delete;
        Snapshot& operator=(const Snapshot&) = delete;

        Snapshot(Snapshot&& other) noexcept
            : _registry(std::move(other._registry)), _sequence(other._sequence) { }

        Snapshot& operator=(Snapshot&& other) noexcept {
            if (this != &other) {
                reset();
                _registry = std::move(other._registry);
                _sequence = other._sequence;
            }
            return *this;
        }

        ~Snapshot() { reset(); }

        [[nodiscard]] uint64_t sequence() const noexcept { return _sequence; }
        [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(_registry); }

        void reset() noexcept {
            if (!_registry) {
                return;
            }
            auto registry = std::move(_registry);
            std::lock_guard<std::mutex> guard(registry->mutex);
            auto it = registry->active.find(_sequence);
            if (--it->second == 0) {
                registry->active.erase(it);
            }
        }

    private:
        friend class DB;
        Snapshot(std::shared_ptr<SnapshotRegistry> registry, uint64_t sequence)
            : _registry(std::move(registry)), _sequence(sequence) { }

        std::shared_ptr<SnapshotRegistry> _registry;
        uint64_t _sequence = 0;
    };

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
        {
            std::lock_guard<std::mutex> guard(_flush_mutex);
            if (_flush_thread.joinable()) {
                _flush_thread.join();
            }
        }
        {
            std::lock_guard<std::mutex> guard(_compaction_mutex);
            _stop_compaction = true;
        }
        _compaction_cv.notify_all();
        if (_compaction_thread.joinable()) {
            _compaction_thread.join();
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
        const Snapshot read = snapshot();
        return getAtSequence(key, read.sequence());
    }

    // A handle from another DB instance, or one already released, is invalid.
    [[nodiscard]] std::optional<Value_> get(const UserKey_& key,
                                              const Snapshot& snapshot) const {
        if (!snapshot._registry || snapshot._registry != _snapshots) {
            throw std::invalid_argument("snapshot is not active for this DB");
        }
        return getAtSequence(key, snapshot.sequence());
    }

    [[nodiscard]] Snapshot snapshot() const {
        std::lock_guard<std::mutex> write_guard(_write_mutex);
        const uint64_t sequence = _versions.lastSequence();
        std::lock_guard<std::mutex> snapshot_guard(_snapshots->mutex);
        ++_snapshots->active[sequence];
        return Snapshot(_snapshots, sequence);
    }

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

    void waitForPendingCompaction() {
        waitForPendingFlush();
        std::unique_lock<std::mutex> lock(_compaction_mutex);
        _compaction_cv.wait(lock, [this] {
            return !_compaction_requested && !_compaction_running;
        });
    }

    [[nodiscard]] std::string lastCompactionError() const {
        std::lock_guard<std::mutex> guard(_compaction_mutex);
        return _compaction_error;
    }

    [[nodiscard]] CompactionStats compactionStats() const {
        std::lock_guard<std::mutex> guard(_compaction_mutex);
        return _compaction_stats;
    }

    [[nodiscard]] const Options& options() const noexcept { return _options; }
    [[nodiscard]] std::shared_ptr<const Version> currentVersion() const {
        return _versions.current();
    }

private:
    [[nodiscard]] std::optional<Value_> getAtSequence(const UserKey_& key,
                                                       uint64_t sequence) const {
        Value_ value;
        switch (_memtables->lookup(key, sequence, value)) {
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
            version->get(std::string_view(key.data(), key.size()), sequence);
        if (!entry || entry->type == ValueType::kDelete) {
            return std::nullopt;
        }
        return Value_(entry->value.data(), entry->value.size());
    }
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
            // Clear crash leftovers before replay allocates new table numbers.
            removeObsoleteFiles();
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
        _compaction_thread = std::thread([this] { compactionWorker(); });
        requestCompaction();
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
                _versions.advanceFileNumberPast(number);
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
        if (meta.number != 0) {
            _pending_tables.erase(meta.number);
        }
    }

    // ── write path ────────────────────────────────────────────────────────────

    void write(ValueType type, const UserKey_& key, Value_ value) {
        {
            std::lock_guard<std::mutex> guard(_write_mutex);
            const uint64_t seq = _versions.newSequence();
            _memtables->add(seq, type, key, std::move(value));
        }

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
            std::optional<uint64_t> output_number;
            try {
                VersionEdit  edit;
                FileMetaData meta;
                if (buildTable(*promotion.table, meta)) {
                    edit.addFile(0, meta);
                    output_number = meta.number;
                }
                // The new log number retires logs covered by this flush.
                edit.setLogNumber(promotion.log_number);

                {
                    std::lock_guard<std::mutex> guard(_manifest_mutex);
                    _versions.logAndApply(edit);
                    if (output_number) {
                        _pending_tables.erase(*output_number);
                    }
                    removeObsoleteFiles();
                }

                // Publish the version before releasing the in-memory copy.
                _memtables->dropImmutable();
                error.clear();
                requestCompaction();
                break;
            } catch (const std::exception& e) {
                error = e.what();
                if (output_number) {
                    std::lock_guard<std::mutex> guard(_manifest_mutex);
                    if (!_versions.manifestFailed()) {
                        _pending_tables.erase(*output_number);
                        std::error_code ec;
                        std::filesystem::remove(tableFileName(_options.dir, *output_number), ec);
                    }
                }
                if (_versions.manifestFailed()) {
                    break;
                }
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
        {
            std::lock_guard<std::mutex> guard(_manifest_mutex);
            _pending_tables.insert(number);
        }
        try {
            SSTableBuilder builder(path, _options.sstable_block_size);
            for (; it.valid(); it.next()) {
                builder.add(it.key(), it.tag(), it.value());
            }
            meta.number       = number;
            meta.file_size    = builder.finish();
            meta.smallest_key = builder.smallestKey();
            meta.smallest_tag = builder.smallestTag();
            meta.largest_key  = builder.largestKey();
            meta.largest_tag  = builder.largestTag();
        } catch (...) {
            std::lock_guard<std::mutex> guard(_manifest_mutex);
            _pending_tables.erase(number);
            throw;
        }
        return true;
    }

    void requestCompaction() {
        {
            std::lock_guard<std::mutex> guard(_compaction_mutex);
            _compaction_requested = true;
        }
        _compaction_cv.notify_all();
    }

    void compactionWorker() {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(_compaction_mutex);
                _compaction_cv.wait(lock, [this] {
                    return _stop_compaction || _compaction_requested;
                });
                if (_stop_compaction) {
                    return;
                }
                _compaction_requested = false;
                _compaction_running = true;
            }

            std::string error;
            for (size_t attempt = 1; attempt <= kFlushMaxAttempts; ++attempt) {
                try {
                    waitForPendingFlush();
                    if (auto plan = _compaction_picker.pick(_versions.current())) {
                        executeCompaction(*plan);
                    }
                    error.clear();
                    break;
                } catch (const std::exception& e) {
                    error = e.what();
                    if (_versions.manifestFailed() || attempt == kFlushMaxAttempts) {
                        break;
                    }
                    std::this_thread::sleep_for(kFlushRetryBackoff * attempt);
                }
            }

            {
                std::lock_guard<std::mutex> guard(_compaction_mutex);
                _compaction_error = std::move(error);
                _compaction_running = false;
                if (_compaction_error.empty() && _versions.current()->compactionScore() >= 1.0) {
                    _compaction_requested = true;
                }
            }
            _compaction_cv.notify_all();
        }
    }

    [[nodiscard]] bool planStillCurrent(const CompactionPlan& plan) const {
        const auto current = _versions.current();
        const auto matches = [](const FileHandle& selected, const std::vector<FileHandle>& files) {
            const auto found = std::find_if(files.begin(), files.end(), [&](const FileHandle& file) {
                return file.meta.number == selected.meta.number;
            });
            return found != files.end() && found->meta.file_size == selected.meta.file_size &&
                   found->meta.smallest_key == selected.meta.smallest_key &&
                   found->meta.smallest_tag == selected.meta.smallest_tag &&
                   found->meta.largest_key == selected.meta.largest_key &&
                   found->meta.largest_tag == selected.meta.largest_tag;
        };
        for (const auto& file : plan.source) {
            if (!matches(file, current->files(plan.source_level))) {
                return false;
            }
        }
        for (const auto& file : plan.destination) {
            if (!matches(file, current->files(plan.source_level + 1))) {
                return false;
            }
        }
        const auto overlaps = current->overlappingInputs(plan.source_level + 1,
                                                         plan.smallest_key, plan.largest_key);
        return overlaps.size() == plan.destination.size();
    }

    [[nodiscard]] uint64_t compactionCutoff() const {
        std::lock_guard<std::mutex> write_guard(_write_mutex);
        std::lock_guard<std::mutex> snapshot_guard(_snapshots->mutex);
        return _snapshots->active.empty() ? _versions.lastSequence()
                                          : _snapshots->active.begin()->first;
    }

    [[nodiscard]] static bool mayContainBelow(const Version& version, int output_level,
                                               std::string_view key) {
        for (int level = output_level + 1; level < kNumLevels; ++level) {
            const auto& files = version.files(level);
            const auto file = std::partition_point(files.begin(), files.end(),
                                                   [key](const FileHandle& candidate) {
                return candidate.meta.largest_key < key;
            });
            if (file != files.end() && file->meta.smallest_key <= key) {
                return true;
            }
        }
        return false;
    }

    void executeCompaction(CompactionPlan& plan) {
        const auto started = std::chrono::steady_clock::now();
        const uint64_t cutoff = compactionCutoff();
        std::vector<std::unique_ptr<InternalIterator>> children;
        if (plan.source_level == 0) {
            for (const auto& file : plan.source) {
                children.push_back(std::make_unique<SSTableIterator>(file.table));
            }
        } else {
            std::vector<std::shared_ptr<const SSTable>> tables;
            for (const auto& file : plan.source) {
                tables.push_back(file.table);
            }
            children.push_back(newConcatenatingIterator(std::move(tables)));
        }
        if (!plan.destination.empty()) {
            std::vector<std::shared_ptr<const SSTable>> tables;
            for (const auto& file : plan.destination) {
                tables.push_back(file.table);
            }
            children.push_back(newConcatenatingIterator(std::move(tables)));
        }
        MergingIterator merge(std::move(children));

        std::vector<uint64_t> numbers;
        std::vector<FileMetaData> outputs;
        std::unique_ptr<SSTableBuilder> builder;
        uint64_t active_number = 0;
        bool committed = false;

        const auto startOutput = [&] {
            active_number = _versions.newFileNumber();
            {
                std::lock_guard<std::mutex> guard(_manifest_mutex);
                _pending_tables.insert(active_number);
            }
            numbers.push_back(active_number);
            builder = std::make_unique<SSTableBuilder>(
                tableFileName(_options.dir, active_number), _options.sstable_block_size);
        };
        const auto finishOutput = [&] {
            FileMetaData meta;
            meta.number = active_number;
            meta.file_size = builder->finish();
            meta.smallest_key = builder->smallestKey();
            meta.smallest_tag = builder->smallestTag();
            meta.largest_key = builder->largestKey();
            meta.largest_tag = builder->largestTag();
            outputs.push_back(std::move(meta));
            builder.reset();
        };

        try {
            std::string input_key;
            std::string output_key;
            uint64_t input_tag = 0;
            bool have_input = false;
            bool retained_at_cutoff = false;
            bool deeper_overlap = false;
            while (merge.valid()) {
                const std::string_view key = merge.key();
                const uint64_t tag = merge.tag();
                const bool new_key = !have_input || key != input_key;
                if (new_key) {
                    retained_at_cutoff = false;
                    deeper_overlap = mayContainBelow(*plan.base, plan.source_level + 1, key);
                }
                const bool duplicate = have_input && !new_key && tag == input_tag;
                input_key.assign(key);
                input_tag = tag;
                have_input = true;
                if (duplicate) {
                    merge.next();
                    continue;
                }

                bool retain = unpackSeqNumber(tag) > cutoff;
                if (!retain && !retained_at_cutoff) {
                    retained_at_cutoff = true;
                    retain = unpackValueType(tag) != ValueType::kDelete || deeper_overlap;
                }
                if (!retain) {
                    merge.next();
                    continue;
                }
                if (builder && key != output_key &&
                    builder->estimatedSize() >= _options.compaction_output_file_size) {
                    finishOutput();
                }
                if (!builder) {
                    startOutput();
                }
                builder->add(key, tag, merge.value());
                output_key.assign(key);
                merge.next();
            }
            if (builder) {
                finishOutput();
            }

            VersionEdit edit;
            for (const auto& file : plan.source) {
                edit.deleteFile(plan.source_level, file.meta.number);
            }
            for (const auto& file : plan.destination) {
                edit.deleteFile(plan.source_level + 1, file.meta.number);
            }
            for (const auto& meta : outputs) {
                edit.addFile(plan.source_level + 1, meta);
            }
            {
                std::lock_guard<std::mutex> guard(_manifest_mutex);
                if (!planStillCurrent(plan)) {
                    throw std::runtime_error("compaction inputs changed before commit");
                }
                _versions.logAndApply(edit);
                committed = true;
                for (const uint64_t number : numbers) {
                    _pending_tables.erase(number);
                }
            }
        } catch (...) {
            builder.reset();
            std::lock_guard<std::mutex> guard(_manifest_mutex);
            if (!_versions.manifestFailed()) {
                for (const uint64_t number : numbers) {
                    _pending_tables.erase(number);
                    std::error_code ec;
                    std::filesystem::remove(tableFileName(_options.dir, number), ec);
                }
            }
            throw;
        }

        if (committed) {
            plan.base.reset();
            std::lock_guard<std::mutex> guard(_manifest_mutex);
            removeObsoleteFiles();
        }

        uint64_t input_bytes = 0;
        for (const auto& file : plan.source) {
            input_bytes += file.meta.file_size;
        }
        for (const auto& file : plan.destination) {
            input_bytes += file.meta.file_size;
        }
        uint64_t output_bytes = 0;
        for (const auto& meta : outputs) {
            output_bytes += meta.file_size;
        }
        {
            std::lock_guard<std::mutex> guard(_compaction_mutex);
            _compaction_stats.input_bytes += input_bytes;
            _compaction_stats.output_bytes += output_bytes;
            _compaction_stats.input_files += plan.source.size() + plan.destination.size();
            ++_compaction_stats.runs;
            ++_compaction_stats.runs_by_level[static_cast<size_t>(plan.source_level)];
            _compaction_stats.elapsed += std::chrono::steady_clock::now() - started;
        }
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
        if (_versions.manifestFailed()) {
            return;
        }
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
                keep = live.contains(number) || _pending_tables.contains(number);
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

    mutable std::mutex _write_mutex;
    std::shared_ptr<SnapshotRegistry> _snapshots = std::make_shared<SnapshotRegistry>();

    // Serializes manifest commits, which VersionSet requires to be
    // single-writer, and the obsolete-file sweep that reads the state they
    // install.
    std::mutex _manifest_mutex;
    std::set<uint64_t> _pending_tables;

    std::mutex  _flush_mutex;
    std::thread _flush_thread;

    mutable std::mutex _error_mutex;
    std::string        _flush_error;

    CompactionPicker _compaction_picker;
    mutable std::mutex _compaction_mutex;
    std::condition_variable _compaction_cv;
    std::thread _compaction_thread;
    bool _compaction_requested{ false };
    bool _compaction_running{ false };
    bool _stop_compaction{ false };
    std::string _compaction_error;
    CompactionStats _compaction_stats;
};
