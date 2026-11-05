// The sound recorder (dsp_record.c) around the interpreter running a made-up program, as
// dsp.c drives them: the game's mail, the program's memory reads and writes through DMA,
// its mails and interrupts. Writes the recording to the file named by the second argument,
// for test_recomp_sound.py to play back through recomp/tools/sound_replay.c.
#include "../harness.h"
#include <string.h>

#include "dsp_lle.h"
#include "dsp_record.h"
#define TICKS_PER_FRAME (60750000ull / 60)

static uint64_t now_ticks = 1000000;
uint64_t time_base(void) { return now_ticks; }
static int gaps, interrupts;
void gap_hit(const char *id, uint32_t detail) { (void)id; (void)detail; gaps++; }
void dsp_interrupt(void) { interrupts++; }
int dsp_interpreting(void) { return 1; }

static uint8_t rom[8192], coef[4096];

static int load(const char *folder, const char *name, uint8_t *bytes, size_t size) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", folder, name);
  FILE *file = fopen(path, "rb");
  if (!file) return 0;
  size_t got = fread(bytes, 1, size, file);
  fclose(file);
  return got == size;
}

static void put16(uint32_t address, uint16_t value) { mem1[address] = value >> 8; mem1[address + 1] = (uint8_t)value; }
static uint16_t get16(uint32_t address) { return (uint16_t)(mem1[address] << 8 | mem1[address + 1]); }

// Waits for a mail, takes it as the address of 16 bytes, copies them in and out to 0x3000,
// then says so with a mail and an interrupt, and waits again.
static const uint16_t program[] = {
  0x00de, 0xfffe,  // 0: LR $ac0.m, @CMBH
  0x02a0, 0x8000,  // ANDF $ac0.m, #0x8000: logic zero while there's no mail
  0x029d, 0x0000,  // JLZ 0
  0x00da, 0xfffe,  // LR $ax0.h, @CMBH
  0x00d8, 0xffff,  // LR $ax0.l, @CMBL
  0x00fa, 0xffce,  // SR @DSMAH, $ax0.h
  0x00f8, 0xffcf,  // SR @DSMAL, $ax0.l
  0x16cd, 0x0100,  // SI @DSPA, #0x100
  0x16c9, 0x0000,  // SI @DSCR, #0: into data memory
  0x16cb, 0x0010,  // SI @DSBL, #16
  0x16ce, 0x0000,  // SI @DSMAH, #0
  0x16cf, 0x3000,  // SI @DSMAL, #0x3000
  0x16c9, 0x0001,  // SI @DSCR, #1: out to main memory
  0x16cb, 0x0010,  // SI @DSBL, #16
  0x16fc, 0xdcd1,  // SI @DMBH, #0xdcd1
  0x16fd, 0x0002,  // SI @DMBL, #2
  0x16fb, 0x0001,  // SI @DIRQ, #1
  0x029f, 0x0000,  // JMP 0
};

// What dsp.c does for each, with the recorder told.
static void run(uint32_t cycles) {
  dsp_lle_run((int)cycles);
  if (dsp_recording) dsp_record_run(cycles);
  now_ticks += cycles / 2;
}
static void send(uint32_t mail) {
  if (dsp_recording) dsp_record_cpu_write(1, (uint16_t)(mail >> 16));
  if (dsp_recording) dsp_record_cpu_write(0, (uint16_t)mail);
  dsp_lle_mailbox_write_high(1, (uint16_t)(mail >> 16));
  dsp_lle_mailbox_write_low(1, (uint16_t)mail);
}
static uint32_t take(void) {
  if (dsp_recording) dsp_record_read_low(0, dsp_lle_mailbox_peek(0));
  uint32_t high = dsp_lle_mailbox_read_high(0);
  return high << 16 | dsp_lle_mailbox_read_low(0);
}

int main(int argc, char **argv) {
  memory_start();
  CHECK(argc == 3 && load(argv[1], "dsp_rom.bin", rom, sizeof rom) && load(argv[1], "dsp_coef.bin", coef, sizeof coef));
  CHECK(dsp_lle_start(rom, coef));
  for (uint32_t i = 0; i < sizeof program / 2; i++) put16(0x10000 + 2 * i, program[i]);
  dsp_lle_boot(0x10000, sizeof program, 0, 0);
  run(500);  // Waiting for mail before the recording begins.

  dsp_record_request(30, argv[2]);
  dsp_record_update(now_ticks);
  CHECK(dsp_recording);
  for (int mail = 0; mail < 3; mail++) {
    // The third mail names the first's block again, unchanged, so the recording needn't
    // hold its bytes twice.
    uint32_t from = mail == 1 ? 0x2040 : 0x2000;
    for (int i = 0; i < 16; i++) mem1[from + i] = (uint8_t)(mail == 1 ? 0xa0 + i : i);
    send(from);
    for (int i = 0; i < 6; i++) run(40);
    interrupts = 0;
    CHECK_EQ(take(), 0xdcd10002u);
    CHECK_EQ(get16(0x3000), mail == 1 ? 0xa0a1 : 0x0001);
    run(100);
  }
  // Time's up: the recording ends as the game next writes its mailbox.
  now_ticks += 31 * TICKS_PER_FRAME;
  dsp_record_update(now_ticks);
  CHECK(dsp_recording);
  send(0x2000);
  CHECK(!dsp_recording);

  FILE *file = fopen(argv[2], "rb");
  DspRecordHeader written;
  CHECK(file && fread(&written, sizeof written, 1, file) == 1);
  // The events, counted by kind, and the bytes read and written.
  uint32_t kinds[16] = {0}, bytes_read = 0, bytes_written = 0;
  if (file) {
    fseek(file, (long)(sizeof written + sizeof(DspLle)), SEEK_SET);
    for (int kind; (kind = fgetc(file)) != EOF;) {
      static const int sizes[] = {0, 8, 4, 2, 2, 5, 2, -1, -1, 4, 0};
      CHECK(kind > 0 && kind <= DSP_EVENT_INTERRUPT);
      if (kind <= 0 || kind > DSP_EVENT_INTERRUPT) break;
      kinds[kind]++;
      if (sizes[kind] >= 0) {
        fseek(file, sizes[kind], SEEK_CUR);
        continue;
      }
      uint32_t address_count[2];
      CHECK(fread(address_count, 4, 2, file) == 2);
      if (kind == DSP_EVENT_MEMORY_READ) bytes_read += address_count[1];
      else bytes_written += address_count[1];
      fseek(file, address_count[1], SEEK_CUR);
    }
    fclose(file);
  }
  CHECK_EQ(bytes_read, 31);  // 16 for each of the first two mails, less the first zero byte, none for the third.
  CHECK_EQ(bytes_written, 48);
  CHECK_EQ(kinds[DSP_EVENT_CPU_HIGH], 3);
  CHECK_EQ(kinds[DSP_EVENT_CPU_LOW], 3);
  CHECK_EQ(kinds[DSP_EVENT_MAIL], 3);
  CHECK_EQ(kinds[DSP_EVENT_INTERRUPT], 3);
  CHECK_EQ(kinds[DSP_EVENT_READ_LOW], 3);
  CHECK_EQ(kinds[DSP_EVENT_RUN], 21);
  CHECK_EQ(written.magic, DSP_RECORD_MAGIC);
  CHECK_EQ(written.version, DSP_RECORD_VERSION);
  CHECK_EQ(written.mails, 3);
  CHECK_EQ(written.state_bytes, sizeof(DspLle));
  CHECK(written.end_time > written.start_time);
  CHECK_EQ(gaps, 0);
  return finish();
}
