"""Mario Kart Wii (RMCE01): drives from the strap screen to a 50cc Grand Prix race on Luigi
Circuit, pointing at what it wants with the remote, holds accelerate, and keeps racing for RACE_SECONDS
(60). It goes by the text on each screen, so a slow load doesn't throw it off. For timing,
recomp/tools/race_bench.py runs it and reads the log from where the race begins, which
this saves as race.json in the run's folder.

    python3 recomp/tools/autoplay/autoplay.py race --disc games/RMCE01.wrd --game RMCE01 --keep <folder>

RACE_TIMELINE=<frames> asks for a timeline of that many frames 10 s into the race, and
RACE_RECORD=<frames> records that many frames for recomp/tools/replay, written to the
run's folder. Recording changes how copies are read back for the rest of the run, so
don't time a run that records.
"""
import json
import os
import re
import time

from player import Player

LIMIT = 240  # Seconds to reach the race. It took about 75 when this was written.
RACE_SECONDS = float(os.environ.get("RACE_SECONDS", 60))
RACE_DRAWS = 8000  # Draws a frame: the menus draw up to about 3,400, a race 13,000 to 15,000.

# On the way: when a screen shows the first text, point at the second and press A. Other
# screens (the strap, the title, the license, the character and the kart) take A on
# what's already chosen.
CLICKS = [
    ("Single Player", "Single Player"),
    ("Grand Prix", "Grand Prix"),
    ("Engine Classes", "50cc"),
    ("Automatic", "Automatic"),
    ("Mushroom Cup", "Mushroom Cup"),
    ("Start Race", "OK"),
]


def find(screen, words):
    """The piece of text that is, or starts, the words: the reader can cut a word short."""
    wanted = words.lower()
    for item in screen:
        text = item["text"].lower()
        if text.startswith(wanted) or (len(text) >= 6 and wanted.startswith(text)):
            return item
    return None


def draws_per_frame(line):
    found = re.search(r"^second \d+: (\d+) draws, \d+ copies, (\d+) to screen", line)
    return int(found.group(1)) / max(1, int(found.group(2))) if found else 0


def racing(game):
    """The last two seconds drew like a race."""
    lines = [line for line in game.text().splitlines() if line.startswith("second ")][-2:]
    return len(lines) == 2 and all(draws_per_frame(line) >= RACE_DRAWS for line in lines)


def run(game):
    # A new console builds its shaders before the first picture, which can take a while.
    game.wait_for(r"^second \d+: \d+ draws, \d+ copies, [1-9]\d* to screen", LIMIT, "the first picture")
    player = Player(game)
    seen = []
    while not racing(game):
        if game.seconds() > LIMIT:
            game.fail(f"no race after {LIMIT} s; screens seen: {', '.join(seen) or 'none'}")
        _, screen = player.look()
        target = next((find(screen, words) for shown, words in CLICKS if find(screen, shown)), None)
        if target:
            seen.append(target["text"])
            game.click(target["x"], target["y"], then=2.0)
        else:
            game.press("a", then=2.0)
    game.send("hold a")
    started = len(game.text())
    game.note(f"racing, after: {', '.join(seen)}")
    with open(os.path.join(game.folder, "race.json"), "w") as out:
        json.dump({"log_offset": started, "screens": seen, "seconds_to_race": round(game.seconds(), 1)}, out)
    frames = int(os.environ.get("RACE_TIMELINE", 0))
    record = int(os.environ.get("RACE_RECORD", 0))
    if frames or record:
        game.wait(10)
        if frames:
            game.send(f"timeline {frames} {os.path.join(game.folder, 'race-timeline.json')}")
        if record:
            game.send(f"record {record} {os.path.join(game.folder, 'race.dff')}")
    end = time.time() + RACE_SECONDS - (10 if frames or record else 0)
    while time.time() < end:
        game.wait(min(5, max(0, end - time.time())))
    game.send("release a")
