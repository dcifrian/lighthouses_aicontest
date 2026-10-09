#!/usr/bin/env python3
"""Viewer for the Lighthouses C++ engine (pygame).

    viewer.py GAME.jsonl[.gz]      watch a recording (lighthouses --record FILE ...)
    viewer.py --live               used by `lighthouses --view`: frames arrive on
                                   stdin and the viewer paces the engine by
                                   answering "next" on stdout

The board is drawn like the official engine/view.py: cells shaded by their
energy, triangles tinted with their owner's colour, lighthouses as diamonds,
lasers as lines and players as small squares.

Keys:
  Space          play / pause
  Right, Left    one frame forward / back (a frame is a pre_round, a bot's
                 turn or a post_round)
  Shift+Right/Left  one round forward / back
  Up, Down       faster / slower
  F              fast: follow the engine as quickly as the bots answer
  Home, End      first / last frame
  PgUp, PgDn     previous / next game
  Q, Esc         quit (in --live mode the engine finishes the games headless)

Options: --speed N, --paused, --fast, --exit-at-end, --size WxH. With
lighthouses --view they go in --viewer, e.g.
  lighthouses --view --viewer "python3 viewer/viewer.py --live --paused" ...
"""
import argparse, gzip, json, math, os, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PLAYERC = [
    (255, 0, 0), (0, 0, 255), (0, 255, 0), (255, 255, 0),
    (0, 255, 255), (255, 0, 255), (255, 127, 0), (255, 127, 127),
    (128, 0, 0), (0, 0, 128), (0, 128, 0), (128, 128, 0),
    (0, 128, 128), (128, 0, 128), (128, 127, 0), (128, 127, 127),
]
SPEEDS = [1, 2, 5, 10, 25, 60]  # frames per second
PANEL_W = 360
CELL = 15  # view.py's cell size; drawing below is in these units, then scaled


def cmul(col, mul):
    r, g, b = col
    return int(r * mul), int(g * mul), int(b * mul)


def calpha(col1, col2, a):
    r1, g1, b1 = col1
    r2, g2, b2 = col2
    return (int(r2 * a + r1 * (1 - a)), int(g2 * a + g1 * (1 - a)), int(b2 * a + b1 * (1 - a)))


class Store(object):
    """All games and frames seen so far (appended to by the reader thread)."""

    def __init__(self):
        self.games = []
        self.frames = []
        self.end_scores = None
        self.ended = False
        self.lock = threading.Lock()

    def add(self, msg):
        t = msg.get("type")
        with self.lock:
            if t == "game":
                msg["tri_cells"] = {}
                self.games.append(msg)
            elif t == "frame" and self.games:
                msg["gi"] = len(self.games) - 1
                if "energy" not in msg:
                    prev = self.frames[-1] if self.frames else None
                    msg["energy"] = prev["energy"] if prev and prev["gi"] == msg["gi"] else None
                self.frames.append(msg)
            elif t == "end":
                self.end_scores = msg.get("scores")


class Engine(object):
    """The live engine connection: frames on stdin, "next" acks on a private fd."""

    def __init__(self, store, ack_fd, in_fd=0):
        self.store = store
        self.ack_fd = ack_fd
        self.in_fd = in_fd
        self.waiting = False      # the engine is blocked until we answer
        self.auto_ack = False     # fast mode: answer as soon as a frame arrives
        self.closed = False
        self.lock = threading.Lock()
        threading.Thread(target=self._reader, daemon=True).start()

    def _lines(self):
        # Raw reads (no buffered sys.stdin) so the thread never holds a lock
        # that would block interpreter shutdown.
        buf = b""
        while True:
            try:
                chunk = os.read(self.in_fd, 65536)
            except OSError:
                chunk = b""
            if not chunk:
                return
            buf += chunk
            *lines, buf = buf.split(b"\n")
            for line in lines:
                yield line

    def _reader(self):
        for line in self._lines():
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            self.store.add(msg)
            if msg.get("type") == "game":
                self.next()  # nothing to show yet, keep going to the first frame
            elif msg.get("type") == "frame":
                with self.lock:
                    self.waiting = True
                if self.auto_ack:
                    self.next()
        self.store.ended = True

    def next(self):
        with self.lock:
            if self.closed:
                return
            self.waiting = False
            try:
                os.write(self.ack_fd, b"next\n")
            except OSError:
                self.closed = True

    def close(self):
        with self.lock:
            if not self.closed:
                self.closed = True
                os.close(self.ack_fd)


class Viewer(object):
    def __init__(self, store, engine, size, speed=10, playing=True, fast=False, exit_at_end=False):
        self.store = store
        self.engine = engine
        self.cursor = -1
        self.playing = playing
        self.speed = min(range(len(SPEEDS)), key=lambda i: abs(SPEEDS[i] - speed))  # index into SPEEDS
        self.fast = fast
        self.exit_at_end = exit_at_end
        self.seek = None  # predicate: keep moving forward until a frame matches
        self.next_time = 0.0
        pygame.init()
        pygame.display.set_caption("Lighthouses")
        self.screen = pygame.display.set_mode(size, pygame.RESIZABLE)
        self.font = pygame.font.Font(None, 24)
        self.small = pygame.font.Font(None, 20)
        sys.path.insert(0, os.path.join(ROOT, "engine"))
        import geom  # the official triangle rasteriser
        self.geom = geom

    # -- navigation ---------------------------------------------------------
    def frames(self):
        return self.store.frames

    def forward(self, until=None):
        self.seek = until or (lambda f: True)

    def back(self, until=None):
        self.seek = None
        i = self.cursor - 1
        while i > 0 and until and not until(self.frames()[i]):
            i -= 1
        self.cursor = max(0, i)

    def step_seek(self, budget):
        """Advance towards the seek target; ask the engine for more if needed."""
        frames = self.frames()
        while self.seek and budget > 0:
            if self.cursor < len(frames) - 1:
                self.cursor += 1
                budget -= 1
                if self.seek(frames[self.cursor]):
                    self.seek = None
            elif self.engine and self.engine.waiting and not self.store.ended:
                self.engine.next()
                return
            else:
                if not self.engine or self.store.ended:
                    self.seek = None
                    self.playing = False
                return

    def update(self):
        now = time.monotonic()
        if self.cursor < 0 and self.frames():
            self.cursor = 0
        if self.engine:
            self.engine.auto_ack = self.fast
        if self.fast:
            if self.engine:
                self.cursor = len(self.frames()) - 1
                if self.engine.waiting:
                    self.engine.next()
            else:
                self.seek = self.seek or (lambda f: False)
                self.step_seek(40)
            return
        if self.seek:
            self.step_seek(1 if self.playing else 10 ** 6)
        elif self.playing and now >= self.next_time:
            self.next_time = now + 1.0 / SPEEDS[self.speed]
            self.forward()
            self.step_seek(1)

    def handle_key(self, ev):
        k, shift = ev.key, ev.mod & pygame.KMOD_SHIFT
        frames = self.frames()
        if k in (pygame.K_q, pygame.K_ESCAPE):
            return False
        if not frames:
            return True
        if k == pygame.K_SPACE:
            self.playing = not self.playing
            self.fast = False
            self.seek = None
        elif k == pygame.K_RIGHT:
            self.playing = self.fast = False
            self.forward((lambda f: f["phase"] == "post") if shift else None)
            self.step_seek(10 ** 6)
        elif k == pygame.K_LEFT:
            self.playing = self.fast = False
            self.back((lambda f: f["phase"] == "post") if shift else None)
        elif k == pygame.K_UP:
            self.speed = min(len(SPEEDS) - 1, self.speed + 1)
        elif k == pygame.K_DOWN:
            self.speed = max(0, self.speed - 1)
        elif k == pygame.K_f:
            self.fast = not self.fast
            self.playing = not self.fast and self.playing
            self.seek = None
        elif k == pygame.K_HOME:
            self.cursor, self.seek, self.playing = 0, None, False
        elif k == pygame.K_END:
            self.cursor, self.seek, self.playing = len(frames) - 1, None, False
        elif k == pygame.K_PAGEUP and frames:
            # Start of this game, or of the previous one if already there.
            gi = frames[self.cursor]["gi"]
            start = next(i for i, f in enumerate(frames) if f["gi"] == gi)
            if self.cursor == start and gi > 0:
                start = next(i for i, f in enumerate(frames) if f["gi"] == gi - 1)
            self.cursor, self.seek, self.playing = start, None, False
        elif k == pygame.K_PAGEDOWN and frames:
            gi = frames[self.cursor]["gi"]
            self.playing = False
            self.forward(lambda f: f["gi"] > gi)
            self.step_seek(10 ** 6)
        return True

    # -- drawing ------------------------------------------------------------
    def tri_cells(self, game, tri):
        key = tuple(tri)
        cells = game["tri_cells"].get(key)
        if cells is None:
            pts = tuple(tuple(game["lighthouses"][i]) for i in tri)
            isl = game["island"]
            cells = [p for p in self.geom.render(pts)
                     if 0 <= p[1] < len(isl) and 0 <= p[0] < len(isl[0]) and isl[p[1]][p[0]]]
            game["tri_cells"][key] = cells
        return cells

    def draw_board(self, game, frame, area):
        isl = game["island"]
        h, w = len(isl), len(isl[0])
        scale = max(0.2, min(area.width / float(w * CELL), area.height / float(h * CELL)))
        cs = CELL * scale
        ox = area.x + (area.width - w * cs) / 2.0
        oy = area.y + (area.height - h * cs) / 2.0
        nh = h - 1

        def rect(x, y, ww, hh, color):
            x0, y0 = int(ox + x * scale), int(oy + y * scale)
            x1, y1 = int(ox + (x + ww) * scale), int(oy + (y + hh) * scale)
            self.screen.fill(color, (x0, y0, max(1, x1 - x0), max(1, y1 - y0)))

        lhs = game["lighthouses"]
        lh_index = dict((tuple(p), i) for i, p in enumerate(lhs))
        owners = [o for o, e in frame["lh"]]
        tint = {}
        for tri in frame["tris"]:
            owner = owners[tri[0]]
            for c in self.tri_cells(game, tri):
                tint.setdefault(c, []).append(owner)
        at = {}
        for num, p in enumerate(frame["players"]):
            at.setdefault((p[0], p[1]), []).append(num)
        energy = frame["energy"]
        for cy in range(h):
            for cx in range(w):
                if not isl[cy][cx]:
                    continue
                px, py = cx * CELL, (nh - cy) * CELL
                c = int((energy[cy][cx] if energy else 0) / 100.0 * 25)
                bg = (int(25 + c * 0.8), int(25 + c * 0.8), int(25 + c))
                for owner in tint.get((cx, cy), ()):
                    if owner >= 0:
                        bg = calpha(bg, PLAYERC[owner % len(PLAYERC)], 0.15)
                rect(px, py, CELL, CELL, bg)
                rect(px + CELL // 2, py + CELL // 2, 1, 1, (255, 255, 255))
                cplayers = at.get((cx, cy))
                if cplayers:
                    nx = int(math.ceil(math.sqrt(len(cplayers))))
                    ny = int(math.ceil(len(cplayers) / float(nx)))
                    wx, wy = 12.0 / nx, 12.0 / ny
                    for i, num in enumerate(cplayers):
                        iy, ix = i // nx, i % nx
                        rect(px + 2 + ix * wx, py + 2 + iy * wy, wx - 1, wy - 1,
                             cmul(PLAYERC[num % len(PLAYERC)], 0.5))
                li = lh_index.get((cx, cy))
                if li is not None:
                    owner = owners[li]
                    color = PLAYERC[owner % len(PLAYERC)] if owner >= 0 else (192, 192, 192)
                    mx, my = ox + (px + CELL / 2.0) * scale, oy + (py + CELL / 2.0) * scale
                    s = 4 * scale
                    pygame.draw.polygon(self.screen, color,
                                        [(mx - s, my), (mx, my - s), (mx + s, my), (mx, my + s)])
        width = max(1, int(round(scale)))
        for a, b in frame["conns"]:
            owner = owners[a]
            color = PLAYERC[owner % len(PLAYERC)] if owner >= 0 else (192, 192, 192)
            (x0, y0), (x1, y1) = lhs[a], lhs[b]
            p0 = (ox + (x0 * CELL + CELL / 2.0) * scale, oy + ((nh - y0) * CELL + CELL / 2.0) * scale)
            p1 = (ox + (x1 * CELL + CELL / 2.0) * scale, oy + ((nh - y1) * CELL + CELL / 2.0) * scale)
            if width == 1:
                pygame.draw.aaline(self.screen, color, p0, p1)
            else:
                pygame.draw.line(self.screen, color, p0, p1, width)

    def text(self, s, pos, color=(255, 255, 255), font=None):
        surf = (font or self.font).render(s, True, color)
        self.screen.blit(surf, pos)
        return surf.get_height()

    def draw_panel(self, game, frame, x, y):
        y += self.text("Game %d/%d   Round %d/%d" % (game["game"] + 1, game["games"], frame["round"] + 1,
                                                     game["rounds"]), (x, y)) + 4
        phase = frame["phase"]
        if phase == "turn":
            names = game["names"]
            p = frame["player"]
            phase = "after %s's turn" % (names[p] if p < len(names) else "P%d" % p)
        elif phase == "pre":
            phase = "start of round"
        else:
            phase = "end of round (scored)"
        y += self.text(phase, (x, y), (180, 180, 180), self.small) + 14
        owners = [o for o, e in frame["lh"]]
        for num, p in enumerate(frame["players"]):
            px, py, score, energy, keys, alive = p
            name = game["names"][num] if num < len(game["names"]) else "P%d" % num
            color = PLAYERC[num % len(PLAYERC)]
            total = game["cumulative"].get(name, 0) + score
            y += self.text("P%d %s%s" % (num, name, "" if alive else "  (killed)"), (x, y), color)
            y += self.text("score %d   total %d" % (score, total), (x + 14, y), (220, 220, 220), self.small)
            y += self.text("energy %d   lighthouses %d   keys %d" % (energy, owners.count(num), len(keys)),
                           (x + 14, y), (220, 220, 220), self.small) + 8
        y += 10
        frames = self.frames()
        if self.fast:
            state = "FAST"
        elif self.playing:
            state = "PLAYING  %d frames/s" % SPEEDS[self.speed]
        else:
            state = "PAUSED"
        y += self.text(state, (x, y), (255, 255, 160)) + 2
        y += self.text("frame %d / %d" % (self.cursor + 1, len(frames)), (x, y), (180, 180, 180), self.small)
        if self.engine:
            if self.store.ended:
                src = "engine finished"
            else:
                src = "live (engine waiting)" if self.engine.waiting else "live"
        else:
            src = "recording"
        y += self.text(src, (x, y), (180, 180, 180), self.small) + 10
        if self.store.end_scores is not None and self.cursor == len(frames) - 1:
            y += self.text("Final scores", (x, y)) + 2
            for name, sc in self.store.end_scores.items():
                y += self.text("%s: %d" % (name, sc), (x + 14, y), (220, 220, 220), self.small)
            y += 10
        for line in ("Space play/pause   F fast", "Left/Right frame   +Shift round",
                     "Up/Down speed   Home/End", "PgUp/PgDn game   Q quit"):
            y += self.text(line, (x, y), (130, 130, 130), self.small)

    def draw(self):
        self.screen.fill((0, 0, 0))
        frames = self.frames()
        W, H = self.screen.get_size()
        if self.cursor < 0 and frames:
            self.cursor = 0
        if self.cursor < 0:
            self.text("waiting for the engine..." if self.engine else "empty recording", (20, 20))
        else:
            frame = frames[self.cursor]
            game = self.store.games[frame["gi"]]
            self.draw_board(game, frame, pygame.Rect(10, 10, max(10, W - PANEL_W - 20), max(10, H - 20)))
            self.draw_panel(game, frame, W - PANEL_W + 10, 14)
        pygame.display.flip()

    def run(self):
        clock = pygame.time.Clock()
        running = True
        while running:
            for ev in pygame.event.get():
                if ev.type == pygame.QUIT:
                    running = False
                elif ev.type == pygame.KEYDOWN:
                    running = self.handle_key(ev)
            self.update()
            self.draw()
            clock.tick(60)
            if self.exit_at_end and self.store.ended and self.cursor >= len(self.frames()) - 1:
                running = False
        pygame.quit()
        if self.engine:
            # Closing the ack pipe lets the engine finish the games headless.
            self.engine.close()
            sys.stdout.flush()
            sys.stderr.flush()
            os._exit(0)


def load(path, store):
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8") as f:
        for line in f:
            if line.strip():
                store.add(json.loads(line))
    store.ended = True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording", nargs="?")
    ap.add_argument("--live", action="store_true")
    ap.add_argument("--size", default="1400x800", help="window size WxH")
    ap.add_argument("--speed", type=int, default=10, help="initial playback speed in frames/s")
    ap.add_argument("--paused", action="store_true", help="start paused")
    ap.add_argument("--fast", action="store_true", help="start in fast mode")
    ap.add_argument("--exit-at-end", action="store_true", help="close the window after the last frame")
    ap.add_argument("--screenshot", nargs=2, metavar=("FRAME", "PNG"),
                    help="render frame FRAME (negative counts from the end) of a recording to PNG and exit")
    args = ap.parse_args()
    size = tuple(int(v) for v in args.size.lower().split("x"))

    ack_fd = None
    if args.live:
        # stdout is the ack pipe to the engine: keep it private and send any
        # stray output (pygame's banner, prints) to stderr instead.
        ack_fd = os.dup(1)
        os.dup2(2, 1)
    os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")
    global pygame
    import pygame

    store = Store()
    opts = dict(speed=args.speed, playing=not args.paused, fast=args.fast, exit_at_end=args.exit_at_end)
    if args.live:
        viewer = Viewer(store, Engine(store, ack_fd), size, **opts)
    elif args.recording:
        load(args.recording, store)
        viewer = Viewer(store, None, size, **opts)
    else:
        ap.error("give a recording file or --live")

    if args.screenshot:
        n = len(store.frames)
        idx = int(args.screenshot[0])
        viewer.cursor = idx if idx >= 0 else n + idx
        viewer.playing = False
        viewer.draw()
        pygame.image.save(viewer.screen, args.screenshot[1])
        pygame.quit()
        return
    viewer.run()


if __name__ == "__main__":
    main()
