// The serial interface (si.c): empty ports on the Wii side, and in GameCube mode a
// controller in port 1, polled and asked through the transfer buffer as the SDK's PAD
// library does, with made-up controls.
#include <string.h>
#include "../harness.h"
#include "input.h"

int gamecube_mode;
static int irq_up, gaps, rumble;
void raise_irq(int bit) { CHECK_EQ(bit, 3); irq_up = 1; }
void clear_irq(int bit) { (void)bit; irq_up = 0; }
void gap_hit(const char *id, uint32_t detail) { (void)id; (void)detail; gaps++; }
static PlayerState player;
void input_snapshot(int which, PlayerState *out) { CHECK_EQ(which, 0); *out = player; }
void input_set_outputs(int which, int on, uint8_t lights) { (void)which; (void)lights; rumble = on; }
PlayerOutputs input_outputs(int which) { (void)which; return (PlayerOutputs){0}; }
static uint8_t buffer[0x80];  // The transfer buffer, which hardware.c keeps.
uint32_t hw_peek(uint32_t address, int bytes) { CHECK_EQ(bytes, 1); return buffer[address - 0xcc006480u]; }
void hw_poke(uint32_t address, uint32_t value, int bytes) { CHECK_EQ(bytes, 1); buffer[address - 0xcc006480u] = (uint8_t)value; }
#include "si.c"

enum {
  SI = 0xcc006400u, POLL_AT = SI + 0x30, COMMUNICATION_AT = SI + 0x34, STATUS_AT = SI + 0x38,
  START = 1, IN_3 = 3u << 8, OUT_1 = 1u << 16, DONE = 1u << 31, ERROR = 1u << 29, READ_MASK = 1u << 27,
};

static uint32_t reg(uint32_t address) {
  uint32_t value = 0;
  CHECK(si_read(address, &value));
  return value;
}

static uint32_t transfer(uint32_t channel, uint8_t first) {
  buffer[0] = first;
  si_write(COMMUNICATION_AT, DONE | IN_3 | OUT_1 | channel << 1 | START);
  return reg(COMMUNICATION_AT);
}

static void test_wii_ports_are_empty(void) {
  uint32_t done = transfer(0, 0x00);
  CHECK(done & DONE);
  CHECK(done & ERROR);
  CHECK(reg(STATUS_AT) & 1u << 27);  // No response from port 1.
  si_poll();  // Nothing is polled on the Wii side.
  CHECK_EQ(reg(STATUS_AT) & 1u << 29, 0);
  si_write(STATUS_AT, 0x0f0f0f0fu);
}

static void test_controller_answers(void) {
  uint32_t done = transfer(0, 0x00);
  CHECK_EQ(done & ERROR, 0);
  CHECK_EQ(buffer[0], 0x09);  // A standard controller.
  CHECK_EQ(buffer[1], 0x00);
  transfer(0, 0x41);  // Its origin: centred sticks.
  CHECK_EQ(buffer[2], 0x80);
  CHECK_EQ(buffer[5], 0x80);
  CHECK(transfer(1, 0x00) & ERROR);  // The second port is empty.
  CHECK(reg(STATUS_AT) & 1u << 19);
}

static void test_polling(void) {
  player.buttons = BUTTON_A | BUTTON_PLUS | BUTTON_CLASSIC_ZR;
  player.sticks[0] = 1.0f;   // Full right.
  player.sticks[3] = -1.0f;  // C stick full down.
  si_write(SI, 0x00400300u);  // Direct, mode 3, no rumble.
  si_write(STATUS_AT, 1u << 31);  // Sent while port 1 isn't polled, so the mode takes.
  si_write(POLL_AT, 1u << 7 | 1u << 8);
  si_write(COMMUNICATION_AT, READ_MASK);
  si_poll();
  CHECK(reg(STATUS_AT) & 1u << 29);  // New data in port 1.
  CHECK(irq_up);
  uint32_t high = reg(SI + 4), low = reg(SI + 8);
  CHECK_EQ(high >> 16, 0x0100u | 0x1000u | 0x0020u | 0x0080u);  // A, Start, R, and the origin bit.
  CHECK_EQ(high >> 8 & 0xff, 0xff);
  CHECK_EQ(high & 0xff, 0x80);
  CHECK_EQ(low, 0xffu | 0x80u << 24 | 0x01u << 16);  // R fully pressed; C stick x centred, y down.
  CHECK_EQ(reg(STATUS_AT) & 1u << 29, 0);  // Read, so no longer new.
  CHECK(!irq_up);
  CHECK(reg(SI + 12 + 4) & 1u << 31);  // Port 2: an error, as nothing answers.
  si_write(SI, 0x00400001u);  // Rumble on.
  si_write(STATUS_AT, 1u << 31);
  CHECK(rumble);
  si_write(SI, 0x00400000u);
  si_write(STATUS_AT, 1u << 31);
  CHECK(!rumble);
}

int main(void) {
  test_wii_ports_are_empty();
  gamecube_mode = 1;
  test_controller_answers();
  test_polling();
  CHECK_EQ(gaps, 0);
  return finish();
}
