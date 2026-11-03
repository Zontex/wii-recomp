#!/usr/bin/env python3
"""Measures frame rate, heat and CPU over a fixed run with no input, from the repo root.

perf_run.py [--seconds 170] [--renderer vulkan] [--nand FOLDER] [--env KEY=VALUE]... [--wav PATH] [--log PATH]

The Wii storage folder comes from --nand or WII_RECOMP_NAND, and is optional.
"""
import argparse, os, re, shutil, subprocess, tempfile, time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
MOLTENVK = "/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json"
LOADING_SECONDS = 30


def speed_limit():
    text = subprocess.run(["pmset", "-g", "therm"], capture_output=True, text=True).stdout
    found = re.search(r"CPU_Speed_Limit\s*=\s*(\d+)", text)
    return int(found.group(1)) if found else 100


def wait_for_cool(seconds=300):
    deadline = time.time() + seconds
    while speed_limit() < 100 and time.time() < deadline:
        time.sleep(5)
    print("speed limit at start:", speed_limit())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=170)
    parser.add_argument("--renderer", default="vulkan")
    parser.add_argument("--env", action="append", default=[])
    parser.add_argument("--wav")
    parser.add_argument("--nand", default=os.environ.get("WII_RECOMP_NAND"), help="a Wii storage folder")
    parser.add_argument("--log", help="keep the game's log at this path")
    parser.add_argument("--no-wait", action="store_true")
    options = parser.parse_args()
    os.chdir(ROOT)
    if not options.no_wait:
        wait_for_cool()

    with tempfile.TemporaryDirectory() as folder:
        saves = os.path.join(folder, "saves")
        shutil.copytree("recomp/build/saves", saves)
        env = dict(os.environ, WII_RECOMP_RENDERER=options.renderer, WII_RECOMP_ACTIVITY="1", WII_RECOMP_SAVES=saves,
                   VK_DRIVER_FILES=MOLTENVK)
        if options.wav:
            env["WII_RECOMP_AUDIO_WAV"] = options.wav
        env.update(pair.split("=", 1) for pair in options.env)
        log_path = os.path.join(folder, "run.log")
        with open(log_path, "w") as log:
            game = subprocess.Popen(["recomp/build/wii-recomp", "extracted/sys"] + ([options.nand] if options.nand else []),
                                    stdout=log, stderr=subprocess.STDOUT, env=env)
            limits, loads, start = [], [], time.time()
            while time.time() - start < options.seconds and game.poll() is None:
                limits.append(speed_limit())
                load = subprocess.run(["ps", "-o", "%cpu=", "-p", str(game.pid)], capture_output=True, text=True).stdout
                if load.strip():
                    loads.append(float(load))
                time.sleep(2)
            ended_early = game.poll()
            game.kill()
            game.wait()
        text = open(log_path, errors="replace").read()
        if options.log:
            shutil.copy(log_path, options.log)

    rows = [(int(s), int(d), int(f), float(w)) for s, d, f, w in re.findall(
        r"second (\d+): (\d+) draws, \d+ copies, (\d+) to screen.*worst frame ([\d.]+) ms", text)]
    measured = [row for row in rows if LOADING_SECONDS <= row[0]][:-1]
    frames = [row[2] for row in measured]
    print(f"seconds measured {len(frames)}, ended early: {ended_early}")
    print(f"average {sum(frames) / len(frames):.1f} fps, worst second {min(frames)}, "
          f"at 59+ {sum(f >= 59 for f in frames)} ({100 * sum(f >= 59 for f in frames) / len(frames):.1f}%)")
    print(f"worst frame {max(row[3] for row in measured):.1f} ms, most draws in a second {max(row[1] for row in measured)}")
    print(f"speed limit lowest {min(limits)}, every 20s {limits[::10]}")
    print(f"process CPU average {sum(loads) / len(loads):.0f}%")
    slow = [dict(zip(("gap", "stream", "present", "disc", "audio", "idle", "longest", "preempted"), map(float, row))) for row in re.findall(
        r"slow frame ([\d.]+) ms: stream ([\d.]+), present ([\d.]+), disc ([\d.]+), audio ([\d.]+), idle ([\d.]+) \(longest ([\d.]+)\), preempted (\d+)", text)]
    print(f"frames over 20 ms: {len(slow)}")
    for frame in sorted(slow, key=lambda f: -f["gap"])[:12]:
        other = frame["gap"] - frame["stream"] - frame["disc"] - frame["audio"] - frame["idle"]
        print(f"  {frame['gap']:.1f}: stream {frame['stream']:.1f} (present {frame['present']:.1f}), disc {frame['disc']:.1f}, "
              f"audio {frame['audio']:.1f}, idle {frame['idle']:.1f} (longest {frame['longest']:.1f}), other {other:.1f}, preempted {frame['preempted']:.0f}")
    print("all slow frames, average:", {k: round(sum(f[k] for f in slow) / max(len(slow), 1), 1) for k in ("gap", "stream", "present", "disc", "audio", "idle", "preempted")})
    print("slow seconds:", [(row[0], row[2], row[3]) for row in measured if row[2] < 58][:30])


if __name__ == "__main__":
    main()
