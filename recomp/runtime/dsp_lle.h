// The audio processor run instruction by instruction: an interpreter for its own code, with
// its registers, stacks, memories, mailboxes, DMA and accelerator. Ported faithfully from
// Dolphin's Core/DSP (DSPCore, DSPHWInterface, DSPMemoryMap, DSPStacks, DSPTables,
// DSPAnalyzer and Interpreter/*) and Core/HW/DSPLLE (GPL-2.0-or-later), so that C versions
// of audio programs can be checked against it.
//
// The processor has 16-bit words everywhere. Instruction memory is 0x0000-0x0fff (RAM, the
// program) and 0x8000-0x8fff (the boot ROM). Data memory is 0x0000-0x0fff (RAM), 0x1000-0x17ff
// (the coefficient ROM) and 0xff00-0xffff (hardware registers).
#pragma once
#include <stdint.h>

#include "dsp_accelerator.h"

// Loads the ROM and coefficient table (big-endian bytes as stored in the .bin files), then
// resets: halted, with the init bit set and pc at the ROM's start, as Dolphin's DSPLLE does.
// Returns 0 if their Adler-32 checksums aren't one of the pairs Dolphin's VerifyRoms knows
// (its seven, from Nintendo's own to the free v0.4 we ship).
int dsp_lle_start(const uint8_t *rom, const uint8_t *coef);
void dsp_lle_reset(void);     // pc = 0x8000 and the wrap registers all ones, as SDSP::Reset.
int dsp_lle_run(int cycles);  // About `cycles` instructions; returns the leftover, as DSPCore::RunCycles (0).

// The two mailboxes, as DSPLLE's DSP_Read/WriteMailBox*: cpu = 1 for the CPU's mailbox
// (CPU to DSP), 0 for the DSP's (DSP to CPU). Reading a low half empties that mailbox.
uint16_t dsp_lle_mailbox_read_high(int cpu);
uint16_t dsp_lle_mailbox_read_low(int cpu);
void dsp_lle_mailbox_write_high(int cpu, uint16_t value);
void dsp_lle_mailbox_write_low(int cpu, uint16_t value);
uint32_t dsp_lle_mailbox_peek(int cpu);

// The control register bits the processor owns: reset 0x1, external interrupt 0x2, halt 0x4,
// init code 0x400, init 0x800 (the others are kept as written). Clearing init loads 4 KB from
// 0x81000000 into instruction memory and starts it at 0; init code then reads set for 130
// time base ticks, as Dolphin measured on a Wii. Writing the external interrupt bit raises the
// processor's exception 7 at once, if the program has it enabled.
uint16_t dsp_lle_control_write(uint16_t value);
uint16_t dsp_lle_control_read(void);
int dsp_lle_halted(void);

// Puts a program in instruction memory straight from main memory and starts it at start_pc,
// unhalted: what the boot ROM does after its five mails, for when C code has taken them.
void dsp_lle_boot(uint32_t iram_address, uint32_t iram_bytes, uint16_t iram_dsp_address, uint16_t start_pc);
uint32_t dsp_lle_iram_crc(void);  // Dolphin's HashEctor over the program's bytes as loaded.

// Optional, 0 by default: called for each piece of main memory the program reads (after reading,
// with the bytes it got) or writes (with the bytes written), at physical addresses as host()
// takes them. DMA reports each transfer at once; the accelerator each byte as it goes.
typedef void (*DspMemoryObserver)(uint32_t address, const uint8_t *bytes, uint32_t count);
void dsp_lle_observe_memory(DspMemoryObserver on_read, DspMemoryObserver on_write);

// hardware.c: raises the CPU's DSP interrupt. The program asks for it by writing DIRQ.
void dsp_interrupt(void);

// --- The processor's state, for tests and for code that checks itself against it. ---

#define DSP_STACK_DEPTH 0x20
#define DSP_STACK_MASK 0x1f

// Register numbers, as instructions name them.
enum {
  DSP_REG_AR0 = 0x00, DSP_REG_AR1, DSP_REG_AR2, DSP_REG_AR3,
  DSP_REG_IX0 = 0x04, DSP_REG_IX1, DSP_REG_IX2, DSP_REG_IX3,
  DSP_REG_WR0 = 0x08, DSP_REG_WR1, DSP_REG_WR2, DSP_REG_WR3,
  DSP_REG_ST0 = 0x0c, DSP_REG_ST1, DSP_REG_ST2, DSP_REG_ST3,
  DSP_REG_ACH0 = 0x10, DSP_REG_ACH1 = 0x11,  // Sign-extended 8-bit tops of the accumulators.
  DSP_REG_CR = 0x12, DSP_REG_SR = 0x13,
  DSP_REG_PRODL = 0x14, DSP_REG_PRODM, DSP_REG_PRODH, DSP_REG_PRODM2,
  DSP_REG_AXL0 = 0x18, DSP_REG_AXL1, DSP_REG_AXH0, DSP_REG_AXH1,
  DSP_REG_ACL0 = 0x1c, DSP_REG_ACL1, DSP_REG_ACM0, DSP_REG_ACM1,
};

// Hardware registers (data memory 0xffxx), by their low byte.
enum {
  DSP_COEF_A1_0 = 0xa0,  // 16 ADPCM coefficients, 0xa0-0xaf.
  DSP_DSCR = 0xc9, DSP_DSBL = 0xcb, DSP_DSPA = 0xcd, DSP_DSMAH = 0xce, DSP_DSMAL = 0xcf,  // DMA
  DSP_FORMAT = 0xd1, DSP_ACUNK = 0xd2, DSP_ACDRAW = 0xd3,
  DSP_ACSAH = 0xd4, DSP_ACSAL, DSP_ACEAH, DSP_ACEAL, DSP_ACCAH, DSP_ACCAL,
  DSP_PRED_SCALE = 0xda, DSP_YN1, DSP_YN2, DSP_ACDSAMP, DSP_GAIN, DSP_ACIN,
  DSP_AMDM = 0xef,  // Masks DMA when set.
  DSP_DIRQ = 0xfb, DSP_DMBH, DSP_DMBL, DSP_CMBH, DSP_CMBL,
};

// The control register (DSPLLE's, not the processor's own CR register).
#define DSP_CR_RESET 0x0001u
#define DSP_CR_EXTERNAL_INT 0x0002u
#define DSP_CR_HALT 0x0004u
#define DSP_CR_INIT_CODE 0x0400u
#define DSP_CR_INIT 0x0800u

// The status register, SR.
#define DSP_SR_CARRY 0x0001u
#define DSP_SR_OVERFLOW 0x0002u
#define DSP_SR_ARITH_ZERO 0x0004u
#define DSP_SR_SIGN 0x0008u
#define DSP_SR_OVER_S32 0x0010u        // The result doesn't fit in 32 signed bits.
#define DSP_SR_TOP2BITS 0x0020u        // The top two bits of the middle word are equal.
#define DSP_SR_LOGIC_ZERO 0x0040u
#define DSP_SR_OVERFLOW_STICKY 0x0080u  // Set with OVERFLOW, but only cleared by writing SR.
#define DSP_SR_100 0x0100u              // Always reads 0.
#define DSP_SR_INT_ENABLE 0x0200u
#define DSP_SR_EXT_INT_ENABLE 0x0800u
#define DSP_SR_MUL_MODIFY 0x2000u      // Clear: products are doubled (M2). Set: as they are (M0).
#define DSP_SR_40_MODE_BIT 0x4000u     // SET40: loads to $acX.m extend over the accumulator.
#define DSP_SR_MUL_UNSIGNED 0x8000u    // SET15: MULX-family products of low halves are unsigned.
#define DSP_SR_CMP_MASK 0x003fu

// Exceptions, by number. Each jumps to number * 2.
enum {
  DSP_EXCEPTION_STACK_OVERFLOW = 1,
  DSP_EXCEPTION_ACCELERATOR_RAW_READ = 3,
  DSP_EXCEPTION_ACCELERATOR_RAW_WRITE = 4,
  DSP_EXCEPTION_ACCELERATOR_SAMPLE_READ = 5,
  DSP_EXCEPTION_EXTERNAL = 7,  // Mail from the CPU, through the control register.
};

// Little-endian hosts only (both our targets): the halves overlay the whole as in Dolphin.
typedef union { uint64_t val; struct { uint16_t l, m; uint32_t h; }; } DspAccumulator;  // h keeps val's sign.
typedef union { uint64_t val; struct { uint16_t l, m, h, m2; }; } DspProduct;
typedef union { uint32_t val; struct { uint16_t l, h; }; } DspAux;

typedef struct {
  uint16_t ar[4], ix[4], wr[4], st[4];
  uint16_t cr, sr;
  DspProduct prod;
  DspAux ax[2];
  DspAccumulator ac[2];
} DspRegisters;

typedef struct {
  DspRegisters r;
  uint16_t pc;
  uint16_t control_reg;
  uint64_t control_reg_init_code_clear_time;
  uint8_t reg_stack_ptrs[4];
  uint8_t exceptions;  // Pending, one bit per exception number.
  uint16_t reg_stacks[4][DSP_STACK_DEPTH];
  uint16_t iram[0x1000], irom[0x1000], dram[0x1000], coef[0x800];
  uint16_t ifx_regs[256];
  uint32_t mailbox[2];  // [0] the CPU's, [1] the DSP's. Bit 31 is full.
  DspAccelerator accelerator;
  uint32_t iram_crc;
  uint64_t step_counter;
  int started;
  uint8_t code_flags[0x10000];  // What the analyzer found at each instruction address.
  // Extended instructions write here, and it's applied after the main instruction.
  uint16_t write_back_log[5];
  int write_back_log_idx[5];
} DspLle;
extern DspLle dsp_lle;

// --- Shared by dsp_lle.c and dsp_lle_ops.c. ---
void dsp_lle_step(void);  // One instruction, with exceptions and loops, as Interpreter::Step.
uint16_t dsp_lle_read_imem(uint16_t address);
uint16_t dsp_lle_read_dmem(uint16_t address);
void dsp_lle_write_dmem(uint16_t address, uint16_t value);
uint16_t dsp_lle_fetch(void);
void dsp_lle_skip_instruction(void);
void dsp_lle_store_stack(int stack, uint16_t value);
uint16_t dsp_lle_pop_stack(int stack);
void dsp_lle_set_exception(int exception);
int dsp_lle_check_exceptions(void);
void dsp_lle_check_external_interrupt(void);
void dsp_lle_tables_start(void);
uint8_t dsp_lle_op_size(uint16_t opc);  // In words, from Dolphin's opcode templates.
void dsp_lle_execute(uint16_t opc);
void dsp_lle_handle_loop(void);
