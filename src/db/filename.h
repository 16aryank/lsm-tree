#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

// Every file the engine writes lives in one directory and, apart from CURRENT,
// is named by a number drawn from a single space that VersionSet hands out:
//
//     CURRENT             names the live manifest, e.g. "MANIFEST-000006\n"
//     MANIFEST-000006     a WAL of VersionEdit records
//     000004.log          a write-ahead log
//     000005.sst          a table
//     000006.dbtmp        transient, the staging file for a CURRENT rename
//
// All numbered files share one counter, so a manifest can identify a table by
// number alone.

enum class FileType {
    kUnknown,
    kCurrent,
    kManifest,
    kLog,
    kTable,
    kTemp,
};

[[nodiscard]] std::filesystem::path currentFileName(const std::filesystem::path& dir);
[[nodiscard]] std::filesystem::path manifestFileName(const std::filesystem::path& dir, uint64_t number);
[[nodiscard]] std::filesystem::path logFileName(const std::filesystem::path& dir, uint64_t number);
[[nodiscard]] std::filesystem::path tableFileName(const std::filesystem::path& dir, uint64_t number);
[[nodiscard]] std::filesystem::path tempFileName(const std::filesystem::path& dir, uint64_t number);

// CURRENT stores the manifest's base name, keeping the directory movable.
[[nodiscard]] std::string manifestBaseName(uint64_t number);

// Classifies a directory entry's file name. Unrecognized names leave the
// outputs untouched and are ignored by cleanup.
// `out_number` is 0 for CURRENT, which carries no number.
[[nodiscard]] bool parseFileName(std::string_view name, FileType& out_type, uint64_t& out_number) noexcept;
