// The expansion bus's clock chip, through the real exi.c, driven as the SDK drives it,
// and in GameCube mode the memory card in slot A, in a file of its own.
#include <stdio.h>
#include <unistd.h>
#include "../harness.h"

uint32_t hw_read(uint32_t address, int bytes) { (void)address; (void)bytes; return 0; }
void hw_write(uint32_t address, uint32_t value, int bytes) { (void)address; (void)value; (void)bytes; }
static int irq_up, gaps;
void raise_irq(int bit) { (void)bit; irq_up = 1; }
void clear_irq(int bit) { (void)bit; irq_up = 0; }
void gap_hit(const char *id, uint32_t detail) { (void)id; (void)detail; gaps++; }
static uint64_t now_ticks = 60750000ull * 1000000;  // A million seconds after 2000.
uint64_t time_base(void) { return now_ticks; }
static int32_t counter_bias;  // The settings' IPL.CB, or none when 0.
int files_system_setting(const char *name, uint8_t *out, size_t limit) {
  if (!counter_bias || strcmp(name, "IPL.CB") || limit < 4) return 0;
  for (int i = 0; i < 4; i++) out[i] = (uint8_t)((uint32_t)counter_bias >> (24 - i * 8));
  return 4;
}

// Made-up stand-ins for the boot ROM fonts build_file.py embeds.
const unsigned char data_font_western[64] = "Yay0, a western font made up for the test";
const unsigned char data_font_japanese[64] = "Yay0, a Japanese font made up for the test";
const unsigned data_font_western_bytes = 64, data_font_japanese_bytes = 64;
int gamecube_mode;
#include "exi.c"

enum { EXI = 0xcd006800u, CLOCK_CHIP = 2u << 7, BUFFER = 0x80001000u };

static uint32_t control(int dma, int direction, int bytes) { return 1u | (uint32_t)dma << 1 | (uint32_t)direction << 2 | (uint32_t)(bytes - 1) << 4; }

static void select_chip(void) {
  exi_write(EXI + 0x00, 0);
  exi_write(EXI + 0x00, CLOCK_CHIP);
}

static void command(uint32_t word) {
  select_chip();
  exi_write(EXI + 0x10, word);
  exi_write(EXI + 0x0c, control(0, WRITE, 4));
}

static uint32_t read_clock(void) {
  uint32_t value;
  command(0x20000000);
  exi_write(EXI + 0x0c, control(0, READ, 4));
  exi_read(EXI + 0x10, &value);
  return value;
}

static void test_sram(void) {
  command(0x20000100);  // The settings, after the clock.
  exi_write(EXI + 0x04, BUFFER & 0x01ffffff);
  exi_write(EXI + 0x08, 0x40);
  exi_write(EXI + 0x0c, control(1, READ, 1));
  CHECK_EQ(rd16(BUFFER), 0x002c);       // Checksum.
  CHECK_EQ(rd16(BUFFER + 2), 0xffd0);   // Its complement.
  CHECK_EQ(rd8(BUFFER + 19), 0x2c);     // Flags: stereo, set up.
  CHECK_EQ(rd8(BUFFER + 58), 0xff);     // Memory card ID checksums.
  uint32_t value;
  exi_read(EXI + 0x0c, &value);
  CHECK_EQ(value & 1, 0);  // Finished at once.
}

static void test_clock(void) {
  CHECK_EQ(read_clock(), 1000000);
  now_ticks += 60750000ull * 5;
  CHECK_EQ(read_clock(), 1000005);
  command(0xa0000000);  // Write the clock.
  exi_write(EXI + 0x10, 1000105);
  exi_write(EXI + 0x0c, control(0, WRITE, 4));
  CHECK_EQ(read_clock(), 1000105);
  now_ticks += 60750000ull;
  CHECK_EQ(read_clock(), 1000106);  // It keeps running from where it was set.
  // The menu adds the settings' counter bias to the count, so the count leaves it out,
  // and the menu shows the same time whatever bias its date setting stored.
  counter_bias = -500000;
  CHECK_EQ(read_clock(), 1500106);
  counter_bias = 0;
}

static void test_interrupt(void) {
  exi_write(EXI + 0x00, CLOCK_CHIP | 1u << 2 | 1u << 3);  // Unmask transfer done, and clear it.
  CHECK(!irq_up);
  exi_write(EXI + 0x10, 0x20000000);
  exi_write(EXI + 0x0c, control(0, WRITE, 4));
  CHECK(irq_up);
  uint32_t status;
  exi_read(EXI + 0x00, &status);
  exi_write(EXI + 0x00, status);  // Writing the done bit back acknowledges it.
  CHECK(!irq_up);
}

static void test_no_device(void) {
  exi_write(EXI + 0x14, 1u << 7);  // Channel 1's memory card slot, which is empty.
  exi_write(EXI + 0x14 + 0x0c, control(0, READ, 4));
  uint32_t value;
  exi_read(EXI + 0x14 + 0x10, &value);
  CHECK_EQ(value, 0);
  CHECK_EQ(gaps, 1);
  exi_write(EXI + 0x00, 4u << 7);  // Channel 0's debugging device, which no retail Wii has.
  exi_write(EXI + 0x0c, control(0, READ, 4));
  CHECK_EQ(gaps, 1);
}

// The boot ROM, as OSInitFont reads a font from it: a command with the address, then its
// bytes from there on by DMA, here into the second memory, where a game's heap may hold the
// buffer. The DMA address was once cut to the GameCube's 26 bits, which put it in MEM1.
static void read_rom(uint32_t at, uint32_t buffer, uint32_t bytes) {
  command(at << 6);
  exi_write(EXI + 0x04, buffer & 0x1fffffff);
  exi_write(EXI + 0x08, bytes);
  exi_write(EXI + 0x0c, control(1, READ, 1));
}

static void test_boot_rom_fonts(void) {
  enum { MEM2_BUFFER = 0x90001000u, WESTERN_AT = 0x1fcf00, JAPANESE_AT = 0x1aff00 };
  wr32(BUFFER, 0x5a5a5a5a);  // Where the cut address would land.
  read_rom(WESTERN_AT, MEM2_BUFFER, 64);
  for (uint32_t i = 0; i < 64; i++) CHECK_EQ(rd8(MEM2_BUFFER + i), data_font_western[i]);
  CHECK_EQ(rd32(BUFFER), 0x5a5a5a5a);
  read_rom(JAPANESE_AT, MEM2_BUFFER, 64);
  for (uint32_t i = 0; i < 64; i++) CHECK_EQ(rd8(MEM2_BUFFER + i), data_font_japanese[i]);
  // Past a font, and between the two, the ROM reads as zeros; below them is the boot code,
  // which isn't here, and says so.
  int before = gaps;
  read_rom(WESTERN_AT + 64, MEM2_BUFFER, 32);
  CHECK_EQ(rd32(MEM2_BUFFER), 0);
  CHECK_EQ(gaps, before);
  read_rom(0x100, MEM2_BUFFER, 32);
  CHECK(gaps > before);
}

enum { CARD = 1u << 7, CARD_BUFFER = 0x80002000u, CARD_AT = 0x6000 };

static uint8_t card_exchange(uint8_t out) {
  uint32_t value;
  exi_write(EXI + 0x10, (uint32_t)out << 24);
  exi_write(EXI + 0x0c, control(0, READ_WRITE, 1));
  exi_read(EXI + 0x10, &value);
  return (uint8_t)(value >> 24);
}

// Selects the card, sends a command with its address, and leaves the card selected.
static void card_command(uint8_t command, uint32_t address, int address_bytes) {
  exi_write(EXI + 0x00, 0);
  exi_write(EXI + 0x00, CARD);
  uint8_t bytes[5] = {command, (uint8_t)(address >> 17), (uint8_t)(address >> 9), (uint8_t)(address >> 7 & 3), (uint8_t)(address & 0x7f)};
  for (int i = 0; i < 1 + address_bytes; i++) card_exchange(bytes[i]);
}

static void dma_with_card(int direction, uint32_t memory, uint32_t bytes) {
  exi_write(EXI + 0x04, memory & 0x01ffffff);
  exi_write(EXI + 0x08, bytes);
  exi_write(EXI + 0x0c, control(1, direction, 1));
}

static uint8_t file_byte(const char *path, long at) {
  FILE *file = fopen(path, "rb");
  int value = -1;
  if (file && !fseek(file, at, SEEK_SET)) value = fgetc(file);
  if (file) fclose(file);
  return (uint8_t)value;
}

static void test_memory_card(void) {
  char path[] = "/tmp/wii-recomp-card-XXXXXX";
  int handle = mkstemp(path);
  CHECK(handle >= 0);
  close(handle);
  unlink(path);  // Not there yet: the console makes a blank one.
  setenv("WII_RECOMP_MEMORY_CARD_A", path, 1);
  gamecube_mode = 1;
  int before = gaps;
  exi_start();
  FILE *file = fopen(path, "rb");
  CHECK(file != 0);
  if (file) {
    fseek(file, 0, SEEK_END);
    CHECK_EQ(ftell(file), 0x200000);  // A Memory Card 251: 16 Mbit.
    fclose(file);
  }
  uint32_t status;
  exi_read(EXI, &status);
  CHECK(status & 1u << 12);  // A device is in slot A.
  CHECK(status & 1u << 11);  // And the insertion interrupt is up for the SDK to look.

  card_command(0x00, 0, 0);  // The card's ID: its size in megabits, after a dummy byte.
  CHECK_EQ(card_exchange(0), 0x80);
  uint8_t id[4];
  for (int i = 0; i < 4; i++) id[i] = card_exchange(0);
  CHECK_EQ(id[3], 16);
  CHECK_EQ(id[0] | id[1] | id[2], 0);
  card_command(0x83, 0, 0);
  CHECK_EQ(card_exchange(0) & 0x41, 0x41);  // Ready, and unlocked.

  card_command(0x81, 0, 0);  // Interrupts on, as the SDK asks.
  card_exchange(1);
  for (int i = 0; i < 0x80; i++) wr8(CARD_BUFFER + (uint32_t)i, (uint8_t)(i ^ 0x5a));
  card_command(0xf2, CARD_AT, 4);  // Program a page.
  dma_with_card(WRITE, CARD_BUFFER, 0x80);
  exi_write(EXI + 0x00, 0);  // Letting the card go finishes the command.
  CHECK_EQ(file_byte(path, CARD_AT + 3), 3 ^ 0x5a);  // On disk at once.
  exi_read(EXI, &status);
  CHECK(status & 1u << 1);  // The card's interrupt.
  card_command(0x89, 0, 0);  // Clear status: the interrupt goes.
  exi_write(EXI + 0x00, 1u << 1);
  exi_read(EXI, &status);
  CHECK_EQ(status & 1u << 1, 0);

  for (int i = 0; i < 0x80; i++) wr8(CARD_BUFFER + 0x100 + (uint32_t)i, 0);
  card_command(0x52, CARD_AT, 4);  // Read it back.
  dma_with_card(READ, CARD_BUFFER + 0x100, 0x80);
  exi_write(EXI + 0x00, 0);
  CHECK(!memcmp(host(CARD_BUFFER), host(CARD_BUFFER + 0x100), 0x80));

  card_command(0xf1, CARD_AT, 2);  // Erase its sector.
  exi_write(EXI + 0x00, 0);
  CHECK_EQ(file_byte(path, CARD_AT + 3), 0xff);
  CHECK_EQ(gaps, before);
  gamecube_mode = 0;
  unlink(path);
}

int main(void) {
  memory_start();
  test_sram();
  test_clock();
  test_interrupt();
  test_no_device();
  test_boot_rom_fonts();
  test_memory_card();
  return finish();
}
