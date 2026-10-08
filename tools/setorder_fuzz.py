#!/usr/bin/env python3
"""Generate set-order fixtures from the running CPython.

Mimics how engine.py manipulates Game.conns (a set of frozensets of
lighthouse positions) and new_tris (a set of 3-tuples), and records the
iteration order CPython produces. cpp/tests/test_pyset.cpp replays the
operations against the C++ emulation and requires identical order.

Output lines:
  G                           new game: conns = set()
  H x0 y0 x1 y1 <hash>        hash(frozenset(((x0,y0),(x1,y1))))
  T x0 y0 x1 y1 x2 y2 <hash>  hash(((x0,y0),(x1,y1),(x2,y2)))
  A x0 y0 x1 y1               conns.add(frozenset(...))
  R x y                       conns = set(c for c in conns if (x,y) not in c)
  E x0 y0 x1 y1 ...           expected iteration order (pairs as stored)
  N                           new_tris = set()
  M x0 y0 x1 y1 x2 y2         new_tris.add(...)
  F x0 y0 ...                 expected new_tris iteration order
"""
import random, sys

def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    games = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    rnd = random.Random(seed)
    out = sys.stdout
    for _ in range(games):
        w, h = rnd.randint(5, 60), rnd.randint(5, 30)
        n = rnd.randint(3, 40)
        lhs = list({(rnd.randrange(w), rnd.randrange(h)) for _ in range(n)})
        if len(lhs) < 3:
            continue
        for _ in range(5):
            a, b = rnd.sample(lhs, 2)
            out.write("H %d %d %d %d %d\n" % (a + b + (hash(frozenset((a, b))),)))
            c = rnd.choice(lhs)
            out.write("T %d %d %d %d %d %d %d\n" % (a + b + c + (hash((a, b, c)),)))
        conns = set()
        out.write("G\n")
        for _ in range(rnd.randint(1, 300)):
            r = rnd.random()
            if r < 0.7:
                a, b = rnd.sample(lhs, 2)
                conns.add(frozenset((a, b)))
                out.write("A %d %d %d %d\n" % (a + b))
            elif r < 0.95:
                p = rnd.choice(lhs)
                conns = set(i for i in conns if p not in i)
                out.write("R %d %d\n" % p)
            else:
                nt = set()
                out.write("N\n")
                for _ in range(rnd.randint(0, 12)):
                    t = tuple(rnd.sample(lhs, 3))
                    nt.add(t)
                    out.write("M %d %d %d %d %d %d\n" % (t[0] + t[1] + t[2]))
                out.write("F" + "".join(" %d %d %d %d %d %d" % (t[0] + t[1] + t[2]) for t in nt) + "\n")
            # The order inside each frozenset is irrelevant; emit a canonical
            # (sorted) pair so only the set order is compared.
            out.write("E" + "".join(" %d %d %d %d" % (min(c) + max(c)) for c in conns) + "\n")

if __name__ == "__main__":
    main()
