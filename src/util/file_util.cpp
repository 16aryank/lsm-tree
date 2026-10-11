#include "util/file_util.h"

#include <cerrno>
#include <cstdint>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <unistd.h>

namespace fileutil {
namespace {

[[noreturn]] void fail(int err, std::string_view what) {
    throw std::system_error(err, std::generic_category(), std::string(what));
}

} // namespace

void syncFile(int fd, std::string_view what) {
#if defined(__APPLE__)
    const int rc = ::fsync(fd);
#else
    const int rc = ::fdatasync(fd);
#endif
    if (rc != 0) {
        fail(errno, what);
    }
}

void syncDirectory(const std::filesystem::path& file) noexcept {
    const std::filesystem::path dir = file.parent_path();
    const int fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY);
    if (fd < 0) {
        return;
    }
    ::fsync(fd);
    ::close(fd);
}

void pwriteAll(int fd, const void* buf, size_t n, uint64_t offset, std::string_view what) {
    const auto* p = static_cast<const uint8_t*>(buf);
    while (n > 0) {
        const ssize_t written = ::pwrite(fd, p, n, static_cast<off_t>(offset));
        if (written < 0) {
            // A signal arrived before anything was written; nothing to undo.
            if (errno == EINTR) { continue; }
            fail(errno, what);
        }
        p += written;
        offset += static_cast<uint64_t>(written);
        n -= static_cast<size_t>(written);
    }
}

void writeAll(int fd, const void* buf, size_t n, std::string_view what) {
    const auto* p = static_cast<const uint8_t*>(buf);
    while (n > 0) {
        const ssize_t written = ::write(fd, p, n);
        if (written < 0) {
            if (errno == EINTR) { continue; }
            fail(errno, what);
        }
        p += written;
        n -= static_cast<size_t>(written);
    }
}

size_t preadSome(int fd, void* buf, size_t n, uint64_t offset, std::string_view what) {
    auto*  p    = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < n) {
        const ssize_t r = ::pread(fd, p + done, n - done, static_cast<off_t>(offset + done));
        if (r < 0) {
            if (errno == EINTR) { continue; }
            fail(errno, what);
        }
        if (r == 0) {
            break; // end of file
        }
        done += static_cast<size_t>(r);
    }
    return done;
}

void writeFileAtomic(const std::filesystem::path& path, std::string_view contents,
                     const std::filesystem::path& tmp) {
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail(errno, "create: " + tmp.string());
    }

    try {
        writeAll(fd, contents.data(), contents.size(), "write: " + tmp.string());
        syncFile(fd, "sync: " + tmp.string());
    } catch (...) {
        ::close(fd);
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        throw;
    }

    if (::close(fd) != 0) {
        // Deferred write errors surface here, so a failing close means the
        // bytes are not on disk even though every write returned success.
        const int err = errno;
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        fail(err, "close: " + tmp.string());
    }

    // The rename is the commit point: it is atomic, so `path` names the old
    // contents right up until it names the new ones.
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const int err = errno;
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        fail(err, "rename: " + tmp.string() + " -> " + path.string());
    }
    // Without this the rename itself can be lost, leaving `path` pointing at
    // the file it named before.
    syncDirectory(path);
}

std::string readFileToString(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        fail(errno, "open: " + path.string());
    }

    std::string out;
    try {
        char   buf[4096];
        size_t offset = 0;
        while (true) {
            const size_t got = preadSome(fd, buf, sizeof(buf), offset, "read: " + path.string());
            if (got == 0) {
                break;
            }
            out.append(buf, got);
            offset += got;
        }
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);
    return out;
}

} // namespace fileutil
