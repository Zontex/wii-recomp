"""Plays like a person who reads the screen, for any game. Sessions build on it.

look() reads what the screen says. react() does one thing about it: clicks the button a
person would pick to keep going, or presses A. When a screen hasn't changed for a while
it tries each piece of text on it in turn, as someone unsure would. A game's own player
(games/<game ID>/) adds what's particular to it, such as stepping on the board.
"""
import re

# Buttons a person picks to keep going, best first.
BUTTONS = ["OK", "Correct", "Yes", "Next", "Start", "Begin", "Confirm", "Done", "Continue", "Save"]
STUCK_AFTER = 3  # Reads of an unchanged screen before trying each thing on it.
PARKED = (0.97, -0.97)  # Where the cursor waits while reading, so it covers no words.


class Player:
    buttons = BUTTONS

    def __init__(self, game):
        self.game = game
        self.last_text = None
        self.same = 0
        self.tried = 0

    def look(self):
        """Reads the screen. Returns the text, joined, and the pieces with where they are."""
        self.game.point(*PARKED, then=0.4)
        screen = self.game.read_screen("s")
        text = " | ".join(item["text"] for item in screen)
        self.same = self.same + 1 if text == self.last_text else 0
        self.last_text = text
        return text, screen

    def stuck(self, screen):
        """Tries the next thing on a screen that isn't changing. Returns whether it did."""
        if self.same < STUCK_AFTER or not screen:
            return False
        item = screen[self.tried % len(screen)]
        self.tried += 1
        self.game.note(f"stuck, trying {item['text']!r}")
        self.game.click(item["x"], item["y"])
        return True

    def react(self, text, screen):
        if self.stuck(screen):
            return
        words = {item["text"]: item for item in screen}
        button = next((words[b] for b in self.buttons if b in words), None)
        if button:
            self.game.click(button["x"], button["y"])
        else:
            self.game.press("a")
