// The audio processor's mailboxes, control bits and program slot (dsp.c), with made-up
// programs: the OS's wake-up, the boot ROM's hand-over, the queue's interrupt rules and
// swapping programs. Programs are named by a checksum of their bytes, so the test builds
// bytes with the checksum it wants.
#include "../../recomp/runtime/dsp.c"
#include "../harness.h"
#include <string.h>

static uint64_t now_ticks = 1000000;
uint64_t time_base(void) { return now_ticks; }
static int gaps, interrupts;
static const char *last_gap;
void gap_hit(const char *id, uint32_t detail) { gaps++; last_gap = id; }
void dsp_interrupt(void) { interrupts++; }
// The interpreter, which these made-up programs never need.
DspLle dsp_lle;
const unsigned char data_dsp_rom[1], data_dsp_coef[1];
int dsp_lle_start(const uint8_t *rom, const uint8_t *coef) { return 0; }
int dsp_lle_run(int cycles) { return 0; }
int dsp_lle_halted(void) { return 1; }
void dsp_lle_boot(uint32_t a, uint32_t n, uint16_t d, uint16_t pc) {}
uint16_t dsp_lle_mailbox_read_high(int cpu) { return 0; }
uint16_t dsp_lle_mailbox_read_low(int cpu) { return 0; }
void dsp_lle_mailbox_write_high(int cpu, uint16_t v) {}
void dsp_lle_mailbox_write_low(int cpu, uint16_t v) {}
uint16_t dsp_lle_control_write(uint16_t v) { return 0; }
uint16_t dsp_lle_control_read(void) { return 0; }

// A stand-in for the AX program, which only remembers what it was told.
static int ax_starts;
static uint32_t ax_mails[16], ax_mail_count;
static void fake_start(void) { ax_starts++; }
static void fake_mail(uint32_t mail) {
  ax_mails[ax_mail_count++ % 16] = mail;
  if (mail == TASK_NEW_PROGRAM) dsp_expect_new_program();
}
static void fake_update(void) {}
const DspProgram ax_wii_program = {"fake AX", fake_start, fake_mail, fake_update};
static int zelda_starts;
static void fake_zelda_start(void) { zelda_starts++; }
const DspProgram zelda_program = {"fake Zelda", fake_zelda_start, fake_mail, fake_update};

enum { MAILBOX = 0xcc005000u, FROM = 0xcc005004u };
#define AX_CRC 0x2ea36ce6u
#define CARD_CRC 0x65d6cc6fu
#define CODE_BYTES 16u

// The checksum only ever exclusive-ors and rotates, so each bit of the result is the
// exclusive-or of some input bits. Solving those 32 equations gives bytes for any checksum.
static void bytes_for(uint32_t crc, uint8_t *bytes) {
  uint32_t column[CODE_BYTES * 8], pivot_of[32];
  for (uint32_t i = 0; i < CODE_BYTES; i++) {
    for (uint32_t bit = 0; bit < 8; bit++) {
      uint32_t turn = 3 * (CODE_BYTES - i) % 32, value = 1u << bit;
      column[i * 8 + bit] = turn ? value << turn | value >> (32 - turn) : value;
    }
  }
  uint8_t chosen[CODE_BYTES * 8] = {0};
  uint64_t equations[32][3] = {{0}};  // Per output bit: which of the 128 input bits make it, then its target.
  for (uint32_t r = 0; r < 32; r++) {
    for (uint32_t c = 0; c < CODE_BYTES * 8; c++) {
      if (column[c] >> r & 1) equations[r][c / 64] |= 1ull << (c % 64);
    }
    equations[r][2] = crc >> r & 1;
  }
  uint32_t rank = 0;
  for (uint32_t c = 0; c < CODE_BYTES * 8 && rank < 32; c++) {
    uint32_t found = 32;
    for (uint32_t r = rank; r < 32; r++) {
      if (equations[r][c / 64] >> (c % 64) & 1) { found = r; break; }
    }
    if (found == 32) continue;
    for (int w = 0; w < 3; w++) { uint64_t t = equations[rank][w]; equations[rank][w] = equations[found][w]; equations[found][w] = t; }
    for (uint32_t r = 0; r < 32; r++) {
      if (r != rank && (equations[r][c / 64] >> (c % 64) & 1)) {
        for (int w = 0; w < 3; w++) equations[r][w] ^= equations[rank][w];
      }
    }
    pivot_of[rank++] = c;
  }
  for (uint32_t r = 0; r < rank; r++) chosen[pivot_of[r]] = (uint8_t)equations[r][2];
  memset(bytes, 0, CODE_BYTES);
  for (uint32_t c = 0; c < CODE_BYTES * 8; c++) bytes[c / 8] |= (uint8_t)(chosen[c] << (c % 8));
}

static uint32_t read_mail(void) {
  uint32_t high, low;
  dsp_read(FROM, 2, &high);
  dsp_read(FROM + 2, 2, &low);
  return high << 16 | low;
}

static void send(uint32_t mail) {
  dsp_write(MAILBOX, mail >> 16, 2);
  dsp_write(MAILBOX + 2, mail & 0xffff, 2);
}

static uint32_t place_code(uint32_t address, uint32_t crc) {
  uint8_t bytes[CODE_BYTES];
  bytes_for(crc, bytes);
  memcpy(host(address), bytes, CODE_BYTES);
  return address;
}

static void test_hash(void) {
  const uint8_t three[] = {1, 2, 3};
  CHECK_EQ(dsp_hash(three, 3), 0x298);  // By hand: 1 -> 8, 8^2 -> 0x50, 0x50^3 -> 0x298.
  uint8_t bytes[CODE_BYTES];
  bytes_for(0xd643001fu, bytes);
  CHECK_EQ(dsp_hash(bytes, CODE_BYTES), 0xd643001fu);
}

// The OS wakes the processor as __OSInitAudioSystem does: halted, reset, start-up
// program run by clearing the init bit, then the halt lifted to read its mail.
static void test_wake_up(void) {
  dsp_start();
  CHECK_EQ(dsp_control_read(), DSP_CONTROL_HALT | DSP_CONTROL_INIT);
  uint32_t high;
  dsp_read(FROM, 2, &high);
  CHECK_EQ(high & 0x8000, 0);  // Halted: the ROM's mail can't be seen yet.
  dsp_control_write(DSP_CONTROL_HALT | DSP_CONTROL_INIT | DSP_CONTROL_RESET);
  CHECK_EQ(dsp_control_read() & DSP_CONTROL_RESET, 0);  // Done at once.
  dsp_control_write(DSP_CONTROL_HALT);  // Init cleared: the start-up program runs.
  CHECK(dsp_control_read() & DSP_CONTROL_INIT_CODE);
  now_ticks += INIT_CODE_TICKS;
  CHECK_EQ(dsp_control_read() & DSP_CONTROL_INIT_CODE, 0);
  dsp_control_write(0);  // Running.
  CHECK_EQ(read_mail(), 0x80544348u);
  dsp_read(FROM, 2, &high);
  CHECK_EQ(high, 0x0054);  // Read: the full bit drops, the rest stays.
  CHECK_EQ(interrupts, 0);
}

// The boot ROM answers mail it doesn't expect, then takes the program and hands over.
static void test_boot(void) {
  dsp_control_write(DSP_CONTROL_RESET);
  CHECK_EQ(read_mail(), 0x8071feedu);
  send(0x12345678u);
  CHECK_EQ(read_mail(), 0xfeee5678u);
  uint32_t code = place_code(0x00100000u, AX_CRC);
  send(0x80f3a001u); send(code);
  send(0x80f3a002u); send(CODE_BYTES);
  send(0x80f3c002u); send(0);
  send(0x80f3b002u); send(0);
  CHECK_EQ(ax_starts, 0);
  send(0x80f3d001u); send(0x10);
  CHECK_EQ(ax_starts, 1);
  CHECK_EQ(dsp_program_crc(), AX_CRC);
  send(0xbabe0040u);
  CHECK_EQ(ax_mails[0], 0xbabe0040u);
  uint32_t to;
  dsp_read(MAILBOX, 2, &to);
  CHECK_EQ(to & 0x8000, 0);  // Taken at once.
  CHECK_EQ(gaps, 0);
}

// An interrupt comes with its mail if nothing waits, after its delay if it has one, and
// otherwise when the mail before it is read.
static void test_interrupts(void) {
  interrupts = 0;
  dsp_push_mail(1, 1, 0);
  CHECK_EQ(interrupts, 1);
  read_mail();
  dsp_push_mail(2, 1, 2500);
  CHECK_EQ(interrupts, 1);
  CHECK(dsp_next_due() > now_ticks);
  now_ticks += 2500 / CPU_CYCLES_PER_TICK + 1;
  dsp_update(now_ticks);
  CHECK_EQ(interrupts, 2);
  CHECK_EQ(dsp_next_due(), 0);
  dsp_push_mail(3, 0, 0);  // 2 and 3 now wait.
  dsp_push_mail(4, 1, 0);  // Comes when 2, at the front, is read.
  CHECK_EQ(interrupts, 2);
  CHECK_EQ(read_mail(), 2);
  CHECK_EQ(interrupts, 3);
  CHECK_EQ(read_mail(), 3);
  CHECK_EQ(read_mail(), 4);
  CHECK_EQ(interrupts, 3);
  CHECK_EQ(read_mail(), 4 & ~MAIL_FULL);  // Nothing new: the last mail again.
}

// The game describes the next program in ten mails. The memory card program runs, and
// hands back to the ROM; the old program waits to be swapped back in.
static void test_new_program(void) {
  uint32_t card = place_code(0x00200000u, CARD_CRC);
  send(TASK_NEW_PROGRAM);
  CHECK(dsp_taking_new_program());
  uint32_t mails[10] = {0, 0, 0, card, CODE_BYTES, 0, 0x10, 0, 0, 0};
  for (int i = 0; i < 10; i++) send(mails[i]);
  CHECK(!dsp_taking_new_program());
  CHECK_EQ(dsp_program_crc(), CARD_CRC);
  CHECK_EQ(read_mail(), DSP_MAIL_INIT);
  CHECK_EQ(parked.crc, AX_CRC);
  send(0xff000000u);  // Unlock. Its "done" goes as the ROM starts afresh, as in Dolphin's CARD.cpp.
  CHECK_EQ(dsp_program_crc(), PROGRAM_ROM);
  CHECK_EQ(read_mail(), 0x8071feedu);
  swap_program(AX_CRC);  // The parked one comes back as it was, not started again.
  CHECK_EQ(ax_starts, 1);
  CHECK_EQ(dsp_program_crc(), AX_CRC);
  CHECK(dsp_needs_resume_mail());
  CHECK(!dsp_needs_resume_mail());
  CHECK_EQ(gaps, 0);
}

// A program with no C version names its inventory row; a known one starts.
static void test_unknown(void) {
  dsp_set_program(0x12345678u);
  CHECK_EQ(gaps, 1);
  CHECK(!strcmp(last_gap, "audio.unknown_program"));
  dsp_set_program(0xd643001fu);  // Super Mario Galaxy's.
  CHECK_EQ(zelda_starts, 1);
  CHECK_EQ(gaps, 1);
}

int main(void) {
  memory_start();
  test_hash();
  test_wake_up();
  test_boot();
  test_interrupts();
  test_new_program();
  test_unknown();
  return finish();
}
