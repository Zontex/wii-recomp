// Plays a sound recording (recomp/runtime/dsp_record.c) with no game running, twice:
//
// 1. Through the interpreter, from the state it had when recording began, feeding it the
//    game's mail, mailbox reads and control writes in order and the memory it read just
//    before each run. That times the real program, and checks the replay is faithful: it
//    must write and mail exactly what the recording says.
// 2. Through the console's C version for the recording's program (AX, Zelda), as the game
//    would drive it: the same mail, reads and control writes through dsp.c. Before each
//    mail the memory the real program read until the next mail is put in place. After it,
//    what the C version wrote and mailed is compared with what the real program did, and
//    memory is then set to the real program's, so each mail is judged on its own.
//
// recomp/tools/sound_bench.py builds this with the runtime's own dsp.c, dsp_lle*.c,
// dsp_accelerator.c, ax.c and zelda.c, and reads its last line.
//
// Usage: sound_replay <recording> <folder with dsp_rom.bin and dsp_coef.bin> [--hashes <file>] [--verbose]
//                     [--show <mails>]   (prints both sides' answers to the first mails)
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "cpu.h"
#include "dsp.h"
#include "dsp_lle.h"
#include "dsp_record.h"
#include "guest_memory.h"

Cpu cpu;
uint8_t *mem1, *mem2;
unsigned char data_dsp_rom[8192], data_dsp_coef[4096];
void fpscr_mode_changed(uint32_t fpscr) { (void)fpscr; }
void stage_add(int stage, uint64_t ticks) { (void)stage; (void)ticks; }
uint32_t hw_read(uint32_t address, int bytes) { (void)address; (void)bytes; return 0; }
void hw_write(uint32_t address, uint32_t value, int bytes) { (void)address; (void)value; (void)bytes; }

static uint64_t now;
uint64_t time_base(void) { return now; }

// Gaps are counted, by id, and listed at the end.
static int verbose;
static struct { const char *id; uint32_t detail, count; } gaps[64];
void gap_hit(const char *id, uint32_t detail) {
  for (int i = 0; i < 64; i++) {
    if (!gaps[i].id) gaps[i].id = id, gaps[i].detail = detail;
    else if (strcmp(gaps[i].id, id)) continue;
    gaps[i].count++;
    return;
  }
}
void gap_note(const char *id, uint32_t detail) { gap_hit(id, detail); }
void gap_flush(void) {}

static int interrupts;
void dsp_interrupt(void) { interrupts++; }

static double seconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

static void stop(const char *message) {
  printf("summary failed=%s\n", message);
  fprintf(stderr, "sound_replay: %s\n", message);
  exit(1);
}

// --- The recording, as a list of events. ---

// A read of a low half keeps the mailbox as it was in address.
typedef struct { uint8_t kind; uint64_t value; uint32_t address, count; const uint8_t *bytes; } Event;
static Event *events;
static size_t event_count;
static DspRecordHeader header;
static const DspLle *start_state;

static void load(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) stop("can't open the recording");
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  uint8_t *all = malloc((size_t)size);
  if (!all || fread(all, 1, (size_t)size, file) != (size_t)size) stop("can't read the recording");
  fclose(file);
  if ((size_t)size < sizeof header) stop("not a sound recording");
  memcpy(&header, all, sizeof header);
  if (header.magic != DSP_RECORD_MAGIC) stop("not a sound recording");
  if (header.version != DSP_RECORD_VERSION) stop("recorded by another version of the recorder");
  // A made-up recording, for tests, may leave the interpreter out: its state is then empty.
  if (header.state_bytes && header.state_bytes != sizeof(DspLle)) stop("recorded by a console whose interpreter state is laid out differently");
  if (!header.end_time) stop("the recording wasn't finished");
  size_t at = sizeof header;
  start_state = (const DspLle *)(all + at);  // malloc's alignment covers it.
  at += header.state_bytes;
  events = calloc(header.events + 1, sizeof *events);
  while (at < (size_t)size) {
    if (event_count == header.events) stop("more events than the header says");
    Event *e = &events[event_count++];
    e->kind = all[at++];
    switch (e->kind) {
    case DSP_EVENT_TIME: memcpy(&e->value, all + at, 8); at += 8; break;
    case DSP_EVENT_RUN: case DSP_EVENT_MAIL: { uint32_t v; memcpy(&v, all + at, 4); e->value = v; at += 4; break; }
    case DSP_EVENT_CPU_HIGH: case DSP_EVENT_CPU_LOW: case DSP_EVENT_CONTROL: {
      uint16_t v;
      memcpy(&v, all + at, 2);
      e->value = v;
      at += 2;
      break;
    }
    case DSP_EVENT_READ_LOW: memcpy(&e->address, all + at + 1, 4); e->value = all[at]; at += 5; break;
    case DSP_EVENT_MEMORY_READ: case DSP_EVENT_MEMORY_WRITE:
      memcpy(&e->address, all + at, 4);
      memcpy(&e->count, all + at + 4, 4);
      e->bytes = all + at + 8;
      at += 8 + e->count;
      break;
    case DSP_EVENT_INTERRUPT: break;
    default: stop("an event of a kind this player doesn't know");
    }
    if (at > (size_t)size) stop("the recording is cut short");
  }
}

static uint8_t *memory_at(uint32_t address, uint32_t count) {
  uint8_t *first = host(address), *last = host(address + count - 1);
  return first && last == first + count - 1 ? first : 0;
}

// Steps begin at each mail the game sends; step 0 is what came before the first.
static size_t step_count, *step_start;
static size_t frames;  // Steps in which the real program wrote memory.

static void find_steps(void) {
  step_start = malloc((header.mails + 2) * sizeof *step_start);
  step_start[step_count++] = 0;
  int wrote = 0;
  for (size_t i = 0; i < event_count; i++) {
    if (events[i].kind == DSP_EVENT_CPU_LOW) step_start[step_count++] = i, wrote = 0;
    if (events[i].kind == DSP_EVENT_MEMORY_WRITE && !wrote) wrote = 1, frames++;
  }
  step_start[step_count] = event_count;
}

static inline uint64_t fnv(uint64_t hash, uint64_t value) { return (hash ^ value) * 0x100000001b3ull; }
#define FNV_START 0xcbf29ce484222325ull

// --- 1. The interpreter. ---

static uint64_t replayed_hash;
static void replayed_write(uint32_t address, const uint8_t *bytes, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) replayed_hash = fnv(fnv(replayed_hash, address + i), bytes[i]);
}
static void replayed_mail(uint32_t mail) { replayed_hash = fnv(fnv(replayed_hash, 1ull << 40), mail); }
static void replayed_interrupt(void) { replayed_hash = fnv(replayed_hash, 1ull << 41); }

static double interpreter_seconds;
static long interpreter_differs = -1;  // The first step that didn't replay the same.

static void run_interpreter(void) {
  guest_memory_reserve();
  if (!dsp_lle_start(data_dsp_rom, data_dsp_coef)) stop("the ROM files aren't Dolphin's");
  dsp_lle_restore(start_state);
  dsp_lle_observe_memory(0, replayed_write);
  dsp_lle_observe_mail(replayed_mail, replayed_interrupt);
  now = header.start_time;
  for (size_t step = 0; step < step_count; step++) {
    uint64_t recorded = FNV_START;
    replayed_hash = FNV_START;
    for (size_t i = step_start[step]; i < step_start[step + 1]; i++) {
      Event *e = &events[i];
      switch (e->kind) {
      case DSP_EVENT_TIME: now = e->value; break;
      case DSP_EVENT_MEMORY_READ: {
        uint8_t *to = memory_at(e->address, e->count);
        if (to) memcpy(to, e->bytes, e->count);
        break;
      }
      case DSP_EVENT_MEMORY_WRITE:
        for (uint32_t k = 0; k < e->count; k++) recorded = fnv(fnv(recorded, e->address + k), e->bytes[k]);
        break;
      case DSP_EVENT_MAIL: recorded = fnv(fnv(recorded, 1ull << 40), (uint32_t)e->value); break;
      case DSP_EVENT_INTERRUPT: recorded = fnv(recorded, 1ull << 41); break;
      case DSP_EVENT_RUN: {
        double started = seconds();
        dsp_lle_run((int)e->value);
        interpreter_seconds += seconds() - started;
        break;
      }
      case DSP_EVENT_CPU_HIGH: dsp_lle_mailbox_write_high(1, (uint16_t)e->value); break;
      case DSP_EVENT_CPU_LOW: dsp_lle_mailbox_write_low(1, (uint16_t)e->value); break;
      case DSP_EVENT_READ_LOW: dsp_lle_mailbox_read_low((int)e->value); break;
      case DSP_EVENT_CONTROL: dsp_lle_control_write((uint16_t)e->value); break;
      }
    }
    if (replayed_hash != recorded && interpreter_differs < 0) interpreter_differs = (long)step;
  }
  dsp_lle_observe_memory(0, 0);
  dsp_lle_observe_mail(0, 0);
}

// --- 2. The C version, watched for what it writes: memory is made read-only, and the first
// write to each page saves a copy of it and opens it again. ---

#define MOST_PAGES 8192
typedef struct { uint32_t address; uint8_t *before, *expected, *written; } Page;
static Page pages[MOST_PAGES];
static volatile size_t page_count;
static volatile int too_many_pages;
static size_t page_size;
static uint8_t *page_room;

static int known_page(uint32_t address) {
  for (size_t i = 0; i < page_count; i++) {
    if (pages[i].address == address) return 1;
  }
  return 0;
}

static void keep_page(uint32_t address) {
  if (page_count == MOST_PAGES) {
    too_many_pages = 1;
    return;
  }
  Page *p = &pages[page_count];
  p->address = address;
  p->before = page_room + page_count * page_size * 3;
  p->expected = p->before + page_size;
  p->written = p->expected + page_size;
  memcpy(p->before, GUEST_BASE + address, page_size);
  memset(p->written, 0, page_size);
  page_count++;
}

static void on_fault(int signal_number, siginfo_t *info, void *context) {
  (void)context;
  uint8_t *at = info->si_addr;
  uint32_t offset = (uint32_t)(at - GUEST_BASE);
  int ours = at >= GUEST_BASE && (offset < MEM1_SIZE || offset - MEM2_START < MEM2_SIZE);
  if (!ours) {
    signal(signal_number, SIG_DFL);
    return;  // Faults again, without us.
  }
  uint32_t address = offset & ~(uint32_t)(page_size - 1);
  if (!known_page(address)) keep_page(address);
  mprotect(GUEST_BASE + address, page_size, PROT_READ | PROT_WRITE);
}

static void watch(int on) {
  int mode = on ? PROT_READ : PROT_READ | PROT_WRITE;
  mprotect(GUEST_BASE + MEM1_START, MEM1_SIZE, mode);
  mprotect(GUEST_BASE + MEM2_START, MEM2_SIZE, mode);
}

static int compare_pages(const void *a, const void *b) {
  uint32_t x = ((const Page *)a)->address, y = ((const Page *)b)->address;
  return x < y ? -1 : x > y;
}

// What the C version did against the real program, over the steps after warm-up.
static uint64_t words_compared, words_differ, steps_differ;
static double error_squares;
static int peak_error;
static long first_step = -1;
static uint32_t first_address, first_mail;
static int first_interpreter, first_c;
static uint64_t mail_steps_differ;
static long first_mail_step = -1;
static char first_mail_text[256];
static double c_seconds;
static long in_step_from = -1;
static int compared, interpreted;  // Which passes ran.
static int c_interrupts, interpreter_interrupts;

#define REGIONS 4096
static struct { uint32_t address, count; long first_step; int peak; } regions[REGIONS];

static void count_region(uint32_t address, long step, int error) {
  uint32_t key = address & ~0x3fu, slot = (key >> 6) * 2654435761u % REGIONS;
  for (uint32_t probe = 0; probe < REGIONS; probe++, slot = (slot + 1) % REGIONS) {
    if (!regions[slot].count) {
      regions[slot].address = key;
      regions[slot].first_step = step;
    } else if (regions[slot].address != key) {
      continue;
    }
    regions[slot].count++;
    if (error > regions[slot].peak) regions[slot].peak = error;
    return;
  }
}

// The mails the game took in a step from each side: those the mailbox held, full, as it read.
static uint32_t c_mails[64], c_mail_count;

// The mail that begins a step: its low half, and the high half written before it.
static uint32_t step_mail(size_t step) {
  if (!step) return 0;
  size_t low = step_start[step];
  uint32_t high = 0;
  for (size_t i = low; i-- > 0;) {
    if (events[i].kind == DSP_EVENT_CPU_HIGH) { high = (uint32_t)events[i].value; break; }
    if (events[i].kind == DSP_EVENT_CPU_LOW) break;
  }
  return high << 16 | (uint32_t)events[low].value;
}

static FILE *hashes;
static size_t show_steps;  // --show: the first steps' mails, both sides.

static void judge_step(size_t step, const uint32_t *mails, uint32_t mail_count) {
  int same_mails = mail_count == c_mail_count;
  for (uint32_t i = 0; same_mails && i < mail_count && i < 64; i++) same_mails = mails[i] == c_mails[i];
  if (in_step_from < 0 && same_mails && mail_count) in_step_from = (long)step;
  int judged = in_step_from >= 0 && (long)step >= in_step_from;
  if (step < show_steps) {
    fprintf(stderr, "step %zu mail %08x: real", step, step_mail(step));
    for (uint32_t i = 0; i < mail_count && i < 64; i++) fprintf(stderr, " %08x", mails[i]);
    fprintf(stderr, ", c");
    for (uint32_t i = 0; i < c_mail_count && i < 64; i++) fprintf(stderr, " %08x", c_mails[i]);
    fprintf(stderr, "\n");
  }
  if (judged && !same_mails) {
    mail_steps_differ++;
    if (first_mail_step < 0) {
      first_mail_step = (long)step;
      int at = snprintf(first_mail_text, sizeof first_mail_text, "interpreter");
      for (uint32_t i = 0; i < mail_count && i < 4; i++) at += snprintf(first_mail_text + at, sizeof first_mail_text - at, " %08x", mails[i]);
      at += snprintf(first_mail_text + at, sizeof first_mail_text - at, ", c version");
      for (uint32_t i = 0; i < c_mail_count && i < 4; i++) at += snprintf(first_mail_text + at, sizeof first_mail_text - at, " %08x", c_mails[i]);
    }
  }
  qsort(pages, page_count, sizeof pages[0], compare_pages);
  uint64_t hash = FNV_START;
  int differs = 0;
  for (size_t i = 0; i < page_count; i++) {
    Page *p = &pages[i];
    const uint8_t *now_bytes = GUEST_BASE + p->address;
    for (size_t w = 0; w < page_size; w += 2) {
      int c_wrote = now_bytes[w] != p->before[w] || now_bytes[w + 1] != p->before[w + 1];
      int real_wrote = p->written[w] || p->written[w + 1];
      if (c_wrote) hash = fnv(fnv(hash, p->address + w), (uint32_t)now_bytes[w] << 8 | now_bytes[w + 1]);
      if (!judged || !(c_wrote || real_wrote)) continue;
      words_compared++;
      int real = (int16_t)(p->expected[w] << 8 | p->expected[w + 1]), c = (int16_t)(now_bytes[w] << 8 | now_bytes[w + 1]);
      if (real == c) continue;
      int error = abs(c - real);
      words_differ++;
      differs = 1;
      error_squares += (double)error * error;
      if (error > peak_error) peak_error = error;
      count_region(p->address + (uint32_t)w, (long)step, error);
      if (verbose && words_differ <= 40) {
        fprintf(stderr, "  mail %zu (%08x): %08x real %d, C %d\n", step, step_mail(step), p->address + (uint32_t)w, real, c);
      }
      if (first_step < 0) {
        first_step = (long)step;
        first_address = p->address + (uint32_t)w;
        first_mail = step_mail(step);
        first_interpreter = real;
        first_c = c;
      }
    }
    memcpy(GUEST_BASE + p->address, p->expected, page_size);  // From here on, the real program's memory.
  }
  steps_differ += differs;
  if (hashes) fprintf(hashes, "%zu %016llx\n", step, (unsigned long long)hash);
}

static void run_c_version(const char *hash_path) {
  guest_memory_reserve();
  unsetenv("WII_RECOMP_DSP");
  page_size = (size_t)getpagesize();
  page_room = mmap(0, MOST_PAGES * page_size * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  struct sigaction action = {0};
  action.sa_sigaction = on_fault;
  action.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &action, 0);
  sigaction(SIGBUS, &action, 0);
  if (hash_path && !(hashes = fopen(hash_path, "w"))) stop("can't write the hashes file");
  now = header.start_time;
  dsp_start();
  if (!(header.flags & DSP_RECORD_FROM_BOOT)) {
    dsp_control_write(header.reserved[0] & 0xffffu);
    dsp_set_program(header.start_crc);
    // Its first mail went to the game long before the recording began.
    for (uint32_t mail; dsp_mail_pending();) dsp_read(0xcc005006u, 2, &mail);
    interrupts = 0;
    in_step_from = -1;
  } else {
    in_step_from = 0;  // From power-on both start alike.
  }
  if (!(header.flags & DSP_RECORD_FROM_BOOT) && !dsp_c_version()) {
    fprintf(stderr, "sound_replay: program %08x has no C version to compare\n", header.start_crc);
    return;
  }
  compared = 1;
  uint32_t mails[64];
  for (size_t step = 0; step < step_count; step++) {
    size_t from = step_start[step], to = step_start[step + 1];
    uint32_t mail_count = 0;
    for (size_t i = from; i < to; i++) {
      Event *e = &events[i];
      if (e->kind == DSP_EVENT_MEMORY_READ) {
        uint8_t *at = memory_at(e->address, e->count);
        if (at) memcpy(at, e->bytes, e->count);
      } else if (e->kind == DSP_EVENT_READ_LOW && !e->value && (e->address & 0x80000000u)) {
        if (mail_count < 64) mails[mail_count] = e->address;
        mail_count++;
      } else if (e->kind == DSP_EVENT_INTERRUPT) {
        interpreter_interrupts++;
      }
    }
    page_count = 0;
    c_mail_count = 0;
    int interrupts_before = interrupts;
    watch(1);
    double started = seconds();
    for (size_t i = from; i < to; i++) {
      Event *e = &events[i];
      switch (e->kind) {
      case DSP_EVENT_TIME: now = e->value; dsp_update(now); break;
      case DSP_EVENT_CPU_HIGH: dsp_write(0xcc005000u, (uint32_t)e->value, 2); break;
      case DSP_EVENT_CPU_LOW: dsp_write(0xcc005002u, (uint32_t)e->value, 2); break;
      case DSP_EVENT_READ_LOW: {
        uint32_t mail;
        if (e->value) {
          dsp_read(0xcc005002u, 2, &mail);
        } else {
          dsp_read(0xcc005004u, 4, &mail);  // The high half, with the full bit, then the low.
          if (mail & 0x80000000u) {
            if (c_mail_count < 64) c_mails[c_mail_count] = mail;
            c_mail_count++;
          }
        }
        break;
      }
      case DSP_EVENT_CONTROL: dsp_control_write((uint32_t)e->value); break;
      }
    }
    c_seconds += seconds() - started;
    watch(0);
    c_interrupts += interrupts - interrupts_before;
    if (too_many_pages) stop("the C version wrote more pages in one step than the player keeps");
    // The pages the real program wrote, as it left them.
    for (size_t i = from; i < to; i++) {
      Event *e = &events[i];
      if (e->kind != DSP_EVENT_MEMORY_WRITE) continue;
      for (uint32_t k = 0; k < e->count; k++) {
        uint32_t address = e->address + k, base = address & ~(uint32_t)(page_size - 1);
        if (address >= MEM1_SIZE && address - MEM2_START >= MEM2_SIZE) continue;
        if (!known_page(base)) {
          keep_page(base);
          if (too_many_pages) stop("the real program wrote more pages in one step than the player keeps");
        }
      }
    }
    for (size_t i = 0; i < page_count; i++) memcpy(pages[i].expected, pages[i].before, page_size);
    for (size_t i = from; i < to; i++) {
      Event *e = &events[i];
      if (e->kind != DSP_EVENT_MEMORY_WRITE) continue;
      for (uint32_t k = 0; k < e->count; k++) {
        uint32_t address = e->address + k, base = address & ~(uint32_t)(page_size - 1);
        for (size_t p = 0; p < page_count; p++) {
          if (pages[p].address != base) continue;
          pages[p].expected[address - base] = e->bytes[k];
          pages[p].written[address - base] = 1;
          break;
        }
      }
    }
    judge_step(step, mails, mail_count);
  }
  if (hashes) fclose(hashes);
}

static int by_count(const void *a, const void *b) {
  const typeof(regions[0]) *x = a, *y = b;
  return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

static void load_rom(const char *folder, const char *name, unsigned char *into, size_t bytes) {
  char path[4096];
  snprintf(path, sizeof path, "%s/%s", folder, name);
  FILE *file = fopen(path, "rb");
  if (!file || fread(into, 1, bytes, file) != bytes) stop("can't read the ROM files");
  fclose(file);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: sound_replay <recording> <rom folder> [--hashes <file>] [--verbose]\n");
    return 2;
  }
  const char *hash_path = 0;
  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "--hashes") && i + 1 < argc) hash_path = argv[++i];
    else if (!strcmp(argv[i], "--verbose")) verbose = 1;
    else if (!strcmp(argv[i], "--show") && i + 1 < argc) show_steps = (size_t)atol(argv[++i]);
  }
  load_rom(argv[2], "dsp_rom.bin", data_dsp_rom, sizeof data_dsp_rom);
  load_rom(argv[2], "dsp_coef.bin", data_dsp_coef, sizeof data_dsp_coef);
  load(argv[1]);
  find_steps();
  if (header.state_bytes) run_interpreter(), interpreted = 1;
  run_c_version(hash_path);
  if (compared && in_step_from < 0) {  // Never answered as the real program did.
    first_mail_step = 0;
    snprintf(first_mail_text, sizeof first_mail_text, "never alike");
  }
  char program[64] = "";  // One word, for the summary line.
  if (dsp_c_version()) snprintf(program, sizeof program, "%s", dsp_c_version());
  for (char *c = program; *c; c++) *c = *c == ' ' ? '-' : *c;
  double recorded = (double)(header.end_time - header.start_time) / 60750000.0;
  size_t per = frames ? frames : 1;
  double rms = words_compared ? __builtin_sqrt(error_squares / (double)words_compared) : 0;
  if (verbose) {
    fprintf(stderr, "program %08x, %s; %zu mails, %zu frames, %.2f s of sound\n", header.end_crc, *program ? program : "no C version",
            step_count - 1, frames, recorded);
    fprintf(stderr, "interpreter %.3f ms a frame, %s\n", interpreter_seconds * 1e3 / per,
            interpreter_differs < 0 ? "replays as recorded" : "doesn't replay as recorded");
    fprintf(stderr, "C version %.3f ms a frame, in step from mail %ld\n", c_seconds * 1e3 / per, in_step_from);
    fprintf(stderr, "interrupts: interpreter %d, C version %d\n", interpreter_interrupts, c_interrupts);
    if (first_mail_step >= 0) fprintf(stderr, "mails first differ at mail %ld: %s\n", first_mail_step, first_mail_text);
    for (int i = 0; i < 64 && gaps[i].id; i++) fprintf(stderr, "gap: %s %x, %u times\n", gaps[i].id, gaps[i].detail, gaps[i].count);
    if (interpreter_differs >= 0) fprintf(stderr, "the interpreter first replays differently at mail %ld\n", interpreter_differs);
    qsort(regions, REGIONS, sizeof regions[0], by_count);
    for (int i = 0; i < 12 && regions[i].count; i++) {
      fprintf(stderr, "  %08x: %u words differ, first at mail %ld, peak %d\n", regions[i].address, regions[i].count,
              regions[i].first_step, regions[i].peak);
    }
  }
  printf("summary program=%s crc=%08x mails=%zu frames=%zu seconds=%.3f interpreter_ms=%.4f c_ms=%.4f replays=%s "
         "in_step_from=%ld words=%llu differ=%llu steps_differ=%llu peak=%d rms=%.3f mail_steps_differ=%llu "
         "interrupts=%d/%d first_step=%ld first_address=%08x first_mail=%08x first_values=%d/%d first_mails=%s\n",
         compared && *program ? program : "none", header.end_crc, step_count - 1, frames, recorded, interpreter_seconds * 1e3 / per,
         c_seconds * 1e3 / per, !interpreted ? "skipped" : interpreter_differs < 0 ? "yes" : "no", in_step_from, (unsigned long long)words_compared,
         (unsigned long long)words_differ, (unsigned long long)steps_differ, peak_error, rms,
         (unsigned long long)mail_steps_differ, interpreter_interrupts, c_interrupts, first_step, first_address, first_mail,
         first_interpreter, first_c, first_mail_step < 0 ? "same" : "differ");
  return 0;
}
