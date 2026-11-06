"""Getting around the Wii Menu, for its sessions: reading which screen it's on, and the
trips between the grid, the disc channel, a game and back."""
import re
import time

from player import Player

GRID_DISC = (-0.64, -0.40)  # The disc channel's square, top left of the first page (the hand's tip is above the pointer).
# The grid always shows the date along the bottom, as "Fri 11/6" or "Fri 06/11".
DATE = re.compile(r"\b(Mon|Tue|Wed|Thu|Fri|Sat|Sun)\b\.? ?\d{1,2}/\d{1,2}")
# Once its start-up is over, the grid shows the time over the date, and only then takes input.
TIME = re.compile(r"\b\d{1,2}[: ]?\d{2}\b")  # Text recognition sometimes drops the colon.
# Words a game's Home Menu shows, in English.
HOME_MENU = "Wii Menu"


# The grid's first page: four columns and three rows of squares, read across.
COLUMNS, ROWS = (-0.635, -0.21, 0.21, 0.635), (-0.40, 0.03, 0.46)


def square(index):
    """Where to point for the grid square at index, 0 being the disc channel."""
    return COLUMNS[index % 4], ROWS[index // 4]


# Where the pointer waits while the screen is read: the bar under the grid, between the
# clock and the Wii button, where no channel lies under the hand's tip.
PARKED = (-0.35, 0.95)


class Menu(Player):
    def look(self):
        self.game.point(*PARKED, then=0.4)
        screen = self.game.read_screen("s")
        text = " | ".join(item["text"] for item in screen)
        self.same = self.same + 1 if text == self.last_text else 0
        self.last_text = text
        return text, screen

    def react(self, text, screen):
        if not screen:  # Between screens: wait rather than press, so nothing opens by chance.
            return
        if self.stuck(screen):
            return
        words = {item["text"]: item for item in screen}
        button = next((words[b] for b in self.buttons if b in words), None)
        if button:
            self.click(button["x"], button["y"])
        else:
            self.click(*PARKED)  # A held press, which the menu takes, where nothing lies.

    def click(self, x, y, then=1.0):
        """A held press, which the menu sees reliably where a tap can be missed. The
        pointer moves onto the spot and rests, as the menu only picks what it hovers."""
        self.game.point(x + 0.02, y, then=0.2)
        self.game.point(x, y, then=1.0)
        self.game.send("hold a")
        time.sleep(0.6)
        self.game.send("release a")
        time.sleep(then)

    def at_grid(self, text):
        return bool(DATE.search(text)) and bool(TIME.search(text)) and "Start" not in text

    def find(self, screen, words):
        return next((item for item in screen if item["text"] == words), None)

    def until(self, seconds, what, done, react=True):
        """Reads the screen until done(text, screen) is true, pressing on if react."""
        deadline = time.time() + seconds
        while time.time() < deadline:
            text, screen = self.look()
            if done(text, screen):
                self.game.note(f"at {what}")
                return text, screen
            if react:
                self.react(text, screen)
            time.sleep(1.0)
        self.game.fail(f"never reached {what} in {seconds} s")

    def to_grid(self, seconds=120):
        """From starting up: past the health warning and anything asking to press on."""
        return self.until(seconds, "the channel grid", lambda text, screen: self.at_grid(text))

    def click_words(self, screen, words):
        item = self.find(screen, words)
        if not item:
            self.game.fail(f"no {words!r} on screen")
        self.click(item["x"], item["y"])

    def start_disc(self, seconds=90):
        """From the grid: opens the disc channel and starts the disc, until the console hands over."""
        for _ in range(4):
            self.click(*GRID_DISC, then=3)
            text, screen = self.look()
            if self.find(screen, "Start"):
                break
        else:
            self.game.fail("the disc channel never opened")
        self.click_words(screen, "Start")
        self.game.wait_for(r"^console: starting the disc", seconds, "the hand-over to the disc")

    def press_back_to_grid(self):
        """From a channel's preview: its Wii Menu button, back to the grid."""
        text, screen = self.look()
        if self.find(screen, "Wii Menu"):
            self.click_words(screen, "Wii Menu")
        self.game.wait(2)

    def home_to_menu(self, seconds=90):
        """In a game or channel: Home, Wii Menu, Yes, until the console starts the menu."""
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.game.press("home", then=2)
            text, screen = self.look()
            if self.find(screen, HOME_MENU):
                self.click_words(screen, HOME_MENU)
                time.sleep(1.5)
                _, screen = self.until(15, "the Home Menu's question", lambda text, screen: self.find(screen, "Yes"), react=False)
                self.click_words(screen, "Yes")
                self.game.wait_for(r"^console: starting the Wii Menu", seconds, "the hand-over to the Wii Menu")
                return
            self.react(text, screen)
        self.game.fail(f"no Home Menu with {HOME_MENU!r} in {seconds} s")
