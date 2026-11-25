// The runtime's side of the guest memory window (guest_memory.h): the reservation, and
// what happens when a game reads or writes where a Wii has no memory.
//
// A Wii would raise a DSI exception there, which retail games only use to stop. Before
// the window existed such an access fell through to the hardware registers' store and
// went on, so this keeps going too: the handler opens a zero page at that address and
// the access is retried. The address is named in gaps.txt as cpu.dsi_isi, so it's never
// silent. The handler only calls mprotect and stores to a fixed array, both safe in a
// signal handler. A fault anywhere outside the window is a real crash and goes on to
// whatever handler was there before.
//
// In GameCube mode, the 32 MB from 0x7e000000, which some GameCube games map through
// page tables of their own, is plain memory from the start, as Dolphin's fake VMEM is
// when it doesn't model the memory management unit.
#define VIRTUAL_MEMORY 0x3e000000u  // 0x7e000000, with the window's top two bits cleared.
#define VIRTUAL_MEMORY_SIZE (32u << 20)
#ifdef _WIN32
// Windows: the same, with a vectored exception handler in place of the signal handler.
// It runs first for every access violation in the program, on the thread that faulted.
#include <windows.h>
#undef interface
#include <stdatomic.h>
#include <stdio.h>

#include "gaps.h"
#include "guest_memory.h"
#include "memory_watch.h"

uint8_t *mem1;
uint8_t *mem2;

enum { OPENED_KEPT = 64 };
static uint32_t opened[OPENED_KEPT];
static atomic_uint opened_count;
static unsigned reported;
static int log_faults;

static void say_crash(EXCEPTION_POINTERS *fault) {
  void *pc = fault->ExceptionRecord->ExceptionAddress;
  char module[MAX_PATH] = "?";
  HMODULE base = 0;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, pc, &base)) {
    GetModuleFileNameA(base, module, sizeof module);
  }
  fprintf(stderr, "crash: a fault at %#llx, instruction %p\ncrash: in %s at offset %#llx\n",
          (unsigned long long)fault->ExceptionRecord->ExceptionInformation[1], pc, module,
          (unsigned long long)((uintptr_t)pc - (uintptr_t)base));
  fflush(stderr);
}

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *fault) {
  if (fault->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  uintptr_t at = (uintptr_t)fault->ExceptionRecord->ExceptionInformation[1], base = (uintptr_t)GUEST_BASE;
  if (at < base || at >= base + GUEST_WINDOW + GUEST_GUARD) {
    say_crash(fault);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (watch_fault((uint64_t)(at - base))) return EXCEPTION_CONTINUE_EXECUTION;
  uintptr_t page = at & ~(uintptr_t)4095;
  if (!VirtualAlloc((void *)page, 4096, MEM_COMMIT, PAGE_READWRITE)) {
    say_crash(fault);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (log_faults) fprintf(stderr, "guard page opened at 0x%08x\n", (uint32_t)(at - base));
  unsigned slot = atomic_fetch_add(&opened_count, 1);
  if (slot < OPENED_KEPT) opened[slot] = (uint32_t)(at - base);
  return EXCEPTION_CONTINUE_EXECUTION;
}

void guest_memory_init(void) {
  guest_memory_reserve();
  log_faults = getenv("WII_RECOMP_GUARD_LOG") != 0;
  watch_start(4096);
  AddVectoredExceptionHandler(1, on_fault);
}

void guest_memory_open_virtual(void) {
  if (!VirtualAlloc(GUEST_BASE + VIRTUAL_MEMORY, VIRTUAL_MEMORY_SIZE, MEM_COMMIT, PAGE_READWRITE)) {
    fprintf(stderr, "guest memory: can't open the virtual memory window (error %lu)\n", GetLastError());
  }
}

void guest_memory_report(void) {
  unsigned count = atomic_load(&opened_count);
  while (reported < count && reported < OPENED_KEPT) gap_hit("cpu.dsi_isi", opened[reported++]);
  if (count > OPENED_KEPT && reported == OPENED_KEPT) {
    gap_hit("limit.guard_pages_opened", count);
    reported++;
  }
}
#else
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include <signal.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#include "gaps.h"
#include "guest_memory.h"
#include "memory_watch.h"

uint8_t *mem1;
uint8_t *mem2;

enum { OPENED_KEPT = 64 };
static uint32_t opened[OPENED_KEPT];  // Guest addresses of opened pages, in the order they were opened.
static atomic_uint opened_count;
static unsigned reported;
static struct sigaction before_segv, before_bus;
static uintptr_t page_size;
static int log_faults;

static void write_hex(char *to, uint64_t value) {
  for (int i = 0; i < 16; i++) to[15 - i] = "0123456789abcdef"[(value >> (i * 4)) & 15];
}

// Says where a real crash happened, as a signal handler may, before passing it on.
static void say_crash(siginfo_t *info, void *context) {
  char line[] = "crash: a fault at 0x0000000000000000, instruction 0x0000000000000000, handler 0x0000000000000000\n";
  uint64_t pc = 0;
#if defined(__APPLE__) && defined(__x86_64__)
  pc = ((ucontext_t *)context)->uc_mcontext->__ss.__rip;
#elif defined(__APPLE__) && defined(__aarch64__)
  pc = ((ucontext_t *)context)->uc_mcontext->__ss.__pc;
#elif defined(__linux__) && defined(__x86_64__)
  pc = (uint64_t)((ucontext_t *)context)->uc_mcontext.gregs[REG_RIP];
#elif defined(__linux__) && defined(__aarch64__)
  pc = ((ucontext_t *)context)->uc_mcontext.pc;
#endif
  write_hex(line + 20, (uint64_t)(uintptr_t)info->si_addr);
  write_hex(line + 52, pc);
  write_hex(line + 80, (uint64_t)(uintptr_t)say_crash);  // Where this program is loaded, to look the instruction up.
  if (write(2, line, sizeof line - 1) < 0) {}
  // Which library and function. dladdr isn't meant for signal handlers, but the program
  // is stopping anyway, and this names the place.
  Dl_info where;
  if (dladdr((void *)(uintptr_t)pc, &where) && where.dli_fname) {
    char offset[17] = {0};
    write_hex(offset, pc - (uint64_t)(uintptr_t)where.dli_fbase);
    const char *parts[] = {"crash: in ", where.dli_fname, " at offset 0x", offset, ", ", where.dli_sname ? where.dli_sname : "?", "\n"};
    for (int i = 0; i < 7; i++) if (write(2, parts[i], strlen(parts[i])) < 0) {}
  }
}

static void pass_on(int signal, siginfo_t *info, void *context) {
  say_crash(info, context);
  struct sigaction *before = signal == SIGBUS ? &before_bus : &before_segv;
  if (before->sa_flags & SA_SIGINFO && before->sa_sigaction) {
    before->sa_sigaction(signal, info, context);
  } else if (before->sa_handler != SIG_IGN && before->sa_handler != SIG_DFL && before->sa_handler) {
    before->sa_handler(signal);
  } else {
    // The default action: let the access fault again with nothing in the way.
    sigaction(signal, before, 0);
  }
}

static void on_fault(int signal, siginfo_t *info, void *context) {
  uintptr_t at = (uintptr_t)info->si_addr, base = (uintptr_t)GUEST_BASE;
  if (at < base || at >= base + GUEST_WINDOW + GUEST_GUARD) {
    pass_on(signal, info, context);
    return;
  }
  if (watch_fault((uint64_t)(at - base))) return;
  uintptr_t page = at & ~(page_size - 1);
  if (mprotect((void *)page, page_size, PROT_READ | PROT_WRITE)) {
    pass_on(signal, info, context);
    return;
  }
  if (log_faults) {  // WII_RECOMP_GUARD_LOG: each page as it opens, written as a signal handler may.
    char line[40] = "guard page opened at 0x00000000\n";
    uint32_t guest = (uint32_t)(at - base);
    for (int i = 0; i < 8; i++) line[30 - i] = "0123456789abcdef"[(guest >> (i * 4)) & 15];
    if (write(2, line, 32) < 0) {}
  }
  unsigned slot = atomic_fetch_add(&opened_count, 1);
  if (slot < OPENED_KEPT) opened[slot] = (uint32_t)(at - base);
}

void guest_memory_init(void) {
  guest_memory_reserve();
  page_size = (uintptr_t)sysconf(_SC_PAGESIZE);
  log_faults = getenv("WII_RECOMP_GUARD_LOG") != 0;
  watch_start((unsigned long)page_size);
  struct sigaction action;
  memset(&action, 0, sizeof action);
  action.sa_sigaction = on_fault;
  action.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&action.sa_mask);
  sigaction(SIGSEGV, &action, &before_segv);
  sigaction(SIGBUS, &action, &before_bus);  // macOS reports a protected page as SIGBUS.
}

void guest_memory_open_virtual(void) {
  if (mprotect(GUEST_BASE + VIRTUAL_MEMORY, VIRTUAL_MEMORY_SIZE, PROT_READ | PROT_WRITE)) perror("guest memory: the virtual memory window");
}

void guest_memory_report(void) {
  unsigned count = atomic_load(&opened_count);
  while (reported < count && reported < OPENED_KEPT) gap_hit("cpu.dsi_isi", opened[reported++]);
  if (count > OPENED_KEPT && reported == OPENED_KEPT) {
    gap_hit("limit.guard_pages_opened", count);
    reported++;
  }
}
#endif
