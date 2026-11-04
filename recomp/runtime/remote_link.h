// Real Wii Remotes and Balance Boards, passed straight through to the game. A backend
// (remote_link_linux.c, remote_link_mac.c) finds the devices and moves their HID reports;
// this core gives each one a slot of the virtual Bluetooth chip (bluetooth.c), where the
// game's own Wii Remote code drives it as on a Wii: pointer, motion, attachments,
// speaker, rumble and the board's sensors, with nothing simulated.
//
// Two threads meet here. A backend's threads attach devices and hand in their reports;
// the emulation thread takes the reports and sends the game's. Each slot's reports go
// through a ring with one writer and one reader, so neither waits for the other.
//
// WII_RECOMP_REMOTES chooses the devices:
//   auto (the default)  real ones when they're there, the simulated remote and board until then
//   real                real ones only
//   simulated           the simulated ones only
//   loopback            the simulated ones, but behind this link, as if they were real
#pragma once
#include <stdint.h>

#include "bluetooth.h"

enum { LINK_REPORT_BYTES = 23 };  // The longest report, 22 bytes, and a spare.
enum { LINK_AUTO, LINK_REAL, LINK_SIMULATED, LINK_LOOPBACK };
enum { LINK_UNKNOWN, LINK_REMOTE, LINK_BOARD };

// How the core reaches a device. write gets a whole report, starting with its ID, and
// returns -1 if the device is gone. It's called on the emulation thread, and on the
// backend's thread inside link_attach and link_input while the core asks what the device
// is. close is called on the emulation thread; once it returns the backend calls nothing
// more for that device. Its power_off is set when the game hung up: it returns 1 if the
// device turned off as on a Wii, or 0 if it couldn't, and then the device sleeps until a
// button is pressed.
typedef struct {
  int (*write)(void *device, const uint8_t *report, uint32_t bytes);
  int (*close)(void *device, int power_off);
} LinkOps;

// The emulation thread's side.
void link_start(void);       // Reads WII_RECOMP_REMOTES and starts the backend.
int link_mode(void);
int link_linked(int slot);   // A real (or loopback) device holds this slot.
int link_kind(int slot);
int link_poll(int slot, uint8_t *report);  // The next report from the device, or 0.
void link_send(int slot, const uint8_t *report, uint32_t bytes);
int link_take_wake(int slot);   // The device just linked or a button woke it: it calls the console.
int link_take_lost(int slot);   // The device went away since the last call.
void link_guest_disconnected(int slot);  // The game hung up: the device powers off.
void link_guest_searching(void);         // The game is looking for devices to pair.
void link_update(uint64_t now);          // Gives up on devices that never said what they are.
void link_prepare_handover(void);        // Before exec: linked devices carry on in the next title.

// The backend's side, on any thread.
// link_attach returns the device's id for the other two, or -1 if too many are linked.
// A name such as "Nintendo RVL-WBC-01" says what it is; with none, the core asks it.
int link_attach(const uint8_t address[6], const char *name, const LinkOps *ops, void *device);
void link_input(int id, const uint8_t *report, uint32_t bytes);  // From the device, starting with its ID.
void link_lost(int id);
const char *link_name_of(int id);  // "Nintendo RVL-CNT-01" or "-WBC-01" once known, else "".

// Each backend has these three.
void link_backend_start(int mode);
void link_backend_search(void);       // Look for devices waiting to pair (1+2 or SYNC).
void link_backend_handover(void);     // Keep linked devices across exec.

// The loopback devices (remote_link_loopback.c).
void link_loopback_start(void);

// Slot choice, for tests: a slot pinned to the address, else the first free one of its kind.
int link_choose_slot(const uint8_t address[6], int kind);
