// The Zelda family's audio program (zelda.c), with made-up tables and samples. The
// mailbox is faked so each test sees exactly the mail the program sends. Expected values
// are worked out by hand from Dolphin's Zelda.cpp, step by step in the comments.
#include "../../recomp/runtime/zelda.c"
#include "../harness.h"
#include <string.h>

uint32_t hw_read(uint32_t address, int bytes) { (void)address; (void)bytes; return 0; }
void hw_write(uint32_t address, uint32_t value, int bytes) { (void)address; (void)value; (void)bytes; }

static struct { uint32_t mail; int interrupt; } sent[64];
static int sent_count, interrupts, expect_new, set_to_rom, resume_wanted, gaps;
static uint32_t crc_now;
static const char *last_gap;
static uint32_t last_gap_detail;

void dsp_push_mail(uint32_t mail, int interrupt, uint32_t cycles_later) {
  (void)cycles_later;
  sent[sent_count % 64].mail = mail;
  sent[sent_count % 64].interrupt = interrupt;
  sent_count++;
}
static int sent_later;  // How many of those the program sent after working a while.
void dsp_push_mail_later(uint32_t mail, int interrupt, uint32_t cycles_later) {
  if (cycles_later) sent_later++;
  dsp_push_mail(mail, interrupt, 0);
}
void dsp_mail_interrupt(uint32_t cycles_later) { (void)cycles_later; interrupts++; }
uint32_t dsp_program_crc(void) { return crc_now; }
void dsp_expect_new_program(void) { expect_new++; }
void dsp_set_program(uint32_t crc) { if (crc == PROGRAM_ROM) set_to_rom++; }
int dsp_needs_resume_mail(void) { int wanted = resume_wanted; resume_wanted = 0; return wanted; }
uint64_t time_base(void) { return 0; }
void stage_add(int stage, uint64_t ticks) {}
void gap_hit(const char *id, uint32_t detail) { gaps++; last_gap = id; last_gap_detail = detail; }

#define GALAXY 0xd643001fu
#define PIKMIN_GC 0x4be6a5cbu  // Light protocol.

static void start(uint32_t crc) {
  crc_now = crc;
  sent_count = 0;
  zelda_program.start();
}

static void send(uint32_t mail) { zelda_program.mail(mail); }

static void check_sent(int index, uint32_t mail, int interrupt, int line) {
  if (index >= sent_count || sent[index].mail != mail || sent[index].interrupt != interrupt) {
    printf("FAILED line %d: mail %d is %08x/%d of %d, wanted %08x/%d\n", line, index,
           index < sent_count ? sent[index].mail : 0, index < sent_count ? sent[index].interrupt : 0, sent_count, mail, interrupt);
    failures++;
  }
}
#define SENT(index, mail, interrupt) check_sent(index, mail, interrupt, __LINE__)

static void vpb_word(uint32_t vpb, int word, uint32_t value) { wr16(vpb + word * 2, value); }

// The normal protocol says it's ready with an interrupt, then shakes hands; the light one
// sends a single mail.
static void test_start_mails(void) {
  start(GALAXY);
  CHECK_EQ(sent_count, 2);
  SENT(0, DSP_MAIL_INIT, 1);
  SENT(1, 0xf3551111u, 0);
  CHECK_EQ(u.flags, NO_ARAM | MAKE_DOLBY_LOUDER | COMBINED_CMD_0D);
  start(PIKMIN_GC);
  CHECK_EQ(sent_count, 1);
  SENT(0, 0x88881111u, 0);
  CHECK_EQ(u.flags, LIGHT_PROTOCOL | NO_CMD_0D | SUPPORTS_GBA_CRYPTO);
  gaps = 0;
  start(0x12345678u);  // No flags: reported, and run as the normal protocol.
  CHECK_EQ(gaps, 1);
  SENT(0, DSP_MAIL_INIT, 1);
}

// A header gives the number of words; the command runs after the last and is answered
// with a sync and F355 plus its top half. A leftover word without the top bit is skipped
// when the next command is read.
static void test_word_count_and_ack(void) {
  start(GALAXY);
  sent_count = 0;
  send(0x00000002u);
  send(0x80000000u);  // Command 00, sync 8000.
  CHECK_EQ(sent_count, 0);
  send(0x00000005u);  // Its second word, which it never reads.
  CHECK_EQ(sent_count, 2);
  SENT(0, DSP_MAIL_SYNC, 1);
  SENT(1, 0xf3558000u, 0);
  send(0x00000001u);
  send(0x8f120000u);  // Command 0F reads past the leftover word.
  CHECK_EQ(sent_count, 4);
  SENT(2, DSP_MAIL_SYNC, 1);
  SENT(3, 0xf3558f12u, 0);
}

#define TABLE 0x80010000u
#define AFC_TABLE 0x80011000u
#define VPBS 0x80020000u
#define SAMPLES_AT 0x80030000u
#define LEFT_OUT 0x80040000u
#define RIGHT_OUT 0x80040200u

static void setup_command(uint16_t voices, uint32_t reverb) {
  send(0x00000005u);
  send(0x81000000u | voices);
  send(VPBS);
  send(TABLE);
  send(AFC_TABLE);
  send(reverb);
}

// Command 01 takes the voice blocks' address and copies the tables: 0x100 resampling
// coefficients, 0x100 pattern samples after them, 0x80 sine values after those, and 0x20
// AFC coefficients from their own address.
static void test_setup_command(void) {
  for (uint32_t i = 0; i < 0x280; i++) wr16(TABLE + i * 2, i * 7 + 3);
  for (uint32_t i = 0; i < 0x20; i++) wr16(AFC_TABLE + i * 2, (uint32_t)(-(int32_t)i * 100));
  start(GALAXY);
  sent_count = 0;
  setup_command(3, 0);
  CHECK_EQ(sent_count, 2);
  SENT(0, DSP_MAIL_SYNC, 1);
  SENT(1, 0xf3558100u, 0);
  CHECK_EQ(u.rendering_voices_per_frame, 3);
  CHECK_EQ(r.vpb_base_addr, VPBS);
  CHECK_EQ((uint16_t)r.resampling_coeffs[0], 3);
  CHECK_EQ((uint16_t)r.resampling_coeffs[0xff], 0xff * 7 + 3);
  CHECK_EQ((uint16_t)r.const_patterns[0], 0x100 * 7 + 3);
  CHECK_EQ((uint16_t)r.const_patterns[0xff], 0x1ff * 7 + 3);
  CHECK_EQ((uint16_t)r.sine_table[0], 0x200 * 7 + 3);
  CHECK_EQ((uint16_t)r.sine_table[0x7f], 0x27f * 7 + 3);
  CHECK_EQ(r.afc_coeffs[0], 0);
  CHECK_EQ((uint16_t)r.afc_coeffs[0x1f], (uint16_t)-3100);
  CHECK_EQ(r.reverb_pb_base_addr, 0);
}

static int16_t sample_at(int j) { return (int16_t)((j - 40) * 200); }

// One voice of PCM16 from main memory at ratio 1.0 (0x1000). With the fraction 0 the
// resampler uses coefficients 0-3, here 0x1000, 0x2000, 0, 0x4000, over raw samples
// k..k+3, where raw 0-3 are the kept history (zero) and raw 4+j is sample j:
//   in[k] = (2 * (0x1000 raw[k] + 0x2000 raw[k+1] + 0x4000 raw[k+3])) >> 16
//         = (raw[k] + 2 raw[k+1] + 4 raw[k+3]) >> 3.
// Channel 0 (front left, 0x0d00) holds 0x4000: adds (0x4000 in) >> 16 = in >> 2.
// Channel 1 (front right, 0x0d60) ramps 0 to 0x5000: step (0x5000 << 16) / 0x50 =
// 0x01000000, so sample k adds ((k * 0x100) in) >> 16 = (k in) >> 8, ending at 0x5000.
// Output volume 0x2000 in 4.12 doubles each.
static int16_t resampled(int k) {
  int raw[0x54] = {0};
  for (int j = 0; j < 0x50; j++) raw[4 + j] = sample_at(j);
  return (int16_t)((raw[k] + 2 * raw[k + 1] + 4 * raw[k + 3]) >> 3);
}

static void test_render(void) {
  memset(host(TABLE), 0, 0x500);
  wr16(TABLE + 0, 0x1000);
  wr16(TABLE + 2, 0x2000);
  wr16(TABLE + 6, 0x4000);
  memset(host(VPBS), 0, 0x180);
  for (int j = 0; j < 0x50; j++) wr16(SAMPLES_AT + j * 2, (uint16_t)sample_at(j));
  vpb_word(VPBS, 0x00, 1);       // Enabled.
  vpb_word(VPBS, 0x02, 0x1000);  // Ratio 1.0.
  vpb_word(VPBS, 0x08, 0x0d00); vpb_word(VPBS, 0x09, 0x4000); vpb_word(VPBS, 0x0a, 0x4000);
  vpb_word(VPBS, 0x0c, 0x0d60); vpb_word(VPBS, 0x0d, 0x5000); vpb_word(VPBS, 0x0e, 0);
  vpb_word(VPBS, 0x3b, 0x1000);  // Remaining length.
  vpb_word(VPBS, 0x80, 33);      // PCM16 from main memory.
  vpb_word(VPBS, 0x8a, 0x1000);  // Loop start.
  vpb_word(VPBS, 0x8c, SAMPLES_AT >> 16); vpb_word(VPBS, 0x8d, SAMPLES_AT & 0xffff);

  start(GALAXY);
  setup_command(1, 0);
  sent_count = 0;
  gaps = 0;
  send(0x00000005u);
  send(0x82012000u);  // Render one frame at volume 0x2000.
  send(LEFT_OUT);
  send(RIGHT_OUT);
  send(0x11111111u);  // Command 0D's values, skipped.
  send(0x22222222u);
  CHECK_EQ(sent_count, 0);  // Waiting for the game to unlock voices.
  CHECK(gaps == 1 && !strcmp(last_gap, "audio.zelda_command_0d"));
  send(0x00000000u);  // A sync mail...
  send(0x00008000u);  // ...unlocking voices 0-15, of which voice 0 plays.
  CHECK_EQ(sent_count, 3);
  SENT(0, DSP_MAIL_SYNC, 1);
  SENT(1, 0xf355ff01u, 0);  // The first frame done, counted from 1 as the real program does.
  SENT(2, DSP_MAIL_FRAME_END, 1);
  CHECK_EQ(sent_later, 1);  // After finishing the mix, not with the frame's acknowledgment.

  // A few by hand. k = 1: (4 * -8000) >> 3 = -4000; left 2 * (-4000 >> 2) = -2000;
  // right 2 * ((1 * -4000) >> 8) = 2 * -16 = -32. k = 4: (-8000 - 15600 - 29600) >> 3
  // = -6650; left 2 * -1663, right 2 * (-26600 >> 8) = 2 * -104. k = 0x4f: (7000 + 14400
  // + 30400) >> 3 = 6475; left 2 * 1618, right 2 * (511525 >> 8) = 2 * 1998.
  CHECK_EQ(rd16(LEFT_OUT), 0);
  CHECK_EQ(rd16(LEFT_OUT + 2), (uint16_t)-2000);
  CHECK_EQ(rd16(RIGHT_OUT + 2), (uint16_t)-32);
  CHECK_EQ(rd16(LEFT_OUT + 8), (uint16_t)-3326);
  CHECK_EQ(rd16(RIGHT_OUT + 8), (uint16_t)-208);
  CHECK_EQ(rd16(LEFT_OUT + 0x9e), 3236);
  CHECK_EQ(rd16(RIGHT_OUT + 0x9e), 3996);
  CHECK_EQ(resampled(0x4f), 6475);
  for (int k = 0; k < 0x50; k++) {
    int in = resampled(k);
    CHECK_EQ(rd16(LEFT_OUT + k * 2), (uint16_t)(2 * (in >> 2)));
    CHECK_EQ(rd16(RIGHT_OUT + k * 2), (uint16_t)(2 * ((k * in) >> 8)));
  }
  CHECK_EQ(r.output_lbuf_addr, LEFT_OUT + 0xa0);

  // The block goes back: 0x50 samples on, the last four raw samples kept, the last input
  // sample as the constant, and channel 1 at its target.
  CHECK_EQ(rd16(VPBS + 0x34 * 2), 0x50);
  CHECK_EQ(rd16(VPBS + 0x3b * 2), 0x1000 - 0x50);
  CHECK_EQ(rd16(VPBS + 0x36 * 2), 0x1000);  // Samples before the loop.
  CHECK_EQ(rd16(VPBS + 0x3c * 2), (uint16_t)sample_at(0x4c));
  CHECK_EQ(rd16(VPBS + 0x3f * 2), (uint16_t)sample_at(0x4f));
  CHECK_EQ(rd16(VPBS + 0x33 * 2), 6475);
  CHECK_EQ(rd16(VPBS + 0x0a * 2), 0x4000);
  CHECK_EQ(rd16(VPBS + 0x0e * 2), 0x5000);
  CHECK_EQ(rd16(VPBS + 0x30 * 2), 0);

  // Commands now wait for the game's answer.
  send(0x00000001u);
  send(0x80000000u);
  CHECK_EQ(sent_count, 3);
  send(TASK_CONTINUE);
  CHECK_EQ(sent_count, 5);
  SENT(3, DSP_MAIL_SYNC, 1);
  SENT(4, 0xf3558000u, 0);

  // A game that's behind sends its next render, and the first frame's sync mails, before
  // "continue". They're kept for that render rather than halting the program.
  for (int round = 0; round < 2; round++) {  // The first render ends waiting for "continue".
    sent_count = 0;
    gaps = 0;
    send(0x00000005u);
    send(0x82012000u);
    send(LEFT_OUT);
    send(RIGHT_OUT);
    send(0x11111111u);
    send(0x22222222u);
    send(0x00000000u);
    send(0x00008000u);
  }
  CHECK_EQ(sent_count, 0);  // Still waiting for "continue".
  CHECK_EQ(gaps, 0);         // Nothing unexpected, and the render hasn't run yet.
  send(TASK_CONTINUE);
  CHECK_EQ(sent_count, 3);
  SENT(0, DSP_MAIL_SYNC, 1);
  SENT(1, 0xf355ff01u, 0);
  SENT(2, DSP_MAIL_FRAME_END, 1);
  CHECK_EQ(gaps, 1);

  // With nothing waiting, a sync mail still halts and is named.
  send(TASK_CONTINUE);
  sent_count = 0;
  send(0x00000000u);
  CHECK(!strcmp(last_gap, "audio.zelda_unexpected_mail") && last_gap_detail == 0x05);
}

// AFC with four-bit samples. Header 0x21: scale 1 << 2 = 4, coefficient pair 1, here
// 0x400 and -0x200. Samples 1, 7, 15, 8 become s16(n << 12) >> 1: 2048, 14336, -2048,
// -16384. Each is (4 n + 1024 yn1 - 512 yn2) >> 11:
//   8192 -> 4; 57344 + 4096 -> 30; -8192 + 30720 - 2048 -> 10; -65536 + 10240 - 15360
//   = -70656 -> -35; -35840 - 5120 -> -20; -20480 + 17920 -> -2; -2048 + 10240 -> 4;
//   4096 + 1024 -> 2; 2048 - 2048 -> 0; -1024 -> -1; then -1024 + 512 -> -1 to the end.
// The second block's header 0xf0 makes the scale 1 << 15, which as s16 is -32768, with
// pair 0 (zeros): 7 -> clamped -32768, 15 -> clamped 32767, 1 -> -32768 exactly, then 0.
static const int16_t afc_hq_block[16] = {4, 30, 10, -35, -20, -2, 4, 2, 0, -1, -1, -1, -1, -1, -1, -1};
static const int16_t afc_hq_block2[16] = {-32768, 32767, -32768, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

#define ARAM 0x80050000u

static void afc_setup(void) {
  start(GALAXY);
  memset(r.afc_coeffs, 0, sizeof r.afc_coeffs);
  r.afc_coeffs[2] = 0x400;
  r.afc_coeffs[3] = -0x200;
  r.aram_base_addr = ARAM;
  memset(&vpb_scratch, 0, sizeof vpb_scratch);
}

static void test_afc_4_bit(void) {
  static const uint8_t blocks[18] = {0x21, 0x17, 0xf8, 0, 0, 0, 0, 0, 0, 0xf0, 0x7f, 0x10, 0, 0, 0, 0, 0, 0};
  afc_setup();
  memcpy(host(ARAM + 0x100), blocks, sizeof blocks);
  Vpb *vpb = &vpb_scratch;
  vpb->samples_source_type = SRC_AFC_HQ_FROM_ARAM;
  vpb->current_aram_addr_l = 0x100;
  int16_t out[32];
  decode_afc(vpb, out, 2);
  for (int i = 0; i < 16; i++) CHECK_EQ((uint16_t)out[i], (uint16_t)afc_hq_block[i]);
  for (int i = 0; i < 16; i++) CHECK_EQ((uint16_t)out[16 + i], (uint16_t)afc_hq_block2[i]);
  CHECK_EQ(vpb->current_aram_addr_l, 0x100 + 18);
  CHECK_EQ(vpb->afc_remaining_samples[AFC_YN1], 0);

  // Through the download: a reset voice of 0x20 samples that doesn't loop. 0x50 are
  // asked for, so both blocks decode, the rest is silence and the voice is done. Dolphin
  // leaves the remaining length as it was on this path.
  memset(vpb, 0, sizeof *vpb);
  vpb->samples_source_type = SRC_AFC_HQ_FROM_ARAM;
  vpb->reset_vpb = 1;
  vpb->loop_start_position_l = 0x20;
  vpb->base_address_l = 0x100;
  int16_t raw[0x60];
  memset(raw, 0x55, sizeof raw);
  download_afc_samples_from_aram(raw, vpb, 0x50);
  for (int i = 0; i < 16; i++) CHECK_EQ((uint16_t)raw[i], (uint16_t)afc_hq_block[i]);
  for (int i = 0; i < 16; i++) CHECK_EQ((uint16_t)raw[16 + i], (uint16_t)afc_hq_block2[i]);
  for (int i = 32; i < 0x50; i++) CHECK_EQ(raw[i], 0);
  CHECK_EQ(raw[0x50], 0x5555);
  CHECK_EQ(vpb->done, 1);
  CHECK_EQ(vpb->remaining_length_l, 0x20);
  CHECK_EQ(vpb->current_aram_addr_l, 0x100 + 18);
}

// AFC with two-bit samples. Header 0x31: scale 8, pair 1. Bytes 0x1b and 0xe4 give
// 0 1 2 3 3 2 1 0, then zeros; s16(n << 14) >> 1 makes them 0, 8192, -16384, -8192.
// Each is (8 n + 1024 yn1 - 512 yn2) >> 11:
//   0; 65536 -> 32; -131072 + 32768 -> -48; -65536 - 49152 - 16384 -> -64;
//   -65536 - 65536 + 24576 -> -52; -131072 - 53248 + 32768 -> -74;
//   65536 - 75776 + 26624 -> 8; 8192 + 37888 = 46080 -> 22; 22528 - 4096 -> 9;
//   9216 - 11264 -> -1; -1024 - 4608 -> -3; -3072 + 512 -> -2; -2048 + 1536 -> -1;
//   -1024 + 1024 -> 0; 512 -> 0; 0.
static void test_afc_2_bit(void) {
  static const uint8_t block[5] = {0x31, 0x1b, 0xe4, 0, 0};
  static const int16_t want[16] = {0, 32, -48, -64, -52, -74, 8, 22, 9, -1, -3, -2, -1, 0, 0, 0};
  afc_setup();
  memcpy(host(ARAM + 0x200), block, sizeof block);
  Vpb *vpb = &vpb_scratch;
  vpb->samples_source_type = SRC_AFC_LQ_FROM_ARAM;
  vpb->current_aram_addr_l = 0x200;
  int16_t out[16];
  decode_afc(vpb, out, 1);
  for (int i = 0; i < 16; i++) CHECK_EQ((uint16_t)out[i], (uint16_t)want[i]);
  CHECK_EQ(vpb->current_aram_addr_l, 0x205);
}

// A new program: queued commands run first, then the next ten mails are dsp.c's.
static void test_new_program(void) {
  start(GALAXY);
  u.cmd_can_execute = 0;  // As after a frame.
  send(0x00000001u);
  send(0x80000000u);
  sent_count = 0;
  expect_new = 0;
  send(TASK_NEW_PROGRAM);
  CHECK_EQ(sent_count, 2);
  SENT(1, 0xf3558000u, 0);
  CHECK_EQ(expect_new, 1);
  CHECK_EQ(u.mail_state, MAIL_WAITING);
}

// Reset goes back to the boot ROM. Mail after that is reported, as the program has halted.
static void test_reset(void) {
  start(GALAXY);
  set_to_rom = 0;
  gaps = 0;
  send(TASK_RESET);
  CHECK_EQ(set_to_rom, 1);
  CHECK_EQ(u.mail_state, MAIL_HALTED);
  send(0x00000001u);
  CHECK(gaps == 1 && !strcmp(last_gap, GAP_MAIL) && last_gap_detail == 6);
  resume_wanted = 1;
  sent_count = 0;
  zelda_program.update();  // Swapped back in: it says it's resumed.
  SENT(0, DSP_MAIL_RESUME, 1);
}

// The light protocol knows each command's length and answers with its handler's address:
// 2 * (command & 0x7f) + 0x62. Rendering waits for one zero mail, then renders every
// voice and raises an interrupt.
static void test_light_protocol(void) {
  start(PIKMIN_GC);
  sent_count = 0;
  send(0x80000000u);  // 00: no more words.
  SENT(0, 0x80000062u, 0);
  memset(host(VPBS), 0, 0x180);
  send(0x81000001u);  // 01: four more.
  send(VPBS);
  send(TABLE);
  CHECK_EQ(sent_count, 1);
  send(AFC_TABLE);
  send(0);
  SENT(1, 0x80000064u, 0);
  send(0x82010000u | 0x1000);  // 02: two more. Its answer uses the frame count, 1.
  send(LEFT_OUT);
  send(RIGHT_OUT);
  SENT(2, 0x80000062u, 0);
  CHECK_EQ(u.mail_state, MAIL_RENDERING);
  interrupts = 0;
  send(0);
  CHECK_EQ(interrupts, 1);
  CHECK_EQ(sent_count, 3);
  CHECK_EQ(u.mail_state, MAIL_WAITING);
  CHECK_EQ(u.rendering_curr_frame, 1);
}

int main(void) {
  memory_start();
  test_start_mails();
  test_word_count_and_ack();
  test_setup_command();
  test_render();
  test_afc_4_bit();
  test_afc_2_bit();
  test_new_program();
  test_reset();
  test_light_protocol();
  return finish();
}
