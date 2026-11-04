// Gaps through the real gaps.c. test_recomp_runtime.py reads what this logs and writes.
#include <string.h>

#include "../harness.h"
#include "gaps.h"

int main(void) {
  cpu.lr = 0x80001234;
  gap_hit("ios.setattr", 0);
  gap_hit("ios.setattr", 0);  // The same again is counted, not logged.
  gap_hit("ios.setattr", 2);  // Another detail is its own line.
  char built[32];
  strcpy(built, "cpu.sc");
  gap_note(built, 5);  // Counted, never logged.
  strcpy(built, "overwritten");  // The ID was copied, so this changes nothing kept.
  gap_note("cpu.sc", 5);
  gap_flush();
  printf("ok\n");
  return 0;
}
