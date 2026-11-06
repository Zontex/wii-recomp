#!/usr/bin/env python3
"""Plays a game by script and checks it got where it should: automatic testing for any title.

    python3 recomp/tools/autoplay/autoplay.py <session> [--disc extracted/sys] [--keep FOLDER]
        [--build recomp/build] [--saves FOLDER] [--storage FOLDER] [--env KEY=VALUE]...
    python3 recomp/tools/autoplay/autoplay.py <session> --console FOLDER [--saves FOLDER] ...

It starts the game, presses buttons, moves the pointer and stands on the Balance Board
through the control pipe, saves the game's own pictures, and can read the text on them
(macOS's text recognition, through screen_text.swift). It waits for lines in the game's
log, so nothing depends on where the window is or what covers it.

A session is a Python file with a function run(game). The disc's game ID, read from its
boot.bin, picks where to look first:

    games/<game ID>/<session>.py   sessions for one game, such as games/RFPE01/setup.py,
                                   which plays Wii Fit Plus's first-time setup
    sessions/<session>.py          sessions for any game: look (pictures every few
                                   seconds), explore (reads the screen and presses on)

or give a session's path. With --console, the console starts from its Wii Menu, and
sessions come from games/00000001-00000002/, the menu's title ID. The console is
copied first: its storage (from --saves when given), with its titles, drive and caches
linked, so a session never changes the real console's saves either. To add a game, make games/<its ID>/ and write sessions there;
player.py has the reading and clicking they can build on.

The game runs on a copy of its starting saves, so a session never changes real ones. A
session can name where it starts with SAVES = "<name>", meaning
<build>/session-saves/<game ID>/<name>. Without one it starts from <build>/saves; --saves
overrides both. Pictures and saves are the game's, so they go to --keep or a temporary
folder, never into the repository. Exit status is 0 when every step passed. With
AUTOPLAY_LEAVE_RUNNING=1, a game still running when the session ends is left running,
to look into one that's stuck.
"""
import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
MOLTENVK = "/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json"
MENU_TITLE = "00000001-00000002"  # The Wii Menu's title ID names its sessions' folder.


# Letters text recognition sometimes reads from another alphabet: Cyrillic that looks Latin.
LOOKALIKES = str.maketrans("ОКАВЕНМРСТХоаеорсух", "OKABEHMPCTXoaeopcyx")


class StepFailed(Exception):
    pass


class Game:
    def __init__(self, build, folder, starting_saves, disc, game_id, storage=None, env=(), console=None):
        self.build = build
        self.game_id = game_id
        self.folder = folder
        self.pipe = os.path.join(folder, "control")
        self.log_path = os.path.join(folder, "game.log")
        environment = dict(os.environ, WII_RECOMP_CONTROL=self.pipe, WII_RECOMP_ACTIVITY="1")
        if console:
            self.console = copy_console(console, os.path.join(folder, "console"), starting_saves)
            self.saves = os.path.join(self.console, "nand")
            command = [os.path.join(build, "wii-recomp"), "--console", self.console]
        else:
            self.console = None
            self.saves = os.path.join(folder, "saves")
            shutil.copytree(starting_saves, self.saves)
            environment["WII_RECOMP_SAVES"] = self.saves
            command = [os.path.join(build, "wii-recomp"), disc] + ([storage] if storage else [])
        environment.setdefault("WII_RECOMP_RENDERER", "vulkan")  # Pictures for shot() come from Vulkan.
        if sys.platform == "darwin" and os.path.exists(MOLTENVK):
            environment.setdefault("VK_DRIVER_FILES", MOLTENVK)
        environment.update(pair.split("=", 1) for pair in env)
        self.log = open(self.log_path, "w")
        self.started = time.time()
        self.process = subprocess.Popen(command, stdout=self.log, stderr=subprocess.STDOUT, env=environment, cwd=ROOT)
        self.read_at = 0
        self.shots = 0
        self.wait_for(r"^second 1:", 60, "the game to start drawing")

    def fail(self, message):
        """Ends the session as failed. Sessions call this rather than raising their own."""
        raise StepFailed(message)

    def seconds(self):
        return time.time() - self.started

    def text(self):
        with open(self.log_path, errors="replace") as log:
            return log.read()

    def check_running(self):
        if self.process.poll() is not None:
            raise StepFailed(f"the game stopped with status {self.process.returncode}")
        stops = re.findall(r"^stop:.*$", self.text(), re.M)
        if stops:
            raise StepFailed(stops[0])

    def send(self, command):
        deadline = time.time() + 10
        while not os.path.exists(self.pipe):
            if time.time() > deadline:
                raise StepFailed("the control pipe never appeared")
            time.sleep(0.1)
        with open(self.pipe, "w") as pipe:
            pipe.write(command + "\n")
        self.note(f"-> {command}")

    def note(self, message):
        print(f"{self.seconds():6.1f}  {message}", flush=True)

    def press(self, button, then=0.6):
        self.send(f"press {button}")
        time.sleep(then)

    def point(self, x, y, then=0.3):
        self.send(f"pointer {x} {y}")
        time.sleep(then)

    def click(self, x, y, then=0.8):
        self.point(x, y)
        self.press("a", then)

    def board(self, standing, kilograms=60, lean_x=0, lean_y=0):
        self.send(f"board on {kilograms} {lean_x} {lean_y}" if standing else "board off")

    def wait_for(self, pattern, seconds, what):
        """Waits for a log line matching pattern, written after the last one found."""
        deadline = time.time() + seconds
        expression = re.compile(pattern, re.M)
        while True:
            text = self.text()
            found = expression.search(text, self.read_at)
            if found:
                self.read_at = found.end()
                self.note(f"saw {what}")
                return found
            self.check_running()
            if time.time() > deadline:
                raise StepFailed(f"no sign of {what} after {seconds} s")
            time.sleep(0.25)

    def wait(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.check_running()
            time.sleep(min(0.5, deadline - time.time()) if deadline > time.time() else 0)

    def shot(self, name):
        """Saves the next picture as <folder>/<number>-<name>.png, and returns its path."""
        self.shots += 1
        ppm = os.path.join(self.folder, f"{self.shots:02}-{name}.ppm")
        # Asked again now and then: a console starting another title makes its pipe anew,
        # and a request sent as it does is lost.
        deadline, again = time.time() + 15, 0
        while not os.path.exists(ppm):
            self.check_running()
            if time.time() > deadline:
                raise StepFailed(f"the picture {name} was never saved")
            if time.time() > again:
                self.send(f"shot {ppm}")
                again = time.time() + 4
            time.sleep(0.1)
        png = ppm[:-4] + ".png"
        if sys.platform == "darwin":
            subprocess.run(["sips", "-s", "format", "png", ppm, "--out", png], capture_output=True)
            os.remove(ppm)
            return png
        return ppm

    def read_screen(self, name="screen"):
        """The text on screen now, as a list of {text, x, y}, with x and y in pointer units."""
        picture = self.shot(name)
        found = json.loads(subprocess.run([screen_reader(), picture], capture_output=True, text=True).stdout or "[]")
        for item in found:
            item["text"] = item["text"].translate(LOOKALIKES).strip()
        self.note("reads: " + " | ".join(item["text"] for item in found))
        return found

    def saved_files(self):
        """Each save file with its size and modification time."""
        found = {}
        for folder, _, names in os.walk(self.saves):
            for name in names:
                path = os.path.join(folder, name)
                found[os.path.relpath(path, self.saves)] = (os.path.getsize(path), os.path.getmtime(path))
        return found

    def stop(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait()
        self.log.close()


def copy_console(source, target, nand=None):
    """A console folder of its own: copied storage and settings, the rest linked."""
    os.makedirs(target)
    shutil.copytree(nand or os.path.join(source, "nand"), os.path.join(target, "nand"), symlinks=True)
    for name in os.listdir(source):
        if name == "nand":
            continue
        path = os.path.join(source, name)
        if os.path.isdir(path):
            os.symlink(os.path.abspath(path), os.path.join(target, name))
        else:
            shutil.copy2(path, os.path.join(target, name))
    return target


def screen_reader():
    """The text reader, built from screen_text.swift the first time. macOS only for now."""
    tool = os.path.join(ROOT, "recomp/build/tools/screen_text")
    source = os.path.join(HERE, "screen_text.swift")
    if not os.path.exists(tool) or os.path.getmtime(tool) < os.path.getmtime(source):
        if sys.platform != "darwin":
            raise StepFailed("reading the screen needs macOS's text recognition")
        os.makedirs(os.path.dirname(tool), exist_ok=True)
        subprocess.run(["swiftc", "-O", source, "-o", tool], check=True)
    return tool


def game_id_of(disc):
    """The six-letter game ID at the start of an extracted disc's boot.bin, such as RFPE01."""
    for path in (os.path.join(disc, "boot.bin"), os.path.join(disc, "sys", "boot.bin")):
        if os.path.exists(path):
            with open(path, "rb") as boot:
                return boot.read(6).decode("ascii", "replace")
    return None


def find_session(name, game_id):
    if name.endswith(".py"):
        return name
    places = ([os.path.join(HERE, "games", game_id)] if game_id else []) + [os.path.join(HERE, "sessions")]
    for place in places:
        path = os.path.join(place, name + ".py")
        if os.path.exists(path):
            return path
    raise StepFailed(f"no session {name} in " + " or ".join(places))


def main():
    parser = argparse.ArgumentParser(usage=__doc__)
    parser.add_argument("session")
    parser.add_argument("--disc", default="extracted/sys", help="the extracted disc, or a disc image, to play")
    parser.add_argument("--game", help="its game ID, if it can't be read from the disc")
    parser.add_argument("--keep", help="keep the log, saves and pictures in this folder")
    parser.add_argument("--build", default=os.path.join(ROOT, "recomp/build"))
    parser.add_argument("--saves", help="start from this saves folder instead of the session's")
    parser.add_argument("--storage", default=os.environ.get("WII_RECOMP_NAND"),
                        help="a Wii storage folder; wii/nand/ when not given (see wii/README.md)")
    parser.add_argument("--env", action="append", default=[])
    parser.add_argument("--console", help="start this console folder from its Wii Menu")
    options = parser.parse_args()
    if options.console:
        game_id = options.game or MENU_TITLE
    else:
        game_id = options.game or game_id_of(os.path.join(ROOT, options.disc) if not os.path.isabs(options.disc) else options.disc)
    try:
        path = find_session(options.session, game_id)
    except StepFailed as failure:
        print(f"FAILED: {failure}")
        return 1
    sys.path[:0] = [os.path.dirname(path), HERE]  # Sessions can share helpers: their game's, and player.py.
    spec = importlib.util.spec_from_file_location("session", path)
    session = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(session)

    folder = options.keep or tempfile.mkdtemp(prefix="autoplay-")
    if options.keep:
        shutil.rmtree(folder, ignore_errors=True)
        os.makedirs(folder)
    game = None
    try:
        named = getattr(session, "SAVES", None)
        if options.console:
            starting = options.saves or (os.path.join(options.build, "session-saves", game_id, named) if named else None)
            if starting and not os.path.isdir(starting):
                raise StepFailed(f"no starting storage at {starting}: run the session that makes it first")
            game = Game(options.build, folder, starting, None, game_id, env=options.env, console=options.console)
            session.run(game)
            print(f"PASSED in {game.seconds():.0f} s")
            return 0
        starting = options.saves or (os.path.join(options.build, "session-saves", game_id or "unknown", named) if named
                                     else os.path.join(options.build, "saves"))
        if named and not options.saves and not os.path.isdir(starting):
            raise StepFailed(f"no starting saves at {starting}: run the session that makes them first")
        if not os.path.isdir(starting):
            os.makedirs(os.path.join(folder, "empty-saves"))
            starting = os.path.join(folder, "empty-saves")  # A first run starts with no saves at all.
        game = Game(options.build, folder, starting, options.disc, game_id, options.storage, options.env)
        session.run(game)
        print(f"PASSED in {game.seconds():.0f} s")
        return 0
    except StepFailed as failure:
        print(f"FAILED: {failure}")
        if game:
            print(f"the game's log is {game.log_path}")
        return 1
    finally:
        if game and os.environ.get("AUTOPLAY_LEAVE_RUNNING") and game.process.poll() is None:
            print(f"left the game running, as pid {game.process.pid}, for a look inside (peek through {game.pipe})")
        elif game:
            game.stop()


if __name__ == "__main__":
    sys.exit(main())
