// The audio processor's core: memories, hardware registers, DMA, mailboxes, stacks,
// exceptions, the control register and the run loop. Ported from Dolphin's Core/DSP
// (DSPCore.cpp, DSPHWInterface.cpp, DSPMemoryMap.cpp, DSPStacks.cpp, DSPAnalyzer.cpp,
// Interpreter/DSPInterpreter.cpp) and Core/HW/DSPLLE (DSPLLE.cpp, DSPHost.cpp),
// GPL-2.0-or-later. The instructions themselves are in dsp_lle_ops.c.
#include "dsp_lle.h"

#include <string.h>

#include "cpu.h"
#include "gaps.h"

DspLle dsp_lle;
#define R dsp_lle.r

#define RESET_VECTOR 0x8000u
#define MAILBOX_CPU 0
#define MAILBOX_DSP 1
#define MAIL_FULL 0x80000000u
#define INIT_CODE_TICKS 130u  // Dolphin's measurement on a Wii, not perfectly consistent.
#define INIT_CODE_ADDRESS 0x81000000u
#define INIT_CODE_BYTES 0x1000u
#define DMA_LONGEST 0x4000u
// DMA control (DSCR) bits 0 and 1.
#define DMA_TO_CPU 1u
#define DMA_IMEM 2u
#define DMA_BUSY 4u

// What the analyzer marks (DSPAnalyzer.h). The interpreter only asks about loop ends and idle
// loops; Dolphin's other marks serve its recompiler.
#define CODE_IDLE_SKIP 2u
#define CODE_LOOP_END 8u

// --- The analyzer: finds where loops end, and loops that only wait for mail. ---

// Instruction memory as the analyzer sees it, without reporting holes.
static uint16_t peek_imem(uint16_t address) {
  switch (address >> 12) {
  case 0: return dsp_lle.iram[address & 0x0fff];
  case 8: return dsp_lle.irom[address & 0x0fff];
  default: return 0;
  }
}

// Mail wait loops, from AX and Zelda. 0xffff matches anything; a 0 ends the signature.
#define IDLE_SIGNATURES 8
#define IDLE_SIGNATURE_WORDS 7
static const uint16_t idle_skip_sigs[IDLE_SIGNATURES][IDLE_SIGNATURE_WORDS] = {
    {0x26fc, 0x02c0, 0x8000, 0x029d, 0xffff, 0, 0},  // LRS $30, @DMBH; ANDCF $30, #0x8000; JLZ
    {0x27fc, 0x03c0, 0x8000, 0x029d, 0xffff, 0, 0},  // LRS $31, @DMBH; ANDCF $31, #0x8000; JLZ
    {0x26fe, 0x02c0, 0x8000, 0x029c, 0xffff, 0, 0},  // LRS $30, @CMBH; ANDCF $30, #0x8000; JLNZ
    {0x27fe, 0x03c0, 0x8000, 0x029c, 0xffff, 0, 0},  // LRS $31, @CMBH; ANDCF $31, #0x8000; JLNZ
    {0x26fc, 0x02a0, 0x8000, 0x029c, 0xffff, 0, 0},  // LRS $AC0.M, @DMBH; ANDF $AC0.M, #0x8000; JLNZ
    {0x27fc, 0x03a0, 0x8000, 0x029c, 0xffff, 0, 0},  // LRS $AC1.M, @DMBH; ANDF $AC1.M, #0x8000; JLNZ
    {0x00de, 0xfffe, 0x02c0, 0x8000, 0x029c, 0xffff, 0},  // Zelda: LR $AC0.M, @CMBH; ANDCF; JLNZ
    {0x00da, 0x0352, 0x8600, 0x0295, 0xffff, 0, 0},  // Zelda, experimental: LR $AX0.H, @0x0352; TSTAXH; JZ
};

static void analyze_range(uint16_t start, uint16_t end) {
  // A simple disassembly pass finds where instructions start. Jump tables could throw it off.
  for (uint16_t address = start; address < end;) {
    uint16_t inst = peek_imem(address);
    if ((inst & 0xffe0) == 0x0060 || (inst & 0xff00) == 0x1100) {  // BLOOP, BLOOPI
      dsp_lle.code_flags[peek_imem((uint16_t)(address + 1))] |= CODE_LOOP_END;
    } else if ((inst & 0xffe0) == 0x0040 || (inst & 0xff00) == 0x1000) {  // LOOP, LOOPI
      dsp_lle.code_flags[(uint16_t)(address + 1)] |= CODE_LOOP_END;
    }
    address += dsp_lle_op_size(inst);
  }
  for (int s = 0; s < IDLE_SIGNATURES; s++) {
    for (uint16_t address = start; address < end; address++) {
      int found = 0;
      for (int i = 0; i < IDLE_SIGNATURE_WORDS; i++) {
        if (idle_skip_sigs[s][i] == 0) found = 1;
        if (idle_skip_sigs[s][i] == 0xffff) continue;
        if (idle_skip_sigs[s][i] != peek_imem((uint16_t)(address + i))) break;
      }
      if (found) dsp_lle.code_flags[address] |= CODE_IDLE_SKIP;
    }
  }
}

static void analyze(void) {
  memset(dsp_lle.code_flags, 0, sizeof dsp_lle.code_flags);
  analyze_range(0x0000, 0x1000);  // Instruction RAM
  analyze_range(0x8000, 0x9000);  // ROM
}

// --- Memory. ---

uint16_t dsp_lle_read_imem(uint16_t address) {
  switch (address >> 12) {
  case 0: return dsp_lle.iram[address & 0x0fff];
  case 8: return dsp_lle.irom[address & 0x0fff];  // The boot code and mixing loops.
  default:
    gap_hit("limit.dsp_bad_address", 0x10000u | address);  // Executing nothing.
    return 0;
  }
}

static uint16_t read_ifx(uint16_t address);
static void write_ifx(uint16_t address, uint16_t value);

uint16_t dsp_lle_read_dmem(uint16_t address) {
  switch (address >> 12) {
  case 0x0: return dsp_lle.dram[address & 0x0fff];
  case 0x1: return dsp_lle.coef[address & 0x07ff];
  case 0xf: return read_ifx(address);
  default:
    gap_hit("limit.dsp_bad_address", 0x20000u | address);
    return 0;
  }
}

void dsp_lle_write_dmem(uint16_t address, uint16_t value) {
  switch (address >> 12) {
  case 0x0: dsp_lle.dram[address & 0x0fff] = value; break;
  case 0xf: write_ifx(address, value); break;
  default: gap_hit("limit.dsp_bad_address", 0x30000u | address); break;
  }
}

uint16_t dsp_lle_fetch(void) { return dsp_lle_read_imem(dsp_lle.pc++); }

void dsp_lle_skip_instruction(void) { dsp_lle.pc += dsp_lle_op_size(dsp_lle_read_imem(dsp_lle.pc)); }

// --- Stacks: in their own hardware, not in memory. The top of each is $st0-$st3. ---

void dsp_lle_store_stack(int stack, uint16_t value) {
  dsp_lle.reg_stack_ptrs[stack] = (dsp_lle.reg_stack_ptrs[stack] + 1) & DSP_STACK_MASK;
  dsp_lle.reg_stacks[stack][dsp_lle.reg_stack_ptrs[stack]] = R.st[stack];
  R.st[stack] = value;
}

uint16_t dsp_lle_pop_stack(int stack) {
  uint16_t value = R.st[stack];
  R.st[stack] = dsp_lle.reg_stacks[stack][dsp_lle.reg_stack_ptrs[stack]];
  dsp_lle.reg_stack_ptrs[stack] = (dsp_lle.reg_stack_ptrs[stack] - 1) & DSP_STACK_MASK;
  return value;
}

// --- Exceptions. ---

void dsp_lle_set_exception(int exception) { dsp_lle.exceptions |= (uint8_t)(1u << exception); }

void dsp_lle_check_external_interrupt(void) {
  if (!(R.sr & DSP_SR_EXT_INT_ENABLE)) return;
  dsp_lle_set_exception(DSP_EXCEPTION_EXTERNAL);  // New mail for the program.
  dsp_lle.control_reg &= ~DSP_CR_EXTERNAL_INT;
}

// Takes the highest pending exception that's enabled. The external one isn't masked by the
// interrupt enable bit; a masked one stays pending.
int dsp_lle_check_exceptions(void) {
  if (!dsp_lle.exceptions) return 0;
  for (int i = 7; i > 0; i--) {
    if (!(dsp_lle.exceptions & (1u << i))) continue;
    if ((R.sr & DSP_SR_INT_ENABLE) || i == DSP_EXCEPTION_EXTERNAL) {
      dsp_lle_store_stack(0, dsp_lle.pc);  // RTI takes these back.
      dsp_lle_store_stack(1, R.sr);
      dsp_lle.pc = (uint16_t)(i * 2);
      dsp_lle.exceptions &= (uint8_t)~(1u << i);
      if (i == 7) R.sr &= ~DSP_SR_EXT_INT_ENABLE;
      else R.sr &= ~DSP_SR_INT_ENABLE;
      return 1;
    }
  }
  return 0;
}

// --- The accelerator, reading Wii memory and raising the processor's exceptions. ---

// Every main memory access the program makes goes through here, so a recording can watch them.
static DspMemoryObserver observe_read, observe_write;

void dsp_lle_observe_memory(DspMemoryObserver on_read, DspMemoryObserver on_write) {
  observe_read = on_read;
  observe_write = on_write;
}

static void (*observe_mail)(uint32_t mail);
static void (*observe_interrupt)(void);

void dsp_lle_observe_mail(void (*on_mail)(uint32_t mail), void (*on_interrupt)(void)) {
  observe_mail = on_mail;
  observe_interrupt = on_interrupt;
}

static uint8_t accelerator_read(DspAccelerator *a, uint32_t address) {
  uint8_t value = dsp_accelerator_wii_read(a, address);
  if (observe_read) observe_read(dsp_accelerator_wii_physical(address, 0), &value, 1);
  return value;
}

static void accelerator_write(DspAccelerator *a, uint32_t address, uint8_t value) {
  dsp_accelerator_wii_write(a, address, value);
  if (observe_write) observe_write(dsp_accelerator_wii_physical(address, 1), &value, 1);
}

static void raw_read_end(DspAccelerator *a) { (void)a; dsp_lle_set_exception(DSP_EXCEPTION_ACCELERATOR_RAW_READ); }
static void raw_write_end(DspAccelerator *a) { (void)a; dsp_lle_set_exception(DSP_EXCEPTION_ACCELERATOR_RAW_WRITE); }
static void sample_read_end(DspAccelerator *a) { (void)a; dsp_lle_set_exception(DSP_EXCEPTION_ACCELERATOR_SAMPLE_READ); }

static void accelerator_start(void) {
  memset(&dsp_lle.accelerator, 0, sizeof dsp_lle.accelerator);
  dsp_lle.accelerator.read_memory = accelerator_read;
  dsp_lle.accelerator.write_memory = accelerator_write;
  dsp_lle.accelerator.raw_read_end = raw_read_end;
  dsp_lle.accelerator.raw_write_end = raw_write_end;
  dsp_lle.accelerator.sample_read_end = sample_read_end;
}

// --- Mailboxes. Bit 31 of each is full: writing the low half sets it, reading it clears it. ---

static uint16_t mailbox_read_low(int box) {
  uint32_t value = dsp_lle.mailbox[box];
  dsp_lle.mailbox[box] = value & ~MAIL_FULL;
  return (uint16_t)value;
}

static uint16_t mailbox_read_high(int box) { return (uint16_t)(dsp_lle.mailbox[box] >> 16); }

static void mailbox_write_low(int box, uint16_t value) {
  dsp_lle.mailbox[box] = (dsp_lle.mailbox[box] & ~0xffffu) | value | MAIL_FULL;
}

static void mailbox_write_high(int box, uint16_t value) {
  dsp_lle.mailbox[box] = ((dsp_lle.mailbox[box] & 0xffffu) | (uint32_t)value << 16) & ~MAIL_FULL;
}

uint16_t dsp_lle_mailbox_read_high(int cpu) { return mailbox_read_high(cpu ? MAILBOX_CPU : MAILBOX_DSP); }
uint16_t dsp_lle_mailbox_read_low(int cpu) { return mailbox_read_low(cpu ? MAILBOX_CPU : MAILBOX_DSP); }
uint32_t dsp_lle_mailbox_peek(int cpu) { return dsp_lle.mailbox[cpu ? MAILBOX_CPU : MAILBOX_DSP]; }

// Only the CPU's mailbox takes writes from the CPU. A write while it's full is allowed (Dolphin
// warns: the program didn't read the last one).
void dsp_lle_mailbox_write_high(int cpu, uint16_t value) {
  if (cpu) mailbox_write_high(MAILBOX_CPU, value);
  else gap_hit("hw.mail_from_dsp", value);
}

void dsp_lle_mailbox_write_low(int cpu, uint16_t value) {
  if (cpu) mailbox_write_low(MAILBOX_CPU, value);
  else gap_hit("hw.mail_from_dsp", value);
}

// --- DMA between main memory and the processor's memories. ---

// Main memory as one block, or 0 if any of it is outside memory (Dolphin then copies nothing).
static uint8_t *main_block(uint32_t address, uint32_t bytes) {
  uint8_t *first = host(address), *last = host(address + bytes - 1);
  if (!first || !last || last != first + bytes - 1) return 0;
  return first;
}

// The program's checksum, from its bytes in main memory, and a fresh look for loops.
static void code_loaded(uint32_t address, uint32_t bytes) {
  uint8_t *code = main_block(address, bytes);
  if (code) {
    uint32_t crc = 0;
    for (uint32_t i = 0; i < bytes; i++) {
      crc ^= code[i];
      crc = crc << 3 | crc >> 29;
    }
    dsp_lle.iram_crc = crc;
  }
  analyze();
}

// Memory is big-endian, as the processor's words are. Dolphin's copies don't wrap at the end of
// the processor's memory and would run off it; ours wrap, and say so.
static void dma_to_dsp(uint16_t *memory, uint16_t word, uint32_t address, uint32_t bytes) {
  if (!bytes) return;
  uint8_t *source = main_block(address, bytes);
  if (!source) {
    gap_hit("limit.dsp_dma", address);
    return;
  }
  if (word + bytes / 2 > 0x1000) gap_hit("limit.dsp_dma", 0x10000u | word);
  for (uint32_t i = 0; i < bytes / 2; i++) memory[(word + i) & 0x0fff] = (uint16_t)(source[2 * i] << 8 | source[2 * i + 1]);
  if (observe_read) observe_read(address & 0x3fffffffu, source, bytes);
}

static void dma_from_dsp(const uint16_t *memory, uint16_t word, uint32_t address, uint32_t bytes) {
  if (!bytes) return;
  uint8_t *target = main_block(address, bytes);
  if (!target) {
    gap_hit("limit.dsp_dma", address);
    return;
  }
  if (word + bytes / 2 > 0x1000) gap_hit("limit.dsp_dma", 0x10000u | word);
  for (uint32_t i = 0; i < bytes / 2; i++) {
    uint16_t value = memory[(word + i) & 0x0fff];
    target[2 * i] = (uint8_t)(value >> 8);
    target[2 * i + 1] = (uint8_t)value;
  }
  if (observe_write) observe_write(address & 0x3fffffffu, target, bytes);
}

// Done at once, so the busy bit is never seen set. (Dolphin: these should take time.)
static void do_dma(void) {
  uint16_t *ifx = dsp_lle.ifx_regs;
  uint32_t address = (uint32_t)ifx[DSP_DSMAH] << 16 | ifx[DSP_DSMAL];
  uint16_t control = ifx[DSP_DSCR];
  uint16_t dsp_address = (uint16_t)(ifx[DSP_DSPA] * 2);  // In bytes.
  uint16_t length = ifx[DSP_DSBL];
  if (length > DMA_LONGEST) {
    gap_hit("limit.dsp_dma", 0x20000u | length);  // Dolphin stops the emulator here.
    return;
  }
  switch (control & 3) {
  case DMA_TO_CPU:  // Data memory to main memory.
    dma_from_dsp(dsp_lle.dram, dsp_address / 2, address, length);
    break;
  case 0:  // Main memory to data memory.
    dma_to_dsp(dsp_lle.dram, dsp_address / 2, address, length);
    break;
  case DMA_IMEM | DMA_TO_CPU:  // Instruction memory out: Dolphin only logs it.
    gap_hit("limit.dsp_dma", 0x30000u | dsp_address / 2);
    break;
  case DMA_IMEM:  // A new program.
    dma_to_dsp(dsp_lle.iram, dsp_address / 2, address, length);
    code_loaded(address, length);
    break;
  }
}

// --- Hardware registers. ---

// Registers Dolphin names; others are kept but reported, as Dolphin logs them as errors.
static int known_register(uint16_t low) {
  return (low >= 0xa0 && low <= 0xaf) || low == DSP_DSCR || low == DSP_DSBL || low == DSP_DSPA ||
         low == DSP_DSMAH || low == DSP_DSMAL || low == DSP_FORMAT || (low >= DSP_ACDRAW && low <= DSP_ACIN) ||
         low == DSP_AMDM || low >= DSP_DIRQ;
}

static void write_ifx(uint16_t address, uint16_t value) {
  DspAccelerator *a = &dsp_lle.accelerator;
  uint16_t *ifx = dsp_lle.ifx_regs;
  uint16_t low = address & 0xff;
  switch (low) {
  case DSP_DIRQ:
    if (value & 1) {
      if (observe_interrupt) observe_interrupt();
      dsp_interrupt();
    }
    else if (value) gap_hit("limit.dsp_unknown_register", (uint32_t)low << 16 | value);  // 0 clears; libasnd writes it.
    break;
  case DSP_DMBH: mailbox_write_high(MAILBOX_DSP, value); break;
  case DSP_DMBL:
    mailbox_write_low(MAILBOX_DSP, value);
    if (observe_mail) observe_mail(dsp_lle.mailbox[MAILBOX_DSP]);  // As the game reads it, full.
    break;
  case DSP_CMBH: mailbox_write_high(MAILBOX_CPU, value); break;
  case DSP_CMBL: mailbox_write_low(MAILBOX_CPU, value); break;
  case DSP_DSBL:  // Starts the transfer.
    ifx[DSP_DSBL] = value;
    ifx[DSP_DSCR] |= DMA_BUSY;
    if (!ifx[DSP_AMDM]) do_dma();
    else gap_hit("limit.dsp_dma", 0x40000u | ifx[DSP_AMDM]);  // Masked: skipped, as in Dolphin.
    ifx[DSP_DSCR] &= ~DMA_BUSY;
    ifx[DSP_DSBL] = 0;
    break;
  case DSP_DSPA:
  case DSP_DSMAH:
  case DSP_DSMAL:
  case DSP_DSCR:
    ifx[low] = value;
    break;
  case DSP_ACSAH: dsp_accelerator_set_start_address(a, (uint32_t)value << 16 | (uint16_t)a->start_address); break;
  case DSP_ACSAL: dsp_accelerator_set_start_address(a, (a->start_address >> 16) << 16 | value); break;
  case DSP_ACEAH: dsp_accelerator_set_end_address(a, (uint32_t)value << 16 | (uint16_t)a->end_address); break;
  case DSP_ACEAL: dsp_accelerator_set_end_address(a, (a->end_address >> 16) << 16 | value); break;
  case DSP_ACCAH: dsp_accelerator_set_current_address(a, (uint32_t)value << 16 | (uint16_t)a->current_address); break;
  case DSP_ACCAL: dsp_accelerator_set_current_address(a, (a->current_address >> 16) << 16 | value); break;
  case DSP_FORMAT: dsp_accelerator_set_sample_format(a, value); break;
  case DSP_GAIN: dsp_accelerator_set_gain(a, (int16_t)value); break;
  case DSP_YN1: dsp_accelerator_set_yn1(a, (int16_t)value); break;
  case DSP_YN2: dsp_accelerator_set_yn2(a, (int16_t)value); break;
  case DSP_PRED_SCALE: dsp_accelerator_set_pred_scale(a, value); break;
  case DSP_ACDRAW: dsp_accelerator_write_raw(a, value); break;
  case DSP_ACIN: dsp_accelerator_set_input(a, value); break;
  default:
    if (!known_register(low)) gap_hit("limit.dsp_unknown_register", (uint32_t)low << 16 | value);
    ifx[low] = value;
    break;
  }
}

static uint16_t read_ifx(uint16_t address) {
  DspAccelerator *a = &dsp_lle.accelerator;
  uint16_t low = address & 0xff;
  switch (low) {
  case DSP_DMBH: return mailbox_read_high(MAILBOX_DSP);
  case DSP_DMBL: return mailbox_read_low(MAILBOX_DSP);
  case DSP_CMBH: return mailbox_read_high(MAILBOX_CPU);
  case DSP_CMBL: return mailbox_read_low(MAILBOX_CPU);
  case DSP_DSCR: return dsp_lle.ifx_regs[low];
  case DSP_ACSAH: return (uint16_t)(a->start_address >> 16);
  case DSP_ACSAL: return (uint16_t)a->start_address;
  case DSP_ACEAH: return (uint16_t)(a->end_address >> 16);
  case DSP_ACEAL: return (uint16_t)a->end_address;
  case DSP_ACCAH: return (uint16_t)(a->current_address >> 16);
  case DSP_ACCAL: return (uint16_t)a->current_address;
  case DSP_FORMAT: return a->sample_format;
  case DSP_GAIN: return (uint16_t)a->gain;
  case DSP_YN1: return (uint16_t)a->yn1;
  case DSP_YN2: return (uint16_t)a->yn2;
  case DSP_PRED_SCALE: return a->pred_scale;
  case DSP_ACDSAMP: return dsp_accelerator_read_sample(a, (const int16_t *)&dsp_lle.ifx_regs[DSP_COEF_A1_0]);
  case DSP_ACDRAW: return dsp_accelerator_read_raw(a);
  case DSP_ACIN: return a->input;
  default:
    if (!known_register(low)) gap_hit("limit.dsp_unknown_register", (uint32_t)low << 16);
    return dsp_lle.ifx_regs[low];
  }
}

// --- Starting, the control register and running. ---

static uint32_t adler32(const uint16_t *words, uint32_t count) {
  // Dolphin hashes its in-memory copy, whose words are in the host's (little-endian) order.
  uint32_t a = 1, b = 0;
  for (uint32_t i = 0; i < count; i++) {
    a = (a + (words[i] & 0xff)) % 65521;
    b = (b + a) % 65521;
    a = (a + (words[i] >> 8)) % 65521;
    b = (b + a) % 65521;
  }
  return b << 16 | a;
}

static int known_roms(void) {
  static const uint32_t known[7][2] = {
      {0x66f334fe, 0xf3b93527},  // Nintendo's own
      {0x9c8f593c, 0x10000001},  // Free v0.1 (Zelda only)
      {0xd9907f71, 0xb019c2fb},  // v0.2
      {0xd9907f71, 0xdb6880c1},  // v0.2.1
      {0x3aa4a793, 0xa4a575f5},  // v0.3
      {0x128ea7a2, 0xa4a575f5},  // v0.3.1
      {0xe789b5a5, 0xa4a575f5},  // v0.4
  };
  uint32_t rom = adler32(dsp_lle.irom, 0x1000), coef = adler32(dsp_lle.coef, 0x800);
  for (int i = 0; i < 7; i++) {
    if (rom == known[i][0] && coef == known[i][1]) return 1;
  }
  return 0;
}

int dsp_lle_start(const uint8_t *rom, const uint8_t *coef) {
  memset(&dsp_lle, 0, sizeof dsp_lle);
  dsp_lle_tables_start();
  for (int i = 0; i < 0x1000; i++) dsp_lle.irom[i] = (uint16_t)(rom[2 * i] << 8 | rom[2 * i + 1]);
  for (int i = 0; i < 0x800; i++) dsp_lle.coef[i] = (uint16_t)(coef[2 * i] << 8 | coef[2 * i + 1]);
  if (!known_roms()) return 0;
  for (int i = 0; i < 0x1000; i++) dsp_lle.iram[i] = 0x0021;  // HALT everywhere.
  for (int i = 0; i < 4; i++) R.wr[i] = 0xffff;  // As a real console has them once a program is in.
  R.sr |= DSP_SR_INT_ENABLE | DSP_SR_EXT_INT_ENABLE;
  dsp_lle.control_reg = DSP_CR_INIT | DSP_CR_HALT;
  for (int i = 0; i < 5; i++) dsp_lle.write_back_log_idx[i] = -1;
  accelerator_start();
  dsp_lle.started = 1;
  dsp_lle_reset();
  return 1;
}

void dsp_lle_reset(void) {
  dsp_lle.pc = RESET_VECTOR;
  for (int i = 0; i < 4; i++) R.wr[i] = 0xffff;
  analyze();
}

// Dolphin's Interpreter::WriteControlRegister. The external interrupt bit is handled by the caller.
static void write_control(uint16_t value) {
  if (value & DSP_CR_RESET) {
    dsp_lle_reset();
    value &= ~DSP_CR_RESET;
  }
  // Clearing init loads 4 KB of program from 0x81000000 and starts it at 0, and sets init code,
  // which clears a little later. (Dolphin isn't sure what writing init code itself does.)
  if ((dsp_lle.control_reg & DSP_CR_INIT) && !(value & DSP_CR_INIT)) {
    dsp_lle.pc = 0;
    dma_to_dsp(dsp_lle.iram, 0, INIT_CODE_ADDRESS, INIT_CODE_BYTES);
    code_loaded(INIT_CODE_ADDRESS, INIT_CODE_BYTES);
    value &= ~DSP_CR_INIT;
    value |= DSP_CR_INIT_CODE;
    dsp_lle.control_reg_init_code_clear_time = time_base() + INIT_CODE_TICKS;
  }
  dsp_lle.control_reg = value;
}

uint16_t dsp_lle_control_write(uint16_t value) {
  write_control(value);
  if (value & DSP_CR_EXTERNAL_INT) {
    dsp_lle_check_external_interrupt();
    dsp_lle_check_exceptions();
  }
  return dsp_lle_control_read();
}

uint16_t dsp_lle_control_read(void) {
  if ((dsp_lle.control_reg & DSP_CR_INIT_CODE) && time_base() >= dsp_lle.control_reg_init_code_clear_time) {
    dsp_lle.control_reg &= ~DSP_CR_INIT_CODE;
  }
  return dsp_lle.control_reg;
}

int dsp_lle_halted(void) { return (dsp_lle.control_reg & DSP_CR_HALT) != 0; }

// The boot ROM's last steps: a DMA of the program into instruction memory, through the same
// registers, then a jump to it.
void dsp_lle_boot(uint32_t iram_address, uint32_t iram_bytes, uint16_t iram_dsp_address, uint16_t start_pc) {
  if (iram_bytes > DMA_LONGEST) {
    gap_hit("limit.dsp_dma", 0x20000u | (iram_bytes > 0xffff ? 0xffff : iram_bytes));
    return;
  }
  dsp_lle.ifx_regs[DSP_DSMAH] = (uint16_t)(iram_address >> 16);
  dsp_lle.ifx_regs[DSP_DSMAL] = (uint16_t)iram_address;
  dsp_lle.ifx_regs[DSP_DSPA] = iram_dsp_address;
  dsp_lle.ifx_regs[DSP_DSCR] = DMA_IMEM;
  write_ifx(0xff00 | DSP_DSBL, (uint16_t)iram_bytes);
  dsp_lle.pc = start_pc;
  dsp_lle.control_reg &= ~DSP_CR_HALT;
}

uint32_t dsp_lle_iram_crc(void) { return dsp_lle.iram_crc; }

// The whole state is the one structure, but the accelerator's hooks are this program's own
// addresses, so they're put back rather than taken from the copy.
void dsp_lle_restore(const DspLle *state) {
  dsp_lle = *state;
  DspAccelerator *a = &dsp_lle.accelerator;
  a->read_memory = accelerator_read;
  a->write_memory = accelerator_write;
  a->raw_read_end = raw_read_end;
  a->raw_write_end = raw_write_end;
  a->sample_read_end = sample_read_end;
  a->user = 0;
  dsp_lle_tables_start();
}

void dsp_lle_step(void) {
  dsp_lle_check_exceptions();
  dsp_lle.step_counter++;
  uint16_t opc = dsp_lle_fetch();
  dsp_lle_execute(opc);
  if (dsp_lle.code_flags[(uint16_t)(dsp_lle.pc - 1)] & CODE_LOOP_END) dsp_lle_handle_loop();
}

// Dolphin's Interpreter::RunCycles: a few instructions, then a few that stop early at a mail
// wait loop, then a longer run, and again. Each instruction counts as one cycle.
static int run_cycles(int cycles) {
  for (int i = 0; i < 8; i++) {
    if (dsp_lle.control_reg & DSP_CR_HALT) return 0;
    dsp_lle_step();
    if (--cycles <= 0) return 0;
  }
  for (;;) {
    for (int i = 0; i < 8; i++) {
      if (dsp_lle.control_reg & DSP_CR_HALT) return 0;
      if (dsp_lle.code_flags[dsp_lle.pc] & CODE_IDLE_SKIP) return 0;
      dsp_lle_step();
      if (--cycles <= 0) return 0;
    }
    for (int i = 0; i < 200; i++) {
      dsp_lle_step();
      if (--cycles <= 0) return 0;
    }
  }
}

int dsp_lle_run(int cycles) {
  if (!dsp_lle.started) return cycles;
  while (cycles > 0) cycles = run_cycles(cycles);
  return cycles;
}
