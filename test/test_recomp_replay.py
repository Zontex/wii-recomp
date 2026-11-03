"""Writes tiny FIFO recordings, plays them through recomp/tools/replay and checks the pictures."""
import importlib.util
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from PIL import Image

HERE = Path(__file__).parent
REPLAY = HERE.parent / "recomp/tools/replay"
COMPARE = HERE.parent / "recomp/tools/compare_frames.py"
VULKAN_SDK = os.environ.get("VULKAN_SDK") or str(HERE.parent / "recomp/build/vulkan-sdk")
SCALE = 2  # The Makefile's own default.
WIDTH, HEIGHT = 640, 456

FILE_ID = 0x0d01f1f0
BP_WORDS, CP_WORDS, XF_WORDS, XF_REGISTER_WORDS, TEXTURE_BYTES = 256, 256, 4096, 88, 1 << 20
HEADER_BYTES, FRAME_BYTES, UPDATE_BYTES = 128, 64, 24
UPDATE_VERTICES = 4

BLUE, RED, GREEN, YELLOW = (10, 20, 200), (200, 30, 60), (20, 180, 40), (230, 210, 20)
COPY_TO_SCREEN_AND_CLEAR = 0x4800
TRIANGLES = 0x90
QUADS = 0x80
ZERO, RASTER_COLOR, TEXTURE_COLOR = 15, 10, 8


def words(values):
    return struct.pack(f"<{len(values)}I", *values)


def float_bits(value):
    return struct.unpack(">I", struct.pack(">f", value))[0]


def write_dff(path, state, frames, version=6, first_word=FILE_ID):
    """frames holds (commands, [(position, address, bytes)]), written as Dolphin's Save does."""
    bp, cp, xf, xf_registers, texture = state
    blocks = bytearray()
    start = HEADER_BYTES + len(frames) * FRAME_BYTES

    def add(data):
        at = start + len(blocks)
        blocks.extend(data)
        return at

    bp_at, cp_at, xf_at, xf_registers_at, texture_at = add(words(bp)), add(words(cp)), add(words(xf)), add(words(xf_registers)), add(texture)
    frame_list = bytearray()
    for commands, updates in frames:
        fifo_at = add(commands)
        list_at = add(bytes(len(updates) * UPDATE_BYTES))
        for i, (position, address, data) in enumerate(updates):
            data_at = add(data)
            at = list_at + i * UPDATE_BYTES - start
            blocks[at:at + UPDATE_BYTES] = struct.pack("<IIQIB3x", position, address, data_at, len(data), UPDATE_VERTICES)
        frame_list += struct.pack("<QIIIQI32x", fifo_at, len(commands), 0x00200000, 0x00300000, list_at, len(updates))
    header = struct.pack("<III" + "QI" * 4 + "QII" + "QI" + "II8s24x", first_word, version, 1, bp_at, BP_WORDS, cp_at, CP_WORDS, xf_at, XF_WORDS,
                         xf_registers_at, XF_REGISTER_WORDS, HEADER_BYTES, len(frames), 1, texture_at, TEXTURE_BYTES, 24 << 20, 64 << 20, b"TEST01")
    assert len(header) == HEADER_BYTES
    Path(path).write_bytes(header + frame_list + blocks)


def base_state(clear=BLUE):
    """A chip set to draw vertex colors in pixels of a 640x456 picture."""
    bp, cp, xf, xf_registers = [0] * BP_WORDS, [0] * CP_WORDS, [0] * XF_WORDS, [0] * XF_REGISTER_WORDS
    origin = 342
    bp[0x00] = 1 << 4  # One color channel, one stage, no textures.
    bp[0x20] = origin | (origin << 12)
    bp[0x21] = (origin + HEIGHT - 1) | ((origin + WIDTH - 1) << 12)
    bp[0x59] = (origin // 2) | ((origin // 2) << 10)
    bp[0x41] = 0x18  # Writes color and alpha.
    bp[0x49] = 0
    bp[0x4a] = (WIDTH - 1) | ((HEIGHT - 1) << 10)
    bp[0x4f] = (255 << 8) | clear[0]
    bp[0x50] = (clear[1] << 8) | clear[2]
    bp[0x51] = 0xffffff
    bp[0xc0] = (1 << 19) | (ZERO << 12) | (ZERO << 8) | (ZERO << 4) | RASTER_COLOR
    bp[0xc1] = (1 << 19) | (7 << 13) | (7 << 10) | (7 << 7) | (5 << 4)
    bp[0xf3] = (7 << 16) | (7 << 19)  # The alpha test always passes.
    bp[0xf6], bp[0xf7] = 1 << 2, 2 | (3 << 2)  # Channels in their own order.

    cp[0x50] = (1 << 9) | (1 << 13)  # Position and color in the stream.
    cp[0x70] = 1 | (4 << 1) | (1 << 13) | (5 << 14)  # Three floats, then four bytes of color.

    xf[0], xf[5], xf[10] = (float_bits(1.0),) * 3  # The first matrix is the identity.
    xf_registers[0x09] = 1  # One color channel.
    xf_registers[0x0e] = xf_registers[0x10] = 1  # Color from the vertex, with no lights.
    for i, value in enumerate([WIDTH / 2, -HEIGHT / 2, 0xffffff, origin + WIDTH / 2, origin + HEIGHT / 2, 0xffffff]):
        xf_registers[0x1a + i] = float_bits(value)
    for i, value in enumerate([2 / WIDTH, -1, -2 / HEIGHT, 1, 0, -0.5]):
        xf_registers[0x20 + i] = float_bits(value)
    xf_registers[0x26] = 1  # Orthographic.
    return bp, cp, xf, xf_registers, bytearray(TEXTURE_BYTES)


def bp_command(reg, value):
    return bytes([0x61, reg]) + value.to_bytes(3, "big")


def cp_command(reg, value):
    return bytes([0x08, reg]) + value.to_bytes(4, "big")


def show_frame():
    return bp_command(0x52, COPY_TO_SCREEN_AND_CLEAR)


def vertex(x, y, color):
    return struct.pack(">fff", x, y, 0) + bytes(color) + b"\xff"


def draw(kind, vertices):
    return bytes([kind]) + struct.pack(">H", len(vertices)) + b"".join(vertices)


def triangle_frame(color=RED):
    return draw(TRIANGLES, [vertex(100, 100, color), vertex(500, 100, color), vertex(300, 400, color)]) + show_frame(), []


def positions(*points):
    return b"".join(struct.pack(">fff", x, y, 0) for x, y in points)


def updates_frame():
    """Two draws of the same indices, with memory changed between them, in MEM1 and MEM2."""
    position_array, color_array = 0x00100000, 0x10001000
    setup = (cp_command(0x50, (2 << 9) | (2 << 13)) + cp_command(0xa0, position_array) + cp_command(0xa2, color_array)
             + cp_command(0xb0, 12) + cp_command(0xb2, 4))
    indices = draw(TRIANGLES, [b"\0\0", b"\1\0", b"\2\0"])
    left, right = positions((20, 20), (200, 20), (20, 200)), positions((400, 250), (600, 250), (600, 440))
    commands = setup + indices + indices + show_frame()
    first, second = len(setup), len(setup) + len(indices)
    updates = [(first, position_array, left), (first, color_array, bytes(GREEN) + b"\xff"),
               (second, position_array, right), (second, color_array, bytes(YELLOW) + b"\xff")]
    return commands, updates


def palette_recording():
    """A four-bit texture whose palette was loaded before the recording began."""
    state = base_state()
    bp, cp, xf, xf_registers, texture = state
    slot, image_address = 3, 0x00200000
    red, green, blue = 28, 46, 6
    texture[(slot << 9) + 2:(slot << 9) + 4] = struct.pack(">H", (red << 11) | (green << 5) | blue)
    expected = (red * 255 // 31, green * 255 // 63, blue * 255 // 31)
    bp[0x00] = 1 | (1 << 4)  # One texture coordinate.
    bp[0x28] = 1 << 6  # Stage 0 reads map 0.
    bp[0xc0] = (1 << 19) | (ZERO << 12) | (ZERO << 8) | (ZERO << 4) | TEXTURE_COLOR
    bp[0x88] = 7 | (7 << 10) | (8 << 20)  # 8x8, four bits a pixel.
    bp[0x94] = image_address >> 5
    bp[0x98] = slot | (1 << 10)  # The palette's slot, holding RGB565 colors.
    cp[0x30] = 60 << 6  # The first coordinate's matrix is the identity.
    cp[0x50] = 1 << 9
    cp[0x60] = 1
    cp[0x70] = 1 | (4 << 1) | (1 << 21) | (4 << 22)  # Three floats, then two floats.
    xf_registers[0x3f] = 1
    xf_registers[0x40] = 5 << 7  # From the vertex's first coordinate.
    corners = [(50, 50, 0, 0), (250, 50, 1, 0), (250, 250, 1, 1), (50, 250, 0, 1)]
    commands = draw(QUADS, [struct.pack(">fffff", x, y, 0, u, v) for x, y, u, v in corners]) + show_frame()
    image = bytes([0x11]) * 32  # Every pixel is color 1, which only texture memory holds.
    return state, [(commands, [(0, image_address, image)])], expected


@unittest.skipUnless(shutil.which("clang") and shutil.which("make"), "needs clang and make")
class BadFiles(unittest.TestCase):
    """These stop before any graphics, so they only need the program to exist."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.folder = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)
        self.good = self.folder / "good.dff"
        write_dff(self.good, base_state(), [triangle_frame()])

    def stops_with(self, path, words_expected):
        result = subprocess.run([str(program()), str(path), str(self.folder / "frames")], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 10, result.stdout + result.stderr)
        self.assertTrue(result.stderr.startswith("stop: "), result.stderr)
        self.assertIn(words_expected, result.stderr)
        self.assertFalse((self.folder / "frames").exists())

    def test_rejects_other_versions(self):
        for version in (5, 7):
            path = self.folder / f"version{version}.dff"
            write_dff(path, base_state(), [triangle_frame()], version=version)
            self.stops_with(path, f"file version {version} is not supported")

    def test_rejects_other_files(self):
        path = self.folder / "other.dff"
        write_dff(path, base_state(), [triangle_frame()], first_word=0x12345678)
        self.stops_with(path, "not a Dolphin FIFO recording")

    def test_rejects_a_short_header(self):
        path = self.folder / "short.dff"
        path.write_bytes(self.good.read_bytes()[:40])
        self.stops_with(path, "too short")

    def test_rejects_a_cut_off_file(self):
        path = self.folder / "cut.dff"
        path.write_bytes(self.good.read_bytes()[:-20])
        self.stops_with(path, "runs past the end")

    def test_rejects_updates_out_of_order(self):
        path = self.folder / "order.dff"
        commands, updates = updates_frame()
        write_dff(path, base_state(), [(commands, list(reversed(updates)))])
        self.stops_with(path, "out of order")


def has_vulkan():
    return (Path(VULKAN_SDK) / "include/shaderc/shaderc.h").exists() or Path("/usr/include/shaderc/shaderc.h").exists()


def program():
    """Builds the player once. Make skips what's already done."""
    if not has_vulkan():
        raise unittest.SkipTest("needs the Vulkan and shaderc headers: set VULKAN_SDK")
    make = ["make", "-C", str(REPLAY), f"VULKAN_SDK={VULKAN_SDK}"] if Path(VULKAN_SDK).exists() else ["make", "-C", str(REPLAY)]
    subprocess.run(make, check=True, capture_output=True, text=True)
    return REPLAY / "build/replay"


def replay(recording, frames, **settings):
    return subprocess.run([str(program()), str(recording), str(frames)], capture_output=True, text=True, timeout=120, env={**os.environ, **settings})


@unittest.skipUnless(shutil.which("clang") and shutil.which("make"), "needs clang and make")
class Replay(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.folder = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)

    def play(self, state, frames):
        recording, out = self.folder / "scene.dff", self.folder / "frames"
        write_dff(recording, state, frames)
        result = replay(recording, out)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return out, result

    def pixel(self, picture, x, y):
        return picture.getpixel((x * SCALE, y * SCALE))

    def assert_background(self, picture, x, y):
        """A clear that opens a render pass came out one level high on the Mac's AMD chip through MoltenVK."""
        for found, wanted in zip(self.pixel(picture, x, y), BLUE):
            self.assertLessEqual(abs(found - wanted), 1, (x, y))

    def test_one_colored_triangle(self):
        out, result = self.play(base_state(), [triangle_frame()])
        self.assertIn("version 6, Wii, game TEST01, memory 24 + 64 MiB, 1 frames", result.stdout)
        self.assertEqual(sorted(path.name for path in out.iterdir()), ["frame-0000.png"])
        picture = Image.open(out / "frame-0000.png")
        self.assertEqual(picture.size, (WIDTH * SCALE, HEIGHT * SCALE))
        self.assertEqual(picture.mode, "RGB")
        for x, y in [(300, 110), (300, 200), (300, 390), (120, 105), (480, 105)]:
            self.assertEqual(self.pixel(picture, x, y), RED, (x, y))
        for x, y in [(5, 5), (300, 90), (300, 410), (100, 300), (500, 300), (630, 450)]:
            self.assert_background(picture, x, y)

    def test_drawn_clear_is_exact(self):
        recording, out = self.folder / "scene.dff", self.folder / "frames"
        write_dff(recording, base_state(), [triangle_frame()])
        result = replay(recording, out, WII_RECOMP_DRAW_CLEARS="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.pixel(Image.open(out / "frame-0000.png"), 5, 5), BLUE)

    def test_triangle_edges_land_where_they_should(self):
        out, _ = self.play(base_state(), [triangle_frame()])
        picture = Image.open(out / "frame-0000.png")
        row = [picture.getpixel((x, 200 * SCALE)) for x in range(WIDTH * SCALE)]
        inside = [x for x, color in enumerate(row) if color == RED]
        # At height 200 the triangle runs from x 166.67 to 433.33 of the Wii's pixels.
        self.assertAlmostEqual(inside[0] / SCALE, 166.67, delta=1)
        self.assertAlmostEqual(inside[-1] / SCALE, 433.33, delta=1)
        self.assertEqual(len(inside), inside[-1] - inside[0] + 1)

    def test_memory_updates_land_between_commands(self):
        out, result = self.play(base_state(), [triangle_frame(), updates_frame()])
        self.assertIn("saved 2 of 2 frames", result.stdout)
        first, second = Image.open(out / "frame-0000.png"), Image.open(out / "frame-0001.png")
        self.assertEqual(self.pixel(first, 300, 200), RED)
        self.assert_background(second, 300, 200)  # The copy cleared the first frame.
        self.assertEqual(self.pixel(second, 60, 60), GREEN)
        self.assertEqual(self.pixel(second, 550, 300), YELLOW)
        self.assert_background(second, 550, 100)

    def test_frame_with_no_copy_repeats_the_last_picture(self):
        nothing = (bytes(8), [])
        out, result = self.play(base_state(), [triangle_frame(), nothing])
        self.assertIn("saved 2 of 2 frames", result.stdout)
        self.assertIn("1 frames with none", result.stdout)
        self.assertEqual((out / "frame-0000.png").read_bytes(), (out / "frame-0001.png").read_bytes())

    def test_frame_limit(self):
        recording, out = self.folder / "scene.dff", self.folder / "frames"
        write_dff(recording, base_state(), [triangle_frame(), triangle_frame(GREEN)])
        result = subprocess.run([str(program()), str(recording), str(out), "--frames", "1"], capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sorted(path.name for path in out.iterdir()), ["frame-0000.png"])

    def test_palette_comes_from_texture_memory(self):
        state, frames, expected = palette_recording()
        out, result = self.play(state, frames)
        picture = Image.open(out / "frame-0000.png")
        self.assertEqual(self.pixel(picture, 150, 150), expected, result.stderr)
        self.assert_background(picture, 300, 300)

    def test_full_palettes_in_neighbouring_slots_stay_apart(self):
        state, ((commands, updates),), expected = palette_recording()
        wrong, palette_source, count = bytes(512 * [0xff]), 0x00300000, 16
        load_over_slot_2 = bp_command(0x64, palette_source >> 5) + bp_command(0x65, 2 | (count << 10))
        updates = updates + [(0, palette_source, wrong)]
        out, result = self.play(state, [(load_over_slot_2 + commands, sorted(updates, key=lambda update: update[0]))])
        self.assertEqual(self.pixel(Image.open(out / "frame-0000.png"), 150, 150), expected, result.stderr)

    def test_repeats_exactly(self):
        out_a, _ = self.play(base_state(), [triangle_frame(), updates_frame()])
        renamed = self.folder / "again"
        out_a.rename(renamed)
        out_b, _ = self.play(base_state(), [triangle_frame(), updates_frame()])
        self.assertTrue(load_compare().compare(renamed, out_b))


def load_compare():
    spec = importlib.util.spec_from_file_location("compare_frames", COMPARE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class CompareFrames(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.folder = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)
        self.a, self.b = self.folder / "a", self.folder / "b"
        self.a.mkdir()
        self.b.mkdir()

    def save(self, folder, name, color=(100, 100, 100), size=(8, 8), spot=None):
        picture = Image.new("RGB", size, color)
        if spot:
            picture.putpixel((0, 0), spot)
        picture.save(folder / name)

    def run_tool(self, *arguments):
        return subprocess.run(["python3", str(COMPARE), str(self.a), str(self.b), *map(str, arguments)], capture_output=True, text=True)

    def test_same_frames_match(self):
        self.save(self.a, "frame-0000.png")
        self.save(self.b, "frame-0000.png")
        result = self.run_tool("--out", self.folder / "out")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(list((self.folder / "out").iterdir()), [])

    def test_reports_a_difference_and_draws_it(self):
        self.save(self.a, "frame-0000.png")
        self.save(self.b, "frame-0000.png", spot=(120, 100, 100))
        result = self.run_tool("--out", self.folder / "out")
        self.assertEqual(result.returncode, 1)
        # One channel of 24 in 64 moved by 20: 20 / (64 * 3) / 255 of full scale.
        self.assertIn(f"{20 / (64 * 3) / 255 * 100:.4f}", result.stdout)
        self.assertIn("1.562", result.stdout)  # One pixel in 64.
        change = Image.open(self.folder / "out/diff-0000.png")
        self.assertEqual(change.getpixel((0, 0)), (160, 0, 0))
        self.assertEqual(change.getpixel((1, 1)), (0, 0, 0))

    def test_threshold_lets_small_differences_pass(self):
        self.save(self.a, "frame-0000.png")
        self.save(self.b, "frame-0000.png", spot=(120, 100, 100))
        self.assertEqual(self.run_tool("--threshold", 1).returncode, 0)

    def test_pairs_by_number_not_by_name(self):
        for number in (2, 10):
            self.save(self.a, f"frame-{number}.png", color=(number, 0, 0))
            self.save(self.b, f"dump_{number:04}.png", color=(number, 0, 0))
        self.assertEqual(self.run_tool().returncode, 0)

    def test_missing_frame_fails(self):
        self.save(self.a, "frame-0000.png")
        self.save(self.a, "frame-0001.png")
        self.save(self.b, "frame-0000.png")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("different frame counts", result.stdout)

    def test_size_mismatch_needs_resize(self):
        self.save(self.a, "frame-0000.png", size=(8, 8))
        self.save(self.b, "frame-0000.png", size=(16, 16))
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("sizes differ", result.stdout)
        self.assertEqual(self.run_tool("--resize").returncode, 0)


if __name__ == "__main__":
    unittest.main()
