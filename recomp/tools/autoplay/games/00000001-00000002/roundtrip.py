"""From the grid to the disc in the drive and back, ROUNDS times (1): the disc channel's
Start, the game running natively, then its Home Menu's Wii Menu, Yes, and the grid again.
Saves the game writes on the way must still be there afterwards.

    ROUNDS=20 python3 recomp/tools/autoplay/autoplay.py roundtrip --console <console folder>
"""
import os

from menu import Menu

SAVES = "after-setup"
ROUNDS = int(os.environ.get("ROUNDS", 1))
GAME_SECONDS = float(os.environ.get("GAME_SECONDS", 20))  # How long to let the game run each time.


def run(game):
    menu = Menu(game)
    menu.to_grid()
    for round in range(1, ROUNDS + 1):
        game.note(f"round {round} of {ROUNDS}")
        menu.start_disc()
        game.wait_for(r"^second \d+:", 60, "the game drawing")
        game.wait(GAME_SECONDS)
        menu.home_to_menu()
        menu.to_grid()
