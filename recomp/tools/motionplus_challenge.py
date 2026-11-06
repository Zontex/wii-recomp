#!/usr/bin/env python3
"""Writes recomp/runtime/wii_motionplus_challenge.h: the answers a simulated Wii MotionPlus
gives to the check games run on it.

Usage: python3 recomp/tools/motionplus_challenge.py > recomp/runtime/wii_motionplus_challenge.h

A game asks the MotionPlus for x, then for y0 or y1, and checks that y0 squared is x, or
y1 squared is x times v, all modulo n. n and the square root of v are Dolphin's
(Source/Core/Core/HW/WiimoteEmu/MotionPlus.cpp). x, y0 and y1 follow from any starting
number below n, so the answers never change, and Python's whole numbers work them out
here instead of a big-number library in the runtime.
"""
N = int("67614561104116375676885818084175632651294951727285593632649596941616763967271774525888270484"
        "88546653264235848263182009106217734439508352645687684489830161")
SQRT_V = int("22331959796794118515742337844101477131884013381589363004659408068948154670914705521646304758"
             "02483462872732436570235909421331424649287229820640697259759264")
START = int.from_bytes(b"WII RECOMP PLAYS IT NATIVELY.", "big")
BYTES = 64


def answers():
    """x, y0 and y1, as the 64 little-endian bytes each the MotionPlus gives."""
    x, y0, y1 = START * START % N, START, START * SQRT_V % N
    return {name: value.to_bytes(BYTES, "little") for name, value in (("x", x), ("y0", y0), ("y1", y1))}


def header():
    lines = ["// Written by recomp/tools/motionplus_challenge.py: the simulated MotionPlus's answers to",
             "// the check games run on it. Don't edit by hand.", "#pragma once", "#include <stdint.h>", ""]
    for name, data in answers().items():
        lines.append(f"static const uint8_t MOTIONPLUS_{name.upper()}[{BYTES}] = {{")
        for row in range(0, BYTES, 16):
            lines.append("  " + ", ".join(f"0x{byte:02x}" for byte in data[row:row + 16]) + ",")
        lines.append("};")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    print(header(), end="")
