"""Every channel on the grid's first page, in turn: open its square, Start, the console
starting the channel natively, then back with Home, Wii Menu, Yes. A square with no
Start is empty, and skipped. Channels the console has no native code for are refused
by the console, which the session reports rather than fails on.

    python3 recomp/tools/autoplay/autoplay.py channels --console <console folder>
"""

from menu import Menu, square

SAVES = "after-setup"


def run(game):
    menu = Menu(game)
    menu.to_grid()
    started, refused = [], []
    for index in range(1, 12):  # The disc channel is square 0.
        menu.click(*square(index), then=3)
        text, screen = menu.look()
        if not menu.find(screen, "Start"):
            game.note(f"square {index} has no channel")
            menu.press_back_to_grid()
            continue
        menu.click_words(screen, "Start")
        found = game.wait_for(r"^(console: starting (\w+-\w+)|gap: ios.launch_untranslated_title .*)$", 60,
                              f"square {index}'s channel starting")
        if found.group(2):
            started.append(found.group(2))
            game.wait_for(r"^second \d+:", 60, "the channel drawing")
            game.wait(8)
            menu.leave_to_menu()
        else:
            refused.append(index)
        menu.to_grid()
    game.note(f"started {', '.join(started) or 'none'}; refused on squares {refused or 'none'}")
    if not started:
        game.fail("no channel started")
