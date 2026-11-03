// A module the program links at run time: found by its exports, checked against the
// code that was translated, its imports read from the linked branches, then run natively.
// argv[1]: "match", "changed" (code differs) or "no-native-import".
#include <string.h>
#include "harness.h"
#include "dispatch.h"
#include "../../recomp/runtime/modules.c"

uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
int interrupt_budget;
void poll_interrupts(void) {}
void system_call(uint32_t address) {}
void locked_cache_transfer(void) {}
uint64_t time_base(void) { return 0; }
uint64_t guest_time_base(void) { return 0; }
void guest_time_base_set(int high_half, uint32_t value) {}
void decrementer_set(uint32_t value) {}
uint32_t decrementer_get(void) { return 0; }

enum { HEADER = 0x80100000u, CODE = HEADER + 0xe0, IMPORTED = 0x80200000u, NOT_NATIVE = 0x80300000u };
static int ran_module, ran_import;
static void module_function(void) { ran_module++; }
static void imported(void) { ran_import++; }

static const TitleFunction functions_table[] = {{IMPORTED, imported}};

static uint32_t code_value, section_values[2], import_values[1];
static const ModuleFunction functions[] = {{0, module_function}, {8, module_function}};
static const uint32_t exports[] = {8};
static const uint32_t relocated[] = {4};  // A bl the linker points at the import.
static const ImportSite sites[] = {{1, 4, 0, 0, 0}};
static Module test_module = {"test", 0xe0, 16, 2, 1, functions, 2, exports, 1, relocated, 1, 0, sites, 1,
                             &code_value, section_values, import_values};
static const Module *const test_modules[] = {&test_module};
int apploader_bind(uint32_t address) { return 0; }
void console_hand_over(uint32_t address) {}
const Title *title = &(Title){.functions = functions_table, .function_count = 1, .modules = test_modules, .module_count = 1};

static const uint32_t words[4] = {0x7c0802a6, 0, 0x38600001, 0x4e800020};

static void link(uint32_t target) {
  wr32(HEADER + 8, 2);
  wr32(HEADER + 0xc, HEADER + 0x58);
  wr32(HEADER + 0x58 + 8, CODE);
  for (int i = 0; i < 4; i++) wr32(CODE + i * 4, words[i]);
  wr32(CODE + 4, 0x48000001u | ((target - (CODE + 4)) & 0x3fffffcu));  // bl, as the linker patches it.
}

int main(int count, char **arguments) {
  memory_start();
  fill_code_slots();
  link(IMPORTED);
  test_module.check = code_check(&test_module, CODE);
  if (!strcmp(arguments[1], "changed")) wr32(CODE + 8, 0x38600002);
  if (!strcmp(arguments[1], "no-native-import")) link(NOT_NATIVE);
  setenv("WII_RECOMP_NATIVE_ONLY", "1", 1);
  dispatch(CODE + 8);  // First reached through an export.
  CHECK_EQ(ran_module, 1);
  CHECK_EQ(code_value, CODE);
  CHECK_EQ(section_values[1], CODE);
  CHECK_EQ(import_values[0], IMPORTED);
  dispatch(CODE);  // Every function is ready now, not only the one first reached.
  CHECK_EQ(ran_module, 2);
  return finish();
}
