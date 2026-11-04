// Guest threads. Translated code runs on the host stack, so each guest context
// gets a host stack of its own. Saving and loading a context switches stacks.
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>

#include "cpu.h"

// OSContext layout in guest memory.
enum {
  CONTEXT_CR = 0x80, CONTEXT_LR = 0x84, CONTEXT_CTR = 0x88, CONTEXT_XER = 0x8c, CONTEXT_FPR = 0x90,
  CONTEXT_SRR0 = 0x198, CONTEXT_SRR1 = 0x19c, CONTEXT_STATE = 0x1a2, CONTEXT_GQR = 0x1a4, CONTEXT_PS = 0x1c8,
};
// Tells the scheduler not to save over a context an exception is using.
#define STATE_IN_EXCEPTION 2u

#define MAX_CONTEXTS 256
#define HOST_STACK_BYTES (32u * 1024u * 1024u)
#define GQR_BASE 912
#define INTERRUPTS_ENABLED 0x8000u

// save_context returns before anything resumes it, so a ucontext taken inside it would
// come back to a frame already overwritten: its saved registers and return address are
// on the host stack, and later calls reuse that space. So a stub records the caller's
// own registers instead, and load_context returns straight to the caller with them. On
// the Intel Mac the old way only worked while no later call happened to overwrite those
// slots; the flat guest memory changed the code's frames, and rbx came back as a guest
// value.
#if defined(__aarch64__)
#define RECORDS_CALLER 1
typedef struct {
  uint64_t x19_to_x30[12], stack, d8_to_d15[8], padding;
} CallerRegisters;
#elif defined(__x86_64__)
#define RECORDS_CALLER 1
typedef struct {
  uint64_t rbx, rbp, r12, r13, r14, r15, return_address, stack;
} CallerRegisters;
#endif

typedef struct {
  uint32_t guest;
  uint32_t resume_pc, resume_stack;
  volatile int resumed;
  ucontext_t host;
#ifdef RECORDS_CALLER
  CallerRegisters caller;
  int use_caller;
#endif
} Slot;

static Slot slots[MAX_CONTEXTS];

// The last few switches, printed when something goes wrong.
#define HISTORY 24
typedef struct { const char *what; uint32_t context, pc, stack, link; } Event;
static Event history[HISTORY];
static unsigned history_count;

static void record(const char *what, uint32_t context, uint32_t pc) {
  history[history_count++ % HISTORY] = (Event){what, context, pc, cpu.r[1], cpu.lr};
}

static void print_history(void) {
  unsigned first = history_count > HISTORY ? history_count - HISTORY : 0;
  for (unsigned i = first; i < history_count; i++) {
    Event *e = &history[i % HISTORY];
    fprintf(stderr, "  %-9s context %08x pc %08x stack %08x link %08x\n", e->what, e->context, e->pc, e->stack, e->link);
  }
}

static Slot *slot_for(uint32_t guest) {
  for (int i = 0; i < MAX_CONTEXTS; i++) {
    if (slots[i].guest == guest) return &slots[i];
    if (!slots[i].guest) {
      slots[i].guest = guest;
      return &slots[i];
    }
  }
  fprintf(stderr, "stop: too many thread contexts\n");
  exit(5);
}

#ifdef RECORDS_CALLER
#define NAME_TEXT(text) #text
#define NAME_WITH(prefix, name) NAME_TEXT(prefix) #name
#define SYMBOL(prefix, name) NAME_WITH(prefix, name)

// Puts the registers back and returns from the save_context that recorded them.
#if defined(__x86_64__)
__attribute__((naked, noreturn)) static void return_to_caller(const CallerRegisters *caller) {
  __asm__(
      "movq 0(%rdi), %rbx\n"
      "movq 8(%rdi), %rbp\n"
      "movq 16(%rdi), %r12\n"
      "movq 24(%rdi), %r13\n"
      "movq 32(%rdi), %r14\n"
      "movq 40(%rdi), %r15\n"
      "movq 48(%rdi), %rax\n"
      "movq 56(%rdi), %rsp\n"
      "jmpq *%rax\n");
}
#else
__attribute__((naked, noreturn)) static void return_to_caller(const CallerRegisters *caller) {
  __asm__(
      "ldp x19, x20, [x0, #0]\n"
      "ldp x21, x22, [x0, #16]\n"
      "ldp x23, x24, [x0, #32]\n"
      "ldp x25, x26, [x0, #48]\n"
      "ldp x27, x28, [x0, #64]\n"
      "ldp x29, x30, [x0, #80]\n"
      "ldr x9, [x0, #96]\n"
      "mov sp, x9\n"
      "ldp d8, d9, [x0, #104]\n"
      "ldp d10, d11, [x0, #120]\n"
      "ldp d12, d13, [x0, #136]\n"
      "ldp d14, d15, [x0, #152]\n"
      "ret\n");
}
#endif
#endif

static uint32_t condition_bits(void) {
  uint32_t value = 0;
  for (int i = 0; i < 8; i++) value = (value << 4) | cpu.cr[i];
  return value;
}

static void write_registers(uint32_t c, uint32_t pc) {
  for (int i = 0; i < 32; i++) {
    wr32(c + i * 4, cpu.r[i]);
    wr64(c + CONTEXT_FPR + i * 8, double_bits(cpu.f[i]));
    wr64(c + CONTEXT_PS + i * 8, double_bits(cpu.p[i]));
  }
  wr32(c + CONTEXT_CR, condition_bits());
  wr32(c + CONTEXT_LR, cpu.lr);
  wr32(c + CONTEXT_CTR, cpu.ctr);
  wr32(c + CONTEXT_XER, xer_get());
  wr32(c + CONTEXT_SRR0, pc);
  wr32(c + CONTEXT_SRR1, cpu.msr);
  for (int i = 0; i < 8; i++) wr32(c + CONTEXT_GQR + i * 4, cpu.spr[GQR_BASE + i]);
}

static void read_registers(uint32_t c) {
  for (int i = 0; i < 32; i++) {
    cpu.r[i] = rd32(c + i * 4);
    cpu.f[i] = as_double(rd64(c + CONTEXT_FPR + i * 8));
    cpu.p[i] = as_double(rd64(c + CONTEXT_PS + i * 8));
  }
  uint32_t cr = rd32(c + CONTEXT_CR);
  for (int i = 0; i < 8; i++) cpu.cr[i] = (cr >> (28 - i * 4)) & 15;
  cpu.lr = rd32(c + CONTEXT_LR);
  cpu.ctr = rd32(c + CONTEXT_CTR);
  xer_set(rd32(c + CONTEXT_XER));
  cpu.msr = rd32(c + CONTEXT_SRR1);
  for (int i = 0; i < 8; i++) cpu.spr[GQR_BASE + i] = rd32(c + CONTEXT_GQR + i * 4);
}

// A context that has never run starts its code on a new stack. If that code
// returns, it continues at the link register, which holds the thread's exit.
static uint32_t start_pc;

static void thread_start(void) {
  if (getenv("TRACE_THREADS")) fprintf(stderr, "thread starts at %08x, stack %08x, link %08x\n", start_pc, cpu.r[1], cpu.lr);
  dispatch(start_pc);
  if (getenv("TRACE_THREADS")) fprintf(stderr, "thread from %08x returned, link %08x\n", start_pc, cpu.lr);
  while (1) dispatch(cpu.lr);
}

static void start_fresh(Slot *slot, uint32_t pc) {
  static ucontext_t fresh;
  getcontext(&fresh);
  fresh.uc_stack.ss_sp = malloc(HOST_STACK_BYTES);
  fresh.uc_stack.ss_size = HOST_STACK_BYTES;
  fresh.uc_link = 0;
  start_pc = pc;
  makecontext(&fresh, thread_start, 0);
  slot->resume_pc = 0;
  setcontext(&fresh);
}

void load_context(void) {
  uint32_t c = cpu.r[3];
  uint32_t pc = rd32(c + CONTEXT_SRR0);
  if (!pc) {
    fprintf(stderr, "stop: context %08x has no start address (called from %08x)\n", c, cpu.lr);
    print_history();
    exit(5);
  }
  record("load", c, pc);
  read_registers(c);
  wr16(c + CONTEXT_STATE, rd16(c + CONTEXT_STATE) & ~STATE_IN_EXCEPTION);
  Slot *slot = slot_for(c);
  if (slot->resume_pc == pc && slot->resume_stack == cpu.r[1]) {
    slot->resumed = 1;
#ifdef RECORDS_CALLER
    if (slot->use_caller) return_to_caller(&slot->caller);
#endif
    setcontext(&slot->host);
  }
  start_fresh(slot, pc);
}

#ifdef RECORDS_CALLER
void save_context_recorded(const CallerRegisters *caller);

// Returns 0 the first time, and 1 when something loads this context again.
#if defined(__x86_64__)
// On entry the return address is at the stack pointer, and the caller's stack is 8 above.
// 72 bytes keep the stack 16-aligned for the call.
__attribute__((naked)) void save_context(void) {
  __asm__(
      "subq $72, %rsp\n"
      "movq %rbx, 0(%rsp)\n"
      "movq %rbp, 8(%rsp)\n"
      "movq %r12, 16(%rsp)\n"
      "movq %r13, 24(%rsp)\n"
      "movq %r14, 32(%rsp)\n"
      "movq %r15, 40(%rsp)\n"
      "movq 72(%rsp), %rax\n"
      "movq %rax, 48(%rsp)\n"
      "leaq 80(%rsp), %rax\n"
      "movq %rax, 56(%rsp)\n"
      "movq %rsp, %rdi\n"
      "call " SYMBOL(__USER_LABEL_PREFIX__, save_context_recorded) "\n"
      "addq $72, %rsp\n"
      "ret\n");
}
#else
__attribute__((naked)) void save_context(void) {
  __asm__(
      "sub sp, sp, #176\n"
      "stp x19, x20, [sp, #0]\n"
      "stp x21, x22, [sp, #16]\n"
      "stp x23, x24, [sp, #32]\n"
      "stp x25, x26, [sp, #48]\n"
      "stp x27, x28, [sp, #64]\n"
      "stp x29, x30, [sp, #80]\n"
      "add x9, sp, #176\n"
      "str x9, [sp, #96]\n"
      "stp d8, d9, [sp, #104]\n"
      "stp d10, d11, [sp, #120]\n"
      "stp d12, d13, [sp, #136]\n"
      "stp d14, d15, [sp, #152]\n"
      "mov x0, sp\n"
      "bl " SYMBOL(__USER_LABEL_PREFIX__, save_context_recorded) "\n"
      "ldp x29, x30, [sp, #80]\n"
      "add sp, sp, #176\n"
      "ret\n");
}
#endif

void save_context_recorded(const CallerRegisters *caller) {
  uint32_t c = cpu.r[3], return_pc = cpu.lr;
  uint32_t saved_three = cpu.r[3];
  cpu.r[3] = 1;
  write_registers(c, return_pc);
  cpu.r[3] = saved_three;
  record("save", c, return_pc);
  Slot *slot = slot_for(c);
  slot->resume_pc = return_pc;
  slot->resume_stack = cpu.r[1];
  slot->resumed = 0;
  slot->caller = *caller;
  slot->use_caller = 1;
  cpu.r[3] = 0;
}
#else
// Returns 0 the first time, and 1 when something loads this context again.
void save_context(void) {
  uint32_t c = cpu.r[3], return_pc = cpu.lr;
  uint32_t saved_three = cpu.r[3];
  cpu.r[3] = 1;
  write_registers(c, return_pc);
  cpu.r[3] = saved_three;
  record("save", c, return_pc);
  Slot *slot = slot_for(c);
  slot->resume_pc = return_pc;
  slot->resume_stack = cpu.r[1];
  slot->resumed = 0;
  getcontext(&slot->host);
  if (slot->resumed) return;
  cpu.r[3] = 0;
}
#endif

// An exception saves where the code was, runs the game's handler with
// interrupts off, and comes back here when that handler loads the context.
void enter_exception(uint32_t kind, uint32_t handler) {
  uint32_t c = rd32(0x800000d4);
  static uint32_t marker = 0xe0000000u;
  uint32_t pc = marker += 4;
  record(kind == 8 ? "timer" : "external", c, pc);
  write_registers(c, pc);
  wr16(c + CONTEXT_STATE, rd16(c + CONTEXT_STATE) | STATE_IN_EXCEPTION);
  Slot *slot = slot_for(c);
  slot->resume_pc = pc;
  slot->resume_stack = cpu.r[1];
  slot->resumed = 0;
#ifdef RECORDS_CALLER
  slot->use_caller = 0;
#endif
  getcontext(&slot->host);
  if (slot->resumed) return;
  cpu.r[3] = kind;
  cpu.r[4] = c;
  cpu.msr &= ~INTERRUPTS_ENABLED;
  dispatch(handler);
  fprintf(stderr, "stop: exception handler %08x returned\n", handler);
  exit(6);
}
