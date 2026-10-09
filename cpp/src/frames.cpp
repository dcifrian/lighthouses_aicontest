#include "frames.hpp"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>

extern char** environ;

namespace {

void append_scores(std::string& m, const std::vector<std::pair<py::Str, int64_t>>& scores) {
    m += '{';
    for (size_t i = 0; i < scores.size(); i++) {
        if (i) m += ", ";
        py::json_dump_str(m, scores[i].first);
        m += ": " + std::to_string(scores[i].second);
    }
    m += '}';
}

}  // namespace

FrameSink::~FrameSink() {
    if (record_) record_piped_ ? pclose(record_) : fclose(record_);
    if (viewer_in_ >= 0) ::close(viewer_in_);
    if (viewer_out_ >= 0) ::close(viewer_out_);
}

bool FrameSink::open_record(const std::string& path) {
    if (path.size() > 3 && path.compare(path.size() - 3, 3, ".gz") == 0) {
        std::string quoted = "'";
        for (char c : path) quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
        quoted += "'";
        // "e": the pipe must not leak into bots, or gzip would wait for them.
        record_ = popen(("gzip -c > " + quoted).c_str(), "we");
        record_piped_ = true;
    } else {
        record_ = fopen(path.c_str(), "we");
    }
    return record_ != nullptr;
}

bool FrameSink::start_viewer(const std::string& cmdline) {
    int inpipe[2], outpipe[2];
    if (pipe2(inpipe, O_CLOEXEC) != 0 || pipe2(outpipe, O_CLOEXEC) != 0) return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inpipe[0], 0);
    posix_spawn_file_actions_adddup2(&fa, outpipe[1], 1);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF);
    const char* argv[] = {"/bin/sh", "-c", cmdline.c_str(), nullptr};
    int rc = posix_spawn(&viewer_pid_, "/bin/sh", &fa, &attr, const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    ::close(inpipe[0]);
    ::close(outpipe[1]);
    if (rc != 0) {
        ::close(inpipe[1]);
        ::close(outpipe[0]);
        viewer_pid_ = -1;
        return false;
    }
    viewer_in_ = inpipe[1];
    viewer_out_ = outpipe[0];
    return true;
}

void FrameSink::viewer_gone() {
    if (viewer_in_ >= 0) ::close(viewer_in_);
    if (viewer_out_ >= 0) ::close(viewer_out_);
    viewer_in_ = viewer_out_ = -1;
    fprintf(stderr, "viewer closed, continuing without it\n");
}

void FrameSink::emit(const std::string& line, bool wait_ack) {
    if (record_) fwrite(line.data(), 1, line.size(), record_);
    if (viewer_in_ < 0) return;
    size_t off = 0;
    while (off < line.size()) {
        ssize_t n = ::write(viewer_in_, line.data() + off, line.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return viewer_gone();
        off += size_t(n);
    }
    if (!wait_ack) return;
    // Block until the viewer asks for the next frame.
    while (ackbuf_.find('\n') == std::string::npos) {
        char buf[256];
        ssize_t n = ::read(viewer_out_, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return viewer_gone();
        ackbuf_.append(buf, size_t(n));
    }
    ackbuf_.erase(0, ackbuf_.find('\n') + 1);
}

void FrameSink::game_start(const Game& g, int game, int games, long rounds, const std::vector<py::Str>& names,
                           const std::vector<int>& slots,
                           const std::vector<std::pair<py::Str, int64_t>>& cumulative) {
    if (!active()) return;
    std::string m = "{\"type\": \"game\", \"game\": " + std::to_string(game) + ", \"games\": " +
                    std::to_string(games) + ", \"rounds\": " + std::to_string(rounds) + ", \"island\": [";
    for (int y = 0; y < g.h; y++) {
        m += y ? ", [" : "[";
        for (int x = 0; x < g.w; x++) m += (x ? ", " : "") + std::to_string(g.island[y][x]);
        m += ']';
    }
    m += "], \"lighthouses\": [";
    for (size_t i = 0; i < g.lighthouses.size(); i++)
        m += (i ? ", [" : "[") + std::to_string(g.lighthouses[i].pos.x) + ", " +
             std::to_string(g.lighthouses[i].pos.y) + "]";
    m += "], \"names\": [";
    for (size_t i = 0; i < names.size(); i++) {
        if (i) m += ", ";
        py::json_dump_str(m, names[i]);
    }
    m += "], \"slots\": [";
    for (size_t i = 0; i < slots.size(); i++) m += (i ? ", " : "") + std::to_string(slots[i]);
    m += "], \"cumulative\": ";
    append_scores(m, cumulative);
    m += "}\n";
    emit(m, true);
}

void FrameSink::frame(const Game& g, int game, long round, const char* phase, int player,
                      const std::vector<bool>& alive) {
    if (!active()) return;
    std::string m;
    m.reserve(1024);
    m += "{\"type\": \"frame\", \"game\": " + std::to_string(game) + ", \"round\": " + std::to_string(round) +
         ", \"phase\": \"" + phase + "\", \"player\": " + std::to_string(player) + ", \"lh\": [";
    for (size_t i = 0; i < g.lighthouses.size(); i++)
        m += (i ? ", [" : "[") + std::to_string(g.lighthouses[i].owner) + ", " +
             std::to_string(g.lighthouses[i].energy) + "]";
    m += "], \"conns\": [";
    bool first = true;
    g.conns.for_each([&](const Conn& c) {
        m += (first ? "[" : ", [") + std::to_string(g.lighthouse_at(c.a)) + ", " +
             std::to_string(g.lighthouse_at(c.b)) + "]";
        first = false;
    });
    m += "], \"tris\": [";
    for (size_t i = 0; i < g.tris.size(); i++) {
        const Tri& t = g.tris[i].key;
        m += (i ? ", [" : "[") + std::to_string(g.lighthouse_at(t.a)) + ", " + std::to_string(g.lighthouse_at(t.b)) +
             ", " + std::to_string(g.lighthouse_at(t.c)) + "]";
    }
    m += "], \"players\": [";
    for (size_t i = 0; i < g.players.size(); i++) {
        const Player& p = g.players[i];
        m += (i ? ", [" : "[") + std::to_string(p.pos.x) + ", " + std::to_string(p.pos.y) + ", " +
             std::to_string(p.score) + ", " + std::to_string(p.energy) + ", [";
        bool fk = true;
        for (size_t k = 0; k < p.keys.size(); k++)
            if (p.keys[k]) {
                m += (fk ? "" : ", ") + std::to_string(k);
                fk = false;
            }
        m += "], ";
        m += (i < alive.size() && alive[i]) ? "true]" : "false]";
    }
    m += ']';
    if (strcmp(phase, "pre") == 0) {
        // Cell energy only changes in pre_round.
        m += ", \"energy\": [";
        for (int y = 0; y < g.h; y++) {
            m += y ? ", [" : "[";
            for (int x = 0; x < g.w; x++) m += (x ? ", " : "") + std::to_string(g.energy_at(x, y));
            m += ']';
        }
        m += ']';
    }
    m += "}\n";
    emit(m, true);
}

void FrameSink::end(const std::vector<std::pair<py::Str, int64_t>>& scores) {
    if (!active()) return;
    std::string m = "{\"type\": \"end\", \"scores\": ";
    append_scores(m, scores);
    m += "}\n";
    emit(m, false);
    if (record_) {
        record_piped_ ? pclose(record_) : fclose(record_);
        record_ = nullptr;
    }
}

void FrameSink::wait_viewer() {
    if (viewer_pid_ < 0) return;
    if (viewer_in_ >= 0) {
        ::close(viewer_in_);
        viewer_in_ = -1;
    }
    int st;
    while (waitpid(viewer_pid_, &st, 0) < 0 && errno == EINTR) {
    }
    viewer_pid_ = -1;
}
