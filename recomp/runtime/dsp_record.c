// Records what passes between the game and the audio processor while the interpreter runs
// the game's real program: each mail the game sends and when, each read of the mailboxes
// and write of the control register, every piece of main memory the program reads and
// writes, and each mail and interrupt it sends back. recomp/tools/sound_bench.py then plays
// the recording through a C version of the program, with no game running, and compares
// what that writes and mails with what the real program did.
//
// Ask with the control pipe's "record-sound <frames> <file>", or
// WII_RECOMP_SOUND_RECORD=<frame>:<frames>:<file> (frame 0 records from power-on, through
// the boot ROM). Frames are sixtieths of a second of the game's time. Recordings hold the
// game's own data, so keep them out of the repository, as scene recordings are.
#include "dsp_record.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "dsp.h"
#include "dsp_lle.h"

#define TICKS_PER_FRAME (60750000ull / 60)
#define PENDING_MOST 4096u  // A run of bytes is written out once it's this long.

int dsp_recording;
static _Atomic int requested;  // Frames asked for over the control pipe.
static char requested_path[4096];
static uint64_t env_start, env_frames;
static char env_path[4096];
static FILE *file;
static char path[4096];
static DspRecordHeader header;
static uint64_t stop_at, last_time;
static uint8_t *shadow[2];  // MEM1 and MEM2 as the recording has given them so far.
static struct { int kind; uint32_t address, count; uint8_t bytes[PENDING_MOST]; } pending;

void dsp_record_request(int frames, const char *to) {
  if (frames <= 0 || !to || !*to) return;
  snprintf(requested_path, sizeof requested_path, "%s", to);
  atomic_store(&requested, frames);
}

static void put(const void *bytes, size_t count) { fwrite(bytes, 1, count, file); }

static void event(int kind) {
  uint8_t byte = (uint8_t)kind;
  put(&byte, 1);
  header.events++;
}

static void flush_pending(void) {
  if (!pending.count) return;
  event(pending.kind);
  put(&pending.address, 4);
  put(&pending.count, 4);
  put(pending.bytes, pending.count);
  pending.count = 0;
}

static void note_time(void) {
  uint64_t now = time_base();
  if (now == last_time) return;
  flush_pending();
  last_time = now;
  event(DSP_EVENT_TIME);
  put(&now, 8);
}

static uint8_t *shadow_of(uint32_t address) {
  if (address < MEM1_SIZE) return shadow[0] + address;
  if (address - MEM2_START < MEM2_SIZE) return shadow[1] + (address - MEM2_START);
  return 0;
}

static void add_byte(int kind, uint32_t address, uint8_t value) {
  if (pending.count && (pending.kind != kind || pending.address + pending.count != address || pending.count == PENDING_MOST)) {
    flush_pending();
  }
  if (!pending.count) {
    pending.kind = kind;
    pending.address = address;
  }
  pending.bytes[pending.count++] = value;
}

// Only what the recording hasn't already given for that address: the replay keeps memory.
static void on_read(uint32_t address, const uint8_t *bytes, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    uint8_t *known = shadow_of(address + i);
    if (known && *known == bytes[i]) {
      if (pending.kind == DSP_EVENT_MEMORY_READ) flush_pending();
      continue;
    }
    if (known) *known = bytes[i];
    add_byte(DSP_EVENT_MEMORY_READ, address + i, bytes[i]);
  }
}

static void on_write(uint32_t address, const uint8_t *bytes, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    uint8_t *known = shadow_of(address + i);
    if (known) *known = bytes[i];
    add_byte(DSP_EVENT_MEMORY_WRITE, address + i, bytes[i]);
  }
}

static void on_mail(uint32_t mail) {
  flush_pending();
  event(DSP_EVENT_MAIL);
  put(&mail, 4);
}

static void on_interrupt(void) {
  flush_pending();
  event(DSP_EVENT_INTERRUPT);
}

void dsp_record_run(uint32_t cycles) {
  flush_pending();
  note_time();
  event(DSP_EVENT_RUN);
  put(&cycles, 4);
}

static void finish(void);
static int stopping;  // Time's up: the recording ends before the game's next mail.

void dsp_record_cpu_write(int high, uint16_t value) {
  if (!dsp_recording) return;  // Finished by the high half's write.
  if (stopping) {
    finish();
    return;
  }
  flush_pending();
  note_time();
  event(high ? DSP_EVENT_CPU_HIGH : DSP_EVENT_CPU_LOW);
  put(&value, 2);
  if (!high) header.mails++;
}

void dsp_record_read_low(int cpu, uint32_t mail) {
  flush_pending();
  note_time();
  event(DSP_EVENT_READ_LOW);
  uint8_t box = (uint8_t)cpu;
  put(&box, 1);
  put(&mail, 4);
}

void dsp_record_control(uint16_t value) {
  flush_pending();
  note_time();
  event(DSP_EVENT_CONTROL);
  put(&value, 2);
}

static void finish_at_exit(void) {
  if (dsp_recording) finish();
}

static void begin(const char *to, uint64_t frames, int from_boot) {
  if (!dsp_interpreting()) {
    fprintf(stderr, "record-sound: the interpreter isn't running the game's program; run with WII_RECOMP_DSP=lle\n");
    return;
  }
  file = fopen(to, "wb");
  if (!file) {
    perror("record-sound");
    return;
  }
  snprintf(path, sizeof path, "%s", to);
  if (!shadow[0]) shadow[0] = calloc(MEM1_SIZE, 1), shadow[1] = calloc(MEM2_SIZE, 1);
  if (!shadow[0] || !shadow[1]) { fprintf(stderr, "stop: record-sound: no memory\n"); exit(10); }
  memset(shadow[0], 0, MEM1_SIZE);
  memset(shadow[1], 0, MEM2_SIZE);
  memset(&header, 0, sizeof header);
  header.magic = DSP_RECORD_MAGIC;
  header.version = DSP_RECORD_VERSION;
  header.flags = from_boot ? DSP_RECORD_FROM_BOOT : 0;
  header.start_crc = dsp_lle_iram_crc();
  header.state_bytes = sizeof dsp_lle;
  header.start_time = last_time = time_base();
  header.reserved[0] = dsp_lle.control_reg;
  stop_at = header.start_time + frames * TICKS_PER_FRAME;
  put(&header, sizeof header);
  put(&dsp_lle, sizeof dsp_lle);
  pending.count = 0;
  dsp_lle_observe_memory(on_read, on_write);
  dsp_lle_observe_mail(on_mail, on_interrupt);
  dsp_recording = 1;
  static int at_exit;
  if (!at_exit) at_exit = !atexit(finish_at_exit);
  fprintf(stderr, "record-sound: recording %llu frames to %s\n", (unsigned long long)frames, path);
}

static void finish(void) {
  flush_pending();
  dsp_lle_observe_memory(0, 0);
  dsp_lle_observe_mail(0, 0);
  dsp_recording = 0;
  stopping = 0;
  header.end_crc = dsp_lle_iram_crc();
  header.end_time = time_base();
  long bytes = ftell(file);
  fseek(file, 0, SEEK_SET);
  put(&header, sizeof header);
  fclose(file);
  file = 0;
  fprintf(stderr, "record-sound: %u mails, %u events, %ld bytes in %s\n", header.mails, header.events, bytes, path);
}

void dsp_record_start(void) {
  const char *asked = getenv("WII_RECOMP_SOUND_RECORD");
  if (!asked) return;
  unsigned long long first, frames;
  int used = 0;
  if (sscanf(asked, "%llu:%llu:%n", &first, &frames, &used) != 2 || !used || !asked[used]) {
    fprintf(stderr, "WII_RECOMP_SOUND_RECORD=%s: wanted <frame>:<frames>:<file>\n", asked);
    return;
  }
  snprintf(env_path, sizeof env_path, "%s", asked + used);
  env_frames = frames;
  if (!first && dsp_interpreting()) begin(env_path, frames, 1);
  else env_start = time_base() + (first ? first : 1) * TICKS_PER_FRAME;  // Then waits for the interpreter.
  if (!dsp_interpreting()) fprintf(stderr, "record-sound: waits for the interpreter to run the game's program\n");
}

void dsp_record_update(uint64_t now) {
  // Each mail's work is kept whole: the recording ends as the game sends the next one,
  // or a second later if it doesn't.
  if (dsp_recording) {
    if (now >= stop_at + TICKS_PER_FRAME * 60) finish();
    else if (now >= stop_at) stopping = 1;
    return;
  }
  int frames = atomic_exchange(&requested, 0);
  if (frames) begin(requested_path, (uint64_t)frames, 0);
  else if (env_start && now >= env_start && dsp_interpreting()) {
    env_start = 0;
    begin(env_path, env_frames, 0);
  }
}
