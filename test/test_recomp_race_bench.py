"""race_bench.py's reading of a console log, with a made-up log."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "recomp/tools"))

import race_bench  # noqa: E402

MENUS = "second 1: 60 draws, 60 copies, 60 to screen, 120 draw done, 3 new shaders, slowest 1.0 ms, worst frame 16.7 ms, passes 60, new pipelines 2, drains 0, waited reads 0\n"


def race_second(number, shown, drains=0, waits=0, shaders=0):
    return (f"second {number}: {shown * 15000} draws, {shown * 17} copies, {shown} to screen, {shown * 2} draw done, {shaders} new shaders, "
            f"slowest 0.0 ms, worst frame 20.0 ms, passes {shown * 19}, new pipelines {shaders}, drains {drains}, waited reads {waits}\n")


class Summary(unittest.TestCase):
    def test_reads_only_the_race(self):
        race = race_second(2, 60) + race_second(3, 30, drains=30, waits=2, shaders=1)
        race += "".join("frame 16.67\n" for _ in range(90)) + "frame 33.40\nframe 50.00\n"
        race += "window: 4 pictures not shown while it was busy\ninterpreting 80123450 (return address 80123460)\n"
        log = MENUS + "frame 16.70\n" * 5 + race
        result = race_bench.summarize(log, len(MENUS) + len("frame 16.70\n") * 5)
        self.assertEqual(result["seconds"], 2)
        self.assertEqual(result["fps"], [60, 30])
        self.assertEqual(result["seconds_at_58_or_more"], 1)
        self.assertEqual(result["draws_per_frame"], 15000)
        self.assertEqual(result["frames"], 92)
        self.assertEqual(result["frame_ms_median"], 16.67)
        self.assertEqual(result["frame_ms_99"], 50.0)
        self.assertAlmostEqual(result["even_share"], round(90 / 92, 3))
        self.assertEqual((result["drains"], result["waited_reads"]), (30, 2))
        self.assertEqual((result["new_shaders"], result["new_pipelines"]), (1, 1))
        self.assertEqual((result["not_shown"], result["interpreted"]), (4, 1))

    def test_older_lines_without_the_new_counts(self):
        old = "second 9: 30000 draws, 34 copies, 2 to screen, 4 draw done, 0 new shaders, slowest 0.0 ms, worst frame 500.0 ms, passes 38, new pipelines 0\n"
        result = race_bench.summarize(old)
        self.assertEqual((result["fps"], result["drains"]), ([2], 0))

    def test_report_mentions_each_measure(self):
        text = race_bench.report(race_bench.summarize(race_second(1, 60) + "frame 16.67\n"))
        for word in ("fps median", "even", "drains", "waited reads", "not shown", "interpreted"):
            self.assertIn(word, text)


if __name__ == "__main__":
    unittest.main()
