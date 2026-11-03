#define _GNU_SOURCE
// Reads the stream of commands the game sends to the graphics chip.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "cpu.h"
#include "debug.h"
#include "title.h"
#include "gx.h"
#include "hardware.h"

enum {
  COMMAND_NOP = 0x00, COMMAND_CP = 0x08, COMMAND_XF = 0x10, COMMAND_XF_INDEXED_A = 0x20,
  COMMAND_XF_INDEXED_D = 0x38, COMMAND_CALL_LIST = 0x40, COMMAND_INVALIDATE = 0x48, COMMAND_BP = 0x61,
  COMMAND_DRAW = 0x80, COMMAND_DRAW_LAST = 0xbf,
};
enum {
  BP_MASK = 0xfe, BP_DRAW_DONE = 0x45, BP_TOKEN = 0x47, BP_TOKEN_NOW = 0x48, BP_COPY = 0x52,
  BP_LOAD_PALETTE = 0x65, BP_PIXEL_FORMAT = 0x43, PIXEL_FORMAT_MASK = 7, PIXEL_FORMAT_DEPTH = 3, BP_TEV_REGISTER = 0xe0, TEV_KONST_FLAG = 0x800000,
};
enum {
  PIPE_FIRST = 0xcc008000u, PIPE_LAST = 0xcc008fffu, CP_STATUS = 0xcc000000u, PE_STATUS = 0xcc00100au,
  PE_TOKEN_VALUE = 0xcc00100eu, CP_IDLE = 0x0c, PE_ENABLE_BITS = 3, PE_TOKEN_DONE = 4, PE_FINISH_DONE = 8,
  COPY_CLEAR_BIT = 11, COPY_TO_SCREEN_BIT = 14,
  PI_FIFO_BASE = 0xcc00300cu, PI_FIFO_END = 0xcc003010u, PI_FIFO_WRITE = 0xcc003014u, PI_ADDRESS_MASK = 0x1fffffffu,
  CP_CONTROL = 0xcc000002u, CP_FIFO_BASE = 0xcc000020u, CP_FIFO_END = 0xcc000024u,
  CP_FIFO_WRITE = 0xcc000034u, CP_FIFO_READ = 0xcc000038u, CP_REGISTERS_END = 0xcc000040u,
  CONTROL_READ_ENABLE = 1, CONTROL_INTERRUPT_ENABLE = 2, CONTROL_LINK = 0x10, CONTROL_BREAK_ENABLE = 0x20,
  MSR_INTERRUPTS = 0x8000,
  CP_BREAKPOINT = 0xcc00003cu, CP_STATUS_BREAK = 0x10, CP_STATUS_OVERFLOW = 1, CP_STATUS_UNDERFLOW = 2,
  CONTROL_OVERFLOW_ENABLE = 4, CONTROL_UNDERFLOW_ENABLE = 8, CP_CLEAR = 0xcc000004u, CP_HIGH_MARK = 0xcc000028u, CP_LOW_MARK = 0xcc00002cu,
};
enum { MAX_UNDRAWN_VERTICES = 3 };
#define STREAM_BYTES (4u * 1024u * 1024u)
#define BURST_BYTES 32u
#define CP_ARRAY_BASE 0xa0
#define CP_ARRAY_STRIDE 0xb0
#define XF_INDEXED_FIRST_ARRAY 12
// The game finishes its bookkeeping just after asking, as a real chip takes time,
// so "finished" and "token reached" must not arrive at once.
#define SIGNAL_DELAY_TICKS 12000u

GxState gx;
GxStats gx_stats;

static uint8_t stream[STREAM_BYTES];
static uint32_t stream_used;
static uint8_t burst[BURST_BYTES];
static uint32_t burst_used;
static uint32_t pe_enabled, pe_status, pe_token;
static _Atomic uint64_t finish_due, token_due;
static int at_breakpoint;
static uint32_t fifo_flags;  // Overflow and underflow, until the game clears them.
// Games that drive the chip with breakpoints check at a retrace that the frame token is
// exactly the next one, as a real chip takes that long over a frame. They may check only
// every other retrace, at 30 fps. This chip is instant, so it pauses after each token
// until the game has read it, or a few retraces pass if it never does.
enum { HOLD_RETRACES_AT_MOST = 120 };  // A last resort: a retrace can pass while interrupts are off.
static int paces_by_token, held_at_token, token_seen, retraces_held;
static enum { RUNNING, WAITING_FOR_RETRACE, RESUME_DUE } resume_state;

static uint32_t big32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t big16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }

static void raise_pe(uint32_t done, uint32_t enable, int irq) {
  pe_status |= done;
  if (pe_enabled & enable) raise_irq(irq);
}

static void write_bp(uint32_t value) {
  uint32_t reg = value >> 24, data = value & 0xffffff;
  if (reg == BP_MASK) {
    gx.bp[BP_MASK] = data;
    return;
  }
  uint32_t mask = gx.bp[BP_MASK] ? gx.bp[BP_MASK] : 0xffffff;
  gx.bp[reg] = (gx.bp[reg] & ~mask) | (data & mask);
  gx.bp[BP_MASK] = 0;
  if (reg >= BP_TEV_REGISTER && reg < BP_TEV_REGISTER + 8) {
    uint32_t *bank = data & TEV_KONST_FLAG ? gx.tev_konst : gx.tev_color;
    bank[reg - BP_TEV_REGISTER] = gx.bp[reg];
  }
  if (reg == BP_DRAW_DONE) {
    gx_stats.draw_done++;
    finish_due = time_base() + SIGNAL_DELAY_TICKS;
  }
  else if (reg == BP_TOKEN || reg == BP_TOKEN_NOW) {
    pe_token = data & 0xffff;
    if (reg == BP_TOKEN) token_due = time_base() + SIGNAL_DELAY_TICKS;
    if (reg == BP_TOKEN && paces_by_token) {
      held_at_token = 1;
      token_seen = retraces_held = 0;
    }
  } else if (reg == BP_LOAD_PALETTE) {
    gx_load_palette();
  } else if (reg == BP_COPY) {
    gx_stats.copies++;
    if (data & (1u << COPY_TO_SCREEN_BIT)) {
      static uint64_t last_frame;
      uint64_t frame_time = time_base();
      float gap = last_frame ? (float)(frame_time - last_frame) / 60750.0f : 0;  // 60,750 ticks to the millisecond.
      if (gap > gx_stats.worst_frame_ms) gx_stats.worst_frame_ms = gap;
      last_frame = frame_time;
      stage_frame(gap);
      gx_stats.to_screen++;
    }
    // The chip copies the picture out first, then clears for the next frame.
    if (data & (1u << COPY_TO_SCREEN_BIT)) {
      uint64_t present_start = time_base();
      gx_present();
      stage_add(STAGE_PRESENT, time_base() - present_start);
    }
    else gx_copy_to_texture(data, (gx.bp[BP_PIXEL_FORMAT] & PIXEL_FORMAT_MASK) == PIXEL_FORMAT_DEPTH);
    if (data & (1u << COPY_CLEAR_BIT)) gx_clear();
  }
}

static void load_indexed(int command, uint32_t value) {
  int array = XF_INDEXED_FIRST_ARRAY + (command - COMMAND_XF_INDEXED_A) / 8;
  uint32_t index = value >> 16, count = ((value >> 12) & 15) + 1, address = value & 0xfff;
  uint32_t source = (gx.cp[CP_ARRAY_BASE + array] + index * (gx.cp[CP_ARRAY_STRIDE + array] & 0xff)) | 0x80000000u;
  for (uint32_t i = 0; i < count && address + i < XF_SIZE; i++) gx.xf[address + i] = rd32(source + i * 4);
}

// Parses one command, returns its bytes, or 0 if it isn't all here yet.
static uint32_t parse_one(const uint8_t *p, uint32_t left) {
  int command = p[0];
  if (command == COMMAND_NOP || command == COMMAND_INVALIDATE) return 1;
  if (command == COMMAND_BP) {
    if (left < 5) return 0;
    write_bp(big32(p + 1));
    return 5;
  }
  if (command == COMMAND_CP) {
    if (left < 6) return 0;
    gx.cp[p[1]] = big32(p + 2);
    return 6;
  }
  if (command == COMMAND_XF) {
    if (left < 5) return 0;
    uint32_t count = big16(p + 1) + 1, address = big16(p + 3), bytes = 5 + count * 4;
    if (left < bytes) return 0;
    for (uint32_t i = 0; i < count && address + i < XF_SIZE; i++) gx.xf[address + i] = big32(p + 5 + i * 4);
    return bytes;
  }
  if (command >= COMMAND_XF_INDEXED_A && command <= COMMAND_XF_INDEXED_D && !(command & 7)) {
    if (left < 5) return 0;
    load_indexed(command, big32(p + 1));
    return 5;
  }
  if (command == COMMAND_CALL_LIST) {
    if (left < 9) return 0;
    uint8_t *list = host(big32(p + 1) | 0x80000000u);
    if (list) gx_parse(list, big32(p + 5));
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
        gx_draw(primitive, gx_read_vertices(p + 3, format, count, gx_draw_reserve(count)));
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
// tokens and draw done. So on Linux commands go to a thread of their own, through a ring
// mapped twice in a row, which lets a command crossing the end read as one piece. A game
// using FIFO breakpoints needs the chip in step with it, and stays on this thread.
static int threaded = -1;
#ifdef __linux__
#define RING_BYTES (16u * 1024u * 1024u)
#define EMPTY_TURNS_BEFORE_SLEEP 200
#define EMPTY_SLEEP_NANOSECONDS 50000
static uint8_t *ring;
static _Atomic uint64_t ring_written, ring_parsed;

static void wait_briefly(void) {
  struct timespec pause = {0, EMPTY_SLEEP_NANOSECONDS};
  nanosleep(&pause, 0);
}

static void *commands_thread(void *unused) {
  (void)unused;
  uint64_t parsed = 0;
  int empty_turns = 0;
  while (1) {
    uint64_t written = atomic_load_explicit(&ring_written, memory_order_acquire);
    uint32_t used = written == parsed ? 0 : parse_one(ring + (parsed & (RING_BYTES - 1)), (uint32_t)(written - parsed));
    if (used) {
      parsed += used;
      atomic_store_explicit(&ring_parsed, parsed, memory_order_release);
      empty_turns = 0;
    } else if (++empty_turns < EMPTY_TURNS_BEFORE_SLEEP) {
      sched_yield();
    } else {
      wait_briefly();
    }
  }
  return 0;
}

static int start_commands_thread(void) {
  int file = memfd_create("wii-recomp-commands", 0);
  if (file < 0 || ftruncate(file, RING_BYTES)) return 0;
  uint8_t *both = mmap(0, RING_BYTES * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (both == MAP_FAILED || mmap(both, RING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, file, 0) == MAP_FAILED
      || mmap(both + RING_BYTES, RING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, file, 0) == MAP_FAILED) {
    return 0;
  }
  ring = both;
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, 16u * 1024u * 1024u);
  pthread_t thread;
  return pthread_create(&thread, &attributes, commands_thread, 0) == 0;
}

int graphics_busy(void) {
  return threaded > 0 && atomic_load_explicit(&ring_parsed, memory_order_acquire) != atomic_load_explicit(&ring_written, memory_order_relaxed);
}

static void ring_add(const uint8_t *bytes, uint32_t count) {
  uint64_t written = atomic_load_explicit(&ring_written, memory_order_relaxed);
  while (written + count - atomic_load_explicit(&ring_parsed, memory_order_acquire) > RING_BYTES) wait_briefly();
  memcpy(ring + (written & (RING_BYTES - 1)), bytes, count);
  atomic_store_explicit(&ring_written, written + count, memory_order_release);
}
#else
static int start_commands_thread(void) { return 0; }
int graphics_busy(void) { return 0; }
static void ring_add(const uint8_t *bytes, uint32_t count) { (void)bytes; (void)count; }
#endif

static void run_stream(void) {
  uint32_t at = 0;
  while (at < stream_used && !held_at_token) {
    uint32_t used = parse_one(stream + at, stream_used - at);
    if (!used) break;
    at += used;
  }
  if (at) {
    memmove(stream, stream + at, stream_used - at);
    stream_used -= at;
  }
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
  int breaking = title->fifo_breakpoints && (control & CONTROL_BREAK_ENABLE);
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
  for (int i = bytes - 1; i >= 0; i--) {
    burst[burst_used++] = (uint8_t)(value >> (i * 8));
    if (burst_used == BURST_BYTES) {
      burst_used = 0;
      send_burst();
    }
  }
}

uint64_t graphics_next_due(void) {
  uint64_t token = atomic_load(&token_due), finish = atomic_load(&finish_due);
  if (token && finish) return token < finish ? token : finish;
  return token ? token : finish;
}

void graphics_retrace(void) {
  if (resume_state == WAITING_FOR_RETRACE) resume_state = RESUME_DUE;
  if (held_at_token && ++retraces_held >= HOLD_RETRACES_AT_MOST) token_seen = 1;
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
  fprintf(stderr, "\n  vertex format %x %x, needs %u for one vertex of format 0\n", gx.cp[0x50], gx.cp[0x60], gx_vertices_size(0, 1));
}

int graphics_write(uint32_t address, uint32_t value, int bytes) {
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
    if (value & PE_FINISH_DONE) clear_irq(IRQ_PE_FINISH);
    return 1;
  }
  return 0;
}

int graphics_read(uint32_t address, uint32_t *value) {
  if (address == CP_STATUS) *value = CP_IDLE | (at_breakpoint ? CP_STATUS_BREAK : 0) | fifo_flags;
  else if (address == PE_STATUS) *value = pe_enabled | pe_status;
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
