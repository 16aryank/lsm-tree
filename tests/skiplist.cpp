// Basic insertions, removal, and search tests for skiplist

#include "src/skiplist.h"
#include <string>
#include <cassert>

int main() {
    SkipList<int, std::string> sl;
    sl.insert(5, "five");
    sl.insert(3, "three");
    sl.insert(8, "eight");

    // Assertions
    assert(sl.search(5) == "five");
    assert(sl.search(3) == "three");
    assert(!sl.search(4).has_value());

    sl.insert(5, "FIVE");
    assert(sl.search(5) == "FIVE");
    sl.remove(3);

    assert(!sl.search(3).has_value());
    assert(sl.search(8) == "eight");

    return 0;
}