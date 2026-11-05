// Watching guest memory for writes (memory_watch.h). guest_memory.c's fault handler
// passes each fault in the window here first.
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "guest_memory.h"
#include "memory_watch.h"

// Watched pages (memory_watch.h). Only pages of MEM1 and MEM2 are watched, and those are
// always there, so a fault on one is always a write to a watched page. One lock covers
// making a page read-only and opening it, so a page is never left open while counted as
// watched. Nothing in either holds the lock while writing guest memory, so a thread that
// faults never waits on itself.
enum { WATCH_SPAN = MEM2_START + MEM2_SIZE, BUSY_STREAK = 4, BUSY_FRAMES = 600 };
static int watching;
static unsigned page_shift;
static _Atomic uint32_t *page_counts, *busy_until;
static _Atomic uint8_t *page_watched;
static uint32_t *last_written;
static uint8_t *streak;
static atomic_flag watch_lock = ATOMIC_FLAG_INIT;
static _Atomic uint32_t frame;

static void lock_watch(void) { while (atomic_flag_test_and_set_explicit(&watch_lock, memory_order_acquire)) {} }
static void unlock_watch(void) { atomic_flag_clear_explicit(&watch_lock, memory_order_release); }

static int is_memory(uint32_t physical) { return physical < MEM1_SIZE || (physical >= MEM2_START && physical < MEM2_START + MEM2_SIZE); }

// A range lies in one memory, so both ends do.
static int watchable(uint32_t address, uint32_t bytes, uint32_t *first, uint32_t *last) {
  if (!watching || !bytes) return 0;
  *first = address & 0x3fffffffu;
  if (bytes - 1 > WATCH_SPAN || *first > WATCH_SPAN - bytes) return 0;
  *last = *first + bytes - 1;
  return is_memory(*first) && is_memory(*last) && (*first < MEM1_SIZE) == (*last < MEM1_SIZE);
}

static void protect(uint32_t first_page, uint32_t pages, int prot) {
  mprotect(GUEST_BASE + ((uintptr_t)first_page << page_shift), (size_t)pages << page_shift, prot);
}

// Called with the lock held, for a page that is being or may be written.
static void open_page(uint32_t page) {
  atomic_store_explicit(&page_watched[page], 0, memory_order_relaxed);
  protect(page, 1, PROT_READ | PROT_WRITE);
  atomic_fetch_add_explicit(&page_counts[page], 1, memory_order_release);
  uint32_t now = atomic_load_explicit(&frame, memory_order_relaxed);
  if (last_written[page] == now) return;
  streak[page] = last_written[page] + 1 == now ? streak[page] + 1 : 1;
  last_written[page] = now;
  if (streak[page] >= BUSY_STREAK) {
    atomic_store_explicit(&busy_until[page], now + BUSY_FRAMES, memory_order_relaxed);
    streak[page] = 0;
  }
}

static uint64_t count_pages(uint32_t first, uint32_t last) {
  uint64_t sum = 0;
  for (uint32_t page = first >> page_shift; page <= last >> page_shift; page++) {
    if (!atomic_load_explicit(&page_watched[page], memory_order_acquire)) return WATCH_NONE;
    sum += atomic_load_explicit(&page_counts[page], memory_order_acquire);
  }
  return sum == WATCH_NONE ? sum - 1 : sum;
}

uint64_t watch_range(uint32_t address, uint32_t bytes) {
  uint32_t first, last;
  if (!watchable(address, bytes, &first, &last)) return WATCH_NONE;
  uint32_t now = atomic_load_explicit(&frame, memory_order_relaxed), first_page = first >> page_shift, last_page = last >> page_shift;
  for (uint32_t page = first_page; page <= last_page; page++) {
    if ((int32_t)(atomic_load_explicit(&busy_until[page], memory_order_relaxed) - now) > 0) return WATCH_NONE;
  }
  lock_watch();
  uint32_t run = 0, run_pages = 0;
  for (uint32_t page = first_page; page <= last_page + 1; page++) {
    if (page <= last_page && !atomic_load_explicit(&page_watched[page], memory_order_relaxed)) {
      if (!run_pages) run = page;
      run_pages++;
      atomic_store_explicit(&page_watched[page], 1, memory_order_relaxed);
      continue;
    }
    if (run_pages) protect(run, run_pages, PROT_READ);
    run_pages = 0;
  }
  unlock_watch();
  return count_pages(first, last);
}

uint64_t watch_count(uint32_t address, uint32_t bytes) {
  uint32_t first, last;
  return watchable(address, bytes, &first, &last) ? count_pages(first, last) : WATCH_NONE;
}

void watch_before_write(const void *host_address, size_t bytes) {
  uintptr_t at = (uintptr_t)host_address, base = (uintptr_t)GUEST_BASE;
  if (!watching || !bytes || at < base || at - base >= WATCH_SPAN) return;
  uint32_t first = (uint32_t)(at - base), last = bytes > WATCH_SPAN - first ? WATCH_SPAN - 1 : first + (uint32_t)bytes - 1;
  lock_watch();
  for (uint32_t page = first >> page_shift; page <= last >> page_shift; page++) {
    if (is_memory(page << page_shift) && atomic_load_explicit(&page_watched[page], memory_order_relaxed)) open_page(page);
  }
  unlock_watch();
}

void watch_next_frame(void) { atomic_fetch_add_explicit(&frame, 1, memory_order_relaxed); }
int watch_enabled(void) { return watching; }

void watch_start(unsigned long page_bytes) {
  free((void *)page_counts), free((void *)busy_until), free((void *)page_watched), free(last_written), free(streak);
  page_counts = 0;
  watching = 0;
  page_shift = (unsigned)__builtin_ctzl(page_bytes);
  if (getenv("WII_RECOMP_NO_WATCH")) return;
  size_t pages = WATCH_SPAN >> page_shift;
  page_counts = calloc(pages, sizeof *page_counts);
  busy_until = calloc(pages, sizeof *busy_until);
  page_watched = calloc(pages, sizeof *page_watched);
  last_written = calloc(pages, sizeof *last_written);
  streak = calloc(pages, sizeof *streak);
  watching = page_counts && busy_until && page_watched && last_written && streak;
}

static _Atomic unsigned faults;

int watch_fault(uint64_t offset) {
  if (!watching || offset >= WATCH_SPAN || !is_memory((uint32_t)offset)) return 0;
  lock_watch();
  open_page((uint32_t)(offset >> page_shift));
  unlock_watch();
  atomic_fetch_add_explicit(&faults, 1, memory_order_relaxed);
  return 1;
}

unsigned watch_faults(void) { return atomic_load_explicit(&faults, memory_order_relaxed); }
