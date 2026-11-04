// The guest's memory: one window at a fixed host address, indexed by the guest address
// with its top two bits cleared. That mask is what makes 0x80000000 and 0xc0000000 the
// same memory as 0x00000000, as on a Wii. A fixed address lets translated code reach
// memory with no pointer to load first.
//
//   0x00000000-0x017fffff  MEM1, 24 MiB
//   0x08000000-0x0fffffff  hardware: the picture memory, the registers and Hollywood.
//                          Never touched through the window: rd32 and friends send it
//                          to hw_read and hw_write.
//   0x10000000-0x13ffffff  MEM2, 64 MiB
//   0x20000000-0x20003fff  the processor's locked cache, 16 KiB (0xe0000000)
//
// The rest is guard pages. Every part starts on a 64 KiB boundary, so the layout holds
// with 4 KiB pages (the Mac) and 16 KiB pages (the Raspberry Pi 5) alike.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#if defined(__x86_64__)
#define GUEST_BASE_ADDRESS 0x100000000000ull  // 16 TiB, clear of where macOS and Linux place programs.
#elif defined(__aarch64__) && defined(__APPLE__)
#define GUEST_BASE_ADDRESS 0x8000000000ull    // 512 GiB, above Darwin's shared cache.
#elif defined(__aarch64__)
#define GUEST_BASE_ADDRESS 0x1000000000ull    // 64 GiB, reachable with the 39-bit address spaces some kernels use.
#else
#error "no guest memory address chosen for this processor"
#endif

#define GUEST_BASE ((uint8_t *)GUEST_BASE_ADDRESS)
#define GUEST_WINDOW (1ull << 30)
#define GUEST_GUARD (1ull << 16)  // Past the end, for an 8-byte access that starts in the last bytes.
#define MEM1_START 0x00000000u
#define MEM1_SIZE (24u << 20)
#define MEM2_START 0x10000000u
#define MEM2_SIZE (64u << 20)
#define LOCKED_CACHE_PHYSICAL 0x20000000u
#define LOCKED_CACHE_START 0xe0000000u
#define LOCKED_CACHE_SIZE 0x4000u

extern uint8_t *mem1;
extern uint8_t *mem2;

// Reserves the window and opens its three memories, all zero. Calling it again starts
// them over at zero, which tests use. Stops the program if the address is taken. Files
// that ask for strict POSIX (threads.c) don't see MAP_ANON on macOS, and don't need this.
#if defined(MAP_ANON) || defined(MAP_ANONYMOUS)
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
static inline void guest_memory_reserve(void) {
  int reserved = mem1 == GUEST_BASE;  // Shared by every file, unlike a static here would be.
  size_t size = GUEST_WINDOW + GUEST_GUARD;
  if (reserved) munmap(GUEST_BASE, size);
  int flags = MAP_PRIVATE | MAP_ANON;
#ifdef MAP_NORESERVE
  flags |= MAP_NORESERVE;
#endif
#ifdef MAP_FIXED_NOREPLACE
  flags |= reserved ? MAP_FIXED : MAP_FIXED_NOREPLACE;
#else
  if (reserved) flags |= MAP_FIXED;  // Otherwise the address is a hint, checked below.
#endif
  void *at = mmap(GUEST_BASE, size, PROT_NONE, flags, -1, 0);
  if (at != (void *)GUEST_BASE) {
    fprintf(stderr, "guest memory: can't reserve 1 GiB at %#llx, something else is there\n", GUEST_BASE_ADDRESS);
    exit(1);
  }
  const uint32_t parts[][2] = {{MEM1_START, MEM1_SIZE}, {MEM2_START, MEM2_SIZE}, {LOCKED_CACHE_PHYSICAL, LOCKED_CACHE_SIZE}};
  for (int i = 0; i < 3; i++) {
    if (mprotect(GUEST_BASE + parts[i][0], parts[i][1], PROT_READ | PROT_WRITE)) {
      perror("guest memory");
      exit(1);
    }
  }
  mem1 = GUEST_BASE + MEM1_START;
  mem2 = GUEST_BASE + MEM2_START;
}
#endif

// The runtime's part (guest_memory.c): the reservation, plus a handler that opens a guard
// page a game touches and names it in gaps.txt, so a stray access is reported, not fatal.
void guest_memory_init(void);
void guest_memory_report(void);  // Names the guard pages opened since the last call.
