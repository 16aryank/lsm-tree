// Manifest edit round trips and malformed input handling.

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>

#include "coding.h"
#include "version/version_edit.h"

namespace {

FileMetaData meta(uint64_t number, uint64_t size, std::string smallest, uint64_t smallest_tag,
                  std::string largest, uint64_t largest_tag) {
    FileMetaData m;
    m.number       = number;
    m.file_size    = size;
    m.smallest_key = std::move(smallest);
    m.smallest_tag = smallest_tag;
    m.largest_key  = std::move(largest);
    m.largest_tag  = largest_tag;
    return m;
}

bool sameMeta(const FileMetaData& a, const FileMetaData& b) {
    return a.number == b.number && a.file_size == b.file_size &&
           a.smallest_key == b.smallest_key && a.smallest_tag == b.smallest_tag &&
           a.largest_key == b.largest_key && a.largest_tag == b.largest_tag;
}

// Encodes, decodes into a fresh edit, and asserts the two agree on everything.
VersionEdit roundTrip(const VersionEdit& edit) {
    VersionEdit out;
    assert(out.decode(edit.encode()));

    assert(out.hasLogNumber() == edit.hasLogNumber());
    assert(out.hasNextFileNumber() == edit.hasNextFileNumber());
    assert(out.hasLastSequence() == edit.hasLastSequence());
    if (edit.hasLogNumber())      { assert(out.logNumber() == edit.logNumber()); }
    if (edit.hasNextFileNumber()) { assert(out.nextFileNumber() == edit.nextFileNumber()); }
    if (edit.hasLastSequence())   { assert(out.lastSequence() == edit.lastSequence()); }

    assert(out.addedFiles().size() == edit.addedFiles().size());
    for (size_t i = 0; i < edit.addedFiles().size(); ++i) {
        assert(out.addedFiles()[i].first == edit.addedFiles()[i].first);
        assert(sameMeta(out.addedFiles()[i].second, edit.addedFiles()[i].second));
    }

    assert(out.deletedFiles() == edit.deletedFiles());
    return out;
}

} // namespace

int main() {
    // An edit with nothing in it is legal and encodes to nothing. VersionSet
    // never writes one, but a decoder that choked on an empty payload would
    // turn a harmless record into a recovery failure.
    {
        VersionEdit empty;
        assert(empty.encode().empty());
        const VersionEdit decoded = roundTrip(empty);
        assert(decoded.addedFiles().empty());
        assert(decoded.deletedFiles().empty());
    }

    // Each scalar survives on its own, including 0 -- which is why the has*
    // flags exist rather than treating 0 as absent.
    {
        VersionEdit edit;
        edit.setLogNumber(0);
        const VersionEdit decoded = roundTrip(edit);
        assert(decoded.hasLogNumber());
        assert(decoded.logNumber() == 0);
        assert(!decoded.hasLastSequence());
    }

    // Every field at once, with values past 32 bits to catch a varint32 used
    // where a varint64 belongs.
    {
        VersionEdit edit;
        edit.setLogNumber(1ull << 40);
        edit.setNextFileNumber((1ull << 33) + 7);
        edit.setLastSequence((1ull << 55) - 1);
        edit.addFile(0, meta(11, 4096, "apple", 0x0100, "mango", 0x0200));
        edit.addFile(3, meta(12, 1ull << 35, "nectarine", 0x0301, "papaya", 0x0400));
        edit.deleteFile(0, 9);
        edit.deleteFile(6, 10);
        roundTrip(edit);
    }

    // Keys are arbitrary bytes: embedded NULs and empty keys have to survive,
    // since a length-prefixed field is the only thing delimiting them.
    {
        VersionEdit edit;
        edit.addFile(1, meta(20, 64, std::string("a\0b", 3), 1, "", 2));
        const VersionEdit decoded = roundTrip(edit);
        assert(decoded.addedFiles()[0].second.smallest_key == std::string("a\0b", 3));
        assert(decoded.addedFiles()[0].second.largest_key.empty());
    }

    // clear() has to reset the has* flags too, or a reused edit leaks the
    // previous one's counters into the manifest.
    {
        VersionEdit edit;
        edit.setLastSequence(5);
        edit.addFile(0, meta(1, 1, "a", 1, "b", 2));
        edit.deleteFile(0, 2);
        edit.clear();
        assert(!edit.hasLastSequence());
        assert(edit.addedFiles().empty());
        assert(edit.deletedFiles().empty());
        assert(edit.encode().empty());
    }

    // Truncation. A cut that lands mid-field must be rejected; a cut that
    // happens to land exactly on a field boundary is a genuinely well-formed
    // shorter edit, and this codec cannot tell it apart from one that was
    // written that way -- detecting a torn write is the WAL layer's CRC's job,
    // not this decoder's.
    //
    // What must hold at every cut is that the decoder never invents content it
    // did not read: a prefix either fails, or re-encodes to exactly the bytes
    // it was handed. Cutting one byte at a time walks every field boundary.
    {
        VersionEdit edit;
        edit.setLogNumber(300);
        edit.setLastSequence(1234);
        edit.addFile(2, meta(7, 8192, "alpha", 0x0100, "omega", 0x0200));
        edit.deleteFile(1, 6);
        const std::string encoded = edit.encode();

        size_t rejected = 0;
        for (size_t cut = 1; cut < encoded.size(); ++cut) {
            const std::string prefix = encoded.substr(0, cut);
            VersionEdit       decoded;
            if (!decoded.decode(prefix)) {
                ++rejected;
                // A rejected decode leaves nothing behind to be mistaken for state.
                assert(!decoded.hasLogNumber());
                assert(!decoded.hasLastSequence());
                assert(decoded.addedFiles().empty());
                assert(decoded.deletedFiles().empty());
                continue;
            }
            assert(decoded.encode() == prefix);
        }
        // Most cuts land mid-field, so a run where nothing was rejected would
        // mean the decoder is accepting whatever it is given.
        assert(rejected > 0);

        VersionEdit whole;
        assert(whole.decode(encoded));
        assert(whole.encode() == encoded);
    }

    // A stray byte after a complete edit is read as the start of another field,
    // and 0 is not a tag this format assigns -- so the payload is rejected
    // rather than silently accepted as the edit it looks like from the front.
    {
        VersionEdit edit;
        edit.setLogNumber(4);
        std::string encoded = edit.encode();
        encoded.push_back('\0');
        VersionEdit decoded;
        assert(!decoded.decode(encoded));
    }

    // An unknown tag is unskippable -- its payload length is part of what the
    // tag means -- so a record from a newer format is rejected outright.
    {
        std::string encoded;
        putVarint32(encoded, 99);
        putVarint64(encoded, 1);
        VersionEdit decoded;
        assert(!decoded.decode(encoded));
    }

    // A level outside the tree is corruption, not something to clamp: it would
    // otherwise index a level array out of bounds.
    {
        std::string encoded;
        putVarint32(encoded, static_cast<uint32_t>(EditTag::kDeletedFile));
        putVarint32(encoded, static_cast<uint32_t>(kNumLevels));
        putVarint64(encoded, 3);
        VersionEdit decoded;
        assert(!decoded.decode(encoded));
    }

    // A repeated scalar is ambiguous about which value wins.
    {
        std::string encoded;
        putVarint32(encoded, static_cast<uint32_t>(EditTag::kLastSequence));
        putVarint64(encoded, 1);
        putVarint32(encoded, static_cast<uint32_t>(EditTag::kLastSequence));
        putVarint64(encoded, 2);
        VersionEdit decoded;
        assert(!decoded.decode(encoded));
    }

    // A key length prefix larger than the bytes that follow it.
    {
        std::string encoded;
        putVarint32(encoded, static_cast<uint32_t>(EditTag::kNewFile));
        putVarint32(encoded, 0);
        putVarint64(encoded, 1);      // number
        putVarint64(encoded, 100);    // file size
        putVarint64(encoded, 4096);   // smallest key length, with no key behind it
        VersionEdit decoded;
        assert(!decoded.decode(encoded));
    }

    return 0;
}
