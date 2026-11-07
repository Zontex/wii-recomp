"""Sets the console up for its own online services, as a person would in Wii Settings:
WiiConnect24's standby connection on, the connection test, and the Wii Network Services
Agreement accepted. Keeps the storage as "online", for the Shop's sessions.

    python3 recomp/tools/autoplay/autoplay.py online --console <console folder>
"""
import os
import shutil
import time

from menu import WII_BUTTON, Menu

SAVES = "after-setup"


def run(game):
    menu = Menu(game)
    for attempt in range(3):
        # The first time, WiiConnect24 asks for the agreement, whose program goes back to
        # the Wii Menu once it is accepted; the way in is then taken again.
        menu.to_grid()
        menu.click(*WII_BUTTON, then=3)
        menu.choose("Wii Settings")
        menu.choose("WiiConnect24")
        if past_questions(game, menu):
            break
    else:
        game.fail("WiiConnect24's settings never showed")
    menu.choose("Standby Connection")
    turn_on(game, menu)
    menu.choose("Confirm")
    menu.until(20, "a confirmation", lambda text, screen: True)
    for _ in range(4):
        text, screen = menu.look()
        game.note("reads: " + text)
        menu.react(text, screen)
    target = os.path.join(game.build, "session-saves", game.game_id, "online")
    shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(game.saves, target)
    game.note(f"kept the storage as {target}")


def past_questions(game, menu, seconds=180):
    """Answers what WiiConnect24 asks, the agreement among it, until its settings show
    (true) or the Wii Menu comes back (false)."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        text, screen = menu.look()
        game.note("reads: " + text)
        if "Standby Connection" in text:
            return True
        if menu.at_grid(text):
            return False
        menu.react(text, screen)
        time.sleep(1)
    game.fail("WiiConnect24 kept asking")


def turn_on(game, menu, seconds=20):
    """Picks "On", which the text reader misses (two letters alone on a button). It sits
    right above "Off", a button's height and gap up: 190 of the picture's 912 lines."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        text, screen = menu.look()
        off = menu.find(screen, "Off")
        if off:
            game.note("choosing 'On', above 'Off'")
            menu.click(off["x"], off["y"] - 2 * 190 / 912, then=2.5)
            return
        time.sleep(1)
    game.fail("never found 'Off', under 'On'")
