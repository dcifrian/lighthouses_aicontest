#!/usr/bin/env python3
"""Run the official Python engine and the C++ engine side by side.

    compare.py --cpp BUILD/lighthouses --python PY310 [--runs N] [--seed S] [-j J]
    compare.py --cpp ... --python ... --scenario 'MAP|ROUNDS|FAST|bot cmd|bot cmd...'

Each scenario is played by both engines with the same deterministic test
bots (tools/bots/testbot.py). Compared byte for byte:
  * stdout and exit code
  * transcript: every line sent to and read from each bot
  * state dump: full game state after every pre_round/turn/post_round
stderr is compared after dropping timing-dependent lines (soft-timeout
warnings) and tracebacks, and only reported as a warning.
"""
import argparse, concurrent.futures, difflib, glob, os, random, shlex, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTBOT = os.path.join(ROOT, "tools", "bots", "testbot.py")
PYREF = os.path.join(ROOT, "tools", "pyref.py")

# Faults whose outcome is deterministic in the Python engine. exit_after and
# bad_move_exit race the bot's exit against the engine's next write, so
# Python itself is not deterministic there (see cpp/README.md).
FAULTS = ["exit", "float_move", "bad_json", "not_dict", "no_command", "bad_utf8",
          "double_line", "partial_exit", "empty_line", "hang"]
NAMES = ["it's", 'say "hi"', "both ' \"", "\u65e5\u672c", "tab\there", "back\\slash", "\u00e9\u2028x"]


def map_players(path):
    with open(path) as f:
        return sum(1 for c in f.read() if c not in "#! \n\r")


def random_scenario(r, bot_python):
    maps = sorted(glob.glob(os.path.join(ROOT, "maps", "*.txt")) +
                  glob.glob(os.path.join(ROOT, "maps", "*", "*.txt")))
    m = r.choice(maps)
    nplayers = min(map_players(m), r.choice([2, 2, 2, 3, 4, 4, 5, 8]))
    fast = r.random() < 0.2
    rounds = r.choice([1, 5, 30, 100, 200, 400])
    bots, hard = [], None
    for k in range(nplayers):
        cmd = [bot_python, TESTBOT, "--mode", r.choice(["rand", "conn", "conn", "chaos", "chaos"]),
               "--seed", str(r.randrange(10 ** 6))]
        if r.random() < 0.15:
            cmd += ["--name", r.choice(NAMES)]
        if r.random() < 0.1:
            fault = r.choice(FAULTS)
            cmd += ["--fault", fault, "--fault-at", str(r.randrange(max(1, rounds)))]
            if fault == "hang":
                hard = 1.0
        bots.append(" ".join(shlex.quote(c) for c in cmd))
    return {"map": m, "rounds": rounds, "fast": fast, "bots": bots, "hard": hard}


def run_engine(cmd, workdir, tag, timeout):
    out_path = os.path.join(workdir, tag + ".out")
    err_path = os.path.join(workdir, tag + ".err")
    with open(out_path, "wb") as out, open(err_path, "wb") as err:
        try:
            rc = subprocess.run(cmd, stdout=out, stderr=err, stdin=subprocess.DEVNULL,
                                timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            rc = "timeout"
    return rc


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return ""


def clean_stderr(text):
    keep = []
    skip_tb = False
    for line in text.splitlines():
        if line.endswith("over soft timeout"):
            continue
        if line.startswith("Traceback") or line.startswith("During handling") or not line:
            skip_tb = line.startswith("Traceback") or skip_tb
            continue
        if skip_tb:
            if line.startswith(" ") or line.startswith("During handling") or not line:
                continue
            skip_tb = False
            continue  # the exception line itself (C++ only approximates it)
        keep.append(line)
    return sorted(keep)  # bot stderr interleaves nondeterministically


def first_diff(a, b, name):
    al, bl = a.splitlines(), b.splitlines()
    for i, (x, y) in enumerate(zip(al, bl)):
        if x != y:
            return "%s differs at line %d:\n  py:  %s\n  cpp: %s" % (name, i + 1, x[:400], y[:400])
    if len(al) != len(bl):
        return "%s length differs: py %d lines, cpp %d lines" % (name, len(al), len(bl))
    return None


def run_scenario(sc, args, workdir):
    os.makedirs(workdir, exist_ok=True)
    common = ["--rounds", str(sc["rounds"])]
    if sc.get("hard"):
        common += ["--hard-timeout", str(sc["hard"])]
    py_cmd = [args.python, PYREF] + common + ["--transcript", os.path.join(workdir, "py.tr"),
                                              "--state-dump", os.path.join(workdir, "py.st")]
    if sc["fast"]:
        py_cmd.append("--fast")
    py_cmd += [sc["map"]] + sc["bots"]
    cpp_cmd = [args.cpp] + common + ["--transcript", os.path.join(workdir, "cpp.tr"),
                                     "--state-dump", os.path.join(workdir, "cpp.st"), sc["map"]]
    if sc["fast"]:
        cpp_cmd.append("FAST")
    cpp_cmd += sc["bots"]
    with open(os.path.join(workdir, "scenario.txt"), "w") as f:
        f.write(" ".join(shlex.quote(c) for c in py_cmd) + "\n" +
                " ".join(shlex.quote(c) for c in cpp_cmd) + "\n")
    rc_py = run_engine(py_cmd, workdir, "py", args.timeout)
    rc_cpp = run_engine(cpp_cmd, workdir, "cpp", args.timeout)
    problems, warnings = [], []
    if rc_py != rc_cpp:
        problems.append("exit code: py %r, cpp %r" % (rc_py, rc_cpp))
    for ext, name in (("out", "stdout"), ("tr", "transcript"), ("st", "state dump")):
        d = first_diff(read(os.path.join(workdir, "py." + ext)), read(os.path.join(workdir, "cpp." + ext)), name)
        if d:
            problems.append(d)
    if clean_stderr(read(os.path.join(workdir, "py.err"))) != clean_stderr(read(os.path.join(workdir, "cpp.err"))):
        a = clean_stderr(read(os.path.join(workdir, "py.err")))
        b = clean_stderr(read(os.path.join(workdir, "cpp.err")))
        warnings.append("stderr differs:\n" + "\n".join(
            difflib.unified_diff(a, b, "py", "cpp", lineterm="", n=0)))
    return problems, warnings


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpp", required=True)
    ap.add_argument("--python", required=True, help="CPython 3.10 interpreter for the reference")
    ap.add_argument("--bot-python", default=sys.executable)
    ap.add_argument("--runs", type=int, default=20)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("-j", "--jobs", type=int, default=2)
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--workdir")
    ap.add_argument("--scenario", action="append",
                    help="MAP|ROUNDS|FAST(0/1)|bot cmd|bot cmd... (repeatable)")
    args = ap.parse_args()
    workroot = args.workdir or tempfile.mkdtemp(prefix="lh-compare-")

    if args.scenario:
        scenarios = []
        for s in args.scenario:
            parts = s.split("|")
            scenarios.append({"map": parts[0], "rounds": int(parts[1]), "fast": parts[2] == "1",
                              "bots": parts[3:], "hard": None})
    else:
        r = random.Random(args.seed)
        scenarios = [random_scenario(r, args.bot_python) for _ in range(args.runs)]

    failed = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        futs = {ex.submit(run_scenario, sc, args, os.path.join(workroot, "run%04d" % i)): i
                for i, sc in enumerate(scenarios)}
        for fut in concurrent.futures.as_completed(futs):
            i = futs[fut]
            problems, warnings = fut.result()
            sc = scenarios[i]
            label = "run%04d %s rounds=%d%s players=%d" % (
                i, os.path.relpath(sc["map"], ROOT), sc["rounds"], " FAST" if sc["fast"] else "", len(sc["bots"]))
            if problems:
                failed += 1
                print("FAIL " + label)
                for p in problems:
                    print("  " + p.replace("\n", "\n  "))
            else:
                print("ok   " + label)
            for w in warnings:
                print("  warning: " + w.replace("\n", "\n    "))
            sys.stdout.flush()
    print("%d/%d scenarios identical (artifacts in %s)" % (len(scenarios) - failed, len(scenarios), workroot))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
