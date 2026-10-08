// Emulation of CPython 3.10 hashing and set iteration order.
//
// The official engine keeps connections in a Python set of frozensets, and
// the order in which that set is iterated leaks into the turn message (the
// "connections" list of each lighthouse). To produce byte-identical output
// we reproduce Objects/setobject.c (3.10) for the operations the engine uses:
// add, membership and building a new set from an iterable. Sets are never
// shrunk with discard() here (the engine rebuilds them instead), but the
// emulation is still exact for that case because dummies never appear.
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace py {

using hash_t = int64_t;
using uhash_t = uint64_t;

// hash() of a Python int.
inline hash_t hash_int(int64_t v) {
    const uint64_t modulus = (uint64_t(1) << 61) - 1;
    uint64_t a = v < 0 ? uint64_t(0) - uint64_t(v) : uint64_t(v);
    a %= modulus;
    hash_t h = v < 0 ? -hash_t(a) : hash_t(a);
    return h == -1 ? -2 : h;
}

// hash() of a tuple, given the hashes of its items (Objects/tupleobject.c).
inline hash_t hash_tuple(const hash_t* items, size_t len) {
    const uhash_t P1 = 11400714785074694791ULL;
    const uhash_t P2 = 14029467366897019727ULL;
    const uhash_t P5 = 2870177450012600261ULL;
    uhash_t acc = P5;
    for (size_t i = 0; i < len; i++) {
        acc += uhash_t(items[i]) * P2;
        acc = (acc << 31) | (acc >> 33);
        acc *= P1;
    }
    acc += uhash_t(len) ^ (P5 ^ 3527539ULL);
    if (acc == uhash_t(-1)) return 1546275796;
    return hash_t(acc);
}

inline hash_t hash_pos(int64_t x, int64_t y) {
    hash_t items[2] = {hash_int(x), hash_int(y)};
    return hash_tuple(items, 2);
}

inline uhash_t shuffle_bits(uhash_t h) {
    return ((h ^ 89869747ULL) ^ (h << 16)) * 3644798167ULL;
}

// hash() of a frozenset with the given (distinct) item hashes. The result is
// independent of the table layout: null and dummy entries cancel out.
inline hash_t hash_frozenset(const hash_t* items, size_t len) {
    uhash_t h = 0;
    for (size_t i = 0; i < len; i++) h ^= shuffle_bits(uhash_t(items[i]));
    h ^= (uhash_t(len) + 1) * 1927868237ULL;
    h ^= (h >> 11) ^ (h >> 25);
    h = h * 69069U + 907133923ULL;
    if (h == uhash_t(-1)) h = 590923713ULL;
    return hash_t(h);
}

// A set of K with CPython's table layout. Iteration (begin/end over items())
// follows the table order exactly like `for x in some_set`.
template <class K>
class Set {
public:
    struct Entry {
        hash_t hash = 0;
        bool active = false;
        K key{};
    };

    Set() : table_(kMinSize) {}

    size_t size() const { return used_; }
    bool empty() const { return used_ == 0; }

    bool contains(const K& key, hash_t hash) const {
        size_t mask = table_.size() - 1;
        size_t perturb = size_t(hash);
        size_t i = size_t(hash) & mask;
        while (true) {
            size_t probes = (i + kLinearProbes <= mask) ? kLinearProbes : 0;
            size_t j = i;
            while (true) {
                const Entry& e = table_[j];
                if (!e.active) return false;
                if (e.hash == hash && e.key == key) return true;
                if (probes-- == 0) break;
                j++;
            }
            perturb >>= kPerturbShift;
            i = (i * 5 + 1 + perturb) & mask;
        }
    }

    void add(const K& key, hash_t hash) {
        size_t mask = table_.size() - 1;
        size_t perturb = size_t(hash);
        size_t i = size_t(hash) & mask;
        while (true) {
            size_t probes = (i + kLinearProbes <= mask) ? kLinearProbes : 0;
            size_t j = i;
            while (true) {
                Entry& e = table_[j];
                if (!e.active) {
                    e.active = true;
                    e.hash = hash;
                    e.key = key;
                    used_++;
                    if (used_ * 5 < mask * 3) return;
                    resize(used_ > 50000 ? used_ * 2 : used_ * 4);
                    return;
                }
                if (e.hash == hash && e.key == key) return;
                if (probes-- == 0) break;
                j++;
            }
            perturb >>= kPerturbShift;
            i = (i * 5 + 1 + perturb) & mask;
        }
    }

    // set(x for x in self if pred(x))
    template <class Pred>
    Set filtered(Pred pred) const {
        Set out;
        for (const Entry& e : table_)
            if (e.active && pred(e.key)) out.add(e.key, e.hash);
        return out;
    }

    template <class F>
    void for_each(F f) const {
        for (const Entry& e : table_)
            if (e.active) f(e.key);
    }

    std::vector<K> items() const {
        std::vector<K> out;
        out.reserve(used_);
        for (const Entry& e : table_)
            if (e.active) out.push_back(e.key);
        return out;
    }

private:
    static constexpr size_t kMinSize = 8;
    static constexpr size_t kLinearProbes = 9;
    static constexpr int kPerturbShift = 5;

    void resize(size_t minused) {
        size_t newsize = kMinSize;
        while (newsize <= minused) newsize <<= 1;
        std::vector<Entry> old(newsize);
        old.swap(table_);
        size_t mask = newsize - 1;
        for (Entry& e : old)
            if (e.active) insert_clean(mask, std::move(e));
    }

    void insert_clean(size_t mask, Entry&& entry) {
        size_t perturb = size_t(entry.hash);
        size_t i = size_t(entry.hash) & mask;
        while (true) {
            if (!table_[i].active) {
                table_[i] = std::move(entry);
                return;
            }
            if (i + kLinearProbes <= mask) {
                for (size_t j = 1; j <= kLinearProbes; j++) {
                    if (!table_[i + j].active) {
                        table_[i + j] = std::move(entry);
                        return;
                    }
                }
            }
            perturb >>= kPerturbShift;
            i = (i * 5 + 1 + perturb) & mask;
        }
    }

    std::vector<Entry> table_;
    size_t used_ = 0;  // == fill, there are never dummies
};

}  // namespace py
