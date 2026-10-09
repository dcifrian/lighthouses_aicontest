// Game-state frames for the viewer (viewer/viewer.py), as JSON Lines.
//
// Frames go to a recording file (--record) and/or a live viewer process
// (--view). The live viewer paces the game: after each frame the engine
// waits for it to answer "next". If the viewer goes away the engine simply
// carries on headless. Nothing here touches stdout or what bots see.
#pragma once

#include <sys/types.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "engine.hpp"
#include "pyval.hpp"

class FrameSink {
public:
    ~FrameSink();

    // A path ending in .gz is compressed through the system gzip.
    bool open_record(const std::string& path);
    bool start_viewer(const std::string& cmdline);
    bool active() const { return record_ || viewer_out_ >= 0; }

    void game_start(const Game& g, int game, int games, long rounds, const std::vector<py::Str>& names,
                    const std::vector<std::pair<py::Str, int64_t>>& cumulative);
    // phase is "pre", "turn" or "post"; player is the bot that just moved.
    void frame(const Game& g, int game, long round, const char* phase, int player,
               const std::vector<bool>& alive);
    void end(const std::vector<std::pair<py::Str, int64_t>>& scores);
    // Wait for a live viewer window to be closed.
    void wait_viewer();

private:
    void emit(const std::string& line, bool wait_ack);
    void viewer_gone();

    FILE* record_ = nullptr;
    bool record_piped_ = false;  // writing through `gzip` (popen)
    pid_t viewer_pid_ = -1;
    int viewer_in_ = -1;   // viewer's stdin (frames)
    int viewer_out_ = -1;  // viewer's stdout (acks)
    std::string ackbuf_;
};
