#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// Shared POSIX file operations. Failed syscalls throw std::system_error with
// errno; preadSome reports a short read for the caller to interpret.
namespace fileutil {

// Uses fsync on macOS and fdatasync elsewhere.
void syncFile(int fd, std::string_view what);

// Best-effort directory sync for file creation and rename.
void syncDirectory(const std::filesystem::path& file) noexcept;

// Writes all n bytes at an explicit offset, retrying short writes and EINTR.
// Does not advance the file description's own offset.
void pwriteAll(int fd, const void* buf, size_t n, uint64_t offset, std::string_view what);

// Writes all n bytes at the file description's current offset.
void writeAll(int fd, const void* buf, size_t n, std::string_view what);

// Reads up to n bytes at an explicit offset, retrying short reads and EINTR.
// Returns the number actually read, which is < n only at end of file.
[[nodiscard]] size_t preadSome(int fd, void* buf, size_t n, uint64_t offset, std::string_view what);

// Writes `contents` to `tmp`, fsyncs it, renames it over `path`, then fsyncs
// the containing directory. A reader of `path` therefore sees either the whole
// old file or the whole new one, never a mix -- which is what makes CURRENT
// safe to update while a crash is possible.
void writeFileAtomic(const std::filesystem::path& path, std::string_view contents,
                     const std::filesystem::path& tmp);

// Throws std::system_error if the file cannot be opened or read.
[[nodiscard]] std::string readFileToString(const std::filesystem::path& path);

} // namespace fileutil
