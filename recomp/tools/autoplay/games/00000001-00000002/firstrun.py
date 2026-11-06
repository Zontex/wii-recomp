"""The Wii Menu's first start on a new console: the health warning and first-time setup,
until the channel grid. Keeps the storage as the "after-setup" starting point other
sessions name with SAVES = "after-setup".

    python3 recomp/tools/autoplay/autoplay.py firstrun --console <console folder>
"""
import os
import shutil

from menu import Menu


def run(game):
    menu = Menu(game)
    menu.to_grid(seconds=600)
    game.wait(3)  # The menu writes its settings as the grid appears.
    target = os.path.join(game.build, "session-saves", game.game_id, "after-setup")
    shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(game.saves, target)
    game.note(f"kept the storage as {target}")
