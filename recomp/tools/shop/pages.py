"""The Wii Shop Channel's pages, as the console's own server answers them: to look round,
not to buy. The markup and scripts are ours. The page names, the places things sit on the
608 by 456 page, and the pictures and style sheets they use follow the Shop's own pages,
whose pictures and style sheets stay on this computer in wii/shop/ (assets.py), never in
the repository. Without them the pages still work, plainly drawn.

Each page talks to the channel through the objects it gives pages (wiiShop, wiiSound),
as documented by the Open Shop Channel (docs.oscwii.org/wii-shop-channel). Downloading
is refused on our own page: nothing here hands out titles.
"""
import html
import json
import re
import os
from urllib.parse import quote

IMAGES = "/oss/oss/common/images/"
PER_PAGE = 10
# The Virtual Console's systems, in the order the Shop listed them, by the names in the catalogue.
SYSTEMS = [("NES", ["NES"]), ("Super NES", ["Super NES"]), ("Nintendo 64", ["Nintendo 64"]),
           ("Sega Genesis", ["Sega Genesis", "SEGA Mega Drive"]), ("TurboGrafx16", ["TurboGrafx16", "Turbografx16"]),
           ("NEOGEO", ["NEOGEO"]), ("Master System", ["Master System", "MASTER SYSTEM"]),
           ("Commodore 64", ["Commodore 64"]), ("Virtual Console Arcade", ["Virtual Console Arcade"])]
VIRTUAL_CONSOLE = {name for _, names in SYSTEMS for name in names}
# What a Virtual Console game usually cost on each system, for titles the catalogue has
# no price for.
USUAL_POINTS = {"NES": 500, "Super NES": 800, "Nintendo 64": 1000, "Sega Genesis": 800, "TurboGrafx16": 600,
                "NEOGEO": 900, "Master System": 500, "Commodore 64": 500, "Virtual Console Arcade": 800}
SYSTEM_OF = {name: system for system, names in SYSTEMS for name in names}
KINDS = {"vc": "Virtual Console", "wiiware": "WiiWare", "channels": "Wii Channels"}


def e(text):
    return html.escape(str(text or ""), quote=True)


def plain(text):
    """A catalogue name as text: its trademark marks as characters, other markup gone."""
    text = re.sub(r"<sup>\s*TM\s*</sup>", "\u2122", text or "", flags=re.I)
    text = re.sub(r"<sup>\s*(\(R\)|R|&reg;|\u00ae)\s*</sup>", "\u00ae", text, flags=re.I)
    return html.unescape(re.sub(r"<[^>]+>", "", text)).strip()


class Catalogue:
    """The titles from wii/shop/catalogue.json, or none."""

    def __init__(self, path):
        self.titles, self.region = [], ""
        if os.path.isfile(path):
            with open(path, encoding="utf-8") as source:
                data = json.load(source)
            self.region = data.get("region", "")
            seen = set()
            for title in data.get("titles", []):
                if title["id"] not in seen:
                    seen.add(title["id"])
                    for key in ("title1", "title2", "publisher", "genre", "players"):
                        title[key] = plain(title.get(key))
                    self.titles.append(title)
        self.by_id = {title["id"]: title for title in self.titles}

    def kind_of(self, title):
        if title["console"] == "WiiWare":
            return "wiiware"
        return "vc" if title["console"] in VIRTUAL_CONSOLE else "channels"

    def choose(self, kind=None, system=None, genre=None, order="popular"):
        titles = [t for t in self.titles if (not kind or self.kind_of(t) == kind)]
        if system:
            names = dict(SYSTEMS).get(system, [system])
            titles = [t for t in titles if t["console"] in names]
        if genre:
            titles = [t for t in titles if genre in genre_names(t)]
        if order == "popular":  # Nintendo's own systems first, as the Shop's lists began.
            first = [name for name, _ in SYSTEMS]
            titles.sort(key=lambda t: first.index(SYSTEM_OF[t["console"]]) if t["console"] in SYSTEM_OF else len(first))
        elif order == "newest":
            titles.sort(key=lambda t: date_key(t["date"]), reverse=True)
        elif order == "name":
            titles.sort(key=lambda t: (t["title1"] + " " + t["title2"]).lower())
        return titles

    def genres(self, kind):
        found = {}
        for title in self.choose(kind):
            for genre in genre_names(title):
                found[genre] = found.get(genre, 0) + 1
        return sorted(found)


def genre_names(title):
    return [part.strip() for part in (title.get("genre") or "").split(",") if part.strip()]


def date_key(date):
    """A sortable key for the catalogue's dates, written m/d/yyyy or d.m.yyyy."""
    for separator, order in (("/", (2, 0, 1)), (".", (2, 1, 0))):
        parts = date.split(separator)
        if len(parts) == 3 and all(part.isdigit() for part in parts):
            return tuple(int(parts[i]) for i in order)
    return (0, 0, 0)


# What every page has: the channel's objects, sounds, and moving between pages.
SCRIPT = """<script type="text/javascript">
var shop = null, snd = null, connecting = null;
if (typeof wiiShop != "undefined") { shop = new wiiShop(); connecting = shop.connecting; }
if (typeof wiiSound != "undefined") snd = new wiiSound();
function sound(effect) { if (snd) snd.playSE(effect); }
function focusSound() { sound(2); }
function decideSound() { sound(3); }
function cancelSound() { sound(4); }
function swap(id, picture) { var image = document.getElementById(id); if (image) image.src = picture; }
function go(url) { if (shop) shop.beginWaiting(); location.href = url; }
function wiiMenu() { if (shop) shop.returnToMenu(); }
function started() {
  if (shop) { shop.setWallpaper(%(wallpaper)d); shop.enableHRP(); shop.endWaiting(); }
  if (window.pageStarted) pageStarted();
}
</script>"""


def page(title, body, style="", wallpaper=0, sheets=(), background=None):
    links = "".join(f'<link href="/oss/oss/common/css/{name}" rel="stylesheet" type="text/css" />' for name in ("oss.css",) + tuple(sheets))
    back = f"background-image: url({IMAGES}{background});" if background else ""
    return f"""<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 4.01 Transitional//EN">
<html><head><meta http-equiv="Content-Type" content="text/html; charset=utf-8" />
<title>{e(title)}</title>{links}
<style type="text/css">
body {{ margin: 0; background-color: #ffffff; background-repeat: no-repeat; {back} font-family: sans-serif; }}
.at {{ position: absolute; }}
.plain {{ font-size: 16px; color: #333333; }}
{style}
</style>
{SCRIPT % {"wallpaper": wallpaper}}
</head>
<body onload="started()">
{body}
</body></html>
"""


def hover(name, normal, over, link, width, height, sound_on_click="decideSound()"):
    """A spacer over a picture that swaps the picture while the pointer is on it."""
    return (f'<a href="javascript:go(\'{link}\')" onclick="{sound_on_click}">'
            f'<img src="{IMAGES}spacer.gif" width="{width}" height="{height}" border="0" '
            f'onmouseover="swap(\'{name}\', \'{IMAGES}{over}\'); focusSound();" '
            f'onmouseout="swap(\'{name}\', \'{IMAGES}{normal}\')" /></a>')


def under_button(side, text, script):
    """One of the long buttons along the bottom: L, R, or the middle one."""
    suffix = {"L": "L", "R": "R", "": ""}[side]
    name = f"under{suffix}"
    return f"""<div id="underButton{suffix}">
  <div class="buttonBannerShadow"><img src="{IMAGES}banner/under_banner_shadow.gif" width="211" height="75" /></div>
  <div class="buttonBanner"><img src="{IMAGES}banner/under_banner_a.gif" width="187" height="55" id="{name}" /></div>
  <div class="buttonSpacer"><a href="javascript:{script}" onclick="decideSound()">
    <img src="{IMAGES}spacer.gif" width="187" height="55" border="0"
     onmouseover="swap('{name}', '{IMAGES}banner/under_banner_b.gif'); focusSound();"
     onmouseout="swap('{name}', '{IMAGES}banner/under_banner_a.gif')" /></a></div>
  <div align="center" class="buttonTextBlackM buttonWord"><table width="100%" height="100%"><tr><td align="center" valign="middle">{e(text)}</td></tr></table></div>
</div>"""


def balance():
    """The Wii Points balance between the bottom buttons. Nothing can be bought, so 0."""
    return """<div id="Wiipoint"><div align="center" class="buttonTextBlackM">Wii Points</div></div>
<div id="point" class="wiiPoint">0</div>"""


def frame(title, back_link="W_03.jsp", long_line=False):
    """A browsing page's frame: its title, the button back to the start, the dotted lines,
    the balance and the Wii Menu button."""
    dots = "･" * 73
    upper = (f'<div class="dot" id="upperLineLong">{dots}</div>' if long_line
             else f'<div class="dot" id="line01">{"･" * 59}</div>')
    return f"""<div id="text01-01" class="at" style="left:29px; top:28px; width:442px; height:36px; z-index:33; overflow:hidden">
  <div align="left" class="titleBlackL">{e(title)}</div></div>
<div id="tophelpshadow"><img src="{IMAGES}banner/top_help_shadow01.gif" width="132" height="75" /></div>
<div id="top"><img src="{IMAGES}banner/top_a.gif" width="52" height="55" border="0" id="TopImage" /></div>
<div id="top" style="z-index:101">{hover("TopImage", "banner/top_a.gif", "banner/top_b.gif", back_link, 52, 55, "cancelSound()")}</div>
<div id="help"><img src="{IMAGES}banner/help_gray.gif" width="52" height="55" border="0" /></div>
{upper}
<div class="dot" id="line02">{dots}</div>
{balance()}
{under_button("L", "Wii Menu", "wiiMenu()")}"""


def welcome(catalogue):
    """W_01: the first page, with the way in."""
    count = len(catalogue.titles)
    note = f"{count} titles to look through." if count else "No catalogue yet: run recomp/tools/shop/assets.py."
    body = f"""<div class="at" style="left:40px; top:120px; width:528px; text-align:center">
  <div class="titleBlueL">Welcome to the Wii Shop Channel</div>
  <div class="plain" style="margin-top:24px">Nintendo's Wii Shop has closed. This one, from your own computer,
  shows what it offered: look round as much as you like. Nothing can be bought or downloaded.</div>
  <div class="contentsGrayM" style="margin-top:18px">{e(note)}</div></div>
<div class="at" style="left:123px; top:365px; width:365px; height:76px; z-index:3">
  <img src="{IMAGES}banner/GifA_GoShop_noAction.png" width="365" height="76" id="GoShop" /></div>
<div class="at" style="left:129px; top:375px; width:352px; height:56px; z-index:4; text-align:center">
  <div style="color:#333333; font-size:24px; margin-top:12px">Start Shopping</div></div>
<div class="at" style="left:129px; top:375px; width:352px; height:56px; z-index:5">
  {hover("GoShop", "banner/GifA_GoShop_noAction.png", "banner/GifA_GoShop_Over.png", "W_03.jsp", 352, 56)}</div>"""
    # The Shop's own music starts here, as on the original first page, and plays on.
    music = '<script type="text/javascript">function pageStarted() { if (snd) snd.playBGM(); }</script>'
    return page("Wii Shop Channel", music + body, wallpaper=0)


def main_menu(catalogue):
    """W_03: the three kinds of title, and the account's buttons."""
    big = [("01", "17", "vc", "top_buy_vc", "Virtual Console", "B_01.jsp?kind=vc"),
           ("07", "210", "wiiware", "top_buy_wiiware", "WiiWare", "B_01.jsp?kind=wiiware"),
           ("02", "403", "channels", "top_buy_wiichannel", "Wii Channels", "B_04.jsp?kind=channels")]
    parts = []
    for number, left, _, picture, text, link in big:
        x = int(left)
        parts.append(f"""<div class="at" style="left:{x}px; top:73px; width:191px; height:149px; z-index:36">
  <img src="{IMAGES}banner/{picture}_a.png" width="191" height="149" id="Image{number}" /></div>
<div class="at" style="left:{x + 10}px; top:174px; width:171px; height:22px; z-index:37">
  <div align="center" class="contentsBlackS">{e(text)}</div></div>
<div class="at" style="left:{x + 10}px; top:83px; width:171px; height:129px; z-index:100">
  {hover(f"Image{number}", f"banner/{picture}_a.png", f"banner/{picture}_b.png", link, 171, 129)}</div>""")
    small = [("04", 21, "Account Activity", "W_04.jsp?what=activity"), ("06", 212, "Titles You've Downloaded", "W_04.jsp?what=owned"),
             ("05", 403, "Settings and Features", "W_04.jsp?what=settings")]
    for number, x, text, link in small:
        parts.append(f"""<div class="at" style="left:{x}px; top:303px; width:184px; height:59px; z-index:24">
  <img src="{IMAGES}banner/option2_a.png" width="184" height="59" id="Image{number}" /></div>
<div class="at" style="left:{x + 10}px; top:303px; width:164px; height:59px; z-index:25">
  <table width="100%" height="100%"><tr><td align="center" valign="middle"><div class="buttonTextBlackM">{e(text)}</div></td></tr></table></div>
<div class="at" style="left:{x}px; top:303px; width:184px; height:59px; z-index:26">
  {hover(f"Image{number}", "banner/option2_a.png", "banner/option2_b.png", link, 184, 59)}</div>""")
    parts.append(f"""<div class="at" style="left:158px; top:223px; width:292px; height:69px; z-index:30">
  <img src="{IMAGES}banner/Wiipoint_banner_a.gif" width="292" height="69" id="Image03" /></div>
<div class="at" style="left:193px; top:244px; width:224px; height:22px; z-index:31">
  <div align="center" class="buttonTextBlackM">Add Wii Points</div></div>
<div class="at" style="left:158px; top:223px; width:292px; height:69px; z-index:32">
  {hover("Image03", "banner/Wiipoint_banner_a.gif", "banner/Wiipoint_banner_b.gif", "W_04.jsp?what=points", 292, 69)}</div>
<div class="at" style="left:24px; top:25px; width:551px; height:42px; z-index:3">
  <img src="{IMAGES}banner/GifA_SoftList_noAction.png" id="goTop" /></div>
<div class="at titleBlueL" style="left:29px; top:28px; width:570px; height:28px; z-index:28">Wii Shop Channel</div>
<div class="at" style="left:29px; top:28px; width:554px; height:40px; z-index:46">
  {hover("goTop", "banner/GifA_SoftList_noAction.png", "banner/goTop_Over.png", "W_01.jsp", 554, 36, "cancelSound()")}</div>
<div class="dot" id="line02">{"･" * 73}</div>
{balance()}
{under_button("L", "Wii Menu", "wiiMenu()")}""")
    return page("Wii Shop Channel", "\n".join(parts), background="back/A_03_back3.gif")


def kind_menu(catalogue, kind):
    """B_01: one kind of title's ways in: popular, newest, by system or by genre."""
    name = KINDS.get(kind, "Virtual Console")
    count = len(catalogue.choose(kind))
    rows = [("01", 28, 105, 260, "B01_halfbanner", "Popular Titles", f"B_04.jsp?kind={kind}&order=popular"),
            ("02", 320, 105, 260, "B01_halfbanner", "Newest Additions", f"B_04.jsp?kind={kind}&order=newest")]
    if kind == "vc":
        rows.append(("03", 28, 190, 552, "B01_banner", "Search by Platform", "B_02.jsp?kind=vc"))
    rows.append(("04", 28, 275 if kind == "vc" else 190, 552, "B01_banner", "Search by Genre", f"B_03.jsp?kind={kind}"))
    parts = [f"""<div class="at" style="left:29px; top:80px; width:545px; height:18px; z-index:61">
  <div align="left" class="contentsGrayM">{count} titles</div></div>"""]
    for number, x, y, width, picture, text, link in rows:
        parts.append(f"""<div class="at" style="left:{x}px; top:{y}px; width:{width}px; height:85px; z-index:28">
  <img src="{IMAGES}banner/{picture}_a.png" width="{width}" height="85" id="Image{number}" /></div>
<div class="at" style="left:{x + 20}px; top:{y + 10}px; width:{width - 40}px; height:65px; z-index:29">
  <table width="100%" height="100%"><tr><td align="center" valign="middle"><div class="buttonTextBlackL">{e(text)}</div></td></tr></table></div>
<div class="at" style="left:{x + 10}px; top:{y + 10}px; width:{width - 20}px; height:65px; z-index:30; overflow:hidden">
  {hover(f"Image{number}", f"banner/{picture}_a.png", f"banner/{picture}_b.png", link, width - 20, 65)}</div>""")
    return page(name, frame(name) + "\n".join(parts))


def choice_list(title, choices):
    """B_02 and B_03: a list of systems or genres, each to its titles."""
    parts = []
    for i, (text, count, link) in enumerate(choices):
        column, row = i % 2, i // 2
        x, y = 36 + column * 272, 4 + row * 46
        parts.append(f"""<div class="at" style="left:{x - 36}px; top:{y}px; width:268px; height:42px">
  <img src="{IMAGES}banner/B01_halfbanner_a.png" width="268" height="42" id="Choice{i}" /></div>
<div class="at" style="left:{x - 26}px; top:{y + 10}px; width:248px; height:22px; text-align:center">
  <span class="buttonTextBlackM">{e(text)}</span> <span class="contentsGrayS">({count})</span></div>
<div class="at" style="left:{x - 36}px; top:{y}px; width:268px; height:42px">
  {hover(f"Choice{i}", "banner/B01_halfbanner_a.png", "banner/B01_halfbanner_b.png", link, 268, 42)}</div>""")
    inner = "\n".join(parts)
    body = f"""<div class="catalogFrame at" style="left:36px; top:88px; width:540px; height:277px; overflow:auto; border:solid 1px #35beed; z-index:44">
<div style="position:relative; height:{4 + (len(choices) + 1) // 2 * 46}px">{inner}</div></div>"""
    return page(title, frame(title) + body)


def systems(catalogue):
    choices = []
    for name, _ in SYSTEMS:
        count = len(catalogue.choose("vc", system=name))
        if count:
            choices.append((name, count, f"B_04.jsp?kind=vc&system={quote(name)}"))
    return choice_list("Search by Platform", choices)


def genres(catalogue, kind):
    choices = [(genre, len(catalogue.choose(kind, genre=genre)), f"B_04.jsp?kind={kind}&genre={quote(genre)}")
               for genre in catalogue.genres(kind)]
    return choice_list("Search by Genre", choices)


def thumbnail(title, width, height):
    name = f"{title['id']}_{title['thumbnail']}.gif"
    return f'<img src="/oss/thumbnails/{e(name)}" width="{width}" height="{height}" />'


def points_text(title):
    """The price: the catalogue's, a Virtual Console system's usual one when it has none,
    Free for channels, and nothing for anything else without one."""
    points = title.get("points") or "0"
    if points != "0":
        return f"{points} Wii Points"
    usual = USUAL_POINTS.get(SYSTEM_OF.get(title.get("console")))
    if usual:
        return f"{usual} Wii Points"
    return "Free" if title.get("console") not in VIRTUAL_CONSOLE and title.get("console") != "WiiWare" else ""


def title_list(catalogue, kind=None, system=None, genre=None, order="popular", number=1):
    """B_04: titles, ten to a page, each to its details."""
    titles = catalogue.choose(kind, system, genre, order)
    pages = max(1, (len(titles) + PER_PAGE - 1) // PER_PAGE)
    number = min(max(1, number), pages)
    shown = titles[(number - 1) * PER_PAGE:number * PER_PAGE]
    heading = system or genre or {"popular": "Popular Titles", "newest": "Newest Additions"}.get(order, "Titles")
    if kind == "channels":
        heading = "Wii Channels"
    rows = []
    for i, title in enumerate(shown):
        y = i * 91
        link = f"B_05.jsp?titleId={title['id']}"
        rows.append(f"""<div class="at" style="left:0px; top:{y}px; width:488px; height:91px">
  <div class="at" style="left:0px; top:0px"><img src="{IMAGES}banner/B_04_softbanner_a.gif" width="488" height="91" id="Row{i}" /></div>
  <div class="at" style="left:18px; top:9px; width:96px; height:72px; z-index:9">{thumbnail(title, 96, 72)}</div>
  <div class="at catalogTitleBlack" style="left:120px; top:8px; width:350px; z-index:10; overflow:hidden; white-space:nowrap">{e(title['title1'])}</div>
  <div class="at catalogTitleBlack" style="left:120px; top:26px; width:350px; z-index:11; overflow:hidden; white-space:nowrap">{e(title['title2'])}</div>
  <div class="at catalogTitleBlack_01" style="left:120px; top:46px; width:350px; height:18px; z-index:12">{e(title['publisher'])}</div>
  <div class="at catalogTitleBlack_01" style="left:120px; top:64px; width:220px; height:14px; z-index:13">{e(title['console'])}</div>
  <div class="at catalogTitleBlue" style="left:264px; top:64px; width:206px; height:18px; z-index:2; text-align:right">{e(points_text(title))}</div>
  <div class="at" style="left:0px; top:0px; width:488px; height:91px; z-index:14">
    {hover(f"Row{i}", "banner/B_04_softbanner_a.gif", "banner/B_04_softbanner_b.gif", link, 488, 91)}</div>
</div>""")
    query = "&".join(f"{key}={quote(value)}" for key, value in (("kind", kind), ("system", system), ("genre", genre), ("order", order)) if value)
    arrows = []
    if number > 1:
        arrows.append(f"""<div class="at" style="left:372px; top:385px; width:52px; height:52px; z-index:38">
  <img src="{IMAGES}banner/arrowL_page_a.gif" id="ArrowL" /></div>
<div class="at" style="left:372px; top:385px; width:52px; height:52px; z-index:39">
  {hover("ArrowL", "banner/arrowL_page_a.gif", "banner/arrowL_page_b.gif", f"B_04.jsp?{query}&p={number - 1}", 52, 52, "sound(1)")}</div>""")
    if number < pages:
        arrows.append(f"""<div class="at" style="left:525px; top:385px; width:52px; height:52px; z-index:39">
  <img src="{IMAGES}banner/arrowR_page_a.gif" id="ArrowR" /></div>
<div class="at" style="left:525px; top:385px; width:52px; height:52px; z-index:40">
  {hover("ArrowR", "banner/arrowR_page_a.gif", "banner/arrowR_page_b.gif", f"B_04.jsp?{query}&p={number + 1}", 52, 52, "sound(1)")}</div>""")
    empty = "" if titles else '<div class="plain" style="padding:20px">No titles here.</div>'
    body = f"""<div class="catalogFrame at" style="left:36px; top:88px; width:540px; height:277px; overflow:auto; border:solid 1px #35beed; z-index:44">
<div style="position:relative; height:{max(1, len(shown)) * 91}px">{empty}{"".join(rows)}</div></div>
<div class="at contentsGrayM" style="left:428px; top:400px; width:92px; height:36px; text-align:center; z-index:20">{number}/{pages}</div>
{"".join(arrows)}"""
    left = frame(heading)
    return page(heading, left.replace(under_button("L", "Wii Menu", "wiiMenu()"), "") + body
                + under_button("L", "Back", "history.back()"))


def details(catalogue, title_id):
    """B_05: one title, with the Download button that can't download."""
    title = catalogue.by_id.get((title_id or "").upper())
    if not title:
        return page("Not found", frame("Details") + '<div class="fullScreenText plain">This title isn\'t in the catalogue.</div>')
    rating = title.get("rating") or ""
    rating_picture = (f'<img src="{IMAGES}Pic_Rating/ESRB/ESRB_{e(rating)}.gif" width="46" height="69" />'
                      if catalogue.region in ("US", "CA") and rating else "")
    controllers = "".join(f'<img src="{IMAGES}banner/{e(name)}" height="40" />' for name in title["controllers"]["pictures"][:3])
    body = f"""<div id="details">
  <img src="{IMAGES}banner/Details.gif" width="537" height="217" />
  <div id="Platform"><div align="left" class="headerWhiteS">{e(title['console'])}</div></div>
  <div id="Photo">{thumbnail(title, 160, 120)}</div>
  <div id="controller" style="width:77px; overflow:hidden">{controllers}</div>
  <div id="Rating">{rating_picture}</div>
  <div class="style8 at" style="left:300px; top:28px; width:228px; height:65px; overflow:hidden; font-size:12px">{e(title.get('ratingdetails'))}</div>
  <div id="date"><div align="left" class="catalogTitleBlack_01">{e(title['date'])}</div></div>
  <div id="Players"><div align="left" class="catalogTitleBlack_01">{e(title['players'])}</div></div>
  <div class="catalogTitleBlack_01" id="Publisher"><div align="left">{e(title['publisher'])}</div></div>
  <div class="catalogTitleBlack_01" id="DisplayCategory" style="overflow:hidden" align="left">{e(title['genre'])}</div>
  <div id="TitleName1" style="overflow:hidden" nowrap><div align="center" class="headerBlueM">{e(title['title1'])}</div></div>
  <div id="TitleName2" style="overflow:hidden" nowrap><div align="center" class="headerBlueM">{e(title['title2'])}</div></div>
</div>
<div id="buyButton" style="left:52px">
  <div id="buybanner"><img src="{IMAGES}banner/buy_a.gif" width="241" height="76" id="BuyImage" /></div>
  <div id="text03-01" class="buttonTextWhiteL" align="center">Download</div>
  <div id="price" class="buttonTextWhiteL" align="center">{e(points_text(title))}</div>
  <div id="buyspacer">{hover("BuyImage", "banner/buy_a.gif", "banner/buy_b.gif", f"B_06.jsp?titleId={title['id']}", 241, 76)}</div>
  <div id="buyshadow"><img src="{IMAGES}banner/banner03_shadow.gif" width="274" height="90" /></div>
</div>
<div class="at" style="left:321px; top:293px; width:241px; height:76px">
  <div class="at" style="left:0px; top:0px; z-index:32"><img src="{IMAGES}banner/buy_a.gif" width="241" height="76" id="GiftImage" /></div>
  <div class="at buttonTextWhiteL" style="left:0px; top:6px; width:241px; height:28px; z-index:33" align="center">Gift</div>
  <div class="at buttonTextWhiteL" style="left:0px; top:40px; width:241px; height:28px; z-index:34" align="center">{e(points_text(title))}</div>
  <div class="at" style="left:0px; top:0px; width:241px; height:76px; z-index:35">{hover("GiftImage", "banner/buy_a.gif", "banner/buy_b.gif", f"B_06.jsp?titleId={title['id']}&gift=1", 241, 76)}</div>
  <div id="buyshadow"><img src="{IMAGES}banner/banner03_shadow.gif" width="274" height="90" /></div>
</div>"""
    framed = frame("Details", long_line=True).replace(under_button("L", "Wii Menu", "wiiMenu()"), under_button("L", "Back", "history.back()"))
    return page("Details", framed + body, sheets=("B_05.css",))


def refused(catalogue, title_id, gift=False):
    """B_06: what Download and Gift lead to. Nothing is handed out."""
    title = catalogue.by_id.get((title_id or "").upper(), {"title1": "This title"})
    what = "given as a gift" if gift else "downloaded"
    body = f"""<div class="fullScreenText plain" style="text-align:center">
  <div class="headerBlueM">{e(title['title1'])}</div>
  <p>This title can't be {what}.</p>
  <p>Nintendo's Wii Shop has closed, and this one, from your own computer, is only for
  looking round. It has nothing to download.</p></div>"""
    framed = frame("Download").replace(under_button("L", "Wii Menu", "wiiMenu()"), under_button("L", "Back", "history.back()"))
    return page("Download", framed + body)


def other(what):
    """W_04: the account's pages, which have nothing to show here."""
    titles = {"activity": "Account Activity", "owned": "Titles You've Downloaded", "settings": "Settings and Features",
              "points": "Add Wii Points"}
    notes = {"activity": "There's no account activity: nothing can be bought here.",
             "owned": "Nothing has been downloaded from this Shop, and nothing can be.",
             "settings": "There's nothing to set for this Shop.",
             "points": "Wii Points can't be added: nothing can be bought here."}
    title = titles.get(what, "Wii Shop Channel")
    body = f'<div class="fullScreenText plain" style="text-align:center">{e(notes.get(what, ""))}</div>'
    framed = frame(title).replace(under_button("L", "Wii Menu", "wiiMenu()"), under_button("L", "Back", "history.back()"))
    return page(title, framed + body)
