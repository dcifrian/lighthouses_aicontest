#!/usr/bin/env python3
"""Run the official Python engine headless, for side-by-side comparisons.

    pyref.py [options] MAP BOT_CMD [BOT_CMD ...]

Executes engine/game.py unmodified except for:
  * the pygame view is replaced by a no-op stub,
  * sleeps longer than 15 ms are skipped (botplayer's 10 ms polls are kept),
  * ROUNDS and the bot timeouts can be overridden,
  * optional instrumentation (--state-dump, --transcript) that only observes.

Use --fast to run the numba fork's game loop (numba_accelerated/engine/game.py,
`FAST` mode) on top of the official engine modules.
"""
import argparse, json, os, re, sys, time, types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine-dir", default=os.path.join(ROOT, "engine"))
    ap.add_argument("--fast", action="store_true",
                    help="use the numba fork's game.py in FAST mode")
    ap.add_argument("--rounds", type=int)
    ap.add_argument("--games", type=int, help="number of games (default: one per bot)")
    ap.add_argument("--init-timeout", type=float)
    ap.add_argument("--soft-timeout", type=float)
    ap.add_argument("--hard-timeout", type=float)
    ap.add_argument("--state-dump")
    ap.add_argument("--transcript")
    ap.add_argument("map")
    ap.add_argument("bots", nargs="*")
    return ap.parse_args()


def install_view_stub():
    view = types.ModuleType("view")

    class GameView(object):
        def __init__(self, game=None, fast=False):
            self.title = ""

        def attach(self, game):
            pass

        def update(self):
            pass

    view.GameView = GameView
    sys.modules["view"] = view


def install_sleep_filter():
    real_sleep = time.sleep

    def sleep(secs):
        if secs <= 0.015:
            real_sleep(secs)

    time.sleep = sleep


def canonical_state(game):
    isl = game.island
    lhs = [[lh.pos[0], lh.pos[1], -1 if lh.owner is None else lh.owner, lh.energy]
           for lh in game.lighthouses.values()]
    conns = [sorted(list(p) for p in c) for c in game.conns]
    tris = [[list(p) for p in t] + [len(cells)] for t, cells in game.tris.items()]
    players = [[p.pos[0], p.pos[1], p.score, p.energy, sorted(list(k) for k in p.keys)]
               for p in game.players]
    energy = [[isl.energy[x, y] for x in range(isl.w)] for y in range(isl.h)]
    return {"lighthouses": lhs, "conns": conns, "tris": tris, "players": players,
            "energy": energy}


def install_instrumentation(args, engine, botplayer):
    if args.state_dump:
        out = open(args.state_dump, "w")
        orig_pre, orig_post = engine.Game.pre_round, engine.Game.post_round
        orig_turn = botplayer.BotPlayer.turn

        def dump(game, tag):
            out.write(tag + " " + json.dumps(canonical_state(game)) + "\n")

        def pre_round(self):
            orig_pre(self)
            dump(self, "pre")

        def post_round(self):
            orig_post(self)
            dump(self, "post")

        def turn(self):
            orig_turn(self)
            dump(self.game, "turn%d" % self.player.num)

        engine.Game.pre_round = pre_round
        engine.Game.post_round = post_round
        botplayer.BotPlayer.turn = turn

    if args.transcript:
        log = open(args.transcript, "w")
        orig_init = botplayer.BotPlayer.__init__
        orig_send = botplayer.BotPlayer._send

        class StdoutProxy(object):
            def __init__(self, f, num):
                self._f, self._num = f, num
                self._eof = False

            def fileno(self):
                return self._f.fileno()

            def readline(self):
                data = self._f.readline()
                # At EOF the engine spins on readline() until its hard
                # timeout; log the first empty read only.
                if not (data == b"" and self._eof):
                    log.write("<%d %r\n" % (self._num, data))
                self._eof = data == b""
                return data

            def close(self):
                self._f.close()

        def __init__(self, game, playernum, cmdline, debug=False):
            orig_init(self, game, playernum, cmdline, debug)
            self.p.stdout = StdoutProxy(self.p.stdout, playernum)

        def _send(self, data):
            log.write(">%d %s\n" % (self.player.num, json.dumps(data)))
            return orig_send(self, data)

        botplayer.BotPlayer.__init__ = __init__
        botplayer.BotPlayer._send = _send


def main():
    args = parse_args()
    sys.path.insert(0, args.engine_dir)
    install_view_stub()
    install_sleep_filter()
    import engine, botplayer
    for attr, val in (("INIT_TIMEOUT", args.init_timeout), ("MOVE_TIMEOUT", args.soft_timeout),
                      ("MOVE_HARDTIMEOUT", args.hard_timeout)):
        if val is not None:
            setattr(botplayer.BotPlayer, attr, val)
    install_instrumentation(args, engine, botplayer)

    if args.fast:
        script = os.path.join(ROOT, "numba_accelerated", "engine", "game.py")
        argv = [script, args.map, "FAST"] + args.bots
    else:
        script = os.path.join(args.engine_dir, "game.py")
        argv = [script, args.map] + args.bots
    with open(script) as f:
        src = f.read()
    if args.rounds is not None:
        src = re.sub(r"^ROUNDS\s*=\s*\d+", "ROUNDS=%d" % args.rounds, src, flags=re.M)
    if args.games is not None:
        # perms = [... for i in range(len(bots))]: same rotations, N games
        src, k = re.subn(r"(^perms = .*for i in range\()len\(bots\)(\)\])", r"\g<1>%d\2" % args.games,
                         src, flags=re.M)
        assert k == 1, "could not patch the number of games in " + script
    sys.argv = argv
    code = compile(src, script, "exec")
    exec(code, {"__name__": "__main__", "__file__": script})


if __name__ == "__main__":
    main()
