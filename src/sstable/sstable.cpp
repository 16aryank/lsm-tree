#include "sstable/sstable.h"

#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

[[noreturn]] void corrupt(const std::filesystem::path& path, const std::string& what) {
    throw std::runtime_error("SSTable " + path.string() + ": " + what);
}

} // namespace

SSTable::SSTable(std::filesystem::path path, int fd) noexcept
    : _path(std::move(path)), _fd(fd) { }

SSTable::~SSTable() {
    if (_fd >= 0) {
        ::close(_fd);
    }
}

std::shared_ptr<SSTable> SSTable::open(std::filesystem::path path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "SSTable open: " + path.string());
    }
    // Owned from here on, so a throw out of loadIndex() still closes the fd.
    std::shared_ptr<SSTable> table(new SSTable(std::move(path), fd));
    table->loadIndex();
    return table;
}

void SSTable::readAll(void* buf, size_t n, uint64_t offset) const {
    auto* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        const ssize_t got = ::pread(_fd, p, n, static_cast<off_t>(offset));
        if (got < 0) {
            if (errno == EINTR) { continue; }
            throw std::system_error(errno, std::generic_category(),
                                    "SSTable pread: " + _path.string());
        }
        if (got == 0) {
            corrupt(_path, "unexpected end of file");
        }
        p += got;
        offset += static_cast<uint64_t>(got);
        n -= static_cast<size_t>(got);
    }
}

std::string SSTable::readBlock(const BlockHandle& handle) const {
    std::string buffer(static_cast<size_t>(handle.size) + kBlockTrailerSize, '\0');
    readAll(buffer.data(), buffer.size(), handle.offset);

    const uint32_t expected = decodeFixed32(buffer.data() + handle.size);
    buffer.resize(handle.size);
    if (blockChecksum(buffer) != expected) {
        corrupt(_path, "block checksum mismatch at offset " + std::to_string(handle.offset));
    }
    return buffer;
}

void SSTable::loadIndex() {
    struct stat st { };
    if (::fstat(_fd, &st) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "SSTable fstat: " + _path.string());
    }
    _file_size = static_cast<uint64_t>(st.st_size);

    // Smallest legal file: an empty index block, its trailer, and the footer.
    if (_file_size < kFooterSize + kBlockTrailerSize) {
        corrupt(_path, "file too small to be an SSTable");
    }

    char footer_bytes[kFooterSize];
    readAll(footer_bytes, kFooterSize, _file_size - kFooterSize);

    SSTableFooter footer;
    if (!decodeFooter({ footer_bytes, kFooterSize }, footer)) {
        // Also what a build that died before writing its footer looks like.
        corrupt(_path, "bad magic number in footer");
    }

    // The index block must sit immediately before the footer, and the data
    // blocks entirely before the index.
    if (footer.index_offset > _file_size || footer.index_size > _file_size ||
        footer.index_offset + footer.index_size + kBlockTrailerSize != _file_size - kFooterSize) {
        corrupt(_path, "index block does not fit the file");
    }
    _num_entries = footer.num_entries;

    const std::string index_block =
        readBlock({ footer.index_offset, static_cast<uint32_t>(footer.index_size) });

    size_t           offset = 0;
    SSTableEntryView entry;
    while (offset < index_block.size()) {
        const size_t next = decodeEntry(index_block, offset, entry);
        if (next == 0) {
            corrupt(_path, "truncated index entry");
        }
        BlockHandle handle;
        if (!decodeBlockHandle(entry.value, handle)) {
            corrupt(_path, "malformed block handle in index");
        }
        // A handle pointing at (or past) the index would let a lookup read
        // the wrong region entirely, so bound it against the data area.
        if (handle.offset + handle.size + kBlockTrailerSize > footer.index_offset) {
            corrupt(_path, "data block outside the data region");
        }
        _index.push_back({ std::string(entry.key), entry.tag, handle });
        offset = next;
    }

    if (_index.empty()) {
        return;
    }
    _largest_key = _index.back().key;

    // The smallest key is the first entry of the first block; one extra read
    // at open time, and the bound is wanted for every overlap check later.
    const std::string first_block = readBlock(_index.front().handle);
    if (decodeEntry(first_block, 0, entry) == 0) {
        corrupt(_path, "truncated first data block");
    }
    _smallest_key.assign(entry.key);
}

std::optional<SSTable::Entry> SSTable::get(std::string_view key, uint64_t seq) const {
    if (_index.empty()) {
        return std::nullopt;
    }
    const InternalKeyComparator compare;
    // Tags sort descending within a key, so the target is the *newest* version
    // the caller is allowed to see; anything newer sorts before it.
    const uint64_t target_tag = packSeqAndType(seq, ValueType::kValue);

    // Blocks are keyed by their last entry, so the first block whose last key
    // is >= the target is the only one that can hold it.
    const auto block = std::partition_point(
        _index.begin(), _index.end(), [&](const IndexEntry& e) {
            return compare(e.key, e.tag, key, target_tag) < 0;
        });
    if (block == _index.end()) {
        return std::nullopt;
    }

    const std::string contents = readBlock(block->handle);
    size_t            offset   = 0;
    SSTableEntryView  entry;
    while (offset < contents.size()) {
        const size_t next = decodeEntry(contents, offset, entry);
        if (next == 0) {
            corrupt(_path, "truncated data entry");
        }
        if (compare(entry.key, entry.tag, key, target_tag) >= 0) {
            // First entry at or after the target: a hit only if it is still
            // the same user key, otherwise the key has no version here.
            if (entry.key != key) {
                return std::nullopt;
            }
            return Entry{ unpackSeqNumber(entry.tag), unpackValueType(entry.tag),
                          std::string(entry.value) };
        }
        offset = next;
    }
    return std::nullopt;
}

SSTable::Iterator::Iterator(const SSTable* table) : _table(table) {
    loadBlock(0);
}

void SSTable::Iterator::loadBlock(size_t index) {
    for (size_t block = index; block < _table->_index.size(); ++block) {
        _buffer      = _table->readBlock(_table->_index[block].handle);
        _block_index = block;

        const size_t next = decodeEntry(_buffer, 0, _current);
        if (next == 0) {
            if (_buffer.empty()) { continue; } // no entries here; try the next
            corrupt(_table->_path, "truncated data entry");
        }
        _offset = next;
        _valid  = true;
        return;
    }
    _buffer.clear();
    _current = { };
    _valid   = false;
}

void SSTable::Iterator::next() {
    if (!_valid) {
        return;
    }
    if (_offset >= _buffer.size()) {
        loadBlock(_block_index + 1);
        return;
    }
    const size_t next = decodeEntry(_buffer, _offset, _current);
    if (next == 0) {
        corrupt(_table->_path, "truncated data entry");
    }
    _offset = next;
}
