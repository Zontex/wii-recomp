// The interpreter on its own, through the real dispatch: returns, calls in both
// directions, interrupts, and instructions checked against worked-out answers.
#include "../harness.h"
#include "dispatch.h"
#include "interp.h"

int interrupt_budget;
static int polls, system_calls;
static uint32_t system_call_at, last_decrementer;
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
uint64_t time_base(void) { return 0; }
uint64_t guest_time_base(void) { return 0x1122334455667788ull; }
void guest_time_base_set(int high_half, uint32_t value) {}
void decrementer_set(uint32_t value) { last_decrementer = value; }
uint32_t decrementer_get(void) { return 99; }
void locked_cache_transfer(void) {}
void poll_interrupts(void) { interrupt_budget = 0; polls++; }
void interrupts_turned_on(void) {}
void system_call(uint32_t address) { system_calls++; system_call_at = address; }
void trap_taken(uint32_t address) { (void)address; }

#define TRANSLATED_ADD 0x80001000u
#define TRANSLATED_CALLS_BACK 0x80001100u
#define CODE 0x80100000u
#define CALLED_BACK (CODE + 0x800)
#define STOP 0x80000100u
#define STACK 0x80200000u

// Hand-assembled, as nothing here should depend on the translator.
#define BLR 0x4e800020u
#define BLRL 0x4e800021u
#define BCTR 0x4e800420u
#define BCTRL 0x4e800421u
#define SC 0x44000002u
#define RFI 0x4c000064u
#define LI_R3(v) (0x38600000u | (v))
#define ADD_ONE_R3 0x38630001u
#define LIS_R4(v) (0x3c800000u | (v))
#define ORI_R4(v) (0x60840000u | (v))
#define MTLR_R4 0x7c8803a6u
#define MTLR_R5 0x7ca803a6u
#define MTLR_R6 0x7cc803a6u
#define MTCTR_R4 0x7c8903a6u
#define MFLR_R3 0x7c6802a6u
#define MFLR_R0 0x7c0802a6u
#define MTLR_R0 0x7c0803a6u
#define STW_R0 0x90010004u
#define LWZ_R0 0x80010004u
#define PUSH 0x9421fff0u
#define POP 0x38210010u
#define MTSRR0_R4 0x7c9a03a6u
#define MTSRR1_R5 0x7cbb03a6u

static uint32_t branch(uint32_t from, uint32_t to, uint32_t link) { return 0x48000000u | ((to - from) & 0x3fffffcu) | link; }

static void put(uint32_t address, const uint32_t *words, int count) {
  for (int i = 0; i < count; i++) wr32(address + (uint32_t)i * 4, words[i]);
}
#define PUT(address, ...) do { const uint32_t code[] = {__VA_ARGS__}; put(address, code, sizeof code / 4); } while (0)

static void translated_add(void) { cpu.r[3]++; }

// Translated code that calls back into code with no translation.
static void translated_calls_back(void) {
  cpu.lr = 0x80001104u;
  dispatch(CALLED_BACK);
  cpu.r[3] += 100;
}

#include "title.h"
static const TitleFunction functions_table[] = {{TRANSLATED_ADD, translated_add}, {TRANSLATED_CALLS_BACK, translated_calls_back}};
int apploader_bind(uint32_t address) { return 0; }
void console_hand_over(uint32_t address) {}
const Title *title = &(Title){.functions = functions_table, .function_count = 2};

static void reset(void) {
  memset(&cpu, 0, sizeof cpu);
  cpu.lr = STOP;
  cpu.r[1] = STACK;
  polls = system_calls = 0;
  interrupt_budget = 0;
}

static void test_return(void) {
  reset();
  PUT(CODE, LI_R3(7), BLR, LI_R3(8));
  dispatch(CODE);
  CHECK_EQ(cpu.r[3], 7);
  CHECK(translated_at(CODE) == 0);
  CHECK(translated_at(TRANSLATED_ADD) == translated_add);
}

static void test_calls_translated_and_is_called_back(void) {
  reset();
  // A frame, a call to translated code, and a call that comes back here.
  PUT(CODE, MFLR_R0, STW_R0, PUSH, LI_R3(1),
      branch(CODE + 16, TRANSLATED_ADD, 1),
      branch(CODE + 20, TRANSLATED_CALLS_BACK, 1),
      ADD_ONE_R3, POP, LWZ_R0, MTLR_R0, BLR);
  PUT(CALLED_BACK, ADD_ONE_R3, ADD_ONE_R3, BLR);
  dispatch(CODE);
  CHECK_EQ(cpu.r[3], 1 + 1 + 2 + 100 + 1);
  CHECK_EQ(cpu.lr, STOP);
  CHECK_EQ(cpu.r[1], STACK);
}

// Setting the link register by hand is a jump, not a return.
static void test_link_register_jump_keeps_running(void) {
  reset();
  uint32_t target = CODE + 0x100;
  cpu.r[5] = STOP;
  PUT(CODE, LIS_R4(target >> 16), ORI_R4(target & 0xffff), MTLR_R4, BLR, LI_R3(1));
  PUT(target, LI_R3(2), MTLR_R5, BLR);
  dispatch(CODE);
  CHECK_EQ(cpu.r[3], 2);
}

// Entered by a jump after a call, so the link register on entry was stale. The code
// restores the real one as it pops its frame, and that blr is still a return.
static void test_popped_frame_means_return(void) {
  reset();
  uint32_t elsewhere = CODE + 0x100;
  cpu.r[6] = elsewhere;
  cpu.r[1] = STACK - 16;
  PUT(elsewhere, LI_R3(99), BLR);
  PUT(CODE, POP, MTLR_R6, BLR);
  dispatch(CODE);
  CHECK(cpu.r[3] != 99);
  CHECK_EQ(cpu.r[1], STACK);
}

static void test_reading_own_address(void) {
  reset();
  PUT(CODE, MFLR_R0, STW_R0, branch(CODE + 8, CODE + 12, 1), MFLR_R3, LWZ_R0, MTLR_R0, BLR);
  dispatch(CODE);
  CHECK_EQ(cpu.r[3], CODE + 12);
}

static void test_back_branches_poll(void) {
  reset();
  // li r4,5; mtctr r4; bdnz .; blr
  PUT(CODE, 0x38800005u, MTCTR_R4, 0x42000000u, BLR);
  dispatch(CODE);
  CHECK_EQ(polls, 5);  // One on entry and one for each of the four times round.
  CHECK_EQ(cpu.ctr, 0);
}

static void test_system_call(void) {
  reset();
  PUT(CODE, SC, LI_R3(4), BLR);
  dispatch(CODE);
  CHECK_EQ(system_calls, 1);
  CHECK_EQ(system_call_at, CODE);
  CHECK_EQ(cpu.r[3], 4);
}

static void test_return_from_interrupt(void) {
  reset();
  uint32_t target = CODE + 0x100;
  cpu.r[5] = 0x8000;
  PUT(CODE, LIS_R4(target >> 16), ORI_R4(target & 0xffff), MTSRR0_R4, MTSRR1_R5, RFI, LI_R3(1));
  PUT(target, LI_R3(2), BLR);
  dispatch(CODE);
  CHECK_EQ(cpu.r[3], 2);
  CHECK_EQ(cpu.msr, 0x8000);
}

static void test_jumps(void) {
  reset();
  PUT(CODE, branch(CODE, TRANSLATED_ADD, 0), LI_R3(50));  // A tail call into translated code.
  dispatch(CODE);
  CHECK_EQ(cpu.r[3], 1);

  reset();
  uint32_t target = CODE + 0x100;
  cpu.r[5] = STOP;
  // bctrl changes the link register, so it must be put back before the last blr.
  PUT(CODE, LIS_R4(target >> 16), ORI_R4(target & 0xffff), MTCTR_R4, BCTRL, MTLR_R5, ADD_ONE_R3, BLR);
  PUT(target, LI_R3(3), BLR);
  dispatch(CODE);
  CHECK_EQ(cpu.r[3], 4);

  reset();
  PUT(CODE, LIS_R4(target >> 16), ORI_R4(target & 0xffff), MTCTR_R4, BCTR, LI_R3(9));  // An untranslated tail call.
  dispatch(CODE);
  CHECK_EQ(cpu.r[3], 3);

  reset();
  cpu.r[5] = STOP;
  PUT(CODE, LIS_R4(target >> 16), ORI_R4(target & 0xffff), MTLR_R4, BLRL, MTLR_R5, ADD_ONE_R3, BLR);
  dispatch(CODE);
  CHECK_EQ(cpu.r[3], 4);
}

static void run_one(uint32_t word) { interpret_instruction(CODE, word); }

static void test_division(void) {
  reset();
  cpu.r[4] = 0x80000000u, cpu.r[5] = 0xffffffffu;
  run_one(0x7c642bd6u);  // divw r3,r4,r5
  CHECK_EQ(cpu.r[3], 0xffffffffu);  // Overflow, negative dividend.
  cpu.r[4] = 0xfffffffbu, cpu.r[5] = 0;
  run_one(0x7c642bd6u);
  CHECK_EQ(cpu.r[3], 0xffffffffu);  // By zero, negative dividend.
  cpu.r[4] = 5;
  run_one(0x7c642bd6u);
  CHECK_EQ(cpu.r[3], 0);            // By zero, positive dividend.
  cpu.r[4] = 0xfffffffbu, cpu.r[5] = 2;
  run_one(0x7c642bd6u);
  CHECK_EQ(cpu.r[3], 0xfffffffeu);  // -5 / 2 goes towards zero.
  cpu.r[4] = 5, cpu.r[5] = 0;
  run_one(0x7c642b96u);             // divwu
  CHECK_EQ(cpu.r[3], 0);
  run_one(0x7c642f96u);             // divwuo
  CHECK_EQ(cpu.ov, 1);
  CHECK_EQ(cpu.so, 1);
  cpu.r[5] = 5;
  run_one(0x7c642f96u);
  CHECK_EQ(cpu.ov, 0);
  CHECK_EQ(cpu.so, 1);
}

static void test_strings(void) {
  reset();
  uint32_t at = 0x80101000u;
  PUT(at, 0x11223344u, 0x55667788u, 0x99aabbccu);
  cpu.r[3] = at;
  cpu.r[6] = 0xffffffffu;
  run_one(0x7ca32cAAu);  // lswi r5,r3,5
  CHECK_EQ(cpu.r[5], 0x11223344u);
  CHECK_EQ(cpu.r[6], 0x55000000u);  // The rest of the last register is cleared.

  run_one(0x7fe344aau);  // lswi r31,r3,8 wraps round to r0.
  CHECK_EQ(cpu.r[31], 0x11223344u);
  CHECK_EQ(cpu.r[0], 0x55667788u);

  cpu.r[7] = 0xdeadbeefu;
  cpu.xer_string = 0;
  cpu.r[4] = 0;
  run_one(0x7ce3242au);  // lswx r7,r3,r4 with a count of zero changes nothing.
  CHECK_EQ(cpu.r[7], 0xdeadbeefu);
  cpu.xer_string = 2;
  run_one(0x7ce3242au);
  CHECK_EQ(cpu.r[7], 0x11220000u);

  cpu.r[5] = 0xa1a2a3a4u, cpu.r[6] = 0xb1b2b3b4u;
  uint32_t out = 0x80101100u;
  cpu.r[3] = out;
  PUT(out, 0, 0);
  run_one(0x7ca32dAAu);  // stswi r5,r3,5
  CHECK_EQ(rd32(out), 0xa1a2a3a4u);
  CHECK_EQ(rd32(out + 4), 0xb1000000u);
  cpu.xer_string = 3;
  PUT(out, 0);
  run_one(0x7ca3252au);  // stswx r5,r3,r4
  CHECK_EQ(rd32(out), 0xa1a2a300u);
}

static void test_condition_and_status(void) {
  reset();
  cpu.so = 1, cpu.ov = 1, cpu.ca = 1;
  run_one(0x7d800400u);  // mcrxr cr3
  CHECK_EQ(cpu.cr[3], 14);
  CHECK_EQ(cpu.so | cpu.ov | cpu.ca, 0);

  cpu.fpscr = 0x90000000u;
  cpu.f[1] = 1.0;
  run_one(0xfc000a11u);  // fmr. f0,f1
  CHECK_EQ(cpu.cr[1], 9);

  cpu.fpscr = 0;
  cpu.p[2] = 5.0;
  cpu.f[3] = 2.0;
  run_one(0xfc401818u);  // frsp f2,f3
  CHECK(cpu.f[2] == 2.0 && cpu.p[2] == 2.0);

  cpu.f[4] = 4.0;
  run_one(0xec402030u);  // fres f2,f4
  CHECK(cpu.f[2] == 0.25 && cpu.p[2] == 0.25);

  run_one(0x7c7602a6u);  // mfspr r3,DEC
  CHECK_EQ(cpu.r[3], 99);
  cpu.r[3] = 1234;
  run_one(0x7c7603a6u);  // mtspr DEC,r3
  CHECK_EQ(last_decrementer, 1234);
  run_one(0x7c6c42e6u);  // mftb r3
  CHECK_EQ(cpu.r[3], 0x55667788u);
  run_one(0x7c6d42e6u);  // mftbu r3
  CHECK_EQ(cpu.r[3], 0x11223344u);
}

int main(int count, char **arguments) {
  memory_start();
  fill_code_slots();
  if (count > 1) {
    reset();
    if (!strcmp(arguments[1], "null-call")) dispatch(0);
    if (!strcmp(arguments[1], "hardware-call")) dispatch(0xcc003000u);
    if (!strcmp(arguments[1], "unknown")) dispatch(CODE);  // Memory holds zeros.
    if (!strcmp(arguments[1], "logs")) {
      PUT(CODE, BLR);
      PUT(CODE + 8, BLR);
      dispatch(CODE);
      dispatch(CODE);
      dispatch(CODE + 8);
      dispatch(CODE);
      printf("ok\n");
    }
    return 0;
  }
  test_return();
  test_calls_translated_and_is_called_back();
  test_link_register_jump_keeps_running();
  test_popped_frame_means_return();
  test_reading_own_address();
  test_back_branches_poll();
  test_system_call();
  test_return_from_interrupt();
  test_jumps();
  test_division();
  test_strings();
  test_condition_and_status();
  return finish();
}
