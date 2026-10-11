#include "db/filename.h"

#include <charconv>
#include <cstdio>
#include <string_view>

namespace {

// Six digits zero-padded, matching what the pre-manifest flusher wrote. The
// width is cosmetic -- parsing accepts any number of digits -- so a database
// that outlives 999999 files keeps working, just with longer names.
[[nodiscard]] std::string numbered(uint64_t number, const char* format) {
    char buf[64];
    const int n = std::snprintf(buf, sizeof(buf), format,
                                static_cast<unsigned long long>(number));
    return std::string(buf, static_cast<size_t>(n));
}

// Parses `text` as an unsigned decimal number that uses up every character.
// Rejects "12abc", "+3", "", and anything else that is not purely digits.
[[nodiscard]] bool parseNumber(std::string_view text, uint64_t& out) noexcept {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end   = text.data() + text.size();
    uint64_t    value = 0;
    const auto [stop, err] = std::from_chars(begin, end, value);
    if (err != std::errc{ } || stop != end) {
        return false;
    }
    out = value;
    return true;
}

constexpr std::string_view kManifestPrefix = "MANIFEST-";
constexpr std::string_view kCurrentName    = "CURRENT";

} // namespace

std::string manifestBaseName(uint64_t number) {
    return numbered(number, "MANIFEST-%06llu");
}

std::filesystem::path currentFileName(const std::filesystem::path& dir) {
    return dir / kCurrentName;
}

std::filesystem::path manifestFileName(const std::filesystem::path& dir, uint64_t number) {
    return dir / manifestBaseName(number);
}

std::filesystem::path logFileName(const std::filesystem::path& dir, uint64_t number) {
    return dir / numbered(number, "%06llu.log");
}

std::filesystem::path tableFileName(const std::filesystem::path& dir, uint64_t number) {
    return dir / numbered(number, "%06llu.sst");
}

std::filesystem::path tempFileName(const std::filesystem::path& dir, uint64_t number) {
    return dir / numbered(number, "%06llu.dbtmp");
}

bool parseFileName(std::string_view name, FileType& out_type, uint64_t& out_number) noexcept {
    if (name == kCurrentName) {
        out_type   = FileType::kCurrent;
        out_number = 0;
        return true;
    }

    if (name.starts_with(kManifestPrefix)) {
        uint64_t number = 0;
        if (!parseNumber(name.substr(kManifestPrefix.size()), number)) {
            return false;
        }
        out_type   = FileType::kManifest;
        out_number = number;
        return true;
    }

    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    const std::string_view stem      = name.substr(0, dot);
    const std::string_view extension = name.substr(dot);

    FileType type = FileType::kUnknown;
    if (extension == ".log") {
        type = FileType::kLog;
    } else if (extension == ".sst") {
        type = FileType::kTable;
    } else if (extension == ".dbtmp") {
        type = FileType::kTemp;
    } else {
        return false;
    }

    uint64_t number = 0;
    if (!parseNumber(stem, number)) {
        return false;
    }
    out_type   = type;
    out_number = number;
    return true;
}
