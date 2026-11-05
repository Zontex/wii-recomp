"""Small scenes for what the graphics chip can do, each checked by the pixels it makes."""
import struct
import tempfile
import shutil
import unittest
from pathlib import Path

from PIL import Image

from test_recomp_replay import (HEIGHT, QUADS, RASTER_COLOR, SCALE, TEXTURE_BYTES, TEXTURE_COLOR, TRIANGLES, WIDTH, ZERO, base_state, bp_command, cp_command, draw,
                                float_bits, program, replay, show_frame, vertex, write_dff)

COLOR_R0, ALPHA_R0, COLOR_R1, ALPHA_R1, COLOR_R2, ALPHA_R2, COLOR_R3, ALPHA_R3 = range(8)
TEXTURE_ALPHA, RASTER_ALPHA, COLOR_ONE, COLOR_HALF, KONST = 9, 11, 12, 13, 14
A_R0, A_R1, A_R2, A_R3, A_TEXTURE, A_RASTER, A_KONST, A_ZERO = range(8)
BIAS_COMPARE = 3
GREATER, EQUAL = 0, 1
MODE_R8, MODE_GR16, MODE_BGR24, MODE_RGB8 = range(4)
MODE_A8 = 3
WHITE = (255, 255, 255)
IMAGE_ADDRESS = 0x00200000
PROJECTED, INPUT_XYZ = 1 << 1, 1 << 2
MATRIX_1 = 1 << 9


def color_stage(a=ZERO, b=ZERO, c=ZERO, d=ZERO, bias=0, subtract=0, clamp=1, scale=0, target=0):
    return d | (c << 4) | (b << 8) | (a << 12) | (bias << 16) | (subtract << 18) | (clamp << 19) | (scale << 20) | (target << 22)


def alpha_stage(a=A_ZERO, b=A_ZERO, c=A_ZERO, d=A_ZERO, bias=0, subtract=0, clamp=1, scale=0, target=0, raster_swap=0, texture_swap=0):
    return (raster_swap | (texture_swap << 2) | (d << 4) | (c << 7) | (b << 10) | (a << 13) | (bias << 16) | (subtract << 18) | (clamp << 19)
            | (scale << 20) | (target << 22))


def register(index, red, green, blue, alpha, konst=False):
    """Commands for a color stage register, or a konst color, in 255ths."""
    flag = (1 << 23) if konst else 0
    return (bp_command(0xe0 + index * 2, (red & 0x7ff) | ((alpha & 0x7ff) << 12) | flag)
            + bp_command(0xe1 + index * 2, (blue & 0x7ff) | ((green & 0x7ff) << 12) | flag))


def quad(color, left=100, top=100, right=300, bottom=300, alpha=255):
    corners = [(left, top), (right, top), (right, bottom), (left, bottom)]
    return draw(QUADS, [struct.pack(">fff", x, y, 0) + bytes(color) + bytes([alpha]) for x, y in corners])


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

    def pixel(self, picture, x, y):
        return picture.getpixel((x * SCALE, y * SCALE))

    def assert_pixel(self, picture, x, y, wanted, tolerance=0):
        found = self.pixel(picture, x, y)
        for channel in range(3):
            self.assertLessEqual(abs(found[channel] - wanted[channel]), tolerance, f"at {x},{y}: {found} not {wanted}\n{self.log}")


class FinalOutput(Scene):
    def test_color_comes_from_the_last_stages_register(self):
        state = base_state()
        bp = state[0]
        bp[0x00] |= 1 << 10  # Two stages.
        # The first leaves its color in R1, the second moves R2 to R3, with R0 still stale.
        bp[0xc0] = color_stage(d=RASTER_COLOR, target=1)
        bp[0xc1] = alpha_stage(d=A_RASTER, target=1)
        bp[0xc2] = color_stage(d=COLOR_R1, target=3)
        bp[0xc3] = alpha_stage(d=A_R1, target=3)
        picture = self.picture(state, register(0, 9, 8, 7, 0) + quad((200, 100, 50)))
        self.assert_pixel(picture, 200, 200, (200, 100, 50))

    def test_alpha_comes_from_the_last_stages_register(self):
        state = base_state()
        bp = state[0]
        bp[0xc0] = color_stage(d=COLOR_ONE)
        bp[0xc1] = alpha_stage(d=A_R2, target=2)  # Alpha goes to R2, leaving R0's 0 behind.
        bp[0xf3] = (4 << 16) | (7 << 19) | 100  # Passes above 100.
        picture = self.picture(state, register(0, 0, 0, 0, 0) + register(2, 0, 0, 0, 200) + quad((0, 0, 0)))
        self.assert_pixel(picture, 200, 200, WHITE)

    def test_only_the_low_byte_of_an_unclamped_output_counts(self):
        state = base_state()
        bp = state[0]
        bp[0xc0] = color_stage(a=ZERO, b=KONST, c=COLOR_ONE, d=COLOR_R1, clamp=0)
        bp[0xf6] = (12 << 4) | (bp[0xf6] & 15)  # K0.
        commands = register(1, 200, 50, 100, 255) + register(0, 100, 10, 200, 0, konst=True) + quad((0, 0, 0))
        # Dolphin ends with `prev & 255`, so 300 shows as 44.
        self.assert_pixel(self.picture(state, commands), 200, 200, (44, 60, 44))


def textured_state():
    """One stage showing texture map 0 through the first coordinate, on an 8x8 RGB565 image."""
    state = base_state()
    bp, cp, xf, xf_registers, _ = state
    bp[0x00] = 1 | (1 << 4)
    bp[0x28] = 1 << 6
    bp[0xc0] = color_stage(d=TEXTURE_COLOR)
    bp[0xc1] = alpha_stage(d=A_TEXTURE)
    bp[0x88] = 7 | (7 << 10) | (4 << 20)
    bp[0x94] = IMAGE_ADDRESS >> 5
    bp[0x30], bp[0x31] = 7, 7
    cp[0x30] = 60 << 6
    cp[0x50] = 1 << 9
    cp[0x60] = 1
    cp[0x70] = 1 | (4 << 1) | (1 << 21) | (4 << 22)
    xf_registers[0x3f] = 1
    xf_registers[0x40] = 5 << 7
    return state


def textured_quad(left=100, top=100, right=300, bottom=300, uv=((0, 0), (1, 0), (1, 1), (0, 1))):
    corners = [(left, top), (right, top), (right, bottom), (left, bottom)]
    return draw(QUADS, [struct.pack(">fffff", x, y, 0, u, v) for (x, y), (u, v) in zip(corners, uv)])


def rgb565(red, green, blue):
    return struct.pack(">H", ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3))


def solid_image(color, width=8, height=8):
    """An RGB565 image of one color, which has no need of its tiles."""
    return rgb565(*color) * (((width + 3) // 4) * ((height + 3) // 4) * 16)


class TextureOff(Scene):
    def test_white_when_coordinates_are_on(self):
        state = textured_state()
        state[0][0x28] = 0  # Stage 0 has no texture.
        picture = self.picture(state, textured_quad())
        self.assert_pixel(picture, 200, 200, WHITE)

    def test_alpha_is_white_too(self):
        state = textured_state()
        state[0][0x28] = 0
        state[0][0xc0] = color_stage(d=TEXTURE_ALPHA)
        self.assert_pixel(self.picture(state, textured_quad()), 200, 200, WHITE)

    def test_zero_when_no_coordinates_are_on(self):
        state = textured_state()
        state[0][0x00] = 1 << 4  # No coordinates, though the stage asks for a texture.
        picture = self.picture(state, textured_quad(), [(0, IMAGE_ADDRESS, solid_image(WHITE))])
        self.assert_pixel(picture, 200, 200, (0, 0, 0))

    def test_a_coordinate_never_enabled_reads_the_first_one(self):
        state = textured_state()
        state[0][0x28] = (1 << 6) | (3 << 3)  # Coordinate 3, with one enabled.
        picture = self.picture(state, textured_quad(), [(0, IMAGE_ADDRESS, solid_image((248, 0, 0)))])
        self.assert_pixel(picture, 200, 200, (255, 0, 0))


def flat_quad(color, left, top, right, bottom):
    """A quad of one color from konst 0, in the textured format."""
    konst = register(0, *color, 255, konst=True)
    return (konst + bp_command(0xf6, (12 << 4) | (1 << 2)) + bp_command(0xc0, color_stage(d=KONST)) + textured_quad(left, top, right, bottom)
            + bp_command(0xc0, color_stage(d=TEXTURE_COLOR)))


def copy_picture(target, control, width=64, height=64, stride=None, x=0, y=0):
    """Copies a piece of the picture to memory, leaving the copy rectangle whole again for the screen."""
    stride = (width // 4) * 32 if stride is None else stride
    return (bp_command(0x49, x | (y << 10)) + bp_command(0x4a, (width - 1) | ((height - 1) << 10)) + bp_command(0x4b, target >> 5)
            + bp_command(0x4d, stride >> 5) + bp_command(0x52, control)
            + bp_command(0x49, 0) + bp_command(0x4a, (WIDTH - 1) | ((HEIGHT - 1) << 10)))


def use_texture(address, width, height, kind):
    return bp_command(0x88, (width - 1) | ((height - 1) << 10) | (kind << 20)) + bp_command(0x94, address >> 5)


class PictureCopies(Scene):
    TARGET = 0x00300000
    RED, GREEN = (200, 30, 60), (20, 180, 40)
    COPY_RGB565 = 8 << 3  # Copy format codes hold their low bit at the top.
    TEXTURE_I8, TEXTURE_RGB565, TEXTURE_CMPR = 1, 4, 14

    def scene(self, *between, texture=None, target=None):
        """Draws, copies, then draws the copy elsewhere, with `between` in the middle."""
        target = self.TARGET if target is None else target
        made = flat_quad(self.RED, 0, 0, 64, 64) + copy_picture(self.TARGET, self.COPY_RGB565)
        return made + b"".join(between) + (texture or use_texture(target, 64, 64, self.TEXTURE_RGB565)) + textured_quad(200, 200, 300, 300)

    def assert_copy_shown(self, picture):
        self.assert_pixel(picture, 250, 250, self.RED)

    def assert_copy_not_shown(self, picture):
        self.assertNotEqual(self.pixel(picture, 250, 250), self.RED)

    def test_a_copy_shows_as_a_texture(self):
        self.assert_copy_shown(self.picture(textured_state(), self.scene()))

    def test_memory_written_after_the_copy_wins(self):
        commands = self.scene()
        before = flat_quad(self.RED, 0, 0, 64, 64) + copy_picture(self.TARGET, self.COPY_RGB565)
        image = solid_image((0, 252, 0), 64, 64)
        picture = self.picture(textured_state(), commands, [(len(before), self.TARGET, image)])
        self.assert_pixel(picture, 250, 250, (0, 255, 0))

    def test_a_small_memory_write_also_counts(self):
        commands = self.scene()
        before = flat_quad(self.RED, 0, 0, 64, 64) + copy_picture(self.TARGET, self.COPY_RGB565)
        # A quarter of the image in the middle of it, as a game patching a copy might.
        picture = self.picture(textured_state(), commands, [(len(before), self.TARGET + 0x800, bytes(0x800))])
        self.assert_copy_not_shown(picture)

    def test_a_new_copy_works_after_one_was_overwritten(self):
        first = flat_quad(self.GREEN, 0, 0, 64, 64) + copy_picture(self.TARGET, self.COPY_RGB565)
        commands = first + self.scene()
        update = (len(first), self.TARGET, solid_image((0, 0, 0), 64, 64))
        self.assert_copy_shown(self.picture(textured_state(), commands, [update]))

    def test_a_different_size_reads_memory(self):
        commands = self.scene(texture=use_texture(self.TARGET, 32, 32, self.TEXTURE_RGB565))
        picture = self.picture(textured_state(), commands)
        self.assert_copy_not_shown(picture)
        self.assertIn("picture copy read at another size", self.log)

    def test_a_format_of_another_size_reads_memory(self):
        commands = self.scene(texture=use_texture(self.TARGET, 64, 64, self.TEXTURE_I8))
        self.assert_copy_not_shown(self.picture(textured_state(), commands))
        self.assertIn("picture copy read as a format of another size", self.log)

    def test_copies_are_never_cmpr(self):
        commands = self.scene(texture=use_texture(self.TARGET, 64, 64, self.TEXTURE_CMPR))
        self.assert_copy_not_shown(self.picture(textured_state(), commands))

    def test_a_format_of_the_same_size_uses_the_copy_and_says_so(self):
        rgb5a3 = 5
        commands = self.scene(texture=use_texture(self.TARGET, 64, 64, rgb5a3))
        self.assert_copy_shown(self.picture(textured_state(), commands))
        self.assertIn("picture copy read as another format = 0x405", self.log)

    def test_a_copy_landing_on_another_replaces_it(self):
        over = flat_quad(self.GREEN, 0, 0, 64, 64) + copy_picture(self.TARGET + 0x400, self.COPY_RGB565)
        self.assert_copy_not_shown(self.picture(textured_state(), self.scene(over)))

    def test_a_copy_next_to_another_leaves_it(self):
        bytes_in_copy = 16 * 16 * 32
        beside = copy_picture(self.TARGET + bytes_in_copy, self.COPY_RGB565)
        self.assert_copy_shown(self.picture(textured_state(), self.scene(beside)))

    def test_a_copy_with_gaps_between_rows_is_not_read_whole(self):
        made = flat_quad(self.RED, 0, 0, 64, 64) + copy_picture(self.TARGET, self.COPY_RGB565, stride=1024)
        commands = made + use_texture(self.TARGET, 64, 64, self.TEXTURE_RGB565) + textured_quad(200, 200, 300, 300)
        self.assert_copy_not_shown(self.picture(textured_state(), commands))
        self.assertIn("picture copy with gaps between its rows read whole", self.log)

    def test_a_stride_of_zero_copies_nothing(self):
        made = flat_quad(self.RED, 0, 0, 64, 64) + copy_picture(self.TARGET, self.COPY_RGB565, stride=0)
        commands = made + use_texture(self.TARGET, 64, 64, self.TEXTURE_RGB565) + textured_quad(200, 200, 300, 300)
        self.assert_copy_not_shown(self.picture(textured_state(), commands))

    def test_a_report_comes_once(self):
        texture = use_texture(self.TARGET, 32, 32, self.TEXTURE_RGB565)
        commands = self.scene(texture=texture) + textured_quad(310, 200, 400, 300)
        self.picture(textured_state(), commands)
        self.assertEqual(self.log.count("picture copy read at another size"), 1)


class Comparisons(Scene):
    """a and b come from registers 1 and 2, c from register 3, and d is black."""
    SEEN = (10, 20, 30)
    NONE = (0, 0, 0)

    def compared(self, a, b, mode, comparison, alpha_a=0, alpha_b=0):
        state = base_state()
        state[0][0xc0] = color_stage(a=COLOR_R1, b=COLOR_R2, c=COLOR_R3, bias=BIAS_COMPARE, subtract=comparison, scale=mode)
        state[0][0xc1] = alpha_stage(d=A_RASTER)
        commands = register(1, *a, alpha_a) + register(2, *b, alpha_b) + register(3, *self.SEEN, 0) + quad((0, 0, 0))
        return self.pixel(self.picture(state, commands), 200, 200)

    def compared_alpha(self, a, b, mode, comparison, alpha_a=0, alpha_b=0):
        """A second stage shows the first stage's alpha, as a color."""
        state = base_state()
        state[0][0x00] |= 1 << 10
        state[0][0xc0] = color_stage(a=COLOR_R1, b=COLOR_R2, target=3)
        state[0][0xc1] = alpha_stage(a=A_R1, b=A_R2, c=A_R3, bias=BIAS_COMPARE, subtract=comparison, scale=mode)
        state[0][0xc2] = color_stage(d=ALPHA_R0)
        state[0][0xc3] = alpha_stage(d=A_RASTER)
        commands = register(1, *a, alpha_a) + register(2, *b, alpha_b) + register(3, 0, 0, 0, 77) + quad((0, 0, 0))
        return self.pixel(self.picture(state, commands), 200, 200)[0]

    def test_r8(self):
        self.assertEqual(self.compared((5, 0, 0), (4, 255, 255), MODE_R8, GREATER), self.SEEN)
        self.assertEqual(self.compared((4, 0, 0), (5, 0, 0), MODE_R8, GREATER), self.NONE)
        self.assertEqual(self.compared((5, 1, 2), (5, 9, 9), MODE_R8, EQUAL), self.SEEN)
        self.assertEqual(self.compared((5, 1, 2), (6, 1, 2), MODE_R8, EQUAL), self.NONE)

    def test_gr16_has_green_on_top(self):
        self.assertEqual(self.compared((0, 2, 0), (255, 1, 255), MODE_GR16, GREATER), self.SEEN)
        self.assertEqual(self.compared((255, 1, 0), (0, 2, 0), MODE_GR16, GREATER), self.NONE)
        self.assertEqual(self.compared((7, 8, 1), (7, 8, 2), MODE_GR16, EQUAL), self.SEEN)
        self.assertEqual(self.compared((7, 8, 0), (8, 8, 0), MODE_GR16, EQUAL), self.NONE)

    def test_bgr24_has_blue_on_top(self):
        self.assertEqual(self.compared((0, 0, 2), (255, 255, 1), MODE_BGR24, GREATER), self.SEEN)
        self.assertEqual(self.compared((255, 255, 1), (0, 0, 2), MODE_BGR24, GREATER), self.NONE)
        self.assertEqual(self.compared((1, 2, 3), (1, 2, 3), MODE_BGR24, EQUAL), self.SEEN)
        self.assertEqual(self.compared((1, 2, 3), (1, 2, 4), MODE_BGR24, EQUAL), self.NONE)

    def test_rgb8_decides_each_channel_alone(self):
        self.assertEqual(self.compared((5, 1, 9), (4, 2, 9), MODE_RGB8, GREATER), (10, 0, 0))
        self.assertEqual(self.compared((5, 1, 9), (4, 2, 9), MODE_RGB8, EQUAL), (0, 0, 30))

    def test_alpha_forms_compare_the_color_inputs(self):
        self.assertEqual(self.compared_alpha((5, 0, 0), (4, 0, 0), MODE_R8, GREATER), 77)
        self.assertEqual(self.compared_alpha((4, 0, 0), (5, 0, 0), MODE_R8, GREATER), 0)
        self.assertEqual(self.compared_alpha((0, 2, 0), (255, 1, 0), MODE_GR16, GREATER), 77)
        self.assertEqual(self.compared_alpha((1, 2, 3), (1, 2, 3), MODE_BGR24, EQUAL), 77)
        self.assertEqual(self.compared_alpha((1, 2, 3), (1, 2, 4), MODE_BGR24, EQUAL), 0)

    def test_a8_compares_alpha(self):
        self.assertEqual(self.compared_alpha((0, 0, 0), (9, 9, 9), MODE_A8, GREATER, alpha_a=6, alpha_b=5), 77)
        self.assertEqual(self.compared_alpha((9, 9, 9), (9, 9, 9), MODE_A8, EQUAL, alpha_a=6, alpha_b=6), 77)
        self.assertEqual(self.compared_alpha((9, 9, 9), (0, 0, 0), MODE_A8, EQUAL, alpha_a=5, alpha_b=6), 0)

    def test_inputs_compare_by_their_low_byte(self):
        # 300 reads as 44 at a stage's a, b and c.
        self.assertEqual(self.compared((300, 0, 0), (100, 0, 0), MODE_R8, GREATER), self.NONE)

    def test_d_is_added_after_comparing(self):
        state = base_state()
        state[0][0xc0] = color_stage(a=COLOR_ONE, b=ZERO, c=COLOR_R3, d=COLOR_R1, bias=BIAS_COMPARE)
        commands = register(1, 1, 2, 3, 0) + register(3, *self.SEEN, 0) + quad((0, 0, 0))
        self.assertEqual(self.pixel(self.picture(state, commands), 200, 200), (11, 22, 33))


def half_and_half(left=(248, 0, 0), right=(0, 252, 0)):
    """An 8x8 RGB565 image, whose left half and right half differ."""
    tile = rgb565(*left) * 16 + rgb565(*right) * 16
    return tile * 2


def matrix(xf, index, *rows):
    for r, row in enumerate(rows):
        for c, value in enumerate(row):
            xf[index * 4 + r * 4 + c] = float_bits(value)


class Coordinates(Scene):
    RED, GREEN = (255, 0, 0), (0, 255, 0)
    MATRIX = 3

    def state(self, control, *rows):
        """Coordinate 0 made by `control` through a matrix, so u lands on one half of the image or the other."""
        state = textured_state()
        cp, xf, xf_registers = state[1], state[2], state[3]
        cp[0x30] = self.MATRIX << 6
        xf_registers[0x40] = control
        matrix(xf, self.MATRIX, *rows)
        return state

    def image(self):
        return [(0, IMAGE_ADDRESS, half_and_half())]

    def test_the_third_value_divides_at_each_pixel(self):
        # Across the quad, f runs 0 to 1, s = 4f and q = 1 + 3f, so u = 4f / (1 + 3f) reaches 0.5
        # at f = 0.2. Dividing at the vertices gets there at f = 0.5.
        state = self.state(PROJECTED | INPUT_XYZ, (4 / 200, 0, 0, -2), (0, 0, 0, 0), (3 / 200, 0, 0, -0.5))
        picture = self.picture(state, textured_quad(), self.image())
        self.assert_pixel(picture, 136, 200, self.RED)
        self.assert_pixel(picture, 144, 200, self.GREEN)

    def test_a_third_value_of_zero_halves_and_clamps(self):
        state = self.state(PROJECTED | INPUT_XYZ, (0, 0, 0, 0.6), (0, 0, 0, 0), (0, 0, 0, 0))
        self.assert_pixel(self.picture(state, textured_quad(), self.image()), 200, 200, self.RED)

    def test_input_form_sets_z_to_one(self):
        rows = ((0, 0, 1, 0), (0, 0, 0, 0))
        corners = [(100, 100), (300, 100), (300, 300), (100, 300)]
        lifted = draw(QUADS, [struct.pack(">fffff", x, y, 0.25, 0, 0) for x, y in corners])
        self.assert_pixel(self.picture(self.state(INPUT_XYZ, *rows), lifted, self.image()), 200, 200, self.RED)
        self.assert_pixel(self.picture(self.state(0, *rows), lifted, self.image()), 200, 200, self.GREEN)

    def normals_state(self, control):
        """Vertices with three normals in floats, then a coordinate that isn't read."""
        state = textured_state()
        bp, cp, xf, xf_registers, _ = state
        cp[0x50] = (1 << 9) | (1 << 11)
        cp[0x70] = 1 | (4 << 1) | (1 << 9) | (4 << 10) | (1 << 21) | (4 << 22)
        xf_registers[0x40] = control
        return state

    def normals_quad(self, normal, tangent, binormal, left=100, top=100, right=300, bottom=300):
        corners = [(left, top), (right, top), (right, bottom), (left, bottom)]
        return draw(QUADS, [struct.pack(">3f3f3f3f2f", x, y, 0, *normal, *tangent, *binormal, 0, 0) for x, y in corners])

    def test_tangent_and_binormal_are_sources(self):
        lines = self.normals_quad((0.1, 0, 0), (0.25, 0, 0), (0.75, 0, 0))
        tangent, binormal = self.normals_state((3 << 7) | INPUT_XYZ), self.normals_state((4 << 7) | INPUT_XYZ)
        self.assert_pixel(self.picture(tangent, lines, self.image()), 200, 200, self.RED)
        self.assert_pixel(self.picture(binormal, lines, self.image()), 200, 200, self.GREEN)

    def test_a_missing_source_reads_zero(self):
        state = textured_state()
        state[3][0x40] = (3 << 7) | INPUT_XYZ
        self.assert_pixel(self.picture(state, textured_quad(), self.image()), 200, 200, self.RED)

    def emboss_state(self, light_x):
        """Coordinate 1 is coordinate 0 moved towards light 2, and it's the one the stage reads."""
        state = self.normals_state(5 << 7)
        bp, cp, xf, xf_registers, _ = state
        bp[0x00] = 2 | (1 << 4)
        bp[0x28] = (1 << 6) | (1 << 3)
        bp[0x32], bp[0x33] = 7, 7
        xf_registers[0x3f] = 2
        xf_registers[0x41] = (1 << 4) | (0 << 12) | (2 << 15)
        for i in (0, 4, 8):
            xf[0x400 + i] = float_bits(1.0)
        light = 0x600 + 2 * 0x10
        xf[light + 10], xf[light + 11], xf[light + 12] = float_bits(light_x), float_bits(0.0), float_bits(0.0)
        return state

    def test_emboss_moves_a_coordinate_towards_a_light(self):
        def corners(u):
            return draw(QUADS, [struct.pack(">3f3f3f3f2f", x, y, 0, 0, 0, 1, 0.4, 0, 0, 0, 1, 0, u, 0) for x, y in [(100, 100), (300, 100), (300, 300), (100, 300)]])
        # The light is far to the right, so the tangent's 0.4 adds to the coordinate.
        self.assert_pixel(self.picture(self.emboss_state(1e6), corners(0.3), self.image()), 200, 200, self.GREEN)
        # Now to the left, and it subtracts.
        self.assert_pixel(self.picture(self.emboss_state(-1e6), corners(0.7), self.image()), 200, 200, self.RED)

    def test_emboss_keeps_the_last_vertex_tangent_for_vertices_with_none(self):
        state = self.emboss_state(1e6)
        state[2][0x600 + 10] = float_bits(1e6)  # Dolphin uses light 0 when the vertices have no normals.
        loaded = self.normals_quad((0, 0, 1), (0.4, 0, 0), (0, 1, 0), 0, 0, 10, 10)
        plain = cp_command(0x50, 1 << 9)
        vertices = draw(QUADS, [struct.pack(">3f2f", x, y, 0, 0.3, 0) for x, y in [(100, 100), (300, 100), (300, 300), (100, 300)]])
        self.assert_pixel(self.picture(state, loaded + plain + cp_command(0x70, 1 | (4 << 1) | (1 << 21) | (4 << 22)) + vertices, self.image()), 200, 200, self.GREEN)


def uv_quad(u, v=0.5, left=100, top=100, right=300, bottom=300):
    return textured_quad(left, top, right, bottom, uv=[(u, v)] * 4)


class Indirect(Scene):
    """Map 0 is half red and half green over 8 texels, moved by bytes read from map 1."""
    RED, GREEN = (255, 0, 0), (0, 255, 0)
    OFFSET_ADDRESS = 0x00210000
    TEXTURE_IA8 = 3

    def state(self, command, scale_exponent=18, values=(512, 0)):
        """Matrix 0 holds `values` in its first column, scaled by 2 to the power of exponent - 17."""
        state = textured_state()
        bp = state[0]
        bp[0x00] = 1 | (1 << 4) | (1 << 16)
        bp[0x06] = (values[0] & 0x7ff) | ((values[1] & 0x7ff) << 11) | ((scale_exponent & 3) << 22)
        bp[0x07] = ((scale_exponent >> 2) & 3) << 22
        bp[0x08] = ((scale_exponent >> 4) & 3) << 22
        bp[0x10] = command
        bp[0x27] = 1  # The fetch reads map 1 through coordinate 0.
        bp[0x89] = 7 | (7 << 10) | (self.TEXTURE_IA8 << 20)
        bp[0x95] = self.OFFSET_ADDRESS >> 5
        return state

    def images(self, alpha, intensity):
        return [(0, IMAGE_ADDRESS, half_and_half()), (0, self.OFFSET_ADDRESS, bytes([alpha, intensity]) * 64)]

    def test_a_fixed_matrix_moves_the_coordinate(self):
        state = self.state(MATRIX_1 | (1 << 4))  # S has 128 taken off.
        self.assert_pixel(self.picture(state, uv_quad(0.25), self.images(132, 0)), 200, 200, self.GREEN)
        self.assert_pixel(self.picture(state, uv_quad(0.25), self.images(128, 0)), 200, 200, self.RED)

    def test_a_short_format_takes_the_top_bits(self):
        state = self.state(MATRIX_1 | (1 << 2))  # Five bits, and no bias.
        self.assert_pixel(self.picture(state, uv_quad(0.25), self.images(0b00100_000, 0)), 200, 200, self.GREEN)
        self.assert_pixel(self.picture(state, uv_quad(0.25), self.images(0b00000_111, 0)), 200, 200, self.RED)

    def test_s_kind_scales_with_the_coordinate(self):
        # 2 texels * 128 / 256 * 2 to the 2 is 4 texels.
        state = self.state(MATRIX_1 | (1 << 11), scale_exponent=19)
        self.assert_pixel(self.picture(state, uv_quad(0.25), self.images(128, 0)), 200, 200, self.GREEN)
        self.assert_pixel(self.picture(state, uv_quad(0.25), self.images(0, 128)), 200, 200, self.RED)

    def test_t_kind_reads_the_second_byte(self):
        state = self.state(MATRIX_1 | (2 << 11), scale_exponent=19)
        self.assert_pixel(self.picture(state, uv_quad(0.25), self.images(0, 128)), 200, 200, self.GREEN)
        self.assert_pixel(self.picture(state, uv_quad(0.25), self.images(128, 0)), 200, 200, self.RED)

    def test_scaled_kinds_keep_growing_across_the_quad(self):
        state = self.state(MATRIX_1 | (1 << 11), scale_exponent=17)
        # From u 0 to 0.5, 0 to 4 texels, plus half that again, so green begins at u = 2/3 of 0.5.
        picture = self.picture(state, textured_quad(uv=((0, 0.5), (0.5, 0.5), (0.5, 0.5), (0, 0.5))), self.images(128, 0))
        self.assert_pixel(picture, 225, 200, self.RED)
        self.assert_pixel(picture, 240, 200, self.GREEN)

    def test_a_stage_with_no_texture_moves_the_next_ones_coordinate(self):
        state = self.state(MATRIX_1 | (1 << 4) | (6 << 13))  # Wraps S to zero, leaving only the offset.
        bp = state[0]
        bp[0x00] |= 1 << 10
        bp[0x28] = (1 << 6) << 12  # Stage 0 has no texture, and stage 1 reads map 0.
        bp[0x11] = 1 << 20  # Adds on to what stage 0 made.
        bp[0xc0], bp[0xc1] = color_stage(), alpha_stage()
        bp[0xc2], bp[0xc3] = color_stage(d=TEXTURE_COLOR), alpha_stage()
        self.assert_pixel(self.picture(state, uv_quad(0.125), self.images(131, 0)), 200, 200, self.GREEN)

    def test_adding_on_to_a_plain_stage(self):
        state = textured_state()
        bp = state[0]
        bp[0x00] |= (1 << 10)
        bp[0x28] = (1 << 6) | (1 << 18)
        bp[0x11] = (1 << 20) | (6 << 13)  # Stage 1 adds nothing of its own to stage 0's coordinate.
        bp[0xc2], bp[0xc3] = color_stage(d=TEXTURE_COLOR), alpha_stage()
        self.assert_pixel(self.picture(state, uv_quad(0.625), [(0, IMAGE_ADDRESS, half_and_half())]), 200, 200, self.GREEN)

    def bump(self, bump_select, form, alpha, intensity=0, raster=5):
        state = self.state((bump_select << 7) | (form << 2))
        state[0][0x28] = raster << 7
        state[0][0xc0] = color_stage(d=RASTER_COLOR)
        return self.picture(state, uv_quad(0.25), self.images(alpha, intensity))

    def test_bump_alpha_takes_the_fetchs_bits(self):
        self.assert_pixel(self.bump(1, 0, 0x85), 200, 200, (0x80, 0x80, 0x80))
        self.assert_pixel(self.bump(1, 3, 0x15), 200, 200, (0xa8, 0xa8, 0xa8))
        self.assert_pixel(self.bump(2, 1, 0, intensity=0b101), 200, 200, (0xa0, 0xa0, 0xa0))

    def test_normalized_bump_alpha_fills_the_low_bits(self):
        self.assert_pixel(self.bump(1, 0, 0xff, raster=6), 200, 200, WHITE)
        self.assert_pixel(self.bump(1, 0, 0x85, raster=6), 200, 200, (0x84, 0x84, 0x84))


class Vertices(Scene):
    RED, GREEN = (255, 0, 0), (0, 255, 0)
    ARRAY = 0x00220000

    def normals_state(self, control, a_extra=0, normal_kind=2):
        """Vertices with an indexed normal, and a coordinate 0 made from `control`."""
        state = textured_state()
        cp, xf_registers = state[1], state[3]
        cp[0x50] = (1 << 9) | (normal_kind << 11)
        cp[0x70] = 1 | (4 << 1) | (4 << 10) | (1 << 21) | (4 << 22) | a_extra
        cp[0xa1] = self.ARRAY
        cp[0xb1] = 36
        xf_registers[0x40] = control
        return state

    def indexed(self, *indices):
        corners = [(100, 100), (300, 100), (300, 300), (100, 300)]
        return draw(QUADS, [struct.pack(">3f", x, y, 0) + bytes(indices) + struct.pack(">2f", 0, 0) for x, y in corners])

    def updates(self):
        rows = [[0.25] * 9, [0.75] * 9]
        return [(0, IMAGE_ADDRESS, half_and_half()), (0, self.ARRAY, b"".join(struct.pack(">9f", *row) for row in rows))]

    def test_three_indexes_read_one_normal_each(self):
        three = (1 << 9) | (1 << 31)
        tangent = self.normals_state((3 << 7) | INPUT_XYZ, three)
        binormal = self.normals_state((4 << 7) | INPUT_XYZ, three)
        normal = self.normals_state((1 << 7) | INPUT_XYZ, three)
        commands = self.indexed(0, 1, 0)
        self.assert_pixel(self.picture(tangent, commands, self.updates()), 200, 200, self.GREEN)
        self.assert_pixel(self.picture(binormal, commands, self.updates()), 200, 200, self.RED)
        self.assert_pixel(self.picture(normal, commands, self.updates()), 200, 200, self.RED)

    def test_three_indexes_keep_the_vertex_size(self):
        state = self.normals_state((3 << 7) | INPUT_XYZ, (1 << 9) | (1 << 31))
        corners = [(350, 100), (500, 100), (500, 300), (350, 300)]
        second = draw(QUADS, [struct.pack(">3f", x, y, 0) + bytes([0, 1, 0]) + struct.pack(">2f", 0, 0) for x, y in corners])
        picture = self.picture(state, self.indexed(0, 1, 0) + second, self.updates())
        self.assert_pixel(picture, 420, 200, self.GREEN)

    def test_one_index_reads_all_three_together(self):
        state = self.normals_state((3 << 7) | INPUT_XYZ, 1 << 9)
        corners = [(100, 100), (300, 100), (300, 300), (100, 300)]
        commands = draw(QUADS, [struct.pack(">3f", x, y, 0) + bytes([1]) + struct.pack(">2f", 0, 0) for x, y in corners])
        self.assert_pixel(self.picture(state, commands, self.updates()), 200, 200, self.GREEN)

    def test_a_null_position_index_leaves_the_vertex_out(self):
        for index_size, null in ((1, 0xff), (2, 0xffff)):
            state = base_state()
            cp = state[1]
            cp[0x50] = (index_size + 1 << 9) | (1 << 13)
            cp[0xa0] = self.ARRAY
            cp[0xb0] = 12
            points = [(100, 100), (300, 100), (100, 300), (320, 100), (500, 100), (320, 300)]
            array = b"".join(struct.pack(">fff", x, y, 0) for x, y in points)
            indices = [0, 1, null, 2, 3, 4, 5]
            vertices = [i.to_bytes(index_size, "big") + bytes((200, 30, 60, 255)) for i in indices]
            picture = self.picture(state, draw(TRIANGLES, vertices), [(0, self.ARRAY, array)])
            self.assert_pixel(picture, 150, 150, (200, 30, 60))
            self.assert_pixel(picture, 350, 150, (200, 30, 60))

    def lit_state(self, facing_vertex_normals):
        """Vertices lit by a light straight ahead, shown through a stage reading the color."""
        state = base_state()
        cp, xf, xf_registers = state[1], state[2], state[3]
        xf_registers[0x0a] = 0x000000ff
        xf_registers[0x0c] = 0xffffffff
        xf_registers[0x0e] = 2 | (1 << 2) | (2 << 7)  # Lit by light 0, facing it counts.
        xf_registers[0x10] = 1
        for i in (0, 4, 8):
            xf[0x400 + i] = float_bits(1.0)
        xf[0x603] = 0xff0000ff
        xf[0x60c] = float_bits(1e6)
        cp[0x50] = (1 << 9) | (1 << 11)
        cp[0x70] = 1 | (4 << 1) | (4 << 10)
        return state

    def normal_vertices(self, normals, left=0, right=20):
        corners = [(left, 0), (right, 0), (right, 20), (left, 20)]
        return draw(QUADS, [struct.pack(">3f3f", x, y, 0, *normal) for (x, y), normal in zip(corners, normals)])

    def test_a_vertex_with_no_normal_keeps_the_last_one(self):
        away, ahead = (0, 0, -1), (0, 0, 1)
        no_normals = cp_command(0x50, 1 << 9)
        first = self.normal_vertices([away, away, away, ahead])
        # Only the last vertex of the draw before counts.
        picture = self.picture(self.lit_state(True), first + no_normals + draw(QUADS, [struct.pack(">3f", x, y, 0) for x, y in [(100, 100), (300, 100), (300, 300), (100, 300)]]))
        self.assert_pixel(picture, 200, 200, (255, 0, 0))
        first = self.normal_vertices([ahead, ahead, ahead, away])
        picture = self.picture(self.lit_state(True), first + no_normals + draw(QUADS, [struct.pack(">3f", x, y, 0) for x, y in [(100, 100), (300, 100), (300, 300), (100, 300)]]))
        self.assert_pixel(picture, 200, 200, (0, 0, 0))

    def test_a_short_draw_still_leaves_its_normal(self):
        ahead = (0, 0, 1)
        two_vertices = draw(TRIANGLES, [struct.pack(">3f3f", 0, 0, 0, *ahead)] * 2)
        no_normals = cp_command(0x50, 1 << 9)
        plain = draw(QUADS, [struct.pack(">3f", x, y, 0) for x, y in [(100, 100), (300, 100), (300, 300), (100, 300)]])
        self.assert_pixel(self.picture(self.lit_state(True), two_vertices + no_normals + plain), 200, 200, (255, 0, 0))

    def test_unsigned_normals_use_their_own_scale(self):
        state = self.normals_state((1 << 7) | INPUT_XYZ, 0, normal_kind=1)
        state[1][0x70] = 1 | (4 << 1) | (0 << 10) | (1 << 21) | (4 << 22)  # Bytes, unsigned.
        corners = [(100, 100), (300, 100), (300, 300), (100, 300)]
        commands = draw(QUADS, [struct.pack(">3f", x, y, 0) + bytes([96, 0, 0]) + struct.pack(">2f", 0, 0) for x, y in corners])
        self.assert_pixel(self.picture(state, commands, self.updates()), 200, 200, self.GREEN)


class Sampling(Scene):
    """An 8x8 image whose smaller versions each have their own color."""
    LEVELS = [(248, 0, 0), (0, 252, 0), (0, 0, 248), (248, 252, 248)]
    SHOWN = [(255, 0, 0), (0, 255, 0), (0, 0, 255), WHITE]
    MIP_NEAREST, MIP_LINEAR = 1 << 5, 2 << 5
    MINIFY_LINEAR, MAGNIFY_LINEAR = 1 << 7, 1 << 4

    def state(self, mode, largest=3.0, smallest=0.0):
        state = textured_state()
        state[0][0x80] = mode
        state[0][0x84] = int(smallest * 16) | (int(largest * 16) << 8)
        return state

    def image(self):
        data = solid_image(self.LEVELS[0], 8, 8) + solid_image(self.LEVELS[1], 4, 4) + solid_image(self.LEVELS[2], 2, 2) + solid_image(self.LEVELS[3], 1, 1)
        return [(0, IMAGE_ADDRESS, data)]

    def shown(self, state, width):
        """A square `width` of the Wii's pixels across, or twice that of the picture's."""
        return self.pixel(self.picture(state, textured_quad(100, 100, 100 + width, 100 + width), self.image()), 100 + width / 2, 100 + width / 2)

    def test_stored_levels_are_read(self):
        state = self.state(self.MIP_NEAREST)
        self.assertEqual(self.shown(state, 40), self.SHOWN[0])
        self.assertEqual(self.shown(state, 2), self.SHOWN[1])
        self.assertEqual(self.shown(state, 1), self.SHOWN[2])

    def test_levels_follow_whole_tiles(self):
        """I8 tiles are 8 by 4, so a 4x4 or 2x2 level still takes 32 bytes."""
        state = self.state(self.MIP_NEAREST)
        state[0][0x88] = 7 | (7 << 10) | (1 << 20)
        data = bytes([0x20]) * 64 + bytes([0x80]) * 32 + bytes([0xf0]) * 32
        picture = self.picture(state, textured_quad(100, 100, 101, 101), [(0, IMAGE_ADDRESS, data)])
        self.assertEqual(self.pixel(picture, 100.5, 100.5), (240, 240, 240))

    def test_no_mip_filter_reads_only_the_full_image(self):
        self.assertEqual(self.shown(self.state(0), 1), self.SHOWN[0])

    def test_smallest_level_holds_the_full_image_back(self):
        self.assertEqual(self.shown(self.state(self.MIP_NEAREST, smallest=1.0), 40), self.SHOWN[1])

    def test_largest_level_stops_the_smaller_ones(self):
        self.assertEqual(self.shown(self.state(self.MIP_NEAREST, largest=1.0), 1), self.SHOWN[1])

    def test_largest_level_wins_over_smallest(self):
        self.assertEqual(self.shown(self.state(self.MIP_NEAREST, largest=1.0, smallest=2.0), 40), self.SHOWN[1])

    def test_bias_moves_the_level_both_ways(self):
        up, down = 32 << 9, (256 - 32) << 9
        self.assertEqual(self.shown(self.state(self.MIP_NEAREST | up), 4), self.SHOWN[1])
        self.assertEqual(self.shown(self.state(self.MIP_NEAREST | down), 1), self.SHOWN[1])

    def test_bias_needs_a_mip_filter(self):
        self.assertEqual(self.shown(self.state(32 << 9), 4), self.SHOWN[0])

    def test_linear_mip_filter_mixes_levels(self):
        mixed = self.shown(self.state(self.MIP_LINEAR | (16 << 9)), 4)  # Level 0.5.
        self.assertGreater(mixed[0], 90)
        self.assertGreater(mixed[1], 90)

    def stripes(self):
        tile = (rgb565(248, 0, 0) + rgb565(0, 0, 248)) * 8
        return [(0, IMAGE_ADDRESS, tile * 4)]

    def test_minify_filter(self):
        quad = textured_quad(100, 100, 102, 102)
        picture = self.picture(self.state(0), quad, self.stripes())
        self.assertIn(picture.getpixel((201, 201))[0], (0, 255))
        picture = self.picture(self.state(self.MINIFY_LINEAR), quad, self.stripes())
        self.assertAlmostEqual(picture.getpixel((201, 201))[0], 127, delta=2)

    def test_magnify_filter_stays_its_own(self):
        quad = textured_quad(100, 100, 260, 260)
        picture = self.picture(self.state(self.MINIFY_LINEAR), quad, self.stripes())
        self.assertIn(self.pixel(picture, 105, 105)[0], (0, 255))

    def test_wrap_mode_three_clamps(self):
        state = self.state(3)
        wide = textured_quad(uv=((0, 0), (2, 0), (2, 1), (0, 1)))
        picture = self.picture(state, wide, [(0, IMAGE_ADDRESS, half_and_half())])
        self.assert_pixel(picture, 170, 200, (0, 255, 0))
        self.assert_pixel(picture, 280, 200, (0, 255, 0))

    def test_settings_not_done_are_named(self):
        self.shown(self.state(1 << 21), 40)
        self.assertIn("texture level of detail clamp bit", self.log)

    def test_anisotropy_and_diagonal_level_of_detail_take_effect(self):
        # Both are done, so neither is named, and the stripes still show unblurred close up.
        quad = textured_quad(100, 100, 260, 260)
        picture = self.picture(self.state((1 << 19) | (1 << 8) | self.MINIFY_LINEAR), quad, self.stripes())
        self.assertIn(self.pixel(picture, 105, 105)[0], (0, 255))
        self.assertNotIn("unsupported graphics", self.log)


def fog_value(value):
    """A float as fog's registers hold it, with 11 bits of fraction."""
    bits = float_bits(value)
    return ((bits >> 31) << 19) | (((bits >> 23) & 255) << 11) | ((bits >> 12) & 0x7ff)


class Fog(Scene):
    """A white quad at depth 0.5, so 2**23 of the 24-bit range."""
    LINEAR, EXP, BACKWARD_EXP2 = 2, 4, 7
    ORTHOGRAPHIC = 1 << 20

    def fogged(self, a, c, kind=LINEAR, color=(0, 0, 0), orthographic=True, magnitude=0, shift=0, more=None, x=200):
        state = base_state()
        bp = state[0]
        bp[0xee] = a if isinstance(a, int) else fog_value(a)
        bp[0xef], bp[0xf0] = magnitude, shift
        bp[0xf1] = (c if isinstance(c, int) else fog_value(c)) | (self.ORTHOGRAPHIC if orthographic else 0) | (kind << 21)
        bp[0xf2] = (color[0] << 16) | (color[1] << 8) | color[2]
        if more:
            more(bp)
        return self.pixel(self.picture(state, quad(WHITE, 20, 100, 620, 300)), x, 200)

    def test_linear_mixes_toward_the_color(self):
        # A * z - C = 0.5 - 0.25, which is 64 of 256.
        self.assertEqual(self.fogged(1.0, 0.25, color=(255, 0, 0)), (255, 191, 191))

    def test_perspective_divides_by_the_depth(self):
        # A * 2**24 / (B - (z >> 1)) = 0.125 * 2**24 / 2**22 = 0.5.
        self.assertEqual(self.fogged(0.125, 0.0, orthographic=False, magnitude=1 << 23, shift=1), (127, 127, 127))

    def test_exponential(self):
        self.assertEqual(self.fogged(1.0, 0.25, kind=self.EXP), (63, 63, 63))  # 1 - 2**-2.

    def test_backward_exponential_squared(self):
        self.assertEqual(self.fogged(1.0, 0.25, kind=self.BACKWARD_EXP2), (244, 244, 244))  # 2**-4.5 of 256 is 11.

    def test_off_leaves_the_color(self):
        self.assertEqual(self.fogged(1.0, -1.0, kind=0), WHITE)

    def test_infinite_a_and_c_both_positive_fog_fully(self):
        infinite = 255 << 11
        self.assertEqual(self.fogged(infinite, infinite, color=(10, 20, 30)), (10, 20, 30))
        self.assertEqual(self.fogged(infinite, infinite | (1 << 19), color=(10, 20, 30)), WHITE)

    def test_range_adds_distance_toward_the_sides(self):
        def ranged(bp):
            bp[0xe8] = (342 + 320) | (1 << 10)
            for i in range(5):
                bp[0xe9 + i] = 256 | (256 << 12)  # Every value is 4.
        self.assertEqual(self.fogged(1.0, 0.25, more=ranged, x=320), (191, 191, 191))
        offset = ((64 * SCALE + 0.5) / SCALE - 320) / 320
        fog = 0.5 * (offset * offset + 16) ** 0.5 / 4 - 0.25
        shade = 255 * (256 - round(fog * 256)) >> 8
        self.assertEqual(self.fogged(1.0, 0.25, more=ranged, x=64), (shade, shade, shade))


class LogicOps(Scene):
    """A red quad over a blue one, through each logic operation. The blue is drawn, as clears
    can come out a level high on some chips."""
    CLEAR, AND, COPY, NOOP, INVERT, COPY_INVERTED, SET = 0, 1, 3, 5, 10, 12, 15
    BLUE, RED = (10, 20, 200), (200, 30, 60)

    def drawn(self, mode, blend=0):
        commands = quad(self.BLUE) + bp_command(0x41, 0x18 | 2 | (mode << 12) | blend) + quad(self.RED)
        return self.pixel(self.picture(base_state(), commands), 200, 200)

    def test_exact_operations(self):
        self.assertEqual(self.drawn(self.CLEAR), (0, 0, 0))
        self.assertEqual(self.drawn(self.SET), WHITE)
        self.assertEqual(self.drawn(self.COPY), self.RED)
        self.assertEqual(self.drawn(self.COPY_INVERTED), (55, 225, 195))
        self.assertEqual(self.drawn(self.NOOP), self.BLUE)
        self.assertEqual(self.drawn(self.INVERT), (245, 235, 55))

    def test_blending_wins(self):
        self.assertEqual(self.drawn(self.CLEAR, blend=1 | (1 << 8)), self.RED)  # Source times one, target times zero.

    def test_approximations_are_named(self):
        self.drawn(self.AND)
        self.assertIn("logic operation, approximated by blending = 0x1", self.log)


class LinesAndPoints(Scene):
    LINES, LINE_STRIP, POINTS = 0xa8, 0xb0, 0xb8
    BLUE, RED = (10, 20, 200), (200, 30, 60)

    def drawn(self, kind, points, line=36, point=0, more=None):
        """Sizes are in sixths of a pixel, so 36 is 6 pixels."""
        state = base_state()
        state[0][0x22] = line | (point << 8)
        if more:
            more(state)
        background = quad(self.BLUE, 0, 0, 640, 456)
        return self.picture(state, background + draw(kind, [vertex(x, y, self.RED) for x, y in points]))

    def assert_drawn(self, picture, inside, outside):
        for x, y in inside:
            self.assert_pixel(picture, x, y, self.RED)
        for x, y in outside:
            self.assert_pixel(picture, x, y, self.BLUE)

    def test_a_wide_line_is_widened_up_and_down(self):
        picture = self.drawn(self.LINES, [(100, 200), (300, 200)])
        self.assert_drawn(picture, [(200, 197), (200, 202.5), (100.5, 200), (299.5, 200)], [(200, 196.5), (200, 203.5), (99.5, 200), (300.5, 200)])

    def test_a_tall_line_is_widened_sideways(self):
        picture = self.drawn(self.LINES, [(200, 100), (200, 300)])
        self.assert_drawn(picture, [(197, 200), (202.5, 200), (200, 100.5), (200, 299.5)], [(196.5, 200), (203.5, 200), (200, 99.5), (200, 300.5)])

    def test_a_slanted_line_has_square_ends(self):
        # More wide than tall, so its ends are cut straight up and down.
        picture = self.drawn(self.LINES, [(100, 100), (300, 200)])
        self.assert_drawn(picture, [(150, 125), (100.5, 102), (299.5, 197.5)], [(99.5, 100), (300.5, 200), (150, 120)])

    def test_strips_join_each_vertex_to_the_next(self):
        picture = self.drawn(self.LINE_STRIP, [(100, 100), (300, 100), (300, 300)])
        self.assert_drawn(picture, [(200, 100), (300, 200)], [(200, 200)])

    def test_points_are_squares(self):
        picture = self.drawn(self.POINTS, [(200, 200), (400, 300)], point=60)
        self.assert_drawn(picture, [(195.5, 195.5), (204.5, 204.5), (400, 300)], [(194.5, 200), (205.5, 200), (200, 205.5), (300, 250)])

    def test_culling_leaves_them_alone(self):
        def cull_all(state):
            state[0][0x00] |= 3 << 14
        picture = self.drawn(self.LINES, [(100, 200), (300, 200)], more=cull_all)
        self.assert_pixel(picture, 200, 200, self.RED)

    def test_texture_offsets_are_named(self):
        def offsets(state):
            state[0][0x22] |= 1 << 16
            state[0][0x30] |= 1 << 18
        self.drawn(self.LINES, [(100, 200), (300, 200)], more=offsets)
        self.assertIn("line texture offset", self.log)


def xf_command(address, *values):
    return bytes([0x10]) + struct.pack(">I", ((len(values) - 1) << 16) | address) + b"".join(struct.pack(">I", v) for v in values)


class DepthRange(Scene):
    """Quads sit at the middle of the chip's z, -0.5 of -1 to 0."""
    RED, GREEN = (200, 30, 60), (20, 180, 40)
    HALF = 1 << 23

    def depth_range(self, z_range, far):
        return xf_command(0x101c, float_bits(z_range)) + xf_command(0x101f, float_bits(far))

    def test_range_moves_what_fog_sees(self):
        state = base_state()
        bp, xf_registers = state[0], state[3]
        xf_registers[0x1c] = xf_registers[0x1f] = float_bits(self.HALF)  # 0 to a half, so the quad is at a quarter.
        bp[0xee], bp[0xf1] = fog_value(1.0), fog_value(0.0) | (1 << 20) | (2 << 21)
        self.assert_pixel(self.picture(state, quad(WHITE)), 200, 200, (191, 191, 191))

    def scene(self, z_range, far):
        state = base_state()
        state[0][0x40] = 1 | (1 << 1) | (1 << 4)  # Passes when less, and writes.
        return self.picture(state, quad(self.RED) + self.depth_range(z_range, far) + quad(self.GREEN, 150, 150, 350, 350))

    def test_a_nearer_range_passes(self):
        picture = self.scene(self.HALF, self.HALF)  # A quarter, in front of a half.
        self.assert_pixel(picture, 200, 200, self.GREEN)

    def test_a_farther_range_fails(self):
        picture = self.scene(self.HALF, 0xffffff)  # From a half to all the way, so three quarters.
        self.assert_pixel(picture, 200, 200, self.RED)
        self.assert_pixel(picture, 320, 320, self.GREEN)

    def test_past_the_buffer_is_named(self):
        self.scene(self.HALF, 2.0 * 0xffffff)
        self.assertIn("viewport depth past the depth buffer", self.log)

    def depth_through(self, *betweens):
        """Four frames: a near red quad, then one of `betweens` in turn, then a far green quad tested against it.
        A pass whose draws leave depth alone runs with a scratch depth picture, so the real one
        must come through it untouched, frame after frame, as each frame learns from the last."""
        state = base_state()
        state[0][0x40] = 1 | (1 << 1) | (1 << 4)  # So the player's opening clear clears depth too.
        tested = bp_command(0x40, 1 | (1 << 1) | (1 << 4))
        near = tested + quad(self.RED)  # In front, at a quarter.
        far = tested + self.depth_range(self.HALF, 0xffffff) + quad(self.GREEN, 150, 150, 350, 350) + self.depth_range(self.HALF, self.HALF)
        recording, out = self.folder / "scene.dff", self.folder / "frames"
        write_dff(recording, state, [(near + betweens[i % len(betweens)] + far + show_frame(), []) for i in range(4)])
        result = replay(recording, out)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.log = result.stderr
        for i in range(1, 4):  # The first frame starts from the player's own opening clear.
            picture = Image.open(out / f"frame-{i:04d}.png").convert("RGB")
            self.assert_pixel(picture, 200, 200, self.RED)  # The far quad stays behind.
            self.assert_pixel(picture, 320, 320, self.GREEN)

    # A copy ends the pass, as copies do, then a draw across the whole picture with depth off
    # and nothing written opens one of its own.
    COPY = copy_picture(0x00300000, 0, width=8, height=8, stride=64)
    UNSEEN = COPY + bp_command(0x40, 0) + bp_command(0x41, 0) + quad(WHITE, 0, 0, 640, 456) + bp_command(0x41, 0x18)

    def test_depth_outlives_a_pass_that_never_uses_it(self):
        self.depth_through(self.UNSEEN + self.COPY)

    def test_a_pass_without_depth_ends_when_a_draw_needs_it(self):
        # Frames alternate, so the pass after the copy left depth alone in the last one, and
        # each frame with the far quad straight after it finds it needs depth after all.
        self.depth_through(self.UNSEEN + self.COPY, self.UNSEEN)


class KeptDrawState(Scene):
    """A draw keeps what the last one worked out from the registers until one is written."""
    RED, GREEN = (200, 30, 60), (20, 180, 40)

    def test_a_new_matrix_moves_the_next_draw(self):
        # Only matrix memory changes between the two draws.
        moved = xf_command(0, float_bits(1.0), 0, 0, float_bits(300.0))
        picture = self.picture(base_state(), quad(self.RED) + moved + quad(self.GREEN))
        self.assert_pixel(picture, 200, 200, self.RED)
        self.assert_pixel(picture, 500, 200, self.GREEN)

    def test_a_new_stage_color_reaches_the_next_draw(self):
        state = base_state()
        state[0][0xc0] = color_stage(d=COLOR_R1)  # The stage gives register 1, not the vertex color.
        picture = self.picture(state, register(1, *self.RED, 255) + quad(WHITE) + register(1, *self.GREEN, 255) + quad(WHITE, 400, 100, 600, 300))
        self.assert_pixel(picture, 200, 200, self.RED)
        self.assert_pixel(picture, 500, 200, self.GREEN)


class CoveredPasses(Scene):
    """A pass whose first draw covers all of its area starts without loading what was there."""
    RED, GREEN = (200, 30, 60), (20, 180, 40)
    ORIGIN = 342

    def scissor(self, left, top, right, bottom):
        o = self.ORIGIN
        return bp_command(0x20, (o + top) | ((o + left) << 12)) + bp_command(0x21, (o + bottom - 1) | ((o + right - 1) << 12))

    def scene(self, green_right):
        # Red everywhere, a copy to end the pass, then green over a scissored square.
        state = base_state()
        state[0][0x41] = 0x18  # Color and alpha written, nothing blended.
        whole = self.scissor(0, 0, 640, 456)
        commands = (quad(self.RED, 0, 0, 640, 456) + copy_picture(0x00300000, 0, width=8, height=8, stride=64)
                    + self.scissor(100, 100, 300, 300) + quad(self.GREEN, 100, 100, green_right, 300) + whole)
        return self.picture(state, commands)

    def test_a_covering_draw_replaces_the_area(self):
        picture = self.scene(300)
        self.assert_pixel(picture, 200, 200, self.GREEN)
        self.assert_pixel(picture, 50, 50, self.RED)

    def test_a_draw_over_part_of_it_leaves_the_rest(self):
        picture = self.scene(200)
        self.assert_pixel(picture, 150, 200, self.GREEN)
        self.assert_pixel(picture, 250, 200, self.RED)  # In the pass, but not drawn over.


class DestinationAlpha(Scene):
    """Draws into the corner, copies it with its alpha, and shows that alpha as gray."""
    TARGET = 0x00300000
    RGB8, RGBA6 = 0, 1
    COPY_RGBA8, TEXTURE_RGBA8 = 12 << 3, 6

    def drawn(self, color, alpha, blend=0x18, constant=None):
        settings = bp_command(0x41, blend) + bp_command(0x42, (1 << 8) | constant if constant is not None else 0)
        return (settings + bp_command(0xc0, color_stage(d=COLOR_R1)) + bp_command(0xc1, alpha_stage(d=A_R1)) + register(1, *color, alpha)
                + textured_quad(0, 0, 64, 64))

    def alpha_seen(self, pixel_format, *draws):
        state = textured_state()
        state[0][0x43] = pixel_format
        shown = (bp_command(0x41, 0x18) + bp_command(0x42, 0) + copy_picture(self.TARGET, self.COPY_RGBA8, stride=1024) + bp_command(0xc0, color_stage(d=TEXTURE_ALPHA))
                 + bp_command(0xc1, alpha_stage(d=A_TEXTURE)) + use_texture(self.TARGET, 64, 64, self.TEXTURE_RGBA8) + textured_quad(200, 200, 300, 300))
        picture = self.picture(state, b"".join(draws) + shown)
        self.corner = self.pixel(picture, 30, 30)
        return self.pixel(picture, 250, 250)[0]

    def test_alpha_is_kept_with_rgba6(self):
        self.assertEqual(self.alpha_seen(self.RGBA6, self.drawn((200, 30, 60), 100)), 100)

    def test_constant_alpha_replaces_it(self):
        self.assertEqual(self.alpha_seen(self.RGBA6, self.drawn((200, 30, 60), 100, constant=50)), 50)

    def test_a_picture_without_alpha_copies_it_as_full(self):
        self.assertEqual(self.alpha_seen(self.RGB8, self.drawn((200, 30, 60), 100, constant=50)), 255)

    def test_without_alpha_target_alpha_blends_as_one(self):
        # Alpha 0 kept while the format had it, then red blended by the target's alpha.
        source_target_alpha = 1 | (6 << 8) | (0 << 5) | 0x18
        self.alpha_seen(self.RGBA6, self.drawn((10, 20, 200), 0), bp_command(0x43, self.RGB8), self.drawn((200, 30, 60), 255, blend=source_target_alpha))
        self.assertEqual(self.corner, (200, 30, 60))

    def test_subtracting_leaves_a_constant_alpha_alone(self):
        subtract = 1 | (1 << 11) | 0x18
        seen = self.alpha_seen(self.RGBA6, self.drawn((200, 30, 60), 200), self.drawn((10, 10, 10), 0, blend=subtract, constant=50))
        self.assertEqual(seen, 50)
        self.assertEqual(self.corner, (190, 20, 50))

    def test_a_no_op_still_writes_a_constant_alpha(self):
        no_op = 2 | (5 << 12) | 0x18
        seen = self.alpha_seen(self.RGBA6, self.drawn((200, 30, 60), 200), self.drawn((10, 10, 10), 0, blend=no_op, constant=50))
        self.assertEqual(seen, 50)
        self.assertEqual(self.corner, (200, 30, 60))


if __name__ == "__main__":
    unittest.main()
