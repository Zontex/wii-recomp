#!/usr/bin/env python3
"""Gathers what the console's Wii Shop Channel pages show, into wii/shop/, which is never
committed: Nintendo's own pictures and style sheet for the Shop's pages, the catalogue of
titles the Shop offered, and each title's picture.

    python3 recomp/tools/shop/assets.py <WiiMart folder> [--region US] [--no-thumbnails]

The pages themselves are ours (pages.py); the pictures, the style sheet and the catalogue
are Nintendo's, so they stay on this computer. They come from a copy of the WiiMart
project (github.com/WiiMart/WiiMart), which keeps the Shop's server files: only its
www/oss/oss/common folder is needed, for instance

    git clone --depth 1 --filter=blob:none --sparse https://github.com/WiiMart/WiiMart.git
    git -C WiiMart sparse-checkout set www/oss/oss/common

The catalogue keeps one region's titles (the console's own is US unless its settings say
otherwise) with what the pages show: names, platform, publisher, genre, date, price in
Wii Points, players, rating and controllers. Download links in WiiMart's list are never
kept: nothing here can be downloaded. Each title's picture is fetched once, here, from
Nintendo's content server, which still has them; the console's server never goes online.
"""
import argparse
import html
import json
import os
import re
import shutil
import sys
import urllib.request
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
TARGET = os.path.join(ROOT, "wii", "shop")
THUMBNAILS = "http://ccs.cdn.shop.wii.com/ccs/download/{title}/{content}"
KEPT = ["id", "title1", "title2", "console", "publisher", "genre", "date", "points", "players", "rating",
        "ratingdetails", "thumbnail"]


def controllers_text(markup):
    """The controllers a title takes, from the catalogue's markup: picture names and lines."""
    pictures = re.findall(r'banner/(B_08_[A-Za-z0-9_]+\.gif)', markup or "")
    lines = [html.unescape(re.sub(r"<[^>]+>", "", line)).strip()
             for line in re.findall(r'class="contentsBlackS">(.*?)</div>', markup or "", re.S)]
    return {"pictures": pictures, "lines": [line for line in lines if line]}


def catalogue(games, region):
    """The titles of one region, with only what the pages show."""
    titles = []
    for game in games:
        if game.get("region") != region or not re.fullmatch(r"[0-9A-Fa-f]{16}", game.get("id", "")):
            continue
        title = {key: game.get(key, "") for key in KEPT}
        title["id"] = title["id"].upper()
        title["controllers"] = controllers_text(game.get("controllers"))
        titles.append(title)
    return titles


def copy_pictures(common):
    """Nintendo's pictures and style sheet for the pages."""
    for name in ("images", "css"):
        source, target = os.path.join(common, name), os.path.join(TARGET, "oss", "common", name)
        if not os.path.isdir(source):
            raise SystemExit(f"assets: {source} isn't there; is this WiiMart's www/oss/oss/common?")
        shutil.rmtree(target, ignore_errors=True)
        shutil.copytree(source, target)


def fetch_thumbnail(title):
    path = os.path.join(TARGET, "thumbnails", f"{title['id']}_{title['thumbnail']}.gif")
    if os.path.isfile(path) or not re.fullmatch(r"[0-9A-Fa-f]{8}", title["thumbnail"] or ""):
        return True
    try:
        with urllib.request.urlopen(THUMBNAILS.format(title=title["id"], content=title["thumbnail"]), timeout=30) as reply:
            data = reply.read()
    except OSError:
        return False
    if not data.startswith((b"GIF8", b"\x89PNG", b"\xff\xd8")):
        return False
    with open(path + ".part", "wb") as out:
        out.write(data)
    os.replace(path + ".part", path)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("wiimart", help="a copy of WiiMart, or its www/oss/oss/common folder")
    parser.add_argument("--region", default="US", help="the catalogue's region: US, GB, JP, DE, FR...")
    parser.add_argument("--no-thumbnails", action="store_true", help="don't fetch the titles' pictures")
    options = parser.parse_args()
    common = options.wiimart
    if os.path.isdir(os.path.join(common, "www/oss/oss/common")):
        common = os.path.join(common, "www/oss/oss/common")
    with open(os.path.join(common, "js", "games.json"), encoding="utf-8") as source:
        titles = catalogue(json.load(source), options.region)
    if not titles:
        raise SystemExit(f"assets: no titles for region {options.region}")
    os.makedirs(os.path.join(TARGET, "thumbnails"), exist_ok=True)
    copy_pictures(common)
    with open(os.path.join(TARGET, "catalogue.json"), "w", encoding="utf-8") as out:
        json.dump({"region": options.region, "titles": titles}, out, ensure_ascii=False, indent=0)
    print(f"assets: {len(titles)} titles for {options.region}, pictures in {TARGET}", file=sys.stderr)
    if not options.no_thumbnails:
        with ThreadPoolExecutor(8) as pool:
            pictures = list({(title["id"], title["thumbnail"]): title for title in titles}.values())
            fetched = sum(pool.map(fetch_thumbnail, pictures))
        print(f"assets: {fetched} of {len(pictures)} titles' pictures", file=sys.stderr)


if __name__ == "__main__":
    main()
