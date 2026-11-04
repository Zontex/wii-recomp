"""Wii Fit Plus (RFPE01): plays the first-time setup from blank saves, through the Body
Test, and passes once the game has written its save and reached the Wii Fit Plaza. The
saves are kept as the "after-setup" starting point other sessions can name with
SAVES = "after-setup", in <build>/session-saves/RFPE01/after-setup. About 15 minutes.

    python3 recomp/tools/autoplay/autoplay.py setup --saves <a folder with no Wii Fit data>
"""
import os
import shutil
import time

from person import Person

LIMIT = 1500  # Seconds. The setup took about 900 when this was written.
SAVE_FILE = "title/00010004/52465045/data/FitPlus0.dat"


def run(game):
    person = Person(game)
    before = game.saved_files().get(SAVE_FILE)
    while True:
        if game.seconds() > LIMIT:
            game.fail(f"no Wii Fit Plaza with a written save after {LIMIT} s")
        text, screen = person.look()
        if "Wii Fit Plaza" in text or "Select New Profile" in text:
            if game.saved_files().get(SAVE_FILE) == before:
                game.fail("the Plaza appeared, but the save wasn't written")
            break
        person.react(text, screen)
        time.sleep(1.5)
    target = os.path.join(game.build, "session-saves", game.game_id, "after-setup")
    shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(game.saves, target)
    game.note(f"kept the saves as {target}")
