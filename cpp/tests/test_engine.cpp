// Rule tests: geom.py's self-checks plus a few hand-made scenarios.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/engine.hpp"
#include "../src/geom.hpp"

static int failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                \
        }                                                              \
    } while (0)

static std::string expect_move_error(Game& g, Player& p, int dest) {
    try {
        g.connect(p, dest);
    } catch (const MoveError& e) {
        return e.msg;
    }
    return "";
}

static GameConfig square_config() {
    // 7x7 board, lighthouses at (1,1), (5,1), (1,5), (3,3) and (5,5).
    GameConfig cfg;
    cfg.island.assign(7, std::vector<int>(7, 1));
    for (int k = 0; k < 7; k++) cfg.island[0][k] = cfg.island[6][k] = cfg.island[k][0] = cfg.island[k][6] = 0;
    cfg.lighthouses = {{1, 1}, {5, 1}, {1, 5}, {3, 3}, {5, 5}};
    cfg.players = {{1, 1}, {5, 5}};
    return cfg;
}

static int real_main();
int main() {
    try {
        return real_main();
    } catch (const MoveError& e) {
        fprintf(stderr, "unexpected MoveError: %s\n", e.msg.c_str());
        return 1;
    }
}

static int real_main() {
    // geom.py __main__ asserts
    CHECK(geom::orient2d({0, 0}, {0, 1}, {1, 0}) < 0);
    CHECK(geom::orient2d({0, 1}, {1, 0}, {0, 0}) < 0);
    CHECK(geom::orient2d({1, 0}, {0, 0}, {0, 1}) < 0);
    CHECK(geom::orient2d({0, 1}, {0, 0}, {1, 0}) > 0);
    CHECK(geom::orient2d({1, 0}, {0, 1}, {0, 0}) > 0);
    CHECK(geom::orient2d({0, 0}, {1, 0}, {0, 1}) > 0);
    CHECK(!geom::intersect({0, 0}, {2, 2}, {4, 1}, {1, 4}));
    CHECK(!geom::intersect({0, 0}, {2, 2}, {3, 1}, {1, 3}));
    CHECK(geom::intersect({0, 0}, {2, 2}, {2, 1}, {1, 2}));
    int cells = 0;
    geom::render({0, 0}, {5, 0}, {0, 5}, [&](Pos) { cells++; });
    CHECK(cells == 10);  // len(list(geom.render(((0,0),(5,0),(0,5)))))

    GameConfig cfg = square_config();
    Game g(cfg, 2);
    Player& p0 = g.players[0];
    // Energy growth: a cell next to two lighthouses gets both contributions.
    g.pre_round();
    CHECK(g.energy_at(1, 1) == 0);  // collected by player 0
    // own lighthouse 5, (3,3) at 2.83 -> 2, (5,1) and (1,5) at 4 -> 1 each
    CHECK(p0.energy == 9);
    p0.energy = 100;
    g.attack(g.lighthouse_at({1, 1}), p0, 30);
    CHECK(g.lighthouses[0].owner == 0 && g.lighthouses[0].energy == 30 && p0.energy == 70);
    CHECK(expect_move_error(g, p0, g.lighthouse_at({1, 1})) == "Cannot connect lighthouse to itself");
    CHECK(expect_move_error(g, p0, g.lighthouse_at({5, 5})) == "Both lighthouses must be player-owned");
    try {
        g.attack(0, p0, -1);
        CHECK(false);
    } catch (const MoveError& e) {
        CHECK(e.msg == "Strength must be positive");
    }
    // (1,1)-(5,5) passes exactly through (3,3).
    p0.pos = {5, 5};
    g.attack(g.lighthouse_at({5, 5}), p0, 10);
    p0.keys[g.lighthouse_at({1, 1})] = 1;
    CHECK(g.players[1].pos == (Pos{5, 5}));
    CHECK(expect_move_error(g, p0, g.lighthouse_at({1, 1})) == "Connection cannot intersect a lighthouse");
    // Build a triangle (1,1)-(5,1)-(3,3).
    p0.pos = {5, 1};
    g.attack(g.lighthouse_at({5, 1}), p0, 10);
    p0.pos = {3, 3};
    g.attack(g.lighthouse_at({3, 3}), p0, 10);
    p0.keys.assign(g.lighthouses.size(), 1);
    p0.pos = {5, 1};
    g.connect(p0, g.lighthouse_at({1, 1}));
    p0.pos = {3, 3};
    CHECK(expect_move_error(g, p0, g.lighthouse_at({1, 1})) == "Player does not have the destination key");
    p0.keys.assign(g.lighthouses.size(), 1);  // keys are single use
    g.connect(p0, g.lighthouse_at({1, 1}));
    CHECK(g.tris.empty());
    // (1,5)-(5,1) would pass exactly through (3,3).
    p0.pos = {3, 3};
    g.connect(p0, g.lighthouse_at({5, 1}));
    CHECK(g.tris.size() == 1);
    p0.keys.assign(g.lighthouses.size(), 1);
    CHECK(expect_move_error(g, p0, g.lighthouse_at({5, 1})) == "Connection already exists");
    int64_t before = p0.score;
    g.post_round();
    // 4 owned lighthouses * 2 + 3 connections * 2 + triangle cells
    CHECK(p0.score - before == 8 + 6 + int64_t(g.tris[0].cells.size()));
    // Losing a vertex removes its connections and the triangle.
    g.decay(g.lighthouse_at({1, 1}), 1000);
    CHECK(g.tris.empty() && g.conns.size() == 1);

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("engine: OK\n");
    return 0;
}
