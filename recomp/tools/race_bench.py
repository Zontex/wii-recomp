#!/usr/bin/env python3
"""Times a Mario Kart Wii race the same way every time, to tell whether a change made the
console faster and smoother: autoplay's race session drives from the strap screen to a
race on Luigi Circuit and holds accelerate, and this reads the console's log from the
moment the race begins.

    python3 recomp/tools/race_bench.py [--console recomp/build] [--game games/RMCE01.wrd]
        [--seconds 60] [--env KEY=VALUE]... [--timeline FRAMES] [--record FRAMES] [--out DIR]

It reports frames a second, how evenly frames came (the median and the slowest 5% and 1%
of frame times, and the share between 15.5 and 17.9 ms, which is what an even 60 looks
like), times the card was emptied mid-frame (drains), copies the game had to wait for
(waited reads), shaders built during the race, pictures the window didn't show, and code
run by the interpreter. The run's log, pictures and any timeline or recording stay in
--out, outside the repository, as they hold game data.

Measure on a quiet machine: screen unlocked, nothing building. Recording a scene changes
how copies are read back for the rest of the run, so a run with --record isn't a timing.
"""
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
EVEN = (15.5, 17.9)  # Milliseconds: a frame on time at 60.

SECOND = re.compile(r"^second \d+: (\d+) draws, \d+ copies, (\d+) to screen.*?(\d+) new shaders.*?new pipelines (\d+)"
                    r"(?:, drains (\d+), waited reads (\d+))?", re.M)
FRAME = re.compile(r"^frame (\d+(?:\.\d+)?)$", re.M)
NOT_SHOWN = re.compile(r"^window: (\d+) pictures not shown", re.M)
INTERPRETING = re.compile(r"^interpreting ", re.M)


def percentile(values, share):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(share * len(ordered)))] if ordered else 0.0


def summarize(log, offset=0):
    """What the race part of a log says, as a dict."""
    race = log[offset:]
    seconds = [tuple(int(value or 0) for value in found) for found in SECOND.findall(race)]
    frames = [float(value) for value in FRAME.findall(race)]
    fps = [second[1] for second in seconds]
    result = {
        "seconds": len(seconds),
        "fps": fps,
        "fps_median": statistics.median(fps) if fps else 0,
        "seconds_at_58_or_more": sum(1 for value in fps if value >= 58),
        "draws_per_frame": round(sum(s[0] for s in seconds) / max(1, sum(fps))),
        "frames": len(frames),
        "frame_ms_median": round(statistics.median(frames), 2) if frames else 0,
        "frame_ms_95": round(percentile(frames, 0.95), 2),
        "frame_ms_99": round(percentile(frames, 0.99), 2),
        "even_share": round(sum(1 for value in frames if EVEN[0] <= value <= EVEN[1]) / len(frames), 3) if frames else 0,
        "new_shaders": sum(s[2] for s in seconds),
        "new_pipelines": sum(s[3] for s in seconds),
        "drains": sum(s[4] for s in seconds),
        "waited_reads": sum(s[5] for s in seconds),
        "not_shown": sum(int(value) for value in NOT_SHOWN.findall(race)),
        "interpreted": len(INTERPRETING.findall(race)),
    }
    return result


def report(result):
    lines = [
        f"race: {result['seconds']} s, {result['fps_median']} fps median, {result['seconds_at_58_or_more']} s at 58 or more,"
        f" {result['draws_per_frame']} draws a frame",
        f"frames: {result['frames']}, median {result['frame_ms_median']} ms, slowest 5% over {result['frame_ms_95']} ms,"
        f" slowest 1% over {result['frame_ms_99']} ms, {result['even_share'] * 100:.1f}% even",
        f"drains {result['drains']}, waited reads {result['waited_reads']}, new shaders {result['new_shaders']},"
        f" new pipelines {result['new_pipelines']}, not shown {result['not_shown']}, interpreted {result['interpreted']}",
        "fps by second: " + " ".join(str(value) for value in result["fps"]),
    ]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--console", default=os.path.join(ROOT, "recomp/build"))
    parser.add_argument("--game", default=os.path.join(ROOT, "games/RMCE01.wrd"))
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--env", action="append", default=[])
    parser.add_argument("--timeline", type=int, default=0, help="a timeline of this many frames, 10 s into the race")
    parser.add_argument("--record", type=int, default=0, help="record this many frames for recomp/tools/replay")
    parser.add_argument("--out", default=os.path.expanduser(f"~/wii-recomp-runs/race-{time.strftime('%Y%m%d-%H%M%S')}"))
    options = parser.parse_args()
    out = os.path.abspath(options.out)
    if out == ROOT or out.startswith(ROOT + os.sep):
        sys.exit("race_bench: --out must be outside the repository: the run holds game data")
    environment = dict(os.environ, RACE_SECONDS=str(options.seconds), RACE_TIMELINE=str(options.timeline),
                       RACE_RECORD=str(options.record))
    command = [sys.executable, os.path.join(HERE, "autoplay/autoplay.py"), "race", "--disc", options.game, "--game", "RMCE01",
               "--build", options.console, "--keep", out, "--env", "WII_RECOMP_FRAME_LOG=1"]
    for pair in options.env:
        command += ["--env", pair]
    played = subprocess.run(command, env=environment)
    log_path = os.path.join(out, "game.log")
    if not os.path.exists(log_path):
        sys.exit(f"race_bench: no log at {log_path}")
    with open(log_path, errors="replace") as handle:
        log = handle.read()
    race_file = os.path.join(out, "race.json")
    if not os.path.exists(race_file):
        sys.exit(f"race_bench: the race never began (see {log_path})")
    with open(race_file) as handle:
        offset = json.load(handle)["log_offset"]
    result = summarize(log, offset)
    with open(os.path.join(out, "bench.json"), "w") as handle:
        json.dump(result, handle, indent=1)
    print(report(result))
    print(f"(log and pictures in {out})")
    return played.returncode


if __name__ == "__main__":
    sys.exit(main())
