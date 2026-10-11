#include "wal_reader.h"
#include <boost/crc.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

#include "util/file_util.h"

WalReader::WalReader(int fd, uint64_t initial_offset, WalReadMode mode)
    : _fd(fd), _file_offset(initial_offset), _mode(mode) { }

// ── low-level I/O ─────────────────────────────────────────────────────────────

size_t WalReader::tryRead(void* buf, size_t n) {
    // A short count is EOF, not an error: a torn tail is exactly what recovery
    // exists to cope with, so the caller decides what a short read means.
    const size_t got = fileutil::preadSome(_fd, buf, n, _file_offset, "WAL pread");
    _file_offset += got;
    return got;
}

bool WalReader::skipBytes(uint32_t n) {
    uint8_t trash[512];
    while (n > 0) {
        const uint32_t chunk = std::min(n, static_cast<uint32_t>(sizeof(trash)));
        const size_t   got   = tryRead(trash, chunk);
        n -= static_cast<uint32_t>(got);
        // A short read means the file ended inside the region being skipped.
        // Report it rather than looping -- tryRead cannot advance past EOF, so
        // retrying would spin forever on an unchanged offset.
        if (got < chunk) { return false; }
    }
    return true;
}

void WalReader::reportCorruption(const char* what) {
    if (_mode == WalReadMode::kStrict) {
        throw std::runtime_error(std::string("WAL: ") + what);
    }
    _stopped_early = true;
}

// ── fragment reader ───────────────────────────────────────────────────────────

WalReader::FragmentStatus WalReader::readFragment(WalRecordType&        out_type,
                                                  std::vector<uint8_t>& out_payload) {
    while (true) {
        // If fewer than kHeaderSize bytes remain in this block, it's trailing
        // padding written by the writer — skip to the next block.
        const uint32_t remaining = kBlockSize - blockOffset();
        if (remaining < kHeaderSize) {
            if (!skipBytes(remaining)) { return FragmentStatus::kEof; }
            continue;
        }

        // Read the 7-byte on-disk header.
        WriteAheadLogRecord hdr;
        const size_t        got = tryRead(&hdr, kHeaderSize);
        if (got == 0) { return FragmentStatus::kEof; }            // clean EOF between records
        if (got < kHeaderSize) { return FragmentStatus::kCorrupt; } // torn header

        // The writer pads a short block tail with zeros, which decode as a
        // kPadding header. When that tail was exactly kHeaderSize bytes,
        // consuming it has already landed us on the block boundary — skipping
        // a further kBlockSize from here would discard a whole block of live
        // records, so only skip when there is genuinely a remainder.
        if (hdr._type == WalRecordType::kPadding) {
            if (blockOffset() != 0 && !skipBytes(kBlockSize - blockOffset())) {
                return FragmentStatus::kEof;
            }
            continue;
        }

        // Read payload.
        out_payload.resize(hdr._length);
        if (hdr._length > 0 && tryRead(out_payload.data(), hdr._length) < hdr._length) {
            return FragmentStatus::kCorrupt; // the file ends inside the payload
        }

        // Verify CRC over [type byte | payload bytes].
        boost::crc_32_type crc;
        crc.process_byte(static_cast<uint8_t>(hdr._type));
        if (!out_payload.empty()) {
            crc.process_bytes(out_payload.data(), out_payload.size());
        }
        if (crc.checksum() != hdr._crc) { return FragmentStatus::kCorrupt; }

        out_type = hdr._type;
        return FragmentStatus::kOk;
    }
}

// ── record reader ─────────────────────────────────────────────────────────────

std::optional<std::vector<uint8_t>> WalReader::readRecord() {
    std::vector<uint8_t> scratch;
    std::vector<uint8_t> fragment;
    WalRecordType        type{ };
    bool                 in_fragment = false;

    _stopped_early = false;

    while (true) {
        switch (readFragment(type, fragment)) {
            case FragmentStatus::kEof:
                // A record whose continuation never reached disk is exactly
                // what a crash mid-append leaves behind. Drop the partial one.
                if (in_fragment) { reportCorruption("incomplete record at end of log"); }
                return std::nullopt;

            case FragmentStatus::kCorrupt:
                reportCorruption("torn or corrupted record");
                return std::nullopt;

            case FragmentStatus::kOk:
                break;
        }

        switch (type) {
            case WalRecordType::kFullType:
                if (in_fragment) {
                    reportCorruption("kFullType inside a fragment sequence");
                    return std::nullopt;
                }
                return fragment;

            case WalRecordType::kFirstType:
                if (in_fragment) {
                    reportCorruption("kFirstType inside a fragment sequence");
                    return std::nullopt;
                }
                scratch     = std::move(fragment);
                in_fragment = true;
                break;

            case WalRecordType::kMiddleType:
                if (!in_fragment) {
                    reportCorruption("kMiddleType without a kFirstType");
                    return std::nullopt;
                }
                scratch.insert(scratch.end(), fragment.begin(), fragment.end());
                break;

            case WalRecordType::kLastType:
                if (!in_fragment) {
                    reportCorruption("kLastType without a kFirstType");
                    return std::nullopt;
                }
                scratch.insert(scratch.end(), fragment.begin(), fragment.end());
                return scratch;

            case WalRecordType::kPadding:
                // readFragment consumes padding itself; it never surfaces here.
                reportCorruption("unexpected padding record");
                return std::nullopt;
        }
    }
}
