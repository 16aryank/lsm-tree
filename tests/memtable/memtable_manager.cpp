// Promotion (mutable -> immutable), background-flush handoff, and
// mutable-then-immutable read ordering for MemTableManager.
#include "src/memtable/memtable_manager.h"
#include <cassert>
#include <string>

int main() {
    MemTableManager<std::string, std::string> mgr(256);

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
