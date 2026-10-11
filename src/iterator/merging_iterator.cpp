#include "iterator/merging_iterator.h"
#include "key.h"

#include <utility>

MergingIterator::MergingIterator(std::vector<std::unique_ptr<InternalIterator>> children)
    : _children(std::move(children)) {
    findSmallest();
}

void MergingIterator::findSmallest() {
    const InternalKeyComparator compare;
    InternalIterator* smallest = nullptr;

    for (const auto& child : _children) {
        if (!child->valid()) {
            continue;
        }
        // Strictly less, so on a tie the earlier child keeps the slot. That is
        // what makes "newest input first" the caller's lever over duplicates.
        if (smallest == nullptr ||
            compare(child->key(), child->tag(), smallest->key(), smallest->tag()) < 0) {
                smallest = child.get();
        }
    }
    _current = smallest;
}

void MergingIterator::seek(std::string_view key, uint64_t tag) {
    for (const auto& child : _children) {
        child->seek(key, tag);
    }
    findSmallest();
}

void MergingIterator::seekToFirst() {
    for (const auto& child : _children) {
        child->seekToFirst();
    }
    findSmallest();
}

void MergingIterator::next() {
    if (_current == nullptr) {
        return;
    }
    _current->next();
    findSmallest();
}

std::unique_ptr<InternalIterator> newMergingIterator(
    std::vector<std::unique_ptr<InternalIterator>> children) {
    return std::make_unique<MergingIterator>(std::move(children));
}
