// Port of engine/engine.py: map loading and game rules.
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "pos.hpp"
#include "pyset.hpp"

// engine.MoveError: the move is rejected and the bot is told why.
struct MoveError {
    std::string msg;
};

// Errors that end the Python program with a traceback. `type` is the
// Python exception class name (e.g. "engine.GameError", "AssertionError").
struct FatalError {
    std::string type, msg;
};

struct GameConfig {
    std::vector<std::vector<int>> island;  // island[y][x], y = 0 is the bottom row
    std::vector<Pos> lighthouses;
    std::vector<Pos> players;

    static GameConfig load(const std::string& path);
};

struct Lighthouse {
    Pos pos;
    int owner = -1;  // -1 == None
    int64_t energy = 0;
};

struct Player {
    int num = 0;
    Pos pos;
    int64_t score = 0;
    int64_t energy = 0;
    std::vector<char> keys;  // keys[lighthouse index]
};

struct Triangle {
    Tri key;
    std::vector<Pos> cells;
};

class Game {
public:
    static constexpr int kMaxEnergy = 100;
    static constexpr int kHorizon = 3;
    static constexpr int kRDist = 5;

    Game(const GameConfig& cfg, size_t numplayers);

    int w = 0, h = 0;
    std::vector<std::vector<int>> island;
    std::vector<std::vector<int64_t>> energymap;  // [y][x]
    std::vector<Lighthouse> lighthouses;           // dict order (map scan order)
    py::Set<Conn> conns;
    std::vector<Triangle> tris;                    // dict order
    std::vector<Player> players;

    bool in_island(int x, int y) const {
        return x >= 0 && x < w && y >= 0 && y < h && island[y][x];
    }
    bool in_bounds(int64_t x, int64_t y) const { return x >= 0 && x < w && y >= 0 && y < h; }
    int64_t energy_at(int x, int y) const { return in_island(x, y) ? energymap[y][x] : 0; }
    void set_energy(int x, int y, int64_t val);
    // Index of the lighthouse at p, or -1.
    int lighthouse_at(Pos p) const {
        return in_bounds(p.x, p.y) ? lh_index_[size_t(p.y) * w + p.x] : -1;
    }

    // Island.get_view
    std::vector<std::vector<int64_t>> view(Pos p) const;

    void pre_round();
    void post_round();

    // Lighthouse.attack. `strength` is a Python int saturated to int64.
    void attack(int lh, Player& player, int64_t strength);
    void decay(int lh, int64_t by);
    void connect(Player& player, int dest);  // dest = lighthouse index of the destination
    // Player.move with integer deltas (already validated as -1..1).
    void move(Player& player, int dx, int dy);

private:
    std::vector<int> lh_index_;
    int horizon_[2 * kHorizon + 1][2 * kHorizon + 1];
    int grow_[2 * kRDist - 1][2 * kRDist - 1];
};
