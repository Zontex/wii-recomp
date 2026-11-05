// The AX programs (ax.c), GameCube and Wii, with made-up voices, lists and filter tables.
// The mailbox is faked so each test sees the mail the program sends. Expected values are
// worked out by hand from Dolphin's AX.cpp, AXWii.cpp and AXVoice.h, as the comments say:
// these are the parts Mario Kart Wii's recordings don't reach, so they're checked against
// Dolphin's code only.
#include "../../recomp/runtime/ax.c"
#include "../harness.h"

uint32_t hw_read(uint32_t address, int bytes) { (void)address; (void)bytes; return 0; }
void hw_write(uint32_t address, uint32_t value, int bytes) { (void)address; (void)value; (void)bytes; }
const unsigned char data_dsp_coef[4096];  // Zeros; tests that need a table write coefficients[].

static uint32_t sent[16];
static int sent_count, gaps;
static uint32_t crc_now;
static const char *last_gap;
void dsp_push_mail(uint32_t mail, int interrupt, uint32_t cycles_later) {
  (void)interrupt;
  (void)cycles_later;
  sent[sent_count++ % 16] = mail;
}
uint32_t dsp_program_crc(void) { return crc_now; }
void dsp_expect_new_program(void) {}
void dsp_set_program(uint32_t crc) { (void)crc; }
int dsp_needs_resume_mail(void) { return 0; }
uint64_t time_base(void) { return 0; }
void stage_add(int stage, uint64_t ticks) { (void)stage; (void)ticks; }
void gap_hit(const char *id, uint32_t detail) { (void)detail; gaps++; last_gap = id; }
void gap_note(const char *id, uint32_t detail) { gap_hit(id, detail); }

#define LIST 0x1000u
#define PB 0x2000u
#define ZEROS 0x3000u     // A setup array of zeros: every bus starts empty.
#define SURROUND 0x5000u
#define OUTPUT 0x6000u
#define SAMPLES 0x40000u  // 16-bit samples; the accelerator counts them in words.

static void put16(uint32_t address, uint32_t value) { wr16(address, value); }
static int16_t sample_at(uint32_t address) { return (int16_t)rd16(address); }

static void start(uint32_t crc) {
  memory_start();
  crc_now = crc;
  sent_count = gaps = 0;
  ax_program.start();
}

static void run_list(const uint16_t *list, int words) {
  for (int i = 0; i < words; i++) put16(LIST + i * 2, list[i]);
  sent_count = 0;
  ax_program.mail(0xbabe0000u | (uint32_t)words);
  ax_program.mail(LIST);
}

// Where a word of Dolphin's largest Wii block (AXPBWii) is in memory, for each layout.
enum { NEWEST, MIDDLE, OLDEST };
static uint32_t wii_word(int layout, int index) {
  if (layout == OLDEST) return index < 106 ? index : index - 6;  // No room for the biquad.
  if (index < 53) return index;
  if (layout == NEWEST || index < 106) return index - 5;          // No updates.
  return index - 11;
}
enum {  // Indexes in AXPBWii.
  W_SRC_TYPE = 4, W_MIXER_LO = 7, W_RUNNING = 8, W_MAIN_LEFT = 10, W_MAIN_RIGHT = 12, W_UPDATES = 53, W_UPDATES_DATA = 56,
  W_VOLUME = 58, W_FORMAT = 61, W_LOOP = 62, W_END = 64, W_CURRENT = 66, W_GAIN = 84, W_RATIO = 88, W_FRACTION = 90,
  W_LAST = 91, W_LPF = 98, W_BIQUAD = 102,
};

// One voice of `count` 16-bit samples, all `value`, to main left and right at full volume.
static void wii_voice(int layout, int16_t value, int count, int src_type) {
  for (int i = 0; i < 160; i++) put16(PB + i * 2, 0);
  for (int i = 0; i < count; i++) put16(SAMPLES + i * 2, (uint16_t)value);
#define W(index, v) put16(PB + wii_word(layout, index) * 2, v)
  W(W_RUNNING, 1);
  W(W_SRC_TYPE, (uint16_t)src_type);
  W(W_MIXER_LO, 3);
  W(W_MAIN_LEFT, 0x8000);
  W(W_MAIN_RIGHT, 0x8000);
  W(W_VOLUME, 0x8000);  // Unsigned on the Wii: one.
  W(W_FORMAT, 0x0a);
  W(W_GAIN, 0x800);     // 16-bit PCM at a gain of one.
  uint32_t first = SAMPLES / 2;
  W(W_LOOP, first >> 16); W(W_LOOP + 1, first & 0xffff);
  W(W_END, (first + 0x8000) >> 16); W(W_END + 1, (first + 0x8000) & 0xffff);
  W(W_CURRENT, first >> 16); W(W_CURRENT + 1, first & 0xffff);
  W(W_RATIO, 1); W(W_RATIO + 1, 0);  // 1.0
  for (int i = 0; i < 4; i++) W(W_LAST + i, (uint16_t)value);
#undef W
}

static uint32_t wii_read(int layout, int index) { return rd16(PB + wii_word(layout, index) * 2); }

static const uint16_t newest_list[] = {0x00, 0, ZEROS, 0x04, 0, PB, 0x0b, 0x8000, 0, SURROUND, 0, OUTPUT, 0x0e};

// The four-tap filter from the table: with each phase's taps {0, 0, 0, 0x4000} and a ratio
// of one, every output is half the newest input (AXVoice.h, ResampleAudio).
static void test_polyphase(void) {
  start(0x347112bau);
  for (int i = 0; i < 0x800; i += 4) coefficients[i + 3] = 0x4000;
  wii_voice(NEWEST, 1000, 200, SRC_POLYPHASE);
  run_list(newest_list, sizeof newest_list / 2);
  CHECK_EQ(sent_count, 2);
  CHECK_EQ(sent[0], DSP_MAIL_SYNC);  // The output command, then the yield.
  CHECK_EQ(sent[1], DSP_MAIL_YIELD);
  CHECK_EQ(sample_at(OUTPUT), 500);      // Right,
  CHECK_EQ(sample_at(OUTPUT + 2), 500);  // then left.
  CHECK_EQ(sample_at(OUTPUT + 95 * 4 + 2), 500);
  CHECK_EQ(wii_read(NEWEST, W_CURRENT + 1), (SAMPLES / 2 + 96) & 0xffff);
}

// Straight lines at half the rate: 48 samples read for 96, and a constant stays constant,
// as s0 * (1 - f) + s1 * f.
static void test_linear(void) {
  start(0x347112bau);
  wii_voice(NEWEST, 1000, 200, SRC_LINEAR);
  put16(PB + wii_word(NEWEST, W_RATIO) * 2, 0);
  put16(PB + wii_word(NEWEST, W_RATIO + 1) * 2, 0x8000);
  run_list(newest_list, sizeof newest_list / 2);
  CHECK_EQ(sample_at(OUTPUT + 2), 1000);
  CHECK_EQ(sample_at(OUTPUT + 95 * 4 + 2), 1000);
  CHECK_EQ(wii_read(NEWEST, W_CURRENT + 1), (SAMPLES / 2 + 48) & 0xffff);
  CHECK_EQ(wii_read(NEWEST, W_FRACTION), 0);
}

// The newest versions' biquad, b0 = 1/8 after its shift by two: half, rounded to even.
static void test_biquad_and_low_pass(void) {
  start(0x347112bau);
  wii_voice(NEWEST, 1000, 200, SRC_NEAREST);
  put16(SAMPLES, 1001);
  put16(SAMPLES + 2, 1003);
  put16(PB + wii_word(NEWEST, W_BIQUAD) * 2, 1);
  put16(PB + wii_word(NEWEST, W_BIQUAD + 5) * 2, 0x2000);  // b0
  run_list(newest_list, sizeof newest_list / 2);
  CHECK_EQ(sample_at(OUTPUT + 2), 500);      // 500.5 rounds to 500,
  CHECK_EQ(sample_at(OUTPUT + 4 + 2), 502);  // 501.5 to 502,
  CHECK_EQ(sample_at(OUTPUT + 8 + 2), 500);  // and 500 is 500.
  CHECK_EQ((int16_t)wii_read(NEWEST, W_BIQUAD + 1), 1000);  // xn1, the last input.

  // The low-pass, y = (x + y1) / 2 with a0 = b0 = 0x4000, ahead of the biquad.
  start(0x347112bau);
  wii_voice(NEWEST, 1000, 200, SRC_NEAREST);
  put16(PB + wii_word(NEWEST, W_LPF) * 2, 1);
  put16(PB + wii_word(NEWEST, W_LPF + 2) * 2, 0x4000);
  put16(PB + wii_word(NEWEST, W_LPF + 3) * 2, 0x4000);
  run_list(newest_list, sizeof newest_list / 2);
  CHECK_EQ(sample_at(OUTPUT + 2), 500);
  CHECK_EQ(sample_at(OUTPUT + 4 + 2), 750);
  CHECK_EQ(sample_at(OUTPUT + 8 + 2), 875);

  // The middle versions have a high-pass filter there instead, which isn't run: reported.
  start(0xadbc06bdu);
  wii_voice(MIDDLE, 1000, 200, SRC_NEAREST);
  put16(PB + wii_word(MIDDLE, W_BIQUAD) * 2, 1);
  run_list(newest_list, sizeof newest_list / 2);
  CHECK_EQ(sample_at(OUTPUT + 2), 1000);
  CHECK(last_gap && !strcmp(last_gap, "audio.axwii_high_pass_filter"));
}

// Each layout writes the block back where it read it, and not past its end.
static void test_layouts(void) {
  static const struct { uint32_t crc; int layout, words; } versions[] = {
    {0x347112bau, NEWEST, 150}, {0xadbc06bdu, MIDDLE, 144}, {0xfa450138u, OLDEST, 149},
  };
  static const uint16_t oldest_list[] = {0x00, 0, ZEROS, 0x04, 0, PB, 0x05, 0x0c, 0, SURROUND, 0, OUTPUT, 0x0f};
  for (int v = 0; v < 3; v++) {
    start(versions[v].crc);
    wii_voice(versions[v].layout, 1000, 200, SRC_NEAREST);
    put16(PB + versions[v].words * 2, 0xbeef);
    if (versions[v].layout == OLDEST) run_list(oldest_list, sizeof oldest_list / 2);
    else run_list(newest_list, sizeof newest_list / 2);
    CHECK_EQ(sample_at(OUTPUT + 2), 1000);
    CHECK_EQ(wii_read(versions[v].layout, W_CURRENT + 1), (SAMPLES / 2 + 96) & 0xffff);
    CHECK_EQ((int16_t)wii_read(versions[v].layout, W_LAST + 3), 1000);
    CHECK_EQ(rd16(PB + versions[v].words * 2), 0xbeef);
    CHECK_EQ(sent[0], DSP_MAIL_SYNC);
  }
}

// The oldest versions' updates: one at the first millisecond sets the envelope to a half,
// which then holds for the frame.
static void test_updates(void) {
  static const uint16_t oldest_list[] = {0x00, 0, ZEROS, 0x04, 0, PB, 0x05, 0x0c, 0, SURROUND, 0, OUTPUT, 0x0f};
  start(0x7699af32u);
  wii_voice(OLDEST, 1000, 200, SRC_NEAREST);
  put16(PB + W_UPDATES * 2, 1);
  put16(PB + W_UPDATES_DATA * 2, 0);
  put16(PB + (W_UPDATES_DATA + 1) * 2, 0x7000);
  put16(0x7000, W_VOLUME);  // Word, new value.
  put16(0x7002, 0x4000);
  run_list(oldest_list, sizeof oldest_list / 2);
  CHECK_EQ(sample_at(OUTPUT + 2), 500);
  CHECK_EQ(sample_at(OUTPUT + 95 * 4 + 2), 500);
}

// Command 8 sends aux A and aux C's right to the game, and mixes four buffers back into
// left, right, surround and aux C's left; the Dolby output then uploads aux C's left.
// Command 13 writes the four Wii Remote mixes, here a count from 7 the setup started.
static void test_aux_uploads_and_remotes(void) {
  start(0x347112bau);
  uint32_t setup = 0x3800;
  for (int i = 0; i < 60; i++) put16(setup + i * 2, 0);
  put16(setup + 3 * 6 + 2, 100);  // Aux A left starts at 100.
  put16(setup + 12 * 6 + 2, 7);   // Remote 0's main at 7, one more a sample.
  put16(setup + 12 * 6 + 4, 1);
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 96; j++) wr32(0x8000 + i * 0x200 + j * 4, 50);
  }
  uint16_t list[] = {0x00, 0, (uint16_t)setup, 0x08, 0x8000, 0, 0x9000, 0, 0xa000, 0, 0x8000, 0, 0x8200, 0, 0x8400, 0, 0x8600,
                     0x0c, 0x8000, 0, SURROUND, 0, OUTPUT, 0x0d, 0, 0xb000, 0, 0xb100, 0, 0xb200, 0, 0xb300, 0x0e};
  wii_voice(NEWEST, 0, 200, SRC_NEAREST);
  put16(PB + W_RUNNING * 2, 0);
  run_list(list, sizeof list / 2);
  CHECK_EQ(rd32(0x9000), 100);              // Aux A left, uploaded.
  CHECK_EQ(rd32(0x9000 + 96 * 4), 0);       // Aux A right.
  CHECK_EQ(sample_at(OUTPUT + 2), 50);      // Mixed back at full volume.
  CHECK_EQ(sample_at(OUTPUT), 50);
  CHECK_EQ(rd32(SURROUND), 50);
  CHECK_EQ(rd32(SURROUND + 96 * 4), 50);    // Aux C's left, after the surround.
  CHECK_EQ(sample_at(0xb000), 7);
  CHECK_EQ(sample_at(0xb000 + 17 * 2), 24);
  CHECK_EQ(sample_at(0xb100), 0);
}

// The GameCube version: 5 ms frames of 160 samples, a signed envelope (0x7fff, just under
// one), no sync mail, and its own command numbers.
enum {  // Indexes in AXPB.
  G_SRC_TYPE = 4, G_MIXER = 6, G_RUNNING = 7, G_MAIN_LEFT = 9, G_MAIN_RIGHT = 11, G_VOLUME = 50, G_FORMAT = 56, G_LOOP = 57,
  G_END = 59, G_CURRENT = 61, G_GAIN = 79, G_RATIO = 83, G_LPF = 93,
};
static void gc_voice(int mixer) {
  for (int i = 0; i < 130; i++) put16(PB + i * 2, 0);
  for (int i = 0; i < 400; i++) put16(SAMPLES + i * 2, 1000);
  put16(PB + G_RUNNING * 2, 1);
  put16(PB + G_SRC_TYPE * 2, SRC_NEAREST);
  put16(PB + G_MIXER * 2, (uint16_t)mixer);
  put16(PB + G_MAIN_LEFT * 2, 0x8000);
  put16(PB + G_MAIN_RIGHT * 2, 0x8000);
  put16(PB + G_VOLUME * 2, 0x7fff);
  put16(PB + G_FORMAT * 2, 0x0a);
  put16(PB + G_GAIN * 2, 0x800);
  uint32_t first = SAMPLES / 2;
  put16(PB + G_LOOP * 2, first >> 16); put16(PB + (G_LOOP + 1) * 2, first);
  put16(PB + G_END * 2, (first + 0x8000) >> 16); put16(PB + (G_END + 1) * 2, first + 0x8000);
  put16(PB + G_CURRENT * 2, first >> 16); put16(PB + (G_CURRENT + 1) * 2, first);
  put16(PB + G_RATIO * 2, 1);
}

static void test_gamecube(void) {
  static const uint16_t list[] = {0x00, 0, ZEROS, 0x02, 0, PB, 0x03, 0x0e, 0, SURROUND, 0, OUTPUT, 0x0f};
  start(0x07f88145u);
  gc_voice(3);
  run_list(list, sizeof list / 2);
  CHECK_EQ(sent_count, 1);
  CHECK_EQ(sent[0], DSP_MAIL_YIELD);
  CHECK_EQ(sample_at(OUTPUT + 2), 999);  // 1000 * 0x7fff >> 15.
  CHECK_EQ(sample_at(OUTPUT + 159 * 4), 999);
  CHECK_EQ(rd16(PB + (G_CURRENT + 1) * 2), (SAMPLES / 2 + 160) & 0xffff);

  // The oldest GameCube version mixes left and right whatever its control says, and its
  // blocks have no low-pass filter: that word is the loop counter, and nothing filters.
  start(0x4e8a8b21u);
  gc_voice(0);
  put16(PB + G_LPF * 2, 1);
  put16(PB + (G_LPF + 2) * 2, 0x4000);
  run_list(list, sizeof list / 2);
  CHECK_EQ(sample_at(OUTPUT + 2), 999);
  CHECK_EQ(sample_at(OUTPUT + 4 + 2), 999);

  // An unknown command ends the list, reported.
  start(0x07f88145u);
  static const uint16_t unknown[] = {0x77, 0x0f};
  run_list(unknown, 2);
  CHECK(last_gap && !strcmp(last_gap, "unlisted.audio.ax_command"));
  CHECK_EQ(sent[0], DSP_MAIL_YIELD);
}

// Where we differ from Dolphin, as the real program does (measured on Mario Kart Wii and
// Wii Fit Plus): a playing voice clears the depop value of every channel it isn't mixed
// into, main, aux and Wii Remote, and keeps the last sample in those it is.
static void test_unmixed_depop_cleared(void) {
  start(0x4cc52064u);
  wii_voice(NEWEST, 1000, 200, SRC_NEAREST);
  put16(PB + W_MIXER_LO * 2, 1);                    // Main left only.
  put16(PB + 45 * 2, 5);                            // dpop main right, left from an earlier use.
  put16(PB + 49 * 2, 7);                            // dpop main surround.
  put16(PB + wii_word(NEWEST, 113) * 2, 1);         // A remote channel asked for,
  put16(PB + wii_word(NEWEST, 130) * 2, 0xfffd);    // with -3 left in its depop,
  put16(PB + wii_word(NEWEST, 112) * 2, 0);         // but the voice isn't sent to the remotes.
  run_list(newest_list, sizeof newest_list / 2);
  CHECK_EQ((int16_t)rd16(PB + 41 * 2), 1000);       // Main left: the last sample.
  CHECK_EQ(rd16(PB + 45 * 2), 0);
  CHECK_EQ(rd16(PB + 49 * 2), 0);
  CHECK_EQ(rd16(PB + wii_word(NEWEST, 130) * 2), 0);
  CHECK_EQ(sample_at(OUTPUT), 0);                   // Right isn't mixed.
  CHECK_EQ(sample_at(OUTPUT + 2), 1000);
}

int main(void) {
  test_unmixed_depop_cleared();
  test_polyphase();
  test_linear();
  test_biquad_and_low_pass();
  test_layouts();
  test_updates();
  test_aux_uploads_and_remotes();
  test_gamecube();
  return finish();
}
