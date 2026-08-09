#include "wal_writer.h"
#include <boost/crc.hpp>
#include <algorithm>
#include <system_error>
#include <unistd.h>
#include <cassert>
#include <fcntl.h>

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
    const uint32_t tail = kBlockSize - blockOffset();
    if (tail == 0) return;
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

        WalRecordType type =    begin && last   ?   WalRecordType::kFirstType :
                                begin           ?   WalRecordType::kFirstType :
                                last            ?   WalRecordType::kLastType  :
                                                    WalRecordType::kMiddleType;

        emitFragment(type, data.subspan(written, frag_len));
        written += frag_len;
        begin    = false;
    } while (written < total);
}

void WalWriter::sync() {
    // Writes the modified file and critical metadata to secure storage.

    // MacOS does not support fdatasync(), use fsync().
    // Default to fdataasync because we do not need to store
    // metadata about file size or access time.

    // fsync() does is not truly equivalent to fdataasync()
    // because it only pushes data from the kernel to disk, not
    // the drive's hardware cache to physical storage.
    // This is fine because we're only concerned with application
    // crashes, and fcntl() is very slow.
    auto write_file = 
#ifdef __APPLE__ && defined(__MACH__)
    fsync(_fd);
#else
    ::fdataasync(_fd)
#endif

    if (write_file != 0)
        throw std::system_error(errno, std::generic_category(), "WAL fdatasync");
}