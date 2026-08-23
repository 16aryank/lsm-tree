#include "wal_writer.h"
#include <boost/crc.hpp>
#include <algorithm>
#include <cerrno>
#include <system_error>
#include <unistd.h>
#include <cassert>
#include <fcntl.h>

namespace {

// fdatasync is not available on MacOS.
int syncFile(int fd) {
#if defined(__APPLE__)
    return ::fsync(fd);
#else
    return ::fdatasync(fd);
#endif
}

} // namespace

WalWriter::WalWriter(int fd, uint64_t initial_offset)
    : _fd(fd), _file_offset(initial_offset) {}

void WalWriter::writeAll(const void* buf, size_t n) {
    const auto* p = static_cast<const uint8_t*>(buf);
    while (n > 0) {
        ssize_t r = ::pwrite(_fd, p, n, static_cast<off_t>(_file_offset));
        if (r < 0) {
            // Signal was delivered to the thread while written, no bytes are written
            if (errno == EINTR) { continue; }
            throw std::system_error(errno, std::generic_category(), "WAL pwrite");
        }
        p += r;
        _file_offset += static_cast<uint64_t>(r);
        n -= static_cast<size_t>(r);
    }
}

void WalWriter::padToNextBlock() {
    // Guard on the offset, not on the remaining byte count: `tail` below is
    // kBlockSize - blockOffset(), which is never 0, so testing it would never
    // short-circuit and a call made at a block boundary would write a whole
    // 32 KB block of zeros.
    if (blockOffset() == 0) return;

    const uint32_t tail = kBlockSize - blockOffset();
    static constexpr uint8_t zeros[kBlockSize]{ };
    writeAll(zeros, tail);
    // blockOffset() is now 0 because _file_offset is a multiple of kBlockSize.
    assert(blockOffset() == 0);
}

void WalWriter::emitFragment(WalRecordType type, std::span<const uint8_t> payload) {
    boost::crc_32_type crc;
    crc.process_byte(static_cast<uint8_t>(type));
    if (!payload.empty())
        crc.process_bytes(payload.data(), payload.size());

    WriteAheadLogRecord hdr;
    hdr._crc    = crc.checksum();
    hdr._length = static_cast<uint16_t>(payload.size());
    hdr._type   = type;

    writeAll(&hdr, kHeaderSize);
    if (!payload.empty())
        writeAll(payload.data(), payload.size());
}

void WalWriter::addRecord(std::span<const uint8_t> data) {
    // One record is the atomic unit: emitFragment writes a header and then a
    // payload, padToNextBlock may run between records, and every one of those
    // steps advances _file_offset. Interleaving two of these would produce a
    // log that reassembles into garbage, so the whole loop is one critical
    // section rather than each write being individually safe.
    std::lock_guard<std::mutex> guard(_mutex);

    bool           begin   = true;
    uint32_t       written = 0;
    const uint32_t total   = static_cast<uint32_t>(data.size());

    do {
        uint32_t remaining = kBlockSize - blockOffset();

        // Need room for the header plus at least one payload byte (unless the
        // record itself is empty, in which case kHeaderSize is enough).
        const bool     has_data  = (total - written > 0);
        const uint32_t min_room  = kHeaderSize + (has_data ? 1u : 0u);
        if (remaining < min_room) {
            padToNextBlock();
            remaining = kBlockSize;
        }

        const uint32_t avail    = remaining - kHeaderSize;
        const uint32_t frag_len = std::min(avail, total - written);
        const bool     last     = (written + frag_len == total);

        // A record that fits in one fragment is kFullType, not kFirstType --
        // the reader keys its reassembly state off this, so mistyping it here
        // leaves it waiting for a kLastType that never arrives.
        WalRecordType type =    begin && last   ?   WalRecordType::kFullType  :
                                begin           ?   WalRecordType::kFirstType :
                                last            ?   WalRecordType::kLastType  :
                                                    WalRecordType::kMiddleType;

        emitFragment(type, data.subspan(written, frag_len));
        written += frag_len;
        begin    = false;
    } while (written < total);
}

void WalWriter::sync() {
    // Pushes the written records out of the kernel's page cache. fdatasync is
    // preferred where it exists: the WAL never needs the metadata (size, mtime)
    // that fsync additionally flushes.
    //
    // Neither one flushes the drive's own write cache -- that would need
    // F_FULLFSYNC on MacOS, which is drastically slower. Guarding against
    // process crashes rather than power loss, which is what this buys, is the
    // deliberate trade.
    if (syncFile(_fd) != 0) {
        throw std::system_error(errno, std::generic_category(), "WAL sync");
    }
}