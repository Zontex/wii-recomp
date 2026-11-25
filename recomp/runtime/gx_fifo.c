#define _GNU_SOURCE
// Reads the stream of commands the game sends to the graphics chip.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/mman.h>
#ifdef __linux__
#include <linux/futex.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/semaphore.h>
#endif
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#undef interface
#endif

#include "timeline.h"
#include "cpu.h"
#include "debug.h"
#include "title.h"
#include "gx.h"
#include "gx_backend.h"
#include "gaps.h"
#include "hardware.h"
#include "memory_watch.h"

enum {
  COMMAND_NOP = 0x00, COMMAND_CP = 0x08, COMMAND_XF = 0x10, COMMAND_XF_INDEXED_A = 0x20,
  COMMAND_XF_INDEXED_D = 0x38, COMMAND_CALL_LIST = 0x40, COMMAND_METRICS = 0x44, COMMAND_INVALIDATE = 0x48, COMMAND_BP = 0x61,
  COMMAND_DRAW = 0x80, COMMAND_DRAW_LAST = 0xbf,
};
enum {
  BP_MASK = 0xfe, BP_DRAW_DONE = 0x45, BP_TOKEN = 0x47, BP_TOKEN_NOW = 0x48, BP_COPY = 0x52,
  BP_LOAD_PALETTE = 0x65, BP_BOUNDING_BOX = 0x55, BP_PIXEL_FORMAT = 0x43, PIXEL_FORMAT_MASK = 7, PIXEL_FORMAT_DEPTH = 3, BP_TEV_REGISTER = 0xe0, TEV_KONST_FLAG = 0x800000,
  BP_GEN_MODE = 0x00, MULTISAMPLE_BIT = 9, BP_PRELOAD = 0x63,
  PIXEL_FORMAT_RGB565_Z16 = 2, PIXEL_FORMAT_Y8 = 4, DEPTH_FORMAT_SHIFT = 3,
};
enum { XF_CLIP_DISABLE = 0x1005 };
enum {
  PIPE_FIRST = 0xcc008000u, PIPE_LAST = 0xcc008fffu, CP_STATUS = 0xcc000000u, PE_STATUS = 0xcc00100au,
  PE_TOKEN_VALUE = 0xcc00100eu, CP_IDLE = 0x0c, PE_ENABLE_BITS = 3, PE_TOKEN_DONE = 4, PE_FINISH_DONE = 8,
  COPY_CLEAR_BIT = 11, COPY_TO_SCREEN_BIT = 14,
  PI_FIFO_BASE = 0xcc00300cu, PI_FIFO_END = 0xcc003010u, PI_FIFO_WRITE = 0xcc003014u, PI_ADDRESS_MASK = 0x1fffffffu,
  PI_FIFO_RESET = 0xcc003018u, FIFO_RESET = 1,
  CP_CONTROL = 0xcc000002u, CP_FIFO_BASE = 0xcc000020u, CP_FIFO_END = 0xcc000024u,
  CP_FIFO_WRITE = 0xcc000034u, CP_FIFO_READ = 0xcc000038u, CP_REGISTERS_END = 0xcc000040u,
  CP_DISTANCE_LOW = 0xcc000030u, CP_DISTANCE_HIGH = 0xcc000032u,
  CONTROL_READ_ENABLE = 1, CONTROL_INTERRUPT_ENABLE = 2, CONTROL_LINK = 0x10, CONTROL_BREAK_ENABLE = 0x20,
  MSR_INTERRUPTS = 0x8000,
  CP_BREAKPOINT = 0xcc00003cu, CP_STATUS_BREAK = 0x10, CP_STATUS_OVERFLOW = 1, CP_STATUS_UNDERFLOW = 2,
  CONTROL_OVERFLOW_ENABLE = 4, CONTROL_UNDERFLOW_ENABLE = 8, CP_CLEAR = 0xcc000004u, CP_HIGH_MARK = 0xcc000028u, CP_LOW_MARK = 0xcc00002cu,
};
enum { MAX_UNDRAWN_VERTICES = 3 };
// The stream holds at least one whole command, or the largest draw would wait forever.
// That is 65,535 vertices of 129 bytes: nine matrix indexes, a position and nine normal
// values as floats, two four-byte colors and eight pairs of float coordinates.
enum { LARGEST_VERTEX_BYTES = 9 + 3 * 4 + 9 * 4 + 2 * 4 + 8 * 2 * 4, LARGEST_DRAW_BYTES = 3 + 65535 * LARGEST_VERTEX_BYTES };
#define STREAM_BYTES (9u * 1024u * 1024u)
_Static_assert(STREAM_BYTES >= LARGEST_DRAW_BYTES, "a draw must fit the stream");
#define BURST_BYTES 32u
#define CP_ARRAY_BASE 0xa0
#define CP_ARRAY_STRIDE 0xb0
#define XF_INDEXED_FIRST_ARRAY 12
// The game finishes its bookkeeping just after asking, as a real chip takes time,
// so "finished" and "token reached" must not arrive at once.
#define SIGNAL_DELAY_TICKS 12000u

GxState gx;
GxStats gx_stats;
unsigned gx_cp_written = 1, gx_settings_written = 1, gx_xf_written[XF_PARTS] = {1, 1, 1, 1};

// The XF memory a write lands in: matrices from 0, normal matrices from 0x400, the
// matrices applied after from 0x500, lights from 0x600, and settings from 0x1000.
void gx_xf_note(uint32_t address, uint32_t count) {
  static const uint32_t starts[XF_PARTS + 1] = {0, 0x400, 0x500, 0x600, 0x1000};
  uint32_t end = address + count;
  for (int part = 0; part < XF_PARTS; part++) {
    if (address < starts[part + 1] && end > starts[part]) gx_xf_written[part]++;
  }
  if (end > starts[XF_PARTS]) gx_settings_written++;
}

static uint8_t stream[STREAM_BYTES];
static uint32_t stream_used;
static uint8_t burst[BURST_BYTES];
static uint32_t burst_used;
static uint32_t pe_enabled;
// The token's value and the signals' status are set by the game's thread, and with a
// BP_TOKEN_NOW also by the commands thread, which reads the status to do it.
static _Atomic uint32_t pe_status, pe_token;
// The last token the chip came to. The game sees it (pe_token) when that token's interrupt
// is raised, not when it's parsed: the chip here is through a frame's tokens in
// microseconds, and a handler still on its way for an earlier one would read the frame's
// last token, which then came round again with its own interrupt. Mario & Sonic at the
// Olympic Winter Games wakes its main thread on that token, so the second one ended its
// wait for the screen and the game ran hundreds of frames a second.
static _Atomic uint32_t token_parsed;
static int frame_shown;  // A copy to the screen was just read.
static _Atomic uint64_t finish_due, token_due;
static int at_breakpoint;
static uint32_t fifo_flags;  // Overflow and underflow, until the game clears them.
// Games that drive the chip with breakpoints check at a retrace that the frame token is
// exactly the next one, as a real chip takes that long over a frame. They may check only
// every other retrace, at 30 fps. This chip is instant, so it pauses after each token
// until the game has read it, or a few retraces pass if it never does.
enum { HOLD_RETRACES_AT_MOST = 120 };  // A last resort: a retrace can pass while interrupts are off.
enum { BP_COPY_ADDRESS = 0x4b };  // Where a copy goes, in 32-byte units.
static int paces_by_token, held_at_token, token_seen, retraces_held;
enum { SCREEN_COPIES_KEPT = 4 };  // Enough for triple buffering.
static uint32_t screen_copies[SCREEN_COPIES_KEPT];  // Where the latest copies to the screen went.
static unsigned screen_copy_count;

int gx_screen_copy_went_to(uint32_t address) {
  for (int i = 0; i < SCREEN_COPIES_KEPT; i++) if (screen_copies[i] && ((screen_copies[i] ^ address) & 0x1fffffff) == 0) return 1;
  return 0;
}
static enum { RUNNING, WAITING_FOR_RETRACE, RESUME_DUE } resume_state;

static uint32_t big32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t big16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }

static void raise_pe(uint32_t done, uint32_t enable, int irq) {
  pe_status |= done;
  if (pe_enabled & enable) raise_irq(irq);
}

// Copy settings the picture copies don't follow yet. Bits from Dolphin's UPE_Copy.
static void copy_gaps(uint32_t control) {
  int to_screen = (control >> COPY_TO_SCREEN_BIT) & 1;
  uint32_t gamma = (control >> 7) & 3, field = (control >> 12) & 3, y_scale = gx.bp[0x4e] & 0x1ff;
  if (to_screen && gamma) gap_hit("pixel.gamma_on_xfb_copy_1_0_1_7_2_2", gamma);
  // A copy to the screen fills the window whatever its height, so scaling it taller for a
  // TV's lines, or copying half a picture for one field of it, shows the same: the field is
  // stretched over the window. Copies anywhere else would come out the wrong height.
  if (!to_screen && ((control >> 10) & 1) && y_scale != 0x100) gap_hit("pixel.copy_y_scale", y_scale);
  if (!to_screen && field) gap_hit("pixel.field_copy_frame_to_field", field);
}

// Pixel settings we don't follow, named as the game sets them. Bits from Dolphin's BPMemory.h.
static void setting_gaps(uint32_t reg, uint32_t data) {
  if (reg == BP_GEN_MODE) {
    if ((data >> MULTISAMPLE_BIT) & 1) gap_hit("pixel.multisample_and_aa_modes", MULTISAMPLE_BIT);
  } else if (reg == BP_PIXEL_FORMAT) {
    uint32_t format = data & PIXEL_FORMAT_MASK;
    if (format == PIXEL_FORMAT_RGB565_Z16) gap_hit("pixel.z16_precision_formats", (data >> DEPTH_FORMAT_SHIFT) & 7);
    else if (format >= PIXEL_FORMAT_Y8) gap_hit("pixel.efb_y8_u8_v8_yuv420", format);
  } else if (reg == BP_PRELOAD && data) {
    gap_hit("gx.0x60_0x63_texture_preload", data);  // The SDK writes 0 here to end a sync.
  }
}

// A real chip takes milliseconds over a frame, so it never finishes two for one field of
// the screen. This one is through a frame in microseconds, and a game paced by the chip
// rather than the screen runs away: Mario & Sonic at the Olympic Winter Games, once its
// "drawing done" flag is a frame out of step, is woken from its wait for the screen by each
// frame's end and made 600 to 900 frames a second. So a picture copied to a new
// framebuffer in the same field as the last, and less than a third of a field after it,
// waits on the commands thread until the screen's next field. Two further apart are let
// through: a frame that takes about a field (Mario Sports Mix on a small machine) ends
// now just before a field begins and now just after, and holding the second of those
// made every tenth picture late. But no more pictures than fields are let through over
// time: each field allows one, a field that passed with none leaves its one for the next
// (so the two of Mario Sports Mix pass), and a picture with none left waits for the next
// field. Without that the same game, in a scene its frames take 11 to 15 ms, made 68
// pictures a second for a screen that shows 60, and its models came out torn into long
// triangles across the picture; held to the screen's count they are whole.
// The wait also ends a field after the last one began by the host's clock: the game's
// thread, which counts the fields, may be waiting for this thread.
// WII_RECOMP_FRAMES_UNHELD=1 turns it off.
static _Atomic unsigned fields_begun;
static _Atomic uint64_t field_began;  // Host nanoseconds.
// Worked out by the video interface on the game's thread, as each field begins: the
// commands thread can't ask vi.c, whose timing the game's thread rewrites when it
// changes the video mode.
static _Atomic uint64_t field_length = 1000000000ull / 60;
static _Thread_local int on_commands_thread;
static void run_handed(int between);
static void idle_wait(void);

static void hold_for_field(uint32_t address) {
  enum { MOST_ALLOWED = 2 };
  static unsigned last_field = ~0u, counted_field;
  static uint32_t last_address;
  static uint64_t last_time;
  static int off = -1, allowed = MOST_ALLOWED;
  if (off < 0) off = getenv("WII_RECOMP_FRAMES_UNHELD") != 0;
  unsigned field = atomic_load(&fields_begun);
  uint64_t field_time = gx_field_nanoseconds(), now = host_nanoseconds();
  int new_picture = address != last_address;
  allowed += (int)(field - counted_field);
  if (allowed > MOST_ALLOWED) allowed = MOST_ALLOWED;
  counted_field = field;
  int close = field == last_field && now - last_time < field_time / 3;
  if (!off && on_commands_thread && new_picture && (close || allowed <= 0)) {
    uint64_t give_up = atomic_load(&field_began) + field_time + 2000000u;
    TIMELINE_BEGIN("held for the field");
    while (atomic_load(&fields_begun) == field && host_nanoseconds() < give_up) {
      run_handed(1);
      // A picture held for its moment on the screen comes due during this wait as at any
      // other time: left until the wait was over, it was shown late and the next one right
      // after it, two pictures 3 ms apart and then none for 30.
      if (gx_backend->show_held) gx_backend->show_held();
      idle_wait();
    }
    TIMELINE_END("held for the field");
    unsigned after = atomic_load(&fields_begun);
    allowed += (int)(after - counted_field);
    if (allowed > MOST_ALLOWED) allowed = MOST_ALLOWED;
    counted_field = field = after;
    now = host_nanoseconds();
  }
  if (new_picture) {
    last_time = now;
    if (allowed > 0) allowed--;
  }
  last_field = field;
  last_address = address;
}

static void write_bp(uint32_t value) {
  uint32_t reg = value >> 24, data = value & 0xffffff;
  gx_settings_written++;
  if (reg == BP_MASK) {
    gx.bp[BP_MASK] = data;
    return;
  }
  uint32_t mask = gx.bp[BP_MASK] ? gx.bp[BP_MASK] : 0xffffff;
  gx.bp[reg] = (gx.bp[reg] & ~mask) | (data & mask);
  gx.bp[BP_MASK] = 0;
  if (reg == BP_GEN_MODE || reg == BP_PIXEL_FORMAT || reg == BP_PRELOAD) setting_gaps(reg, gx.bp[reg]);
  if (reg >= BP_TEV_REGISTER && reg < BP_TEV_REGISTER + 8) {
    uint32_t *bank = data & TEV_KONST_FLAG ? gx.tev_konst : gx.tev_color;
    bank[reg - BP_TEV_REGISTER] = gx.bp[reg];
  }
  if (reg == BP_DRAW_DONE) {
    TIMELINE_MOMENT("draw done read");
    gx_stats.draw_done++;
    finish_due = time_base() + SIGNAL_DELAY_TICKS;
  }
  else if (reg == BP_TOKEN || reg == BP_TOKEN_NOW) {
    atomic_store(&token_parsed, data & 0xffff);
    // With no interrupt for it, the value shows at once unless a handler is on its way.
    if (reg == BP_TOKEN_NOW && !atomic_load(&token_due) && !(pe_status & PE_TOKEN_DONE)) pe_token = data & 0xffff;
    TIMELINE_MOMENT("token read");
    if (reg == BP_TOKEN) token_due = time_base() + SIGNAL_DELAY_TICKS;
    if (reg == BP_TOKEN && paces_by_token) {
      held_at_token = 1;
      token_seen = retraces_held = 0;
    }
  } else if (reg == BP_LOAD_PALETTE) {
    gx_load_palette();
  } else if (reg == BP_BOUNDING_BOX || reg == BP_BOUNDING_BOX + 1) {
    gx_bounding_box_write((int)reg, gx.bp[reg]);
  } else if (reg == BP_COPY) {
    gx_stats.copies++;
    copy_gaps(data);
    if (data & (1u << COPY_TO_SCREEN_BIT)) {
      static uint64_t last_frame;
      uint64_t frame_time = time_base();
      float gap = last_frame ? (float)(frame_time - last_frame) / 60750.0f : 0;  // 60,750 ticks to the millisecond.
      if (gap > gx_stats.worst_frame_ms) gx_stats.worst_frame_ms = gap;
      last_frame = frame_time;
      stage_frame(gap);
      gx_stats.to_screen++;
      screen_copies[screen_copy_count++ % SCREEN_COPIES_KEPT] = gx.bp[BP_COPY_ADDRESS] << 5;
    }
    // The chip copies the picture out first, then clears for the next frame.
    if (data & (1u << COPY_TO_SCREEN_BIT)) {
      hold_for_field(gx.bp[BP_COPY_ADDRESS] << 5);
      gx_bounding_box_off();  // As Dolphin does at a copy to the screen.
      uint64_t present_start = time_base();
      TIMELINE_BEGIN("present");
      gx_present();
      TIMELINE_END("present");
      timeline_frame_shown();
      stage_add(STAGE_PRESENT, time_base() - present_start);
      frame_shown = 1;
    }
    else gx_copy_to_texture(data, (gx.bp[BP_PIXEL_FORMAT] & PIXEL_FORMAT_MASK) == PIXEL_FORMAT_DEPTH);
    if (data & (1u << COPY_CLEAR_BIT)) {
      TIMELINE_BEGIN("clear");
      gx_clear();
      TIMELINE_END("clear");
    }
  }
}

static void load_indexed(int command, uint32_t value) {
  int array = XF_INDEXED_FIRST_ARRAY + (command - COMMAND_XF_INDEXED_A) / 8;
  uint32_t index = value >> 16, count = ((value >> 12) & 15) + 1, address = value & 0xfff;
  uint32_t source = (gx.cp[CP_ARRAY_BASE + array] + index * (gx.cp[CP_ARRAY_STRIDE + array] & 0xff)) | 0x80000000u;
  for (uint32_t i = 0; i < count && address + i < XF_SIZE; i++) gx.xf[address + i] = rd32(source + i * 4);
  gx_xf_note(address, count);
}

// Parses one command, returns its bytes, or 0 if it isn't all here yet.
static uint32_t parse_command(const uint8_t *p, uint32_t left) {
  int command = p[0];
  // 0x44 starts the chip's counters for a profiler, and is one byte, as Dolphin's OpcodeDecoding.cpp reads it.
  if (command == COMMAND_NOP || command == COMMAND_INVALIDATE || command == COMMAND_METRICS) return 1;
  if (command == COMMAND_BP) {
    if (left < 5) return 0;
    write_bp(big32(p + 1));
    return 5;
  }
  if (command == COMMAND_CP) {
    if (left < 6) return 0;
    gx.cp[p[1]] = big32(p + 2);
    gx_cp_written++;
    gx_settings_written++;
    return 6;
  }
  if (command == COMMAND_XF) {
    if (left < 5) return 0;
    uint32_t count = big16(p + 1) + 1, address = big16(p + 3), bytes = 5 + count * 4;
    if (left < bytes) return 0;
    for (uint32_t i = 0; i < count && address + i < XF_SIZE; i++) gx.xf[address + i] = big32(p + 5 + i * 4);
    gx_xf_note(address, count);
    // Turning clipping off only changes what lies outside the view. Not followed.
    if (address <= XF_CLIP_DISABLE && address + count > XF_CLIP_DISABLE && gx.xf[XF_CLIP_DISABLE]) gap_hit("gx.0x1005_clip_disable", gx.xf[XF_CLIP_DISABLE]);
    return bytes;
  }
  if (command >= COMMAND_XF_INDEXED_A && command <= COMMAND_XF_INDEXED_D && !(command & 7)) {
    if (left < 5) return 0;
    load_indexed(command, big32(p + 1));
    return 5;
  }
  if (command == COMMAND_CALL_LIST) {
    if (left < 9) return 0;
    uint32_t address = big32(p + 1) | 0x80000000u, list_bytes = big32(p + 5);
    uint8_t *list = host(address);
    if (list) {
      GxCalledList outer = gx_called_list;
      // Watched only for keeping its draws (gx_read_list_vertices): watching takes a lock for each call,
      // and a list the game rewrites every frame would stop its thread at the first write each time.
      gx_called_list = (GxCalledList){address, list_bytes, gx_lists_kept() ? watch_range(address, list_bytes) : WATCH_NONE, list};
      gx_record_list_enter();
      gx_parse(list, list_bytes);
      gx_record_list_leave();
      gx_called_list = outer;
    }
    return 9;
  }
  if (command >= COMMAND_DRAW && command <= COMMAND_DRAW_LAST) {
    if (left < 3) return 0;
    int format = command & 7, primitive = (command >> 3) & 7, count = (int)big16(p + 1);
    uint32_t bytes = 3 + gx_vertices_size(format, count);
    if (left < bytes) return 0;
    if (count <= MAX_VERTICES) {
      gx_stats.draws++;
      gx_vertex_format = format;
      if (gx_draw_wanted(primitive, count)) {
        Vertex *room = gx_draw_reserve(count);
        gx_draw(primitive, gx_called_list.start ? gx_read_list_vertices(p + 3, format, count, room) : gx_read_vertices(p + 3, format, count, room));
      } else if (gx_draw_expanded(primitive, format, p + 3, count)) {
      } else if (count) {
        Vertex too_few[MAX_UNDRAWN_VERTICES];  // Nothing to draw, but the last normal still counts.
        gx_read_vertices(p + 3, format, count, too_few);
      }
    }
    return bytes;
  }
  static int warnings;
  if (warnings++ < 20) fprintf(stderr, "gx: unknown command %02x\n", command);
  return 1;
}

// Each command read is also offered to a recording (gx_record.c), which ends its frames
// after the command that showed one.
static uint32_t parse_one(const uint8_t *p, uint32_t left) {
  uint32_t used = parse_command(p, left);
  if (used && gx_recording) gx_record_command(p, used);
  if (frame_shown) {
    frame_shown = 0;
    gx_record_frame_shown();
  }
  return used;
}

void gx_parse(const uint8_t *data, uint32_t bytes) {
  uint32_t at = 0;
  while (at < bytes) {
    uint32_t used = parse_one(data + at, bytes - at);
    if (!used) break;
    at += used;
  }
}

static uint32_t read_pair(uint32_t low_address) {
  return ((hw_peek(low_address + 2, 2) << 16) | hw_peek(low_address, 2)) & PI_ADDRESS_MASK;
}

static void write_pair(uint32_t low_address, uint32_t value) {
  hw_poke(low_address, value & 0xffff, 2);
  hw_poke(low_address + 2, value >> 16, 2);
}

// The chip runs beside the CPU on a Wii, and the game already waits for it through
// tokens and draw done. So commands go to a thread of their own, through a ring mapped
// twice in a row, which lets a command crossing the end read as one piece. A game using
// FIFO breakpoints needs the chip in step with it, and stays on this thread, as does
// WII_RECOMP_GX_INLINE=1.
static int threaded = -1;
static uint64_t ring_added;  // Every byte handed to the commands thread, for the stream report.
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
#define RING_BYTES (16u * 1024u * 1024u)
#define EMPTY_TURNS_BEFORE_SLEEP 200
#define EMPTY_SLEEP_NANOSECONDS 50000
static uint8_t *ring;
static _Atomic uint64_t ring_written, ring_parsed;
static pthread_t commands_thread_id;

// Work another thread hands the commands thread, such as a peek at the picture or
// filling in a copy the processor touched. A peek runs once the commands before it have;
// work that's soon runs between the next two, so a touch doesn't wait out a frame's
// backlog of draws. The thread asking waits for it.
static struct { void (*work)(void *); void *argument; int soon; } handed;
static atomic_flag handing = ATOMIC_FLAG_INIT;
static _Atomic int handed_waiting, handed_done;

static void run_handed(int between) {
  if (!atomic_load_explicit(&handed_waiting, memory_order_acquire) || (between && !handed.soon)) return;
  // Work handed in between two draws may leave the backend's draw state (a held copy read
  // back when the game touches its page ends the pass): the next draw sets it all again.
  gx_settings_written++;
  TIMELINE_BEGIN("handed work");
  handed.work(handed.argument);
  TIMELINE_END("handed work");
  atomic_store_explicit(&handed_waiting, 0, memory_order_relaxed);
  atomic_store_explicit(&handed_done, 1, memory_order_release);
}

static void wait_briefly(void) {
  struct timespec pause = {0, EMPTY_SLEEP_NANOSECONDS};
  nanosleep(&pause, 0);
}

// The commands thread's wait when there's nothing to parse. A timer on Windows comes due
// about half a millisecond late however short it's set, and a thread handing work over
// waited that out each time: Mario Sports Mix touches eight held copies a picture, which
// cost it every other frame. So there the wait also ends when work is handed over.
#ifdef _WIN32
static HANDLE idle_wake;
static void idle_wait(void) {
  static HANDLE timer;
  if (!timer) timer = CreateWaitableTimerExW(0, 0, 2 /* high resolution */, TIMER_ALL_ACCESS);
  LARGE_INTEGER due;
  due.QuadPart = -(EMPTY_SLEEP_NANOSECONDS / 100);
  if (!timer || !idle_wake || !SetWaitableTimer(timer, &due, 0, 0, 0, FALSE)) { wait_briefly(); return; }
  HANDLE either[2] = {idle_wake, timer};
  WaitForMultipleObjects(2, either, FALSE, INFINITE);
}
static void wake_idle(void) { if (idle_wake) SetEvent(idle_wake); }
#elif defined(__linux__)
// On Linux a sleep that short comes due 50 to 100 microseconds late as well (the kernel
// rounds a thread's timers together), and New Super Mario Bros. Wii touches some 25 held
// copies a picture: its thread waited a quarter of a millisecond for each, 2.6 ms of every
// frame, which left too little room for a frame with more to do, and one was missed every
// ten seconds or so. So here too the wait ends at once when work is handed over: the
// thread sleeps on a word the hander sets and wakes it by (a futex).
// Commands written to the ring wake it the same way, but only when it is asleep, as the
// game writes them a few bytes at a time and waking costs a call to the kernel.
static _Atomic int idle_word, idle_asleep;
static void idle_wait(void) {
  struct timespec pause = {0, EMPTY_SLEEP_NANOSECONDS};
  atomic_store_explicit(&idle_asleep, 1, memory_order_seq_cst);
  if (!atomic_exchange_explicit(&idle_word, 0, memory_order_seq_cst)) syscall(SYS_futex, &idle_word, FUTEX_WAIT_PRIVATE, 0, &pause, 0, 0);
  atomic_store_explicit(&idle_asleep, 0, memory_order_seq_cst);
  atomic_store_explicit(&idle_word, 0, memory_order_release);
}
static void wake_idle(void) {
  atomic_store_explicit(&idle_word, 1, memory_order_seq_cst);
  if (atomic_load_explicit(&idle_asleep, memory_order_seq_cst)) syscall(SYS_futex, &idle_word, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
}
#define WAKES_FOR_COMMANDS 1
#elif defined(__APPLE__)
// On a Mac a short sleep can come due tens of milliseconds late, whenever the system
// rounds timers together, as it does while the screen is locked or its user is away (see
// host_sleep.h), and the game then waited that long for its frame's token. So the wait
// ends at once when work is handed over or commands written, as on Linux, and when a
// field begins, which a picture held for its moment on the screen waits for. A semaphore
// the hander signals, only when the thread is asleep, stands in for Linux's futex.
static semaphore_t idle_semaphore;
static _Atomic int idle_word, idle_asleep, idle_ready;
static void idle_wait(void) {
  if (!atomic_load(&idle_ready)) { wait_briefly(); return; }
  mach_timespec_t pause = {0, EMPTY_SLEEP_NANOSECONDS};
  atomic_store_explicit(&idle_asleep, 1, memory_order_seq_cst);
  if (!atomic_exchange_explicit(&idle_word, 0, memory_order_seq_cst)) semaphore_timedwait(idle_semaphore, pause);
  atomic_store_explicit(&idle_asleep, 0, memory_order_seq_cst);
  atomic_store_explicit(&idle_word, 0, memory_order_release);
}
static void wake_idle(void) {
  atomic_store_explicit(&idle_word, 1, memory_order_seq_cst);
  if (atomic_load(&idle_ready) && atomic_load_explicit(&idle_asleep, memory_order_seq_cst)) semaphore_signal(idle_semaphore);
}
#define WAKES_FOR_COMMANDS 1
#else
static void idle_wait(void) { wait_briefly(); }
static void wake_idle(void) {}
#endif

static void *commands_thread(void *unused) {
  (void)unused;
  on_commands_thread = 1;
  stage_sample_thread(1);
#ifdef __linux__
  prctl(PR_SET_TIMERSLACK, 1000);  // Short sleeps end when asked, not 50 microseconds later (the kernel's usual rounding).
#endif
  uint64_t parsed = 0;
  int empty_turns = 0;
  while (1) {
    uint64_t written = atomic_load_explicit(&ring_written, memory_order_acquire);
    uint32_t used = written == parsed ? 0 : parse_one(ring + (parsed & (RING_BYTES - 1)), (uint32_t)(written - parsed));
    run_handed(used != 0);
    if (gx_backend->show_held) gx_backend->show_held();
    if (used) {
      parsed += used;
      atomic_store_explicit(&ring_parsed, parsed, memory_order_release);
      empty_turns = 0;
    } else if (++empty_turns < EMPTY_TURNS_BEFORE_SLEEP) {
      sched_yield();
    } else {
      TIMELINE_BEGIN("no commands");
      idle_wait();
      TIMELINE_END("no commands");
    }
  }
  return 0;
}

// Memory with no name in the file system, to map twice. macOS has no memfd_create, so it
// takes shared memory under a name, and drops the name at once.
#ifndef _WIN32
static int ring_file(void) {
#ifdef __APPLE__
  char name[32];
  snprintf(name, sizeof name, "/wii-recomp-%d", (int)getpid());
  int file = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  shm_unlink(name);
  return file;
#else
  return memfd_create("wii-recomp-commands", 0);
#endif
}
#endif

#ifdef _WIN32
// Windows maps a section twice in a row through placeholders (Windows 10 1803 on): a
// reservation split in two, each half replaced by a view of the same memory. The calls
// are looked up at run time, so an older Windows runs the commands on the game's thread.
typedef PVOID (WINAPI *VirtualAlloc2Call)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef PVOID (WINAPI *MapViewOfFile3Call)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
#define PLACEHOLDER_RESERVE 0x00040000u   // MEM_RESERVE_PLACEHOLDER
#define PLACEHOLDER_REPLACE 0x00004000u   // MEM_REPLACE_PLACEHOLDER
#define PLACEHOLDER_PRESERVE 0x00000002u  // MEM_PRESERVE_PLACEHOLDER

static uint8_t *map_ring_twice(void) {
  HMODULE kernel = GetModuleHandleA("kernelbase.dll");
  VirtualAlloc2Call reserve = kernel ? (VirtualAlloc2Call)(void *)GetProcAddress(kernel, "VirtualAlloc2") : 0;
  MapViewOfFile3Call view = kernel ? (MapViewOfFile3Call)(void *)GetProcAddress(kernel, "MapViewOfFile3") : 0;
  if (!reserve || !view) return 0;
  HANDLE section = CreateFileMappingA(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, 0, RING_BYTES, 0);
  if (!section) return 0;
  uint8_t *both = reserve(0, 0, RING_BYTES * 2, MEM_RESERVE | PLACEHOLDER_RESERVE, PAGE_NOACCESS, 0, 0);
  if (!both || !VirtualFree(both, RING_BYTES, MEM_RELEASE | PLACEHOLDER_PRESERVE)
      || !view(section, GetCurrentProcess(), both, 0, RING_BYTES, PLACEHOLDER_REPLACE, PAGE_READWRITE, 0, 0)
      || !view(section, GetCurrentProcess(), both + RING_BYTES, 0, RING_BYTES, PLACEHOLDER_REPLACE, PAGE_READWRITE, 0, 0)) {
    fprintf(stderr, "graphics: no ring of commands (error %lu), so they run on the game's thread\n", GetLastError());
    return 0;
  }
  return both;
}
#endif

static int start_commands_thread(void) {
#ifdef _WIN32
  uint8_t *both = map_ring_twice();
  if (!both) return 0;
#else
  int file = ring_file();
  if (file < 0 || ftruncate(file, RING_BYTES)) return 0;
  uint8_t *both = mmap(0, RING_BYTES * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (both == MAP_FAILED || mmap(both, RING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, file, 0) == MAP_FAILED
      || mmap(both + RING_BYTES, RING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, file, 0) == MAP_FAILED) {
    return 0;
  }
#endif
  ring = both;
#ifdef _WIN32
  idle_wake = CreateEventW(0, FALSE, FALSE, 0);
#elif defined(__APPLE__)
  atomic_store(&idle_ready, semaphore_create(mach_task_self(), &idle_semaphore, SYNC_POLICY_FIFO, 0) == KERN_SUCCESS);
#endif
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, 16u * 1024u * 1024u);
  gx_shows_held = 1;  // The thread shows a held picture between commands.
  return pthread_create(&commands_thread_id, &attributes, commands_thread, 0) == 0;
}

static void lock_graphics(void);
static void unlock_graphics(void);
static int may_use_graphics(void);

static void hand_over(void (*work)(void *), void *argument, int soon) {
  if (threaded <= 0 || pthread_equal(pthread_self(), commands_thread_id)) {
    if (!may_use_graphics()) return;
    lock_graphics();
    work(argument);
    unlock_graphics();
    return;
  }
  while (atomic_flag_test_and_set_explicit(&handing, memory_order_acquire)) wait_briefly();
  handed.work = work;
  handed.argument = argument;
  handed.soon = soon;
  atomic_store_explicit(&handed_done, 0, memory_order_relaxed);
  atomic_store_explicit(&handed_waiting, 1, memory_order_release);
  wake_idle();
  uint64_t waiting_since = time_base();
  while (!atomic_load_explicit(&handed_done, memory_order_acquire)) sched_yield();
  stage_add(STAGE_HANDED, time_base() - waiting_since);
  atomic_flag_clear_explicit(&handing, memory_order_release);
}

void gx_run_on_graphics(void (*work)(void *), void *argument) { hand_over(work, argument, 0); }
void gx_run_on_graphics_soon(void (*work)(void *), void *argument) { hand_over(work, argument, 1); }

int graphics_busy(void) {
  return threaded > 0 && atomic_load_explicit(&ring_parsed, memory_order_acquire) != atomic_load_explicit(&ring_written, memory_order_relaxed);
}

static void ring_add(const uint8_t *bytes, uint32_t count) {
  uint64_t written = atomic_load_explicit(&ring_written, memory_order_relaxed);
  while (written + count - atomic_load_explicit(&ring_parsed, memory_order_acquire) > RING_BYTES) wait_briefly();
  memcpy(ring + (written & (RING_BYTES - 1)), bytes, count);
  ring_added += count;
  atomic_store_explicit(&ring_written, written + count, memory_order_release);
#ifdef WAKES_FOR_COMMANDS
  wake_idle();
#endif
}
#else
#error "the commands thread's ring is built for Linux, macOS and Windows"
#endif

// With no thread of their own, commands run on the game's, which holds this while it
// runs them. Another thread that needs the backend, such as one touching a picture copy's
// memory, waits for it to be between commands. OpenGL works only on the thread that made
// it current, so there such a thread is refused and names it.
static atomic_flag graphics_lock = ATOMIC_FLAG_INIT;
static pthread_t lock_owner, first_runner;
static _Atomic int lock_owned, first_runner_known;
static int lock_depth;

static void lock_graphics(void) {
  if (atomic_load_explicit(&lock_owned, memory_order_acquire) && pthread_equal(lock_owner, pthread_self())) {
    lock_depth++;
    return;
  }
  while (atomic_flag_test_and_set_explicit(&graphics_lock, memory_order_acquire)) sched_yield();
  lock_owner = pthread_self();
  atomic_store_explicit(&lock_owned, 1, memory_order_release);
  lock_depth = 1;
  if (!atomic_load_explicit(&first_runner_known, memory_order_relaxed)) {
    first_runner = pthread_self();
    atomic_store_explicit(&first_runner_known, 1, memory_order_release);
  }
}

static void unlock_graphics(void) {
  if (--lock_depth) return;
  atomic_store_explicit(&lock_owned, 0, memory_order_release);
  atomic_flag_clear_explicit(&graphics_lock, memory_order_release);
}

static int may_use_graphics(void) {
  if (!gx_backend || strcmp(gx_backend->name, "opengl") || !atomic_load_explicit(&first_runner_known, memory_order_acquire)) return 1;
  if (pthread_equal(first_runner, pthread_self())) return 1;
  gap_hit("pixel.copy_to_texture", 0x0e1u);  // OpenGL from another thread.
  return 0;
}

static void run_stream(void) {
  uint32_t at = 0;
  lock_graphics();
  while (at < stream_used && !held_at_token) {
    uint32_t used = parse_one(stream + at, stream_used - at);
    if (!used) break;
    at += used;
  }
  if (at) {
    memmove(stream, stream + at, stream_used - at);
    stream_used -= at;
  }
  unlock_graphics();
}

// The FIFO's end register is 4 bytes short of the true end, and writes arrive in
// 32-byte bursts, so the ring really turns over at the next 32-byte boundary.
static uint32_t ring_limit(uint32_t end) { return (end + 31) & ~31u; }

// A real chip trails the CPU, so a game can set a breakpoint at a place it has already
// finished writing. This chip keeps up with the CPU, so it may already be past there,
// and must stop as if it had just arrived. "Past" means less than half a ring ago.
static int reached_breakpoint(uint32_t read, uint32_t breakpoint, uint32_t base, uint32_t limit) {
  if (breakpoint < base || breakpoint >= limit) return read == breakpoint;
  uint32_t size = limit - base;
  return (read + size - breakpoint) % size < size / 2;
}

// A chip paused at a token can be lapped by the CPU, losing what it hadn't read. A real
// one is slower than the CPU all the time, so the FIFO has marks: past the high one the
// SDK suspends the writing thread, and at the low one it wakes it again.
static void check_fifo_marks(uint32_t control, uint32_t read, uint32_t write, uint32_t size) {
  uint32_t waiting = (write + size - read) % size;
  if ((control & CONTROL_OVERFLOW_ENABLE) && waiting >= read_pair(CP_HIGH_MARK)) fifo_flags |= CP_STATUS_OVERFLOW;
  if ((control & CONTROL_UNDERFLOW_ENABLE) && waiting <= read_pair(CP_LOW_MARK)) fifo_flags |= CP_STATUS_UNDERFLOW;
  if (fifo_flags) raise_irq(IRQ_CP);
}

static int chip_follows_cpu(void) {
  uint32_t cpu_base = hw_peek(PI_FIFO_BASE, 4) & PI_ADDRESS_MASK;
  return (hw_peek(CP_CONTROL, 2) & CONTROL_LINK) && read_pair(CP_FIFO_BASE) == cpu_base;
}

// The chip reads its own FIFO up to the write position. While linked to the CPU's
// FIFO, that position follows the CPU; a display list being recorded is not linked.
static void pump(void) {
  uint32_t control = hw_peek(CP_CONTROL, 2);
  uint32_t base = read_pair(CP_FIFO_BASE), end = read_pair(CP_FIFO_END);
  if (!(control & CONTROL_READ_ENABLE) || end <= base) return;
  if (chip_follows_cpu()) {
    write_pair(CP_FIFO_WRITE, hw_peek(PI_FIFO_WRITE, 4) & PI_ADDRESS_MASK);
  }
  uint32_t read = read_pair(CP_FIFO_READ), write = read_pair(CP_FIFO_WRITE);
  uint32_t limit = ring_limit(end);
  if (title->fifo_breakpoints) check_fifo_marks(control, read_pair(CP_FIFO_READ), read_pair(CP_FIFO_WRITE), limit - base);
  // A game that turns breakpoints on wants them: honoured whenever the chip runs on the
  // game's thread (WII_RECOMP_GX_INLINE=1, or no ring), not only in games built to keep
  // frames apart.
  int breaking = (title->fifo_breakpoints || threaded == 0) && (control & CONTROL_BREAK_ENABLE);
  uint32_t breakpoint = read_pair(CP_BREAKPOINT);
  if (at_breakpoint || held_at_token || resume_state != RUNNING) return;  // Bursts from the CPU land here too.
  if (threaded < 0) threaded = !title->fifo_breakpoints && !getenv("WII_RECOMP_GX_INLINE") && start_commands_thread();
  if (threaded) {
    while (read != write) {
      uint32_t stop = read < write ? write : limit, count = stop - read;
      ring_add(host(read | 0x80000000u), count);
      read = read + count >= limit ? base : read + count;
    }
    write_pair(CP_FIFO_READ, read);
    return;
  }
  while (read != write && stream_used < STREAM_BYTES && !(breaking && reached_breakpoint(read, breakpoint, base, limit))) {
    stream[stream_used++] = (uint8_t)rd8(read | 0x80000000u);
    read = read + 1 >= limit ? base : read + 1;
  }
  write_pair(CP_FIFO_READ, read);
  uint64_t stream_start = time_base();
  run_stream();
  stage_add(STAGE_STREAM, time_base() - stream_start);
  // Games that keep several frames in the FIFO stop the chip here, and its handler
  // moves the breakpoint on to the next frame.
  if (title->fifo_breakpoints) check_fifo_marks(control, read, write, limit - base);
  if (breaking && reached_breakpoint(read, breakpoint, base, limit)) {
    at_breakpoint = 1;
    if (control & CONTROL_INTERRUPT_ENABLE) {
      paces_by_token = 1;
      raise_irq(IRQ_CP);
    }
  }
}

// The CPU's write pipe collects bytes and sends them in 32-byte bursts, so the write
// position is always aligned. The game relies on that when it saves and restores it.
static void send_burst(void) {
  uint32_t write = hw_peek(PI_FIFO_WRITE, 4) & PI_ADDRESS_MASK;
  uint32_t base = hw_peek(PI_FIFO_BASE, 4) & PI_ADDRESS_MASK;
  uint32_t limit = ring_limit(hw_peek(PI_FIFO_END, 4) & PI_ADDRESS_MASK);
  if (write >= limit) write = base;
  memcpy(host(write | 0x80000000u), burst, BURST_BYTES);
  write += BURST_BYTES;
  // The game reads a display list's size from the write position, and the Mii face
  // mask's list fills its buffer exactly, so only the live FIFO turns over at once.
  hw_poke(PI_FIFO_WRITE, write >= limit && chip_follows_cpu() ? base : write, 4);
  pump();
  // Checks for interrupts come too rarely to stop a fast writer before it laps the chip.
  if ((fifo_flags & CP_STATUS_OVERFLOW) && (cpu.msr & MSR_INTERRUPTS)) poll_interrupts();
}

static void feed_stream(uint32_t value, int bytes) {
  if (gx_efb_pokes_waiting) gx_efb_send_pokes();
  if (burst_used + (uint32_t)bytes <= BURST_BYTES) {  // As it nearly always does: the value whole, high byte first.
    uint8_t *at = burst + burst_used;
    if (bytes == 4) at[0] = (uint8_t)(value >> 24), at[1] = (uint8_t)(value >> 16), at[2] = (uint8_t)(value >> 8), at[3] = (uint8_t)value;
    else if (bytes == 2) at[0] = (uint8_t)(value >> 8), at[1] = (uint8_t)value;
    else for (int i = 0; i < bytes; i++) at[i] = (uint8_t)(value >> ((bytes - 1 - i) * 8));
    burst_used += (uint32_t)bytes;
    if (burst_used == BURST_BYTES) {
      burst_used = 0;
      send_burst();
    }
    return;
  }
  for (int i = bytes - 1; i >= 0; i--) {
    burst[burst_used++] = (uint8_t)(value >> (i * 8));
    if (burst_used == BURST_BYTES) {
      burst_used = 0;
      send_burst();
    }
  }
}

void graphics_pipe_write(uint32_t value, int bytes) { feed_stream(value, bytes); }

// A write to WPAR: the bytes the pipe hadn't sent are dropped, as Dolphin's
// GPFifoManager::ResetGatherPipe drops them. GXBeginDisplayList does it after pointing the
// CPU's FIFO at the list, so a partly filled burst from before never lands in the list:
// with them, a list came out 32 bytes longer than the game had measured it.
void graphics_pipe_reset(void) { burst_used = 0; }

uint64_t graphics_next_due(void) {
  uint64_t token = atomic_load(&token_due), finish = atomic_load(&finish_due);
  if (token && finish) return token < finish ? token : finish;
  return token ? token : finish;
}

uint64_t gx_field_nanoseconds(void) { return atomic_load(&field_length); }

void graphics_retrace(double field_rate) {
  if (field_rate > 1) atomic_store(&field_length, (uint64_t)(1e9 / field_rate));
  atomic_store(&field_began, host_nanoseconds());
  atomic_fetch_add(&fields_begun, 1);
  if (resume_state == WAITING_FOR_RETRACE) resume_state = RESUME_DUE;
  if (held_at_token && ++retraces_held >= HOLD_RETRACES_AT_MOST) token_seen = 1;
#ifdef __APPLE__
  wake_idle();  // A picture held for this field goes now, however late the Mac's timers run.
#endif
}

void graphics_update(void) {
  if (held_at_token && token_seen) {
    held_at_token = 0;
    pump();
  }
  if (resume_state == RESUME_DUE) {
    resume_state = RUNNING;
    pump();
  }
  // The game's acknowledgement rewrites the whole status register, so a signal
  // sent while another is waiting would be wiped. One at a time.
  if (pe_status & (PE_TOKEN_DONE | PE_FINISH_DONE)) return;
  // The other thread can set either at any moment, so each is taken only if unchanged.
  uint64_t token = atomic_load(&token_due);
  if (token && time_base() >= token && atomic_compare_exchange_strong(&token_due, &token, 0)) {
    pe_token = atomic_load(&token_parsed);
    raise_pe(PE_TOKEN_DONE, 1, IRQ_PE_TOKEN);
    return;
  }
  uint64_t finish = atomic_load(&finish_due);
  if (finish && time_base() >= finish && atomic_compare_exchange_strong(&finish_due, &finish, 0)) {
    raise_pe(PE_FINISH_DONE, 2, IRQ_PE_FINISH);
  }
}

void gx_stream_report(void) {
  fprintf(stderr, "stream: %u bytes waiting, control %x, read %x, write %x, cpu write %x:", stream_used,
          hw_peek(CP_CONTROL, 2), read_pair(CP_FIFO_READ), read_pair(CP_FIFO_WRITE), hw_peek(PI_FIFO_WRITE, 4));
  for (uint32_t i = 0; i < 24 && i < stream_used; i++) fprintf(stderr, " %02x", stream[i]);
  fprintf(stderr, "\n  held: at breakpoint %d, at token %d (seen %d, retraces %d, paces by token %d), resume state %d, threaded %d",
          at_breakpoint, held_at_token, token_seen, retraces_held, paces_by_token, (int)resume_state, threaded);
  fprintf(stderr, "\n  %llu bytes handed to the commands thread so far, FIFO %x to %x", (unsigned long long)ring_added,
          read_pair(CP_FIFO_BASE), read_pair(CP_FIFO_END));
  fprintf(stderr, "\n  the CPU's write pipe holds %u bytes not yet sent:", burst_used);
  for (uint32_t i = 0; i < burst_used; i++) fprintf(stderr, " %02x", burst[i]);
  fprintf(stderr, "\n  vertex format %x %x, needs %u for one vertex of format 0\n", gx.cp[0x50], gx.cp[0x60], gx_vertices_size(0, 1));
}

// GXAbortFrame throws away what the chip hasn't drawn yet, as Dolphin's
// ProcessorInterface.cpp does it: the CPU's half-filled burst and the commands read but
// not run are dropped, and the chip's FIFO control goes back to linked, with reads and
// interrupts off and the marks at their limits. Commands already handed to the commands
// thread count as drawn, as this chip is never behind by more.
static void fifo_reset(void) {
  burst_used = 0;
  stream_used = 0;
  at_breakpoint = 0;
  resume_state = RUNNING;
  fifo_flags = 0;
  hw_poke(CP_CONTROL, CONTROL_LINK, 2);
  write_pair(CP_LOW_MARK, 0);
  write_pair(CP_HIGH_MARK, PI_ADDRESS_MASK & ~31u);
  clear_irq(IRQ_CP);
}

int graphics_write(uint32_t address, uint32_t value, int bytes) {
  if (address == PI_FIFO_RESET) {
    if (value & FIFO_RESET) fifo_reset();
    return 1;
  }
  if (address >= PIPE_FIRST && address <= PIPE_LAST) {
    feed_stream(value, bytes);
    return 1;
  }
  if (address >= CP_CONTROL && address < CP_REGISTERS_END) {
    if (address == CP_CLEAR) {
      fifo_flags &= ~value;
      if (!fifo_flags && !at_breakpoint) clear_irq(IRQ_CP);
      return 1;
    }
    hw_poke(address, value, bytes);
    // The handler lifts the breakpoint before its callback sets the next one. A real
    // chip barely moves between the two, and takes milliseconds over a frame, so the
    // game sees each frame's token at a retrace. This chip is instant, so it waits for
    // that retrace, or it would run past the next stop and overwrite the token.
    if (address == CP_CONTROL && !(value & CONTROL_BREAK_ENABLE) && at_breakpoint) {
      at_breakpoint = 0;
      resume_state = WAITING_FOR_RETRACE;
      clear_irq(IRQ_CP);
      return 1;
    }
    if (address == CP_CONTROL && !(value & CONTROL_READ_ENABLE)) return 1;
    resume_state = RUNNING;
    pump();
    return 1;
  }
  if (address == PE_STATUS) {
    pe_enabled = value & PE_ENABLE_BITS;
    pe_status &= ~(value & (PE_TOKEN_DONE | PE_FINISH_DONE));
    if (value & PE_TOKEN_DONE) clear_irq(IRQ_PE_TOKEN);
    if ((value & PE_TOKEN_DONE) && !atomic_load(&token_due)) pe_token = atomic_load(&token_parsed);  // Nothing on its way: up to date.
    // The SDK's handler acknowledges the token's interrupt once it has read the value and
    // run the game's draw-sync callback. While the chip is held at that token the value
    // can't have moved on, so the game has seen it. Games that pace frames through the
    // callback alone (Mario Power Tennis) never read the token otherwise, and would wait
    // out the hold's last resort, a frame every two seconds.
    if ((value & PE_TOKEN_DONE) && held_at_token && !atomic_load(&token_due)) token_seen = 1;
    if (value & PE_FINISH_DONE) clear_irq(IRQ_PE_FINISH);
    return 1;
  }
  return 0;
}

// How many bytes of the FIFO the chip has still to read, as Dolphin's CommandProcessor.cpp
// works it out when the chip runs on its own thread: across the turn if the writer has
// wrapped. The SDK reads it to tell how full the FIFO is.
static uint32_t fifo_distance(void) {
  uint32_t base = read_pair(CP_FIFO_BASE), limit = ring_limit(read_pair(CP_FIFO_END));
  uint32_t read = read_pair(CP_FIFO_READ), write = read_pair(CP_FIFO_WRITE);
  return write >= read ? write - read : (limit - read) + (write - base);
}

int graphics_read(uint32_t address, uint32_t *value) {
  if (address == CP_DISTANCE_LOW) *value = fifo_distance() & 0xffff;
  else if (address == CP_DISTANCE_HIGH) *value = fifo_distance() >> 16;
  else if (address == CP_STATUS) *value = CP_IDLE | (at_breakpoint ? CP_STATUS_BREAK : 0) | fifo_flags;
  else if (address == PE_STATUS) *value = pe_enabled | pe_status;
  else if (gx_bounding_box_read(address, value)) {}
  else if (address == PE_TOKEN_VALUE) {
    *value = pe_token;
    // The SDK's interrupt handler also reads it, which doesn't count. That read comes
    // with the token's signal still up, and can be late enough to land on a newer token.
    int signal_up = atomic_load(&token_due) || ((pe_status & PE_TOKEN_DONE) && (pe_enabled & 1));
    if (held_at_token && !signal_up) token_seen = 1;
  }
  else return 0;
  return 1;
}
