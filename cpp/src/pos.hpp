// Board positions and the hashed key types the engine stores in sets.
#pragma once

#include "pyset.hpp"

struct Pos {
    int x = 0, y = 0;
    bool operator==(const Pos& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Pos& o) const { return !(*this == o); }
};

inline py::hash_t hash_of(const Pos& p) { return py::hash_pos(p.x, p.y); }

// frozenset((a, b)): unordered pair. `a` and `b` keep the order they were
// created with, which is also the iteration order inside the frozenset.
struct Conn {
    Pos a, b;
    bool operator==(const Conn& o) const {
        return (a == o.a && b == o.b) || (a == o.b && b == o.a);
    }
    bool has(const Pos& p) const { return a == p || b == p; }
    const Pos& other(const Pos& p) const { return a == p ? b : a; }
};

inline py::hash_t hash_of(const Conn& c) {
    py::hash_t h[2] = {hash_of(c.a), hash_of(c.b)};
    return py::hash_frozenset(h, 2);
}

// (orig, dest, third) triangle key.
struct Tri {
    Pos a, b, c;
    bool operator==(const Tri& o) const { return a == o.a && b == o.b && c == o.c; }
    bool has(const Pos& p) const { return a == p || b == p || c == p; }
};

inline py::hash_t hash_of(const Tri& t) {
    py::hash_t h[3] = {hash_of(t.a), hash_of(t.b), hash_of(t.c)};
    return py::hash_tuple(h, 3);
}
