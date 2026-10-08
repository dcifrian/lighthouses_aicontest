#!/usr/bin/env python3
"""Deterministic test bot for engine parity runs.

    testbot.py --mode rand|conn|chaos --seed N [--name NAME] [--fault KIND --fault-at TURN]

Given the same seed and the same engine messages it always sends the same
bytes, so two engines that send identical messages get identical replies.

Modes:
  rand   port of examples/RandBot (moves, recharges, connects at random)
  conn   greedy: captures lighthouses and builds as many connections as it
         can (stresses connections, triangles and set ordering)
  chaos  like conn, but often sends odd yet survivable commands: wrong
         types, booleans, huge ints, unknown commands, bad destinations...

--init-fault no_name|int_name|bad_json|exit misbehaves when greeting.

Faults (at turn --fault-at, 0-based) exercise the engine's error paths:
  exit, float_move, bad_json, not_dict, no_command, bad_utf8, double_line,
  partial_exit, hang, empty_line
"""
import argparse, json, os, random, sys, time

SURVIVABLE = [
    {"command": "pass"},
    {"command": "pass", "extra": [1, 2]},
    {"command": "move", "x": True, "y": False},
    {"command": "move", "x": 2, "y": 0},
    {"command": "move"},
    {"command": "move", "x": 1},
    {"command": "move", "x": "1", "y": 0},
    {"command": "move", "x": [1], "y": 0},
    {"command": "move", "x": None, "y": 1},
    {"command": "move", "x": {"a": 1}, "y": 1},
    {"command": "move", "x": 0.5, "y": 0},
    {"command": "move", "x": 0, "y": 0},
    {"command": "attack"},
    {"command": "attack", "energy": 1.5},
    {"command": "attack", "energy": -5},
    {"command": "attack", "energy": True},
    {"command": "attack", "energy": False},
    {"command": "attack", "energy": 10 ** 30},
    {"command": "attack", "energy": -10 ** 30},
    {"command": "attack", "energy": "10"},
    {"command": "attack", "energy": 0},
    {"command": "connect"},
    {"command": "connect", "destination": 5},
    {"command": "connect", "destination": None},
    {"command": "connect", "destination": "ab"},
    {"command": "connect", "destination": [[1, 2]]},
    {"command": "connect", "destination": {"a": 1, "b": 2}},
    {"command": "connect", "destination": [1, 2, 3]},
    {"command": "connect", "destination": []},
    {"command": "jump"},
    {"command": 5},
    {"command": 1.5e300},
    {"command": None},
    {"command": True},
    {"command": ["a", 1.5, None, False, {"k": "v"}]},
    {"command": {"k": "v'\"", "n": -0.0}},
    {"command": "日本 \U0001F642"},
    {"command": "\x07\ud800\x7f\x85 "},
    {"command": "it's"},
    {"command": "say \"hi\""},
    {"command": "both ' and \""},
    {"command": "back\\slash\ttab\nnl"},
]

FAULTS = ["exit", "float_move", "bad_json", "not_dict", "no_command", "bad_utf8",
          "double_line", "partial_exit", "hang", "empty_line", "exit_after",
          "bad_move_exit"]


class Bot(object):
    def __init__(self, args, init):
        self.args = args
        self.rnd = random.Random(args.seed)
        self.num = init["player_num"]
        self.map = init["map"]
        self.lhs = [tuple(p) for p in init["lighthouses"]]
        self.target = None

    def island(self, x, y):
        return 0 <= y < len(self.map) and 0 <= x < len(self.map[0]) and self.map[y][x]

    def valid_moves(self, x, y):
        return [(dx, dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1)
                if (dx or dy) and self.island(x + dx, y + dy)]

    def play_rand(self, state):
        cx, cy = state["position"]
        lhs = dict((tuple(lh["position"]), lh) for lh in state["lighthouses"])
        if (cx, cy) in lhs:
            if lhs[(cx, cy)]["owner"] == self.num and self.rnd.randrange(100) < 60:
                cands = [d for d in self.lhs if d != (cx, cy) and lhs[d]["have_key"] and
                         [cx, cy] not in lhs[d]["connections"] and lhs[d]["owner"] == self.num]
                if cands:
                    return {"command": "connect", "destination": self.rnd.choice(cands)}
            if self.rnd.randrange(100) < 60:
                return {"command": "attack", "energy": self.rnd.randrange(state["energy"] + 1)}
        dx, dy = self.rnd.choice(self.valid_moves(cx, cy))
        return {"command": "move", "x": dx, "y": dy}

    def play_conn(self, state):
        cx, cy = state["position"]
        here = (cx, cy)
        lhs = dict((tuple(lh["position"]), lh) for lh in state["lighthouses"])
        if here in lhs:
            lh = lhs[here]
            if lh["owner"] != self.num and state["energy"] > lh["energy"]:
                extra = self.rnd.randrange(state["energy"] - lh["energy"] + 1)
                return {"command": "attack", "energy": lh["energy"] + extra}
            if lh["owner"] == self.num:
                cands = [d for d in self.lhs if d != here and lhs[d]["have_key"] and
                         list(here) not in lhs[d]["connections"] and lhs[d]["owner"] == self.num]
                if cands and self.rnd.random() < 0.9:
                    return {"command": "connect", "destination": list(self.rnd.choice(cands))}
                if state["energy"] > 0 and self.rnd.random() < 0.3:
                    return {"command": "attack", "energy": self.rnd.randrange(state["energy"] + 1)}
            if self.target == here or self.rnd.random() < 0.5:
                self.target = None
        if self.target is None or self.rnd.random() < 0.05:
            self.target = self.rnd.choice(self.lhs)
        tx, ty = self.target
        dx = (tx > cx) - (tx < cx)
        dy = (ty > cy) - (ty < cy)
        if (dx or dy) and self.island(cx + dx, cy + dy) and self.rnd.random() < 0.85:
            return {"command": "move", "x": dx, "y": dy}
        moves = self.valid_moves(cx, cy)
        if not moves:
            return {"command": "pass"}
        dx, dy = self.rnd.choice(moves)
        return {"command": "move", "x": dx, "y": dy}

    def play_chaos(self, state):
        r = self.rnd.random()
        if r < 0.25:
            return self.rnd.choice(SURVIVABLE)
        if r < 0.30:
            # Lighthouse coordinates in odd but valid spellings.
            x, y = self.rnd.choice(self.lhs)
            return {"command": "connect", "destination":
                    self.rnd.choice([[float(x), float(y)], [x, y], [bool(x) if x <= 1 else x, y],
                                     state["position"]])}
        if r < 0.32:
            # Off-island float moves only produce a MoveError.
            return {"command": "move", "x": -1.0, "y": -1.0} if state["position"][0] <= 1 else {"command": "pass"}
        return self.play_conn(state)


def send_raw(data):
    os.write(1, data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="rand", choices=["rand", "conn", "chaos"])
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--name", default=None)
    ap.add_argument("--fault", choices=FAULTS)
    ap.add_argument("--fault-at", type=int, default=-1)
    ap.add_argument("--init-fault", choices=["no_name", "bad_json", "exit", "int_name"],
                    help="misbehave when greeting (the official engine crashes)")
    args = ap.parse_args()

    stdin = sys.stdin.buffer

    def recv():
        line = stdin.readline()
        if not line:
            sys.exit(0)
        return json.loads(line)

    def send(msg):
        send_raw((json.dumps(msg) + "\n").encode())

    init = recv()
    bot = Bot(args, init)
    name = args.name if args.name is not None else "%s%d" % (args.mode, args.seed)
    if args.init_fault == "no_name":
        send({"nombre": name})
    elif args.init_fault == "int_name":
        send({"name": 5})
    elif args.init_fault == "bad_json":
        send_raw(b"{name}\n")
    elif args.init_fault == "exit":
        sys.exit(0)
    else:
        send({"name": name})
    play = getattr(bot, "play_" + args.mode)
    turn = 0
    while True:
        state = recv()
        if turn == args.fault_at:
            f = args.fault
            if f == "exit":
                sys.exit(3)
            elif f == "float_move":
                moves = bot.valid_moves(*state["position"])
                dx, dy = moves[0]
                send({"command": "move", "x": float(dx), "y": dy})
            elif f == "bad_json":
                send_raw(b'{"command": "pass",}\n')
            elif f == "not_dict":
                send_raw(b'["pass"]\n')
            elif f == "no_command":
                send_raw(b'{"cmd": "pass"}\n')
            elif f == "bad_utf8":
                send_raw(b'{"command": "\xff"}\n')
            elif f == "double_line":
                send_raw(b'{"command": "pass"}\n{"command": "pass"}\n')
            elif f == "partial_exit":
                send_raw(b'{"command": "pa')
                sys.exit(0)
            elif f == "hang":
                time.sleep(3)
                send({"command": "pass"})
            elif f == "empty_line":
                send_raw(b'\n')
            elif f == "exit_after":
                send({"command": "pass"})
                recv()
                sys.exit(0)
            elif f == "bad_move_exit":
                send({"command": "jump"})
                recv()
                sys.exit(0)
        else:
            send(play(state))
        recv()  # move result
        turn += 1


if __name__ == "__main__":
    main()
