#!/usr/bin/env python3
"""Turn a Wii disc into a game the console plays, in one command.

Usage: recomp/tools/add_game.py <disc image | extracted disc folder | game ID> [--console FOLDER]
                                [--fifo-breakpoints] [--scale N[/D] | default] [--no-pack] [--jobs N]

It extracts the disc (an .iso, .rvz or .wbfs), translates its program with every piece of
code it can find and the game's own Wii Remote code, builds the console if needed and the
title's native code, and packs both into games/<game ID>.wrd. Play it with
`recomp/build/wii-recomp games/<game ID>.wrd`, or give --console to put it in that
console's drive for the Wii Menu (recomp/tools/prepare_console.py makes one).

The first build compiles everything quickly, and the code counts how often it runs.
Run add_game.py again after playing (with the game ID, the disc or the folder) and the
code that did the work is compiled for speed, using what every run so far recorded:
the profile and any code the interpreter had to run. Only what changed is rebuilt, and
the image's native code is swapped without copying the disc again. The image also
carries the shaders every run drew with, as recipes any computer builds before the
first picture, so they don't hold up frames during play.

Each game builds in recomp/build-<game ID>/, which keeps its settings in settings.json.
--fifo-breakpoints is remembered there, for games that keep several frames in the FIFO.
--scale sets the picture's size against the Wii's for this game, such as 3/2 for a game
too heavy for a small graphics chip at the console's 2; it's remembered there too, and
--scale default goes back to the console's own. WII_RECOMP_SCALE still wins when set.
"""
import argparse
import json
import os
import platform
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RECOMP = ROOT / "recomp"
CONSOLE_BUILD = RECOMP / "build"
GAMES = ROOT / "games"
TITLE_ID_OFFSET = 0x18c  # In the disc's tmd.bin.
LIBRARY = "title.dylib" if sys.platform == "darwin" else "title.so"
DISC_IMAGES = {".iso", ".rvz", ".wbfs", ".ciso", ".gcz", ".wia"}

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_disc  # noqa: E402


def fail(message):
    raise SystemExit(f"add_game: {message}")


def native_platform():
    """The platform name a recompiled disc image files this computer's native code under."""
    system = {"Darwin": "darwin", "Linux": "linux"}.get(platform.system())
    machine = {"AMD64": "x86_64", "x86_64": "x86_64", "arm64": "arm64", "aarch64": "arm64"}.get(platform.machine())
    if system == "linux" and machine == "arm64":
        machine = "aarch64"
    name = f"{system}-{machine}"
    if name not in make_disc.PLATFORMS:
        fail(f"there's no native code format for {platform.system()} {platform.machine()}")
    return name


def check_disc(folder):
    missing = [name for name in ("sys/main.dol", "sys/boot.bin", "sys/fst.bin", "tmd.bin") if not (folder / name).is_file()]
    if missing:
        fail(f"{folder} isn't an extracted Wii disc: it has no {', '.join(missing)}")


def game_id(disc):
    """The six letters on the disc, such as RFPE01, which name its build and image."""
    code = (disc / "sys/boot.bin").read_bytes()[:6]
    if len(code) != 6 or not all(48 <= byte < 58 or 65 <= byte < 91 for byte in code):
        fail(f"{disc / 'sys/boot.bin'} doesn't start with a game ID")
    return code.decode("ascii")


def title_key(disc):
    """The title ID as the console names the title's data folder, such as 00010004-52465045."""
    tmd = (disc / "tmd.bin").read_bytes()
    high, low = int.from_bytes(tmd[TITLE_ID_OFFSET:TITLE_ID_OFFSET + 4], "big"), int.from_bytes(tmd[TITLE_ID_OFFSET + 4:TITLE_ID_OFFSET + 8], "big")
    return f"{high:08x}-{low:08x}"


def run(step, command, cwd=None):
    started = time.monotonic()
    print(f"\n== {step}", flush=True)
    result = subprocess.run(command, cwd=cwd)
    if result.returncode:
        fail(f"{step} failed; see the messages above")
    seconds = time.monotonic() - started
    print(f"   {step}: {seconds:.0f} s", flush=True)
    return seconds


def extract(image):
    """Extracts an image into a game's build folder, named after the ID found inside."""
    incoming = RECOMP / "build-incoming"
    run("extract the disc", [sys.executable, str(RECOMP / "tools/extract_disc.py"), str(image), "--into", str(incoming / "disc"), "--force"])
    disc = incoming / "disc"
    check_disc(disc)
    build = RECOMP / f"build-{game_id(disc)}"
    build.mkdir(exist_ok=True)
    shutil.rmtree(build / "disc", ignore_errors=True)
    disc.rename(build / "disc")
    shutil.rmtree(incoming, ignore_errors=True)
    return build / "disc"


def find_game(source):
    """(build folder, settings) for a game ID, an extracted folder or a disc image."""
    by_id = RECOMP / f"build-{source}"
    if (by_id / "settings.json").is_file():
        return by_id, json.loads((by_id / "settings.json").read_text())
    path = Path(source).expanduser()
    if path.is_dir():
        disc = path.resolve()
    elif path.is_file() and path.suffix.lower() in DISC_IMAGES:
        disc = extract(path.resolve())
    elif path.exists():
        fail(f"{path} is neither a disc image ({', '.join(sorted(DISC_IMAGES))}) nor an extracted disc folder")
    else:
        fail(f"there's no {source}, and no game built under that ID")
    check_disc(disc)
    build = RECOMP / f"build-{game_id(disc)}"
    settings = json.loads((build / "settings.json").read_text()) if (build / "settings.json").is_file() else {}
    settings["disc"] = str(disc)
    return build, settings


def recorded(key, name, consoles):
    """Every copy of a title's data file the consoles wrote: beside the console program for
    direct play, and in each console folder's cache for play from the Wii Menu."""
    places = [CONSOLE_BUILD / "title-data" / key / name] + [Path(folder) / "cache/titles" / key / name for folder in consoles]
    return [path for path in places if path.is_file()]


def merge_profiles(paths, into):
    """Adds the counts from each console's profile into one file for the translator."""
    counts = {}
    for path in paths:
        for line in path.read_text().splitlines():
            if line and not line.startswith("#"):
                address, count = line.split()
                counts[address] = counts.get(address, 0) + int(count)
    into.write_text("".join(f"{address} {count}\n" for address, count in sorted(counts.items())))
    return len(counts)


RECIPE_HEADER = struct.Struct("<4s7I")  # As gx_recipes.c writes it: magic, version, record size, title ID, then room.


def merge_recipes(sources, into):
    """Every shader recipe from each copy (files the consoles wrote, and what the image
    already carries), once each and in the order first seen, into one file. Only copies
    of the newest version are used. Returns how many, or 0 with no file written."""
    copies = []
    for data in sources:
        if len(data) >= RECIPE_HEADER.size:
            magic, version, record_bytes, *_ = RECIPE_HEADER.unpack_from(data)
            if magic == b"WRSR" and record_bytes:
                copies.append((version, data[:RECIPE_HEADER.size], record_bytes, data))
    if not copies:
        into.unlink(missing_ok=True)
        return 0
    newest = max(copies)[1]
    seen, records = set(), []
    for _, header, record_bytes, data in copies:
        if header != newest:
            continue
        for at in range(RECIPE_HEADER.size, len(data) - record_bytes + 1, record_bytes):
            record = data[at:at + record_bytes]
            if record not in seen:
                seen.add(record)
                records.append(record)
    into.write_bytes(newest + b"".join(records))
    return len(records)


def pack(disc, image, library, shaders):
    natives = [(native_platform(), library)]
    GAMES.mkdir(exist_ok=True)
    started = time.monotonic()
    print("\n== pack the recompiled disc image", flush=True)
    try:
        swapped = image.is_file() and make_disc.replace_native(image, natives, shaders)
    except SystemExit:
        swapped = False  # Not a readable image; pack it whole below.
    if not swapped:
        make_disc.write_image(disc, image, natives, shaders)
    print(f"   {'swapped the native code in' if swapped else 'packed'} {image}: {time.monotonic() - started:.0f} s", flush=True)


def insert(image, console):
    """Puts the image in the console's drive, moving any other disc to its shelf."""
    drive = Path(console) / "drive"
    if not drive.is_dir():
        fail(f"{console} has no drive folder; make the console with recomp/tools/prepare_console.py")
    target = drive / image.name
    for other in drive.glob("*.wrd"):
        if other.name != image.name:
            (Path(console) / "shelf").mkdir(exist_ok=True)
            other.rename(Path(console) / "shelf" / other.name)
            print(f"moved {other.name} from the drive to {Path(console) / 'shelf'}")
    if target.exists() and os.path.samefile(target, image):
        return
    target.unlink(missing_ok=True)
    try:
        os.link(image, target)  # The same file, so a later rebuild reaches the drive too.
    except OSError:
        shutil.copy2(image, target)
    print(f"{image.name} is in {drive}")


def main():
    parser = argparse.ArgumentParser(usage=__doc__.split("\n\n")[1].removeprefix("Usage: "), description=__doc__.split("\n")[0])
    parser.add_argument("source", help="a disc image, an extracted disc folder, or the ID of a game added before")
    parser.add_argument("--console", action="append", default=[], help="a console folder to put the game in, and to read its runs from")
    parser.add_argument("--fifo-breakpoints", action="store_true", help="for games that keep several frames in the FIFO; remembered")
    parser.add_argument("--scale", help="the picture's size against the Wii's for this game, such as 3/2, or default; remembered")
    parser.add_argument("--no-pack", action="store_true", help="build the native code but don't pack a disc image")
    parser.add_argument("--jobs", type=int, default=os.cpu_count(), help="compiles at once")
    options = parser.parse_args()

    started = time.monotonic()
    build, settings = find_game(options.source)
    disc = Path(settings["disc"])
    check_disc(disc)
    code, key = game_id(disc), title_key(disc)
    settings["fifo_breakpoints"] = settings.get("fifo_breakpoints", False) or options.fifo_breakpoints
    if options.scale == "default":
        settings.pop("scale", None)
    elif options.scale:
        settings["scale"] = options.scale
    settings["consoles"] = sorted(set(settings.get("consoles", [])) | {str(Path(folder).resolve()) for folder in options.console})
    build.mkdir(exist_ok=True)
    (build / "settings.json").write_text(json.dumps(settings, indent=2) + "\n")
    print(f"{code} (title {key}) from {disc}")

    profiles = recorded(key, "profile.txt", settings["consoles"])
    profile = build / "profile.txt"
    if profiles:
        print(f"using {merge_profiles(profiles, profile)} counted functions from {len(profiles)} profile(s)")
    else:
        profile.unlink(missing_ok=True)
        print("nothing has run yet, so everything compiles quickly and counts its runs")
    discovered = recorded(key, "discovered.txt", settings["consoles"])

    (CONSOLE_BUILD / "obj").mkdir(parents=True, exist_ok=True)
    run("build the console", [sys.executable, str(RECOMP / "translate/build_file.py"), str(CONSOLE_BUILD)])
    run("compile the console", ["ninja", "-j", str(options.jobs), "wii-recomp"], cwd=CONSOLE_BUILD)
    emit = [sys.executable, str(RECOMP / "translate/emit.py"), str(disc / "sys/main.dol"), str(build / "generated"),
            "--all-code", "--virtual-bluetooth", "--profile", str(profile)]
    emit += ["--fifo-breakpoints"] if settings["fifo_breakpoints"] else []
    emit += ["--scale", settings["scale"]] if settings.get("scale") else []
    for path in discovered:
        emit += ["--discovered", str(path)]
    run("translate the program", emit)
    run("write the build file", [sys.executable, str(RECOMP / "translate/build_file.py"), str(build)])
    (build / "obj").mkdir(exist_ok=True)
    run("compile the title", ["ninja", "-j", str(options.jobs), LIBRARY], cwd=build)

    if options.no_pack:
        print(f"\n{code} is built in {build / LIBRARY}: {time.monotonic() - started:.0f} s in all")
        return
    image = GAMES / f"{code}.wrd"
    shaders = build / "shaders.bin"
    carried = []
    try:
        carried = [make_disc.read_shaders(image)] if image.is_file() else []
    except SystemExit:
        pass  # Not a readable image: it's packed again whole.
    sources = [path.read_bytes() for path in recorded(key, "shaders.bin", settings["consoles"])] + [data for data in carried if data]
    count = merge_recipes(sources, shaders)
    if count:
        print(f"carrying {count} shader recipes, so the first picture waits for them rather than play")
    pack(disc, image, build / LIBRARY, shaders if count else None)
    for console in settings["consoles"]:
        insert(image, console)
    print(f"\n{code} is ready in {time.monotonic() - started:.0f} s. Play it with:\n"
          f"  {CONSOLE_BUILD.relative_to(ROOT) / 'wii-recomp'} {image.relative_to(ROOT)}\n"
          "Run this again after playing to compile the code that ran most for speed.")


if __name__ == "__main__":
    main()
