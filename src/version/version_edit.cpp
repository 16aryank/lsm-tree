#include "version/version_edit.h"

#include "coding.h"

namespace {

void putTag(std::string& dst, EditTag tag) {
    putVarint32(dst, static_cast<uint32_t>(tag));
}

// Levels are small non-negative integers; anything else means the record was
// not written by this format, so the decoder rejects rather than clamps.
[[nodiscard]] bool getLevel(std::string_view& src, int& out) noexcept {
    uint32_t level = 0;
    if (!getVarint32(src, level) || level >= static_cast<uint32_t>(kNumLevels)) {
        return false;
    }
    out = static_cast<int>(level);
    return true;
}

[[nodiscard]] bool getString(std::string_view& src, std::string& out) {
    std::string_view view;
    if (!getLengthPrefixed(src, view)) {
        return false;
    }
    out.assign(view);
    return true;
}

} // namespace

std::string VersionEdit::encode() const {
    std::string out;

    if (_log_number) {
        putTag(out, EditTag::kLogNumber);
        putVarint64(out, *_log_number);
    }
    if (_next_file_number) {
        putTag(out, EditTag::kNextFileNumber);
        putVarint64(out, *_next_file_number);
    }
    if (_last_sequence) {
        putTag(out, EditTag::kLastSequence);
        putVarint64(out, *_last_sequence);
    }

    // Deletions before additions, matching the order a builder must apply them
    // in: a compaction can retire a file number and, in principle, an outer
    // layer could mint the same number again, and applying the two the other
    // way round would drop the new file instead of the old one.
    for (const auto& [level, number] : _deleted) {
        putTag(out, EditTag::kDeletedFile);
        putVarint32(out, static_cast<uint32_t>(level));
        putVarint64(out, number);
    }

    for (const auto& [level, meta] : _added) {
        putTag(out, EditTag::kNewFile);
        putVarint32(out, static_cast<uint32_t>(level));
        putVarint64(out, meta.number);
        putVarint64(out, meta.file_size);
        putLengthPrefixed(out, meta.smallest_key);
        putVarint64(out, meta.smallest_tag);
        putLengthPrefixed(out, meta.largest_key);
        putVarint64(out, meta.largest_tag);
    }

    return out;
}

bool VersionEdit::decode(std::string_view src) noexcept try {
    clear();

    while (!src.empty()) {
        uint32_t raw_tag = 0;
        if (!getVarint32(src, raw_tag)) {
            clear();
            return false;
        }

        bool ok = true;
        switch (static_cast<EditTag>(raw_tag)) {
        case EditTag::kLogNumber: {
            uint64_t value = 0;
            // A repeated scalar is ambiguous about which one wins, so it is
            // treated as corruption rather than last-write-wins.
            ok = !_log_number && getVarint64(src, value);
            if (ok) { _log_number = value; }
            break;
        }
        case EditTag::kNextFileNumber: {
            uint64_t value = 0;
            ok = !_next_file_number && getVarint64(src, value);
            if (ok) { _next_file_number = value; }
            break;
        }
        case EditTag::kLastSequence: {
            uint64_t value = 0;
            ok = !_last_sequence && getVarint64(src, value);
            if (ok) { _last_sequence = value; }
            break;
        }
        case EditTag::kDeletedFile: {
            int      level  = 0;
            uint64_t number = 0;
            ok = getLevel(src, level) && getVarint64(src, number);
            if (ok) { _deleted.emplace_back(level, number); }
            break;
        }
        case EditTag::kNewFile: {
            int          level = 0;
            FileMetaData meta;
            ok = getLevel(src, level) &&
                 getVarint64(src, meta.number) &&
                 getVarint64(src, meta.file_size) &&
                 getString(src, meta.smallest_key) &&
                 getVarint64(src, meta.smallest_tag) &&
                 getString(src, meta.largest_key) &&
                 getVarint64(src, meta.largest_tag);
            if (ok) { _added.emplace_back(level, std::move(meta)); }
            break;
        }
        default:
            // An unknown tag is not skippable: its payload length is part of
            // what the tag means, so the decoder cannot find where the next
            // field starts, let alone judge whether ignoring this one is safe.
            ok = false;
            break;
        }

        if (!ok) {
            clear();
            return false;
        }
    }

    return true;
} catch (...) {
    // getLengthPrefixed bounds-checks its length against what is left, so a
    // corrupt prefix cannot ask for an enormous string -- but copying the two
    // keys and growing the two vectors still allocate, and decode() is noexcept
    // because its callers treat a bad record as data, not as an exception.
    clear();
    return false;
}
