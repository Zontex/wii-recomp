#!/usr/bin/env python3
"""Extract a Wii disc image into extracted/, ready for translation.

Usage: recomp/tools/extract_disc.py "path/to/game.rvz" [--force] [--into FOLDER]

Reads ISO, RVZ and WBFS images with nodtool. It uses an installed nodtool, or downloads
a pinned version once and checks it. An existing folder is reused unless --force is given.
"""
import argparse
import hashlib
import platform
import shutil
import stat
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
TOOL_FOLDER = ROOT / ".tools"
MIN_FREE_BYTES = 3 * 1024 ** 3

NODTOOL_VERSION = "v2.0.0-alpha.12"
NODTOOL_URL = "https://github.com/encounter/nod/releases/download/{version}/{asset}"
NODTOOL_BUILDS = {
    ("Darwin", "x86_64"): ("nodtool-macos-x86_64",
                           "d1e7ba904c2e557fafbd3bf2d9b75a7901a2bf1568c0fb3a505fca0f30b0a833"),
    ("Darwin", "arm64"): ("nodtool-macos-arm64",
                          "524c0b27c0c4289e435df4474d288ea7e63d6a34814cc7c650231688dd1e53b4"),
    ("Linux", "x86_64"): ("nodtool-linux-x86_64",
                          "ee8345c004248eb82a86dd0c6068e0fcf3dc75baf68d18fe949bf4589822f5a5"),
    ("Linux", "aarch64"): ("nodtool-linux-aarch64",
                           "33e801c0e4aee7e714ad84e89521f50e2f79005571b9d4779b695e44ea21ff99"),
}


def fail(message):
    sys.exit(f"Extraction stopped: {message}")


def nodtool_build(system, machine):
    """Returns the (file name, sha256) to download for this computer."""
    machine = {"AMD64": "x86_64", "arm64": "arm64", "aarch64": "aarch64"}.get(machine, machine)
    if system == "Linux" and machine == "arm64":
        machine = "aarch64"
    if (system, machine) not in NODTOOL_BUILDS:
        fail(f"no nodtool download for {system} {machine}; install nodtool and retry")
    return NODTOOL_BUILDS[system, machine]


def check_hash(data, expected):
    actual = hashlib.sha256(data).hexdigest()
    if actual != expected:
        fail(f"nodtool download failed its checksum (got {actual})")


def find_nodtool():
    """Uses an installed nodtool, or downloads the pinned version once."""
    installed = shutil.which("nodtool")
    if installed:
        return installed
    tool = TOOL_FOLDER / "nodtool"
    if tool.exists():
        return str(tool)
    asset, expected = nodtool_build(platform.system(), platform.machine())
    url = NODTOOL_URL.format(version=NODTOOL_VERSION, asset=asset)
    print(f"Downloading {asset} {NODTOOL_VERSION}")
    with urllib.request.urlopen(url) as response:
        data = response.read()
    check_hash(data, expected)
    TOOL_FOLDER.mkdir(exist_ok=True)
    tool.write_bytes(data)
    tool.chmod(tool.stat().st_mode | stat.S_IXUSR)
    return str(tool)


def extract_disc(disc, into):
    unfinished = into.with_name(into.name + ".partial")
    into.parent.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(into.parent).free < MIN_FREE_BYTES:
        fail("at least 3 GB of free disk space is needed")
    nodtool = find_nodtool()
    shutil.rmtree(unfinished, ignore_errors=True)
    print("Extracting the disc and checking its data. This takes a few minutes.")
    subprocess.run([nodtool, "extract", "--validate", str(disc), str(unfinished)], check=True)
    # Renamed last, so an interrupted run can't be mistaken for a finished one.
    shutil.rmtree(into, ignore_errors=True)
    unfinished.rename(into)


def main():
    parser = argparse.ArgumentParser(description="Extract a Wii disc image for translation.")
    parser.add_argument("disc", type=Path, help="the .iso, .rvz or .wbfs disc image")
    parser.add_argument("--into", type=Path, default=ROOT / "extracted", help="where to extract (default: extracted/)")
    parser.add_argument("--force", action="store_true", help="extract again, replacing the folder")
    options = parser.parse_args()

    if not options.disc.is_file():
        fail(f"can't find {options.disc}")
    into = options.into.resolve()
    if into.exists() and not options.force:
        print(f"Using the existing {into}. Add --force to extract again.")
        return
    try:
        extract_disc(options.disc.resolve(), into)
    except subprocess.CalledProcessError:
        fail("nodtool failed; see the messages above")
    print(f"Done. The program to translate is {into / 'sys/main.dol'}")


if __name__ == "__main__":
    main()
