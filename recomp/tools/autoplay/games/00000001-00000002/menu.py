"""Getting around the Wii Menu, for its sessions: reading which screen it's on, and the
trips between the grid, the disc channel, a game and back."""
import re
import time

from player import BUTTONS, Player

# Where the menu's hand touches for a pointer position, measured on the grid: across as
# the pointer, and up and down 1.2 times as far, a little above. Clicks take a place on
# the screen and aim the pointer so the hand lands there.


def aim(x, y):
    return x / 0.98, (y + 0.13) / 1.2
GRID_DISC = (-0.64, -0.67)  # The disc channel's square, top left of the first page.
WII_BUTTON = (-0.77, 0.68)  # Under the grid, on the left: Wii Options.
SETTINGS_NEXT = (0.82, -0.06)  # The arrow to Wii Settings' next page, on the right.
# The grid always shows the date along the bottom, as "Fri 11/6" or "Fri 06/11".
DATE = re.compile(r"\b\d{1,2}/\d{1,2}\b")  # The weekday before it isn't always read.
# Once its start-up is over, the grid shows the time over the date, and only then takes input.
TIME = re.compile(r"\b\d{1,2}[: ]?\d{2}\b")  # Text recognition sometimes drops the colon.
# Words a game's Home Menu shows, in English.
HOME_MENU = "Wii Menu"


# The grid's first page: four columns and three rows of squares, read across.
COLUMNS, ROWS = (-0.635, -0.21, 0.21, 0.635), (-0.64, -0.21, 0.21)


def square(index):
    """Where to point for the grid square at index, 0 being the disc channel."""
    return COLUMNS[index % 4], ROWS[index // 4]


# Where the pointer waits while the screen is read: the bar under the grid, between the
# clock and the Wii button, where no channel lies under the hand's tip.
PARKED = (-0.35, 0.68)


class Menu(Player):
    # The Wii Network Services Agreement asks to accept, beside a button not to.
    buttons = ["I ACCEPT"] + BUTTONS

    def look(self):
        self.game.point(*aim(*PARKED), then=0.4)
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
        aim_x, aim_y = aim(x, y)
        self.game.point(aim_x + 0.02, aim_y, then=0.2)
        self.game.point(aim_x, aim_y, then=1.0)
        self.game.send("hold a")
        time.sleep(0.6)
        self.game.send("release a")
        time.sleep(then)

    def at_grid(self, text):
        return bool(DATE.search(text)) and "Start" not in text and "Wii Menu" not in text

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
        """From starting up: past the health warning and anything asking to press on. The
        grid takes input once its clock shows, a moment after the date."""
        found = self.until(seconds, "the channel grid", lambda text, screen: self.at_grid(text))
        self.game.wait(4)
        return found

    def choose(self, words, seconds=20, pages=True):
        """Finds words on screen, turning the settings' pages (1 2 3) if they aren't there,
        and clicks them. Returns the screen it clicked on."""
        deadline = time.time() + seconds
        page = 0
        while time.time() < deadline:
            text, screen = self.look()
            item = next((i for i in screen if i["text"] == words), None) or next((i for i in screen if i["text"].startswith(words)), None)
            if item:
                self.game.note(f"choosing {item['text']!r}")
                self.click(item["x"], item["y"], then=2.5)
                return screen
            if pages and "Settings" in text:
                page += 1
                self.click(*SETTINGS_NEXT, then=2)  # The next page; after the last, the first again.
            else:
                time.sleep(1)
        self.game.fail(f"never found {words!r}")

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

    def leave_to_menu(self, seconds=90):
        """In a channel: its own Wii Menu button when its screen has one, as an error or an
        agreement does, otherwise Home, Wii Menu, Yes. Until the console starts the menu."""
        text, screen = self.look()
        own = next((item for item in screen if item["text"] in ("Wii Menu", "Back to the")), None)
        if own and not self.find(screen, "HOME Menu"):
            self.click(own["x"], own["y"], then=2)
            if self.game.wait_for(r"^console: starting the Wii Menu", 20, "the hand-over to the Wii Menu", fail=False):
                return
        self.home_to_menu(seconds)

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
