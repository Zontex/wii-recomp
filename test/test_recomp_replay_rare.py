"""Rare settings of the graphics chip, each checked by the pixels it makes or the gap it names."""
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from PIL import Image

from test_recomp_replay import BLUE, QUADS, SCALE, base_state, bp_command, draw, float_bits, replay, show_frame, write_dff
from test_recomp_replay_features import IMAGE_ADDRESS, flat_quad, quad, solid_image, textured_quad, textured_state, xf_command

HERE = Path(__file__).parent
RUNTIME = HERE.parent / "recomp/runtime"
RED, GREEN = (200, 30, 60), (20, 180, 40)
HALF = 1 << 23
LESS_AND_WRITE = 1 | (1 << 1) | (1 << 4)  # Depth test on, passing when less, and stored.
EARLY_DEPTH = 1 << 6


@unittest.skipUnless(shutil.which("clang") and shutil.which("make"), "needs clang and make")
class Scene(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.folder = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)
        self.log = ""

    def picture(self, state, commands, updates=()):
        recording, out = self.folder / "scene.dff", self.folder / "frames"
        write_dff(recording, state, [(commands + show_frame(), list(updates))])
        result = replay(recording, out)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.log = result.stderr
        return Image.open(out / "frame-0000.png").convert("RGB")

    def assert_pixel(self, picture, x, y, wanted, tolerance=1):
        found = picture.getpixel((x * SCALE, y * SCALE))
        for channel in range(3):
            self.assertLessEqual(abs(found[channel] - wanted[channel]), tolerance, f"at {x},{y}: {found} not {wanted}\n{self.log}")


class ColorChannels(Scene):
    def test_a_channel_past_the_count_reads_zero(self):
        state = base_state()
        state[3][0x09] = 0  # No color channels.
        self.assert_pixel(self.picture(state, quad(RED)), 200, 200, (0, 0, 0))

    def test_a_channel_within_the_count_reads_the_vertex(self):
        self.assert_pixel(self.picture(base_state(), quad(RED)), 200, 200, RED)

    def test_color_1_alone_feeds_the_first_channel(self):
        state = base_state()
        cp = state[1]
        cp[0x50] = (1 << 9) | (1 << 15)  # Position and color 1, with no color 0.
        cp[0x70] = 1 | (4 << 1) | (1 << 17) | (5 << 18)
        self.assert_pixel(self.picture(state, quad(RED)), 200, 200, RED)


class ByteDequantize(Scene):
    """Positions as 16-bit whole numbers with one bit of fraction, at 200 to 400."""
    def scene(self, dequantize):
        state = base_state()
        state[1][0x70] = 1 | (3 << 1) | (1 << 4) | (1 << 13) | (5 << 14) | (dequantize << 30)
        corners = [(200, 200), (400, 200), (400, 400), (200, 400)]
        return self.picture(state, draw(QUADS, [struct.pack(">hhh", x, y, 0) + bytes(RED) + b"\xff" for x, y in corners]))

    def test_set_scales_by_the_shift(self):
        picture = self.scene(1)
        self.assert_pixel(picture, 150, 150, RED)
        self.assert_pixel(picture, 300, 300, BLUE)

    def test_clear_keeps_the_whole_numbers(self):
        picture = self.scene(0)
        self.assert_pixel(picture, 150, 150, BLUE)
        self.assert_pixel(picture, 300, 300, RED)


class EarlyDepth(Scene):
    """A quad whose alpha test fails everywhere, then one behind it."""
    def scene(self, pixel_format):
        state = base_state()
        state[0][0x40] = LESS_AND_WRITE
        state[0][0x43] = pixel_format
        failing = bp_command(0xf3, (4 << 16) | (7 << 19) | 100) + quad(RED, alpha=0)  # Passes only above 100.
        behind = bp_command(0xf3, (7 << 16) | (7 << 19)) + xf_command(0x101c, float_bits(HALF)) + xf_command(0x101f, float_bits(0xffffff))
        return self.picture(state, failing + behind + quad(GREEN, 150, 150, 350, 350))

    def test_late_depth_leaves_nothing_where_alpha_fails(self):
        self.assert_pixel(self.scene(0), 200, 200, GREEN)

    def test_early_depth_is_stored_where_alpha_fails(self):
        picture = self.scene(EARLY_DEPTH)
        self.assert_pixel(picture, 200, 200, BLUE)
        self.assert_pixel(picture, 320, 320, GREEN)

    def depth_texture_scene(self, pixel_format):
        """A quad whose depth texture puts it in front, then one a quarter of the way in."""
        state = textured_state()
        state[0][0x40] = LESS_AND_WRITE
        state[0][0x43] = pixel_format
        state[0][0xf5] = 2 << 2  # Depth replaced by the texture's, as one byte.
        nearer = bp_command(0xf5, 0) + xf_command(0x101c, float_bits(HALF)) + xf_command(0x101f, float_bits(HALF))
        commands = textured_quad() + nearer + flat_quad(GREEN, 150, 150, 350, 350)
        return self.picture(state, commands, [(0, IMAGE_ADDRESS, solid_image((0, 0, 0)))])

    def test_a_depth_texture_stores_its_depth_late(self):
        picture = self.depth_texture_scene(0)
        self.assertNotEqual(picture.getpixel((200 * SCALE, 200 * SCALE)), GREEN, self.log)

    def test_a_depth_texture_is_ignored_early(self):
        self.assert_pixel(self.depth_texture_scene(EARLY_DEPTH), 200, 200, GREEN)


class NamedGaps(Scene):
    def test_settings_we_dont_follow_are_named(self):
        settings = (bp_command(0x00, (1 << 4) | (1 << 9)) + bp_command(0x43, 2) + bp_command(0x43, 4) + bp_command(0x63, 0x8001)
                    + bp_command(0x43, 0) + bp_command(0x00, 1 << 4))
        self.picture(base_state(), settings + quad(RED))
        for name in ["pixel.multisample_and_aa_modes", "pixel.z16_precision_formats", "pixel.efb_y8_u8_v8_yuv420", "gx.0x60_0x63_texture_preload"]:
            self.assertIn(f"gap: {name} ", self.log)

    def test_defaults_name_nothing(self):
        self.picture(base_state(), quad(RED))
        self.assertNotIn("gap: ", self.log)


@unittest.skipUnless(shutil.which("clang"), "needs clang")
class CommandStream(unittest.TestCase):
    def test_metrics_largest_draw_and_distance(self):
        with tempfile.TemporaryDirectory() as folder:
            program = Path(folder) / "program"
            subprocess.run(["clang", str(HERE / "c/test_fifo_rare.c"), "-w", "-O1", "-fwrapv", f"-I{RUNTIME}", f"-I{HERE}", "-lm", "-o", str(program)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(program)], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(result.stdout.endswith("ok\n"), result.stdout)


if __name__ == "__main__":
    unittest.main()
