#include "sstable/sstable.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/file_util.h"

namespace {

[[noreturn]] void corrupt(const std::filesystem::path& path, const std::string& what) {
    throw std::runtime_error("SSTable " + path.string() + ": " + what);
}

bool blockEndsAt(const BlockHandle& handle, uint64_t end) noexcept {
    return handle.offset <= end &&
           static_cast<uint64_t>(handle.size) + kBlockTrailerSize == end - handle.offset;
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
    // A table is a fixed, fully written artifact -- unlike the WAL, a short
    // read here is never a torn tail, it means the file is not the table the
    // index claims it is.
    const size_t got = fileutil::preadSome(_fd, buf, n, offset,
                                           "SSTable pread: " + _path.string());
    if (got < n) {
        corrupt(_path, "unexpected end of file");
    }
}

std::string SSTable::readBlock(const BlockHandle& handle) const {
    if (static_cast<uint64_t>(handle.size) + kBlockTrailerSize >
        std::numeric_limits<size_t>::max()) {
        corrupt(_path, "block too large to read");
    }
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
    if (st.st_size < 0) {
        corrupt(_path, "negative file size");
    }
    _file_size = static_cast<uint64_t>(st.st_size);

    if (_file_size < sizeof(uint64_t)) {
        corrupt(_path, "file too small to be an SSTable");
    }

    char magic_bytes[sizeof(uint64_t)];
    readAll(magic_bytes, sizeof(magic_bytes), _file_size - sizeof(magic_bytes));
    const uint64_t magic = decodeFixed64(magic_bytes);
    if (magic != kSSTableMagic && magic != kSSTableMagicV2) {
        corrupt(_path, "bad magic number in footer");
    }
    const size_t footer_size = magic == kSSTableMagicV2 ? kFooterV2Size : kFooterSize;
    if (_file_size < footer_size + kBlockTrailerSize) {
        corrupt(_path, "file too small to be an SSTable");
    }
    const uint64_t footer_start = _file_size - footer_size;
    char footer_bytes[kFooterV2Size];
    readAll(footer_bytes, footer_size, footer_start);

    BlockHandle index_handle;
    uint64_t data_end = 0;
    if (magic == kSSTableMagicV2) {
        SSTableFooterV2 footer;
        if (!decodeFooterV2({ footer_bytes, footer_size }, footer)) {
            corrupt(_path, "malformed v2 footer");
        }
        if (!blockEndsAt(footer.index, footer_start) ||
            !blockEndsAt(footer.filter, footer.index.offset) ||
            footer.filter.size < kBloomHeaderSize) {
            corrupt(_path, "filter or index block outside its region");
        }
        data_end = footer.filter.offset;
        index_handle = footer.index;
        _num_entries = footer.num_entries;

        const std::string filter_payload = readBlock(footer.filter);
        _filter = BloomFilter::decode(filter_payload);
        if (!_filter) {
            corrupt(_path, "malformed Bloom filter payload");
        }
        if ((_num_entries == 0) != (_filter->bitCount() == 0)) {
            corrupt(_path, "filter emptiness disagrees with entry count");
        }
    } else {
        SSTableFooter footer;
        if (!decodeFooter({ footer_bytes, footer_size }, footer) ||
            footer.index_size > UINT32_MAX) {
            corrupt(_path, "malformed legacy footer");
        }
        index_handle = { footer.index_offset, static_cast<uint32_t>(footer.index_size) };
        if (!blockEndsAt(index_handle, footer_start)) {
            corrupt(_path, "index block does not fit the file");
        }
        data_end = index_handle.offset;
        _num_entries = footer.num_entries;
    }

    const std::string index_block = readBlock(index_handle);

    size_t           offset = 0;
    uint64_t         next_data_offset = 0;
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
        // Builder output has consecutive data blocks from byte zero through
        // the filter (v2) or index (legacy), with no overlap or hidden gap.
        if (handle.offset != next_data_offset || handle.offset > data_end ||
            static_cast<uint64_t>(handle.size) + kBlockTrailerSize >
                data_end - handle.offset) {
            corrupt(_path, "data block outside the data region or out of order");
        }
        next_data_offset = handle.offset + static_cast<uint64_t>(handle.size) +
                           kBlockTrailerSize;
        _index.push_back({ std::string(entry.key), entry.tag, handle });
        offset = next;
    }

    if (next_data_offset != data_end || (_index.empty() != (_num_entries == 0))) {
        corrupt(_path, "data region does not match index and entry count");
    }

    if (_index.empty()) {
        return;
    }
    _largest_key = _index.back().key;
    _largest_tag = _index.back().tag;

    // The smallest key is the first entry of the first block; one extra read
    // at open time, and the bound is wanted for every overlap check later.
    const std::string first_block = readBlock(_index.front().handle);
    if (decodeEntry(first_block, 0, entry) == 0) {
        corrupt(_path, "truncated first data block");
    }
    _smallest_key.assign(entry.key);
    _smallest_tag = entry.tag;
}

size_t SSTable::findBlock(std::string_view key, uint64_t tag) const noexcept {
    const InternalKeyComparator compare;
    const auto block = std::partition_point(
        _index.begin(), _index.end(), [&](const IndexEntry& e) {
            return compare(e.key, e.tag, key, tag) < 0;
        });
    return static_cast<size_t>(block - _index.begin());
}

std::optional<SSTable::Entry> SSTable::get(std::string_view key, uint64_t seq) const {
    if (_index.empty()) {
        return std::nullopt;
    }
    const InternalKeyComparator compare;
    // Tags sort descending within a key, so the target is the *newest* version
    // the caller is allowed to see; anything newer sorts before it.
    const uint64_t target_tag = packSeqAndType(seq, ValueType::kValue);

    const size_t block_index = findBlock(key, target_tag);
    if (block_index == _index.size()) {
        return std::nullopt;
    }

    const std::string contents = readBlock(_index[block_index].handle);
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
        _buffer = _table->readBlock(_table->_index[block].handle);
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
    _valid = false;
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

void SSTable::Iterator::seek(std::string_view key, uint64_t tag) {
    const size_t block_index = _table->findBlock(key, tag);
    if (block_index == _table->_index.size()) {
        // Every entry in the table sorts before the target.
        _buffer.clear();
        _current = { };
        _valid   = false;
        return;
    }

    loadBlock(block_index);
    if (!_valid) {
        return;
    }

    // loadBlock leaves the cursor on the block's first entry; walk forward to
    // the first one at or after the target. The index guarantees the block's
    // last entry is >= the target, so this cannot run off the end -- except
    // when loadBlock skipped an empty block and landed on a later one, whose
    // first entry is already past the target and is the right answer anyway.
    const InternalKeyComparator compare;
    while (_valid && compare(_current.key, _current.tag, key, tag) < 0) {
        next();
    }
}
