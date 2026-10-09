#!/usr/bin/python
import math
from numba import jit, prange
import numpy as np

@jit(nopython=True,cache=True)
def dist(a, b):
    x0, y0 = a
    x1, y1 = b
    return math.sqrt((x0-x1)**2 + (y0-y1)**2)

@jit(nopython=True,cache=True)
def orient2d(a, b, c):
    return (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])

@jit(nopython=True,cache=True)
def colinear(a, b, c):
    return orient2d(a, b, c) == 0

@jit(nopython=True,cache=True)
def intersect(j, k):
    j1, j2 = j
    k1, k2 = k
    return (
        orient2d(k1, k2, j1) * orient2d(k1, k2, j2) < 0 and
        orient2d(j1, j2, k1) * orient2d(j1, j2, k2) < 0)

@jit(nopython=True,cache=True)
def _bias(p0, p1):
    if (p0[1] == p1[1] and p0[0] > p1[0]) or p0[1] > p1[1]:
        return 0
    else:
        return -1

@jit(nopython=True,cache=True,parallel=True)
def render_parallel(points):
    v0, v1, v2 = points
    if orient2d(v0, v1, v2) < 0:
        v0, v1 = v1, v0
    x0 = min(v0[0], v1[0], v2[0])
    x1 = max(v0[0], v1[0], v2[0])
    y0 = min(v0[1], v1[1], v2[1])
    y1 = max(v0[1], v1[1], v2[1])
    # Pre-allocate based on the loop bounds
    width = x1 - x0 + 1
    height = y1 - y0 + 1
    results = np.empty((height * width, 2), dtype=np.int32)
    valid_mask = np.zeros(height * width, dtype=np.bool_)

    for y in prange(y0, y1+1):
        for x in prange(x0, x1+1):
            idx = (y - y0) * width + (x - x0)  # Compute deterministic index
            p = x, y
            w0 = orient2d(v1, v2, p) + _bias(v1, v2)
            w1 = orient2d(v2, v0, p) + _bias(v2, v0)
            w2 = orient2d(v0, v1, p) + _bias(v0, v1)
            if w0 >= 0 and w1 >= 0 and w2 >= 0:
                results[idx] = p
                valid_mask[idx] = True

    # Extract only valid results in deterministic order
    return results[valid_mask]

def render(points):
    render_results=render_parallel(points)
    for p in render_results:
        yield p

@jit(nopython=True,cache=True)
def renderold(points):
    v0, v1, v2 = points
    if orient2d(v0, v1, v2) < 0:
        v0, v1 = v1, v0
    x0 = min(v0[0], v1[0], v2[0])
    x1 = max(v0[0], v1[0], v2[0])
    y0 = min(v0[1], v1[1], v2[1])
    y1 = max(v0[1], v1[1], v2[1])
    for y in range(y0, y1+1):
        for x in range(x0, x1+1):
            p = x, y
            w0 = orient2d(v1, v2, p) + _bias(v1, v2)
            w1 = orient2d(v2, v0, p) + _bias(v2, v0)
            w2 = orient2d(v0, v1, p) + _bias(v0, v1)
            if w0 >= 0 and w1 >= 0 and w2 >= 0:
                yield p

@jit(nopython=True,cache=True)
def _rendertest(points):
    w = 1+max(p[0] for p in points)
    h = 1+max(p[1] for p in points)
    s = [["."] * w for i in range(h)]
    for x,y in render(points):
        s[y][x] = "#"
    for i,(x,y) in enumerate(points):
        s[y][x] = ("ABC","abc")[s[y][x] == "."][i]
    for l in s[::-1]:
        print("".join(l))
    print()

if __name__ == "__main__":
    assert orient2d((0,0),(0,1),(1,0)) < 0
    assert orient2d((0,1),(1,0),(0,0)) < 0
    assert orient2d((1,0),(0,0),(0,1)) < 0
    assert orient2d((0,1),(0,0),(1,0)) > 0
    assert orient2d((1,0),(0,1),(0,0)) > 0
    assert orient2d((0,0),(1,0),(0,1)) > 0
    assert not intersect(((0,0),(2,2)),((4,1),(1,4)))
    assert not intersect(((0,0),(2,2)),((3,1),(1,3)))
    assert intersect(((0,0),(2,2)),((2,1),(1,2))) 
    _rendertest(((0,0),(5,0),(0,5)))
    _rendertest(((5,5),(5,0),(0,5)))
