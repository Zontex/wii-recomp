#!/usr/bin/env python3
"""Checks and times the C versions of audio programs against the real ones, on sound recordings.

Usage: recomp/tools/sound_bench.py <folder of recordings> [--reference <folder>] [--save-reference <folder>]
                                   [--strict] [--verbose]

Record in any game with the interpreter running its real audio program:
WII_RECOMP_DSP=lle and the control pipe's "record-sound <frames> <file>", or
WII_RECOMP_SOUND_RECORD=<frame>:<frames>:<file> (frame 0 records from power-on). Frames are
sixtieths of a second. Recordings hold the game's own data, so keep them out of the
repository. Each .wsr in the folder is played with no game running (recomp/tools/sound_replay.c),
first through the interpreter and then through the console's C version of the program,
mail by mail, and this prints for each:
  frames       mails after which the real program wrote memory: AX's 3 ms command lists
  real ms      the interpreter's time for a frame
  C ms         the C version's time for a frame
  mails        whether the C version answered every mail as the real program did
  differ       16-bit words the C version wrote differently, of those either wrote
  peak, rms    the largest and the typical difference, over all words compared
  first        the first difference: the mail it followed, the address, both values

Before each mail, memory holds what the real program read until the next one; after it,
memory is set to what the real program left, so each mail is judged on its own. A
recording started mid-game is judged from the first mail both answer alike.

With --save-reference the C version's writes are kept (as one hash a mail), and
--reference compares a later run with them. Exits with 1 if a recording fails to play,
the interpreter doesn't replay it as recorded, or the C version's writes differ from the
reference; --strict also fails on any difference from the real program.

Still to do: the Zelda family's C version (zelda.c) has only been checked on made-up
mail, not against its real program. To check it, record Super Mario Galaxy 2 (or any
game whose program is d643001f, 6c3f6f94, b7eb9a9c or eaeb38cc) with the interpreter:
  WII_RECOMP_DSP=lle WII_RECOMP_SOUND_RECORD=0:1800:galaxy-boot.wsr recomp/build/wii-recomp games/SB4E01.wrd
then a few seconds each of the title, a menu and play with music and effects, through
the control pipe's "record-sound 300 <file>", and run this on the folder. The first
difference it prints is the place to start; Galaxy 2 logs audio.zelda_unknown_buffer
for four buffer numbers, and its sound going flat after about 3 seconds may be one.
"""
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNTIME = HERE.parent / "runtime"
SOURCES = ["dsp.c", "dsp_lle.c", "dsp_lle_ops.c", "dsp_accelerator.c", "dsp_record.c", "ax.c", "zelda.c"]
FLAGS = ["-O2", "-w", "-fwrapv", "-fno-strict-aliasing", "-fsigned-char", "-ffp-contract=off", f"-I{RUNTIME}", "-lm"]


def player(folder):
    program = Path(folder) / "sound_replay"
    subprocess.run(["clang", str(HERE / "sound_replay.c"), *(str(RUNTIME / name) for name in SOURCES), *FLAGS,
                    "-o", str(program)], check=True, capture_output=True, text=True)
    return program


def summary(output):
    """The player's last line: key=value pairs."""
    lines = [line for line in output.splitlines() if line.startswith("summary ")]
    return dict(pair.split("=", 1) for pair in lines[-1].split()[1:]) if lines else {}


def first_difference(result):
    if int(result["differ"]) == 0:
        return "none" if result["first_mails"] == "same" else "mails only"
    return (f"mail {result['first_step']} ({result['first_mail']}) at {result['first_address']}: "
            f"real {result['first_values'].replace('/', ', C ')}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("folder")
    parser.add_argument("--reference", help="folder of the C version's writes from an earlier run")
    parser.add_argument("--save-reference", help="folder to keep this run's C version writes in")
    parser.add_argument("--strict", action="store_true", help="fail on any difference from the real program")
    parser.add_argument("--verbose", action="store_true", help="also print the player's details for each recording")
    args = parser.parse_args()
    recordings = sorted(Path(args.folder).glob("*.wsr"))
    if not recordings:
        sys.exit(f"no .wsr recordings in {args.folder}")
    failed = False
    with tempfile.TemporaryDirectory() as temporary:
        program = player(temporary)
        print(f"{'recording':<24} {'program':<12} {'frames':>6} {'real ms':>7} {'C ms':>6} {'mails':<6} "
              f"{'differ':>15} {'peak':>6} {'rms':>8}  first")
        for recording in recordings:
            hashes = Path(temporary) / f"{recording.stem}.hashes"
            command = [str(program), str(recording), str(RUNTIME / "data"), "--hashes", str(hashes)]
            if args.verbose:
                command.append("--verbose")
            run = subprocess.run(command, capture_output=True, text=True)
            result = summary(run.stdout)
            if run.returncode or "failed" in result or not result:
                problem = result.get("failed") or (run.stdout + run.stderr).strip().splitlines()[-1:] or ["no output"]
                print(f"{recording.stem:<24} failed: {problem if isinstance(problem, str) else problem[0]}")
                failed = True
                continue
            if args.verbose:
                print(run.stderr.rstrip())
            mails = "same" if result["first_mails"] == "same" else "differ"
            differ = f"{result['differ']}/{result['words']}"
            print(f"{recording.stem:<24} {result['program']:<12} {result['frames']:>6} {float(result['interpreter_ms']):>7.3f} "
                  f"{float(result['c_ms']):>6.3f} {mails:<6} {differ:>15} {result['peak']:>6} {float(result['rms']):>8.3f}  "
                  f"{first_difference(result)}")
            if result["replays"] == "no":
                print(f"{'':<24} the interpreter doesn't replay this recording as recorded")
                failed = True
            if result["program"] == "none":
                print(f"{'':<24} the program has no C version, so only the interpreter ran")
            elif int(result["in_step_from"]) < 0:
                print(f"{'':<24} the C version never answered a mail as the real program did")
            elif int(result["in_step_from"]) != 0:
                print(f"{'':<24} recorded mid-game: judged from mail {result['in_step_from']}")
            if args.strict and (int(result["differ"]) or mails != "same"):
                failed = True
            if args.reference:
                kept = Path(args.reference) / hashes.name
                if not kept.exists():
                    print(f"{'':<24} no reference for this recording")
                    failed = True
                else:
                    ours, theirs = hashes.read_text().splitlines(), kept.read_text().splitlines()
                    changed = next((a.split()[0] for a, b in zip(ours, theirs) if a != b), None)
                    if changed is None and len(ours) != len(theirs):
                        changed = str(min(len(ours), len(theirs)))
                    print(f"{'':<24} C version writes {'match the reference' if changed is None else f'differ from the reference from mail {changed}'}")
                    failed |= changed is not None
            if args.save_reference:
                Path(args.save_reference).mkdir(parents=True, exist_ok=True)
                shutil.copy(hashes, Path(args.save_reference) / hashes.name)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
