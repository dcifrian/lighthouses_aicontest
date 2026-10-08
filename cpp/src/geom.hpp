// Port of engine/geom.py (integer geometry and triangle rasterisation).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "pos.hpp"

namespace geom {

inline double dist(Pos a, Pos b) {
    double dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

inline int64_t orient2d(Pos a, Pos b, Pos c) {
    return int64_t(b.x - a.x) * (c.y - a.y) - int64_t(c.x - a.x) * (b.y - a.y);
}

inline bool colinear(Pos a, Pos b, Pos c) { return orient2d(a, b, c) == 0; }

// Strict intersection of segments j1-j2 and k1-k2.
inline bool intersect(Pos j1, Pos j2, Pos k1, Pos k2) {
    return orient2d(k1, k2, j1) * orient2d(k1, k2, j2) < 0 &&
           orient2d(j1, j2, k1) * orient2d(j1, j2, k2) < 0;
}

inline int bias(Pos p0, Pos p1) {
    return ((p0.y == p1.y && p0.x > p1.x) || p0.y > p1.y) ? 0 : -1;
}

// Cells inside the triangle (OpenGL top-left fill rule), in geom.render order.
template <class F>
void render(Pos v0, Pos v1, Pos v2, F&& emit) {
    if (orient2d(v0, v1, v2) < 0) std::swap(v0, v1);
    int x0 = std::min({v0.x, v1.x, v2.x}), x1 = std::max({v0.x, v1.x, v2.x});
    int y0 = std::min({v0.y, v1.y, v2.y}), y1 = std::max({v0.y, v1.y, v2.y});
    int b12 = bias(v1, v2), b20 = bias(v2, v0), b01 = bias(v0, v1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            Pos p{x, y};
            if (orient2d(v1, v2, p) + b12 >= 0 && orient2d(v2, v0, p) + b20 >= 0 &&
                orient2d(v0, v1, p) + b01 >= 0)
                emit(p);
        }
}

}  // namespace geom
