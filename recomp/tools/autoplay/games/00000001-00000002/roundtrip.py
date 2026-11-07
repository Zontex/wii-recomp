"""From the grid to the disc in the drive and back, ROUNDS times (1): the disc channel's
Start, the game running natively, then its Home Menu's Wii Menu, Yes, and the grid again.
DISC=<image on the console's shelf> puts that disc in first, through the control pipe.

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
    if os.environ.get("DISC"):
        game.send(f"insert {os.environ['DISC']}")
        game.wait_for(r"^console: .* is in the drive", 30, "the disc going in")
        game.wait(5)  # The disc channel reads the new disc's banner.
    for round in range(1, ROUNDS + 1):
        game.note(f"round {round} of {ROUNDS}")
        menu.start_disc()
        game.wait_for(r"^second \d+:", 60, "the game drawing")
        game.wait(GAME_SECONDS)
        menu.home_to_menu()
        menu.to_grid()
