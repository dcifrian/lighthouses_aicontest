// Replays tools/setorder_fuzz.py fixtures against the CPython set emulation.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/pos.hpp"

static std::string dump(const py::Set<Conn>& s) {
    std::string out = "E";
    s.for_each([&](const Conn& c) {
        Pos lo = c.a, hi = c.b;
        if (hi.x < lo.x || (hi.x == lo.x && hi.y < lo.y)) std::swap(lo, hi);
        out += " " + std::to_string(lo.x) + " " + std::to_string(lo.y) + " " +
               std::to_string(hi.x) + " " + std::to_string(hi.y);
    });
    return out;
}

static std::string dump(const py::Set<Tri>& s) {
    std::string out = "F";
    s.for_each([&](const Tri& t) {
        for (const Pos* p : {&t.a, &t.b, &t.c})
            out += " " + std::to_string(p->x) + " " + std::to_string(p->y);
    });
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: test_pyset FIXTURE\n";
        return 2;
    }
    std::ifstream in(argv[1]);
    std::string line;
    py::Set<Conn> conns;
    py::Set<Tri> tris;
    long lineno = 0, checks = 0;
    while (std::getline(in, line)) {
        lineno++;
        std::istringstream ss(line);
        char op;
        ss >> op;
        std::string got;
        switch (op) {
            case 'H': {
                Conn c;
                long long h;
                ss >> c.a.x >> c.a.y >> c.b.x >> c.b.y >> h;
                if (hash_of(c) != h) got = "hash " + std::to_string(hash_of(c));
                break;
            }
            case 'T': {
                Tri t;
                long long h;
                ss >> t.a.x >> t.a.y >> t.b.x >> t.b.y >> t.c.x >> t.c.y >> h;
                if (hash_of(t) != h) got = "hash " + std::to_string(hash_of(t));
                break;
            }
            case 'A': {
                Conn c;
                ss >> c.a.x >> c.a.y >> c.b.x >> c.b.y;
                conns.add(c, hash_of(c));
                break;
            }
            case 'R': {
                Pos p;
                ss >> p.x >> p.y;
                conns = conns.filtered([&](const Conn& c) { return !c.has(p); });
                break;
            }
            case 'G': conns = py::Set<Conn>(); break;
            case 'N': tris = py::Set<Tri>(); break;
            case 'M': {
                Tri t;
                ss >> t.a.x >> t.a.y >> t.b.x >> t.b.y >> t.c.x >> t.c.y;
                tris.add(t, hash_of(t));
                break;
            }
            case 'F': if (dump(tris) != line) got = dump(tris); break;
            case 'E': if (dump(conns) != line) got = dump(conns); break;
        }
        checks++;
        if (!got.empty()) {
            std::cerr << "mismatch at line " << lineno << "\nexpected: " << line
                      << "\ngot:      " << got << "\n";
            return 1;
        }
    }
    std::cout << "pyset: " << checks << " checks OK\n";
    return 0;
}
