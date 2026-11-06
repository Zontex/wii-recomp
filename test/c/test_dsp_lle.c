// The audio processor interpreter (dsp_lle.c, dsp_lle_ops.c) and its accelerator
// (dsp_accelerator.c), with made-up programs. Expected values come from reading Dolphin's
// interpreter. Dolphin's own accelerator tests are ported as they are, and its two
// hand-assembled test programs run to completion. The free boot ROM from Dolphin loads from
// the folder given as the first argument.
#include "../harness.h"
#include <string.h>

#include "dsp_lle.h"
#include "dsp_lle_programs.h"

static uint64_t now_ticks = 1000;
uint64_t time_base(void) { return now_ticks; }
static int gaps, interrupts;
static const char *last_gap;
static uint32_t last_gap_detail;
void gap_hit(const char *id, uint32_t detail) {
  gaps++;
  last_gap = id;
  last_gap_detail = detail;
}
void dsp_interrupt(void) { interrupts++; }

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

// Big-endian guest memory helpers. Physical addresses in MEM1.
static void put16(uint32_t address, uint16_t value) { mem1[address] = value >> 8; mem1[address + 1] = (uint8_t)value; }
static uint16_t get16(uint32_t address) { return (uint16_t)(mem1[address] << 8 | mem1[address + 1]); }

static uint32_t ector(const uint8_t *data, uint32_t length) {
  uint32_t crc = 0;
  for (uint32_t i = 0; i < length; i++) {
    crc ^= data[i];
    crc = crc << 3 | crc >> 29;
  }
  return crc;
}

#define PROGRAM_AT 0x00010000u

// A fresh processor with a program at instruction address 0, about to start at `pc`.
static void boot(const uint16_t *words, uint32_t count, uint16_t pc) {
  CHECK(dsp_lle_start(rom, coef));
  for (uint32_t i = 0; i < count; i++) put16(PROGRAM_AT + 2 * i, words[i]);
  dsp_lle_boot(PROGRAM_AT, count * 2, 0, pc);
}

static void run_until_halt(void) {
  for (int i = 0; i < 10000 && !dsp_lle_halted(); i++) dsp_lle_run(1000);
  CHECK(dsp_lle_halted());
}

static void run_program(const uint16_t *words, uint32_t count) {
  boot(words, count, 0);
  run_until_halt();
}
#define RUN(...) do { static const uint16_t program_[] = {__VA_ARGS__}; run_program(program_, sizeof program_ / 2); } while (0)
#define BOOT(...) do { static const uint16_t program_[] = {__VA_ARGS__}; boot(program_, sizeof program_ / 2, 0); } while (0)
#define HALT 0x0021

// ===================== The accelerator alone (Dolphin's DSPAcceleratorTest.cpp) =====================

static int accov_raised;
static uint8_t zero_memory(DspAccelerator *a, uint32_t address) { (void)a; (void)address; return 0; }
static void no_write(DspAccelerator *a, uint32_t address, uint8_t value) { (void)a; (void)address; (void)value; }
static void sample_end(DspAccelerator *a) {
  CHECK(a->reads_stopped);
  accov_raised = 1;
}
static DspAccelerator test_accelerator(void) {
  DspAccelerator a;
  memset(&a, 0, sizeof a);
  a.read_memory = zero_memory;
  a.write_memory = no_write;
  a.sample_read_end = sample_end;
  return a;
}
static uint16_t test_read(DspAccelerator *a) {
  static const int16_t coefs[16] = {0};
  accov_raised = 0;
  return dsp_accelerator_read_sample(a, coefs);
}
static void set_addresses(DspAccelerator *a, uint32_t current, uint32_t start, uint32_t end) {
  dsp_accelerator_set_current_address(a, current);
  dsp_accelerator_set_start_address(a, start);
  dsp_accelerator_set_end_address(a, end);
}

static void test_dolphin_accelerator_cases(void) {
  DspAccelerator a = test_accelerator();
  set_addresses(&a, 0, 0, 0x1000);  // Initialization
  CHECK_EQ(a.start_address, 0);
  CHECK_EQ(a.current_address, 0);
  CHECK_EQ(a.end_address, 0x1000);

  a = test_accelerator();  // SimpleReads
  set_addresses(&a, 0, 0, 0x1000);
  for (uint32_t i = 1; i <= 0xf; i++) {
    test_read(&a);
    CHECK(!accov_raised);
    CHECK_EQ(a.current_address, a.start_address + i);
  }

  a = test_accelerator();  // AddressMasking
  set_addresses(&a, 0x48000000, 0x48000000, 0x48001000);
  CHECK_EQ(a.start_address, 0x08000000);
  CHECK_EQ(a.current_address, 0x08000000);
  CHECK_EQ(a.end_address, 0x08001000);
  set_addresses(&a, 0xffffffff, 0xffffffff, 0xffffffff);
  CHECK_EQ(a.start_address, 0x3fffffff);
  CHECK_EQ(a.current_address, 0xbfffffff);
  CHECK_EQ(a.end_address, 0x3fffffff);

  a = test_accelerator();  // PredScaleRegisterMasking
  dsp_accelerator_set_pred_scale(&a, 0xbbbb);
  CHECK_EQ(a.pred_scale, 0x3b);
  dsp_accelerator_set_pred_scale(&a, 0xcccc);
  CHECK_EQ(a.pred_scale, 0x4c);
  dsp_accelerator_set_pred_scale(&a, 0xffff);
  CHECK_EQ(a.pred_scale, 0x7f);

  a = test_accelerator();  // OverflowBehaviour
  set_addresses(&a, 0, 0, 0xf);
  for (uint32_t i = 1; i <= 0xf; i++) {
    test_read(&a);
    CHECK(!accov_raised);
    CHECK_EQ(a.current_address, a.start_address + i);
  }
  test_read(&a);
  CHECK(accov_raised);
  CHECK_EQ(a.current_address, a.start_address);
  test_read(&a);  // Reads are stopped until YN2 is written.
  CHECK_EQ(a.current_address, a.start_address);
  dsp_accelerator_set_yn2(&a, 0);
  for (uint32_t i = 1; i <= 0xf; i++) {
    test_read(&a);
    CHECK(!accov_raised);
    CHECK_EQ(a.current_address, a.start_address + i);
  }

  a = test_accelerator();  // OverflowFor16ByteAlignedAddresses
  set_addresses(&a, 0, 0, 0x10);
  for (uint32_t i = 1; i <= 0xf; i++) {
    test_read(&a);
    CHECK(!accov_raised);
    CHECK_EQ(a.current_address, a.start_address + i);
  }
  test_read(&a);
  CHECK(!accov_raised);
  CHECK_EQ(a.current_address, a.start_address + 1);
  test_read(&a);
  CHECK_EQ(a.current_address, a.start_address + 2);

  a = test_accelerator();  // OverflowForXXXXXXX1Addresses
  set_addresses(&a, 0, 0, 0x11);
  for (uint32_t i = 1; i <= 0xf; i++) {
    test_read(&a);
    CHECK(!accov_raised);
    CHECK_EQ(a.current_address, a.start_address + i);
  }
  test_read(&a);
  CHECK(!accov_raised);
  CHECK_EQ(a.current_address, a.start_address);
  test_read(&a);
  CHECK_EQ(a.current_address, a.start_address + 1);

  a = test_accelerator();  // CurrentAddressSkips
  set_addresses(&a, 0, 0, 0x1000);
  for (int j = 1; j <= 0xf; j++) test_read(&a);
  CHECK_EQ(a.current_address, 0xf);
  test_read(&a);
  CHECK_EQ(a.current_address, 0x12);
  test_read(&a);
  CHECK_EQ(a.current_address, 0x13);
}

// ===================== The accelerator's decoding, with memory =====================

static uint8_t sample_bytes[64];
static uint8_t array_memory(DspAccelerator *a, uint32_t address) { (void)a; return sample_bytes[address & 63]; }
static uint32_t written_at[4], written_count;
static uint8_t written[4];
static void array_write(DspAccelerator *a, uint32_t address, uint8_t value) {
  (void)a;
  if (written_count < 4) { written_at[written_count] = address; written[written_count++] = value; }
}
static int raw_reads_ended, raw_writes_ended;
static void raw_read_end_hook(DspAccelerator *a) { (void)a; raw_reads_ended++; }
static void raw_write_end_hook(DspAccelerator *a) { (void)a; raw_writes_ended++; }

static void test_accelerator_decoding(void) {
  int16_t coefs[16] = {0};
  DspAccelerator a = test_accelerator();
  a.read_memory = array_memory;
  a.write_memory = array_write;
  a.raw_read_end = raw_read_end_hook;
  a.raw_write_end = raw_write_end_hook;

  // ADPCM: scale 1 << 2, coefficient pair 0 = (1.0, 0) in 11 bits.
  coefs[0] = 0x0800;
  sample_bytes[1] = 0x3f;
  set_addresses(&a, 2, 0, 0x1000);
  dsp_accelerator_set_pred_scale(&a, 0x02);
  CHECK_EQ(dsp_accelerator_read_sample(&a, coefs), 12);  // 4 * 3 + (0x400 >> 11)
  CHECK_EQ(dsp_accelerator_read_sample(&a, coefs), 8);   // 4 * -1 + ((0x400 + 2048 * 12) >> 11)
  CHECK_EQ(a.yn1, 8);
  CHECK_EQ(a.yn2, 12);
  // Saturation.
  dsp_accelerator_set_pred_scale(&a, 0x0b);
  sample_bytes[2] = 0x70;
  dsp_accelerator_set_yn1(&a, 32767);
  set_addresses(&a, 4, 0, 0x1000);
  CHECK_EQ(dsp_accelerator_read_sample(&a, coefs), 0x7fff);
  // A new frame's header byte becomes the predictor and scale, unmasked.
  sample_bytes[8] = 0xda;
  set_addresses(&a, 0xf, 0, 0x1000);
  dsp_accelerator_read_sample(&a, coefs);
  CHECK_EQ(a.pred_scale, 0xda);
  CHECK_EQ(a.current_address, 0x12);

  // 16-bit PCM with gain 1.0 out of 2048.
  memset(coefs, 0, sizeof coefs);
  dsp_accelerator_set_sample_format(&a, 0x0a);
  dsp_accelerator_set_gain(&a, 0x0800);
  dsp_accelerator_set_yn1(&a, 0);
  dsp_accelerator_set_yn2(&a, 0);
  sample_bytes[6] = 0x12;
  sample_bytes[7] = 0x34;
  set_addresses(&a, 3, 0, 0x1000);
  CHECK_EQ(dsp_accelerator_read_sample(&a, coefs), 0x1234);
  CHECK_EQ(a.current_address, 4);
  // MMIO PCM without increment reads ACIN and stays put; gain scale 1.
  dsp_accelerator_set_sample_format(&a, 2 | DSP_DECODE_MMIO_PCM_NO_INC << 2 | 1 << 4);
  dsp_accelerator_set_gain(&a, 2);
  dsp_accelerator_set_input(&a, 0x0100);
  CHECK_EQ(dsp_accelerator_read_sample(&a, coefs), 0x0200);
  CHECK_EQ(a.current_address, 4);

  // Raw reads, 16-bit: the end resets to the start and raises exception 3.
  dsp_accelerator_set_sample_format(&a, 2);
  set_addresses(&a, 3, 1, 3);
  CHECK_EQ(dsp_accelerator_read_raw(&a), 0x1234);
  CHECK_EQ(raw_reads_ended, 1);
  CHECK_EQ(a.current_address, 1);
  // 8-bit reads.
  dsp_accelerator_set_sample_format(&a, 1);
  set_addresses(&a, 7, 0, 0x100);
  CHECK_EQ(dsp_accelerator_read_raw(&a), 0x34);
  CHECK_EQ(a.current_address, 8);

  // Raw writes need the top address bit, and always write 16 bits.
  gaps = 0;
  set_addresses(&a, 5, 0, 0x100);
  dsp_accelerator_write_raw(&a, 0xbeef);
  CHECK_EQ(gaps, 1);
  CHECK_EQ(written_count, 0);
  dsp_accelerator_set_current_address(&a, 0x80000005u);
  dsp_accelerator_write_raw(&a, 0xbeef);
  CHECK_EQ(written_count, 2);
  CHECK_EQ(written_at[0], 0x0000000a);
  CHECK_EQ(written[0], 0xbe);
  CHECK_EQ(written_at[1], 0x0000000b);
  CHECK_EQ(written[1], 0xef);
  CHECK_EQ(raw_writes_ended, 1);
  CHECK_EQ(a.current_address, 0x80000006u);

  // Wii routing: bit 0x10000000 reads MEM2, otherwise MEM1; writes go to MEM2.
  mem1[0x40] = 0x11;
  mem2[0x40] = 0x22;
  CHECK_EQ(dsp_accelerator_wii_read(&a, 0x00000040), 0x11);
  CHECK_EQ(dsp_accelerator_wii_read(&a, 0x10000040), 0x22);
  CHECK_EQ(dsp_accelerator_wii_read(&a, 0x80000040), 0x11);  // Masked to MEM1's 32 MB.
  dsp_accelerator_wii_write(&a, 0x00000041, 0x33);
  CHECK_EQ(mem2[0x41], 0x33);
  CHECK_EQ(mem1[0x41], 0);
  gaps = 0;
  CHECK_EQ(dsp_accelerator_wii_read(&a, 0x01900000), 0);  // Past MEM1's 24 MB.
  CHECK_EQ(gaps, 1);
  mem1[0x40] = mem2[0x40] = mem2[0x41] = 0;
}

// ===================== Starting =====================

static void test_rom_checksums(void) {
  CHECK(dsp_lle_start(rom, coef));
  CHECK_EQ(dsp_lle.pc, 0x8000);
  CHECK(dsp_lle_halted());
  CHECK_EQ(dsp_lle_control_read(), DSP_CR_INIT | DSP_CR_HALT);
  CHECK_EQ(dsp_lle.r.sr, DSP_SR_INT_ENABLE | DSP_SR_EXT_INT_ENABLE);
  CHECK_EQ(dsp_lle.r.wr[2], 0xffff);
  CHECK_EQ(dsp_lle.iram[0x123], HALT);
  uint8_t changed[8192];
  memcpy(changed, rom, sizeof changed);
  changed[100] ^= 1;
  CHECK(!dsp_lle_start(changed, coef));
  CHECK(dsp_lle_start(rom, coef));
}

// ===================== Arithmetic and flags =====================

#define SR_BASE (DSP_SR_INT_ENABLE | DSP_SR_EXT_INT_ENABLE)
#define FLAGS (dsp_lle.r.sr & 0xff)

static void test_add_flags(void) {
  // 0x7f_ffff_ffff + 1 overflows 40 bits into the most negative value.
  BOOT(0x4c00, HALT);  // ADD $ac0, $ac1
  dsp_lle.r.ac[0].val = 0x7fffffffffull;
  dsp_lle.r.ac[1].val = 1;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ac[0].val, 0xffffff8000000000ull);
  CHECK_EQ(FLAGS, DSP_SR_OVERFLOW | DSP_SR_OVERFLOW_STICKY | DSP_SR_SIGN | DSP_SR_OVER_S32 | DSP_SR_TOP2BITS);

  // -1 + 1: zero with carry.
  BOOT(0x4c00, HALT);
  dsp_lle.r.ac[0].val = ~0ull;
  dsp_lle.r.ac[1].val = 1;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ac[0].val, 0);
  CHECK_EQ(FLAGS, DSP_SR_CARRY | DSP_SR_ARITH_ZERO | DSP_SR_TOP2BITS);

  // The sticky overflow bit outlives the next result.
  BOOT(0x4c00, 0x7600, HALT);  // ADD; INC $ac0
  dsp_lle.r.ac[0].val = 0x7fffffffffull;
  dsp_lle.r.ac[1].val = 1;
  run_until_halt();
  CHECK_EQ(FLAGS, DSP_SR_OVERFLOW_STICKY | DSP_SR_SIGN | DSP_SR_OVER_S32 | DSP_SR_TOP2BITS);
}

static void test_subtract_compare_and_negate(void) {
  BOOT(0x8200, HALT);  // CMP: 1 - 2
  dsp_lle.r.ac[0].val = 1;
  dsp_lle.r.ac[1].val = 2;
  run_until_halt();
  CHECK_EQ(FLAGS, DSP_SR_SIGN | DSP_SR_TOP2BITS);  // No carry: it borrowed.
  CHECK_EQ(dsp_lle.r.ac[0].val, 1);

  BOOT(0x5c00, HALT);  // SUB $ac0, $ac1: 5 - 3, no borrow so carry.
  dsp_lle.r.ac[0].val = 5;
  dsp_lle.r.ac[1].val = 3;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ac[0].val, 2);
  CHECK_EQ(FLAGS, DSP_SR_CARRY | DSP_SR_TOP2BITS);

  BOOT(0x7c00, HALT);  // NEG of the most negative value stays, with overflow.
  dsp_lle.r.ac[0].val = 0xffffff8000000000ull;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ac[0].val, 0xffffff8000000000ull);
  CHECK(dsp_lle.r.sr & DSP_SR_OVERFLOW);
  CHECK(!(dsp_lle.r.sr & DSP_SR_CARRY));

  BOOT(0x7c00, HALT);  // NEG of 0 carries.
  run_until_halt();
  CHECK_EQ(FLAGS, DSP_SR_CARRY | DSP_SR_ARITH_ZERO | DSP_SR_TOP2BITS);

  // CMPI against the middle word: 0x0001_0000 - (0x0002 << 16) is negative.
  BOOT(0x0280, 0x0002, HALT);  // CMPI $ac0, #2
  dsp_lle.r.ac[0].val = 0x10000;
  run_until_halt();
  CHECK_EQ(FLAGS, DSP_SR_SIGN | DSP_SR_TOP2BITS);
}

static void test_forty_bit_mode(void) {
  // SET40: loading $ac0.m sign-extends over the accumulator and clears $ac0.l.
  BOOT(0x8f00, 0x009e, 0x8000, HALT);  // SET40; LRI $ac0.m, #0x8000
  dsp_lle.r.ac[0].val = 0x0000001234567890ull;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ac[0].val, 0xffffffff80000000ull);

  BOOT(0x8e00, 0x009e, 0x8000, HALT);  // SET16: only the middle word.
  dsp_lle.r.ac[0].val = 0x0000001234567890ull;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ac[0].val, 0x0000001280007890ull);

  // $ac0.h loads are sign-extended from 8 bits.
  RUN(0x0090, 0x0080, HALT);  // LRI $ac0.h, #0x80
  CHECK_EQ(dsp_lle.r.ac[0].h, 0xffffff80u);
  CHECK_EQ((int64_t)dsp_lle.r.ac[0].val, -0x8000000000ll);

  // Reads of $ac0.m saturate in 40-bit mode when the whole is past 32 bits.
  BOOT(0x8f00, 0x1f1e, HALT);  // SET40; MRR $ax0.l, $ac0.m
  dsp_lle.r.ac[0].val = 0x0100000000ull;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ax[0].l, 0x7fff);
  BOOT(0x8f00, 0x1f1e, HALT);
  dsp_lle.r.ac[0].val = (uint64_t)-0x100000000ll;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ax[0].l, 0x8000);
  BOOT(0x8e00, 0x1f1e, HALT);  // SET16: as it is.
  dsp_lle.r.ac[0].val = 0x0112340000ull;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ax[0].l, 0x1234);

  // Arithmetic wraps at 40 bits: LSL16 pushes the top out, LSR16 brings zeros in.
  BOOT(0xf000, HALT);  // LSL16 $ac0
  dsp_lle.r.ac[0].val = 0x0012345678ull;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ac[0].val, 0x3456780000ull);  // 0x12 went past bit 39.
  BOOT(0xf400, HALT);  // LSR16 $ac0 of a negative value
  dsp_lle.r.ac[0].val = 0xffffff8000000000ull;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ac[0].val, 0x0000000000800000ull);
  BOOT(0x14fc, HALT);  // ASR $ac0, #-4 (encoded as 64 - 4): right by 4, sign in
  dsp_lle.r.ac[0].val = 0xffffff8000000000ull;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ac[0].val, 0xfffffff800000000ull);
}

static void test_logic(void) {
  BOOT(0x0240, 0x0ff0, HALT);  // ANDI $ac0.m, #0x0ff0
  dsp_lle.r.ac[0].val = 0x00f0f01234ull;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ac[0].val, 0x0000f01234ull);  // Only the middle word.
  CHECK_EQ(FLAGS, DSP_SR_TOP2BITS);

  BOOT(0x02c0, 0x8001, 0x02a0, 0x0010, HALT);  // ANDCF #0x8001 (true); ANDF #0x10 (true)
  dsp_lle.r.ac[0].m = 0x8003;
  run_until_halt();
  CHECK(dsp_lle.r.sr & DSP_SR_LOGIC_ZERO);
  BOOT(0x02c0, 0x8004, HALT);  // ANDCF with a missing bit clears LZ.
  dsp_lle.r.ac[0].m = 0x8003;
  dsp_lle.r.sr |= DSP_SR_LOGIC_ZERO;
  run_until_halt();
  CHECK(!(dsp_lle.r.sr & DSP_SR_LOGIC_ZERO));
}

// ===================== The multiplier =====================

static void test_multiply_modes(void) {
  // MUL $ax0.l, $ax0.h signed: -1 * 2, doubled by default (M2).
  BOOT(0x9000, HALT);
  dsp_lle.r.ax[0].l = 0xffff;
  dsp_lle.r.ax[0].h = 0x0002;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.prod.val, 0xfffffffffcull);
  // M0: as it is. MOVP moves it to $ac0 sign-extended.
  BOOT(0x8b00, 0x9000, 0x6e00, HALT);
  dsp_lle.r.ax[0].l = 0xffff;
  dsp_lle.r.ax[0].h = 0x0002;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.prod.val, 0xfffffffffeull);
  CHECK_EQ(dsp_lle.r.ac[0].val, (uint64_t)-2ll);

  // MULX of two low halves: unsigned with SET15, signed without.
  BOOT(0x8b00, 0x8d00, 0xa000, HALT);  // M0; SET15; MULX $ax0.l, $ax1.l
  dsp_lle.r.ax[0].l = 0xffff;
  dsp_lle.r.ax[1].l = 0xffff;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.prod.val, 0xfffe0001ull);
  BOOT(0x8b00, 0x8c00, 0xa000, HALT);  // CLR15
  dsp_lle.r.ax[0].l = 0xffff;
  dsp_lle.r.ax[1].l = 0xffff;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.prod.val, 1);
  // A low and a high half: mixed, unsigned low times signed high.
  BOOT(0x8b00, 0x8d00, 0xa800, HALT);  // MULX $ax0.l, $ax1.h
  dsp_lle.r.ax[0].l = 0xffff;
  dsp_lle.r.ax[1].h = 0xffff;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.prod.val, (uint64_t)-65535ll & 0xffffffffffull);
  // SET15 doesn't change MUL itself.
  BOOT(0x8b00, 0x8d00, 0x9000, HALT);
  dsp_lle.r.ax[0].l = 0xffff;
  dsp_lle.r.ax[0].h = 0xffff;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.prod.val, 1);

  // MADD adds to the product; CLRP's parts sum to zero; TSTPROD sees that.
  BOOT(0x8b00, 0x8400, 0xf200, 0xf200, HALT);  // M0; CLRP; MADD; MADD
  dsp_lle.r.ax[0].l = 3;
  dsp_lle.r.ax[0].h = 5;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.prod.val, 30);
  BOOT(0x8400, 0x8500, HALT);  // CLRP; TSTPROD
  run_until_halt();
  CHECK_EQ(dsp_lle.r.prod.l, 0x0000);
  CHECK_EQ(dsp_lle.r.prod.m, 0xfff0);
  CHECK_EQ(dsp_lle.r.prod.h, 0x00ff);
  CHECK_EQ(dsp_lle.r.prod.m2, 0x0010);
  CHECK(dsp_lle.r.sr & DSP_SR_ARITH_ZERO);

  // MULAC: $ac0 += old product, then a new product.
  BOOT(0x8b00, 0x9400, HALT);  // M0; MULAC $ax0.l, $ax0.h, $ac0
  dsp_lle.r.prod.val = 100;
  dsp_lle.r.ac[0].val = 7;
  dsp_lle.r.ax[0].l = 4;
  dsp_lle.r.ax[0].h = 6;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ac[0].val, 107);
  CHECK_EQ(dsp_lle.r.prod.val, 24);

  // MOVPZ rounds to the middle word: ties go to even.
  BOOT(0xfe00, HALT);
  dsp_lle.r.prod.val = 0x18000;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ac[0].val, 0x20000);
  BOOT(0xfe00, HALT);
  dsp_lle.r.prod.val = 0x28000;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ac[0].val, 0x20000);
}

// ===================== Loops =====================

static void test_loops(void) {
  RUN(0x8100, 0x8900,  // 0: CLR $ac0; 1: CLR $ac1
      0x1005, 0x7600,  // 2: LOOPI #5; 3: INC $ac0
      0x1103, 0x0008,  // 4: BLOOPI #3, 8
      0x7600, 0x7600, 0x7700,  // 6-8: INC $ac0; INC $ac0; INC $ac1
      0x1000, 0x7700,  // 9: LOOPI #0 skips 10: INC $ac1
      HALT);
  CHECK_EQ(dsp_lle.r.ac[0].val, 11);
  CHECK_EQ(dsp_lle.r.ac[1].val, 3);
  for (int i = 0; i < 4; i++) CHECK_EQ(dsp_lle.reg_stack_ptrs[i], 0);

  // LOOP and BLOOP by register, nested; a zero count jumps past the block.
  BOOT(0x0058,          // 0: LOOP $ax0.l (3)
       0x7600,          // 1: INC $ac0
       0x0079, 0x0007,  // 2: BLOOP $ax1.l (2), 7
       0x1102, 0x0006,  // 4: BLOOPI #2, 6
       0x7700,          // 6: INC $ac1, the inner loop's end
       0x7600,          // 7: INC $ac0, the outer loop's end
       0x007a, 0x000a,  // 8: BLOOP $ax0.h (0), 10: skips past 10
       0x7600,          // 10: INC $ac0
       HALT);
  dsp_lle.r.ax[0].l = 3;
  dsp_lle.r.ax[1].l = 2;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ac[0].val, 5);  // 3, then once per outer pass.
  CHECK_EQ(dsp_lle.r.ac[1].val, 4);  // 2 outer passes of 2 inner.
  for (int i = 0; i < 4; i++) CHECK_EQ(dsp_lle.reg_stack_ptrs[i], 0);
}

// ===================== Branches =====================

static void test_branches(void) {
  // CMP 1 vs 2 is less: JL is taken, JZ isn't.
  // 0: CMP; 1: JZ 9; 3: JL 7; 5: LRI $ax0.h (skipped); 7: LRI $ax1.h; 9: HALT
  BOOT(0x8200, 0x0295, 0x0009, 0x0291, 0x0007, 0x009a, 0x1111, 0x009b, 0x2222, HALT);
  dsp_lle.r.ac[0].val = 1;
  dsp_lle.r.ac[1].val = 2;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ax[0].h, 0);
  CHECK_EQ(dsp_lle.r.ax[1].h, 0x2222);

  // IFcc skips a whole two-word instruction when false.
  BOOT(0x8200, 0x0272, 0x009b, 0x1111, 0x009a, 0x2222, HALT);  // CMP; IFG; LRI; LRI
  dsp_lle.r.ac[0].val = 1;
  dsp_lle.r.ac[1].val = 2;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ax[1].h, 0);
  CHECK_EQ(dsp_lle.r.ax[0].h, 0x2222);

  // CALL and RET, and a conditional RET that isn't taken.
  static const uint16_t calls[] = {0x02bf, 0x0005, 0x009b, 0x3333, HALT, 0x02d5, 0x009a, 0x4444, 0x02df};
  // 0: CALL 5; 2: LRI $ax1.h; 4: HALT; 5: RETZ (not zero: stays); 6: LRI $ax0.h; 8: RET
  run_program(calls, sizeof calls / 2);
  CHECK_EQ(dsp_lle.r.ax[0].h, 0x4444);
  CHECK_EQ(dsp_lle.r.ax[1].h, 0x3333);
  CHECK_EQ(dsp_lle.pc, 4);
  CHECK_EQ(dsp_lle.reg_stack_ptrs[0], 0);

  // JMPR through a register.
  RUN(0x0080, 0x0005,  // 0: LRI $ar0, #5
      0x170f,          // 2: JMPR $ar0
      0x009a, 0x5555,  // 3: (skipped)
      0x009b, 0x6666,  // 5: LRI $ax1.h
      HALT);
  CHECK_EQ(dsp_lle.r.ax[0].h, 0);
  CHECK_EQ(dsp_lle.r.ax[1].h, 0x6666);

  // The conditions on the logic zero and over-32-bit flags.
  BOOT(0x029d, 0x0004, HALT, HALT, 0x0299, 0x0008, HALT, HALT, 0x009a, 0x7777, HALT);
  dsp_lle.r.sr |= DSP_SR_LOGIC_ZERO | DSP_SR_OVER_S32;
  run_until_halt();  // JLZ 4; JMPX9 (over 32 bits) 8
  CHECK_EQ(dsp_lle.r.ax[0].h, 0x7777);
}

// ===================== Extensions =====================

static void test_extensions(void) {
  // INC $ac0 : L $ax0.l, @$ar0
  BOOT(0x7640, HALT);
  dsp_lle.dram[0x10] = 0x1234;
  dsp_lle.r.ar[0] = 0x10;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ax[0].l, 0x1234);
  CHECK_EQ(dsp_lle.r.ar[0], 0x11);
  CHECK_EQ(dsp_lle.r.ac[0].val, 1);

  // CLR $ac0 : L $ac0.m, @$ar1 (16-bit mode): the extension's write lands after the CLR.
  BOOT(0x8e00, 0x8171, HALT);
  dsp_lle.dram[0x20] = 0x8001;
  dsp_lle.r.ar[1] = 0x20;
  dsp_lle.r.ac[0].val = 0x123456789aull;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ac[0].val, 0x0080010000ull);
  CHECK_EQ(dsp_lle.r.ar[1], 0x21);
  // In 40-bit mode it sign-extends.
  BOOT(0x8f00, 0x8171, HALT);
  dsp_lle.dram[0x20] = 0x8001;
  dsp_lle.r.ar[1] = 0x20;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ac[0].val, 0xffffffff80010000ull);

  // NX : LS $ax0.l, $ac0.m: store to $ar3, load from $ar0, both step.
  BOOT(0x8080, HALT);
  dsp_lle.r.ar[0] = 0x30;
  dsp_lle.r.ar[3] = 0x40;
  dsp_lle.dram[0x30] = 0xaaaa;
  dsp_lle.r.ac[0].m = 0x5555;
  run_until_halt();
  CHECK_EQ(dsp_lle.dram[0x40], 0x5555);
  CHECK_EQ(dsp_lle.r.ax[0].l, 0xaaaa);
  CHECK_EQ(dsp_lle.r.ar[0], 0x31);
  CHECK_EQ(dsp_lle.r.ar[3], 0x41);

  // NX : SLN $ac1.m, $ax1.h: store to $ar0, load from $ar3, $ar0 += $ix0.
  BOOT(0x80b7, HALT);
  dsp_lle.r.ar[0] = 0x30;
  dsp_lle.r.ix[0] = 4;
  dsp_lle.r.ar[3] = 0x40;
  dsp_lle.dram[0x40] = 0xbbbb;
  dsp_lle.r.ac[1].m = 0x6666;
  run_until_halt();
  CHECK_EQ(dsp_lle.dram[0x30], 0x6666);
  CHECK_EQ(dsp_lle.r.ax[1].h, 0xbbbb);
  CHECK_EQ(dsp_lle.r.ar[0], 0x34);
  CHECK_EQ(dsp_lle.r.ar[3], 0x41);

  // NX : LD $ax0.l, $ax1.l, @$ar0. In different 1 KB areas, two loads.
  BOOT(0x80c0, HALT);
  dsp_lle.r.ar[0] = 0x010;
  dsp_lle.r.ar[3] = 0x410;
  dsp_lle.dram[0x010] = 0x1111;
  dsp_lle.dram[0x410] = 0x2222;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ax[0].l, 0x1111);
  CHECK_EQ(dsp_lle.r.ax[1].l, 0x2222);
  CHECK_EQ(dsp_lle.r.ar[0], 0x011);
  CHECK_EQ(dsp_lle.r.ar[3], 0x411);
  // In the same area, both get the value at $ar0.
  BOOT(0x80c0, HALT);
  dsp_lle.r.ar[0] = 0x010;
  dsp_lle.r.ar[3] = 0x020;
  dsp_lle.dram[0x010] = 0x1111;
  dsp_lle.dram[0x020] = 0x2222;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ax[1].l, 0x1111);

  // NX : MV $ax0.l, $ac0.m saturates in 40-bit mode.
  BOOT(0x8f00, 0x8012, HALT);
  dsp_lle.r.ac[0].val = 0x0100000000ull;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ax[0].l, 0x7fff);

  // XORR $ac0.m, $ax0.h : IR $ar2 (seven-bit extension of 0x3xxx).
  BOOT(0x300a, HALT);
  dsp_lle.r.ac[0].m = 0x0ff0;
  dsp_lle.r.ax[0].h = 0x00ff;
  dsp_lle.r.ar[2] = 7;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ac[0].m, 0x0f0f);
  CHECK_EQ(dsp_lle.r.ar[2], 8);

  // Address registers wrap inside their window: $wr0 = 3 keeps $ar0 in 0x10-0x13.
  BOOT(0x0008, 0x0004, 0x0004, HALT);  // IAR $ar0; DAR $ar0; DAR $ar0
  dsp_lle.r.ar[0] = 0x13;
  dsp_lle.r.wr[0] = 3;
  dsp_lle_run(1);
  CHECK_EQ(dsp_lle.r.ar[0], 0x10);
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ar[0], 0x12);
}

// ===================== Loads, stores and registers =====================

static void test_loads_and_stores(void) {
  gaps = 0;
  RUN(0x0098, 0xabcd,  // LRI $ax0.l
      0x00f8, 0x0123,  // SR @0x0123, $ax0.l
      0x00d9, 0x0123,  // LR $ax1.l, @0x0123
      0x1612, 0x4321,  // SI @0x0012 (sign-extended: 0x0012), #0x4321
      0x0092, 0x0001,  // LRI $cr, #1
      0x2934,          // SRSH @0x34, $ac1.h -> 0x0134
      0x2312,          // LRS $ax1.h, @0x12 -> from 0x0112
      HALT);
  CHECK_EQ(dsp_lle.dram[0x123], 0xabcd);
  CHECK_EQ(dsp_lle.r.ax[1].l, 0xabcd);
  CHECK_EQ(dsp_lle.dram[0x12], 0x4321);
  CHECK_EQ(dsp_lle.r.cr, 1);
  CHECK_EQ(gaps, 0);

  // Reading a stack register pops it; writing pushes.
  RUN(0x0080 | DSP_REG_ST1, 0x1111, 0x0080 | DSP_REG_ST1, 0x2222,  // LRI $st1 twice
      0x1c00 | DSP_REG_AXL0 << 5 | DSP_REG_ST1,  // MRR $ax0.l, $st1
      0x1c00 | DSP_REG_AXL1 << 5 | DSP_REG_ST1,  // MRR $ax1.l, $st1
      HALT);
  CHECK_EQ(dsp_lle.r.ax[0].l, 0x2222);
  CHECK_EQ(dsp_lle.r.ax[1].l, 0x1111);
  CHECK_EQ(dsp_lle.reg_stack_ptrs[1], 0);

  // $sr drops bit 8, $cr keeps 8 bits, $prod.h too.
  RUN(0x0093, 0xffff, 0x0092, 0x1234, 0x0096, 0x1234, HALT);
  CHECK_EQ(dsp_lle.r.sr, 0xfeff);
  CHECK_EQ(dsp_lle.r.cr, 0x34);
  CHECK_EQ(dsp_lle.r.prod.h, 0x34);

  // ILRR reads instruction memory.
  BOOT(0x0210, HALT, 0xcafe);  // ILRR $ac0.m, @$ar0
  dsp_lle.r.ar[0] = 2;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ac[0].m, 0xcafe);

  // The coefficient ROM is readable at 0x1000; writes there go nowhere and are reported.
  gaps = 0;
  RUN(0x00d8, 0x1000, 0x00f8, 0x1000, HALT);
  CHECK_EQ(dsp_lle.r.ax[0].l, dsp_lle.coef[0]);
  CHECK_EQ(gaps, 1);
  CHECK(!strcmp(last_gap, "limit.dsp_bad_address"));
}

// ===================== Mailboxes, interrupts, DMA =====================

static void test_mailboxes(void) {
  BOOT(0x00da, 0xfffe,  // LR $ax0.h, @CMBH
       0x00d8, 0xffff,  // LR $ax0.l, @CMBL
       0x16fc, 0x2bcd,  // SI @DMBH
       0x16fd, 0xef01,  // SI @DMBL
       0x16fb, 0x0001,  // SI @DIRQ, #1
       HALT);
  interrupts = 0;
  dsp_lle_mailbox_write_high(1, 0x1234);
  CHECK_EQ(dsp_lle_mailbox_peek(1), 0x12340000);  // Not full until the low half.
  dsp_lle_mailbox_write_low(1, 0x5678);
  CHECK_EQ(dsp_lle_mailbox_peek(1), 0x92345678);
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ax[0].h, 0x9234);
  CHECK_EQ(dsp_lle.r.ax[0].l, 0x5678);
  CHECK_EQ(dsp_lle_mailbox_peek(1), 0x12345678);  // The program read it.
  CHECK_EQ(interrupts, 1);
  CHECK_EQ(dsp_lle_mailbox_read_high(0), 0xabcd);
  CHECK_EQ(dsp_lle_mailbox_read_low(0), 0xef01);
  CHECK_EQ(dsp_lle_mailbox_peek(0), 0x2bcdef01);
  gaps = 0;
  dsp_lle_mailbox_write_low(0, 1);  // The CPU can't write the processor's mailbox.
  CHECK_EQ(gaps, 1);
  CHECK(!strcmp(last_gap, "hw.mail_from_dsp"));
  CHECK_EQ(dsp_lle_mailbox_peek(0), 0x2bcdef01);
}

// Main memory the program reads and writes, seen through the observer.
static uint32_t observed_reads, observed_writes, observed_address[8], observed_count[8];
static uint8_t observed_first[8];
static void on_read(uint32_t address, const uint8_t *bytes, uint32_t count) {
  if (observed_reads < 8) {
    observed_address[observed_reads] = address;
    observed_count[observed_reads] = count;
    observed_first[observed_reads] = bytes[0];
  }
  observed_reads++;
}
static uint32_t write_address, write_count;
static uint8_t write_bytes[16];
static void on_write(uint32_t address, const uint8_t *bytes, uint32_t count) {
  observed_writes++;
  write_address = address;
  write_count = count;
  memcpy(write_bytes, bytes, count < 16 ? count : 16);
}

static void test_dma_and_observer(void) {
  for (int i = 0; i < 16; i++) mem1[0x2000 + i] = (uint8_t)(0xa0 + i);
  BOOT(0x16ce, 0x0000,  // SI @DSMAH, #0
       0x16cf, 0x2000,  // SI @DSMAL, #0x2000
       0x16cd, 0x0100,  // SI @DSPA, #0x100
       0x16c9, 0x0000,  // SI @DSCR, #0 (data memory from main memory)
       0x16cb, 0x0010,  // SI @DSBL, #16: go
       0x16cf, 0x3000,  // SI @DSMAL, #0x3000
       0x16c9, 0x0001,  // SI @DSCR, #1 (to main memory)
       0x16cb, 0x0010,  // SI @DSBL, #16
       0x00de, 0xffc9,  // LR $ac0.m, @DSCR: done already
       HALT);
  observed_reads = observed_writes = 0;
  dsp_lle_observe_memory(on_read, on_write);
  run_until_halt();
  CHECK_EQ(dsp_lle.dram[0x100], 0xa0a1);
  CHECK_EQ(dsp_lle.dram[0x107], 0xaeaf);
  CHECK_EQ(get16(0x3000), 0xa0a1);
  CHECK_EQ(get16(0x300e), 0xaeaf);
  CHECK_EQ(dsp_lle.r.ac[0].m, 1);  // The busy bit is never seen.
  CHECK_EQ(observed_reads, 1);
  CHECK_EQ(observed_address[0], 0x2000);
  CHECK_EQ(observed_count[0], 16);
  CHECK_EQ(observed_first[0], 0xa0);
  CHECK_EQ(observed_writes, 1);
  CHECK_EQ(write_address, 0x3000);
  CHECK_EQ(write_count, 16);
  CHECK_EQ(write_bytes[15], 0xaf);

  // The accelerator: each byte as it's read, at its physical address (MEM2 with the bit).
  mem2[0x202] = 0xbe;
  mem2[0x203] = 0xef;
  BOOT(0x16d1, 0x0002,  // SI @FORMAT, #2 (16-bit)
       0x16d8, 0x0800,  // SI @ACCAH: 16-bit words, so MEM2's bit is 0x08000000
       0x16d9, 0x0101,  // SI @ACCAL, #0x101
       0x16d6, 0x0800, 0x16d7, 0x0200,  // end well past
       0x00d8, 0xffd3,  // LR $ax0.l, @ACDRAW
       HALT);
  observed_reads = 0;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ax[0].l, 0xbeef);
  CHECK_EQ(observed_reads, 2);
  CHECK_EQ(observed_address[0], 0x10000202);
  CHECK_EQ(observed_first[0], 0xbe);
  CHECK_EQ(observed_count[0], 1);
  CHECK_EQ(observed_address[1], 0x10000203);
  CHECK_EQ(observed_first[1], 0xef);
  dsp_lle_observe_memory(0, 0);
  mem2[0x202] = mem2[0x203] = 0;

  // A masked transfer is skipped and reported.
  gaps = 0;
  RUN(0x16ef, 0x0001, 0x16cb, 0x0010, HALT);
  CHECK_EQ(gaps, 1);
  CHECK(!strcmp(last_gap, "limit.dsp_dma"));
}

// ===================== Exceptions =====================

static void test_exceptions(void) {
  static const uint16_t program[] = {
      HALT, HALT, HALT, HALT, HALT, HALT,  // 0-5
      0x029f, 0x0030,  // 6: exception 3 (raw read end)
      HALT, HALT,
      0x029f, 0x0038,  // a: exception 5 (sample read end)
      HALT, HALT,
      0x029f, 0x0020,  // e: exception 7 (external)
      0x029f, 0x0010,  // 10: wait here
      HALT, HALT, HALT, HALT, HALT, HALT, HALT, HALT, HALT, HALT, HALT, HALT, HALT, HALT,
      0x7700, 0x02ff,  // 20: INC $ac1; RTI
      HALT, HALT, HALT, HALT, HALT, HALT, HALT, HALT, HALT, HALT, HALT, HALT, HALT, HALT,
      0x009a, 0x3333, 0x02ff, HALT, HALT, HALT, HALT, HALT,  // 30: LRI $ax0.h; RTI
      0x009b, 0x5555, 0x02ff,  // 38: LRI $ax1.h; RTI
  };
  boot(program, sizeof program / 2, 0x10);
  dsp_lle_run(20);
  // The CPU's external interrupt: taken at once, saving pc and SR, masking itself.
  uint16_t control = dsp_lle_control_write(DSP_CR_INIT | DSP_CR_EXTERNAL_INT);
  CHECK_EQ(control, DSP_CR_INIT);  // The bit clears when it's taken.
  CHECK_EQ(dsp_lle.pc, 0x000e);
  CHECK_EQ(dsp_lle.r.st[1], SR_BASE);
  CHECK(dsp_lle.r.st[0] == 0x10 || dsp_lle.r.st[0] == 0x12);
  CHECK_EQ(dsp_lle.r.sr, DSP_SR_INT_ENABLE);
  dsp_lle_run(20);
  CHECK_EQ(dsp_lle.r.ac[1].val, 1);
  CHECK_EQ(dsp_lle.r.sr, SR_BASE);
  CHECK(dsp_lle.pc == 0x10 || dsp_lle.pc == 0x11 || dsp_lle.pc == 0x12);
  CHECK_EQ(dsp_lle.reg_stack_ptrs[0], 0);
  // Masked by the program, it isn't taken and the bit stays.
  dsp_lle.r.sr &= ~DSP_SR_EXT_INT_ENABLE;
  control = dsp_lle_control_write(DSP_CR_INIT | DSP_CR_EXTERNAL_INT);
  CHECK_EQ(control, DSP_CR_INIT | DSP_CR_EXTERNAL_INT);
  CHECK_EQ(dsp_lle.exceptions, 0);
  dsp_lle.r.sr |= DSP_SR_EXT_INT_ENABLE;

  // Two pending at once: the higher number goes first. With interrupts off they wait.
  dsp_lle.r.sr &= ~DSP_SR_INT_ENABLE;
  dsp_lle_set_exception(DSP_EXCEPTION_ACCELERATOR_RAW_READ);
  dsp_lle_set_exception(DSP_EXCEPTION_ACCELERATOR_SAMPLE_READ);
  CHECK(!dsp_lle_check_exceptions());
  CHECK_EQ(dsp_lle.exceptions, 0x28);
  dsp_lle.r.sr |= DSP_SR_INT_ENABLE;
  dsp_lle_run(5);
  CHECK_EQ(dsp_lle.r.ax[1].h, 0x5555);  // 5 first, then 3 once RTI turns interrupts back on.
  dsp_lle_run(10);
  CHECK_EQ(dsp_lle.r.ax[0].h, 0x3333);
  CHECK_EQ(dsp_lle.exceptions, 0);

  // The accelerator raises them from a program: a raw read at the end address, then a sample
  // read at the end, which also stops reads and goes back to the start.
  static const uint16_t accelerator[] = {
      0x16d1, 0x000a,  // FORMAT: 16-bit PCM, gain / 2048
      0x16de, 0x0800,  // GAIN 1.0
      0x16d4, 0x0000, 0x16d5, 0x0100,  // ACSA 0x100
      0x16d6, 0x0000, 0x16d7, 0x0101,  // ACEA 0x101
      0x16d8, 0x0000, 0x16d9, 0x0101,  // ACCA 0x101
      0x00d8, 0xffd3,  // LR $ax0.l, @ACDRAW: at the end, exception 3
      0x16d9, 0x0101,  // ACCA 0x101
      0x00d9, 0xffdd,  // LR $ax1.l, @ACDSAMP: exception 5
      0x00df, 0xffdd,  // LR $ac1.m, @ACDSAMP: stopped, 0
      0x00de, 0xffd9,  // LR $ac0.m, @ACCAL
      HALT};
  for (uint32_t i = 0; i < sizeof accelerator / 2; i++) put16(PROGRAM_AT + 0x80 + 2 * i, accelerator[i]);
  boot(program, sizeof program / 2, 0x10);
  dsp_lle_boot(PROGRAM_AT + 0x80, sizeof accelerator, 0x40, 0x40);
  put16(0x202, 0xbeef);
  dsp_lle.r.ac[1].m = 0x1234;
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ax[0].l, 0xbeef);
  CHECK_EQ(dsp_lle.r.ax[0].h, 0x3333);
  CHECK_EQ(dsp_lle.r.ax[1].l, 0xbeef);
  CHECK_EQ(dsp_lle.r.ax[1].h, 0x5555);
  CHECK_EQ(dsp_lle.r.ac[1].m, 0);
  CHECK_EQ(dsp_lle.r.ac[0].m, 0x0100);
  put16(0x202, 0);
}

// ===================== The control register =====================

static void test_control_register(void) {
  // Reset sends pc to the ROM.
  BOOT(0x029f, 0x0000);
  dsp_lle_run(10);
  dsp_lle_control_write(DSP_CR_INIT | DSP_CR_RESET);
  CHECK_EQ(dsp_lle.pc, 0x8000);
  CHECK_EQ(dsp_lle_control_read(), DSP_CR_INIT);

  // Clearing init loads 4 KB from 0x81000000 and starts it at 0; init code clears after 130 ticks.
  CHECK(dsp_lle_start(rom, coef));
  put16(0x01000000, 0x009a);
  put16(0x01000002, 0x7171);
  put16(0x01000004, HALT);
  uint16_t control = dsp_lle_control_write(0);
  CHECK_EQ(control, DSP_CR_INIT_CODE);
  CHECK_EQ(dsp_lle.pc, 0);
  CHECK_EQ(dsp_lle_iram_crc(), ector(mem1 + 0x01000000, 0x1000));
  now_ticks += 129;
  CHECK_EQ(dsp_lle_control_read(), DSP_CR_INIT_CODE);
  now_ticks += 1;
  CHECK_EQ(dsp_lle_control_read(), 0);
  run_until_halt();
  CHECK_EQ(dsp_lle.r.ax[0].h, 0x7171);
  for (int i = 0; i < 6; i++) mem1[0x01000000 + i] = 0;

  // The boot: checksum of the bytes as loaded.
  static const uint16_t words[] = {0x009a, 0x1234, HALT};
  boot(words, 3, 0);
  uint8_t bytes[6] = {0x00, 0x9a, 0x12, 0x34, 0x00, 0x21};
  CHECK_EQ(dsp_lle_iram_crc(), ector(bytes, 6));
  CHECK(!dsp_lle_halted());
}

static void test_unknown_opcode(void) {
  gaps = 0;
  RUN(0x0001, HALT);
  CHECK_EQ(gaps, 1);
  CHECK(!strcmp(last_gap, "limit.dsp_unknown_opcode"));
  CHECK_EQ(last_gap_detail, 0x00010001);
}

// ===================== The free boot ROM =====================

static uint32_t wait_mail(void) {
  for (int i = 0; i < 2000 && !(dsp_lle_mailbox_peek(0) & 0x80000000u); i++) dsp_lle_run(500);
  CHECK(dsp_lle_mailbox_peek(0) & 0x80000000u);
  uint32_t high = dsp_lle_mailbox_read_high(0);
  return high << 16 | dsp_lle_mailbox_read_low(0);
}

static void send_mail(uint32_t mail) {
  dsp_lle_mailbox_write_high(1, (uint16_t)(mail >> 16));
  dsp_lle_mailbox_write_low(1, (uint16_t)mail);
  for (int i = 0; i < 2000 && (dsp_lle_mailbox_peek(1) & 0x80000000u); i++) dsp_lle_run(500);
  CHECK(!(dsp_lle_mailbox_peek(1) & 0x80000000u));
}

static void test_boot_rom(void) {
  CHECK(dsp_lle_start(rom, coef));
  dsp_lle_reset();
  dsp_lle_control_write(DSP_CR_INIT);  // Halt cleared, init kept.
  CHECK_EQ(wait_mail(), 0x8071feed);

  // A tiny program at 0x00800000: one mail, then halt.
  static const uint16_t program[] = {0x16fc, 0x1357, 0x16fd, 0x9bdf, HALT, HALT, HALT, HALT};
  for (uint32_t i = 0; i < 8; i++) put16(0x00800000 + 2 * i, program[i]);
  send_mail(0x80f3a001);
  send_mail(0x00800000);  // Where it is
  send_mail(0x80f3a002);
  send_mail(sizeof program);  // How long
  send_mail(0x80f3c002);
  send_mail(0x0000);  // Where it goes in instruction memory
  send_mail(0x80f3b002);
  send_mail(0);
  send_mail(0x80f3d001);
  send_mail(0x0000);  // Where it starts
  CHECK_EQ(wait_mail(), 0x93579bdfu);  // The full bit reads with the high half.
  run_until_halt();
  CHECK_EQ(dsp_lle_iram_crc(), ector(mem1 + 0x00800000, sizeof program));
  CHECK_EQ(dsp_lle.iram[2], 0x16fd);
}

// ===================== Dolphin's test programs =====================

// DSPTestBinary: mails 0x8888dead for a block to load into data memory, 0x8888beef for a
// register table, runs TSTAXH on those registers and dumps them with all of data memory
// (0x8888feeb), then waits forever.
static void test_dolphin_dsp_test_program(void) {
  enum { BLOCK = 0x00200000, TABLE = 0x00300000 };
  gaps = 0;
  for (uint32_t i = 0; i < 0x1000; i++) put16(BLOCK + 2 * i, (uint16_t)(i * 7 + 3));
  for (uint32_t i = 0; i < 0x1000; i++) put16(TABLE + 2 * i, 0);
  uint16_t table[64];
  for (int i = 0; i < 64; i++) table[i] = (uint16_t)(0x1100 + i);
  table[8] = 0xffff;   // $wr0: $ar0 walks the table, so it must count normally.
  table[14] = 0;       // The loop stacks stay empty.
  table[15] = 0;
  table[16] = 0x0081;  // $ac0.h: sign-extended from 8 bits.
  table[18] = 0x00ff;  // $cr: its short loads and stores reach the hardware registers.
  table[19] = 0x0000;  // $sr: 16-bit mode, so no loads extend.
  table[26] = 0x8000;  // $ax0.h for TSTAXH
  for (int i = 0; i < 64; i++) put16(TABLE + 2 * i, table[i]);

  boot(dolphin_dsp_test, sizeof dolphin_dsp_test / 2, 0x10);
  interrupts = 0;
  CHECK_EQ(wait_mail(), 0x8888dead);
  send_mail(0x80000000u | BLOCK);
  CHECK_EQ(wait_mail(), 0x8888beef);
  send_mail(0x80000000u | TABLE);
  CHECK_EQ(wait_mail(), 0x8888feeb);
  send_mail(0);
  for (int i = 0; i < 20; i++) dsp_lle_run(1000);
  CHECK_EQ(interrupts, 3);
  CHECK_EQ(gaps, 0);

  // The dump of data memory went to the table's address.
  uint16_t return_address = 0;
  for (uint32_t i = 0; i + 2 < sizeof dolphin_dsp_test / 2; i++) {
    if (dolphin_dsp_test[i] == 0x8600 && dolphin_dsp_test[i + 1] == 0x02bf) { return_address = (uint16_t)(i + 3); break; }
  }
  CHECK(return_address);
  for (uint32_t i = 0; i < 0x1000; i++) {
    uint16_t want = (uint16_t)(i * 7 + 3);
    if (i == 0xf7e) want = (TABLE >> 16) & 0x7fff;
    else if (i == 0xf7f) want = TABLE & 0xffff;
    else if (i >= 0xf80 && i < 0xfc0) {
      uint32_t reg = i - 0xf80;
      want = table[reg];
      if (reg == 0x0c) want = return_address;  // The call to send_back is on $st0.
      if (reg == 0x10) want = 0xff81;
      if (reg == 0x11) want = 0x0011;  // Both sign-extend from 8 bits.
      if (reg == 0x12) want &= 0xff;
      if (reg == 0x13) want = DSP_SR_SIGN;  // TSTAXH of 0x8000
      if (reg == 0x16) want &= 0xff;
    }
    if (get16(TABLE + 2 * i) != want) {
      printf("FAILED dsp test dump word %03x = %04x, wanted %04x\n", i, get16(TABLE + 2 * i), want);
      failures++;
      break;
    }
  }
  // It ends in its dead loop.
  uint16_t pc = dsp_lle.pc;
  dsp_lle_run(100);
  CHECK_EQ(dsp_lle.pc, pc);
  CHECK_EQ(dsp_lle.iram[pc], 0x029f);  // JMP to itself
  CHECK_EQ(dsp_lle.iram[pc + 1], pc);
}

// Hermes' libasnd mixer: one mono 16-bit voice at 48000 Hz, mixed into 1024 stereo samples.
static void test_hermes_mixer(void) {
  enum { VOICE = 0x00400000, SAMPLES = 0x00410000, OUTPUT = 0x00420000 };
  gaps = 0;
  for (int i = 0; i < 0x1000; i++) put16(SAMPLES + 2 * i, 0x1000);
  for (int i = 0; i < 0x1000; i++) put16(OUTPUT + 2 * i, 0xdead);
  uint16_t voice[32] = {0};
  voice[0] = OUTPUT >> 16;  voice[1] = OUTPUT & 0xffff;  // Output buffer
  voice[4] = 2;  voice[5] = 1;  // Two bytes a step; mono 16-bit (the code's selector, not its comment)
  voice[6] = SAMPLES >> 16;  voice[7] = SAMPLES & 0xffff;
  voice[8] = (SAMPLES + 0x2000) >> 16;  voice[9] = (SAMPLES + 0x2000) & 0xffff;  // End
  voice[10] = 0;  voice[11] = 48000;  // Frequency
  voice[12] = 0x0800;  voice[13] = 0x0ff0;  // Last samples (SMP_L, SMP_R), already scaled
  voice[16] = 255;  voice[17] = 128;  // Volumes: SMP_R and the first of each pair use the left one
  for (int i = 0; i < 32; i++) put16(VOICE + 2 * i, voice[i]);

  boot(hermes_mixer, sizeof hermes_mixer / 2, 0x10);
  CHECK_EQ(wait_mail(), 0xdcd10000);
  send_mail(0x00000123);  // Where the voice is
  send_mail(VOICE);
  send_mail(0x00000111);  // Mix it
  CHECK_EQ(wait_mail(), 0xdcd10004);
  send_mail(0x00000666);  // Send the mix out
  CHECK_EQ(wait_mail(), 0xdcd10004);
  CHECK_EQ(gaps, 0);
  // 4096 * 128 / 256 on one side, 4096 * 255 / 256 on the other, every sample.
  int wrong = 0;
  for (int i = 0; i < 1024; i++) {
    if (get16(OUTPUT + 4 * i) != 0x0800 || get16(OUTPUT + 4 * i + 2) != 0x0ff0) wrong++;
  }
  CHECK_EQ(wrong, 0);
  if (wrong) printf("  first pair %04x %04x, last %04x %04x\n", get16(OUTPUT), get16(OUTPUT + 2), get16(OUTPUT + 4092), get16(OUTPUT + 4094));
  // The voice came back with its address moved on by the samples it used.
  uint32_t used = (uint32_t)get16(VOICE + 12) << 16 | get16(VOICE + 14);
  CHECK(used > SAMPLES && used < SAMPLES + 0x2000);
  send_mail(0x00000999);  // Done
  CHECK_EQ(wait_mail(), 0xdcd10003);
}

int main(int argc, char **argv) {
  memory_start();
  if (argc < 2 || !load(argv[1], "dsp_rom.bin", rom, sizeof rom) || !load(argv[1], "dsp_coef.bin", coef, sizeof coef)) {
    printf("can't read the ROMs\n");
    return 1;
  }
  test_dolphin_accelerator_cases();
  test_accelerator_decoding();
  test_rom_checksums();
  test_add_flags();
  test_subtract_compare_and_negate();
  test_forty_bit_mode();
  test_logic();
  test_multiply_modes();
  test_loops();
  test_branches();
  test_extensions();
  test_loads_and_stores();
  test_mailboxes();
  test_dma_and_observer();
  test_exceptions();
  test_control_register();
  test_unknown_opcode();
  test_boot_rom();
  test_dolphin_dsp_test_program();
  test_hermes_mixer();
  return finish();
}
