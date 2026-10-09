#!/usr/bin/env python3
"""Run the official Python engine and the C++ engine side by side.

Random scenarios with the bundled deterministic test bots:

    compare.py --cpp build/lighthouses --python PY310 [--runs N] [--seed S] [-j J]

Your own bots (one --bot per player; the same command may be repeated):

    compare.py --cpp build/lighthouses --python PY310 \
        --bot 'java -cp Ungoliant/gson-2.8.6.jar:. Ungoliant.Ungoliant' \
        --bot 'python3 examples/RandBot/randbot.py' \
        [--map maps/island.txt | --map all] [--rounds N] [--fast] [--runs N]

With --bot, the Python engine is first run twice to check that the bots are
deterministic (same input -> same output); otherwise differences between the
engines could come from the bots and the comparison would be meaningless.

Each scenario is played by both engines with the same bots. Compared byte
for byte:
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


def random_scenario(r, bot_python):
    m = r.choice(all_maps())
    nplayers = min(map_starts(m), r.choice([2, 2, 2, 3, 4, 4, 5, 8]))
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


SCENARIO_HELP = """--scenario takes 'MAP|ROUNDS|FAST|BOT CMD|BOT CMD...', fields separated by '|':
  MAP      map file
  ROUNDS   rounds per game
  FAST     1 for FAST mode, 0 otherwise
  BOT CMD  one per player
example: --scenario 'maps/island.txt|1200|0|java -cp bot.jar Bot|java -cp bot.jar Bot'
(--bot/--map/--rounds/--fast are usually easier)"""


def all_maps():
    return sorted(glob.glob(os.path.join(ROOT, "maps", "*.txt")) +
                  glob.glob(os.path.join(ROOT, "maps", "*", "*.txt")))


def run_engine(cmd, workdir, tag, timeout, cwd=None):
    out_path = os.path.join(workdir, tag + ".out")
    err_path = os.path.join(workdir, tag + ".err")
    with open(out_path, "wb") as out, open(err_path, "wb") as err:
        try:
            rc = subprocess.run(cmd, stdout=out, stderr=err, stdin=subprocess.DEVNULL,
                                timeout=timeout, cwd=cwd).returncode
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


def first_diff(a, b, name, left="py", right="cpp"):
    al, bl = a.splitlines(), b.splitlines()
    for i, (x, y) in enumerate(zip(al, bl)):
        if x != y:
            return "%s differs at line %d:\n  %-4s %s\n  %-4s %s" % (
                name, i + 1, left + ":", x[:400], right + ":", y[:400])
    if len(al) != len(bl):
        return "%s length differs: %s %d lines, %s %d lines" % (name, left, len(al), right, len(bl))
    return None


def py_command(sc, args, workdir, tag):
    cmd = [args.python, PYREF] + common_options(sc) + [
        "--transcript", os.path.join(workdir, tag + ".tr"),
        "--state-dump", os.path.join(workdir, tag + ".st")]
    if sc["fast"]:
        cmd.append("--fast")
    return cmd + [sc["map"]] + sc["bots"]


def common_options(sc):
    opts = ["--rounds", str(sc["rounds"])]
    if sc.get("hard"):
        opts += ["--hard-timeout", str(sc["hard"])]
    return opts


def check_determinism(sc, args, workdir):
    """Run the Python engine twice; return a problem description or None."""
    os.makedirs(workdir, exist_ok=True)
    for tag in ("py1", "py2"):
        run_engine(py_command(sc, args, workdir, tag), workdir, tag, args.timeout, args.bot_cwd)
    for ext, name in (("out", "stdout"), ("tr", "transcript"), ("st", "state dump")):
        d = first_diff(read(os.path.join(workdir, "py1." + ext)), read(os.path.join(workdir, "py2." + ext)),
                       name, "run1", "run2")
        if d:
            return d
    return None


def run_scenario(sc, args, workdir):
    os.makedirs(workdir, exist_ok=True)
    py_cmd = py_command(sc, args, workdir, "py")
    cpp_cmd = [args.cpp] + common_options(sc) + ["--transcript", os.path.join(workdir, "cpp.tr"),
                                                 "--state-dump", os.path.join(workdir, "cpp.st"), sc["map"]]
    if sc["fast"]:
        cpp_cmd.append("FAST")
    cpp_cmd += sc["bots"]
    with open(os.path.join(workdir, "scenario.txt"), "w") as f:
        f.write("cwd: %s\n" % (args.bot_cwd or os.getcwd()) +
                " ".join(shlex.quote(c) for c in py_cmd) + "\n" +
                " ".join(shlex.quote(c) for c in cpp_cmd) + "\n")
    rc_py = run_engine(py_cmd, workdir, "py", args.timeout, args.bot_cwd)
    rc_cpp = run_engine(cpp_cmd, workdir, "cpp", args.timeout, args.bot_cwd)
    problems, warnings = [], []
    if rc_py != rc_cpp:
        problems.append("exit code: py %r, cpp %r" % (rc_py, rc_cpp))
    for ext, name in (("out", "stdout"), ("tr", "transcript"), ("st", "state dump")):
        d = first_diff(read(os.path.join(workdir, "py." + ext)), read(os.path.join(workdir, "cpp." + ext)), name)
        if d:
            problems.append(d)
    if problems:
        problems.append("artifacts: " + workdir)
    a = clean_stderr(read(os.path.join(workdir, "py.err")))
    b = clean_stderr(read(os.path.join(workdir, "cpp.err")))
    if a != b:
        warnings.append("stderr differs:\n" + "\n".join(
            difflib.unified_diff(a, b, "py", "cpp", lineterm="", n=0)))
    return problems, warnings


def parse_scenario(s):
    parts = s.split("|")
    if len(parts) < 3 or not parts[1].strip().isdigit() or parts[2].strip() not in ("0", "1"):
        sys.exit("invalid --scenario %r\n%s" % (s, SCENARIO_HELP))
    return {"map": os.path.abspath(parts[0]), "rounds": int(parts[1]), "fast": parts[2].strip() == "1",
            "bots": parts[3:], "hard": None}


def map_starts(path):
    """Number of player start positions in a map (as engine.GameConfig counts them)."""
    with open(path) as f:
        return sum(1 for line in f.read().splitlines() for c in line if c not in "#! ")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cpp", required=True, help="the C++ engine binary")
    ap.add_argument("--python", required=True, help="CPython 3.10 interpreter for the reference")
    ap.add_argument("--bot", action="append", metavar="CMD",
                    help="bot command, one per player (repeatable); disables the random scenarios")
    ap.add_argument("--map", action="append", metavar="PATH",
                    help="map for --bot runs (repeatable, 'all' = every map; default maps/island.txt)")
    ap.add_argument("--rounds", type=int, help="rounds per game for --bot runs (default 1200, 500 with --fast)")
    ap.add_argument("--fast", action="store_true", help="FAST mode for --bot runs")
    ap.add_argument("--hard-timeout", type=float, help="hard turn timeout for --bot runs (both engines)")
    ap.add_argument("--bot-cwd", help="working directory for both engines and their bots (default: current)")
    ap.add_argument("--no-check-determinism", action="store_true",
                    help="skip running the Python engine twice to check that the --bot bots are deterministic")
    ap.add_argument("--bot-python", default=sys.executable, help="interpreter for the bundled test bots")
    ap.add_argument("--runs", type=int, help="random scenarios to play (default 20), or repetitions of each "
                    "--bot/--scenario scenario (default 1)")
    ap.add_argument("--seed", type=int, default=1, help="seed for the random scenarios")
    ap.add_argument("-j", "--jobs", type=int, help="scenarios run in parallel (default 2; 1 with --bot)")
    ap.add_argument("--timeout", type=float, default=3600, help="seconds before an engine run is killed")
    ap.add_argument("--workdir", help="where to keep artifacts (default: a new temporary directory)")
    ap.add_argument("--scenario", action="append", help="MAP|ROUNDS|FAST(0/1)|BOT CMD|BOT CMD... (repeatable)")
    args = ap.parse_args()
    args.cpp = os.path.abspath(args.cpp)
    if os.sep in args.python:
        args.python = os.path.abspath(args.python)
    workroot = os.path.abspath(args.workdir or tempfile.mkdtemp(prefix="lh-compare-"))
    custom = bool(args.bot or args.scenario)
    if args.jobs is None:
        args.jobs = 1 if custom else 2
    elif custom and args.jobs > 1:
        print("note: running %d scenarios in parallel; bots that write shared files (logs) may interfere "
              "with each other and make results differ" % args.jobs)

    if args.bot and args.scenario:
        sys.exit("use either --bot or --scenario, not both")
    if args.scenario:
        base = [parse_scenario(s) for s in args.scenario]
    elif args.bot:
        maps = []
        for m in args.map or [os.path.join(ROOT, "maps", "island.txt")]:
            maps += all_maps() if m == "all" else [os.path.abspath(m)]
        base = []
        for m in maps:
            if not os.path.exists(m):
                sys.exit("map not found: %s" % m)
            if map_starts(m) < len(args.bot):
                print("skipping %s: %d bots but only %d start positions" % (m, len(args.bot), map_starts(m)))
                continue
            base.append({"map": m, "rounds": args.rounds if args.rounds is not None else (500 if args.fast else 1200),
                         "fast": args.fast, "bots": args.bot, "hard": args.hard_timeout})
    if custom:
        scenarios = [sc for sc in base for _ in range(args.runs or 1)]
        if args.bot and not args.no_check_determinism and scenarios:
            print("checking that the bots are deterministic (Python engine twice on %s)..." %
                  os.path.relpath(scenarios[0]["map"]))
            sys.stdout.flush()
            d = check_determinism(scenarios[0], args, os.path.join(workroot, "determinism"))
            if d:
                print("The Python engine produced different results in two identical runs, so the bots are "
                      "not deterministic\n(unseeded randomness, timing, or state kept between runs such as "
                      "files they read back).\nEngine comparisons would be meaningless. First difference:")
                print("  " + d.replace("\n", "\n  "))
                print("artifacts: " + os.path.join(workroot, "determinism"))
                print("Make the bots deterministic (e.g. a fixed seed) or pass --no-check-determinism.")
                sys.exit(2)
            print("ok, deterministic")
    else:
        r = random.Random(args.seed)
        scenarios = [random_scenario(r, args.bot_python) for _ in range(args.runs or 20)]

    failed = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        futs = {ex.submit(run_scenario, sc, args, os.path.join(workroot, "run%04d" % i)): i
                for i, sc in enumerate(scenarios)}
        for fut in concurrent.futures.as_completed(futs):
            i = futs[fut]
            problems, warnings = fut.result()
            sc = scenarios[i]
            label = "run%04d %s rounds=%d%s players=%d" % (
                i, os.path.relpath(sc["map"]), sc["rounds"], " FAST" if sc["fast"] else "", len(sc["bots"]))
            if custom:
                label += "  [" + " | ".join(sc["bots"]) + "]"
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
