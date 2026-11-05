#!/usr/bin/env python3
"""Times and checks recorded scenes: every .dff in a folder, played through recomp/tools/replay.

Usage: recomp/tools/scene_bench.py <folder of recordings> [--scale <n/d>] [--repeat <n>] [--reference <folder>] [--save-reference <folder>]

Record scenes in any game with the control pipe's "record <frames> <file>" (gx_record.c),
or WII_RECOMP_RECORD=<frame>:<frames>:<file>. Recordings hold the game's memory, so keep
them out of the repository. For each one this prints:
  cpu ms     the processor's time for a frame, on average: reading its commands and
             recording the chip's work, leaving out any wait for the chip
  wall ms    the time a frame took, waits included: the larger of the two sets the pace
  gpu ms     the graphics chip's time for one frame, and its three costliest steps
  pictures   with --reference, the largest difference from the pictures saved there

The player draws at the console's own scale, 2 unless --scale or WII_RECOMP_SCALE says
otherwise. With --save-reference the pictures are kept as the reference for later runs.
Exits with 1 if a recording fails to play or a picture differs from its reference.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPLAY = HERE / "replay"
FRAME_COUNT_AT = 68


def player():
    make = ["make", "-C", str(REPLAY)]
    if os.environ.get("VULKAN_SDK"):
        make.append(f"VULKAN_SDK={os.environ['VULKAN_SDK']}")
    subprocess.run(make, check=True, capture_output=True, text=True)
    return REPLAY / "build" / "replay"


def gpu_frames(log):
    """Each timed frame: its total, and its steps as (milliseconds, name)."""
    frames = []
    for line in log.splitlines():
        if line.startswith("gpu frame "):
            frames.append((float(line.split()[2]), []))
        elif frames and re.match(r"\s+[\d.]+ ms ", line):
            ms, name = line.strip().split(" ms ", 1)
            frames[-1][1].append((float(ms), name))
    return frames


def play(program, recording, out, repeat):
    """Times the processor in one run, and the graphics chip in another, as timing the chip
    waits for it after each frame it times."""
    with open(recording, "rb") as file:
        frames = struct.unpack_from("<I", file.read(FRAME_COUNT_AT + 4), FRAME_COUNT_AT)[0]  # dff.c's HEADER_FRAME_COUNT.
    environment = {key: value for key, value in os.environ.items() if key != "WII_RECOMP_GPU_TIMES"}
    result = subprocess.run([str(program), str(recording), str(out), "--repeat", str(repeat)], capture_output=True, text=True, env=environment)
    if result.returncode:
        return None, result.stdout + result.stderr
    timed = re.search(r"timed \d+ frames: ([\d.]+) ms each.*?([\d.]+) ms of it working", result.stdout)
    environment["WII_RECOMP_GPU_TIMES"] = str(max(frames, 2))
    gpu = subprocess.run([str(program), str(recording), str(out) + "-gpu", "--repeat", "6"], capture_output=True, text=True, env=environment)
    return {"wall": float(timed.group(1)) if timed else None, "cpu": float(timed.group(2)) if timed else None, "gpu": gpu_frames(gpu.stderr)}, ""


def compare(a, b):
    result = subprocess.run([sys.executable, str(HERE / "compare_frames.py"), str(a), str(b), "--threshold", "0"], capture_output=True, text=True)
    worst = re.search(r"worst ([\d.]+)", result.stdout)
    return result.returncode == 0, worst.group(1) if worst else "no frames"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("folder")
    parser.add_argument("--scale", help="the picture's size against the Wii's, such as 2 or 5/2, as WII_RECOMP_SCALE takes it")
    parser.add_argument("--repeat", type=int, default=10, help="times to play each recording, the first left out of the timing")
    parser.add_argument("--reference", help="folder of pictures from an earlier run to compare with")
    parser.add_argument("--save-reference", help="folder to keep this run's pictures in")
    args = parser.parse_args()
    recordings = sorted(Path(args.folder).glob("*.dff"))
    if not recordings:
        sys.exit(f"no .dff recordings in {args.folder}")
    program = player()
    if args.scale:
        os.environ["WII_RECOMP_SCALE"] = args.scale
    failed = False
    print(f"{'scene':<28} {'cpu ms':>7} {'wall ms':>7} {'gpu ms':>7}  costliest graphics steps")
    with tempfile.TemporaryDirectory() as temporary:
        for recording in recordings:
            out = Path(temporary) / recording.stem
            timing, problem = play(program, recording, out, args.repeat)
            if timing is None:
                print(f"{recording.stem:<28} failed: {problem.strip().splitlines()[-1] if problem.strip() else 'no output'}")
                failed = True
                continue
            # The median, as a timed frame can be the one that brings all memory in.
            gpu = sorted(timing["gpu"], key=lambda frame: frame[0])
            middle = gpu[len(gpu) // 2] if gpu else None
            costliest = ", ".join(f"{ms:.1f} {name}" for ms, name in sorted(middle[1], reverse=True)[:3]) if middle else ""
            cpu, wall = (f"{timing[key]:.2f}" if timing[key] is not None else "-" for key in ("cpu", "wall"))
            print(f"{recording.stem:<28} {cpu:>7} {wall:>7} {f'{middle[0]:.2f}' if middle else '-':>7}  {costliest}")
            if args.reference:
                same, worst = compare(Path(args.reference) / recording.stem, out)
                print(f"{'':<28} pictures {'match' if same else 'differ'}, worst difference {worst}%")
                failed |= not same
            if args.save_reference:
                kept = Path(args.save_reference) / recording.stem
                shutil.rmtree(kept, ignore_errors=True)
                shutil.copytree(out, kept)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
