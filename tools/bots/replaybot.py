#!/usr/bin/env python3
"""Replay the replies a bot sent in a recorded game.

    replaybot.py RECORDING CLAIMS_DIR

RECORDING is JSON mapping "P-K" (player number, game index) to the list of
raw lines that player's bot wrote in game K, as latin-1 strings (built by
tools/compare.py from a pyref.py transcript). The bot learns P from the init
message and claims the next unused K for that player by creating marker files
in CLAIMS_DIR (games run one after another, so claims follow the recorded
order). It then answers every init/state message with the next recorded
line, ignoring the content, and exits when the recording runs out, as the
real bot did.

If the engine under test sends the same messages as the recorded engine, the
recorded replies are the right ones; the first differing message shows where
the engines diverge. This lets a non-deterministic bot serve as a parity test.
"""
import json, os, sys


def claim(claims_dir, player):
    os.makedirs(claims_dir, exist_ok=True)
    k = 0
    while True:
        try:
            os.close(os.open(os.path.join(claims_dir, "%d-%d" % (player, k)), os.O_CREAT | os.O_EXCL))
            return k
        except FileExistsError:
            k += 1


def main():
    recording, claims_dir = sys.argv[1], sys.argv[2]
    with open(recording) as f:
        rec = json.load(f)
    stdin = sys.stdin.buffer
    msg = stdin.readline()
    if not msg:
        return
    player = json.loads(msg)["player_num"]
    replies = rec.get("%d-%d" % (player, claim(claims_dir, player)), [])
    for reply in replies:
        os.write(1, reply.encode("latin-1"))
        while True:  # next message that expects a reply (skip move results)
            msg = stdin.readline()
            if not msg:
                return
            if not msg.startswith(b'{"success"'):
                break


if __name__ == "__main__":
    main()
