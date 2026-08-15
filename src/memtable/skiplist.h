#pragma once
#include "memtable/arena.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <string_view>

// Hard ceiling on skip-list height. Bounds the stack buffer findPredecessors
// uses, which is what keeps insert() free of per-operation heap allocation.
inline constexpr size_t kMaxSkipListLevel     = 16;
inline constexpr size_t kDefaultSkipListLevel = 12;

// Orders entries the way an LSM memtable needs: user key ascending by byte
// value, then tag (the packed seqnum|type) descending, so the newest version
// of a key is the first one seek() lands on.
//
// Byte ordering matches std::string's operator< -- char_traits<char>::compare
// has unsigned-byte semantics -- so string keys sort exactly as before.
struct InternalKeyComparator {
    int operator()(std::string_view a_key, uint64_t a_tag,
                   std::string_view b_key, uint64_t b_tag) const noexcept {
        if (const int c = a_key.compare(b_key); c != 0) {
            return c;
        }
        if (a_tag > b_tag) return -1;
        if (a_tag < b_tag) return 1;
        return 0;
    }
};

// An ordered map of byte-string keys to byte-string values, stored entirely in
// an Arena.
//
// Each entry is one arena allocation laid out as a fixed header, the node's
// forward pointers, then the key and value bytes inline:
//
//     +0   tag        uint64   packed seqnum|type
//     +8   key_len    uint32
//     +12  value_len  uint32
//     +16  level      uint32
//     +20  reserved   uint32
//     +24  forward[level]      Node*, 8 bytes each
//     ...  key bytes (estimated to be ~50 bytes in practice)
//     ...  value bytes (estimated to be ~100 bytes in practice)
//
// Lifetime: nodes live exactly as long as the Arena passed to the constructor,
// which must outlive the list. Nothing stored here has a destructor, and
// nothing is ever individually freed -- remove() only unlinks.
//
// Thread-safety: none. Callers synchronize externally (MemTable holds a
// shared_mutex). The Arena underneath is thread-safe on its own.
template <typename Comparator = InternalKeyComparator>
class SkipList {
private:
    struct Node {
        uint64_t tag;
        uint32_t key_len;
        uint32_t value_len;
        uint32_t level;
        uint32_t reserved; // keeps the layout explicit; forward[] must land at +24
    };
    static_assert(sizeof(Node) == 24, "node header must be 24 bytes");
    static_assert(alignof(Node) == 8, "forward[] following the header must stay 8-aligned");

    // forward[], key bytes, and value bytes all trail the header in the same
    // allocation, so their addresses are computed rather than stored.
    static Node** forwardOf(Node* node) noexcept {
        return reinterpret_cast<Node**>(reinterpret_cast<char*>(node) + sizeof(Node));
    }
    static Node* const* forwardOf(const Node* node) noexcept {
        return reinterpret_cast<Node* const*>(reinterpret_cast<const char*>(node) + sizeof(Node));
    }
    static const char* keyOf(const Node* node) noexcept {
        return reinterpret_cast<const char*>(node) + sizeof(Node) + node->level * sizeof(Node*);
    }
    static std::string_view keyView(const Node* node) noexcept {
        return { keyOf(node), node->key_len };
    }
    static std::string_view valueView(const Node* node) noexcept {
        return { keyOf(node) + node->key_len, node->value_len };
    }

public:
    // `arena` must outlive this list.
    explicit SkipList(Arena& arena, size_t maximum_level = kDefaultSkipListLevel)
        : _arena(&arena), _maximum_level(maximum_level) {
        assert(maximum_level >= 1 && maximum_level <= kMaxSkipListLevel);
        // Head sentinel: no key or value, full height so every level has a
        // starting point. Its key is never compared -- traversal only ever
        // looks at a node's successors.
        _head = newNode({}, 0, {}, static_cast<uint32_t>(maximum_level));
    }

    SkipList(const SkipList&)            = delete;
    SkipList& operator=(const SkipList&) = delete;

    // Forward-only, read-only cursor over the level-0 chain in ascending
    // order. Default-constructed iterators are invalid (end). Valid only
    // while the backing Arena lives.
    class Iterator {
    public:
        Iterator() = default;

        [[nodiscard]] bool valid() const noexcept { return _node != nullptr; }
        void next() { _node = forwardOf(_node)[0]; }

        [[nodiscard]] std::string_view key() const noexcept { return keyView(_node); }
        [[nodiscard]] uint64_t tag() const noexcept { return _node->tag; }
        [[nodiscard]] std::string_view value() const noexcept { return valueView(_node); }

    private:
        friend class SkipList;
        explicit Iterator(Node* node) noexcept : _node(node) { }

        Node* _node = nullptr;
    };

    // Copies `key` and `value` into the arena. Always links a new node; an
    // entry with an identical (key, tag) is shadowed rather than replaced,
    // because inline values are variably sized and can't be overwritten in
    // place. The new node lands ahead of any equal one, so lookups see it
    // first -- which is the ordering an LSM wants anyway, since a later write
    // carries a higher sequence number.
    void insert(std::string_view key, uint64_t tag, std::string_view value) {
        Node* update[kMaxSkipListLevel];
        findPredecessors(key, tag, update);

        const uint32_t level = randomLevel();
        Node* node  = newNode(key, tag, value, level);
        Node** next = forwardOf(node);

        for (uint32_t i = 0; i < level; ++i) {
            Node** prev = forwardOf(update[i]);
            next[i] = prev[i];
            prev[i] = node;
        }
    }

    // Unlinks the first entry matching (key, tag) exactly. Its bytes are not
    // reclaimed -- an arena only gives memory back when it is destroyed.
    void remove(std::string_view key, uint64_t tag) {
        Node* update[kMaxSkipListLevel];
        findPredecessors(key, tag, update);

        Node* target = forwardOf(update[0])[0];
        if (target == nullptr || _compare(keyView(target), target->tag, key, tag) != 0) {
            return;
        }
        for (uint32_t i = 0; i < target->level; ++i) {
            Node** prev = forwardOf(update[i]);
            if (prev[i] == target) {
                prev[i] = forwardOf(target)[i];
            }
        }
    }

    // Value for an exact (key, tag) match, or nullopt. The returned view
    // points into the arena.
    [[nodiscard]] std::optional<std::string_view> search(std::string_view key, uint64_t tag) const {
        Node* node = firstAtLeast(key, tag);
        if (node == nullptr || _compare(keyView(node), node->tag, key, tag) != 0) {
            return std::nullopt;
        }
        return valueView(node);
    }

    // First entry in the list.
    [[nodiscard]] Iterator begin() const { return Iterator(forwardOf(_head)[0]); }

    // First entry ordering at or after (key, tag), i.e. lower_bound. Invalid
    // if no such entry exists.
    [[nodiscard]] Iterator seek(std::string_view key, uint64_t tag) const {
        return Iterator(firstAtLeast(key, tag));
    }

private:
    // Carves one allocation and writes the header, an all-null forward array,
    // and the key/value bytes into it.
    Node* newNode(std::string_view key, uint64_t tag, std::string_view value, uint32_t level) {
        const size_t bytes =
            sizeof(Node) + static_cast<size_t>(level) * sizeof(Node*) + key.size() + value.size();

        char* raw  = _arena->allocate(bytes);
        Node* node = reinterpret_cast<Node*>(raw);

        node->tag       = tag;
        node->key_len   = static_cast<uint32_t>(key.size());
        node->value_len = static_cast<uint32_t>(value.size());
        node->level     = level;
        node->reserved  = 0;

        Node** next = forwardOf(node);
        for (uint32_t i = 0; i < level; ++i) {
            next[i] = nullptr;
        }

        char* bytes_at = raw + sizeof(Node) + static_cast<size_t>(level) * sizeof(Node*);
        if (!key.empty()) {
            std::memcpy(bytes_at, key.data(), key.size());
        }
        if (!value.empty()) {
            std::memcpy(bytes_at + key.size(), value.data(), value.size());
        }
        return node;
    }

    // Returns a level in [1, _maximum_level] via repeated coin flips:
    // increment on heads, stop on tails.
    [[nodiscard]] uint32_t randomLevel() {
        uint32_t level = 1;
        while (level < _maximum_level && _coin(_rng)) {
            ++level;
        }
        return level;
    }

    // First node ordering at or after (key, tag), or null.
    [[nodiscard]] Node* firstAtLeast(std::string_view key, uint64_t tag) const {
        Node* cur = _head;
        for (size_t i = _maximum_level; i-- > 0;) {
            for (;;) {
                Node* next = forwardOf(cur)[i];
                if (next == nullptr || _compare(keyView(next), next->tag, key, tag) >= 0) {
                    break;
                }
                cur = next;
            }
        }
        return forwardOf(cur)[0];
    }

    // Fills update[i] with the last node at level i ordering strictly before
    // (key, tag). Writes all _maximum_level entries -- levels the list hasn't
    // grown into simply get the head -- so insert() needs no special case for
    // a node taller than anything currently linked.
    //
    // `update` must have room for _maximum_level entries; callers pass a
    // kMaxSkipListLevel stack array, which is why this allocates nothing.
    void findPredecessors(std::string_view key, uint64_t tag, Node** update) const {
        Node* cur = _head;
        for (size_t i = _maximum_level; i-- > 0;) {
            for (;;) {
                Node* next = forwardOf(cur)[i];
                if (next == nullptr || _compare(keyView(next), next->tag, key, tag) >= 0) {
                    break;
                }
                cur = next;
            }
            update[i] = cur;
        }
    }

    Arena* _arena;
    size_t _maximum_level;
    Node*  _head;

    Comparator _compare{};

    std::mt19937 _rng{ std::random_device{}() };
    std::bernoulli_distribution _coin{ 0.5 };
};
