#include "wal_reader.h"
#include <boost/crc.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

WalReader::WalReader(int fd, uint64_t initial_offset)
    : _fd(fd), _file_offset(initial_offset) {}

// ── low-level I/O ─────────────────────────────────────────────────────────────

size_t WalReader::tryRead(void* buf, size_t n) {
    auto* p   = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::pread(_fd, p + got, n - got, static_cast<off_t>(_file_offset + got));
        if (r < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "WAL pread");
        }
        if (r == 0) break; // EOF
        got += static_cast<size_t>(r);
    }
    _file_offset += got;
    return got;
}

void WalReader::readAll(void* buf, size_t n) {
    if (tryRead(buf, n) != n)
        throw std::runtime_error("WAL: unexpected EOF inside record");
}

void WalReader::skipBytes(uint32_t n) {
    uint8_t trash[512];
    while (n > 0) {
        const uint32_t chunk = std::min(n, static_cast<uint32_t>(sizeof(trash)));
        const size_t   got   = tryRead(trash, chunk);
        n -= static_cast<uint32_t>(got);
        if (got < chunk) return; // hit EOF, let next read surface it
    }
}

// ── fragment reader ───────────────────────────────────────────────────────────

bool WalReader::readFragment(WalRecordType& out_type, std::vector<uint8_t>& out_payload) {
    while (true) {
        // If fewer than kHeaderSize bytes remain in this block, it's trailing
        // padding written by the writer — skip to the next block.
        const uint32_t remaining = kBlockSize - blockOffset();
        if (remaining < kHeaderSize) {
            skipBytes(remaining);
            continue;
        }

        // Read the 7-byte on-disk header.
        WriteAheadLogRecord hdr;
        const size_t got = tryRead(&hdr, kHeaderSize);
        if (got == 0) return false; // clean EOF between records
        if (got < kHeaderSize) throw std::runtime_error("WAL: truncated header");

        // Type == 0 is not a valid WalRecordType; the writer uses it as a
        // sentinel when it pads exactly kHeaderSize bytes at a block boundary.
        if (static_cast<uint8_t>(hdr._type) == 0) {
            // We consumed kHeaderSize bytes; skip whatever's left in this block.
            skipBytes(kBlockSize - blockOffset());
            continue;
        }

        // Read payload.
        out_payload.resize(hdr._length);
        if (hdr._length > 0)
            readAll(out_payload.data(), hdr._length);

        // Verify CRC over [type byte | payload bytes].
        boost::crc_32_type crc;
        crc.process_byte(static_cast<uint8_t>(hdr._type));
        if (!out_payload.empty())
            crc.process_bytes(out_payload.data(), out_payload.size());
        if (crc.checksum() != hdr._crc)
            throw std::runtime_error("WAL: CRC mismatch — data corrupted");

        out_type = hdr._type;
        return true;
    }
}

// ── record reader ─────────────────────────────────────────────────────────────

std::optional<std::vector<uint8_t>> WalReader::readRecord() {
    std::vector<uint8_t> scratch;
    std::vector<uint8_t> fragment;
    WalRecordType        type{};
    bool                 in_fragment = false;

    while (true) {
        if (!readFragment(type, fragment)) {
            if (in_fragment)
                throw std::runtime_error("WAL: incomplete record at EOF");
            return std::nullopt;
        }

        switch (type) {
            case WalRecordType::kFullType:
                if (in_fragment) throw std::runtime_error("WAL: kFullType inside fragment sequence");
                return fragment;

            case WalRecordType::kFirstType:
                if (in_fragment) throw std::runtime_error("WAL: kFirstType inside fragment sequence");
                scratch      = std::move(fragment);
                in_fragment  = true;
                break;

            case WalRecordType::kMiddleType:
                if (!in_fragment) throw std::runtime_error("WAL: kMiddleType without kFirstType");
                scratch.insert(scratch.end(), fragment.begin(), fragment.end());
                break;

            case WalRecordType::kLastType:
                if (!in_fragment) throw std::runtime_error("WAL: kLastType without kFirstType");
                scratch.insert(scratch.end(), fragment.begin(), fragment.end());
                return scratch;
        }
    }
}
