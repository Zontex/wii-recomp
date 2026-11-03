#!/usr/bin/env python3
"""Measures the Wii Menu's frame rate on a fixed tour of its screens, from the repo root.

menu_bench.py [--build recomp/build-menu] [--renderer vulkan] [--shots FOLDER] [--no-wait]

Each run starts from a fresh copy of the console's storage, after first-time setup,
and drives the menu through the control pipe. For every scene it reports the frames
shown per second and the worst frame, leaving out the scene's first 2 seconds, and
whether the scene passes: at least 59 frames a second, no frame over 25 ms. With
--shots, it saves the picture at the end of each scene, to check the tour went where
it should. Code without a native translation stops the run, as it should never run.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from perf_run import ROOT, speed_limit, wait_for_cool  # noqa: E402

SETTLE_SECONDS, PASS_FPS, PASS_WORST_MS = 2, 59, 25

# Pointer positions run from -1 to 1 across and down the screen, measured from pictures.
GRID_DISC, GRID_OPTIONS, GRID_MAIL = (-0.64, -0.58), (-0.76, 0.68), (0.76, 0.69)
GRID_NEXT_PAGE, GRID_BACK_PAGE = (0.93, -0.17), (-0.93, -0.17)
OPTIONS_DATA, OPTIONS_SETTINGS = (-0.5, -0.04), (0.5, -0.04)
BACK_BUTTON = (-0.5, 0.78)       # Bottom left on the options, settings and disc channel screens.
SETTINGS_NEXT_PAGE = (0.81, -0.05)


def click(position):
    """A held press, which the menu sees reliably."""
    return [f"pointer {position[0]} {position[1]}", "wait 0.3", "hold a", "wait 0.4", "release a"]


# (name, seconds, commands sent at its start)
TOUR = [
    ("health warning", 6, []),
    ("grid", 8, click((0.0, 0.0))),
    ("grid with pointer moving", 8, ["sweep"]),
    ("grid page 2", 8, click(GRID_NEXT_PAGE)),
    ("grid page 1", 6, click(GRID_BACK_PAGE)),
    ("disc channel", 8, click(GRID_DISC)),
    ("back to grid", 6, click(BACK_BUTTON)),
    ("wii options", 8, click(GRID_OPTIONS)),
    ("wii settings page 1", 8, click(OPTIONS_SETTINGS)),
    ("wii settings page 2", 6, click(SETTINGS_NEXT_PAGE)),
    ("wii settings page 3", 6, click(SETTINGS_NEXT_PAGE)),
    ("wii options again", 6, click(BACK_BUTTON)),
    ("data management", 8, click(OPTIONS_DATA)),
    ("wii options once more", 6, click(BACK_BUTTON)),
    ("back to grid again", 6, click(BACK_BUTTON)),
    ("message board", 10, click(GRID_MAIL)),
]


def screen_locked():
    """macOS slows apps it can't show, so a locked screen makes the numbers meaningless."""
    import ctypes
    import ctypes.util
    graphics = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreGraphics"))
    foundation = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreFoundation"))
    graphics.CGSessionCopyCurrentDictionary.restype = ctypes.c_void_p
    foundation.CFDictionaryGetValue.restype = ctypes.c_void_p
    foundation.CFDictionaryGetValue.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    foundation.CFStringCreateWithCString.restype = ctypes.c_void_p
    foundation.CFStringCreateWithCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
    foundation.CFBooleanGetValue.argtypes = [ctypes.c_void_p]
    session = graphics.CGSessionCopyCurrentDictionary()
    if not session:
        return False
    key = foundation.CFStringCreateWithCString(None, b"CGSSessionScreenIsLocked", 0x08000100)
    value = foundation.CFDictionaryGetValue(session, key)
    return bool(value) and bool(foundation.CFBooleanGetValue(value))


def send(pipe, command):
    with open(pipe, "w") as out:
        out.write(command + "\n")


def run_commands(pipe, commands):
    for command in commands:
        if command.startswith("wait "):
            time.sleep(float(command.split()[1]))
        elif command == "sweep":  # Slowly across the grid and back, as a hand would.
            for step in range(40):
                x = -0.8 + 1.6 * (step if step < 20 else 39 - step) / 19
                send(pipe, f"pointer {x:.2f} -0.2")
                time.sleep(0.1)
        else:
            send(pipe, command)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="recomp/build-menu")
    parser.add_argument("--renderer", default="vulkan")
    parser.add_argument("--shots")
    parser.add_argument("--nand", help="the console storage to start from, by default <build>/console/nand")
    parser.add_argument("--no-wait", action="store_true")
    options = parser.parse_args()
    os.chdir(ROOT)
    if screen_locked():
        sys.exit("The screen is locked, and macOS slows the menu down while it can't be seen: unlock it first")
    if not options.no_wait:
        wait_for_cool()
    build = os.path.abspath(options.build)
    seconds = []  # (time it was printed, frames shown, worst frame ms)
    stops = []

    with tempfile.TemporaryDirectory() as folder:
        saves, pipe = os.path.join(folder, "nand"), os.path.join(folder, "control")
        shutil.copytree(os.path.abspath(options.nand) if options.nand else os.path.join(build, "console", "nand"), saves)
        env = dict(os.environ, WII_RECOMP_RENDERER=options.renderer, WII_RECOMP_ACTIVITY="1", WII_RECOMP_SAVES=saves,
                   WII_RECOMP_CONTROL=pipe, WII_RECOMP_MUTE="1", WII_RECOMP_NATIVE_ONLY="1")
        menu = subprocess.Popen([os.path.join(build, "wii-recomp"), "disc/sys"], cwd=build, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")

        def read():
            for line in menu.stdout:
                found = re.search(r"second \d+: .* (\d+) to screen.*worst frame ([\d.]+) ms", line)
                if found:
                    seconds.append((time.time(), int(found.group(1)), float(found.group(2))))
                elif line.startswith("stop:"):
                    stops.append(line.strip())

        threading.Thread(target=read, daemon=True).start()
        time.sleep(25)  # Start-up, until the health warning is up.
        limits, scenes = [speed_limit()], []
        for name, length, commands in TOUR:
            if menu.poll() is not None:
                break
            start = time.time()
            run_commands(pipe, commands)
            time.sleep(max(0, length - (time.time() - start)))
            scenes.append((name, start, time.time()))
            limits.append(speed_limit())
            if options.shots:
                os.makedirs(options.shots, exist_ok=True)
                send(pipe, f"shot {os.path.abspath(os.path.join(options.shots, f'{len(scenes):02}.ppm'))}")
                time.sleep(0.5)
        locked_during = screen_locked()
        menu.kill()
        menu.wait()

    failed = bool(stops)
    print(f"speed limit {min(limits)}-{max(limits)}%, renderer {options.renderer}")
    print(f"{'scene':28} {'fps':>5} {'worst ms':>9}  result")
    for name, start, end in scenes:
        rows = [row for row in seconds if start + SETTLE_SECONDS <= row[0] <= end]
        if not rows:
            print(f"{name:28} {'-':>5} {'-':>9}  no measurement")
            failed = True
            continue
        fps = sum(row[1] for row in rows) / len(rows)
        worst = max(row[2] for row in rows)
        passed = fps >= PASS_FPS and worst <= PASS_WORST_MS
        failed |= not passed
        print(f"{name:28} {fps:5.1f} {worst:9.1f}  {'pass' if passed else 'FAIL'}")
    for line in stops:
        print(line)
    if locked_during:
        print("not comparable: the screen locked during the run")
        failed = True
    if min(limits) < 100:
        print("not comparable: the Mac was slowed down by heat during the run")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
