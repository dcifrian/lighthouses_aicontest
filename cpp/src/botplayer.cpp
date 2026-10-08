#include "botplayer.hpp"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

extern char** environ;

namespace {

using Clock = std::chrono::steady_clock;

double now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

PyError comm_error(const std::string& msg) {
    return {"botplayer.CommError", msg, py::exc_repr("CommError", msg)};
}

void append_pos(std::string& out, Pos p) {
    out += '[';
    out += std::to_string(p.x);
    out += ", ";
    out += std::to_string(p.y);
    out += ']';
}

// Write everything; returns the unwritten tail on error (empty on success).
bool write_all(int fd, const std::string& data, std::string* rest = nullptr) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (rest) *rest = data.substr(off);
            return false;
        }
        off += size_t(n);
    }
    return true;
}

}  // namespace

std::string init_message(const Game& game, int playernum) {
    std::string m = "{\"player_num\": " + std::to_string(playernum) +
                    ", \"player_count\": " + std::to_string(game.players.size()) + ", \"position\": ";
    append_pos(m, game.players[playernum].pos);
    m += ", \"map\": [";
    for (size_t y = 0; y < game.island.size(); y++) {
        if (y) m += ", ";
        m += '[';
        for (size_t x = 0; x < game.island[y].size(); x++) {
            if (x) m += ", ";
            m += char('0' + game.island[y][x]);
        }
        m += ']';
    }
    m += "], \"lighthouses\": [";
    for (size_t i = 0; i < game.lighthouses.size(); i++) {
        if (i) m += ", ";
        append_pos(m, game.lighthouses[i].pos);
    }
    m += "]}";
    return m;
}

std::string state_message(const Game& game, int playernum) {
    const Player& pl = game.players[playernum];
    std::string m;
    m.reserve(256 + game.lighthouses.size() * 120);
    m += "{\"position\": ";
    append_pos(m, pl.pos);
    m += ", \"score\": " + std::to_string(pl.score) + ", \"energy\": " + std::to_string(pl.energy) + ", \"view\": [";
    auto view = game.view(pl.pos);
    for (size_t y = 0; y < view.size(); y++) {
        if (y) m += ", ";
        m += '[';
        for (size_t x = 0; x < view[y].size(); x++) {
            if (x) m += ", ";
            m += std::to_string(view[y][x]);
        }
        m += ']';
    }
    m += "], \"lighthouses\": [";
    auto conns = game.conns.items();
    for (size_t i = 0; i < game.lighthouses.size(); i++) {
        const Lighthouse& lh = game.lighthouses[i];
        if (i) m += ", ";
        m += "{\"position\": ";
        append_pos(m, lh.pos);
        m += ", \"owner\": " + std::to_string(lh.owner) + ", \"energy\": " + std::to_string(lh.energy) +
             ", \"connections\": [";
        bool first = true;
        for (const Conn& c : conns) {
            if (!c.has(lh.pos)) continue;
            if (!first) m += ", ";
            first = false;
            append_pos(m, c.other(lh.pos));
        }
        m += "], \"have_key\": ";
        m += pl.keys[i] ? "true" : "false";
        m += '}';
    }
    m += "]}";
    return m;
}

BotPlayer::BotPlayer(Game& game, int playernum, const std::string& cmdline, const BotOptions& opts)
    : game_(game), num_(playernum), opts_(opts) {
    name = py::from_ascii("Player " + std::to_string(playernum));
    int inpipe[2], outpipe[2];
    if (pipe2(inpipe, O_CLOEXEC) != 0 || pipe2(outpipe, O_CLOEXEC) != 0)
        throw PyError{"OSError", "pipe() failed", "OSError('pipe() failed')"};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inpipe[0], 0);
    posix_spawn_file_actions_adddup2(&fa, outpipe[1], 1);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    // Popen(restore_signals=True) resets these to their defaults.
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGXFSZ);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF);
    const char* argv[] = {"/bin/sh", "-c", cmdline.c_str(), nullptr};
    int rc = posix_spawn(&pid_, "/bin/sh", &fa, &attr, const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    ::close(inpipe[0]);
    ::close(outpipe[1]);
    if (rc != 0) {
        ::close(inpipe[1]);
        ::close(outpipe[0]);
        throw PyError{"OSError", "posix_spawn failed", "OSError('posix_spawn failed')"};
    }
    in_fd_ = inpipe[1];
    out_fd_ = outpipe[0];
}

bool BotPlayer::poll_child() {
    if (exited_) return true;
    int st;
    pid_t r = waitpid(pid_, &st, WNOHANG);
    if (r == pid_) {
        exited_ = true;
        returncode_ = WIFSIGNALED(st) ? -WTERMSIG(st) : WEXITSTATUS(st);
    }
    return exited_;
}

void BotPlayer::send_signal(int sig) {
    if (poll_child()) return;
    kill(pid_, sig);
}

void BotPlayer::send(const std::string& json) {
    if (opts_.transcript) fprintf(opts_.transcript, ">%d %s\n", num_, json.c_str());
    std::string data = json + "\n";
    std::string rest;
    if (in_fd_ < 0 || !write_all(in_fd_, data, &rest)) {
        // A BufferedWriter keeps what it could not flush, so the later
        // stdin.close() tries again; data larger than the buffer is written
        // directly and is not kept.
        if (data.size() <= 8192) wpending_ = rest;
        throw comm_error("Error sending data");
    }
}

std::string BotPlayer::readline() {
    std::string line;
    size_t nl = rbuf_.find('\n', rpos_);
    if (nl != std::string::npos) {
        line = rbuf_.substr(rpos_, nl + 1 - rpos_);
        rpos_ = nl + 1;
    } else {
        line = rbuf_.substr(rpos_);
        rbuf_.clear();
        rpos_ = 0;
        char buf[8192];
        while (true) {
            ssize_t n = ::read(out_fd_, buf, sizeof buf);
            if (n < 0) {
                if (errno == EINTR) continue;
                throw PyError{"OSError", "read failed", "OSError(" + std::to_string(errno) + ", 'read failed')"};
            }
            if (n == 0) break;
            const char* p = static_cast<const char*>(memchr(buf, '\n', size_t(n)));
            if (p) {
                size_t take = size_t(p - buf) + 1;
                line.append(buf, take);
                rbuf_.assign(buf + take, size_t(n) - take);
                break;
            }
            line.append(buf, size_t(n));
        }
    }
    if (rpos_ > 0 && rpos_ == rbuf_.size()) {
        rbuf_.clear();
        rpos_ = 0;
    }
    if (opts_.transcript && !(line.empty() && last_log_eof_))
        fprintf(opts_.transcript, "<%d %s\n", num_, py::repr_bytes(line).c_str());
    last_log_eof_ = line.empty();
    return line;
}

py::Value BotPlayer::recv(double soft_timeout, double hard_timeout) {
    double et = now() + soft_timeout;
    double ht = now() + hard_timeout;
    py::Str line;
    auto unknown = [](const std::string& inner_repr) { return comm_error("Unknown error: " + inner_repr); };
    while (line.empty() || line.back() != U'\n') {
        double timeout = ht - now();
        if (timeout < 0) throw unknown("ValueError('timeout must be non-negative')");
        struct pollfd pfd = {out_fd_, POLLIN, 0};
        struct timespec ts;
        ts.tv_sec = time_t(timeout);
        ts.tv_nsec = long((timeout - double(ts.tv_sec)) * 1e9);
        int r;
        do {
            r = ppoll(&pfd, 1, &ts, nullptr);
        } while (r < 0 && errno == EINTR);
        if (r <= 0) throw unknown(py::exc_repr("CommError", "Bot " + py::repr(name) + " over hard timeout"));
        std::string raw;
        try {
            raw = readline();
        } catch (const PyError& e) {
            throw unknown(e.repr);
        }
        std::string err;
        if (!py::utf8_decode(raw, line, err)) throw unknown(err);
        // At EOF Python keeps calling select()/readline() until the hard
        // timeout makes select() reject a negative timeout. Skip the wait.
        if (raw.empty()) throw unknown("ValueError('timeout must be non-negative')");
    }
    if (now() > et) fprintf(stderr, "Bot %s over soft timeout\n", py::repr(name).c_str());
    py::Value v;
    std::string err;
    if (!py::json_loads(line, v, err)) throw comm_error("Invalid JSON: " + err);
    return v;
}

void BotPlayer::initialize() {
    if (!alive_) return;
    send(init_message(game_, num_));
    py::Value reply = recv(opts_.init_timeout, opts_.init_timeout);
    const py::Value* n = reply.get("name");
    if (!n || n->t != py::Value::T::Str) throw comm_error("Bot did not greet with name");
    name = n->s;
}

void BotPlayer::turn() {
    try {
        turn_inner();
    } catch (const PyError& e) {
        std::string msg = "Bot " + py::utf8_encode(name) + " failed with exception " + e.repr + ", killing\n";
        fwrite(msg.data(), 1, msg.size(), stdout);
        close();
    }
}

void BotPlayer::turn_inner() {
    if (!alive_) return;
    send_signal(SIGCONT);
    send(state_message(game_, num_));
    py::Value move = recv(opts_.move_timeout, opts_.move_hardtimeout);
    if (move.t != py::Value::T::Dict || !move.get("command")) throw comm_error("Invalid command structure");
    try {
        apply(move);
        send("{\"success\": true}");
        send_signal(SIGSTOP);
    } catch (const MoveError& e) {
        std::string m = "{\"success\": false, \"message\": ";
        py::json_dump_str(m, py::from_ascii(e.msg));
        m += "}";
        send(m);
    }
}

namespace {

bool is_str(const py::Value& v, const char* s) {
    return v.t == py::Value::T::Str && v.s == py::from_ascii(s);
}

// Integer value of a number that compares equal to a small int.
bool small_int(const py::Value& v, int64_t& out) {
    switch (v.t) {
        case py::Value::T::Bool: out = v.b; return true;
        case py::Value::T::Int:
            if (v.i == INT64_MIN || v.i == INT64_MAX) return false;
            out = v.i;
            return true;
        case py::Value::T::Float:
            if (!(std::fabs(v.f) < 1e9) || v.f != std::floor(v.f)) return false;
            out = int64_t(v.f);
            return true;
        default: return false;
    }
}

}  // namespace

void BotPlayer::apply(const py::Value& move) {
    using T = py::Value::T;
    Player& pl = player();
    const py::Value& cmd = *move.get("command");
    if (is_str(cmd, "pass")) {
        return;
    } else if (is_str(cmd, "move")) {
        const py::Value *x = move.get("x"), *y = move.get("y");
        if (!x || !y) throw MoveError{"Move command requires x, y"};
        int64_t dx, dy;
        auto delta_ok = [](const py::Value& v, int64_t& d) { return small_int(v, d) && d >= -1 && d <= 1; };
        if (!delta_ok(*x, dx) || !delta_ok(*y, dy)) throw MoveError{"Delta must be 1 cell away"};
        int64_t nx = pl.pos.x + dx, ny = pl.pos.y + dy;
        if (!game_.in_bounds(nx, ny)) throw MoveError{"Target pos is not in island"};
        if (x->t == T::Float || y->t == T::Float)
            throw PyError{"TypeError", "list indices must be integers or slices, not float",
                          "TypeError('list indices must be integers or slices, not float')"};
        game_.move(pl, int(dx), int(dy));
    } else if (is_str(cmd, "attack")) {
        const py::Value* e = move.get("energy");
        if (!e || !e->is_int()) throw MoveError{"Attack command requires integer energy"};
        int lh = game_.lighthouse_at(pl.pos);
        if (lh < 0) throw MoveError{"Player must be located at target lighthouse"};
        game_.attack(lh, pl, e->as_int());
    } else if (is_str(cmd, "connect")) {
        const py::Value* d = move.get("destination");
        if (!d) throw MoveError{"Connect command requires destination"};
        // dest = tuple(move["destination"]); hash(dest)
        bool iterable = d->t == T::List || d->t == T::Str || d->t == T::Dict;
        bool hashable = true;
        if (d->t == T::List)
            for (const py::Value& e : d->list)
                if (e.t == T::List || e.t == T::Dict) hashable = false;
        if (!iterable || !hashable) throw MoveError{"Destination must be a coordinate pair"};
        int dest = -1;
        int64_t dx, dy;
        if (d->t == T::List && d->list.size() == 2 && small_int(d->list[0], dx) && small_int(d->list[1], dy) &&
            game_.in_bounds(dx, dy))
            dest = game_.lighthouse_at({int(dx), int(dy)});
        if (game_.lighthouse_at(pl.pos) < 0) throw MoveError{"Player must be located at the origin lighthouse"};
        game_.connect(pl, dest);
    } else {
        throw MoveError{"Invalid command " + py::repr(cmd)};
    }
}

void BotPlayer::close() {
    if (!alive_) return;
    send_signal(SIGCONT);
    if (in_fd_ >= 0) {
        int fd = in_fd_;
        in_fd_ = -1;
        bool failed = !wpending_.empty() && !write_all(fd, wpending_);
        wpending_.clear();
        ::close(fd);
        if (failed) throw PyError{"BrokenPipeError", "[Errno 32] Broken pipe", "BrokenPipeError(32, 'Broken pipe')"};
    }
    if (out_fd_ >= 0) {
        ::close(out_fd_);
        out_fd_ = -1;
    }
    // Same loop as botplayer.py: 100 x (sleep 10 ms, poll). The exact
    // timing matters for bots that outlive SIGINT (e.g. behind /bin/sh).
    auto wait_exit = [&]() {
        for (int k = 0; k < 100; k++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (poll_child()) return true;
        }
        return false;
    };
    if (!wait_exit()) {
        send_signal(SIGINT);
        if (!wait_exit()) send_signal(SIGKILL);
    }
    if (!exited_) {
        int st;
        while (waitpid(pid_, &st, 0) < 0 && errno == EINTR) {
        }
        exited_ = true;
        returncode_ = WIFSIGNALED(st) ? -WTERMSIG(st) : WEXITSTATUS(st);
    }
    fprintf(stderr, "Bot %s exit code: %d\n", py::repr(name).c_str(), returncode_);
    alive_ = false;
}
