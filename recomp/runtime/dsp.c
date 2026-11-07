// The audio processor's mailboxes, control bits and program slot. The rules for the mail
// queue follow Dolphin's HW/DSPHLE/MailHandler.cpp, the registers DSPHLE.cpp, and the
// boot ROM, start-up and memory card programs ROM.cpp, INIT.cpp and CARD.cpp.
//
// A program with no C version is run as it is, by the interpreter (dsp_lle.c) on Dolphin's
// free boot ROM, which takes over the mailboxes and control bits from then on.
// WII_RECOMP_DSP=lle runs every program that way, from power-on, to check C versions.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "dsp.h"
#include "dsp_lle.h"
#include "dsp_record.h"
#include "gaps.h"
#include "zelda.h"

#define MAILBOX_TO_DSP 0xcc005000u
#define MAILBOX_FROM_DSP 0xcc005004u
#define MAIL_FULL 0x80000000u
#define QUEUE 64u                      // Mail the game hasn't read yet. Programs send a few at a time.
#define TIMERS 16u                     // Interrupts waiting their moment.
#define LATER 16u                      // Mail the program hasn't finished working out yet.
#define TICKS_PER_SECOND 60750000ull
#define CPU_CYCLES_PER_TICK 12u        // 729 MHz against the 60.75 MHz time base.
#define UPDATE_TICKS (TICKS_PER_SECOND / 1000)  // Programs are looked at every millisecond, as in Dolphin.
#define INIT_CODE_TICKS 130u           // How long the start-up bit stays set: Dolphin measured it on a Wii.

#define DSP_CYCLES_PER_TICK 2u      // A sixth of the processor's clock, as Dolphin's DSPLLE runs it.
#define MAIL_SLICE 72u              // Run before a mailbox read, so a waiting loop sees progress (DSP.cpp).
#define MOST_CYCLES_OWED 243000u    // 2 ms: past this, a long gap between checks is let go.

extern const unsigned char data_dsp_rom[], data_dsp_coef[];  // Embedded by build_file.py.
static int interpreting;           // The interpreter owns the mailboxes and control bits.
static uint64_t interpreted_until;
static uint32_t cycles_owed;

static struct { uint32_t mail; int interrupt; } queue[QUEUE];
static uint32_t queue_first, queue_count, last_mail, halted;
static uint32_t to_dsp;  // The game's mailbox, as it writes it.
static uint32_t control = DSP_CONTROL_HALT | DSP_CONTROL_INIT;
static uint64_t init_code_clear_at;
static uint64_t timers[TIMERS];
static struct { uint64_t due; uint32_t mail; int interrupt; } later[LATER];
static uint32_t later_first, later_count;
static uint64_t next_update;

typedef struct { const DspProgram *program; uint32_t crc; } Slot;
static Slot current = {0, PROGRAM_NONE}, parked = {0, PROGRAM_NONE};

// The ten mails after TASK_NEW_PROGRAM describe the next program (UCodes.cpp, PrepareBootUCode).
static int taking_new_program, new_program_mails, resume_mail_wanted;
static uint32_t new_iram_address, new_iram_length, new_iram_dsp_address, new_start, new_mram_length, new_dram_length;

uint32_t dsp_hash(const uint8_t *data, uint32_t length) {
  uint32_t crc = 0;
  for (uint32_t i = 0; i < length; i++) {
    crc ^= data[i];
    crc = crc << 3 | crc >> 29;
  }
  return crc;
}

// The game's program, still in main memory. Dolphin hashes the bytes the game names.
static uint32_t hash_in_memory(uint32_t address, uint32_t length) {
  uint8_t *first = host(address), *last = length ? host(address + length - 1) : first;
  if (!first || !last || last != first + length - 1) {
    gap_hit("hw.instruction_ram_rom_real_dsp_code", address);
    return PROGRAM_NONE;
  }
  return dsp_hash(first, length);
}

uint32_t dsp_program_crc(void) { return current.crc; }

void dsp_mail_interrupt(uint32_t cycles_later) {
  if (!cycles_later) {
    dsp_interrupt();
    return;
  }
  uint64_t due = time_base() + (cycles_later + CPU_CYCLES_PER_TICK - 1) / CPU_CYCLES_PER_TICK;
  for (uint32_t i = 0; i < TIMERS; i++) {
    if (!timers[i]) {
      timers[i] = due;
      return;
    }
  }
  gap_hit("limit.dsp_interrupts_waiting", TIMERS);
  dsp_interrupt();
}

// An interrupt asked for while mail is still waiting comes when that mail is read.
void dsp_push_mail(uint32_t mail, int interrupt, uint32_t cycles_later) {
  if (interrupt) {
    if (!queue_count) dsp_mail_interrupt(cycles_later);
    else queue[queue_first].interrupt = 1;
  }
  if (queue_count == QUEUE) {
    gap_hit("limit.dsp_mail_queue", mail);
    return;
  }
  uint32_t at = (queue_first + queue_count++) % QUEUE;
  queue[at].mail = mail;
  queue[at].interrupt = 0;
}

// The C versions answer at once, where the program takes time: a mail that a game
// should see only after that time waits here, in order, and is sent when it falls due.
void dsp_push_mail_later(uint32_t mail, int interrupt, uint32_t cycles_later) {
  if (later_count == LATER) {
    gap_hit("limit.dsp_mail_queue", mail);
    return;
  }
  uint64_t due = time_base() + (cycles_later + CPU_CYCLES_PER_TICK - 1) / CPU_CYCLES_PER_TICK;
  if (later_count) {  // Never ahead of mail sent before it.
    uint64_t last = later[(later_first + later_count - 1) % LATER].due;
    if (due < last) due = last;
  }
  uint32_t at = (later_first + later_count++) % LATER;
  later[at].due = due, later[at].mail = mail, later[at].interrupt = interrupt;
}

static void send_mail_due(uint64_t now) {
  while (later_count && now >= later[later_first].due) {
    dsp_push_mail(later[later_first].mail, later[later_first].interrupt, 0);
    later_first = (later_first + 1) % LATER;
    later_count--;
  }
}

int dsp_mail_pending(void) { return queue_count != 0; }

static uint32_t read_high(void) {
  if (!halted && queue_count) last_mail = queue[queue_first].mail;
  return last_mail >> 16;
}

// The game reads the high half first. Taking the low half takes the mail, and the next
// high read shows the mailbox empty, with the rest of the last mail still there.
static uint32_t read_low(void) {
  if (!halted && queue_count) {
    last_mail = queue[queue_first].mail;
    int interrupt = queue[queue_first].interrupt;
    queue_first = (queue_first + 1) % QUEUE;
    queue_count--;
    if (interrupt) dsp_interrupt();
  }
  uint32_t low = last_mail & 0xffffu;
  last_mail &= ~MAIL_FULL;
  return low;
}

static const DspProgram *program_for(uint32_t crc);
static int hand_over(uint32_t crc, uint32_t address, uint32_t length, uint32_t dsp_address, uint32_t start);

void dsp_set_program(uint32_t crc) {
  queue_count = 0;
  later_count = 0;
  current.crc = crc;
  current.program = program_for(crc);
  if (current.program) current.program->start();
}

// Swapping keeps the program it leaves, once, so the game can come back to it (DSPHLE.cpp).
static void swap_program(uint32_t crc) {
  queue_count = 0;
  later_count = 0;
  if (parked.program && parked.crc == crc) {
    current = parked;
    parked.program = 0;
    parked.crc = PROGRAM_NONE;
    return;
  }
  if (hand_over(crc, new_iram_address, new_iram_length, new_iram_dsp_address, new_start)) return;
  if (!parked.program) parked = current;
  const DspProgram *next = program_for(crc);
  if (next && next == parked.program) gap_hit("limit.dsp_same_program_twice", crc);  // Each keeps one state.
  current.crc = crc;
  current.program = next;
  if (next) next->start();
}

void dsp_expect_new_program(void) { taking_new_program = 1; }
int dsp_taking_new_program(void) { return taking_new_program; }

int dsp_needs_resume_mail(void) {
  if (!resume_mail_wanted) return 0;
  resume_mail_wanted = 0;
  return 1;
}

void dsp_new_program_mail(uint32_t mail) {
  switch (new_program_mails++) {
  case 1: new_mram_length = mail & 0xffffu; break;
  case 3: new_iram_address = mail; break;
  case 4: new_iram_length = mail & 0xffffu; break;
  case 5: new_iram_dsp_address = mail & 0xffffu; break;
  case 6: new_start = mail & 0xffffu; break;
  case 8: new_dram_length = mail & 0xffffu; break;
  }
  if (new_program_mails < 10) return;
  new_program_mails = 0;
  taking_new_program = 0;
  resume_mail_wanted = 1;
  // Saving the old program's memory, or loading the new one's, isn't done by Dolphin either.
  if (new_mram_length || new_dram_length) gap_hit("audio.program_memory_upload", new_mram_length | new_dram_length << 16);
  swap_program(hash_in_memory(new_iram_address, new_iram_length));
}

// The boot ROM. It says it's ready, then takes the program's address, length, and where
// to start, a key mail before each value. Anything else is answered and ignored.
static uint32_t rom_key, rom_address, rom_length, rom_dsp_address;

static void rom_start(void) {
  rom_key = 0;
  dsp_push_mail(0x8071feedu, 0, 0);
}

static void rom_mail(uint32_t mail) {
  if (!rom_key) {
    if ((mail & 0xffff0000u) == 0x80f30000u) rom_key = mail;
    else dsp_push_mail(0xfeee0000u | (mail & 0xffffu), 0, 0);
    return;
  }
  uint32_t key = rom_key;
  rom_key = 0;
  if (key == 0x80f3a001u) rom_address = mail;
  else if (key == 0x80f3a002u) rom_length = mail & 0xffffu;
  else if (key == 0x80f3c002u) rom_dsp_address = mail & 0xffffu;
  else if (key == 0x80f3d001u) {
    uint32_t crc = hash_in_memory(rom_address, rom_length);
    fprintf(stderr, "audio: program %08x (%u bytes at %08x)\n", crc, rom_length, rom_address);
    if (!hand_over(crc, rom_address, rom_length, rom_dsp_address, mail & 0xffffu)) dsp_set_program(crc);
  }
}

static void nothing(void) {}

static const DspProgram rom_program = {"boot ROM", rom_start, rom_mail, nothing};

// What the OS runs at start-up to wake the processor. It only says it ran.
static void init_start(void) { dsp_push_mail(0x80544348u, 0, 0); }
static void ignore_mail(uint32_t mail) { (void)mail; }
static const DspProgram init_program = {"start-up", init_start, ignore_mail, nothing};

// Unlocks a GameCube memory card, then hands back to the ROM.
static void card_start(void) { dsp_push_mail(DSP_MAIL_INIT, 0, 0); }
static void card_update(void) {
  if (dsp_mail_pending()) dsp_mail_interrupt(0);
}
static void card_mail(uint32_t mail) {
  (void)mail;
  dsp_push_mail(DSP_MAIL_DONE, 0, 0);
  dsp_set_program(PROGRAM_ROM);
}
static const DspProgram card_program = {"memory card", card_start, card_mail, card_update};

// Programs we know but have no C version of yet: the interpreter runs them. Each names its
// inventory row, and if the interpreter can't start, the game carries on without answers.
#define MISSING(variable, title, row)                                     \
  static void variable##_start(void) { gap_hit(row, current.crc); }       \
  static const DspProgram variable = {title, variable##_start, ignore_mail, nothing};
static const char *missing_row(const DspProgram *program);
MISSING(ax_wii_layout_missing, "AXWii, unknown block layout", "audio.axwii_unknown_block_layout")
MISSING(gba_missing, "Game Boy Advance link", "audio.card_gba")
MISSING(homebrew_missing, "homebrew mixer", "audio.asnd_aesnd_homebrew")
MISSING(unknown_missing, "unknown", "audio.unknown_program")

// Dolphin's table of programs by checksum (UCodes.cpp, UCodeFactory), and ASnd.h, AESnd.h.
static const struct { uint32_t crc; const DspProgram *program; } known[] = {
  {0x65d6cc6fu, &card_program},
  {0xdd7e72d5u, &gba_missing},
  {0x3ad3b7acu, &ax_program}, {0x3daf59b9u, &ax_program}, {0x4e8a8b21u, &ax_program},
  {0x07f88145u, &ax_program}, {0xe2136399u, &ax_program}, {0x3389a79eu, &ax_program},
  {0x86840740u, &zelda_program}, {0x6ca33a6du, &zelda_program}, {0xd643001fu, &zelda_program}, {0x6ba3b3eau, &zelda_program},
  {0x24b22038u, &zelda_program}, {0x2fcdf1ecu, &zelda_program}, {0xdf059f68u, &zelda_program}, {0x4be6a5cbu, &zelda_program},
  {0x267fd05au, &zelda_program}, {0x42f64ac4u, &zelda_program}, {0x56d36052u, &zelda_program}, {0x6c3f6f94u, &zelda_program},
  {0xb7eb9a9cu, &zelda_program}, {0xeaeb38ccu, &zelda_program},
  // Two Wii versions whose block layout neither Dolphin nor we know: the interpreter runs them.
  {0x2ea36ce6u, &ax_wii_layout_missing}, {0x5ef56da3u, &ax_wii_layout_missing}, {0x347112bau, &ax_program},
  {0xadbc06bdu, &ax_program}, {0x4cc52064u, &ax_program}, {0xd9c4bf34u, &ax_program},
  {0xfa450138u, &ax_program}, {0x7699af32u, &ax_program},
  {0x8d69a19bu, &homebrew_missing}, {0xcc2fd441u, &homebrew_missing}, {0xa81582e2u, &homebrew_missing},
  {0xdbbeeb61u, &homebrew_missing}, {0xbad876efu, &homebrew_missing}, {0xfa9c576fu, &homebrew_missing},
  {0x614dd145u, &homebrew_missing}, {0x5dbf8bf1u, &homebrew_missing}, {0x373a950eu, &homebrew_missing},
  {0x008366afu, &homebrew_missing}, {0x078066abu, &homebrew_missing}, {0x5ad4d933u, &homebrew_missing},
  {0x84c680a9u, &homebrew_missing}, {0xa02a6131u, &homebrew_missing}, {0x2e5e4100u, &homebrew_missing},
  {0x002e5e41u, &homebrew_missing},
};

static const DspProgram *program_for(uint32_t crc) {
  if (crc == PROGRAM_ROM) return &rom_program;
  if (crc == PROGRAM_INIT) return &init_program;
  if (crc == PROGRAM_NONE) return 0;
  for (size_t i = 0; i < sizeof known / sizeof known[0]; i++) {
    if (known[i].crc == crc) return known[i].program;
  }
  return &unknown_missing;
}

static const char *missing_row(const DspProgram *program) {
  if (program == &ax_wii_layout_missing) return "audio.axwii_unknown_block_layout";
  if (program == &gba_missing) return "audio.card_gba";
  if (program == &homebrew_missing) return "audio.asnd_aesnd_homebrew";
  if (program == &unknown_missing) return "audio.unknown_program";
  return 0;
}

const char *dsp_c_version(void) {
  return current.program && !missing_row(current.program) ? current.program->name : 0;
}

static int interpreter_ready(void) {
  static int tried, ready;
  if (!tried) {
    tried = 1;
    ready = dsp_lle_start(data_dsp_rom, data_dsp_coef);
    if (!ready) gap_hit("limit.dsp_rom_checksum", 0);
  }
  return ready;
}

// A program with no C version: the interpreter takes it from where the boot ROM would jump.
static int hand_over(uint32_t crc, uint32_t address, uint32_t length, uint32_t dsp_address, uint32_t start) {
  const char *row = missing_row(program_for(crc));
  if (!row || !interpreter_ready()) return 0;
  fprintf(stderr, "audio: no C version of program %08x, so the interpreter runs it\n", crc);
  gap_note(row, crc);
  queue_count = 0;
  later_count = 0;
  current.crc = crc;
  current.program = 0;
  dsp_lle.control_reg = (uint16_t)control;
  interpreting = 1;
  interpreted_until = time_base();
  dsp_lle_boot(address, length, (uint16_t)dsp_address, (uint16_t)start);
  return 1;
}

void dsp_start(void) {
  const char *engine = getenv("WII_RECOMP_DSP");
  if (engine && !strcmp(engine, "lle")) {
    if (interpreter_ready()) {
      fprintf(stderr, "audio: the interpreter runs every program, from the boot ROM\n");
      interpreting = 1;
      interpreted_until = time_base();
      dsp_record_start();
      return;
    }
  } else if (engine) {
    fprintf(stderr, "WII_RECOMP_DSP=%s isn't known: lle runs the interpreter\n", engine);
  }
  dsp_set_program(PROGRAM_ROM);
  control = DSP_CONTROL_HALT | DSP_CONTROL_INIT;
  halted = 1;
  dsp_record_start();  // Waits for the interpreter, for a program that has no C version.
}

// The interpreter keeps time with the game: its share of the cycles since it last ran.
static void interpret(uint64_t now, uint32_t most) {
  if (now > interpreted_until) {
    uint64_t owed = cycles_owed + (now - interpreted_until) * DSP_CYCLES_PER_TICK;
    cycles_owed = owed > MOST_CYCLES_OWED ? MOST_CYCLES_OWED : (uint32_t)owed;
    interpreted_until = now;
  }
  uint32_t cycles = cycles_owed < most ? cycles_owed : most;
  if (!cycles) return;
  cycles_owed -= cycles;
  if (dsp_lle_halted()) return;
  dsp_lle_run((int)cycles);
  if (dsp_recording) dsp_record_run(cycles);
}

int dsp_interpreting(void) { return interpreting; }

uint32_t dsp_control_read(void) {
  if (interpreting) return dsp_lle_control_read();
  if ((control & DSP_CONTROL_INIT_CODE) && time_base() >= init_code_clear_at) control &= ~DSP_CONTROL_INIT_CODE;
  return control;
}

// Reset starts the boot ROM again. Clearing the init bit runs the start-up program, and
// the init code bit stays set a moment after.
uint32_t dsp_control_write(uint32_t value) {
  if (interpreting) {
    uint32_t now = dsp_lle_control_write((uint16_t)value);
    if (dsp_recording) dsp_record_control((uint16_t)value);  // After what it caused, such as a DMA.
    return now;
  }
  uint32_t next = value & DSP_CONTROL_OWN;
  halted = (next & DSP_CONTROL_HALT) != 0;
  if (next & DSP_CONTROL_RESET) {
    dsp_set_program(PROGRAM_ROM);
    next &= ~DSP_CONTROL_RESET;
  }
  if ((control & DSP_CONTROL_INIT) && !(next & DSP_CONTROL_INIT)) {
    dsp_set_program(PROGRAM_INIT);
    next |= DSP_CONTROL_INIT_CODE;
    init_code_clear_at = time_base() + INIT_CODE_TICKS;
  }
  control = next;
  return control;
}

int dsp_read(uint32_t address, int bytes, uint32_t *value) {
  if (address < MAILBOX_TO_DSP || address >= MAILBOX_FROM_DSP + 4) return 0;
  if (interpreting) {
    int cpu = address < MAILBOX_FROM_DSP, high = (address & 2) == 0;
    if (high) interpret(time_base(), MAIL_SLICE);
    if (dsp_recording && (bytes == 4 || !high)) dsp_record_read_low(cpu, dsp_lle_mailbox_peek(cpu));
    if (bytes == 4) *value = (uint32_t)dsp_lle_mailbox_read_high(cpu) << 16 | dsp_lle_mailbox_read_low(cpu);
    else *value = high ? dsp_lle_mailbox_read_high(cpu) : dsp_lle_mailbox_read_low(cpu);
    return 1;
  }
  uint32_t mail = address < MAILBOX_FROM_DSP ? to_dsp : 0;
  if (address == MAILBOX_FROM_DSP) {
    uint32_t high = read_high();
    *value = bytes == 4 ? high << 16 | read_low() : high;
  } else if (address == MAILBOX_FROM_DSP + 2) {
    *value = read_low();
  } else {
    *value = address == MAILBOX_TO_DSP ? (bytes == 4 ? mail : mail >> 16) : mail & 0xffffu;
  }
  return 1;
}

// Writing the low half sends the mail, and the full bit drops as the program takes it.
int dsp_write(uint32_t address, uint32_t value, int bytes) {
  if (address < MAILBOX_TO_DSP || address >= MAILBOX_FROM_DSP + 4) return 0;
  if (address >= MAILBOX_FROM_DSP) {
    gap_hit("hw.mail_from_dsp", value);  // Only the processor writes that one.
    return 1;
  }
  if (interpreting) {
    if (dsp_recording) {
      if (bytes == 4 || address == MAILBOX_TO_DSP) dsp_record_cpu_write(1, (uint16_t)(bytes == 4 ? value >> 16 : value));
      if (bytes == 4 || address != MAILBOX_TO_DSP) dsp_record_cpu_write(0, (uint16_t)value);
    }
    if (bytes == 4) {
      dsp_lle_mailbox_write_high(1, (uint16_t)(value >> 16));
      dsp_lle_mailbox_write_low(1, (uint16_t)value);
    } else if (address == MAILBOX_TO_DSP) {
      dsp_lle_mailbox_write_high(1, (uint16_t)value);
    } else {
      dsp_lle_mailbox_write_low(1, (uint16_t)value);
    }
    return 1;
  }
  if (address == MAILBOX_TO_DSP && bytes == 4) to_dsp = value;
  else if (address == MAILBOX_TO_DSP) {
    to_dsp = (to_dsp & 0xffffu) | value << 16;
    return 1;
  } else to_dsp = (to_dsp & 0xffff0000u) | (value & 0xffffu);
  if (current.program) {
    if (taking_new_program) dsp_new_program_mail(to_dsp);
    else current.program->mail(to_dsp);
  }
  to_dsp &= ~MAIL_FULL;
  return 1;
}

void dsp_update(uint64_t now) {
  dsp_record_update(now);
  send_mail_due(now);
  for (uint32_t i = 0; i < TIMERS; i++) {
    if (timers[i] && now >= timers[i]) {
      timers[i] = 0;
      dsp_interrupt();
    }
  }
  if (interpreting) {
    interpret(now, MOST_CYCLES_OWED);
    return;
  }
  if (now < next_update) return;
  next_update = now + UPDATE_TICKS;
  if (current.program) current.program->update();
}

uint64_t dsp_next_due(void) {
  if (interpreting && !dsp_lle_halted()) return time_base() + UPDATE_TICKS;  // It runs while the game sleeps.
  uint64_t due = later_count ? later[later_first].due : 0;
  for (uint32_t i = 0; i < TIMERS; i++) {
    if (timers[i] && (!due || timers[i] < due)) due = timers[i];
  }
  return due;
}
