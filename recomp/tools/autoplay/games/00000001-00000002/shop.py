"""The Wii Shop Channel from the Wii Menu, on the console's own server: open it from the
grid, Start Shopping, Virtual Console, Popular Titles, the first title's details, then
Download, which must be refused, and back to the Wii Menu with the Shop's own button.
Needs the "online" storage (online.py) and the Shop's pictures and catalogue
(recomp/tools/shop/assets.py).

    python3 recomp/tools/autoplay/autoplay.py shop --console <console folder>
"""
from menu import Menu

SAVES = "online"
SHOP = "00010002-48414241"
# Places on the Shop's 608 by 456 page, which fills the screen, as the screen's -1 to 1.
FIRST_ROW = (280 / 304 - 1, 133 / 228 - 1)
BACK_TO_START = (503 / 304 - 1, 50 / 228 - 1)  # The triangle at the top, to the main menu.


def run(game):
    menu = Menu(game)
    menu.to_grid()
    _, screen = menu.until(30, "the Shop's square", lambda text, screen: menu.find(screen, "Wii Shop Channel"), react=False)
    shop = menu.find(screen, "Wii Shop Channel")  # Squares move, so their names aren't read every time.
    menu.click(shop["x"], shop["y"], then=3)
    menu.choose("Start", pages=False)
    game.wait_for(rf"^console: starting {SHOP}", 60, "the Shop starting")
    menu.until(120, "the Shop's welcome page", lambda text, screen: "Start Shopping" in text, react=False)
    menu.choose("Start Shopping", pages=False)
    menu.until(30, "the Shop's main menu", lambda text, screen: "WiiWare" in text, react=False)
    menu.choose("Virtual Console", pages=False)
    menu.until(30, "Virtual Console", lambda text, screen: "Popular Titles" in text, react=False)
    menu.choose("Popular Titles", pages=False)
    menu.until(30, "a list of titles", lambda text, screen: "Wii Points" in text and "Popular" in text, react=False)
    menu.click(*FIRST_ROW, then=3)
    text, _ = menu.until(30, "a title's details", lambda text, screen: "Download" in text, react=False)
    game.note("details: " + text)
    menu.choose("Download", pages=False)
    menu.until(30, "the download refused", lambda text, screen: "downloaded" in text, react=False)
    menu.click(*BACK_TO_START, then=3)
    menu.until(30, "the Shop's main menu again", lambda text, screen: "WiiWare" in text, react=False)
    menu.choose("Wii Menu", pages=False)
    game.wait_for(r"^console: starting the Wii Menu", 60, "the way back to the Wii Menu")
    menu.to_grid()
    game.note("browsed the Shop, Download refused, and back at the grid")
