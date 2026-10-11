#include "sstable/sstable_builder.h"

#include <cassert>
#include <cerrno>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <unistd.h>

#include "util/file_util.h"

SSTableBuilder::SSTableBuilder(std::filesystem::path path, size_t block_size)
    : _path(std::move(path)), _fd(-1), _block_size(block_size) {
    assert(block_size > 0);
    _fd = ::open(_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (_fd < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "SSTable create: " + _path.string());
    }
}

SSTableBuilder::~SSTableBuilder() {
    abandon();
}

void SSTableBuilder::writeAll(const void* buf, size_t n) {
    // Plain write, not pwrite: a builder appends sequentially and owns its fd,
    // so the file description's own offset is the right cursor to ride.
    fileutil::writeAll(_fd, buf, n, "SSTable write: " + _path.string());
    _offset += n;
}

void SSTableBuilder::writeBlock(const std::string& contents, BlockHandle& handle) {
    handle.offset = _offset;
    handle.size = static_cast<uint32_t>(contents.size());

    writeAll(contents.data(), contents.size());

    std::string trailer;
    putFixed32(trailer, blockChecksum(contents));
    writeAll(trailer.data(), trailer.size());
}

void SSTableBuilder::flushDataBlock() {
    if (_block.empty()) {
        return;
    }
    BlockHandle handle;
    writeBlock(_block, handle);

    // The index keys a block by its *last* entry, so a seek for some target
    // lands on the first block whose last key is >= that target -- which is
    // the only block the target can be in.
    std::string handle_bytes;
    appendBlockHandle(handle_bytes, handle);
    appendEntry(_index, _last_key, _last_tag, handle_bytes);

    _block.clear();
}

void SSTableBuilder::add(std::string_view key, uint64_t tag, std::string_view value) {
    assert(!_finished);
    assert(key.size() <= UINT32_MAX && value.size() <= UINT32_MAX);
    assert(_num_entries == 0 ||
           InternalKeyComparator{}(_last_key, _last_tag, key, tag) < 0);

    appendEntry(_block, key, tag, value);
    if (_num_entries == 0) {
        _first_key.assign(key);
        _first_tag = tag;
    }
    _last_key.assign(key);
    _last_tag = tag;
    ++_num_entries;

    // Cut the block only after the entry is in, so an entry larger than
    // _block_size gets a block of its own rather than being split.
    if (_block.size() >= _block_size) {
        flushDataBlock();
    }
}

uint64_t SSTableBuilder::finish() {
    assert(!_finished);
    flushDataBlock();

    BlockHandle index_handle;
    writeBlock(_index, index_handle);

    std::string footer;
    appendFooter(footer, { index_handle.offset, index_handle.size, _num_entries });
    writeAll(footer.data(), footer.size());

    fileutil::syncFile(_fd, "SSTable sync: " + _path.string());
    if (::close(_fd) != 0) {
        // Deferred write errors surface here, so a failing close means the
        // table is not on disk even though every write returned success.
        const int err = errno;
        _fd = -1;
        throw std::system_error(err, std::generic_category(),
                                "SSTable close: " + _path.string());
    }
    _fd       = -1;
    _finished = true;

    fileutil::syncDirectory(_path);
    return _offset;
}

void SSTableBuilder::abandon() noexcept {
    if (_finished) {
        return;
    }
    if (_fd >= 0) {
        ::close(_fd);
        _fd = -1;
    }
    std::error_code ec;
    std::filesystem::remove(_path, ec); // nothing useful to do if this fails
    _finished = true;
}
