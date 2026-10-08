#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "geom.hpp"
#include "pyval.hpp"

GameConfig GameConfig::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw FatalError{"FileNotFoundError", "[Errno 2] No such file or directory: " + py::repr(py::from_ascii(path))};
    std::stringstream ss;
    ss << f.rdbuf();
    std::string raw = ss.str();
    py::Str text;
    std::string err;
    if (!py::utf8_decode(raw, text, err)) throw FatalError{"UnicodeDecodeError", err};
    // open(mapfile, "r") uses universal newlines: \r\n and \r become \n.
    py::Str norm;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == U'\r') {
            norm += U'\n';
            if (i + 1 < text.size() && text[i + 1] == U'\n') i++;
        } else {
            norm += text[i];
        }
    }
    // readlines() + replace("\n", "")
    std::vector<py::Str> lines;
    size_t start = 0;
    while (start < norm.size()) {
        size_t nl = norm.find(U'\n', start);
        if (nl == py::Str::npos) {
            lines.push_back(norm.substr(start));
            break;
        }
        lines.push_back(norm.substr(start, nl - start));
        start = nl + 1;
    }

    GameConfig cfg;
    std::vector<std::pair<char32_t, Pos>> starts;
    int y = 0;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it, ++y) {
        std::vector<int> row;
        int x = 0;
        for (char32_t c : *it) {
            if (c == U'#') {
                row.push_back(0);
            } else if (c == U'!') {
                row.push_back(1);
                cfg.lighthouses.push_back({x, y});
            } else if (c == U' ') {
                row.push_back(1);
            } else {
                row.push_back(1);
                starts.push_back({c, {x, y}});
            }
            x++;
        }
        cfg.island.push_back(std::move(row));
    }
    std::sort(starts.begin(), starts.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        if (a.second.x != b.second.x) return a.second.x < b.second.x;
        return a.second.y < b.second.y;
    });
    for (auto& s : starts) cfg.players.push_back(s.second);
    if (cfg.island.empty()) throw FatalError{"IndexError", "list index out of range"};
    size_t w = cfg.island[0].size();
    for (auto& row : cfg.island)
        if (row.size() != w) throw FatalError{"engine.GameError", "All map rows must have the same width"};
    auto all_zero = [](const std::vector<int>& r) { return std::all_of(r.begin(), r.end(), [](int v) { return !v; }); };
    bool bad = !all_zero(cfg.island.front()) || !all_zero(cfg.island.back());
    for (auto& row : cfg.island) {
        if (row.empty()) throw FatalError{"IndexError", "list index out of range"};
        if (row.front() || row.back()) bad = true;
    }
    if (bad) throw FatalError{"engine.GameError", "Map border must not be part of island"};
    return cfg;
}

Game::Game(const GameConfig& cfg, size_t numplayers) {
    if (numplayers > cfg.players.size()) throw FatalError{"AssertionError", ""};
    island = cfg.island;
    h = int(island.size());
    w = int(island[0].size());
    energymap.assign(h, std::vector<int64_t>(w, 0));
    for (int y = -kHorizon; y <= kHorizon; y++)
        for (int x = -kHorizon; x <= kHorizon; x++)
            horizon_[y + kHorizon][x + kHorizon] = geom::dist({0, 0}, {x, y}) <= kHorizon;
    for (int y = -kRDist + 1; y < kRDist; y++)
        for (int x = -kRDist + 1; x < kRDist; x++)
            grow_[y + kRDist - 1][x + kRDist - 1] = int(std::floor(kRDist - geom::dist({0, 0}, {x, y})));
    lh_index_.assign(size_t(w) * h, -1);
    for (const Pos& p : cfg.lighthouses) {
        lh_index_[size_t(p.y) * w + p.x] = int(lighthouses.size());
        lighthouses.push_back({p, -1, 0});
    }
    for (size_t i = 0; i < numplayers; i++) {
        Player pl;
        pl.num = int(i);
        pl.pos = cfg.players[i];
        pl.keys.assign(lighthouses.size(), 0);
        players.push_back(std::move(pl));
    }
}

void Game::set_energy(int x, int y, int64_t val) {
    if (val > kMaxEnergy) val = kMaxEnergy;
    if (in_island(x, y)) energymap[y][x] = val;
}

std::vector<std::vector<int64_t>> Game::view(Pos p) const {
    std::vector<std::vector<int64_t>> out;
    for (int y = -kHorizon; y <= kHorizon; y++) {
        std::vector<int64_t> row;
        for (int x = -kHorizon; x <= kHorizon; x++)
            row.push_back(horizon_[y + kHorizon][x + kHorizon] ? energy_at(p.x + x, p.y + y) : -1);
        out.push_back(std::move(row));
    }
    return out;
}

void Game::pre_round() {
    for (const Lighthouse& lh : lighthouses)
        for (int y = lh.pos.y - kRDist + 1; y < lh.pos.y + kRDist; y++)
            for (int x = lh.pos.x - kRDist + 1; x < lh.pos.x + kRDist; x++) {
                int delta = grow_[y - lh.pos.y + kRDist - 1][x - lh.pos.x + kRDist - 1];
                if (delta > 0) set_energy(x, y, energy_at(x, y) + delta);
            }
    // player_posmap: positions in first-seen order, players in order.
    std::vector<std::pair<Pos, std::vector<Player*>>> posmap;
    for (Player& pl : players) {
        auto it = std::find_if(posmap.begin(), posmap.end(), [&](const auto& e) { return e.first == pl.pos; });
        if (it != posmap.end())
            it->second.push_back(&pl);
        else
            posmap.push_back({pl.pos, {&pl}});
        int lh = lighthouse_at(pl.pos);
        if (lh >= 0) pl.keys[lh] = 1;
    }
    for (auto& [pos, group] : posmap) {
        int64_t e = energy_at(pos.x, pos.y) / int64_t(group.size());
        for (Player* pl : group) pl->energy += e;
        set_energy(pos.x, pos.y, 0);
    }
    for (size_t i = 0; i < lighthouses.size(); i++) decay(int(i), 10);
}

void Game::post_round() {
    for (const Lighthouse& lh : lighthouses)
        if (lh.owner >= 0) players[lh.owner].score += 2;
    conns.for_each([&](const Conn& c) { players[lighthouses[lighthouse_at(c.a)].owner].score += 2; });
    for (const Triangle& t : tris)
        players[lighthouses[lighthouse_at(t.key.a)].owner].score += int64_t(t.cells.size());
}

void Game::decay(int idx, int64_t by) {
    Lighthouse& lh = lighthouses[idx];
    lh.energy -= by;
    if (lh.energy <= 0) {
        lh.energy = 0;
        lh.owner = -1;
        Pos p = lh.pos;
        conns = conns.filtered([&](const Conn& c) { return !c.has(p); });
        tris.erase(std::remove_if(tris.begin(), tris.end(), [&](const Triangle& t) { return t.key.has(p); }),
                   tris.end());
    }
}

void Game::attack(int idx, Player& player, int64_t strength) {
    if (strength < 0) throw MoveError{"Strength must be positive"};
    if (strength > player.energy) strength = player.energy;
    player.energy -= strength;
    Lighthouse& lh = lighthouses[idx];
    if (lh.owner >= 0 && lh.owner != player.num) {
        int64_t d = std::min(lh.energy, strength);
        decay(idx, d);
        strength -= d;
    }
    if (strength) {
        lh.owner = player.num;
        lh.energy += strength;
    }
}

void Game::connect(Player& player, int dest_idx) {
    int orig_idx = lighthouse_at(player.pos);
    if (orig_idx < 0) throw MoveError{"Player must be located at the origin lighthouse"};
    if (dest_idx < 0) throw MoveError{"Destination must be an existing lighthouse"};
    const Lighthouse& orig = lighthouses[orig_idx];
    const Lighthouse& dest = lighthouses[dest_idx];
    if (orig.owner != player.num || dest.owner != player.num) throw MoveError{"Both lighthouses must be player-owned"};
    if (!player.keys[dest_idx]) throw MoveError{"Player does not have the destination key"};
    if (orig_idx == dest_idx) throw MoveError{"Cannot connect lighthouse to itself"};
    Conn pair{orig.pos, dest.pos};
    py::hash_t pair_hash = hash_of(pair);
    if (conns.contains(pair, pair_hash)) throw MoveError{"Connection already exists"};
    int x0 = std::min(orig.pos.x, dest.pos.x), x1 = std::max(orig.pos.x, dest.pos.x);
    int y0 = std::min(orig.pos.y, dest.pos.y), y1 = std::max(orig.pos.y, dest.pos.y);
    for (const Lighthouse& lh : lighthouses) {
        Pos p = lh.pos;
        if (x0 <= p.x && p.x <= x1 && y0 <= p.y && p.y <= y1 && p != orig.pos && p != dest.pos &&
            geom::colinear(orig.pos, dest.pos, p))
            throw MoveError{"Connection cannot intersect a lighthouse"};
    }
    py::Set<Tri> new_tris;
    bool crosses = false;
    conns.for_each([&](const Conn& c) {
        if (crosses) return;
        if (geom::intersect(c.a, c.b, orig.pos, dest.pos)) {
            crosses = true;
            return;
        }
        if (c.has(orig.pos)) {
            Pos third = c.other(orig.pos);
            Conn other{third, dest.pos};
            if (conns.contains(other, hash_of(other))) {
                Tri t{orig.pos, dest.pos, third};
                new_tris.add(t, hash_of(t));
            }
        }
    });
    if (crosses) throw MoveError{"Connection cannot intersect another connection"};

    player.keys[dest_idx] = 0;
    conns.add(pair, pair_hash);
    new_tris.for_each([&](const Tri& t) {
        Triangle tri{t, {}};
        geom::render(t.a, t.b, t.c, [&](Pos p) {
            if (in_island(p.x, p.y)) tri.cells.push_back(p);
        });
        auto it = std::find_if(tris.begin(), tris.end(), [&](const Triangle& e) { return e.key == t; });
        if (it != tris.end())
            it->cells = std::move(tri.cells);
        else
            tris.push_back(std::move(tri));
    });
}

void Game::move(Player& player, int dx, int dy) {
    Pos np{player.pos.x + dx, player.pos.y + dy};
    if (!in_island(np.x, np.y)) throw MoveError{"Target pos is not in island"};
    player.pos = np;
}
