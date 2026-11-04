"""Saves a picture every few seconds with no input, for seeing where a game goes on its
own. LOOK_SECONDS (60) and LOOK_EVERY (5) set how long and how often."""
import os

SECONDS = float(os.environ.get("LOOK_SECONDS", 60))
EVERY = float(os.environ.get("LOOK_EVERY", 5))


def run(game):
    while game.seconds() < SECONDS:
        game.shot(f"t{int(game.seconds()):03}")
        game.wait(EVERY)
