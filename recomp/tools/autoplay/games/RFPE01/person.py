"""Plays Wii Fit Plus (RFPE01) like a person who reads the screen: the generic player
(player.py), plus what this game asks for. It steps on and off the Balance Board when
told, stamps today on the calendar, picks the first Mii, and declines a password.
"""
import re
import time

from player import BUTTONS, Player

FIRST_MII = (-0.6, -0.3)  # The first face on "Choose a Mii", which has no text to click.
TODAY = (0.0, 0.0)  # Today's box on "Stamp Today's Date", which the pointer must be on.


class Person(Player):
    buttons = BUTTONS + ["Light"]

    def __init__(self, game, kilograms=60):
        super().__init__(game)
        self.kilograms = kilograms
        self.standing = False

    def stand(self, on):
        if on != self.standing:
            self.game.board(on, self.kilograms)
            self.standing = on

    def react(self, text, screen):
        game = self.game
        words = {item["text"]: item for item in screen}
        if self.stuck(screen):
            return
        if re.search(r"to stamp", text, re.I):
            game.click(*TODAY)
        elif re.search(r"password", text, re.I) and "No" in words:
            game.click(words["No"]["x"], words["No"]["y"])
        elif re.search(r"step off", text, re.I):
            if self.standing:
                self.stand(False)
                time.sleep(1.5)
            game.press("a")
        elif re.search(r"don.t step on", text, re.I):
            self.stand(False)
        elif re.search(r"step on|stand on|get on", text, re.I):
            self.stand(True)
        elif "Choose a Mii" in text:
            game.click(*FIRST_MII)
        else:
            super().react(text, screen)
