// Port of engine/botplayer.py: bot processes and the JSON line protocol.
//
// The pipe handling deliberately mirrors what CPython does underneath the
// Python code (select() on the raw fd, then a buffered readline(); a
// BufferedWriter that keeps unflushed data after a failed write), because
// those details decide the outcome when a bot misbehaves.
#pragma once

#include <sys/types.h>

#include <cstdio>
#include <string>

#include "engine.hpp"
#include "pyval.hpp"

// A Python exception: `qualname` and `str` form the last traceback line,
// `repr` is what "%r" prints.
struct PyError {
    std::string qualname, str, repr;
};

struct BotOptions {
    double init_timeout = 15.0;
    double move_timeout = 2.0;       // soft: only prints a warning
    double move_hardtimeout = 10.0;  // hard: the bot is killed
    FILE* transcript = nullptr;
};

class BotPlayer {
public:
    BotPlayer(Game& game, int playernum, const std::string& cmdline, const BotOptions& opts);
    BotPlayer(const BotPlayer&) = delete;
    BotPlayer& operator=(const BotPlayer&) = delete;

    void initialize();  // throws PyError (uncaught in game.py)
    void turn();        // catches everything except an error raised by close()
    void close();       // throws PyError if flushing stdin fails

    bool alive() const { return alive_; }
    Player& player() { return game_.players[num_]; }
    py::Str name;

private:
    void turn_inner();
    void send(const std::string& json);
    py::Value recv(double soft_timeout, double hard_timeout);
    std::string readline();
    void send_signal(int sig);
    bool poll_child();  // Popen.poll(): true once the child has exited
    void apply(const py::Value& move);

    Game& game_;
    int num_;
    BotOptions opts_;
    bool alive_ = true;
    pid_t pid_ = -1;
    int returncode_ = 0;
    bool exited_ = false;
    int in_fd_ = -1;   // bot's stdin
    int out_fd_ = -1;  // bot's stdout
    std::string wpending_;  // BufferedWriter contents after a failed flush
    std::string rbuf_;      // BufferedReader contents
    size_t rpos_ = 0;
    bool last_log_eof_ = false;
    unsigned seen_child_exits_ = ~0u;  // g_child_exits when we last called waitpid
};

// Messages, formatted exactly like json.dumps().
std::string init_message(const Game& game, int playernum);
std::string state_message(const Game& game, int playernum);
