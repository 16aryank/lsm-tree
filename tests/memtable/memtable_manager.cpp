// Promotion (mutable -> immutable), background-flush handoff, and
// mutable-then-immutable read ordering for MemTableManager.
//
// No WAL factory is exercised here: WalWriter's real constructor lives in
// src/wal/wal_writer.cpp, which this test's build does not link (tests
// under tests/memtable/ only link src/memtable/*.cpp, per the Makefile),
// and it does not currently compile on macOS (fdatasync is Linux-only).
// MemTableManager's WAL-rotation hook (an injected factory, invoked once at
// construction and again on every promotion) was exercised manually against
// a patched WalWriter; wiring a real one back in belongs to whoever fixes
// that portability bug.

#include "src/memtable/memtable_manager.h"
#include <cassert>
#include <string>

int main() {
    MemTableManager<std::string, std::string> mgr(64); // tiny threshold to force promotion quickly

    mgr.add(1, ValueType::kValue, "a", "small");
    assert(!mgr.hasPendingFlush());
    assert(mgr.get("a", 10) == "small");
    assert(!mgr.get("missing", 10).has_value());

    // Push the mutable table over its threshold; this promotes it to
    // immutable and kicks off a background flush.
    mgr.add(2, ValueType::kValue, "b", std::string(200, 'x'));

    // Deterministically wait for the background flush instead of racing it.
    mgr.waitForPendingFlush();
    assert(!mgr.hasPendingFlush());

    // Writes after promotion land in the fresh mutable table.
    mgr.add(3, ValueType::kValue, "c", "fresh");
    assert(mgr.get("c", 10) == "fresh");

    return 0;
}
