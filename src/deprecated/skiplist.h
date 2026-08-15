#pragma once
#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <random>
#include <vector>

// DEPRECATED -- the pre-arena skip list, kept only so the benchmark can
// measure the arena rewrite against it. Superseded by src/memtable/skiplist.h.
//
// Every entry here costs five heap allocations: the node (via make_shared,
// which also carries a control block), the value (a second make_shared with a
// second control block), the forward vector's backing array, and the key's and
// value's own string buffers. Every pointer hop copies a shared_ptr, so
// traversal pays two atomic refcount operations per step.
//
// It also cannot be destroyed at scale: _head owns the level-0 chain through
// nested shared_ptrs, so ~SkipList recurses once per node and overflows the
// 8 MB main-thread stack at roughly 131k entries. That never surfaced in
// practice only because a 4 MB memtable holds ~22k entries, but it is a latent
// crash for any larger threshold. The arena rewrite has no such limit --
// teardown is a munmap per block.
//
// Everything lives in namespace deprecated so this header can be included
// alongside the current one in a single translation unit.
namespace deprecated {

// Key_ must support the ordering skip-list traversal relies on.
template <typename T>
concept Orderable = requires(const T& a, const T& b) {
    { a < b } -> std::convertible_to<bool>;
    { a == b } -> std::convertible_to<bool>;
};

template <Orderable Key_, typename Value_>
class SkipList {
public:
    [[deprecated("pre-arena skip list; use ::SkipList from memtable/skiplist.h")]]
    explicit SkipList(size_t maximum_level = 12);

    [[deprecated("pre-arena skip list; use ::SkipList from memtable/skiplist.h")]]
    [[nodiscard]] std::optional<Value_> search(const Key_& key) const;

    [[deprecated("pre-arena skip list; use ::SkipList from memtable/skiplist.h")]]
    void insert(Key_ key, Value_ value);

    [[deprecated("pre-arena skip list; use ::SkipList from memtable/skiplist.h")]]
    void remove(const Key_& key);

private:
    struct Node {
        // nullopt only for the head sentinel, which holds no key/value.
        std::optional<Key_> key;
        std::shared_ptr<Value_> value;

        // forward[i] is the next node at level i
        std::vector<std::shared_ptr<Node>> forward;

        // Sentinel constructor.
        [[deprecated("pre-arena node layout")]]
        explicit Node(size_t level) : key(std::nullopt), value(nullptr), forward(level) { }

        [[deprecated("pre-arena node layout")]]
        Node(Key_ k, Value_ v, size_t level)
            : key(std::move(k)),
              value(std::make_shared<Value_>(std::move(v))),
              forward(level) {}
    };

public:
    // Forward-only, read-only cursor over the level-0 chain, in ascending
    // key order. Default-constructed iterators are invalid (end).
    class Iterator {
    public:
        Iterator() = default;

        [[deprecated("pre-arena iterator")]]
        [[nodiscard]] bool valid() const noexcept { return _node != nullptr; }

        [[deprecated("pre-arena iterator")]]
        void next() { _node = _node->forward[0]; }

        [[deprecated("pre-arena iterator")]]
        [[nodiscard]] const Key_& key() const { return *_node->key; }

        [[deprecated("pre-arena iterator")]]
        [[nodiscard]] const Value_& value() const { return *_node->value; }

    private:
        friend class SkipList;
        explicit Iterator(std::shared_ptr<Node> node) : _node(std::move(node)) { }

        std::shared_ptr<Node> _node;
    };

    // Returns an iterator positioned at the smallest key.
    [[deprecated("pre-arena skip list; use ::SkipList from memtable/skiplist.h")]]
    [[nodiscard]] Iterator begin() const;

    // Returns an iterator positioned at the first node whose key is >= key
    // (i.e. lower_bound). Invalid if no such node exists.
    [[deprecated("pre-arena skip list; use ::SkipList from memtable/skiplist.h")]]
    [[nodiscard]] Iterator seek(const Key_& key) const;

private:
    // Returns a level in [1, _maximum_level] via repeated coin flips.
    // Increments the level if heads, stops if tails.
    [[deprecated("pre-arena skip list internals")]]
    [[nodiscard]] size_t randomLevel() const;

    // Fills update[i] with the last node at level i whose key precedes `key`.
    // Heap-allocates a vector of _maximum_level shared_ptrs on every call.
    [[deprecated("pre-arena skip list internals")]]
    std::vector<std::shared_ptr<Node>> findPredecessors(const Key_& key) const;

    size_t _maximum_level;

    // Number of levels in the skiplist
    size_t _level = 1;
    std::shared_ptr<Node> _head;

    // Declared mutable because randomLevel() is const.
    mutable std::mt19937 _rng{ std::random_device{}() };
    mutable std::bernoulli_distribution _coin{0.5};
};

template <Orderable Key_, typename Value_>
SkipList<Key_, Value_>::SkipList(size_t maximum_level)
    : _maximum_level(maximum_level), _head(std::make_shared<Node>(maximum_level)) { }

template <Orderable Key_, typename Value_>
size_t SkipList<Key_, Value_>::randomLevel() const {
    size_t level = 1;
    while (level < _maximum_level && _coin(_rng)) {
        ++level;
    }
    return level;
}

template <Orderable Key_, typename Value_>
std::vector<std::shared_ptr<typename SkipList<Key_, Value_>::Node>>
SkipList<Key_, Value_>::findPredecessors(const Key_& key) const {
    std::vector<std::shared_ptr<Node>> update(_maximum_level, _head);
    std::shared_ptr<Node> cur = _head;
    for (size_t i = _level; i-- > 0;) {
        while (cur->forward[i] && cur->forward[i]->key < key) {
            cur = cur->forward[i];
        }
        update[i] = cur;
    }
    return update;
}

template <Orderable Key_, typename Value_>
std::optional<Value_> SkipList<Key_, Value_>::search(const Key_& key) const {
    std::shared_ptr<Node> cur = _head;
    for (size_t i = _level; i-- > 0;) {
        while (cur->forward[i] && cur->forward[i]->key < key) {
            cur = cur->forward[i];
        }
    }
    cur = cur->forward[0];
    if (cur && cur->key == key) {
        return *cur->value;
    }
    return std::nullopt;
}

template <Orderable Key_, typename Value_>
typename SkipList<Key_, Value_>::Iterator SkipList<Key_, Value_>::begin() const {
    return Iterator(_head->forward[0]);
}

template <Orderable Key_, typename Value_>
typename SkipList<Key_, Value_>::Iterator SkipList<Key_, Value_>::seek(const Key_& key) const {
    std::shared_ptr<Node> cur = _head;
    for (size_t i = _level; i-- > 0;) {
        while (cur->forward[i] && cur->forward[i]->key < key) {
            cur = cur->forward[i];
        }
    }
    return Iterator{ cur->forward[0] };
}

template <Orderable Key_, typename Value_>
void SkipList<Key_, Value_>::insert(Key_ key, Value_ value) {
    std::vector<std::shared_ptr<Node>> update = findPredecessors(key);

    std::shared_ptr<Node> existing = update[0]->forward[0];
    if (existing && existing->key == key) {
        existing->value = std::make_shared<Value_>(std::move(value));
        return;
    }

    size_t new_level = randomLevel();
    if (new_level > _level) {
        for (size_t i = _level; i < new_level; ++i) {
            update[i] = _head;
        }
        _level = new_level;
    }

    auto node = std::make_shared<Node>(std::move(key), std::move(value), new_level);
    for (size_t i = 0; i < new_level; ++i) {
        node->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = node;
    }
}

template <Orderable Key_, typename Value_>
void SkipList<Key_, Value_>::remove(const Key_& key) {
    std::vector<std::shared_ptr<Node>> update = findPredecessors(key);

    std::shared_ptr<Node> target = update[0]->forward[0];
    if (!target || !(target->key == key)) {
        return;
    }

    for (size_t i = 0; i < _level; ++i) {
        if (update[i]->forward[i] != target) {
            break;
        }
        update[i]->forward[i] = target->forward[i];
    }

    while (_level > 1 && !_head->forward[_level - 1]) {
        --_level;
    }
}

} // namespace deprecated
