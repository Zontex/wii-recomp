"""From the first screen into the opening cutscenes without a save: English, Continue
without save, then A every few seconds, reading the screen and taking a picture each
time. The same presses at the same times each run, so two runs can be compared frame
time for frame time. SECONDS (150) sets how long."""
import os

SECONDS = float(os.environ.get("SECONDS", 150))


def run(game):
    game.wait(12)
    game.press("a")  # English.
    game.wait(5)
    game.read_screen("card")
    game.press("down", then=0.8)  # Continue without save.
    game.press("a")
    step = 0
    while game.seconds() < SECONDS:
        game.wait(5)
        game.read_screen(f"step-{step:02d}")
        game.press("a")
        step += 1
