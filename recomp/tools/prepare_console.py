#!/usr/bin/env python3
"""Make a private, writable copy of a Wii storage folder for the native console.

The copy is the console's own storage from then on: the menu reads and writes it, and
deletes really delete. The source folder is only read. Run once; an existing copy is
left alone unless --replace is given.

Usage: recomp/tools/prepare_console.py <Wii storage folder> <console folder> [--replace]
Then run with WII_RECOMP_SAVES=<console folder>/nand and no storage folder argument.
"""
import shutil
import sys
from pathlib import Path


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    source, console = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    nand = console / "nand"
    if not (source / "title").is_dir():
        raise SystemExit(f"{source} has no title folder, so it isn't Wii storage")
    if nand == source or source in nand.parents or nand in source.parents:
        raise SystemExit("The copy can't be inside the source, or the source inside the copy")
    if nand.exists():
        if "--replace" not in sys.argv:
            raise SystemExit(f"{nand} already exists; pass --replace to start over")
        shutil.rmtree(nand)
    console.mkdir(parents=True, exist_ok=True)
    partial = console / "nand.partial"
    shutil.rmtree(partial, ignore_errors=True)
    shutil.copytree(source, partial, symlinks=False)
    partial.rename(nand)  # Only a finished copy gets the real name.
    files = sum(1 for path in nand.rglob("*") if path.is_file())
    print(f"copied {files} files to {nand}")


if __name__ == "__main__":
    main()
