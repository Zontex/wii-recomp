#!/usr/bin/env python3
"""Plays every game you have for a while, unattended, and says what to fix first.

Usage: recomp/tools/compat_run.py <game> ... | all  [--seconds 90] [--out FOLDER] [--dry-run]
       recomp/tools/compat_run.py --report-from FOLDER ...

A game is a disc image, an extracted disc folder, a game ID added before, or a .wrd image;
"all" means every games/*.wrd. One game at a time, it:

  1. builds the game with add_game.py when it has no image yet (or with --rebuild, or
     when the console refuses the image as built for another version of the console),
  2. plays it for --seconds with a generic explorer, the same reading and pressing as
     autoplay's explore session: it clicks the button a screen names (OK, Yes, Next,
     Start, Continue...), presses A otherwise, and tries other buttons and each piece of
     text on a screen that doesn't change. No game has a script of its own here;
     --session picks an autoplay session instead, which may set game.goal,
  3. keeps the log, the gaps file, a picture every --picture-every seconds and what it
     read and pressed, in <out>/<game ID>/,
  4. gives the game a tier from what it measured, and writes report.md and report.json
     in <out>, ranking the gaps across every game against docs/hardware_inventory.md.

Tiers, lowest first. Each is reached only from signals in the log or the pictures:

  doesn't boot            no second of the log both drew and put a picture on screen
  boots to first picture  some second had draws and a copy to the screen
  reaches menus           the game answered input at least twice: two pictures just
                          before a press showed a still screen, and the look after it
                          showed a screen not seen before (pictures compared on a 16 by
                          12 grid) or text not read before. Asking for a still screen
                          first keeps movies, and warnings that change on a timer, out
  in game                 after reaching menus, --in-game-seconds seconds in a row (5)
                          averaging --in-game-draws draws a frame or more (1500), as
                          play draws far more than menus and movies do. In Mario Kart
                          Wii the title screen and opening movie stay under 70 a frame,
                          the menus with 3D drivers under 600, and a race is near 15000.
                          Or a --session that set game.goal

How a run ended is kept apart from the tier: ran the whole time, stopped (a "stop:"
line), crashed (killed by a signal), exited, or stalled (no activity for the last 10 s).
The frame rate is the median and the low (10th percentile) of the console's "fps" lines.

The ranking scores each gap row by the games that hit it. A game counts more the less far
it got (in game 1, reaches menus 2, boots 3, doesn't boot 4), as its gaps are more likely
what stops it, and a row counts more the further it is from done (missing 1, partial and
unknown 0.75, on demand and done 0.5, not needed 0.25). A gap first met in the 10 s
before a game stopped or stalled is marked as near the stop.

It's safe to leave running: each game starts from a fresh copy of --saves inside its
run folder, so real saves are never touched. By default that's a copy of the console's
own saves (recomp/build/saves), as a console that has never run keeps no system
settings, and without them the game doesn't pair the simulated remote; "--saves empty"
starts from nothing, as a new Wii does. Title data (the speed
profile add_game.py builds from) is shared unless --fresh-title-data; remotes are
simulated unless --env says otherwise; and only processes it started are ever stopped,
by their process ID, at a time limit or when it's interrupted. --dry-run lists what it
would do and starts nothing. Pictures and logs are the game's, so --out must be outside
the repository (~/wii-recomp-runs/compat-<date> by default).

Text recognition needs macOS. Elsewhere screens are still compared by picture, so the
tiers work the same, with fewer steps seen where only the text changed. Where Vulkan
doesn't start, the game is played again with OpenGL, and so are the games after it.
"""
import argparse
import datetime
import importlib.util
import json
import os
import platform
import re
import shutil
import signal
import statistics
import subprocess
import sys
import threading
import time
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
GAMES = ROOT / "games"
CONSOLE_BUILD = ROOT / "recomp/build"
INVENTORY = ROOT / "docs/hardware_inventory.md"
sys.path[:0] = [str(HERE), str(HERE / "autoplay")]
import gap_report  # noqa: E402

TIERS = ["doesn't boot", "boots to first picture", "reaches menus", "in game"]
GAME_WEIGHT = {3: 1, 2: 2, 1: 3, 0: 4}  # By tier: the less far a game got, the more its gaps count.
STATUS_WEIGHT = {"missing": 1.0, "partial": 0.75, "unknown": 0.75, "on demand": 0.5, "done": 0.5, "not needed": 0.25}
MENU_STEPS = 2  # Presses followed by a new screen, for "reaches menus".
NEW_SCREEN = 12.0  # Mean difference over the grid, out of 255, that makes a screen new.
FLAT = 3.0  # A picture whose grid varies less than this is one flat colour.
NEAR_STOP = 10  # Seconds before a stop or stall that a gap counts as near it.
STALL = 10  # Seconds without an activity line at the end of a run that count as a stall.
BOOT_WAIT = 60  # Seconds to wait for the first activity line before giving up on input.
GRID = (16, 12)
HOLD = 0.2  # Seconds a button is held for one press.
HANG_UPS = 20  # Times the game drops the remote before the report says input may not have reached it.

ACTIVITY = re.compile(r"^second (\d+): (\d+) draws, (\d+) copies, (\d+) to screen, (\d+) draw done, (\d+) new shaders, "
                      r"slowest ([\d.]+) ms, worst frame ([\d.]+) ms", re.M)
LINE = re.compile(r"^(second (\d+):|fps ([\d.]+)|gap: (\S+) (0x[0-9a-f]+)|stop: .*)", re.M)
GAME_ID = re.compile(r"^[A-Z0-9]{6}$")
BUILT_AS = re.compile(r"^([A-Z0-9]{6}) \(title [0-9a-f-]+\) from ", re.M)
REBUILD_STOPS = ("was built for another version of the console", "has no native code for")
NO_VULKAN = "no Vulkan here"
DISC_IMAGES = {".iso", ".rvz", ".wbfs", ".ciso", ".gcz", ".wia"}


# Processes. Only what this tool started is ever signalled, and only by its own ID.

class Processes:
    def __init__(self):
        self.started = {}  # pid -> Popen
        self.lock = threading.RLock()  # Held while stopping, so two threads never signal one ID.

    def start(self, command, **options):
        with self.lock:
            process = subprocess.Popen(command, **options)
            self.started[process.pid] = process
        return process

    def owns(self, process):
        with self.lock:
            return self.started.get(getattr(process, "pid", None)) is process

    def stop(self, process, grace=3.0):
        """Ends one process this started: asked first, then killed. Anything else is refused.
        Returns its exit status. A process that already ended is only reaped."""
        with self.lock:
            if not self.owns(process):
                if process.__class__ is subprocess.Popen and process.returncode is not None:
                    return process.returncode  # Stopped here before.
                raise ValueError(f"process {getattr(process, 'pid', process)} wasn't started here, so it's left alone")
            if process.poll() is None:  # Not reaped yet, so the ID is still this process's.
                os.kill(process.pid, signal.SIGTERM)
                try:
                    process.wait(grace)
                except subprocess.TimeoutExpired:
                    os.kill(process.pid, signal.SIGKILL)
                    process.wait()
            del self.started[process.pid]
            return process.returncode

    def stop_all(self):
        with self.lock:
            for process in list(self.started.values()):
                self.stop(process)


# Pictures: the console saves PPM, kept as PNG, and compared on a small grid.

def read_ppm(path):
    data = Path(path).read_bytes()
    parts, at = [], 0
    while len(parts) < 4:  # P6, width, height, largest value, then one whitespace byte.
        while data[at:at + 1].isspace():
            at += 1
        end = at
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[at:end])
        at = end
    if parts[0] != b"P6" or parts[3] != b"255":
        raise ValueError(f"{path} isn't an 8-bit P6 picture")
    width, height = int(parts[1]), int(parts[2])
    return width, height, data[at + 1:at + 1 + width * height * 3]


def write_png(path, width, height, pixels):
    def chunk(kind, body):
        return len(body).to_bytes(4, "big") + kind + body + zlib.crc32(kind + body).to_bytes(4, "big")
    stride = width * 3
    rows = b"".join(b"\0" + pixels[y * stride:(y + 1) * stride] for y in range(height))
    header = width.to_bytes(4, "big") + height.to_bytes(4, "big") + bytes([8, 2, 0, 0, 0])
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 6))
                           + chunk(b"IEND", b""))


def signature(width, height, pixels, grid=GRID, samples=6):
    """The picture's brightness on a grid of cells, each the mean of a few points in it."""
    columns, rows = grid
    cells = []
    for row in range(rows):
        for column in range(columns):
            total = 0
            for sy in range(samples):
                y = (row * samples + sy) * height // (rows * samples)
                for sx in range(samples):
                    x = (column * samples + sx) * width // (columns * samples)
                    at = (y * width + x) * 3
                    total += pixels[at] * 3 + pixels[at + 1] * 6 + pixels[at + 2]
            cells.append(round(total / (10 * samples * samples)))
    return cells


def difference(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def is_flat(cells):
    return statistics.pstdev(cells) < FLAT


# Reading a run's log.

def parse_log(text):
    """Seconds of activity, fps lines, gaps in the order first met, and stop lines."""
    seconds = [{"second": int(m[1]), "draws": int(m[2]), "copies": int(m[3]), "to_screen": int(m[4]),
                "new_shaders": int(m[6]), "worst_frame_ms": float(m[8])} for m in ACTIVITY.finditer(text)]
    fps, gaps, stops, current = [], [], [], 0
    for match in LINE.finditer(text):
        if match[2]:
            current = int(match[2])
        elif match[3]:
            fps.append(float(match[3]))
        elif match[4]:
            gaps.append({"id": match[4], "detail": match[5], "second": current})
        else:
            stops.append(match[0])
    return {"seconds": seconds, "fps": fps, "gaps_met": gaps, "stops": stops}


def percentile(values, share):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(share * len(ordered)))] if ordered else None


def frame_rate(fps):
    """Median and low of the fps lines, leaving out the first few seconds of start-up."""
    steady = fps[3:] if len(fps) > 6 else fps
    if not steady:
        return {"median": None, "low": None, "seconds": 0}
    return {"median": round(statistics.median(steady), 1), "low": round(percentile(steady, 0.1), 1), "seconds": len(steady)}


def ending(returncode, stops, seconds, killed_at_limit, silent_for):
    """How the run ended, apart from how far it got. silent_for is how long, in seconds of
    wall time, the log had gone without an activity line when the run was stopped."""
    if stops:
        return "stopped", stops[0]
    if returncode is not None and returncode < 0 and not killed_at_limit:
        try:
            name = signal.Signals(-returncode).name
        except ValueError:
            name = f"signal {-returncode}"
        return "crashed", f"killed by {name}"
    if returncode not in (None, 0) and not killed_at_limit:
        return "exited", f"exited with status {returncode}"
    if killed_at_limit and silent_for is not None and silent_for >= STALL:
        last = seconds[-1]["second"] if seconds else 0
        return "stalled", f"no activity after second {last}, for the last {silent_for:.0f} s"
    return "ran the whole time", ""


def text_key(text):
    """Text as compared between screens: its words of three letters or more, in lower case."""
    return " ".join(re.findall(r"[A-Za-z]{3,}", text)).lower()


def input_steps(observations):
    """The looks where input moved the game on: the screen was still just before a button
    was pressed (so it was waiting, not playing a movie or changing on a timer), and the
    look after showed a screen or text not seen before."""
    return [seen for seen in observations
            if seen.get("after_input") and seen.get("still_before") and (seen.get("new_screen") or seen.get("new_text"))]


def classify(seconds, observations, goal=None, in_game_draws=1500, in_game_seconds=5):
    """The tier from measured signals only, with the reasons for it. See the module's notes."""
    pictured = [s for s in seconds if s["draws"] and s["to_screen"]]
    if not pictured:
        return 0, ["no second both drew and put a picture on screen"]
    reasons = [f"first picture in second {pictured[0]['second']}"]
    tier = 1
    steps = input_steps(observations)
    texts = {text_key(o.get("text", "")) for o in observations} - {""}
    menu_at = steps[MENU_STEPS - 1].get("log_second", 0) if len(steps) >= MENU_STEPS else None
    if menu_at is not None:
        tier = 2
        reasons.append(f"answered input by second {menu_at}: {len(steps)} presses on a still screen led to a new one")
    else:
        reasons.append(f"{len(steps)} press(es) on a still screen led to a new one, short of the {MENU_STEPS} for menus")
    if texts:
        reasons.append(f"text read on {len(texts)} different screens")

    if goal:
        return 3, reasons + [f"the session reached its goal: {goal}"]
    heavy = [s for s in seconds if s["to_screen"] and s["draws"] / s["to_screen"] >= in_game_draws]
    most = max((s["draws"] / s["to_screen"] for s in seconds if s["to_screen"]), default=0)
    if menu_at is not None:
        run, best = 0, 0
        for second in seconds:
            if second["second"] > menu_at:
                run = run + 1 if second in heavy else 0
                best = max(best, run)
        if best >= in_game_seconds:
            tier = 3
            reasons.append(f"{best} seconds in a row of {in_game_draws}+ draws a frame after the menus")
        else:
            reasons.append(f"at most {best} seconds in a row of {in_game_draws}+ draws a frame after the menus")
    reasons.append(f"most draws a frame in any second: {most:.0f}")
    return tier, reasons


def near_stop(gaps_met, end_kind, seconds):
    """Gap IDs first met within NEAR_STOP seconds of where a stopped or stalled run ended."""
    if end_kind not in ("stopped", "crashed", "stalled", "exited") or not seconds:
        return set()
    last = seconds[-1]["second"]
    return {gap["id"] for gap in gaps_met if gap["second"] >= last - NEAR_STOP}


# The report.

def rank(games, details):
    """Every gap ID hit, scored across games: see the module's notes. Highest first."""
    rows = {ident: (row["item"], row["status"]) for ident, row in details.items()}
    hits = gap_report.collect([(game["id"], [tuple(g) for g in game["gaps"]]) for game in games])
    tiers = {game["id"]: game["tier"] for game in games}
    near = {}
    for game in games:
        for ident in game.get("near_stop", []):
            near.setdefault(ident, set()).add(game["id"])
    ranked = []
    for ident, entry in hits.items():
        row = details.get(ident)
        status = row["status"] if row else "no row"
        weight = STATUS_WEIGHT.get(status, 1.0)
        score = sum(GAME_WEIGHT[tiers[name]] for name in entry["runs"]) * weight + 2 * len(near.get(ident, ()))
        ranked.append({"id": ident, "score": round(score, 2), "status": status, "games": sorted(entry["runs"]),
                       "hits": entry["count"], "details": sorted(entry["details"]), "near_stop": sorted(near.get(ident, ())),
                       "item": row["item"] if row else "", "section": row["section"] if row else "",
                       "line": row["line"] if row else None})
    ranked.sort(key=lambda r: (-r["score"],) + gap_report.rank_key(r["id"], hits, rows))
    return ranked


def link(entry, inventory=INVENTORY):
    if not entry.get("line"):
        return f"`{entry['id']}`"
    return f"[`{entry['id']}`]({inventory.as_uri()}#L{entry['line']})"


def fps_text(rate):
    return f"{rate['median']} (low {rate['low']})" if rate and rate.get("median") is not None else "none"


def markdown(report, inventory=INVENTORY):
    lines = [f"# Compatibility run, {report['when']}", "",
             f"{len(report['games'])} game(s), {report['settings']['seconds']} s each on {report['settings']['machine']}. "
             "Tiers and scores are explained in recomp/tools/compat_run.py.", "",
             "| Game | Tier | Ended | fps | Steps by input | Top gaps |", "|---|---|---|---|---|---|"]
    for game in report["games"]:
        top = ", ".join(f"`{g['id']}`" for g in game["top_gaps"][:3]) or "none"
        ended = game["ended"] + (f": {game['ended_detail']}" if game["ended_detail"] else "")
        lines.append(f"| {game['id']} | {TIERS[game['tier']]} | {ended} | {fps_text(game['fps'])} | {game['input_steps']} | {top} |")
    for game in report["games"]:
        lines += ["", f"## {game['id']}: {TIERS[game['tier']]}", ""]
        lines += [f"- {reason}" for reason in game["reasons"]]
        if game.get("last_text"):
            lines.append(f"- last text read: {game['last_text'][:200]}")
        lines.append(f"- {game['pictures']} pictures, log and gaps in {game['folder']}")
        if game.get("note"):
            lines.append(f"- {game['note']}")
        if game["top_gaps"]:
            lines += ["", "| Gap | Status | Hits | Near the stop |", "|---|---|---|---|"]
            for gap in game["top_gaps"]:
                lines.append(f"| {link(gap, inventory)} | {gap['status']} | {gap['hits']} | {'yes' if gap['near_stop'] else ''} |")
    groups = [("What to fix first", lambda r: r["status"] not in ("done", "not needed", "no row")),
              ("Marked done or not needed, but hit", lambda r: r["status"] in ("done", "not needed")),
              ("No inventory row", lambda r: r["status"] == "no row")]
    for title, wanted in groups:
        chosen = [r for r in report["ranking"] if wanted(r)]
        if not chosen:
            continue
        lines += ["", f"## {title}", "", "| Score | Gap | Status | Games | Hits | Near a stop | Section | Item |",
                  "|---|---|---|---|---|---|---|---|"]
        for r in chosen:
            lines.append(f"| {r['score']} | {link(r, inventory)} | {r['status']} | {', '.join(r['games'])} | {r['hits']} | "
                         f"{', '.join(r['near_stop'])} | {r['section']} | {r['item']} |")
    return "\n".join(lines) + "\n"


def build_report(games, settings, inventory=INVENTORY):
    details = gap_report.inventory_details(inventory)
    ranking = rank(games, details)
    by_id = {r["id"]: r for r in ranking}
    for game in games:
        counts = {}
        for ident, _, count in game["gaps"]:
            counts[ident] = counts.get(ident, 0) + count
        entries = [dict(by_id[ident], hits=count, near_stop=ident in game.get("near_stop", [])) for ident, count in counts.items()]
        entries.sort(key=lambda e: (-e["near_stop"], -STATUS_WEIGHT.get(e["status"], 1.0), -e["hits"]))
        game["top_gaps"] = entries[:8]
    return {"when": settings["when"], "settings": settings, "games": games, "ranking": ranking}


def write_report(report, out, inventory=INVENTORY):
    out = Path(out)
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    (out / "report.md").write_text(markdown(report, inventory))
    return out / "report.md"


# Planning: what each named game means.

def read_disc_id(path):
    """The game ID at the start of a plain disc image or a WBFS file, or None for others."""
    offsets = {".iso": 0, ".wbfs": 0x200}
    at = offsets.get(path.suffix.lower())
    if at is None:
        return None
    with open(path, "rb") as image:
        image.seek(at)
        code = image.read(6).decode("ascii", "replace")
    return code if GAME_ID.match(code) else None


def plan(names, games_folder=None, recomp=None, rebuild=False):
    """One entry per game: its ID (if known yet), the image to play, and what to build it from."""
    games_folder, recomp = Path(games_folder or GAMES), Path(recomp or ROOT / "recomp")
    entries, seen = [], set()
    for name in names:
        if name == "all":
            images = sorted(games_folder.glob("*.wrd"))
            if not images:
                raise SystemExit(f"compat_run: there are no images in {games_folder}; add games with add_game.py")
            found = [{"id": p.stem, "image": p, "build_from": p.stem if (recomp / f"build-{p.stem}/settings.json").is_file() else None}
                     for p in images]
        else:
            path = Path(name).expanduser()
            if path.suffix == ".wrd" and path.is_file():
                found = [{"id": path.stem, "image": path.resolve(), "build_from": None}]
            elif GAME_ID.match(name) and not path.exists():
                built = (recomp / f"build-{name}/settings.json").is_file()
                if not built and not (games_folder / f"{name}.wrd").is_file():
                    raise SystemExit(f"compat_run: there's no game {name}: no {games_folder / (name + '.wrd')} and no build")
                found = [{"id": name, "image": games_folder / f"{name}.wrd", "build_from": name if built else None}]
            elif path.is_dir():
                boot = path / "sys/boot.bin"
                if not boot.is_file():
                    raise SystemExit(f"compat_run: {path} isn't an extracted disc: it has no sys/boot.bin")
                code = boot.read_bytes()[:6].decode("ascii", "replace")
                found = [{"id": code, "image": games_folder / f"{code}.wrd", "build_from": str(path.resolve())}]
            elif path.is_file() and path.suffix.lower() in DISC_IMAGES:
                code = read_disc_id(path)
                found = [{"id": code, "image": games_folder / f"{code}.wrd" if code else None, "build_from": str(path.resolve())}]
            else:
                raise SystemExit(f"compat_run: {name} is neither a disc image, an extracted disc, a .wrd nor a game ID")
        for entry in found:
            key = entry["id"] or entry["build_from"]
            if key in seen:
                continue
            seen.add(key)
            image = entry["image"]
            entry["build"] = bool(entry["build_from"]) and (rebuild or image is None or not Path(image).is_file())
            if not entry["build"] and (image is None or not Path(image).is_file()):
                raise SystemExit(f"compat_run: {key} has no image and nothing to build it from")
            entries.append(entry)
    return entries


# Safety checks on folders.

def inside(path, folder):
    path, folder = Path(path).resolve(), Path(folder).resolve()
    return path == folder or folder in path.parents


def check_out_folder(out, root=None):
    if inside(out, root or ROOT):
        raise SystemExit(f"compat_run: {out} is inside the repository; pictures and logs are the game's, so keep them outside")


def fresh_saves(starting, run_folder, console=CONSOLE_BUILD):
    """A copy of the starting saves (or an empty folder) in the run folder, for this run only."""
    target = Path(run_folder) / "saves"
    real = Path(console) / "saves"
    if inside(target, real) or (starting and inside(target, starting)):
        raise SystemExit(f"compat_run: {target} would be inside the saves it copies from")
    if starting and inside(starting, target):
        raise SystemExit(f"compat_run: the starting saves {starting} are inside the run folder")
    if target.exists():
        shutil.rmtree(target)
    if starting:
        shutil.copytree(starting, target, symlinks=True)
    else:
        target.mkdir(parents=True)
    return target


def starting_saves(chosen, console=CONSOLE_BUILD):
    """The folder each game's saves are copied from: the one named, none for "empty", or
    the console's own when it has one."""
    if chosen == "empty":
        return None
    if chosen:
        return Path(chosen).expanduser().resolve()
    own = Path(console) / "saves"
    return own if own.is_dir() else None


def environment(run_folder, saves, options_env=(), fresh_title_data=False, base=None):
    """What the console runs with: everything it writes per run goes to the run folder."""
    run_folder = Path(run_folder)
    env = dict(os.environ if base is None else base)
    env.update(WII_RECOMP_SAVES=str(saves), WII_RECOMP_GAPS=str(run_folder / "gaps.txt"),
               WII_RECOMP_REMOTE_SLOTS=str(run_folder / "remotes.txt"), WII_RECOMP_CONTROL=str(run_folder / "control"),
               WII_RECOMP_ACTIVITY="1", WII_RECOMP_FPS_LOG="1")
    env.setdefault("WII_RECOMP_REMOTES", "simulated")  # Leave the user's real remotes alone.
    moltenvk = "/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json"
    if sys.platform == "darwin" and os.path.exists(moltenvk):
        env.setdefault("VK_DRIVER_FILES", moltenvk)
    if fresh_title_data:
        env["WII_RECOMP_TITLE_DATA"] = str(run_folder / "title-data")
    for pair in options_env:
        key, _, value = pair.partition("=")
        env[key] = value
    if Path(env["WII_RECOMP_SAVES"]).resolve() == (CONSOLE_BUILD / "saves").resolve():
        raise SystemExit("compat_run: a run may not use the console's own saves")
    return env


# Playing one game.

class Run:
    """The parts of autoplay's Game a session uses, for one run that measures rather than
    fails: a step that can't be done is noted and the run goes on."""

    def __init__(self, processes, program, image, folder, env, storage, picture_every):
        self.processes = processes
        self.folder = Path(folder)
        self.pipe = env["WII_RECOMP_CONTROL"]
        self.log_path = self.folder / "game.log"
        self.picture_every = picture_every
        self.shots, self.kept_at, self.pictures = 0, -1e9, 0
        self.observations, self.screens, self.pressed, self.goal = [], [], False, None
        self.previous, self.texts = None, set()
        self.still = self.still_at_press = None
        self.can_read = sys.platform == "darwin" and (shutil.which("swiftc") or (CONSOLE_BUILD / "tools/screen_text").exists())
        self.log = open(self.log_path, "w")
        self.started = time.time()
        command = [str(program), str(image)] + ([str(storage)] if storage else [])
        self.process = processes.start(command, stdout=self.log, stderr=subprocess.STDOUT, env=env, cwd=ROOT)
        self.last_second, self.last_change = 0, self.started
        self.watching = True
        threading.Thread(target=self.watch, daemon=True).start()

    def watch(self):
        """Notes when the log's activity second last moved on, to tell a stall from a run."""
        while self.watching:
            second = self.log_second()
            if second != self.last_second:
                self.last_second, self.last_change = second, time.time()
            time.sleep(0.5)

    def silent_for(self):
        return time.time() - self.last_change

    def seconds(self):
        return time.time() - self.started

    def text(self):
        return self.log_path.read_text(errors="replace")

    def alive(self):
        return self.process.poll() is None and not re.search(r"^stop:", self.text(), re.M)

    def log_second(self):
        found = re.findall(r"^second (\d+):", self.text(), re.M)
        return int(found[-1]) if found else 0

    def note(self, message):
        print(f"{self.seconds():6.1f}  {message}", flush=True)

    def fail(self, message):
        raise RuntimeError(message)

    def send(self, command):
        if not self.alive() or not os.path.exists(self.pipe):
            return
        try:  # Without waiting: a pipe whose game has just gone has no reader, and would block.
            descriptor = os.open(self.pipe, os.O_WRONLY | os.O_NONBLOCK)
        except OSError:
            return
        try:
            os.write(descriptor, (command + "\n").encode())
        except OSError:
            pass
        finally:
            os.close(descriptor)

    def check_running(self):
        if not self.alive():
            raise RuntimeError("the game stopped")

    def wait_for(self, pattern, seconds, what):
        """As autoplay's: waits for a log line matching pattern."""
        deadline = time.time() + seconds
        while time.time() < deadline:
            found = re.search(pattern, self.text(), re.M)
            if found:
                return found
            self.check_running()
            time.sleep(0.25)
        raise RuntimeError(f"no sign of {what} after {seconds} s")

    def press(self, button, then=0.6):
        """Held for a fifth of a second, as a person presses: some games miss the console's
        own 100 ms press when it falls between their reads."""
        self.send(f"hold {button}")
        time.sleep(HOLD)
        self.send(f"release {button}")
        if not self.pressed:
            self.still_at_press = self.still
        self.pressed = True
        time.sleep(then)

    def point(self, x, y, then=0.3):
        self.send(f"pointer {x} {y}")
        time.sleep(then)

    def click(self, x, y, then=0.8):
        self.point(x, y)
        self.press("a", then)

    def board(self, standing, kilograms=60, lean_x=0, lean_y=0):
        self.send(f"board on {kilograms} {lean_x} {lean_y}" if standing else "board off")

    def wait(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline and self.alive():
            time.sleep(0.25)

    def wait_for_drawing(self, seconds=BOOT_WAIT):
        deadline = time.time() + seconds
        while time.time() < deadline and self.alive():
            if re.search(r"^second \d+:", self.text(), re.M):
                return True
            time.sleep(0.25)
        return False

    def capture(self, name):
        """The next picture, as (width, height, pixels, grid), or None if it never came."""
        self.shots += 1
        ppm = self.folder / f"{self.shots:03}-{name}.ppm"
        self.send(f"shot {ppm}")
        deadline = time.time() + 10
        while not ppm.exists():
            if time.time() > deadline or not self.alive():
                return None
            time.sleep(0.1)
        width, height, pixels = read_ppm(ppm)
        ppm.unlink()
        return width, height, pixels, signature(width, height, pixels)

    def glance(self):
        """Looks again without keeping anything, to tell whether the screen is still: a
        screen that changes with nobody pressing is a movie or plays by itself."""
        picture = self.capture("g")
        if picture and self.previous is not None:
            self.still = difference(picture[3], self.previous) <= NEW_SCREEN
        return self.still

    def shot(self, name="s"):
        """Saves the next picture and records what it shows. None if it never came."""
        picture = self.capture(name)
        if picture is None:
            return None
        width, height, pixels, cells = picture
        ppm = self.folder / f"{self.shots:03}-{name}.ppm"
        new = all(difference(cells, other) > NEW_SCREEN for other in self.screens)
        self.previous = cells
        if new:
            self.screens.append(cells)
        seen = {"t": round(self.seconds(), 1), "log_second": self.log_second(), "new_screen": new, "flat": is_flat(cells),
                "after_input": self.pressed, "still_before": self.still_at_press, "text": ""}
        self.pressed, self.still, self.still_at_press = False, None, None
        self.observations.append(seen)
        if self.seconds() - self.kept_at >= self.picture_every or new:
            self.kept_at = self.seconds()
            png = ppm.with_suffix(".png")
            write_png(png, width, height, pixels)
            seen["picture"] = png.name
            self.pictures += 1
        self.last_picture = (width, height, pixels)
        return seen

    def read_screen(self, name="s"):
        """The text on screen now, as autoplay's Game gives it; [] where it can't be read."""
        seen = self.shot(name)
        if seen is None or not self.can_read:
            return []
        import autoplay
        picture = self.folder / "reading.png"
        write_png(picture, *self.last_picture)
        try:
            out = subprocess.run([autoplay.screen_reader(), str(picture)], capture_output=True, text=True, timeout=30).stdout
            found = json.loads(out or "[]")
        except (subprocess.SubprocessError, json.JSONDecodeError, OSError, autoplay.StepFailed) as failure:
            self.note(f"can't read the screen: {failure}")
            self.can_read = False
            found = []
        finally:
            picture.unlink(missing_ok=True)
        for item in found:
            item["text"] = item["text"].translate(autoplay.LOOKALIKES).strip()
        seen["text"] = " | ".join(item["text"] for item in found)
        words = text_key(seen["text"])
        seen["new_text"] = bool(words) and words not in self.texts
        self.texts.add(words)
        self.note("reads: " + seen["text"])
        return found


EXTRA_BUTTONS = ["plus", "two", "one", "a"]  # Tried on a screen with no text that doesn't change.


def explore(run, until):
    """The generic explorer: autoplay's Player, which clicks the button a screen names or
    presses A, plus other buttons on a screen with no text that isn't changing."""
    from player import Player
    player = Player(run)
    tried = 0
    while run.seconds() < until and run.alive():
        text, screen = player.look()
        run.glance()
        if not screen and player.same >= 3:
            run.press(EXTRA_BUTTONS[tried % len(EXTRA_BUTTONS)])
            tried += 1
        else:
            player.react(text, screen)
        time.sleep(1.0)


def run_session(run, session, game_id):
    import autoplay
    path = autoplay.find_session(session, game_id)
    sys.path.insert(0, os.path.dirname(path))
    spec = importlib.util.spec_from_file_location("session", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.run(run)


def play(processes, program, entry, folder, options):
    """Plays one game and returns what was measured."""
    folder.mkdir(parents=True, exist_ok=True)
    for old in folder.glob("*.png"):  # From an earlier try of the same game.
        old.unlink()
    saves = fresh_saves(starting_saves(options.saves), folder)
    env = environment(folder, saves, options.env, options.fresh_title_data)
    run = Run(processes, program, entry["image"], folder, env, options.storage, options.picture_every)
    limit = options.seconds
    hard_stop = []

    def at_hard_limit():  # A session that hangs still ends: the game is stopped here.
        hard_stop.append(run.silent_for())
        run.watching = False
        if processes.owns(run.process):
            processes.stop(run.process)
    timer = threading.Timer(limit + 30, at_hard_limit)
    timer.start()
    note = ""
    try:
        if run.wait_for_drawing():
            if options.session:
                run_session(run, options.session, entry["id"])
            else:
                explore(run, limit)
            run.wait(limit - run.seconds())
    except Exception as failure:  # A session's own failure is a finding, not the end of the whole run.
        note = f"the session stopped early: {failure}"
        run.note(note)
    finally:
        timer.cancel()
        killed = run.process.poll() is None or bool(hard_stop)
        silent = hard_stop[0] if hard_stop else run.silent_for()
        run.watching = False
        returncode = processes.stop(run.process) if processes.owns(run.process) else run.process.wait()
        run.log.close()
    text = run.text()
    parsed = parse_log(text)
    hang_ups = len(re.findall(r"^bluetooth: the game hung up on device 0\b", text, re.M))
    if hang_ups >= HANG_UPS:
        note = (note + "; " if note else "") + (f"the game hung up on the remote {hang_ups} times, so presses may not "
                                                "have reached it (a console with no saved settings has no paired remote)")
    kind, detail = ending(returncode, parsed["stops"], parsed["seconds"], killed, silent)
    tier, reasons = classify(parsed["seconds"], run.observations, run.goal, options.in_game_draws, options.in_game_seconds)
    gaps = gap_report.read_gaps(folder / "gaps.txt") if (folder / "gaps.txt").is_file() else []
    texts = [o["text"] for o in run.observations if o["text"]]
    result = {"id": entry["id"], "image": str(entry["image"]), "folder": str(folder), "seconds": limit,
              "tier": tier, "tier_name": TIERS[tier],
              "reasons": reasons, "ended": kind, "ended_detail": detail, "returncode": returncode,
              "fps": frame_rate(parsed["fps"]), "seconds_logged": len(parsed["seconds"]),
              "most_draws": max((s["draws"] for s in parsed["seconds"]), default=0),
              "input_steps": len(input_steps(run.observations)),
              "screens": len(run.screens), "pictures": run.pictures, "last_text": texts[-1] if texts else "",
              "text_read": run.can_read, "gaps": [list(g) for g in gaps],
              "near_stop": sorted(near_stop(parsed["gaps_met"], kind, parsed["seconds"])),
              "observations": run.observations, "draws_by_second": [s["draws"] for s in parsed["seconds"]], "note": note}
    (folder / "run.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def build(processes, entry, folder, minutes):
    """Runs add_game.py for one game, stopping it at the time limit. Returns the game ID or None."""
    folder.mkdir(parents=True, exist_ok=True)
    log_path = folder / "build.log"
    with open(log_path, "w") as log:
        process = processes.start([sys.executable, str(HERE / "add_game.py"), entry["build_from"]], stdout=log,
                                  stderr=subprocess.STDOUT, cwd=ROOT)
        try:
            process.wait(minutes * 60)
        except subprocess.TimeoutExpired:
            processes.stop(process)
            print(f"  the build took longer than {minutes} minutes and was stopped; see {log_path}")
            return None
        processes.stop(process)
    found = BUILT_AS.search(log_path.read_text(errors="replace"))
    if process.returncode or not found:
        print(f"  the build failed; see {log_path}")
        return None
    return found[1]


def describe(entry, options, program, out):
    folder = out / (entry["id"] or "new")
    lines = []
    if entry["build"]:
        lines.append(f"build: {sys.executable} {HERE / 'add_game.py'} {entry['build_from']}  (log in {folder / 'build.log'})")
    lines.append(f"play:  {program} {entry['image'] or GAMES / '<game ID>.wrd'}" + (f" {options.storage}" if options.storage else "")
                 + f"  for {options.seconds} s, " + (f"session {options.session}" if options.session else "generic explorer"))
    lines.append(f"       saves: a fresh copy of {starting_saves(options.saves) or 'nothing (empty)'} in {folder / 'saves'}")
    lines.append(f"       log, gaps and pictures every {options.picture_every} s in {folder}")
    return lines


def main():
    parser = argparse.ArgumentParser(usage=__doc__.split("\n\n")[1].removeprefix("Usage: "), description=__doc__.split("\n")[0],
                                     epilog="See the notes at the top of recomp/tools/compat_run.py for the tiers and scores.")
    parser.add_argument("games", nargs="*", help="disc images, extracted discs, game IDs, .wrd images, or all")
    parser.add_argument("--seconds", type=float, default=90, help="how long to play each game (90)")
    parser.add_argument("--picture-every", type=float, default=10, help="keep a picture this often, and at every new screen (10)")
    parser.add_argument("--out", help="where the runs and report go; outside the repository")
    parser.add_argument("--saves", help="a saves folder each game starts from a copy of, or empty (recomp/build/saves)")
    parser.add_argument("--storage", default=os.environ.get("WII_RECOMP_NAND"), help="a Wii storage folder, read only")
    parser.add_argument("--session", help="an autoplay session to play instead of the generic explorer")
    parser.add_argument("--env", action="append", default=[], help="KEY=VALUE for the console, any number of times")
    parser.add_argument("--rebuild", action="store_true", help="build every game first, even those with an image")
    parser.add_argument("--no-build", action="store_true", help="never build: play only games with an image")
    parser.add_argument("--build-minutes", type=float, default=30, help="time limit for one game's build (30)")
    parser.add_argument("--fresh-title-data", action="store_true", help="don't add these runs to the shared speed profiles")
    parser.add_argument("--in-game-draws", type=int, default=1500, help="draws a frame that count as 3D play (1500)")
    parser.add_argument("--in-game-seconds", type=int, default=5, help="seconds in a row of them for \"in game\" (5)")
    parser.add_argument("--report-from", nargs="+", metavar="FOLDER", help="only write a report from earlier runs' folders")
    parser.add_argument("--dry-run", action="store_true", help="list what would be done, and do nothing")
    options = parser.parse_args()

    when = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    out = Path(options.out or Path.home() / "wii-recomp-runs" / f"compat-{datetime.datetime.now():%Y%m%d-%H%M%S}").expanduser().resolve()
    check_out_folder(out)
    settings = {"when": when, "seconds": options.seconds, "machine": f"{platform.system()} {platform.machine()}",
                "session": options.session or "generic explorer", "in_game_draws": options.in_game_draws,
                "in_game_seconds": options.in_game_seconds}

    if options.report_from:
        games = []
        for folder in options.report_from:
            games += [json.loads(p.read_text()) for p in sorted(Path(folder).expanduser().glob("*/run.json"))]
        if not games:
            raise SystemExit("compat_run: no run.json files in those folders")
        settings["seconds"] = ", ".join(sorted({f"{g.get('seconds', '?')}" for g in games}))
        out.mkdir(parents=True, exist_ok=True)
        print(f"report: {write_report(build_report(games, settings), out)}")
        return
    if not options.games:
        parser.error("name at least one game, or all")
    entries = plan(options.games, rebuild=options.rebuild)
    if options.no_build:
        entries = [dict(e, build=False) for e in entries if Path(e["image"] or "").is_file()]
    program = CONSOLE_BUILD / "wii-recomp"
    if options.saves not in (None, "empty") and not Path(options.saves).is_dir():
        raise SystemExit(f"compat_run: there's no saves folder {options.saves}")
    if options.saves is None and starting_saves(None) is None:
        print(f"note: there are no saves at {CONSOLE_BUILD / 'saves'}, so games start as on a new Wii, with no paired remote")
    if options.dry_run:
        print(f"would play {len(entries)} game(s), one at a time, into {out}:")
        for entry in entries:
            print(f"\n{entry['id'] or entry['build_from']}")
            for line in describe(entry, options, program, out):
                print(f"  {line}")
        print(f"\nthen write {out / 'report.md'} and {out / 'report.json'}")
        return

    processes = Processes()

    def interrupted(number, frame):
        processes.stop_all()
        raise SystemExit("compat_run: interrupted; the processes it started are stopped")
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)

    out.mkdir(parents=True, exist_ok=True)
    games = []
    try:
        for number, entry in enumerate(entries, 1):
            name = entry["id"] or Path(entry["build_from"]).name
            print(f"\n== {number}/{len(entries)} {name}", flush=True)
            folder = out / name
            if entry["build"]:
                code = build(processes, entry, folder, options.build_minutes)
                if not code:
                    continue
                if code != name:
                    shutil.rmtree(out / code, ignore_errors=True)
                    folder.rename(out / code)
                entry, folder = dict(entry, id=code, image=GAMES / f"{code}.wrd"), out / code
            if not program.is_file():
                raise SystemExit(f"compat_run: there's no console at {program}; add a game with add_game.py to build it")
            result = play(processes, program, entry, folder, options)
            if NO_VULKAN in result["ended_detail"] and not any(e.startswith("WII_RECOMP_RENDERER=") for e in options.env):
                print("  Vulkan doesn't work here, so this and later games play with OpenGL")
                options.env.append("WII_RECOMP_RENDERER=opengl")
                result = play(processes, program, entry, folder, options)
            refused = any(stop in result["ended_detail"] for stop in REBUILD_STOPS)
            if refused and entry.get("build_from") and not options.no_build:
                print("  the console refused the image, so it's built again and played once more")
                if build(processes, entry, folder, options.build_minutes):
                    result = play(processes, program, entry, folder, options)
            print(f"  {TIERS[result['tier']]}; {result['ended']}; fps {fps_text(result['fps'])}", flush=True)
            games.append(result)
    finally:
        processes.stop_all()
    if games:
        print(f"\nreport: {write_report(build_report(games, settings), out)}")


if __name__ == "__main__":
    main()
