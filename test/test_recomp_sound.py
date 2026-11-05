"""Sound recordings (recomp/runtime/dsp_record.c) and their player and bench
(recomp/tools/sound_replay.c, sound_bench.py), on made-up programs and data."""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).parent
ROOT = HERE.parent
RUNTIME = ROOT / "recomp/runtime"
TOOLS = ROOT / "recomp/tools"
FLAGS = ["-w", "-O1", "-fwrapv", "-fno-strict-aliasing", "-fsigned-char", "-ffp-contract=off",
         f"-I{RUNTIME}", f"-I{HERE}", "-lm"]
PLAYER_SOURCES = ["dsp.c", "dsp_lle.c", "dsp_lle_ops.c", "dsp_accelerator.c", "dsp_record.c", "ax.c", "zelda.c"]

# dsp_record.h
MAGIC, VERSION = 0x52535257, 2
TIME, RUN, CPU_HIGH, CPU_LOW, READ_LOW, CONTROL, MEMORY_READ, MEMORY_WRITE, MAIL, INTERRUPT = range(1, 11)
AX_WII = 0x347112ba


class Recording:
    """A made-up recording with no interpreter state, as the bench takes for tests."""

    def __init__(self, crc):
        self.crc, self.events, self.mails, self.count = crc, bytearray(), 0, 0

    def event(self, kind, payload=b""):
        self.events += bytes([kind]) + payload
        self.count += 1

    def time(self, ticks):
        self.event(TIME, struct.pack("<Q", ticks))

    def send(self, mail):
        self.event(CPU_HIGH, struct.pack("<H", mail >> 16))
        self.event(CPU_LOW, struct.pack("<H", mail & 0xffff))
        self.mails += 1

    def memory(self, kind, address, data):
        self.event(kind, struct.pack("<II", address, len(data)) + bytes(data))

    def answer(self, mail, interrupt=True):
        self.event(MAIL, struct.pack("<I", mail))
        if interrupt:
            self.event(INTERRUPT)
        self.event(READ_LOW, b"\0" + struct.pack("<I", mail))

    def save(self, path, start, end):
        header = struct.pack("<IIIIIIQQII16x", MAGIC, VERSION, 0, self.crc, self.crc, 0, start, end, self.mails, self.count)
        Path(path).write_bytes(header + self.events)


def words(values):
    return b"".join(struct.pack(">H", value & 0xffff) for value in values)


# The newest Wii voice block in memory, by 16-bit word (Dolphin's AXPBWii without its updates).
PB_WORDS, RUNNING, SRC_TYPE, MIXER_LO, MAIN_LEFT, MAIN_RIGHT, DPOP_LEFT, DPOP_RIGHT = 150, 8, 4, 7, 10, 12, 41, 45
VOLUME, FORMAT, LOOP, END, CURRENT, GAIN, YN1, YN2, LAST_SAMPLES = 53, 56, 57, 59, 61, 79, 81, 82, 86
LIST, PB, ZEROS, SAMPLES, SURROUND, OUTPUT = 0x1000, 0x2000, 0x3000, 0x40000, 0x5000, 0x6000


def ax_recording(frames, altered=None):
    """One voice of 16-bit samples, at full volume, no rate change, to both sides: each frame
    plays the next 96 samples as they are. The real program's writes are worked out from
    Dolphin's AXVoice.h; altered=(frame, sample, by) changes one left sample in them."""
    recording = Recording(AX_WII)
    ticks = 1000
    recording.time(ticks)
    samples = [(i * 37) % 2000 - 1000 for i in range(96 * frames)]
    pb = [0] * PB_WORDS
    pb[RUNNING], pb[SRC_TYPE], pb[MIXER_LO] = 1, 2, 3  # Nearest, main left and right.
    pb[MAIN_LEFT] = pb[MAIN_RIGHT] = pb[VOLUME] = 0x8000
    pb[FORMAT], pb[GAIN] = 0x0a, 0x800  # 16-bit PCM, at a gain of one.
    first = SAMPLES // 2  # In 16-bit words.
    pb[LOOP], pb[LOOP + 1] = first >> 16, first & 0xffff
    pb[END], pb[END + 1] = 0, first + 0x10000
    pb[CURRENT], pb[CURRENT + 1] = first >> 16, first & 0xffff
    command_list = words([0x0000, 0, ZEROS, 0x0004, 0, PB, 0x000b, 0x8000, 0, SURROUND, 0, OUTPUT, 0x000e])
    for frame in range(frames):
        recording.send(0xbabe0000 | len(command_list) // 2)
        ticks += 100
        recording.time(ticks)
        recording.send(LIST)
        if frame == 0:
            recording.memory(MEMORY_READ, LIST, command_list)
            recording.memory(MEMORY_READ, PB, words(pb))
            recording.memory(MEMORY_READ, SAMPLES, words(samples))
        played = samples[frame * 96:(frame + 1) * 96]
        position = first + (frame + 1) * 96
        pb[CURRENT], pb[CURRENT + 1] = position >> 16, position & 0xffff
        pb[YN1], pb[YN2] = played[-1], played[-2]
        pb[LAST_SAMPLES:LAST_SAMPLES + 4] = played[-4:]
        pb[DPOP_LEFT] = pb[DPOP_RIGHT] = played[-1]
        recording.memory(MEMORY_WRITE, PB, words(pb))
        recording.memory(MEMORY_WRITE, SURROUND, bytes(96 * 4))
        left = list(played)
        if altered and altered[0] == frame:
            left[altered[1]] += altered[2]
        recording.memory(MEMORY_WRITE, OUTPUT, words(value for pair in zip(played, left) for value in pair))
        recording.answer(0xdcd10004)  # Sync, then yield.
        recording.answer(0xdcd10002)
        ticks += 1000
        recording.time(ticks)
        recording.send(0xcdd10003)  # Carry on.
    return recording, ticks


GALAXY = 0xd643001f  # A Zelda-family checksum.


def zelda_recording(wrong_ack=False):
    """The Zelda family's mail protocol, as Dolphin's Zelda.cpp answers it: a header with
    the command's word count, its words, then a sync and F355 with the command's top half.
    Command 01 also reads its tables from memory. wrong_ack makes the real program's
    second answer differ."""
    recording = Recording(GALAXY)
    ticks = 1000
    recording.time(ticks)
    table, afc = 0x10000, 0x11000
    commands = [[0x80000000, 0x00000005], [0x81000003, 0x20000, table, afc, 0], [0x8f120000]]
    for number, words_of in enumerate(commands):
        recording.send(len(words_of))
        for i, word in enumerate(words_of):
            recording.send(word)
            if number == 1 and i == 0:
                recording.memory(MEMORY_READ, table, words(range(3, 3 + 0x280)))
                recording.memory(MEMORY_READ, afc, words(range(0x20)))
        ticks += 500
        recording.time(ticks)
        ack = 0xf3550000 | words_of[0] >> 16
        recording.answer(0xdcd10004)
        recording.answer(ack + (1 if wrong_ack and number == 1 else 0), interrupt=False)
    return recording, ticks


def summary(output):
    line = [line for line in output.splitlines() if line.startswith("summary ")][-1]
    return dict(pair.split("=", 1) for pair in line.split()[1:])


@unittest.skipUnless(shutil.which("clang"), "needs clang")
class SoundRecordings(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory()
        cls.player = Path(cls.folder.name) / "sound_replay"
        subprocess.run(["clang", str(TOOLS / "sound_replay.c"), *(str(RUNTIME / name) for name in PLAYER_SOURCES),
                        *FLAGS, "-o", str(cls.player)], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def play(self, recording):
        run = subprocess.run([str(self.player), str(recording), str(RUNTIME / "data")], capture_output=True, text=True,
                             timeout=60)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        return summary(run.stdout)

    def test_interpreter_recording_round_trip(self):
        """The recorder around the interpreter, then the interpreter replaying it as recorded."""
        recorder, recording = Path(self.folder.name) / "recorder", Path(self.folder.name) / "made-up.wsr"
        subprocess.run(["clang", str(HERE / "c/test_sound_record.c"), str(RUNTIME / "dsp_record.c"),
                        *(str(RUNTIME / name) for name in ["dsp_lle.c", "dsp_lle_ops.c", "dsp_accelerator.c"]),
                        *FLAGS, "-o", str(recorder)], check=True, capture_output=True, text=True)
        run = subprocess.run([str(recorder), str(RUNTIME / "data"), str(recording)], capture_output=True, text=True,
                             timeout=60)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertTrue(run.stdout.endswith("ok\n"), run.stdout)
        result = self.play(recording)
        self.assertEqual((result["replays"], result["mails"], result["frames"], result["program"]), ("yes", "3", "3", "none"))

    def test_c_version_matches_made_up_real_program(self):
        recording, end = ax_recording(3)
        path = Path(self.folder.name) / "ax.wsr"
        recording.save(path, 1000, end)
        result = self.play(path)
        self.assertEqual(result["program"], "AX")
        self.assertEqual(result["replays"], "skipped")
        self.assertEqual(result["frames"], "3")
        self.assertEqual(result["in_step_from"], "2")  # Started mid-game: from the first list's answers.
        self.assertEqual(result["first_mails"], "same")
        self.assertEqual((result["differ"], result["peak"]), ("0", "0"))
        self.assertGreater(int(result["words"]), 3 * 96 * 2)
        self.assertEqual(result["interrupts"], "6/6")

    def test_one_different_sample_is_found(self):
        recording, end = ax_recording(3, altered=(1, 10, 5))
        path = Path(self.folder.name) / "ax-altered.wsr"
        recording.save(path, 1000, end)
        result = self.play(path)
        self.assertEqual((result["differ"], result["steps_differ"], result["peak"]), ("1", "1", "5"))
        self.assertEqual(result["first_step"], "5")  # The second frame's list address.
        self.assertEqual(int(result["first_address"], 16), OUTPUT + 10 * 4 + 2)  # Left follows right.
        sample = [(i * 37) % 2000 - 1000 for i in range(96 * 3)][96 + 10]
        self.assertEqual(result["first_values"], f"{sample + 5}/{sample}")
        self.assertAlmostEqual(float(result["rms"]), (25 / int(result["words"])) ** 0.5, places=3)

    def test_bench_table_reference_and_strict(self):
        recordings = Path(self.folder.name) / "bench"
        recordings.mkdir(exist_ok=True)
        recording, end = ax_recording(2)
        recording.save(recordings / "clean.wsr", 1000, end)
        reference = Path(self.folder.name) / "reference"
        bench = [sys.executable, str(TOOLS / "sound_bench.py"), str(recordings)]
        saved = subprocess.run([*bench, "--save-reference", str(reference)], capture_output=True, text=True, timeout=120)
        self.assertEqual(saved.returncode, 0, saved.stdout + saved.stderr)
        self.assertRegex(saved.stdout, r"clean\s+AX\s+2\s")
        again = subprocess.run([*bench, "--reference", str(reference), "--strict"], capture_output=True, text=True, timeout=120)
        self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
        self.assertIn("match the reference", again.stdout)
        recording, end = ax_recording(2, altered=(0, 3, -7))
        recording.save(recordings / "clean.wsr", 1000, end)
        strict = subprocess.run([*bench, "--strict"], capture_output=True, text=True, timeout=120)
        self.assertEqual(strict.returncode, 1, strict.stdout)
        self.assertTrue(re.search(r"real -?\d+, C -?\d+", strict.stdout), strict.stdout)

    def test_zelda_protocol(self):
        """A made-up recording of the Zelda family's mail, until one of Super Mario Galaxy 2
        can be made (see recomp/tools/sound_bench.py)."""
        recording, end = zelda_recording()
        path = Path(self.folder.name) / "zelda.wsr"
        recording.save(path, 1000, end)
        result = self.play(path)
        self.assertEqual((result["program"], result["first_mails"], result["mail_steps_differ"]), ("Zelda-family", "same", "0"))
        self.assertEqual(result["in_step_from"], "3")  # The first command's last word.
        recording, end = zelda_recording(wrong_ack=True)
        recording.save(path, 1000, end)
        result = self.play(path)
        self.assertEqual((result["first_mails"], result["mail_steps_differ"]), ("differ", "1"))


    def test_muting_keeps_timing(self):
        """The same interrupts and mails at the same times, muted or not: only the output
        device is left out."""
        program = Path(self.folder.name) / "mute_timing"
        sources = ["audio.c", "ax.c", "zelda.c", "dsp_lle.c", "dsp_lle_ops.c", "dsp_accelerator.c"]
        subprocess.run(["clang", str(HERE / "c/test_mute_timing.c"), *(str(RUNTIME / name) for name in sources), *FLAGS,
                        "-o", str(program)], check=True, capture_output=True, text=True)
        runs = []
        for muted in (False, True):
            environment = {key: value for key, value in os.environ.items() if key != "WII_RECOMP_MUTE"}
            if muted:
                environment["WII_RECOMP_MUTE"] = "1"
            run = subprocess.run([str(program), str(RUNTIME / "data")], capture_output=True, text=True, timeout=60,
                                 env=environment)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertTrue(run.stdout.endswith("ok\n"), run.stdout[-500:])
            runs.append(run.stdout.splitlines())
        unmuted, muted = runs
        self.assertEqual(unmuted[-2], "output opened")
        self.assertEqual(muted[-2], "output not opened")
        self.assertEqual(unmuted[:-2], muted[:-2])
        self.assertGreater(sum("dsp interrupt, mail dcd10002" in line for line in muted), 500)
        self.assertGreater(sum("dma interrupt" in line for line in muted), 500)


if __name__ == "__main__":
    unittest.main()
