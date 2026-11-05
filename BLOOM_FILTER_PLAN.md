# Bloom filter plan

## Status and goal

Steps 1–3 complete; step 4 remains. Add one file-wide Bloom filter to every newly written SSTable. It covers distinct **user keys**, including keys whose newest entry is a tombstone, independent of sequence number. A filter-negative point lookup must return a miss before reading a data block. False positives may still read a block; false negatives are unacceptable.

`SSTableBuilder` is shared by memtable flushes, recovery flushes, and compaction outputs, so building the filter there covers all three paths. `Version::get` already delegates candidate files to `SSTable::get`; the lookup check belongs in `SSTable::get`. Existing files use the `LSMSST01` 32-byte footer and have no filter. Opening them must keep working, with the current lookup path as a fallback until compaction rewrites them.

The persisted filter and index are read once when a table opens. “Before any block I/O” means before **data-block I/O caused by a point lookup**; the filter itself must be loaded from disk before it can be consulted. Today `SSTable::open` also reads the first data block for metadata. That open-time read is separate from a lookup and should remain unless a metadata redesign proves necessary.

## Design choices to settle in code

- Use Kirsch–Mitzenmacher double hashing: positions `g_i(key) = (h1(key) + i * h2(key)) mod m`, `i = 0..k-1`. Use two deterministic, independently seeded 64-bit hashes of the full byte string, including embedded NULs; do not use `std::hash` for persisted data. Ensure the second step is nonzero. Use identical hashing and bit numbering in the writer and reader. The [Kirsch–Mitzenmacher paper](https://jameshunt.us/papers/kirsch-mitz.pdf) is the algorithm reference.
- Start with a nominal 10 bits per distinct key and `k = 7`, a minimum 64-bit filter for nonempty tables, and a bit count rounded to whole bytes. For an empty table, write an empty filter that rejects every key. Bound arithmetic and serialized sizes before allocating memory. Record the actual `m` and `k` on disk so the reader does not infer them from defaults.
- Add a checksum-protected filter block between data blocks and the index. Introduce a new SSTable magic, `LSMSST02`, and a footer containing filter and index block handles plus the entry count. Keep the old footer decoder for `LSMSST01`. Read the final magic first to choose the footer size, then validate block order, bounds, sizes, and checksums before trusting the filter. A malformed new filter fails table open; silently skipping it could hide corruption or change lookup behavior.
- While `SSTableBuilder::add` streams sorted entries, retain the two hashes only when the user key changes. At `finish`, size and build one filter from those hashes, then write filter block, index block, and footer before syncing the table. This keeps the existing one-pass table write path and avoids storing full duplicate keys, at the cost of temporary memory proportional to distinct keys. Include the filter in `finish()` file size and account for it in output-size estimates if splitting behavior materially changes.
- Keep a loaded filter per `SSTable` instance. Check it at the start of `SSTable::get`, before `findBlock` and `readBlock`. Iterators still read actual entries and never use the filter. A filter match is only permission to continue the normal lookup; it is not proof that a key or snapshot-visible version exists.

## Step 1 — Implement and prove the filter primitive

**Complete.** `src/sstable/bloom_filter.{h,cpp}` implements a versioned payload, deterministic seeded hashes, adjacent user-key deduplication, and bounded decode/build. `tests/sstable/bloom_filter.cpp` covers this step. The focused test and `make test` pass. Shifting the double-hash probe position by one temporarily caused the fixed encoding assertion to fail; the correct expression was restored and the focused test passed again. The fixed probe set produced 84 false positives in 10,000 absent keys.

Add a small filter encoder/decoder and deterministic two-hash implementation in `src/sstable`, with no general-purpose filtering framework. Specify the payload version, bit count, probe count, and bit array in the format code.

**Test gate:** Focused tests cover empty and tiny filters, duplicate user keys, embedded NUL and high-byte keys, deterministic encoding across repeated builds, round-trip decoding, malformed payload rejection, and zero false negatives for a substantial fixed key set. Include a deterministic disjoint probe set and count false positives. A temporary change to the hash step or bit access must break a focused test. Run `make test` before step 2.

## Step 2 — Persist and load one filter per new SSTable

**Complete.** New tables write a checked filter block and `LSMSST02` footer; opening loads and validates the filter, index, and nonoverlapping data region. The frozen `LSMSST01` fixture still opens. The SSTable test covers multi-block reads, empty tables, malformed and truncated filters, checksum and handle corruption, and oversized offsets. The focused test and `make test` pass. Temporarily omitting the filter block or ignoring block checksums broke the focused test; both changes were restored.

Extend `SSTableBuilder::finish`, the footer format, and `SSTable::open` with the new filter block. Preserve `LSMSST01` reading. Validate offset arithmetic before narrowing any stored sizes to `size_t` or `uint32_t`, and reject overlap between data, filter, index, and footer. Existing metadata and iterator behavior must stay intact.

**Test gate:** A new multi-block table reopens with exactly one checked filter block and still returns all entries, versions, and tombstones. Empty tables reopen with an empty filter. Corrupt or truncate the filter, its checksum, and its footer handle; open must fail. A frozen `LSMSST01` fixture must still open and answer lookups. A mutation that omits the filter block or ignores its checksum must fail these tests. Run `make test` before step 3.

## Step 3 — Gate point reads before data-block I/O

**Complete.** `SSTable::get` checks the loaded filter before its index seek and data-block read. A read-only `mayContainUserKey` method reports the same decision; legacy tables return true and keep their existing lookup path. The focused test selects a filter-negative key inside a multi-block table's range, corrupts the data block a normal lookup would read, and proves the miss avoids that checksum error while a present key detects it. Snapshot-sequence reads and tombstones remain correct. Temporarily bypassing the filter check caused the negative lookup to fail on the corrupted block; the check was restored. The focused test and `make test` pass. The DB sweep test now waits for compaction before comparing the settled file set with the current version.

Check the in-memory filter in `SSTable::get` before seeking or reading a data block. Keep the existing index and range checks in `Version::get`; a legacy table without a filter uses its existing behavior. Provide a narrow way for tests to observe the persisted table's filter decision, such as a read-only `mayContainUserKey` method, rather than a general instrumentation system.

**Test gate:** Find a known filter-negative absent key inside a multi-block table's key range, corrupt the later data block it would otherwise probe, and verify the lookup returns a miss without a checksum error. A present key in that block must still read it and detect corruption. Verify newer and older snapshot reads and tombstones remain correct. Temporarily bypassing the filter check must break the negative-lookup test. Run `make test` before step 4.

## Step 4 — Exercise every table-writing path and measure false positives

Use DB tests to force a flush, restart, and compaction with output splitting. Check that each newly written SSTable has a filter and that byte-string keys, point reads, snapshots, and tombstones remain correct before and after rewriting. Keep an old-format table in a recovery fixture to verify fallback until rewritten.

For the empirical measurement, build and reopen a persisted table with at least 10,000 distinct deterministic keys. Probe at least 100,000 distinct absent keys from a disjoint namespace through its loaded filter. Record `n`, actual bit count `m`, `k`, probe count, false-positive count, and `false positives / probes`; assert zero false negatives and a generous upper bound (initially 3%) to catch a broken distribution without requiring an exact probabilistic result. The test should report the observed count and rate, not a theoretical value substituted for measurement. Use fixed inputs so the number is reproducible.

**Test gate:** The integration and empirical tests pass, and a temporary change that disables filter construction or lookup breaks the appropriate test. Run `make test`. Add the measured rate and workload parameters to `ARCHITECTURE.md`, alongside the on-disk layout, legacy-file behavior, and lookup I/O contract. Commit passing milestones using `COMMIT_MESSAGES.md`; delete this plan after implementation and durable architecture documentation are complete.
