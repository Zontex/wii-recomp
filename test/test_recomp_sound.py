"""Sound recordings (recomp/runtime/dsp_record.c) and their player and bench
(recomp/tools/sound_replay.c, sound_bench.py), on made-up programs and data."""
import shutil
import struct
import subprocess
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

    def answer(self, mail):
        self.event(MAIL, struct.pack("<I", mail))
        self.event(INTERRUPT)
        self.event(READ_LOW, b"\0" + struct.pack("<I", mail))

    def save(self, path, start, end):
        header = struct.pack("<IIIIIIQQII16x", MAGIC, VERSION, 0, self.crc, self.crc, 0, start, end, self.mails, self.count)
        Path(path).write_bytes(header + self.events)


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


if __name__ == "__main__":
    unittest.main()
