"""compat_run.py: tiers, endings, the gap ranking and report, and its safety rules, all on
made-up logs, gap files and pictures."""
import contextlib
import importlib.util
import io
import json
import os
import signal
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("compat_run", ROOT / "recomp/tools/compat_run.py")
compat = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compat)


def second(n, draws, to_screen=60):
    return (f"second {n}: {draws} draws, {to_screen} copies, {to_screen} to screen, {to_screen * 2} draw done, 0 new shaders, "
            f"slowest 0.0 ms, worst frame 17.0 ms, passes 0, new pipelines 0\n")


def seconds(*draws, to_screen=60):
    return compat.parse_log("".join(second(n + 1, d, to_screen) for n, d in enumerate(draws)))["seconds"]


def look(log_second, new=True, after_input=True, still_before=True, text="", new_text=False):
    return {"log_second": log_second, "new_screen": new, "after_input": after_input, "still_before": still_before,
            "text": text, "new_text": new_text}


INVENTORY = """# Made-up inventory

## 1. CPU

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `cpu.alpha` | Alpha | missing | x |  |
| `cpu.beta` | Beta | partial | x |  |

## 2. Graphics

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `gx.gamma` | Gamma | done | x |  |
| `gx.delta` | Delta | not needed | x |  |
"""


class Log(unittest.TestCase):
    def test_parse_log_reads_seconds_fps_gaps_and_stops(self):
        text = (second(1, 10) + "fps 59.9\ngap: cpu.alpha 0x4\n" + second(2, 20) + "fps 60.0\n"
                + "gap: gx.gamma 0x0\nstop: no code at 80001234 (return address 80000000)\n")
        parsed = compat.parse_log(text)
        self.assertEqual([s["draws"] for s in parsed["seconds"]], [10, 20])
        self.assertEqual(parsed["fps"], [59.9, 60.0])
        self.assertEqual(parsed["gaps_met"], [{"id": "cpu.alpha", "detail": "0x4", "second": 1},
                                              {"id": "gx.gamma", "detail": "0x0", "second": 2}])
        self.assertEqual(parsed["stops"], ["stop: no code at 80001234 (return address 80000000)"])

    def test_frame_rate_leaves_out_start_up(self):
        rate = compat.frame_rate([5, 10, 20] + [60] * 9 + [30])
        self.assertEqual(rate["median"], 60)
        self.assertEqual(rate["seconds"], 10)
        self.assertIsNone(compat.frame_rate([])["median"])


class Tiers(unittest.TestCase):
    def test_no_picture_doesnt_boot(self):
        self.assertEqual(compat.classify(seconds(0, 0, to_screen=0), [])[0], 0)
        self.assertEqual(compat.classify([], [])[0], 0)
        self.assertEqual(compat.classify(seconds(50, 50, to_screen=0), [])[0], 0)  # Drawing, but nothing shown.

    def test_a_picture_without_answers_to_input_boots(self):
        tier, reasons = compat.classify(seconds(60, 60, 60), [look(1, after_input=False), look(2, new=False)])
        self.assertEqual(tier, 1)
        self.assertIn("first picture in second 1", reasons)

    def test_two_answers_on_still_screens_reach_menus(self):
        observations = [look(1, after_input=False), look(3), look(5)]
        self.assertEqual(compat.classify(seconds(*[60] * 6), observations)[0], 2)

    def test_a_movie_or_timed_warnings_arent_menus(self):
        # New screens after every press, but the screen was never still before it.
        observations = [look(n, still_before=False) for n in range(1, 10)]
        self.assertEqual(compat.classify(seconds(*[60] * 10), observations)[0], 1)

    def test_new_text_counts_as_an_answer(self):
        observations = [look(2, new=False, new_text=True, text="Main Menu"), look(4, new=False, new_text=True, text="Options")]
        self.assertEqual(compat.classify(seconds(*[60] * 5), observations)[0], 2)

    def test_heavy_drawing_after_menus_is_in_game(self):
        observations = [look(2), look(3)]
        draws = [60, 60, 60] + [1600 * 60] * 5
        tier, reasons = compat.classify(seconds(*draws), observations)
        self.assertEqual(tier, 3)
        self.assertTrue(any("5 seconds in a row" in r for r in reasons))

    def test_heavy_drawing_needs_enough_seconds_and_menus_first(self):
        observations = [look(2), look(3)]
        self.assertEqual(compat.classify(seconds(60, 60, 60, *[1600 * 60] * 4, 60), observations)[0], 2)
        early = [look(8), look(9)]  # Menus only after the heavy part, such as an attract demo.
        self.assertEqual(compat.classify(seconds(*[1600 * 60] * 6, 60, 60, 60), early)[0], 2)
        self.assertEqual(compat.classify(seconds(*[1600 * 60] * 6), [])[0], 1)

    def test_a_session_goal_is_in_game(self):
        self.assertEqual(compat.classify(seconds(60), [], goal="the first race")[0], 3)
        self.assertEqual(compat.classify([], [], goal="the first race")[0], 0)  # A goal can't make up a picture.


class Endings(unittest.TestCase):
    def test_endings(self):
        two = seconds(60, 60)
        self.assertEqual(compat.ending(1, ["stop: x"], two, False, 0), ("stopped", "stop: x"))
        self.assertEqual(compat.ending(-signal.SIGSEGV, [], two, False, 0), ("crashed", "killed by SIGSEGV"))
        self.assertEqual(compat.ending(3, [], two, False, 0)[0], "exited")
        self.assertEqual(compat.ending(-signal.SIGTERM, [], two, True, 1), ("ran the whole time", ""))
        self.assertEqual(compat.ending(-signal.SIGTERM, [], two, True, 25)[0], "stalled")

    def test_gaps_near_a_stop(self):
        met = [{"id": "cpu.alpha", "second": 2}, {"id": "cpu.beta", "second": 50}]
        log = seconds(*[60] * 55)
        self.assertEqual(compat.near_stop(met, "stopped", log), {"cpu.beta"})
        self.assertEqual(compat.near_stop(met, "ran the whole time", log), set())


class Report(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.inventory = Path(self.folder.name) / "inventory.md"
        self.inventory.write_text(INVENTORY)

    def tearDown(self):
        self.folder.cleanup()

    def game(self, ident, tier, gaps, near=()):
        return {"id": ident, "tier": tier, "gaps": gaps, "near_stop": list(near), "reasons": ["made up"], "ended": "ran the whole time",
                "ended_detail": "", "fps": {"median": 60.0, "low": 58.0, "seconds": 10}, "input_steps": 2, "pictures": 3,
                "folder": "/nowhere", "last_text": ""}

    def test_inventory_details_give_line_and_section(self):
        details = compat.gap_report.inventory_details(self.inventory)
        self.assertEqual(details["cpu.alpha"]["status"], "missing")
        self.assertEqual(details["cpu.alpha"]["section"], "1. CPU")
        self.assertEqual(INVENTORY.splitlines()[details["gx.gamma"]["line"] - 1].split("`")[1], "gx.gamma")

    def test_ranking_weighs_games_tiers_and_status(self):
        games = [self.game("AAAE01", 3, [("cpu.alpha", "0x0", 5), ("cpu.beta", "0x1", 9)]),
                 self.game("BBBE01", 0, [("cpu.beta", "0x2", 1), ("gx.gamma", "0x0", 2), ("odd.thing", "0x0", 1)]),
                 self.game("CCCE01", 1, [("cpu.alpha", "0x3", 1)], near=["cpu.alpha"])]
        ranked = compat.rank(games, compat.gap_report.inventory_details(self.inventory))
        scores = {r["id"]: r["score"] for r in ranked}
        # alpha: in game (1) + boots (3), missing (1.0), near one stop (+2). beta: 1 + 4 at partial (0.75).
        self.assertEqual(scores, {"cpu.alpha": 6.0, "cpu.beta": 3.75, "gx.gamma": 2.0, "odd.thing": 4.0})
        self.assertEqual(ranked[0]["id"], "cpu.alpha")
        self.assertEqual(ranked[0]["games"], ["AAAE01", "CCCE01"])
        self.assertEqual(ranked[0]["details"], ["0x0", "0x3"])
        self.assertEqual(next(r for r in ranked if r["id"] == "odd.thing")["status"], "no row")

    def test_report_files(self):
        games = [self.game("AAAE01", 2, [("cpu.alpha", "0x0", 5), ("gx.gamma", "0x0", 1)]),
                 self.game("BBBE01", 0, [("cpu.alpha", "0x0", 1), ("odd.thing", "0x0", 1)])]
        report = compat.build_report(games, {"when": "today", "seconds": 60, "machine": "test"}, self.inventory)
        path = compat.write_report(report, self.folder.name, self.inventory)
        text = path.read_text()
        self.assertIn("| AAAE01 | reaches menus | ran the whole time | 60.0 (low 58.0) | 2 |", text)
        self.assertIn("| BBBE01 | doesn't boot |", text)
        fix_first = text.split("## What to fix first")[1].split("##")[0]
        self.assertIn(f"[`cpu.alpha`]({self.inventory.as_uri()}#L7)", fix_first)
        self.assertIn("| AAAE01, BBBE01 | 6 |", fix_first)
        self.assertIn("`gx.gamma`", text.split("## Marked done or not needed, but hit")[1])
        self.assertIn("`odd.thing`", text.split("## No inventory row")[1])
        saved = json.loads((Path(self.folder.name) / "report.json").read_text())
        self.assertEqual([g["id"] for g in saved["games"]], ["AAAE01", "BBBE01"])
        self.assertEqual(saved["ranking"][0]["id"], "cpu.alpha")
        self.assertEqual(saved["games"][0]["top_gaps"][0]["id"], "cpu.alpha")


class Pictures(unittest.TestCase):
    def picture(self, folder, name, colour, width=64, height=48, half=None):
        pixels = bytearray(bytes(colour) * width * height)
        if half:
            for y in range(height):
                pixels[y * width * 3:(y * width + width // 2) * 3] = bytes(half) * (width // 2)
        path = Path(folder) / name
        path.write_bytes(b"P6\n%d %d\n255\n" % (width, height) + bytes(pixels))
        return compat.read_ppm(path)

    def test_flat_new_and_png(self):
        with tempfile.TemporaryDirectory() as folder:
            grey = compat.signature(*self.picture(folder, "a.ppm", (128, 128, 128)))
            split = compat.signature(*self.picture(folder, "b.ppm", (0, 0, 0), half=(255, 255, 255)))
            self.assertTrue(compat.is_flat(grey))
            self.assertFalse(compat.is_flat(split))
            self.assertLess(compat.difference(grey, grey), compat.NEW_SCREEN)
            self.assertGreater(compat.difference(grey, split), compat.NEW_SCREEN)
            width, height, pixels = self.picture(folder, "c.ppm", (1, 2, 3))
            png = Path(folder) / "c.png"
            compat.write_png(png, width, height, pixels)
            data = png.read_bytes()
            self.assertTrue(data.startswith(b"\x89PNG\r\n\x1a\n"))
            size = int.from_bytes(data[33:37], "big")
            self.assertEqual(len(zlib.decompress(data[41:41 + size])), height * (1 + width * 3))


class Safety(unittest.TestCase):
    def test_only_processes_it_started_are_stopped(self):
        processes = compat.Processes()
        own = processes.start([sys.executable, "-c", "import time; time.sleep(30)"])
        other = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
        try:
            with self.assertRaises(ValueError):
                processes.stop(other)
            processes.stop_all()
            self.assertEqual(own.returncode, -signal.SIGTERM)
            self.assertIsNone(other.poll())  # Still running: it wasn't this tool's.
            self.assertEqual(processes.stop(own), -signal.SIGTERM)  # Stopping again only reports.
        finally:
            other.kill()
            other.wait()

    def test_signals_go_only_to_its_own_ids(self):
        processes = compat.Processes()
        own = processes.start([sys.executable, "-c", "import time; time.sleep(30)"])
        sent = []
        real_kill = os.kill
        with mock.patch.object(compat.os, "kill", side_effect=lambda pid, number: (sent.append(pid), real_kill(pid, number))):
            processes.stop_all()
            stranger = mock.Mock(pid=own.pid + 1)
            with self.assertRaises(ValueError):
                processes.stop(stranger)
        self.assertEqual(sent, [own.pid])

    def test_saves_are_a_fresh_copy(self):
        with tempfile.TemporaryDirectory() as folder:
            real = Path(folder) / "console/saves"
            (real / "shared2/sys").mkdir(parents=True)
            (real / "shared2/sys/SYSCONF").write_bytes(b"settings")
            run = Path(folder) / "runs/AAAE01"
            self.assertEqual(compat.starting_saves(None, Path(folder) / "console"), real)
            self.assertIsNone(compat.starting_saves("empty", Path(folder) / "console"))
            copy = compat.fresh_saves(real, run, console=Path(folder) / "console")
            (copy / "shared2/sys/SYSCONF").write_bytes(b"changed by the game")
            (copy / "new.dat").write_bytes(b"new")
            self.assertEqual((real / "shared2/sys/SYSCONF").read_bytes(), b"settings")
            self.assertFalse((real / "new.dat").exists())
            again = compat.fresh_saves(real, run, console=Path(folder) / "console")  # The next run starts over.
            self.assertEqual((again / "shared2/sys/SYSCONF").read_bytes(), b"settings")
            self.assertFalse((again / "new.dat").exists())
            self.assertEqual(sorted(p.name for p in compat.fresh_saves(None, run, console=Path(folder) / "console").iterdir()), [])
            with self.assertRaises(SystemExit):  # A run folder inside the real saves.
                compat.fresh_saves(real, real / "x", console=Path(folder) / "console")

    def test_environment_keeps_everything_in_the_run_folder(self):
        with tempfile.TemporaryDirectory() as folder:
            env = compat.environment(folder, Path(folder) / "saves", ["EXTRA=1"], base={})
            self.assertEqual(env["WII_RECOMP_SAVES"], str(Path(folder) / "saves"))
            for key in ("WII_RECOMP_GAPS", "WII_RECOMP_REMOTE_SLOTS", "WII_RECOMP_CONTROL"):
                self.assertTrue(env[key].startswith(folder), key)
            self.assertEqual(env["WII_RECOMP_REMOTES"], "simulated")
            self.assertEqual(env["EXTRA"], "1")
            self.assertNotIn("WII_RECOMP_TITLE_DATA", env)
            self.assertTrue(compat.environment(folder, Path(folder) / "saves", (), True, base={})["WII_RECOMP_TITLE_DATA"].startswith(folder))
            with self.assertRaises(SystemExit):
                compat.environment(folder, compat.CONSOLE_BUILD / "saves", base={})
            with self.assertRaises(SystemExit):
                compat.environment(folder, Path(folder) / "saves", [f"WII_RECOMP_SAVES={compat.CONSOLE_BUILD / 'saves'}"], base={})

    def test_out_folder_stays_outside_the_repository(self):
        with self.assertRaises(SystemExit):
            compat.check_out_folder(ROOT / "runs")
        compat.check_out_folder(Path(tempfile.gettempdir()) / "runs")


class Planning(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        base = Path(self.folder.name)
        self.games, self.recomp = base / "games", base / "recomp"
        self.games.mkdir()
        (self.games / "AAAE01.wrd").write_bytes(b"made up")
        (self.games / "BBBE01.wrd").write_bytes(b"made up")
        (self.recomp / "build-BBBE01").mkdir(parents=True)
        (self.recomp / "build-BBBE01/settings.json").write_text("{}")
        (self.recomp / "build-CCCE01").mkdir(parents=True)
        (self.recomp / "build-CCCE01/settings.json").write_text("{}")
        self.disc = base / "disc"
        (self.disc / "sys").mkdir(parents=True)
        (self.disc / "sys/boot.bin").write_bytes(b"DDDE01" + bytes(26))

    def tearDown(self):
        self.folder.cleanup()

    def plan(self, names, **options):
        return compat.plan(names, self.games, self.recomp, **options)

    def test_all_means_every_image(self):
        entries = self.plan(["all"])
        self.assertEqual([(e["id"], e["build"]) for e in entries], [("AAAE01", False), ("BBBE01", False)])
        self.assertEqual([e["build"] for e in self.plan(["all"], rebuild=True)], [False, True])  # AAAE01 has nothing to build from.

    def test_ids_folders_and_duplicates(self):
        entries = self.plan(["CCCE01", str(self.disc), "AAAE01", str(self.games / "AAAE01.wrd")])
        self.assertEqual([(e["id"], e["build"]) for e in entries], [("CCCE01", True), ("DDDE01", True), ("AAAE01", False)])
        with self.assertRaises(SystemExit):
            self.plan(["ZZZE01"])
        with self.assertRaises(SystemExit):
            self.plan([str(Path(self.folder.name) / "nothing.iso")])

    def test_dry_run_starts_nothing(self):
        out = Path(tempfile.gettempdir()) / "compat-dry-run-test"
        arguments = ["compat_run.py", "all", "--dry-run", "--out", str(out)]
        printed = io.StringIO()
        with mock.patch.object(compat, "GAMES", self.games), mock.patch.object(compat, "ROOT", Path(self.folder.name)), \
                mock.patch.object(sys, "argv", arguments), mock.patch.object(compat.subprocess, "Popen", side_effect=AssertionError), \
                contextlib.redirect_stdout(printed):
            compat.main()
        text = printed.getvalue()
        self.assertIn("would play 2 game(s)", text)
        self.assertIn("AAAE01", text)
        self.assertIn("BBBE01", text)
        self.assertFalse(out.exists())


if __name__ == "__main__":
    unittest.main()
