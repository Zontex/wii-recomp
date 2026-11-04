// Runs translated code and the interpreter from identical states and compares everything
// each one changed. See test_recomp_differential.py.
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "differential.h"
#include "interp.h"

Cpu cpu;
uint8_t *mem1, *mem2;
int interrupt_budget;

// Everything guest code can do apart from registers and memory.
typedef struct {
  uint64_t hardware_hash, guest_time_offset;
  uint32_t unknown_count, polls, system_calls, system_call_address, traps, trap_address;
  uint32_t decrementer_writes, decrementer_value, cache_transfers;
} Effects;

static Effects effects;
static const Function *functions;
static int function_count_now;
static uint32_t function_hidden;

#define TIME_NOW 0x0123456789abcdefull
#define DECREMENTER_NOW 0x00c0ffeeu

static void mix(uint64_t value) { effects.hardware_hash = (effects.hardware_hash ^ value) * 0x100000001b3ull; }

uint32_t hw_read(uint32_t address, int bytes) {
  mix(address);
  mix((uint64_t)bytes);
  uint32_t value = address * 2654435761u;
  return bytes == 4 ? value : value & (bytes == 2 ? 0xffff : 0xff);
}
void hw_write(uint32_t address, uint32_t value, int bytes) {
  mix(address);
  mix(value);
  mix((uint64_t)bytes);
}
uint64_t time_base(void) { return TIME_NOW; }
uint64_t guest_time_base(void) { return TIME_NOW + effects.guest_time_offset; }
void guest_time_base_set(int high_half, uint32_t value) {
  uint64_t now = guest_time_base();
  uint64_t wanted = high_half ? (uint64_t)value << 32 | (uint32_t)now : (now & 0xffffffff00000000ull) | value;
  effects.guest_time_offset += wanted - now;
}
void decrementer_set(uint32_t value) { effects.decrementer_writes++; effects.decrementer_value = value; }
uint32_t decrementer_get(void) { return DECREMENTER_NOW; }
void locked_cache_transfer(void) { effects.cache_transfers++; }
void poll_interrupts(void) { interrupt_budget = 0; effects.polls++; }
void fpscr_mode_changed(uint32_t fpscr) { (void)fpscr; }
void system_call(uint32_t address) { effects.system_calls++; effects.system_call_address = address; }
void trap_taken(uint32_t address) { effects.traps++; effects.trap_address = address; }
void unimplemented(uint32_t address, const char *name) { effects.unknown_count++; }

GuestFunction translated_at(uint32_t address) {
  for (int i = 0; i < function_count_now; i++) {
    if (functions[i].address == address && address != function_hidden) return functions[i].run;
  }
  return 0;
}

void dispatch(uint32_t address) {
  GuestFunction run = translated_at(address);
  if (run) run();
  else interpret(address);
}

// Pseudo-random, and the same on every run.
static uint64_t random_state;
static uint32_t next_random(void) {
  random_state ^= random_state >> 12;
  random_state ^= random_state << 25;
  random_state ^= random_state >> 27;
  return (uint32_t)((random_state * 0x2545f4914f6cdd1dull) >> 32);
}
static uint64_t next_random64(void) { return (uint64_t)next_random() << 32 | next_random(); }
static uint32_t below(uint32_t limit) { return next_random() % limit; }

static const uint32_t INTEGERS[] = {
    0, 1, 2, 0xffffffffu, 0x7fffffffu, 0x80000000u, 0x80000001u, 0xfffffffeu, 0xffff, 0x10000, 0xffff0000u,
    31, 32, 33, 63, 64, 0xff, 0x80, 0x8000,
};
static const uint64_t DOUBLES[] = {
    0, DOUBLE_SIGN, 0x3ff0000000000000ull, 0xbff0000000000000ull,  // 0, -0, 1, -1
    0x3fe0000000000000ull, 0xbfe0000000000000ull, 0x3ff8000000000000ull,  // 0.5, -0.5, 1.5
    0x4004000000000000ull, 0xc004000000000000ull, 0x400c000000000000ull,  // 2.5, -2.5, 3.5
    0x3fd3333333333333ull, 0xbfd3333333333333ull, 0xbfe6666666666666ull,  // 0.3, -0.3, -0.7
    0x41dfffffffc00000ull, 0x41dfffffffe00000ull, 0x41e0000000000000ull,  // 2^31-1, 2^31-0.5, 2^31
    0xc1e0000000000000ull, 0xc1e0000000100000ull, 0xc1e0000000200000ull,  // -2^31, -2^31-0.5, -2^31-1
    0x4202a05f20000000ull, 0xc202a05f20000000ull,  // 1e10, -1e10
    0x7ff0000000000000ull, 0xfff0000000000000ull,  // Infinities.
    0x7ff8000000000000ull, 0xfff8000000000000ull, 0x7ff8000012345678ull,  // Quiet NaNs.
    0x7ff0000000000001ull, 0xfff0000000000001ull, 0x7ff4000020000000ull,  // Signaling NaNs.
    1, 0x000fffffffffffffull, DOUBLE_SIGN | 1,  // Double denormals.
    0x36a0000000000000ull, 0x3690000000000000ull, 0xb80fffffc0000000ull, 0x380fffffe0000000ull,  // Float denormals.
    0x47efffffe0000000ull, 0x47f0000000000000ull, 0x47effffff0000000ull,  // Float's upper limit.
    0x7e37e43c8800759cull, 0x01a56e1fc2f8f359ull,  // 1e300, 1e-300
    0x3ff0000000000001ull, 0x3fb999999999999aull,  // Needs more than a float's precision.
};
static const uint32_t FLOAT_WORDS[] = {
    0, 0x80000000u, 0x3f800000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u,
    0x7f800001u, 0xff800001u, 0x7fbfffffu, 0xffa040ffu, 1, 0x80000001u, 0x007fffffu, 0x00800000u,
};
#define COUNT(table) (sizeof(table) / sizeof(table[0]))

static uint32_t random_integer(void) {
  return below(3) ? INTEGERS[below(COUNT(INTEGERS))] + (below(4) ? 0 : below(3) - 1) : next_random();
}

static double random_float(void) {
  switch (below(4)) {
    case 0: return as_double(DOUBLES[below(COUNT(DOUBLES))]);
    case 1: return as_double(next_random64());
    case 2: return (double)(float)((int32_t)next_random() % 4000) / 64.0;
    default: return (double)as_float(next_random() & 0xbfffffffu | 0x30000000u);  // A sensible single.
  }
}

// Valid GQR types, plus now and then a reserved one.
static uint32_t random_gqr(void) {
  static const uint32_t TYPES[] = {0, 4, 5, 6, 7, 0, 4, 5, 6, 7, 1, 2, 3};
  return TYPES[below(COUNT(TYPES))] | TYPES[below(COUNT(TYPES))] << 16 | below(64) << 8 | below(64) << 24;
}

#define WINDOW_BYTES 0x1000u
#define WINDOW_MIDDLE 0x800u
#define WINDOWS 3
static uint8_t *windows[WINDOWS];
#define MEM1_WINDOW 0x100000u
#define MEM2_WINDOW 0x100000u

typedef struct {
  Cpu cpu;
  Effects effects;
  uint8_t memory[WINDOWS][WINDOW_BYTES];
} Snapshot;

static Snapshot start_state, first, second;

// Copying memory is most of the work, so it only happens for code that can touch it.
static int memory_in_use;

static void capture(Snapshot *to) {
  to->cpu = cpu;
  to->effects = effects;
  if (!memory_in_use) return;
  for (int i = 0; i < WINDOWS; i++) memcpy(to->memory[i], windows[i], WINDOW_BYTES);
}

static void restore(const Snapshot *from) {
  cpu = from->cpu;
  effects = from->effects;
  if (!memory_in_use) return;
  for (int i = 0; i < WINDOWS; i++) memcpy(windows[i], from->memory[i], WINDOW_BYTES);
}

static void randomize_memory(void) {
  for (int i = 0; i < WINDOWS; i++) {
    for (uint32_t at = 0; at < WINDOW_BYTES; at += 4) {
      uint32_t value = below(3) ? next_random() : FLOAT_WORDS[below(COUNT(FLOAT_WORDS))];
      memcpy(windows[i] + at, &value, 4);
    }
  }
}

static void randomize_registers(void) {
  memset(&cpu, 0, sizeof cpu);
  memset(&effects, 0, sizeof effects);
  for (int i = 0; i < 32; i++) {
    cpu.r[i] = random_integer();
    cpu.f[i] = random_float();
    cpu.p[i] = random_float();
  }
  for (int i = 0; i < 8; i++) cpu.cr[i] = (uint8_t)below(16);
  for (int i = 0; i < 16; i++) cpu.sr[i] = next_random();
  for (int i = 0; i < 8; i++) cpu.spr[912 + i] = random_gqr();
  for (int i = 16; i < 32; i++) cpu.spr[i + 256] = next_random();  // SPRG, EAR, TBL and more.
  cpu.spr[26] = next_random();
  cpu.spr[27] = next_random();
  cpu.lr = next_random();
  cpu.ctr = below(4) ? below(3) : next_random();
  xer_set(next_random() & 0xe000ffffu & ~0x80u);
  if (below(2)) cpu.xer_string &= 0x7f;
  cpu.msr = next_random();
  cpu.fpscr = next_random() & ~(uint32_t)FPSCR_RESERVED;
}

// Base registers land in one of the memory windows, or sometimes in hardware.
static uint32_t random_base(void) {
  switch (below(8)) {
    case 0: return 0xc0000000u + MEM1_WINDOW + WINDOW_MIDDLE;  // Uncached view of MEM1.
    case 1: return 0x90000000u + MEM2_WINDOW + WINDOW_MIDDLE;
    case 2: return 0xcc003000u + WINDOW_MIDDLE;
    default: return 0x80000000u + MEM1_WINDOW + WINDOW_MIDDLE;
  }
}

static void aim_at_memory(const Case *c) {
  uint32_t base = random_base() + below(64);
  if (c->base > 0) cpu.r[c->base] = base;
  if (c->index >= 0) cpu.r[c->index] = c->base > 0 ? below(256) : base;
}

static int nan_bits(uint64_t bits) { return (bits & 0x7fffffffffffffffull) > 0x7ff0000000000000ull; }

// Returns differences found. Prints the first ones only.
static int printed;
#define MOST_PRINTED 3  // For each form.

static void report(const char *name, uint32_t word, int state, const char *field, int index, uint64_t a, uint64_t b) {
  if (printed++ >= MOST_PRINTED) return;
  printf("DIFF %s %08x state=%d %s[%d] translated=%llx interpreted=%llx\n", name, word, state, field, index,
         (unsigned long long)a, (unsigned long long)b);
}

static int nan_only_differences;

static int compare(const char *name, uint32_t word, int state, int any_nan) {
  if (!memcmp(&first, &second, memory_in_use ? sizeof first : offsetof(Snapshot, memory))) return 0;
  int found = 0;
#define SAME(field, index, a, b) do { if ((uint64_t)(a) != (uint64_t)(b)) { report(name, word, state, field, index, a, b); found++; } } while (0)
  for (int i = 0; i < 32; i++) SAME("r", i, first.cpu.r[i], second.cpu.r[i]);
  for (int i = 0; i < 64; i++) {
    uint64_t a = double_bits(i < 32 ? first.cpu.f[i] : first.cpu.p[i - 32]);
    uint64_t b = double_bits(i < 32 ? second.cpu.f[i] : second.cpu.p[i - 32]);
    if (a == b) continue;
    if (any_nan && nan_bits(a) && nan_bits(b)) nan_only_differences++;
    else { report(name, word, state, i < 32 ? "f" : "p", i & 31, a, b); found++; }
  }
  for (int i = 0; i < 8; i++) SAME("cr", i, first.cpu.cr[i], second.cpu.cr[i]);
  SAME("lr", 0, first.cpu.lr, second.cpu.lr);
  SAME("ctr", 0, first.cpu.ctr, second.cpu.ctr);
  SAME("ca", 0, first.cpu.ca, second.cpu.ca);
  SAME("so", 0, first.cpu.so, second.cpu.so);
  SAME("ov", 0, first.cpu.ov, second.cpu.ov);
  SAME("xer_string", 0, first.cpu.xer_string, second.cpu.xer_string);
  SAME("msr", 0, first.cpu.msr, second.cpu.msr);
  SAME("fpscr", 0, first.cpu.fpscr, second.cpu.fpscr);
  for (int i = 0; i < 1024; i++) SAME("spr", i, first.cpu.spr[i], second.cpu.spr[i]);
  for (int i = 0; i < 16; i++) SAME("sr", i, first.cpu.sr[i], second.cpu.sr[i]);
  for (int w = 0; w < WINDOWS && memory_in_use; w++) {
    for (uint32_t at = 0; at < WINDOW_BYTES; at++) SAME("memory", (int)at, first.memory[w][at], second.memory[w][at]);
  }
  SAME("hardware", 0, first.effects.hardware_hash, second.effects.hardware_hash);
  SAME("guest_time", 0, first.effects.guest_time_offset, second.effects.guest_time_offset);
  SAME("unknown", 0, first.effects.unknown_count, second.effects.unknown_count);
  SAME("decrementer_writes", 0, first.effects.decrementer_writes, second.effects.decrementer_writes);
  SAME("decrementer", 0, first.effects.decrementer_value, second.effects.decrementer_value);
  SAME("cache_transfers", 0, first.effects.cache_transfers, second.effects.cache_transfers);
  SAME("system_calls", 0, first.effects.system_calls, second.effects.system_calls);
  SAME("system_call_address", 0, first.effects.system_call_address, second.effects.system_call_address);
  SAME("traps", 0, first.effects.traps, second.effects.traps);
  SAME("trap_address", 0, first.effects.trap_address, second.effects.trap_address);
  return found;
}

static int run_case(int index, int states) {
  const Case *c = &cases[index];
  int bad = 0;
  printed = 0;
  for (int state = 0; state < states; state++) {
    random_state = 0x9e3779b97f4a7c15ull ^ ((uint64_t)index + 1) << 20 ^ (uint64_t)state + 1;
    randomize_registers();
    memory_in_use = c->flags & CASE_MEMORY;
    if (memory_in_use) {
      randomize_memory();
      aim_at_memory(c);
    }
    capture(&start_state);
    c->translated();
    capture(&first);
    restore(&start_state);
    interpret_instruction(0x80001000u, c->word);
    capture(&second);
    if (compare(c->name, c->word, state, c->flags & CASE_ANY_NAN)) bad++;
    restore(&start_state);
  }
  return bad;
}

#define STACK_TOP (0x80000000u + MEM1_WINDOW + WINDOW_MIDDLE)
#define STOP_ADDRESS 0x80000104u

static void load_program(const Program *p) {
  for (int i = 0; i < p->word_count; i++) wr32(p->start + (uint32_t)i * 4, p->words[i]);
}

static void set_up_program_state(uint64_t seed) {
  random_state = seed;
  randomize_registers();
  randomize_memory();
  cpu.r[1] = STACK_TOP;
  cpu.lr = STOP_ADDRESS;
  interrupt_budget = 0;
}

static void clear_polls_unless(int compared) { if (!compared) first.effects.polls = second.effects.polls = 0; }

static int run_program(int index, int states) {
  const Program *p = &programs[index];
  int bad = 0;
  printed = 0;
  load_program(p);
  functions = p->functions;
  memory_in_use = 1;
  for (int state = 0; state < states; state++) {
    set_up_program_state(0x853c49e6748fea9bull ^ ((uint64_t)index + 1) << 24 ^ (uint64_t)state + 1);
    capture(&start_state);

    static Snapshot reference;
    function_count_now = p->function_count;
    function_hidden = 0;
    p->functions[0].run();
    capture(&reference);

    // The three other ways to split the work between translated and interpreted code.
    for (int way = 0; way < 3; way++) {
      if (way && p->function_count < 2) break;
      restore(&start_state);
      interrupt_budget = 0;
      function_count_now = way == 0 ? 0 : p->function_count;
      function_hidden = way == 1 ? p->start : 0;
      if (way == 2) {
        // Only the first function translated, reaching the others through dispatch.
        function_count_now = 1;
        p->through_dispatch();
      } else {
        interpret(p->start);
      }
      capture(&second);
      first = reference;
      clear_polls_unless(p->compare_polls);
      if (p->compare_polls && first.effects.polls != second.effects.polls) {
        report(p->name, 0, state, "polls", way, first.effects.polls, second.effects.polls);
        bad++;
      }
      first.effects.polls = second.effects.polls = 0;
      static char label[80];
      snprintf(label, sizeof label, "%s/way%d", p->name, way);
      if (compare(label, 0, state, 1)) bad++;
    }
    restore(&start_state);
  }
  return bad;
}

static int check_decoding(void) {
  int bad = 0;
  for (int pass = 0; pass < 2; pass++) {
    const uint32_t *words = pass ? unknown_words : known_words;
    int count = pass ? unknown_word_count : known_word_count;
    for (int i = 0; i < count; i++) {
      memset(&cpu, 0, sizeof cpu);
      memset(&effects, 0, sizeof effects);
      interpret_instruction(0x80001000u, words[i]);
      if ((effects.unknown_count != 0) != pass) {
        printf("DECODE %08x decode.py %s it, the interpreter %s\n", words[i], pass ? "doesn't know" : "knows", pass ? "does" : "doesn't");
        bad++;
      }
    }
  }
  return bad;
}

int main(int count, char **arguments) {
  int states = count > 1 ? atoi(arguments[1]) : 200, bad_cases = 0, bad_states = 0;
  mem1 = calloc(1, MEM1_SIZE);
  mem2 = calloc(1, MEM2_SIZE);
  windows[0] = mem1 + MEM1_WINDOW;
  windows[1] = mem2 + MEM2_WINDOW;
  windows[2] = mem1;
  for (int i = 0; i < case_count; i++) {
    int bad = run_case(i, states);
    if (bad) {
      printf("CASE %s %08x %d/%d\n", cases[i].name, cases[i].word, bad, states);
      bad_cases++;
      bad_states += bad;
    }
  }
  int bad_programs = 0;
  for (int i = 0; i < program_count; i++) {
    int bad = run_program(i, states / 4 + 1);
    if (bad) {
      printf("PROGRAM %s %d\n", programs[i].name, bad);
      bad_programs++;
    }
  }
  int bad_decoding = check_decoding();
  printf("SUMMARY forms=%d states=%d bad_forms=%d bad_states=%d programs=%d bad_programs=%d bad_decoding=%d nan_payload_only=%d\n",
         case_count, states, bad_cases, bad_states, program_count, bad_programs, bad_decoding, nan_only_differences);
  return 0;
}
