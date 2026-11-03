#!/usr/bin/env python3
"""Set up a console folder: the Wii's storage, the Wii Menu's native code and a drive slot.

The storage is a private, writable copy: the menu reads and writes it, and deletes really
delete. The source folder is only read. An existing copy is left alone unless --replace
is given. With --menu, the Wii Menu built in that folder (by prepare_title.py, emit.py,
build_file.py and ninja) is installed as the console's menu, replacing any before it.

Usage: recomp/tools/prepare_console.py <Wii storage folder> <console folder> [--replace] [--menu recomp/build-menu]
Then: <build>/wii-recomp --console <console folder>, with a disc image from make_disc.py
in <console folder>/drive for a disc in the drive.
"""
import argparse
import shutil
import sys
from pathlib import Path


MENU = "00000001-00000002"
MENU_FILES = ["main.dol", "boot.bin", "fst.bin"]
LIBRARY = "title.dylib" if sys.platform == "darwin" else "title.so"


def install_menu(build, console):
    """The menu's program files and its native code, swapped in whole."""
    sources = [build / "disc/sys" / name for name in MENU_FILES] + [build / LIBRARY]
    missing = [str(path) for path in sources if not path.is_file()]
    if missing:
        raise SystemExit("the menu build is missing " + ", ".join(missing))
    target = console / "titles" / MENU
    partial = target.with_name(MENU + ".partial")
    shutil.rmtree(partial, ignore_errors=True)
    partial.mkdir(parents=True)
    for path in sources:
        shutil.copy2(path, partial / path.name)
    shutil.rmtree(target, ignore_errors=True)
    partial.rename(target)
    print(f"installed the Wii Menu from {build}")


def copy_storage(source, console, replace):
    nand = console / "nand"
    if not (source / "title").is_dir():
        raise SystemExit(f"{source} has no title folder, so it isn't Wii storage")
    if nand == source or source in nand.parents or nand in source.parents:
        raise SystemExit("The copy can't be inside the source, or the source inside the copy")
    if nand.exists():
        if not replace:
            raise SystemExit(f"{nand} already exists; pass --replace to start over")
        shutil.rmtree(nand)
    console.mkdir(parents=True, exist_ok=True)
    partial = console / "nand.partial"
    shutil.rmtree(partial, ignore_errors=True)
    shutil.copytree(source, partial, symlinks=False)
    partial.rename(nand)  # Only a finished copy gets the real name.
    files = sum(1 for path in nand.rglob("*") if path.is_file())
    print(f"copied {files} files to {nand}")


def main():
    parser = argparse.ArgumentParser(usage=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("console", type=Path)
    parser.add_argument("--replace", action="store_true")
    parser.add_argument("--menu", type=Path)
    options = parser.parse_args()
    source, console = options.source.resolve(), options.console.resolve()
    if not (console / "nand").exists() or options.replace:
        copy_storage(source, console, options.replace)
    elif not options.menu:
        raise SystemExit(f"{console / 'nand'} already exists; pass --replace to start over")
    (console / "drive").mkdir(exist_ok=True)
    if options.menu:
        install_menu(options.menu.resolve(), console)


if __name__ == "__main__":
    main()
