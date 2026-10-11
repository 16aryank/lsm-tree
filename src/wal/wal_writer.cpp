#include "wal_writer.h"
#include <boost/crc.hpp>
#include <algorithm>
#include <cassert>

#include "util/file_util.h"

WalWriter::WalWriter(int fd, uint64_t initial_offset)
    : _fd(fd), _file_offset(initial_offset) {}

void WalWriter::writeAll(const void* buf, size_t n) {
    // pwrite rather than write: the offset the WAL appends at is _file_offset,
    // which this class tracks itself, so the fd's own offset is never consulted
    // and a reader may share the same open file description.
    fileutil::pwriteAll(_fd, buf, n, _file_offset, "WAL pwrite");
    _file_offset += n;
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
    fileutil::syncFile(_fd, "WAL sync");
}