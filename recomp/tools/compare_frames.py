#!/usr/bin/env python3
"""Compare two folders of frames, as saved by recomp/tools/replay or by Dolphin.

Usage: recomp/tools/compare_frames.py <folder a> <folder b> [--out <folder>] [--threshold <n>] [--resize]

Frames pair up in order, sorted by the number in their names, so the two folders can
name them differently. For each pair it prints:
  difference   the mean change per color channel, as a percentage of full scale
  pixels       the share of pixels that differ at all
  largest      the largest change in any channel, from 0 to 255

Exits with 1 if any frame's difference is above the threshold, a frame has no partner,
or two frames differ in size without --resize. With --out, each pair that differs gets
a picture there: black where they match, brighter the more they differ.
"""
import argparse
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image

DIFFERENCE_BRIGHTNESS = 8  # Small differences would be invisible at their true size.


def frame_order(path):
    numbers = re.findall(r"\d+", path.stem)
    return (int(numbers[-1]) if numbers else -1, path.name)


def frames_in(folder):
    return sorted(Path(folder).glob("*.png"), key=frame_order)


def load(path, size=None):
    picture = Image.open(path).convert("RGB")
    if size and picture.size != size:
        picture = picture.resize(size, Image.Resampling.BILINEAR)
    return np.asarray(picture, dtype=np.int16)


def measure(a, b):
    """The difference picture and what it comes to."""
    change = np.abs(a - b)
    return change, {
        "difference": float(change.mean()) / 255 * 100,
        "pixels": float(change.any(axis=2).mean()) * 100,
        "largest": int(change.max()),
    }


def save_difference(change, path):
    bright = np.clip(change * DIFFERENCE_BRIGHTNESS, 0, 255).astype(np.uint8)
    Image.fromarray(bright).save(path)


def compare(folder_a, folder_b, out=None, threshold=0.0, resize=False):
    """Prints the report and says whether the folders match."""
    frames_a, frames_b = frames_in(folder_a), frames_in(folder_b)
    if not frames_a or not frames_b:
        print(f"no frames: {len(frames_a)} in {folder_a}, {len(frames_b)} in {folder_b}")
        return False
    if out:
        Path(out).mkdir(parents=True, exist_ok=True)
    matched = len(frames_a) == len(frames_b)
    if not matched:
        print(f"different frame counts: {len(frames_a)} in {folder_a}, {len(frames_b)} in {folder_b}")
    print("frame difference% pixels% largest")
    differences = []
    for number, (path_a, path_b) in enumerate(zip(frames_a, frames_b)):
        a = load(path_a)
        b = load(path_b, (a.shape[1], a.shape[0]) if resize else None)
        if a.shape != b.shape:
            print(f"{number:5} sizes differ: {a.shape[1]}x{a.shape[0]} and {b.shape[1]}x{b.shape[0]}; --resize compares them anyway")
            matched = False
            continue
        change, result = measure(a, b)
        differences.append(result["difference"])
        print(f"{number:5} {result['difference']:11.4f} {result['pixels']:7.3f} {result['largest']:7}")
        matched &= result["difference"] <= threshold
        if out and result["largest"]:
            save_difference(change, Path(out) / f"diff-{number:04}.png")
    if differences:
        print(f"mean {sum(differences) / len(differences):.4f}, worst {max(differences):.4f}, over {len(differences)} frames")
    return matched


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--out", help="folder for difference pictures")
    parser.add_argument("--threshold", type=float, default=0.0, help="the difference allowed per frame, in percent")
    parser.add_argument("--resize", action="store_true", help="scale b's frames to a's size, for two different resolutions")
    arguments = parser.parse_args()
    sys.exit(0 if compare(arguments.a, arguments.b, arguments.out, arguments.threshold, arguments.resize) else 1)


if __name__ == "__main__":
    main()
