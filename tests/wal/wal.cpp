// Round-trip through WalWriter and WalReader: fragment typing, records that
// span blocks, the block-padding path, how a torn tail is treated in each of
// the two read modes, and that concurrent appends stay intact.
#include "src/wal/wal_codec.h"
#include "src/wal/wal_reader.h"
#include "src/wal/wal_writer.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace {

std::filesystem::path testDir() {
    return std::filesystem::temp_directory_path() /
           ("lsm-wal-test-" + std::to_string(::getpid()));
}

std::vector<uint8_t> payloadOf(size_t n, uint8_t seed) {
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<uint8_t>(seed + i);
    }
    return out;
}

std::span<const uint8_t> spanOf(const std::vector<uint8_t>& v) { return { v.data(), v.size() }; }

int openWrite(const std::filesystem::path& p) {
    const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    return fd;
}

int openRead(const std::filesystem::path& p) {
    const int fd = ::open(p.c_str(), O_RDONLY);
    assert(fd >= 0);
    return fd;
}

// Flips one byte at `offset`, which is how a corrupted payload is simulated.
void flipByte(const std::filesystem::path& p, uint64_t offset) {
    const int fd = ::open(p.c_str(), O_RDWR);
    assert(fd >= 0);
    uint8_t byte = 0;
    assert(::pread(fd, &byte, 1, static_cast<off_t>(offset)) == 1);
    byte ^= 0xFF;
    assert(::pwrite(fd, &byte, 1, static_cast<off_t>(offset)) == 1);
    ::close(fd);
}

} // namespace

int main() {
    const std::filesystem::path dir = testDir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    // A single small record round-trips, and the log then ends cleanly. This is
    // the case that was broken: the writer tagged it kFirstType, so the reader
    // waited for a continuation that never came.
    {
        const auto path = dir / "single.log";
        const auto data = payloadOf(64, 1);
        {
            const int fd = openWrite(path);
            WalWriter writer(fd);
            writer.addRecord(spanOf(data));
            writer.sync();
            ::close(fd);
        }
        const int fd = openRead(path);
        WalReader reader(fd);
        const auto got = reader.readRecord();
        assert(got.has_value());
        assert(*got == data);
        assert(!reader.readRecord().has_value());
        assert(!reader.stoppedEarly()); // a clean end, not a torn one
        ::close(fd);
    }

    // Many records of mixed sizes, in order, including an empty one.
    {
        const auto path = dir / "many.log";
        std::vector<std::vector<uint8_t>> written;
        for (size_t i = 0; i < 500; ++i) {
            written.push_back(payloadOf(i % 97, static_cast<uint8_t>(i)));
        }
        {
            const int fd = openWrite(path);
            WalWriter writer(fd);
            for (const auto& record : written) {
                writer.addRecord(spanOf(record));
            }
            writer.sync();
            ::close(fd);
        }
        const int fd = openRead(path);
        WalReader reader(fd);
        for (const auto& expected : written) {
            const auto got = reader.readRecord();
            assert(got.has_value());
            assert(*got == expected);
        }
        assert(!reader.readRecord().has_value());
        assert(!reader.stoppedEarly());
        ::close(fd);
    }

    // Records larger than a block fragment into First/Middle/Last and are
    // reassembled. 100000 bytes spans four fragments across four blocks.
    {
        const auto path = dir / "big.log";
        const auto small = payloadOf(10, 7);
        const auto huge  = payloadOf(100000, 3);
        const auto after = payloadOf(50, 9);
        {
            const int fd = openWrite(path);
            WalWriter writer(fd);
            writer.addRecord(spanOf(small));
            writer.addRecord(spanOf(huge));
            writer.addRecord(spanOf(after));
            writer.sync();
            ::close(fd);
        }
        const int fd = openRead(path);
        WalReader reader(fd);
        assert(*reader.readRecord() == small);
        assert(*reader.readRecord() == huge);
        assert(*reader.readRecord() == after);
        assert(!reader.readRecord().has_value());
        ::close(fd);
    }

    // The padding path, aimed by hand rather than left to chance.
    //
    // A first record of exactly kMaxPayload - kHeaderSize bytes costs
    // kHeaderSize + that = kBlockSize - kHeaderSize bytes, leaving precisely
    // kHeaderSize bytes of block tail. That is the one width the writer pads
    // with a full zero header rather than a short run, and the width at which
    // the reader used to consume the padding, find itself already on the block
    // boundary, and then skip a further kBlockSize -- discarding the whole of
    // the next block.
    {
        const auto path = dir / "padding.log";
        const auto filler = payloadOf(kBlockSize - 2 * kHeaderSize, 11);
        const auto behind_the_padding = payloadOf(1234, 13);
        const auto trailing = payloadOf(77, 17);
        {
            const int fd = openWrite(path);
            WalWriter writer(fd);
            writer.addRecord(spanOf(filler));
            // Exactly kHeaderSize bytes of tail remain in block 0.
            assert(writer.fileOffset() % kBlockSize == kBlockSize - kHeaderSize);
            writer.addRecord(spanOf(behind_the_padding));
            // The pad pushed the second record into block 1.
            assert(writer.fileOffset() / kBlockSize == 1);
            writer.addRecord(spanOf(trailing));
            writer.sync();
            ::close(fd);
        }
        const int fd = openRead(path);
        WalReader reader(fd);
        assert(*reader.readRecord() == filler);
        assert(*reader.readRecord() == behind_the_padding);
        assert(*reader.readRecord() == trailing);
        assert(!reader.readRecord().has_value());
        assert(!reader.stoppedEarly());
        ::close(fd);
    }

    // A torn tail is what a crash mid-append leaves behind. Recovery reads
    // every complete record and then stops, reporting that the stop was not
    // clean; strict mode raises instead.
    {
        const auto path = dir / "torn.log";
        const auto first  = payloadOf(100, 21);
        const auto second = payloadOf(100, 23);
        uint64_t   complete_through = 0;
        {
            const int fd = openWrite(path);
            WalWriter writer(fd);
            writer.addRecord(spanOf(first));
            complete_through = writer.fileOffset();
            writer.addRecord(spanOf(second));
            writer.sync();
            ::close(fd);
        }
        // Cut the second record in half, mid-payload.
        {
            const int fd = ::open(path.c_str(), O_WRONLY);
            assert(fd >= 0);
            assert(::ftruncate(fd, static_cast<off_t>(complete_through + kHeaderSize + 50)) == 0);
            ::close(fd);
        }
        {
            const int fd = openRead(path);
            WalReader reader(fd);
            assert(*reader.readRecord() == first);
            assert(!reader.readRecord().has_value());
            assert(reader.stoppedEarly()); // torn, not clean
            ::close(fd);
        }
        {
            const int fd = openRead(path);
            WalReader reader(fd, 0, WalReadMode::kStrict);
            assert(*reader.readRecord() == first);
            bool threw = false;
            try {
                (void)reader.readRecord();
            } catch (const std::exception&) {
                threw = true;
            }
            assert(threw);
            ::close(fd);
        }
    }

    // A truncation landing exactly on a record boundary is a clean end, not a
    // torn one -- otherwise every recovery would report damage.
    {
        const auto path = dir / "clean-cut.log";
        const auto first = payloadOf(100, 31);
        uint64_t   boundary = 0;
        {
            const int fd = openWrite(path);
            WalWriter writer(fd);
            writer.addRecord(spanOf(first));
            boundary = writer.fileOffset();
            writer.addRecord(spanOf(payloadOf(100, 33)));
            writer.sync();
            ::close(fd);
        }
        const int fd = ::open(path.c_str(), O_WRONLY);
        assert(::ftruncate(fd, static_cast<off_t>(boundary)) == 0);
        ::close(fd);

        const int rfd = openRead(path);
        WalReader reader(rfd);
        assert(*reader.readRecord() == first);
        assert(!reader.readRecord().has_value());
        assert(!reader.stoppedEarly());
        ::close(rfd);
    }

    // A byte flipped inside a payload fails the CRC. Recovery stops there;
    // strict mode raises. Either way the records before it still read back.
    {
        const auto path = dir / "corrupt.log";
        const auto first  = payloadOf(100, 41);
        const auto second = payloadOf(100, 43);
        uint64_t   second_payload_at = 0;
        {
            const int fd = openWrite(path);
            WalWriter writer(fd);
            writer.addRecord(spanOf(first));
            second_payload_at = writer.fileOffset() + kHeaderSize;
            writer.addRecord(spanOf(second));
            writer.sync();
            ::close(fd);
        }
        flipByte(path, second_payload_at + 10);
        {
            const int fd = openRead(path);
            WalReader reader(fd);
            assert(*reader.readRecord() == first);
            assert(!reader.readRecord().has_value());
            assert(reader.stoppedEarly());
            ::close(fd);
        }
        {
            const int fd = openRead(path);
            WalReader reader(fd, 0, WalReadMode::kStrict);
            assert(*reader.readRecord() == first);
            bool threw = false;
            try {
                (void)reader.readRecord();
            } catch (const std::exception&) {
                threw = true;
            }
            assert(threw);
            ::close(fd);
        }
    }

    // A reader resumes from a saved offset, which is what appending to an
    // existing log and replaying only its tail will need.
    {
        const auto path = dir / "resume.log";
        const auto first  = payloadOf(80, 51);
        const auto second = payloadOf(80, 53);
        uint64_t   after_first = 0;
        {
            const int fd = openWrite(path);
            WalWriter writer(fd);
            writer.addRecord(spanOf(first));
            after_first = writer.fileOffset();
            writer.addRecord(spanOf(second));
            writer.sync();
            ::close(fd);
        }
        const int fd = openRead(path);
        WalReader reader(fd, after_first);
        assert(*reader.readRecord() == second);
        assert(!reader.readRecord().has_value());
        ::close(fd);

        // And a writer resuming at the same offset appends rather than
        // overwriting, since it preads at an explicit offset.
        const int wfd = ::open(path.c_str(), O_WRONLY);
        assert(wfd >= 0);
        WalWriter appender(wfd, std::filesystem::file_size(path));
        const auto third = payloadOf(80, 55);
        appender.addRecord(spanOf(third));
        appender.sync();
        ::close(wfd);

        const int rfd = openRead(path);
        WalReader replay(rfd);
        assert(*replay.readRecord() == first);
        assert(*replay.readRecord() == second);
        assert(*replay.readRecord() == third);
        assert(!replay.readRecord().has_value());
        ::close(rfd);
    }

    // ── payload codec ─────────────────────────────────────────────────────────

    // Round-trips over the shapes a real write path produces, including the
    // ones with an empty side: a tombstone carries no value, and an empty key
    // is legal (the skiplist test already covers it end to end).
    {
        struct Case {
            uint64_t    seq;
            ValueType   type;
            std::string key;
            std::string value;
        };
        const Case cases[] = {
            { 1, ValueType::kValue, "a", "1" },
            { 2, ValueType::kDelete, "gone", "" },
            { 127, ValueType::kValue, "", "" },
            { 128, ValueType::kValue, std::string(300, 'k'), std::string(70000, 'v') },
            { (1ull << 55) - 1, ValueType::kDelete, "high-seq", "" },
        };
        for (const auto& c : cases) {
            const std::string encoded = encodeWalEntry(c.seq, c.type, c.key, c.value);
            WalEntryView decoded;
            assert(decodeWalEntry(encoded, decoded));
            assert(decoded.sequenceNumber() == c.seq);
            assert(decoded.valueType() == c.type);
            assert(decoded.key == c.key);
            assert(decoded.value == c.value);
        }

        // Malformed inputs are rejected rather than silently half-decoded.
        WalEntryView scratch;
        const std::string good = encodeWalEntry(9, ValueType::kValue, "key", "value");
        assert(!decodeWalEntry("", scratch));                     // empty
        assert(!decodeWalEntry(good.substr(0, good.size() - 2), scratch)); // truncated value
        assert(!decodeWalEntry(good.substr(0, 1), scratch));      // truncated after the tag
        assert(!decodeWalEntry(good + "junk", scratch));          // trailing bytes
    }

    // Codec through framing: the combination the write path actually uses. The
    // large value forces a record to span blocks, so reassembly and decoding
    // have to agree on where the payload ends.
    {
        const auto path = dir / "codec.log";
        struct Entry {
            uint64_t    seq;
            ValueType   type;
            std::string key;
            std::string value;
        };
        std::vector<Entry> entries;
        for (uint64_t i = 0; i < 200; ++i) {
            entries.push_back({ i + 1,
                                (i % 5 == 0) ? ValueType::kDelete : ValueType::kValue,
                                "key" + std::to_string(i),
                                (i % 5 == 0) ? "" : std::string(i * 40, 'v') });
        }
        {
            const int fd = openWrite(path);
            WalWriter writer(fd);
            for (const auto& e : entries) {
                const std::string record = encodeWalEntry(e.seq, e.type, e.key, e.value);
                writer.addRecord(byteSpan(record));
            }
            writer.sync();
            ::close(fd);
        }
        const int fd = openRead(path);
        WalReader reader(fd);
        for (const auto& expected : entries) {
            const auto record = reader.readRecord();
            assert(record.has_value());
            WalEntryView decoded;
            assert(decodeWalEntry(byteView(*record), decoded));
            assert(decoded.sequenceNumber() == expected.seq);
            assert(decoded.valueType() == expected.type);
            assert(decoded.key == expected.key);
            assert(decoded.value == expected.value);
        }
        assert(!reader.readRecord().has_value());
        assert(!reader.stoppedEarly());
        ::close(fd);
    }

    // ── concurrent appends ────────────────────────────────────────────────────

    // Several threads appending to one WalWriter. Every record must survive
    // whole: a missing lock would let one thread's header be followed by
    // another's payload, and the log would reassemble into garbage.
    //
    // Value sizes are chosen to straddle kBlockSize so the multi-fragment path
    // is contended too -- that is where an interleaved append does the most
    // damage, since a First fragment can end up matched with the wrong Last.
    {
        constexpr int kThreads       = 4;
        constexpr int kPerThread     = 40;
        const auto path = dir / "concurrent.log";

        const auto keyFor = [](int thread, int i) {
            return "t" + std::to_string(thread) + "-r" + std::to_string(i);
        };
        // 0, 12000, ... 72000: the last four exceed kMaxPayload and fragment.
        const auto valueFor = [](int thread, int i) {
            return std::string(static_cast<size_t>(i % 7) * 12000,
                               static_cast<char>('a' + thread));
        };

        {
            const int fd = openWrite(path);
            WalWriter writer(fd);

            std::vector<std::thread> writers;
            for (int t = 0; t < kThreads; ++t) {
                writers.emplace_back([&, t] {
                    for (int i = 0; i < kPerThread; ++i) {
                        const std::string record = encodeWalEntry(
                            static_cast<uint64_t>(t * kPerThread + i + 1),
                            (i % 5 == 0) ? ValueType::kDelete : ValueType::kValue,
                            keyFor(t, i), valueFor(t, i));
                        writer.addRecord(byteSpan(record));
                    }
                });
            }
            for (auto& w : writers) {
                w.join();
            }
            writer.sync();
            ::close(fd);
        }

        // Replay. Records may arrive in any order -- threads race for the lock,
        // and nothing promises sequence order matches log order -- so check
        // each one is present exactly once with the right contents rather than
        // checking the sequence.
        std::unordered_map<std::string, std::string> outstanding;
        for (int t = 0; t < kThreads; ++t) {
            for (int i = 0; i < kPerThread; ++i) {
                outstanding.emplace(keyFor(t, i), valueFor(t, i));
            }
        }

        const int fd = openRead(path);
        WalReader reader(fd);
        size_t seen = 0;
        while (const auto record = reader.readRecord()) {
            WalEntryView entry;
            assert(decodeWalEntry(byteView(*record), entry));
            const auto found = outstanding.find(std::string(entry.key));
            assert(found != outstanding.end()); // unknown or duplicated key
            assert(entry.value == found->second);
            outstanding.erase(found);
            ++seen;
        }
        assert(!reader.stoppedEarly());
        assert(seen == static_cast<size_t>(kThreads * kPerThread));
        assert(outstanding.empty());
        ::close(fd);
    }

    std::filesystem::remove_all(dir);
    return 0;
}
