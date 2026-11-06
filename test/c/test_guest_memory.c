// The guest memory window: mirrors, the hardware block, the locked cache, the stack
// accessors, and a guard page a game touches.
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "memory_watch.h"

Cpu cpu;
static int failures;
#define CHECK_EQ(got, want) do { unsigned long long g = (got), w = (want); \
  if (g != w) { printf("FAILED line %d: %s = %llx, wanted %llx\n", __LINE__, #got, g, w); failures++; } } while (0)

static uint32_t hardware_reads, hardware_writes, last_address, last_value;
uint32_t hw_read(uint32_t address, int bytes) { hardware_reads++; last_address = address; return 0x1234u + (uint32_t)bytes; }
void hw_write(uint32_t address, uint32_t value, int bytes) { (void)bytes; hardware_writes++; last_address = address; last_value = value; }

static char gap_id[64];
static uint32_t gap_detail;
static int gap_count;
void gap_hit(const char *id, uint32_t detail) { snprintf(gap_id, sizeof gap_id, "%s", id); gap_detail = detail; gap_count++; }

// Fills a held range when it's first touched, as gx_copy.c does with a copy's bytes.
static uint32_t resolved_at;
static int resolves;
static void fill_held(uint32_t physical) {
  resolved_at = physical;
  resolves++;
  watch_release(0x80400000u, 0x3000u);
  memset(host(0x80400000u), 0x5a, 0x3000u);
}

int main(void) {
  guest_memory_init();

  // MEM1 is the same memory at 0x00, 0x80 and 0xc0, and MEM2 at 0x10, 0x90 and 0xd0.
  wr32(0x80001000u, 0xdeadbeefu);
  CHECK_EQ(rd32(0xc0001000u), 0xdeadbeefu);
  CHECK_EQ(rd32(0x00001000u), 0xdeadbeefu);
  CHECK_EQ(rd8(0x80001001u), 0xad);
  CHECK_EQ(rd16(0x80001002u), 0xbeef);
  CHECK_EQ(mem1[0x1000], 0xde);
  wr64(0x90000010u, 0x0102030405060708ull);
  CHECK_EQ(rd64(0xd0000010u), 0x0102030405060708ull);
  CHECK_EQ(rd32(0x10000014u), 0x05060708u);
  CHECK_EQ(mem2[0x10], 0x01);
  CHECK_EQ(host(0x93ffffffu) - mem2, 0x3ffffffu);

  // The hardware block and the picture memory go to hw_read and hw_write.
  CHECK_EQ(rd32(0xcc003000u), 0x1238u);
  CHECK_EQ(last_address, 0xcc003000u);
  wr16(0xcd006c00u, 0x12345u);
  CHECK_EQ(last_value, 0x2345u);
  CHECK_EQ(rd32(0xc8000000u), 0x1238u);
  CHECK_EQ(hardware_reads, 2);
  CHECK_EQ(hardware_writes, 1);
  CHECK_EQ(host(0xcc003000u) == 0, 1);

  // The locked cache is memory too.
  wr32(0xe0000100u, 0xcafef00du);
  CHECK_EQ(rd32(0xe0000100u), 0xcafef00du);
  CHECK_EQ(host(0xe0003fffu) != 0, 1);
  CHECK_EQ(host(0xe0004000u) == 0, 1);
  CHECK_EQ(hardware_reads, 2);

  // The stack accessors reach the same memory without the check.
  wr32s(0x80002000u, 0x11223344u);
  CHECK_EQ(rd32(0x80002000u), 0x11223344u);
  wr64s(0x80002008u, 0xaabbccddeeff0011ull);
  CHECK_EQ(rd64s(0x80002008u), 0xaabbccddeeff0011ull);
  CHECK_EQ(rd16s(0x80002002u), 0x3344);

  // Past the end of MEM1 a Wii has nothing. The access still works, reading zero and
  // then what was written, and it's named.
  CHECK_EQ(rd32(0x81900000u), 0);
  wr32(0x81900004u, 7);
  CHECK_EQ(rd32(0x81900004u), 7);
  guest_memory_report();
  CHECK_EQ(strcmp(gap_id, "cpu.dsi_isi"), 0);
  CHECK_EQ(gap_detail, 0x01900000u);
  CHECK_EQ(gap_count, 1);
  guest_memory_report();
  CHECK_EQ(gap_count, 1);

  // A watched range keeps its count until something writes there: translated code, the
  // runtime's own copies, or the kernel once told. Neighbouring pages don't count.
  uint64_t watched = watch_range(0x80100000u, 0x2000u);
  CHECK_EQ(watched != WATCH_NONE, 1);
  CHECK_EQ(watch_count(0x80100000u, 0x2000u), watched);
  CHECK_EQ(rd32(0x80100010u), 0);  // Reading is free.
  CHECK_EQ(watch_count(0x80100000u, 0x2000u), watched);
  wr32(0x80300000u, 1);
  CHECK_EQ(watch_count(0x80100000u, 0x2000u), watched);
  wr32(0xc0101ffcu, 0x55667788u);  // Through the uncached mirror, as the chip would see it.
  CHECK_EQ(rd32(0x80101ffcu), 0x55667788u);
  CHECK_EQ(watch_count(0x80100000u, 0x2000u), WATCH_NONE);
  uint64_t again = watch_range(0x80100000u, 0x2000u);
  CHECK_EQ(again != watched && again != WATCH_NONE, 1);
  memset(host(0x80100100u), 9, 4);
  CHECK_EQ(mem1[0x100100], 9);
  CHECK_EQ(watch_count(0x80100000u, 0x2000u) != again, 1);
  again = watch_range(0x80100000u, 0x2000u);
  watch_before_write(host(0x80100000u), 16);
  CHECK_EQ(watch_count(0x80100000u, 0x2000u) != again, 1);
  // A page written frame after frame stops being watched, and its readers look again.
  for (int frame = 0; frame < 6; frame++) {
    watch_range(0x90200000u, 4);
    wr32(0x90200000u, (uint32_t)frame);
    watch_next_frame();
  }
  CHECK_EQ(watch_range(0x90200000u, 4), WATCH_NONE);
  CHECK_EQ(rd32(0x90200000u), 5);
  // Only memory is watched, and a range can't run from one memory into another.
  CHECK_EQ(watch_range(0xcc000000u, 4), WATCH_NONE);
  CHECK_EQ(watch_range(0x817ffff0u, 0x20u), WATCH_NONE);
  CHECK_EQ(gap_count, 1);  // Not a guard page.

  // A held range can't be read or written until its resolver fills it, from whatever
  // touches it first, and then it reads as filled. It counts as written once let go.
  watch_set_resolver(fill_held);
  uint64_t before_hold = watch_range(0x80400000u, 0x3000u);
  CHECK_EQ(watch_hold(0x80400000u, 0x3000u), 1);
  CHECK_EQ(watch_held(0x80401000u, 4), 1);
  CHECK_EQ(watch_held(0x80500000u, 4), 0);
  uint64_t held_count = watch_count(0x80400000u, 0x3000u);
  CHECK_EQ(held_count != before_hold && held_count != WATCH_NONE, 1);
  CHECK_EQ(rd32(0x80401234u), 0x5a5a5a5au);
  CHECK_EQ(resolves, 1);
  CHECK_EQ(resolved_at >> 12, 0x401u);
  CHECK_EQ(watch_held(0x80400000u, 0x3000u), 0);
  CHECK_EQ(rd32(0x80400000u), 0x5a5a5a5au);
  CHECK_EQ(resolves, 1);
  CHECK_EQ(watch_count(0x80400000u, 0x3000u) == held_count, 0);
  // A write goes first through the resolver too, and the kernel's through watch_before_write.
  watch_hold(0x80400000u, 0x3000u);
  wr32(0x80402000u, 0x01020304u);
  CHECK_EQ(resolves, 2);
  CHECK_EQ(rd32(0x80402000u), 0x01020304u);
  CHECK_EQ(rd32(0x80402004u), 0x5a5a5a5au);
  watch_hold(0x80400000u, 0x3000u);
  watch_before_write(host(0x80400010u), 4);
  CHECK_EQ(resolves, 3);
  CHECK_EQ(watch_held(0x80400000u, 0x3000u), 0);
  // With no resolver taking it, a touched page is let go as it is.
  watch_set_resolver(0);
  watch_hold(0x80400000u, 0x1000u);
  CHECK_EQ(rd32(0x80400000u), 0x5a5a5a5au);
  CHECK_EQ(gap_count, 1);

  // Starting over clears memory.
  guest_memory_reserve();
  CHECK_EQ(rd32(0x80001000u), 0);

  if (!failures) printf("ok\n");
  return failures != 0;
}
