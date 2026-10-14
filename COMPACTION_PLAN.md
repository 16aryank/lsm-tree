# Compaction plan

## Status

Steps 0–6 are implemented. The picker, background worker, bounded retries, output splitting, pending-file protection, manifest failure guard, worker metrics, registered snapshots, and reclamation rules are in place. Tests cover level-0 and level-1 selection, level-0-to-1 and level-1-to-2 compaction, snapshots across compactions, snapshot release, both tombstone rules, restart, and an older pinned `Version` through cleanup. Fault-injection tests for output and manifest I/O failures remain open.

## Goal and current starting point

Move flushed SSTables from level 0 through levels 1–6, keeping point reads correct while reducing the number of files they inspect. The implementation uses immutable `Version`s, `VersionEdit` file additions and deletions, level scores, overlap queries, sorted iterators, and `SSTableBuilder`.

The first milestone merged files without discarding historical versions or tombstones. The next milestone replaced unrestricted sequence-number reads with registered snapshot handles, allowing compaction to reclaim entries once no active reader needs them.

## 0. Make write order match sequence order

Before step 0, `DB::write()` reserved a sequence number before entering `MemTableManager::add()`. Concurrent writers could insert out of order, including across a memtable rotation. A newer SSTable could then contain an older sequence than a file below it, while level-0 reads assume newer file numbers win. `snapshot()` could also observe a reserved sequence before its write had landed.

For the first implementation, serialize sequence assignment through WAL append and memtable insertion with a DB write mutex. Take that mutex when creating a snapshot so it captures a completed prefix of writes. A later optimization can replace serialization with a committed-sequence watermark and explicit handling of in-flight writes.

**Acceptance:** two writers repeatedly overwrite the same key while flushes occur; captured snapshots remain stable during the DB instance's lifetime, and the latest value survives restart.

## 1. Define and validate level invariants

- Level 0 permits overlapping user-key ranges and is searched newest file first. Levels 1–6 require strictly disjoint user-key ranges in ascending key order. Adjacent files must satisfy `previous.largest_key < next.smallest_key`; two files sharing a boundary user key are overlapping even if their internal-key tags differ.
- Add validation in `VersionBuilder::build()` for the lower-level invariant, file-number uniqueness, metadata bounds, and output table readability. Reject an invalid edit before appending it to the manifest.
- Keep the current score policy initially: compact level 0 at four files; score levels 1–5 by bytes against their existing tenfold budgets. A score of at least 1 makes a level eligible. Level 6 is the bottom and has no destination.

**Acceptance:** malformed overlapping level-1 edits fail before manifest publication; valid versions still recover from the manifest.

## 2. Add a deterministic compaction picker

- Introduce a `CompactionPlan` containing a pinned source `Version`, source and destination levels, selected file handles, and the inclusive user-key range. Keep selection separate from execution so it can be tested without writing files.
- Start with all level-0 files when level 0 is selected. For a deeper level, pick one file at a time, advancing a per-level cursor so the first file is not selected forever.
- Select every overlapping file in the destination level. Expand the key range and repeat selection in both levels until the input sets stop changing. This closes over files that touch either boundary and leaves the output disjoint from unselected destination files.
- Prefer the highest score at or above 1. A pinned `Version` keeps input tables available while the merge runs.

**Acceptance:** picker tests cover overlapping level-0 files, inclusive boundary keys, gaps, cascading range expansion, empty levels, and levels 1–5.

## 3. Write sorted output files

- Construct a `MergingIterator` from selected inputs in read priority: newest level-0 files first, then source-level files, then destination-level files. For levels above 0, use concatenating iterators for each disjoint run.
- Stream entries into `SSTableBuilder` in internal-key order. `MergingIterator` emits duplicate exact `(user key, tag)` entries; emit only the first such entry, since the builder requires strictly increasing internal keys. Keep every distinct version and tombstone in this milestone.
- Add a configurable target output file size. Cut an output only **between user keys**, even when one key's versions exceed the target, so files in the destination level remain disjoint by user key. Populate `FileMetaData` from each finished builder.
- Register each output file number as pending before creating its file. The flush worker already sweeps unreferenced `.sst` files, so cleanup must retain pending compaction outputs while a merge runs. Remove the pending registration after commit or failure. The builder removes unfinished files; the compaction job removes finished outputs when it aborts. Unreferenced outputs left by a process crash are removed on open.
- Create output files exclusively so a recovered or stray file with the chosen number cannot be silently truncated.

**Acceptance:** direct merge tests verify exact-key tie priority, tombstones, multiple versions, output splitting, an oversized single key, and byte-for-byte reads before and after rewriting.

## 4. Install one atomic file-set change

- Build and sync every output before changing the manifest. Create one `VersionEdit` deleting all selected source and destination files and adding all outputs at the destination level.
- Serialize the edit with flush commits using `DB::_manifest_mutex`. Build output outside that lock. Immediately before commit, compare the selected file identities and destination overlaps with the current `Version`; discard and replan if another commit changed them. A new level-0 flush may remain above an in-flight deeper compaction.
- Call `VersionSet::logAndApply()` once for the swap. Release the compaction's pinned source `Version` before a follow-up obsolete-file sweep; `liveFiles()` retains inputs while a reader still holds an older `Version`.
- Harden the manifest append failure path before running background compactions: after a partial append or sync error, prevent further commits to that manifest until recovery or a safe rotation has restored a readable record stream.

**Acceptance:** after restart, the manifest names either the old input set or the complete new output set. Tests cover a failed output build, an uncommitted finished output, a failed manifest commit, and a reader holding the old `Version` through cleanup.

## 5. Schedule and observe background work

- Run at most one compaction job at a time. Wake it after a successful flush, after recovery, and after a compaction that leaves another level over budget. Give flushes priority; keep merge I/O outside the manifest lock so writes can continue.
- Join the worker during DB shutdown. Expose a wait method and last compaction error for deterministic tests and operational diagnosis. Retry transient failures with bounded backoff; do not spin on a persistent failure.
- Record input bytes, output bytes, files removed, elapsed time, and per-level run counts. Add level-0 write throttling only after measuring whether compaction falls behind sustained writes.

**Acceptance:** repeated writes trigger level-0-to-1 and deeper compactions; concurrent reads and writes stay correct; shutdown waits for the worker; the full test suite and a restart test pass.

## 6. Reclaim obsolete versions safely

The merge-only milestone reduced file count and read amplification while retaining old values and tombstones. `DB::snapshot()` now returns a registered, move-only RAII handle, and `get(key, Snapshot)` is the public historical-read API. Raw sequence-number reads are private. This is an API change: unrestricted historical reads and version reclamation cannot both be guaranteed. Snapshot registration and the compaction cutoff use the completed-write sequence established in step 0.

Compaction retains all versions newer than the oldest active snapshot and the first version at or below it for each user key; it drops older versions. A tombstone at or below the cutoff is dropped only when no deeper level can contain a value it must mask. User-key range checks against levels below the output level make that decision conservatively. The absence of active snapshots sets the cutoff to the completed current sequence. Snapshot release enables reclamation during a later compaction; it does not trigger an immediate rewrite.

## Suggested implementation order

1. Ordered writes and stable snapshots, with a same-key concurrency test.
2. Level validation and picker, with deterministic tests.
3. Merge-only output writer and file-splitting tests.
4. Atomic manifest installation, failure cleanup, and restart tests.
5. Background scheduling and concurrent integration tests.
6. Snapshot API and garbage collection rules.

Each step should leave `make test` passing and preserve the existing point-read behavior until the snapshot API change is made explicitly.
