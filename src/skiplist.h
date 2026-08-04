#pragma once
#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <random>
#include <vector>

// Key_ must support the ordering skip-list traversal relies on.
template <typename T>
concept Orderable = requires(const T& a, const T& b) {
    { a < b } -> std::convertible_to<bool>;
    { a == b } -> std::convertible_to<bool>;
};

template <Orderable Key_, typename Value_>
class SkipList {
public:
    explicit SkipList(size_t maximum_level = 12);

    std::optional<Value_> search(const Key_& key) const;
    void insert(Key_ key, Value_ value);
    void remove(const Key_& key);

private:
    struct Node {
        // nullopt only for the head sentinel, which holds no key/value.
        std::optional<Key_> key;
        // Heap-allocated so copying/sharing a node never copies expensive values.
        std::shared_ptr<Value_> value;
        // forward[i] is the next node at level i; shared_ptr gives free, cycle-safe
        // cleanup since the list is a forward-only DAG.
        std::vector<std::shared_ptr<Node>> forward;

        // Sentinel constructor.
        explicit Node(size_t level) : key(std::nullopt), value(nullptr), forward(level) {}

        Node(Key_ k, Value_ v, size_t level)
            : key(std::move(k)),
              value(std::make_shared<Value_>(std::move(v))),
              forward(level) {}
    };

    // Returns a level in [1, _maximum_level] via repeated coin flips.
    size_t randomLevel() const;

    // Fills update[i] with the last node at level i whose key precedes `key`.
    std::vector<std::shared_ptr<Node>> findPredecessors(const Key_& key) const;

    size_t _maximum_level;
    size_t _level = 1;
    std::shared_ptr<Node> _head;
    mutable std::mt19937 _rng{ std::random_device{}() };
    mutable std::bernoulli_distribution _coin{0.5};
};

template <Orderable Key_, typename Value_>
SkipList<Key_, Value_>::SkipList(size_t maximum_level)
    : _maximum_level(maximum_level), _head(std::make_shared<Node>(maximum_level)) {}

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
