#include "sstable/sstable_builder.h"

#include <cassert>
#include <cerrno>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <unistd.h>

namespace {

// fdatasync is not in MacOS
int syncFile(int fd) {
#if defined(__APPLE__)
    return ::fsync(fd);
#else
    return ::fdatasync(fd);
#endif
}

// Makes the directory entry for a newly created file durable. Without this a
// crash can leave a fully fsynced table that the directory has never heard of.
// Best effort: a filesystem that refuses to open a directory read-only is not
// a reason to fail the flush.
void syncDirectory(const std::filesystem::path& file) {
    const std::filesystem::path dir = file.parent_path();
    const int fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY);
    if (fd < 0) {
        return;
    }
    ::fsync(fd);
    ::close(fd);
}

} // namespace

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
    const auto* p = static_cast<const uint8_t*>(buf);
    while (n > 0) {
        const ssize_t written = ::write(_fd, p, n);
        if (written < 0) {
            // A signal arrived before anything was written; nothing to undo.
            if (errno == EINTR) { continue; }
            throw std::system_error(errno, std::generic_category(),
                                    "SSTable write: " + _path.string());
        }
        p += written;
        _offset += static_cast<uint64_t>(written);
        n -= static_cast<size_t>(written);
    }
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

    if (syncFile(_fd) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "SSTable sync: " + _path.string());
    }
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

    syncDirectory(_path);
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
