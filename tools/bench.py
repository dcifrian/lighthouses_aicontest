#!/usr/bin/env python3
"""Measure wall time, CPU time and peak RSS of an engine run.

    bench.py -- ENGINE_CMD...

CPU time includes the bots (they are children of the engine), so benchmark
with a cheap bot such as tools/bots/fastbot.c to see the engine's own cost.
Peak RSS is the engine process alone (sampled from /proc/PID/status VmHWM).
"""
import os, subprocess, sys, threading, time


def watch_hwm(pid, result):
    while True:
        try:
            with open("/proc/%d/status" % pid) as f:
                for line in f:
                    if line.startswith("VmHWM:"):
                        result[0] = max(result[0], int(line.split()[1]))
        except (FileNotFoundError, ProcessLookupError):
            return
        time.sleep(0.02)

def main():
    argv = sys.argv[1:]
    if argv and argv[0] == "--":
        argv = argv[1:]
    t0 = time.monotonic()
    p = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    hwm = [0]
    t = threading.Thread(target=watch_hwm, args=(p.pid, hwm), daemon=True)
    t.start()
    # Reap only once the watcher has seen the final VmHWM (zombies lose it).
    own = None
    while True:
        try:
            with open("/proc/%d/stat" % p.pid) as f:
                fields = f.read().split(")")[-1].split()
            if fields[0] == "Z":
                # utime/stime of the engine itself (not its bots), in clock ticks
                tick = os.sysconf("SC_CLK_TCK")
                own = (int(fields[11]) / tick, int(fields[12]) / tick)
                break
        except FileNotFoundError:
            break
        time.sleep(0.01)
    t.join(0.1)
    _, status, ru = os.wait4(p.pid, 0)
    wall = time.monotonic() - t0
    print("wall %.2f s  cpu %.2f s (user %.2f + sys %.2f, incl. bots)  engine peak rss %.1f MB  exit %d" % (
        wall, ru.ru_utime + ru.ru_stime, ru.ru_utime, ru.ru_stime, hwm[0] / 1024.0,
        os.waitstatus_to_exitcode(status)))
    if own:
        print("engine alone: user %.2f s + sys %.2f s" % own)

if __name__ == "__main__":
    main()
