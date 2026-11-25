"""What kind of disc an extraction is, and the title the console files it under.

A disc's ID, the first bytes of boot.bin, says which it is: a Wii disc has its mark at
0x18, a GameCube disc at 0x1c. A Wii disc (or a channel written by prepare_title.py)
brings its title ID and the IOS it runs on in tmd.bin. A GameCube disc has no TMD: a
Wii runs it in GameCube mode, without IOS. The console files its data under 00010000
and the first four letters of its game ID, as a Wii disc of that ID would be.
"""
import struct
from pathlib import Path

WII_MAGIC_AT, WII_MAGIC = 0x18, 0x5d1c9ea3
GAMECUBE_MAGIC_AT, GAMECUBE_MAGIC = 0x1c, 0xc2339f3d
TITLE_ID_OFFSET = 0x18c  # In a Wii disc's tmd.bin.
IOS_OFFSET = 0x184  # The IOS the title asks for, in its TMD; the low word is its number.
GAMECUBE_TITLE_HIGH = 0x00010000


def word(data, at):
    return struct.unpack_from(">I", data, at)[0]


def is_gamecube(boot):
    """Whether boot.bin, the disc's first 0x440 bytes, is a GameCube disc's."""
    return len(boot) >= GAMECUBE_MAGIC_AT + 4 and word(boot, GAMECUBE_MAGIC_AT) == GAMECUBE_MAGIC


def is_gamecube_folder(extracted):
    boot = Path(extracted) / "sys/boot.bin"
    return boot.is_file() and is_gamecube(boot.read_bytes())


def title(extracted):
    """(title ID high word, low word, IOS) for an extracted disc or channel."""
    extracted = Path(extracted)
    if (extracted / "tmd.bin").is_file():
        tmd = (extracted / "tmd.bin").read_bytes()
        return word(tmd, TITLE_ID_OFFSET), word(tmd, TITLE_ID_OFFSET + 4), word(tmd, IOS_OFFSET + 4)
    if is_gamecube_folder(extracted):
        return GAMECUBE_TITLE_HIGH, word((extracted / "sys/boot.bin").read_bytes(), 0), 0
    raise SystemExit(f"{extracted} has no tmd.bin, and isn't a GameCube disc")
