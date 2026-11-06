"""Ranks the gaps games hit, from the gaps.txt files the console writes (see runtime/gaps.h).

    python3 recomp/tools/gap_report.py [name=]<gaps.txt> ... [--inventory docs/hardware_inventory.md]

Each file is one run, named by `name=` or by its folder. Gaps hit by the most runs come
first, then those whose row is furthest from done. Rows marked done that a game still hit,
and IDs with no row, are listed apart, as both mean the inventory needs a look.
"""
import argparse
import re
from pathlib import Path

INVENTORY = Path(__file__).resolve().parents[2] / "docs/hardware_inventory.md"
ROW = re.compile(r"^\| `([a-z0-9_.]+)` \| (.+?) \| ([a-z ]+?) \|", re.M)
STATUS_ORDER = ["missing", "partial", "unknown", "on demand", "done", "not needed"]
OWN_PREFIXES = ("limit.", "unlisted.")  # Limits of our own, and requests the inventory doesn't list yet.


def inventory_rows(path=INVENTORY):
    """Every row's ID, mapped to its item and status."""
    return {ident: (item.strip(), status.strip()) for ident, item, status in ROW.findall(Path(path).read_text())}


def inventory_details(path=INVENTORY):
    """Every row's ID, mapped to its item, status, line number and section heading."""
    found, section = {}, ""
    for number, line in enumerate(Path(path).read_text().splitlines(), 1):
        if line.startswith("#"):
            section = line.lstrip("#").strip()
        match = ROW.match(line)
        if match:
            ident, item, status = match.groups()
            found[ident] = {"item": item.strip(), "status": status.strip(), "line": number, "section": section}
    return found


def read_gaps(path):
    found = []
    for line in Path(path).read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        ident, detail, count, _ = line.split()
        found.append((ident, detail, int(count)))
    return found


def collect(runs):
    """Merges runs, each (name, what read_gaps found), into id -> {"runs", "count", "details"}."""
    hits = {}
    for name, gaps in runs:
        for ident, detail, count in gaps:
            entry = hits.setdefault(ident, {"runs": set(), "count": 0, "details": set()})
            entry["runs"].add(name)
            entry["count"] += count
            entry["details"].add(detail)
    return hits


def rank_key(ident, hits, rows):
    """Most runs first, then the row furthest from done, then the most hits."""
    status = rows.get(ident, ("", "missing"))[1]
    return (-len(hits[ident]["runs"]), STATUS_ORDER.index(status) if status in STATUS_ORDER else 0, -hits[ident]["count"])


def split(hits, rows):
    """The IDs still to close, those whose row says done or not needed, and those with no row."""
    listed = [i for i in hits if i in rows and rows[i][1] not in ("done", "not needed")]
    done = [i for i in hits if i in rows and rows[i][1] in ("done", "not needed")]
    return listed, done, [i for i in hits if i not in rows]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("runs", nargs="+", help="gaps.txt files, optionally as name=path")
    parser.add_argument("--inventory", default=INVENTORY)
    arguments = parser.parse_args()
    rows = inventory_rows(arguments.inventory)
    runs = []
    for run in arguments.runs:
        name, _, path = run.rpartition("=")
        runs.append((name or Path(path).resolve().parent.name, read_gaps(path)))
    hits = collect(runs)

    def rank(ident):
        return rank_key(ident, hits, rows)

    listed, done, unknown = split(hits, rows)

    def table(title, idents):
        if not idents:
            return
        print(f"\n## {title}\n")
        print("| ID | Status | Runs | Hits | Details | Item |")
        print("|---|---|---|---|---|---|")
        for ident in sorted(idents, key=rank):
            item, status = rows.get(ident, ("", "no row"))
            entry = hits[ident]
            details = ", ".join(sorted(entry["details"])[:4]) + (" …" if len(entry["details"]) > 4 else "")
            print(f"| `{ident}` | {status} | {', '.join(sorted(entry['runs']))} | {entry['count']} | {details} | {item} |")

    print(f"# Gaps from {len(arguments.runs)} run(s): {len(hits)} kinds")
    table("Gaps to close", listed)
    table("Marked done or not needed, but hit", done)
    table("No inventory row", unknown)


if __name__ == "__main__":
    main()
