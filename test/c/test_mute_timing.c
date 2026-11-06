// Muting leaves timing alone: a made-up program runs on the interpreter behind the real
// mailbox (dsp.c), with the audio DMA (audio.c) playing what it writes, while a made-up
// game answers each interrupt with the next mail. It prints every interrupt and mail with
// its time; test_recomp_sound.py runs it with and without WII_RECOMP_MUTE and compares.
// Arguments: the folder with Dolphin's free ROM files.
#include "../../recomp/runtime/dsp.c"
#include "../harness.h"

static uint64_t now_ticks = 1000000;
uint64_t time_base(void) { return now_ticks; }
void gap_hit(const char *id, uint32_t detail) { printf("gap %s %x\n", id, detail); }
void gap_note(const char *id, uint32_t detail) { gap_hit(id, detail); }
void stage_add(int stage, uint64_t ticks) { (void)stage; (void)ticks; }
uint32_t hw_read(uint32_t address, int bytes) { (void)address; (void)bytes; return 0; }
void hw_write(uint32_t address, uint32_t value, int bytes) { (void)address; (void)value; (void)bytes; }
uint32_t hw_peek(uint32_t address, int bytes) { (void)bytes; return address == 0xcc006c00u ? 0x40 : 0; }  // 32 kHz.
const unsigned char data_dsp_rom[1], data_dsp_coef[1];
// The sound recorder isn't used.
int dsp_recording;
void dsp_record_start(void) {}
void dsp_record_update(uint64_t now) { (void)now; }
void dsp_record_run(uint32_t cycles) { (void)cycles; }
void dsp_record_cpu_write(int high, uint16_t value) { (void)high; (void)value; }
void dsp_record_read_low(int cpu, uint32_t mail) { (void)cpu; (void)mail; }
void dsp_record_control(uint16_t value) { (void)value; }

static int dsp_interrupts, dma_interrupts, outputs_opened;
void dsp_interrupt(void) { dsp_interrupts++; }
void audio_dma_interrupt(void) { dma_interrupts++; }
int audio_dma_interrupt_unanswered(void) { return 0; }
int audio_output_open(void) { outputs_opened++; return 1; }
void audio_start(void);
void audio_update(uint64_t now);
int audio_dma_write(uint32_t address, uint32_t value, int bytes);

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

// Waits for a mail naming 16 bytes, copies them to 0x3000, where the audio DMA plays,
// mails back with an interrupt, and waits again (as test_sound_record.c's).
static const uint16_t program[] = {
  0x00de, 0xfffe, 0x02a0, 0x8000, 0x029d, 0x0000, 0x00da, 0xfffe, 0x00d8, 0xffff, 0x00fa, 0xffce, 0x00f8, 0xffcf,
  0x16cd, 0x0100, 0x16c9, 0x0000, 0x16cb, 0x0010, 0x16ce, 0x0000, 0x16cf, 0x3000, 0x16c9, 0x0001, 0x16cb, 0x0010,
  0x16fc, 0xdcd1, 0x16fd, 0x0002, 0x16fb, 0x0001, 0x029f, 0x0000,
};

int main(int argc, char **argv) {
  memory_start();
  CHECK(argc == 2 && load(argv[1], "dsp_rom.bin", rom, sizeof rom) && load(argv[1], "dsp_coef.bin", coef, sizeof coef));
  CHECK(dsp_lle_start(rom, coef));
  for (uint32_t i = 0; i < sizeof program / 2; i++) wr16(0x10000 + 2 * i, program[i]);
  audio_start();
  // As hand_over does for a program with no C version.
  interpreting = 1;
  interpreted_until = time_base();
  dsp_lle_boot(0x10000, sizeof program, 0, 0);
  audio_dma_write(0xcc005030u, 0, 2);
  audio_dma_write(0xcc005032u, 0x3000, 2);
  audio_dma_write(0xcc005036u, 0x8000 | 1, 2);  // One block, over and over.
  uint32_t mail = 0x2000;
  for (int i = 0; i < 16; i++) wr16(0x2000 + i * 2, (uint16_t)(i * 300));
  dsp_write(0xcc005000u, mail, 4);
  for (int ms = 0; ms < 200; ms++) {
    for (int part = 0; part < 4; part++) {
      now_ticks += 60750 / 4;
      dsp_update(now_ticks);
      audio_update(now_ticks);
      if (dsp_interrupts) {
        dsp_interrupts = 0;
        uint32_t got;
        dsp_read(0xcc005004u, 4, &got);
        printf("%llu dsp interrupt, mail %08x\n", (unsigned long long)now_ticks, got);
        mail ^= 0x10;
        dsp_write(0xcc005000u, mail, 4);
      }
      if (dma_interrupts) {
        printf("%llu dma interrupt x%d\n", (unsigned long long)now_ticks, dma_interrupts);
        dma_interrupts = 0;
      }
    }
  }
  printf("output %s\n", outputs_opened ? "opened" : "not opened");
  return finish();
}
