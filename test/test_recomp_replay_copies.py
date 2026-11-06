"""Picture copies reaching memory, and EFB peeks and pokes, through the Vulkan renderer.

Each scene is played by recomp/tools/replay, which then reads guest memory as the
processor would, or peeks and pokes the picture. Expected bytes come from the formats'
layouts: RGB565 is big-endian 5, 6 and 5 bits, RGBA8 a block of alpha and red pairs then
one of green and blue, and intensity is Y = (66 R + 129 G + 25 B + 4096) / 256, rounded.
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_recomp_replay import bp_command, program, show_frame, write_dff
from test_recomp_replay_features import copy_picture, flat_quad, textured_quad, textured_state, use_texture

TARGET = 0x00300000
RED, GREEN = (200, 30, 60), (20, 180, 40)
# Copy format codes hold their low bit at the top: RGB565 is 4, RGBA8 6, R8 8.
COPY_RGB565, COPY_RGBA8, COPY_R8 = 8 << 3, 12 << 3, 1 << 3
INTENSITY = (1 << 15) | (1 << 16)
HALF = 1 << 9
CLEAR = 1 << 11


def efb(x, y, depth=False):
    return 0x08000000 | (0x400000 if depth else 0) | (y << 12) | (x << 2)


@unittest.skipUnless(shutil.which("clang") and shutil.which("make"), "needs clang and make")
class Played(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.folder = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)

    def play(self, frames, *arguments, state=None, show_last=True):
        """Each frame ends by showing the picture, which clears it, unless the last is kept."""
        recording = self.folder / "scene.dff"
        # A frame is its commands, or its commands and the memory updates landing among them.
        frames = [frame if isinstance(frame, tuple) else (frame, []) for frame in frames]
        shown = [(commands + (show_frame() if show_last or i + 1 < len(frames) else b""), updates) for i, (commands, updates) in enumerate(frames)]
        write_dff(recording, state or textured_state(), shown)
        result = subprocess.run([str(program()), str(recording), str(self.folder / "frames"), *arguments], capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.log = result.stdout + result.stderr
        return result

    def memory(self, frames, address, size, state=None):
        path = self.folder / "memory.bin"
        self.play(frames, "--memory", f"{address:#x}:{size}:{path}", state=state)
        return path.read_bytes()


@unittest.skipUnless(shutil.which("clang") and shutil.which("make"), "needs clang and make")
class CopiesInMemory(Played):
    def test_rgb565_lands_as_its_bytes(self):
        made = flat_quad(RED, 0, 0, 64, 64) + copy_picture(TARGET, COPY_RGB565)
        self.assertEqual(self.memory([made], TARGET, 64 * 64 * 2), bytes([0xc8, 0xe7]) * (64 * 64))

    def test_rgba8_lands_as_alpha_red_then_green_blue(self):
        made = flat_quad(RED, 0, 0, 64, 64) + copy_picture(TARGET, COPY_RGBA8, stride=16 * 64)
        block = bytes([0xff, 200]) * 16 + bytes([30, 60]) * 16  # An RGB8 picture's alpha is full.
        self.assertEqual(self.memory([made], TARGET, 64 * 64 * 4), block * (16 * 16))

    def test_intensity_lands_as_y(self):
        made = flat_quad(RED, 0, 0, 64, 64) + copy_picture(TARGET, COPY_R8 | INTENSITY, stride=8 * 32)
        self.assertEqual(self.memory([made], TARGET, 64 * 64), bytes([89]) * (64 * 64))

    def test_red_without_intensity_is_red(self):
        made = flat_quad(RED, 0, 0, 64, 64) + copy_picture(TARGET, COPY_R8, stride=8 * 32)
        self.assertEqual(self.memory([made], TARGET, 64 * 64), bytes([200]) * (64 * 64))

    def test_half_size_averages_each_box(self):
        # Boxes start at x 63: the first holds a red and a green column, the rest green.
        made = flat_quad(RED, 0, 0, 64, 64) + flat_quad(GREEN, 64, 0, 128, 64) + copy_picture(TARGET, COPY_RGBA8 | HALF, 8, 8, stride=64, x=63)
        data = self.memory([made], TARGET, 64)
        mixed = [(a + b) // 2 for a, b in zip(RED, GREEN)]
        for found, wanted in zip([data[1], data[32], data[33]], mixed):
            self.assertLessEqual(abs(found - wanted), 1, list(data))
        self.assertEqual([data[3], data[34], data[35]], list(GREEN))

    def test_depth_lands_as_its_three_bytes(self):
        state = textured_state()
        bp = state[0]
        bp[0x40] = 1 << 4  # Depth written, so the clear reaches it.
        made = (bp_command(0x51, 0x123456) + copy_picture(TARGET + 0x10000, COPY_RGBA8 | CLEAR, stride=16 * 64) + bp_command(0x43, 3)
                + copy_picture(TARGET, COPY_RGBA8, stride=16 * 64) + bp_command(0x43, 0))
        data = self.memory([made], TARGET, 64, state=state)
        self.assertEqual(data[:2], bytes([0xff, 0x12]))
        self.assertEqual(data[32:34], bytes([0x34, 0x56]))

    def test_a_late_copy_leaves_what_was_written_after_it(self):
        # Data just past the copy, on its last page, is touched, so that page is let go and
        # the copy lands a frame later. A write into the copy's bytes there in the meantime
        # came after the copy, and stays; the rest of the page takes the copy's bytes.
        target = TARGET + 0x100
        made = flat_quad(RED, 0, 0, 64, 64) + copy_picture(target, COPY_RGB565)
        end = target + 64 * 64 * 2
        updates = [(len(made), end + 0x40, b"\x01\x02\x03\x04"), (len(made), end - 0x80, b"\x11\x22\x33\x44")]
        data = self.memory([(made, updates), b"", b""], end - 0x100, 0x100)
        self.assertEqual(data[0x80:0x84], b"\x11\x22\x33\x44")
        self.assertEqual(data[:0x80], bytes([0xc8, 0xe7]) * 0x40)
        self.assertEqual(data[0x84:], bytes([0xc8, 0xe7]) * 0x3e)

    def test_a_copy_read_once_is_read_back_as_it_is_made(self):
        # The first frame's smaller texture reads the copy from memory, so the next copy
        # there is read back at once, and lands in memory as a later picture is shown.
        first = flat_quad(RED, 0, 0, 64, 64) + copy_picture(TARGET, COPY_RGB565) + use_texture(TARGET, 32, 32, 4) + textured_quad(200, 200, 300, 300)
        again = flat_quad(GREEN, 0, 0, 64, 64) + copy_picture(TARGET, COPY_RGB565)
        data = self.memory([first, again, b"", b""], TARGET, 2)
        self.assertEqual(data, bytes([0x15, 0xa5]))  # Green: 2, 45 and 5.
        # Only the first, read as a texture in the frame it was made, waited for the card.
        self.assertEqual(self.log.count("waited for the card"), 1)

    def test_a_copy_partly_covered_keeps_its_bytes_apart(self):
        # A big copy, then a small one at the same place, as a blur chain makes: nothing is
        # read back while they're made, and memory has the small one's bytes over the big one's.
        big = flat_quad(RED, 0, 0, 64, 64) + copy_picture(TARGET, COPY_RGB565)
        small = flat_quad(GREEN, 0, 0, 64, 64) + copy_picture(TARGET, COPY_RGB565, 32, 32, stride=16 * 32)
        data = self.memory([big + small], TARGET, 64 * 64 * 2)
        self.assertEqual(data[:2], bytes([0x15, 0xa5]))  # Green, from the small copy's first tile.
        self.assertEqual(data[256:258], bytes([0xc8, 0xe7]))  # The big one's, in the gap between the small one's rows.
        self.assertEqual(data[512:514], bytes([0x15, 0xa5]))  # The small one's second row of tiles.
        self.assertEqual(data[8 * 512:8 * 512 + 2], bytes([0xc8, 0xe7]))  # The big one's, past the small one's rows.
        self.assertIn("1 kept apart from a later one, 0 landed early, 0 waited for", self.log)


@unittest.skipUnless(shutil.which("clang") and shutil.which("make"), "needs clang and make")
class PeeksAndPokes(Played):
    def peeks(self, frames, *arguments, state=None):
        result = self.play(frames, *arguments, state=state, show_last=False)
        return [int(line.split()[-1], 16) for line in result.stdout.splitlines() if line.startswith("peek ")]

    def test_a_peek_reads_the_picture(self):
        # Alpha reads as 0 in the PE's first alpha read mode.
        made = flat_quad(RED, 0, 0, 64, 64)
        self.assertEqual(self.peeks([made], "--peek", hex(efb(10, 10)), "--peek", hex(efb(700, 10))), [0x00c81e3c, 0])

    def test_a_poke_reads_back(self):
        found = self.peeks([b""], "--poke", f"{efb(100, 100):#x}=0xff112233", "--peek", hex(efb(100, 100)), "--peek", hex(efb(101, 100)),
                           "--poke", f"{efb(5, 6, True):#x}=0xabcdef", "--peek", hex(efb(5, 6, True)))
        self.assertEqual(found[0], 0x00112233)
        self.assertNotEqual(found[1], 0x00112233)  # Only that pixel.
        self.assertEqual(found[2], 0xabcdef)

    def test_a_depth_peek_reads_a_clear(self):
        state = textured_state()
        state[0][0x40] = 1 << 4
        made = bp_command(0x51, 0x654321) + copy_picture(TARGET, COPY_RGBA8 | CLEAR, stride=16 * 64)
        self.assertEqual(self.peeks([made], "--peek", hex(efb(3, 3, True)), state=state), [0x654321])


if __name__ == "__main__":
    unittest.main()


@unittest.skipUnless(shutil.which("clang") and shutil.which("make"), "needs clang and make")
class BoundingBox(Played):
    def box(self, commands):
        result = self.play([commands], "--bounding-box", "0", show_last=False)
        lines = [line for line in result.stdout.splitlines() if line.startswith("bounding box ")]
        return [int(value) for value in lines[-1].split()[2:]]

    def start(self):
        # Left 1023 and right 0, top 1023 and bottom 0: anything drawn widens it.
        return bp_command(0x55, 0x3ff) + bp_command(0x56, 0x3ff)

    def test_drawn_pixels_widen_it_in_squares_of_two(self):
        # Pixels 101 to 150 across and 120 to 200 down, rounded out to whole squares of 2 by 2.
        self.assertEqual(self.box(self.start() + flat_quad(RED, 101, 120, 151, 201)), [100, 151, 120, 201])

    def test_draws_before_it_is_set_leave_it_alone(self):
        commands = flat_quad(RED, 10, 10, 50, 50) + self.start() + flat_quad(RED, 200, 200, 210, 210)
        self.assertEqual(self.box(commands), [200, 209, 200, 209])

    def test_nothing_drawn_reads_as_set(self):
        self.assertEqual(self.box(bp_command(0x55, 5 | (6 << 10)) + bp_command(0x56, 7 | (8 << 10))), [5, 6, 7, 8])
