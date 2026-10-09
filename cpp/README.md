# Lighthouses engine in C++

A drop-in replacement for the official Python engine (`engine/game.py`). It
takes the same command line, sends the same bytes to the bots, prints the
same stdout and ends games the same way. It has no dependencies beyond a
C++20 compiler and is about 5x faster per game, with roughly a quarter of the
memory of a Python engine that has no pygame or numba loaded.

## Build

```sh
cmake -S cpp -B build
cmake --build build -j
./build/lighthouses --help
```

## Usage

```sh
./build/lighthouses [options] MAP 'bot0 cmd' 'bot1 cmd' ...
./build/lighthouses [options] MAP FAST 'bot0 cmd' 'bot1 cmd' ...
```

Like `game.py`, it plays one game per rotation of the bot list. Each bot
command is run with `/bin/sh -c`. It prints `Launching bots...`,
`Bots launched.`, one `########### ROUND n SCORE: ...` line per round and
finally `repr(scores)`, a Python dict of total score per bot name.

`FAST` reproduces the output of the numba fork's FAST mode
(`numba_accelerated/engine/game.py`). It echoes `FAST`, prints no per-round
lines and defaults to 500 rounds.

Options (they must come before `MAP`):

| option | default | official engine |
|---|---|---|
| `--rounds N` | 1200 (500 with `FAST`) | 1200 |
| `--init-timeout S` | 15 | 15 |
| `--soft-timeout S` | 2 | 0.2 |
| `--hard-timeout S` | 10 | 1.0 |
| `--transcript FILE` | | log of every line sent to / read from each bot |
| `--state-dump FILE` | | full game state after every pre_round, turn and post_round |

The timeout defaults come from the numba fork. Timeouts only change how long
a slow bot is waited for, never what a working bot sees. Use
`--soft-timeout 0.2 --hard-timeout 1` to get the official values.

Unlike `game.py`, the engine does not sleep: 10 s + 5 s before games,
20 ms per round, 2 s after each game and 1500 s at the end. Those sleeps
only exist for the pygame viewer, which is not ported.

## How identical is it?

Everything a bot or the orchestrator can observe is reproduced, including
these Python details:

- **Connection order.** `Game.conns` is a Python `set` of `frozenset`s, and
  its iteration order decides the order of each lighthouse's `connections`
  list in the turn message. `src/pyset.hpp` re-implements CPython 3.10's
  tuple and frozenset hashing and its set table: linear probing, perturbation,
  resize policy, and the rebuild that `decay()` does every round.
- **JSON input.** `src/pyval.cpp` ports CPython 3.10's `_json.c` scanner and
  the UTF-8 decoder. So `NaN`/`Infinity`, big ints, duplicate keys, and every
  `JSONDecodeError`/`UnicodeDecodeError` message match exactly. These
  messages reach stdout through `Bot X failed with exception ...`.
- **`repr()`.** Strings (quote choice, escapes, Unicode printability from
  Python 3.10's tables), floats (shortest round-trip), lists, dicts and
  exceptions all match. They appear in `Invalid command ...` replies, failure
  lines and the final score dict.
- **Type quirks.**
  - `true`/`false` count as ints.
  - `[1.0, 2.0]` and `[true, 2]` are valid lighthouse coordinates.
  - A float move like `{"x": 1.0}` raises a TypeError that kills the bot.
- **Pipe I/O.** Python uses `select()` on the fd followed by a buffered
  `readline()`, so a bot that writes two lines at once ends up one reply out
  of step. A bot that closes its stdout is reported as
  `ValueError('timeout must be non-negative')`. The C++ engine reproduces
  both.
- **Process control.** SIGSTOP after a successful turn, SIGCONT before the
  next one, and the close sequence (SIGCONT, close the pipes, 100 × 10 ms
  polls, SIGINT, the same again, then SIGKILL) are reproduced, as is the
  BrokenPipeError crash when a bot dies between turns.

### Verifying it yourself

The reference runs on **CPython 3.10** (for example `uv python install 3.10`).

```sh
PY310=$(uv python find 3.10)
cmake -S cpp -B build -DPYTHON310=$PY310 && cmake --build build -j
(cd build && ctest)    # rules, plus set order, JSON and repr checked against CPython fixtures
python3 tools/compare.py --cpp build/lighthouses --python $PY310 --runs 50 -j 3
```

`tools/compare.py` plays random scenarios with both engines: every map, 1–8
players, official and FAST modes, and seeded test bots
(`tools/bots/testbot.py`). The bot mix includes greedy connectors, random
bots, a "chaos" bot that sends malformed commands, and injected faults such
as crashes, hangs, bad JSON, invalid UTF-8 and double lines. For each
scenario it compares, byte for byte:

- stdout and the exit code
- the per-bot transcript
- a full state dump after every phase: energy map, lighthouses, connections
  in set order, triangles, players and keys

### Comparing with your own bots

Pass one `--bot` per player, quoted the way you would quote it in a shell.
Repeat the same command to make a bot play itself:

```sh
python3 tools/compare.py --cpp build/lighthouses --python $PY310 \
    --bot 'java -cp Ungoliant/gson-2.8.6.jar:. Ungoliant.Ungoliant' \
    --bot 'java -cp Ungoliant/gson-2.8.6.jar:. Ungoliant.Ungoliant' \
    --map all --rounds 1200
```

| option | meaning |
|---|---|
| `--map PATH` | repeatable; `all` means every map; default `maps/island.txt` |
| `--rounds N` | rounds per game; default 1200, or 500 with `--fast` |
| `--fast` | FAST mode |
| `--runs N` | repeats each map N times |
| `--hard-timeout S` | passed to both engines |
| `--bot-cwd DIR` | working directory for both engines and their bots, for relative classpaths; default: the current directory |

Things to know when using your own bots:

- **Determinism check.** Before comparing, the Python engine plays the
  first scenario twice. If the two runs differ, your bots are not
  deterministic (unseeded randomness, timing, or files they read back), and
  the script stops with exit code 2: engine differences would be
  meaningless. Use a fixed seed, or pass `--no-check-determinism` to compare
  anyway.
- **Parallel runs.** With `--bot`, scenarios run one at a time (`-j 1`),
  because bots that write log files could disturb each other in parallel.
- **Signals and Java bots.** `/bin/sh` is dash on most Linux systems, and it
  runs the command as a child instead of replacing itself with it. So the
  per-turn SIGSTOP/SIGCONT and the SIGINT at close reach only the shell, not
  your JVM, unless the command starts with `exec`. Both engines behave the
  same way.

The older `--scenario 'MAP|ROUNDS|FAST(0/1)|cmd|cmd...'` form still works.

`tools/pyref.py` is the reference runner. It executes the unmodified
`engine/game.py` headless: the pygame view is stubbed out and the long
sleeps are skipped.

### Known differences

None of these affect what a well-behaved bot sees or the scores.

- **Traceback text.** When the Python engine crashes (an uncaught exception,
  exit code 1), the C++ engine prints only the final traceback line, not the
  whole stack.
- **EOF wait.** When a bot closes stdout, Python busy-waits until the hard
  timeout before reporting the error. C++ reports the same error immediately.
- **Races that are racy in Python too.** A bot that exits right after a turn
  races its exit against the engine's next write. Both engines then either
  report the EOF or crash with BrokenPipeError, depending on timing. Bot exit
  codes printed on stderr are also timing-dependent in both engines.
- **Pathological input.**
  - JSON nested about 1000 levels deep: Python's RecursionError depth depends
    on its stack, and C++ cuts off at 990.
  - Bot names containing lone UTF-16 surrogates make Python crash when it
    prints them.
  - Integer literals longer than 4300 digits produce the error message of
    CPython ≥ 3.10.7.

## Performance

4 bots (the trivial C bot `tools/bots/fastbot.c`), 4 rotated games × 1200
rounds, measured with `tools/bench.py`. The CPU column includes the bots.

| map | engine | wall | CPU | engine peak RSS |
|---|---|---|---|---|
| island.txt | Python 3.10 (no pygame, no sleeps) | 9.8 s | 9.6 s | 15.4 MB |
| island.txt | C++ | 2.0 s | 1.9 s | 4.3 MB |
| ee30.txt | Python 3.10 (no pygame, no sleeps) | 11.4 s | 11.1 s | 15.6 MB |
| ee30.txt | C++ | 2.0 s | 1.9 s | 4.3 MB |

Most of the C++ engine's remaining CPU is kernel time: pipe I/O, plus the
SIGCONT/SIGSTOP pair the official engine sends every turn.

## Layout

| file | ports |
|---|---|
| `src/engine.*` | `engine.py`: map loading, energy, attack/connect, scoring |
| `src/geom.hpp` | `geom.py` |
| `src/botplayer.*` | `botplayer.py`: processes, protocol, timeouts |
| `src/main.cpp` | `game.py`, and the fork's FAST mode |
| `src/pyset.hpp` | CPython set/hash emulation |
| `src/pyval.*` | Python JSON decoding, `repr()`, `json.dumps` strings |
| `src/unicode_printable.inc` | generated by `tools/gen_printable.py` |
