"""Plays like a person who reads the screen, for finding out where a game goes. Prints
what it reads and does, and checks nothing. EXPLORE_SECONDS sets how long (900)."""
import os
import time

from player import Player

UNTIL = float(os.environ.get("EXPLORE_SECONDS", 900))


def run(game):
    player = Player(game)
    while game.seconds() < UNTIL:
        text, screen = player.look()
        player.react(text, screen)
        time.sleep(1.5)
