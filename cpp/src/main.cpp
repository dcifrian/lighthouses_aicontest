// Lighthouses AI contest engine: a C++ port of engine/game.py.
//
//   lighthouses [options] MAP BOT_CMD [BOT_CMD ...]
//   lighthouses [options] MAP FAST BOT_CMD [BOT_CMD ...]
//
// Default output is byte-identical to the official engine/game.py (minus its
// sleeps); FAST reproduces the numba fork's FAST mode output.
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "botplayer.hpp"
#include "engine.hpp"
#include "frames.hpp"
#include "pyval.hpp"

namespace {

const char* kUsage =
    "usage: lighthouses [options] MAP [FAST] BOT_CMD [BOT_CMD ...]\n"
    "\n"
    "Plays one game per rotation of the bot list, like engine/game.py.\n"
    "\n"
    "options (must come before MAP):\n"
    "  --rounds N          rounds per game (default 1200; 500 with FAST)\n"
    "  --init-timeout S    seconds to answer the init message (default 15)\n"
    "  --soft-timeout S    seconds per turn before a warning (default 2)\n"
    "  --hard-timeout S    seconds per turn before the bot is killed (default 10)\n"
    "  --transcript FILE   log every line sent to / read from the bots\n"
    "  --state-dump FILE   log the full game state after every phase\n"
    "  --record FILE       record the game for the viewer (viewer/viewer.py FILE)\n"
    "  --view              watch live in the viewer, which also controls the pace\n"
    "  --viewer CMD        viewer command for --view (default: python3 viewer/viewer.py --live)\n"
    "\n"
    "FAST (first argument after MAP) mimics the numba fork's FAST mode: no\n"
    "per-round score lines.\n";

void out(const std::string& s) { fwrite(s.data(), 1, s.size(), stdout); }

void dump_state(FILE* f, const std::string& tag, const Game& g) {
    std::string m = tag + " {\"lighthouses\": [";
    for (size_t i = 0; i < g.lighthouses.size(); i++) {
        const Lighthouse& lh = g.lighthouses[i];
        if (i) m += ", ";
        m += "[" + std::to_string(lh.pos.x) + ", " + std::to_string(lh.pos.y) + ", " + std::to_string(lh.owner) +
             ", " + std::to_string(lh.energy) + "]";
    }
    auto pos = [](Pos p) { return "[" + std::to_string(p.x) + ", " + std::to_string(p.y) + "]"; };
    auto less = [](Pos a, Pos b) { return a.x != b.x ? a.x < b.x : a.y < b.y; };
    m += "], \"conns\": [";
    bool first = true;
    g.conns.for_each([&](const Conn& c) {
        if (!first) m += ", ";
        first = false;
        Pos a = c.a, b = c.b;
        if (less(b, a)) std::swap(a, b);
        m += "[" + pos(a) + ", " + pos(b) + "]";
    });
    m += "], \"tris\": [";
    for (size_t i = 0; i < g.tris.size(); i++) {
        const Triangle& t = g.tris[i];
        if (i) m += ", ";
        m += "[" + pos(t.key.a) + ", " + pos(t.key.b) + ", " + pos(t.key.c) + ", " + std::to_string(t.cells.size()) +
             "]";
    }
    m += "], \"players\": [";
    for (size_t i = 0; i < g.players.size(); i++) {
        const Player& p = g.players[i];
        if (i) m += ", ";
        m += "[" + std::to_string(p.pos.x) + ", " + std::to_string(p.pos.y) + ", " + std::to_string(p.score) + ", " +
             std::to_string(p.energy) + ", [";
        std::vector<Pos> keys;
        for (size_t k = 0; k < p.keys.size(); k++)
            if (p.keys[k]) keys.push_back(g.lighthouses[k].pos);
        std::sort(keys.begin(), keys.end(), less);
        for (size_t k = 0; k < keys.size(); k++) m += (k ? ", " : "") + pos(keys[k]);
        m += "]]";
    }
    m += "], \"energy\": [";
    for (int y = 0; y < g.h; y++) {
        if (y) m += ", ";
        m += "[";
        for (int x = 0; x < g.w; x++) m += (x ? ", " : "") + std::to_string(g.energy_at(x, y));
        m += "]";
    }
    m += "]}\n";
    fwrite(m.data(), 1, m.size(), f);
}

double parse_double(const char* opt, const char* s) {
    char* end;
    double v = strtod(s, &end);
    if (*s == '\0' || *end != '\0') {
        fprintf(stderr, "invalid value for %s: %s\n", opt, s);
        exit(2);
    }
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    // Python's stdout is line buffered on a terminal and block buffered otherwise.
    static char outbuf[8192];
    setvbuf(stdout, outbuf, isatty(1) ? _IOLBF : _IOFBF, sizeof outbuf);

    BotOptions opts;
    FrameSink frames;
    bool view = false;
    std::string viewer_cmd = std::string("python3 '") + LH_VIEWER + "' --live";
    long rounds = -1;
    FILE* state_dump = nullptr;
    int i = 1;
    for (; i < argc && strncmp(argv[i], "--", 2) == 0; i++) {
        std::string a = argv[i];
        if (a == "--help") {
            fputs(kUsage, stdout);
            return 0;
        }
        if (a == "--view") {
            view = true;
            continue;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "missing value for %s\n%s", a.c_str(), kUsage);
            return 2;
        }
        const char* v = argv[++i];
        if (a == "--rounds") {
            rounds = long(parse_double(a.c_str(), v));
        } else if (a == "--init-timeout") {
            opts.init_timeout = parse_double(a.c_str(), v);
        } else if (a == "--soft-timeout") {
            opts.move_timeout = parse_double(a.c_str(), v);
        } else if (a == "--hard-timeout") {
            opts.move_hardtimeout = parse_double(a.c_str(), v);
        } else if (a == "--record") {
            if (!frames.open_record(v)) {
                perror(v);
                return 2;
            }
        } else if (a == "--viewer") {
            viewer_cmd = v;
        } else if (a == "--transcript" || a == "--state-dump") {
            FILE* f = fopen(v, "we");
            if (!f) {
                perror(v);
                return 2;
            }
            (a == "--transcript" ? opts.transcript : state_dump) = f;
        } else {
            fprintf(stderr, "unknown option %s\n%s", a.c_str(), kUsage);
            return 2;
        }
    }
    if (i >= argc) {
        fputs(kUsage, stderr);
        return 2;
    }
    std::string cfg_file = argv[i++];
    bool fast = i < argc && strcmp(argv[i], "FAST") == 0;
    if (fast) {
        out(std::string(argv[i]) + "\n");  // the fork prints sys.argv[2]
        i++;
    }
    if (rounds < 0) rounds = fast ? 500 : 1200;
    std::vector<std::string> bots(argv + i, argv + argc);
    if (view && !frames.start_viewer(viewer_cmd)) {
        fprintf(stderr, "could not start the viewer: %s\n", viewer_cmd.c_str());
        return 2;
    }
    auto alive = [&](const auto& actors) {
        std::vector<bool> v;
        for (auto& a : actors) v.push_back(a->alive());
        return v;
    };

    std::vector<std::pair<py::Str, int64_t>> scores;
    std::vector<std::unique_ptr<BotPlayer>> actors;
    try {
        size_t n = bots.size();
        for (size_t gn = 0; gn < n; gn++) {
            std::vector<std::string> perm;
            for (size_t j = 0; j < n; j++) perm.push_back(bots[(gn + j) % n]);
            GameConfig config = GameConfig::load(cfg_file);
            Game game(config, perm.size());
            out("Launching bots...\n");
            actors.clear();
            for (size_t j = 0; j < perm.size(); j++)
                actors.push_back(std::make_unique<BotPlayer>(game, int(j), perm[j], opts));
            out("Bots launched.\n");
            for (auto& a : actors) a->initialize();
            if (frames.active()) {
                std::vector<py::Str> names;
                for (auto& a : actors) names.push_back(a->name);
                frames.game_start(game, int(gn), int(n), rounds, names, scores);
            }

            for (long round = 0; round < rounds; round++) {
                game.pre_round();
                if (state_dump) dump_state(state_dump, "pre", game);
                if (frames.active()) frames.frame(game, int(gn), round, "pre", -1, alive(actors));
                for (auto& a : actors) {
                    a->turn();
                    if (state_dump) dump_state(state_dump, "turn" + std::to_string(a->player().num), game);
                    if (frames.active()) frames.frame(game, int(gn), round, "turn", a->player().num, alive(actors));
                }
                game.post_round();
                if (state_dump) dump_state(state_dump, "post", game);
                if (frames.active()) frames.frame(game, int(gn), round, "post", -1, alive(actors));
                if (!fast) {
                    std::string line = "########### ROUND " + std::to_string(round) + " SCORE: ";
                    for (const Player& p : game.players)
                        line += "P" + std::to_string(p.num) + ": " + std::to_string(p.score) + " ";
                    line += "\n";
                    out(line);
                }
            }
            for (auto& a : actors) {
                const Player& p = a->player();
                auto it = std::find_if(scores.begin(), scores.end(), [&](const auto& s) { return s.first == a->name; });
                if (it == scores.end())
                    scores.push_back({a->name, p.score});
                else
                    it->second += p.score;
            }
            for (auto& a : actors) a->close();
        }
        std::string s = "{";
        for (size_t k = 0; k < scores.size(); k++) {
            if (k) s += ", ";
            s += py::repr(scores[k].first) + ": " + std::to_string(scores[k].second);
        }
        out(s + "}\n");
        frames.end(scores);
    } catch (const PyError& e) {
        fflush(stdout);
        fprintf(stderr, "Traceback (most recent call last):\n  (C++ engine)\n%s: %s\n", e.qualname.c_str(),
                e.str.c_str());
        for (auto& a : actors) {
            try {
                a->close();
            } catch (const PyError&) {
            }
        }
        frames.wait_viewer();
        return 1;
    } catch (const FatalError& e) {
        fflush(stdout);
        fprintf(stderr, "Traceback (most recent call last):\n  (C++ engine)\n%s%s%s\n", e.type.c_str(),
                e.msg.empty() ? "" : ": ", e.msg.c_str());
        for (auto& a : actors) {
            try {
                a->close();
            } catch (const PyError&) {
            }
        }
        frames.wait_viewer();
        return 1;
    }
    fflush(stdout);
    frames.wait_viewer();
    if (opts.transcript) fclose(opts.transcript);
    if (state_dump) fclose(state_dump);
    return 0;
}
