// The link core (remote_link.h): which real device holds which slot, and the reports
// between them and the game.
//
// A device the backend can't name is asked what it is, as Dolphin's IsBalanceBoard does:
// the board answers 04 02 from its attachment register 0xa400fe, which a remote doesn't.
// The board always takes slot 4, where the game looks for it. A remote takes the slot it
// had last time, which remotes.txt remembers by address, so player 1 stays player 1.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "remote_link.h"

enum { ENTRIES = 8, RING = 64, RING_FOR_DATA = 48, FIRST_DATA_REPORT = 0x30, TICKS_PER_SECOND = 60750000 };
enum { FREE, PROBING, DECIDING, LINKED, ASLEEP, LOST };
enum { PROBE_INITIALISE, PROBE_UNLOCK, PROBE_READ };  // Waiting for the answer to each step.
enum { OUTPUT_WRITE = 0x16, OUTPUT_READ = 0x17, REPORT_MEMORY = 0x21, REPORT_ACK = 0x22, REPORT_STATUS = 0x20 };

typedef struct {
  _Atomic int state;
  int kind, slot, probe_step;
  uint64_t probe_started;
  uint8_t address[6];
  const LinkOps *ops;
  void *device;
  uint8_t reports[RING][LINK_REPORT_BYTES];
  uint8_t sizes[RING];
  _Atomic unsigned head, tail;  // The backend writes at head; the emulation thread reads at tail.
  _Atomic int wake;
} Entry;

static Entry entries[ENTRIES];
static _Atomic int slot_entry[BLUETOOTH_DEVICES] = {-1, -1, -1, -1, -1};
static _Atomic int slot_lost[BLUETOOTH_DEVICES];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int mode = -1;

int link_mode(void) {
  if (mode < 0) {
    const char *chosen = getenv("WII_RECOMP_REMOTES");
    mode = !chosen || !strcmp(chosen, "auto") ? LINK_AUTO : !strcmp(chosen, "real") ? LINK_REAL
         : !strcmp(chosen, "simulated") ? LINK_SIMULATED : !strcmp(chosen, "loopback") ? LINK_LOOPBACK : -2;
    if (mode == -2) {
      fprintf(stderr, "stop: WII_RECOMP_REMOTES is auto, real, simulated or loopback, not %s\n", chosen);
      exit(1);
    }
  }
  return mode;
}

void link_start(void) {
  static int started;
  if (started) return;
  started = 1;
  if (link_mode() == LINK_LOOPBACK) link_loopback_start();
  else if (link_mode() != LINK_SIMULATED) link_backend_start(link_mode());
}

// --- Slots, and the file that remembers them ---

static const char *slots_file(void) {
  const char *path = getenv("WII_RECOMP_REMOTE_SLOTS");
  return path ? path : "recomp/build/remotes.txt";
}

static void format_address(const uint8_t address[6], char *text) {
  snprintf(text, 18, "%02x:%02x:%02x:%02x:%02x:%02x", address[0], address[1], address[2], address[3], address[4], address[5]);
}

static int pinned_slot(const uint8_t address[6]) {
  char wanted[18], line[64], text[32];
  int slot;
  format_address(address, wanted);
  FILE *file = fopen(slots_file(), "r");
  if (!file) return -1;
  int found = -1;
  while (fgets(line, sizeof line, file)) {
    if (sscanf(line, "%31s %d", text, &slot) == 2 && !strcmp(text, wanted)) found = slot;
  }
  fclose(file);
  return found;
}

static void pin_slot(const uint8_t address[6], int slot) {
  if (pinned_slot(address) == slot) return;
  char text[18];
  format_address(address, text);
  FILE *file = fopen(slots_file(), "a");
  if (!file) return;  // Without the file, slots are just chosen in order.
  fprintf(file, "%s %d\n", text, slot);  // The last line for an address wins.
  fclose(file);
}

int link_choose_slot(const uint8_t address[6], int kind) {
  if (kind == LINK_BOARD) return atomic_load(&slot_entry[BLUETOOTH_BOARD]) < 0 ? BLUETOOTH_BOARD : -1;
  int pinned = pinned_slot(address);
  if (pinned >= 0 && pinned < BLUETOOTH_BOARD && atomic_load(&slot_entry[pinned]) < 0) return pinned;
  for (int slot = 0; slot < BLUETOOTH_BOARD; slot++) {
    if (atomic_load(&slot_entry[slot]) < 0) return slot;
  }
  return -1;
}

// The device is known: give it a slot, and it calls the console.
static void take_slot(int id) {
  Entry *entry = &entries[id];
  pthread_mutex_lock(&lock);
  int slot = link_choose_slot(entry->address, entry->kind);
  if (slot >= 0) {
    entry->slot = slot;
    atomic_store(&slot_lost[slot], 0);
    atomic_store(&entry->state, LINKED);
    atomic_store(&entry->wake, 1);
    atomic_store(&slot_entry[slot], id);
    pin_slot(entry->address, slot);
  }
  pthread_mutex_unlock(&lock);
  char text[18];
  format_address(entry->address, text);
  if (slot < 0) {
    fprintf(stderr, "remotes: %s %s, but every slot is taken\n", entry->kind == LINK_BOARD ? "a board" : "a remote", text);
    atomic_store(&entry->state, LOST);
  } else {
    fprintf(stderr, "remotes: %s %s is player %d\n", entry->kind == LINK_BOARD ? "the board" : "remote", text, slot + 1);
  }
}

// --- Asking a device what it is ---

static void probe_write(Entry *entry, uint32_t address, uint8_t value) {
  uint8_t report[22] = {OUTPUT_WRITE, (uint8_t)(address >> 24), (uint8_t)(address >> 16), (uint8_t)(address >> 8), (uint8_t)address, 1, value};
  entry->ops->write(entry->device, report, sizeof report);
}

static void decide(int id, int kind) {
  int expected = PROBING;
  if (!atomic_compare_exchange_strong(&entries[id].state, &expected, DECIDING)) return;  // Decided already.
  entries[id].kind = kind;
  take_slot(id);
}

// The answers, in order: the attachment initialised, unlocked, then its two ID bytes.
static void probe_answer(int id, const uint8_t *report, uint32_t bytes) {
  Entry *entry = &entries[id];
  if (report[0] == REPORT_ACK && bytes >= 5 && report[3] == OUTPUT_WRITE) {
    if (report[4]) { decide(id, LINK_REMOTE); return; }  // Nothing in the attachment port.
    if (entry->probe_step == PROBE_INITIALISE) {
      entry->probe_step = PROBE_UNLOCK;
      probe_write(entry, 0x04a400fb, 0x00);
    } else if (entry->probe_step == PROBE_UNLOCK) {
      entry->probe_step = PROBE_READ;
      const uint8_t read[7] = {OUTPUT_READ, 0x04, 0xa4, 0x00, 0xfe, 0x00, 0x02};
      entry->ops->write(entry->device, read, sizeof read);
    }
  } else if (report[0] == REPORT_MEMORY && bytes >= 8 && entry->probe_step == PROBE_READ) {
    int failed = report[3] & 0x0f;
    decide(id, !failed && report[6] == 0x04 && report[7] == 0x02 ? LINK_BOARD : LINK_REMOTE);
  }
}

int link_attach(const uint8_t address[6], const char *name, const LinkOps *ops, void *device) {
  int id = -1;
  for (int i = 0; i < ENTRIES && id < 0; i++) {
    int expected = FREE;
    if (atomic_compare_exchange_strong(&entries[i].state, &expected, PROBING)) id = i;
  }
  if (id < 0) return -1;
  Entry *entry = &entries[id];
  memcpy(entry->address, address, 6);
  entry->ops = ops;
  entry->device = device;
  entry->slot = -1;
  entry->probe_step = PROBE_INITIALISE;
  entry->probe_started = time_base();
  atomic_store(&entry->head, 0);
  atomic_store(&entry->tail, 0);
  atomic_store(&entry->wake, 0);
  if (name && strstr(name, "RVL-WBC")) decide(id, LINK_BOARD);
  else if (name && strstr(name, "RVL-CNT")) decide(id, LINK_REMOTE);
  else probe_write(entry, 0x04a400f0, 0x55);
  return id;
}

// --- Reports ---

// Every input report from 0x20 to 0x3f starts with the buttons.
static int buttons_held(const uint8_t *report, uint32_t bytes) {
  return report[0] >= REPORT_STATUS && report[0] < 0x40 && bytes >= 3 && ((report[1] & 0x1f) || (report[2] & 0x9f));
}

void link_input(int id, const uint8_t *report, uint32_t bytes) {
  if (id < 0 || id >= ENTRIES || !bytes) return;
  Entry *entry = &entries[id];
  if (bytes > LINK_REPORT_BYTES) bytes = LINK_REPORT_BYTES;
  int state = atomic_load(&entry->state);
  if (state == PROBING) {
    probe_answer(id, report, bytes);
    return;
  }
  if (state == ASLEEP) {
    // Turned away by the game but still connected: a press wakes it, and it calls again.
    if (buttons_held(report, bytes)) {
      atomic_store(&entry->state, LINKED);
      atomic_store(&entry->wake, 1);
    }
    return;
  }
  if (state != LINKED) return;
  unsigned head = atomic_load_explicit(&entry->head, memory_order_relaxed);
  unsigned used = head - atomic_load_explicit(&entry->tail, memory_order_acquire);
  // Data reports give way first, so a status or an answer always has room.
  if (used >= RING || (report[0] >= FIRST_DATA_REPORT && used >= RING_FOR_DATA)) return;
  memcpy(entry->reports[head % RING], report, bytes);
  entry->sizes[head % RING] = (uint8_t)bytes;
  atomic_store_explicit(&entry->head, head + 1, memory_order_release);
}

void link_lost(int id) {
  if (id < 0 || id >= ENTRIES) return;
  int state = atomic_exchange(&entries[id].state, LOST);
  int slot = entries[id].slot;
  if ((state == LINKED || state == ASLEEP) && slot >= 0) atomic_store(&slot_lost[slot], 1);
  else entries[id].slot = -1;  // Never had a slot: link_update closes it.
}

static Entry *entry_at(int slot) {
  int id = atomic_load(&slot_entry[slot]);
  return id >= 0 ? &entries[id] : 0;
}

// What a backend tells the next title the device is, so it needn't ask again.
const char *link_name_of(int id) {
  if (id < 0 || id >= ENTRIES) return "";
  return entries[id].kind == LINK_BOARD ? "Nintendo RVL-WBC-01" : entries[id].kind == LINK_REMOTE ? "Nintendo RVL-CNT-01" : "";
}

int link_linked(int slot) { return slot >= 0 && slot < BLUETOOTH_DEVICES && atomic_load(&slot_entry[slot]) >= 0; }

int link_kind(int slot) {
  Entry *entry = entry_at(slot);
  return entry ? entry->kind : LINK_UNKNOWN;
}

int link_poll(int slot, uint8_t *report) {
  Entry *entry = entry_at(slot);
  if (!entry) return 0;
  unsigned tail = atomic_load_explicit(&entry->tail, memory_order_relaxed);
  if (tail == atomic_load_explicit(&entry->head, memory_order_acquire)) return 0;
  int bytes = entry->sizes[tail % RING];
  memcpy(report, entry->reports[tail % RING], (size_t)bytes);
  atomic_store_explicit(&entry->tail, tail + 1, memory_order_release);
  return bytes;
}

void link_send(int slot, const uint8_t *report, uint32_t bytes) {
  Entry *entry = entry_at(slot);
  if (!entry || atomic_load(&entry->state) != LINKED) return;
  if (entry->ops->write(entry->device, report, bytes) < 0) link_lost((int)(entry - entries));
}

int link_take_wake(int slot) {
  Entry *entry = entry_at(slot);
  return entry && atomic_exchange(&entry->wake, 0);
}

// Frees the slot of a device that went away. The backend stops using it in close.
int link_take_lost(int slot) {
  if (!atomic_exchange(&slot_lost[slot], 0)) return 0;
  int id = atomic_exchange(&slot_entry[slot], -1);
  if (id >= 0) {
    entries[id].ops->close(entries[id].device, 0);
    atomic_store(&entries[id].state, FREE);
  }
  return 1;
}

// On a Wii the remote turns off. A backend that can't turn it off leaves it asleep, and
// a press brings it back.
void link_guest_disconnected(int slot) {
  Entry *entry = entry_at(slot);
  if (!entry) return;
  if (entry->ops->close(entry->device, 1)) {
    atomic_store(&slot_entry[slot], -1);
    atomic_store(&entry->state, FREE);
    fprintf(stderr, "remotes: player %d turned off\n", slot + 1);
  } else {
    atomic_store(&entry->state, ASLEEP);
    fprintf(stderr, "remotes: player %d is asleep until a button is pressed\n", slot + 1);
  }
}

// A device that never answers what it is has no attachment port to ask: a remote.
void link_update(uint64_t now) {
  for (int id = 0; id < ENTRIES; id++) {
    if (atomic_load(&entries[id].state) == PROBING && now - entries[id].probe_started > 2ull * TICKS_PER_SECOND) decide(id, LINK_REMOTE);
    // A device given up on, with no slot to free, is closed here.
    if (atomic_load(&entries[id].state) == LOST && entries[id].slot < 0 && entries[id].ops) {
      entries[id].ops->close(entries[id].device, 0);
      entries[id].ops = 0;
      atomic_store(&entries[id].state, FREE);
    }
  }
}

void link_guest_searching(void) {
  if (link_mode() != LINK_SIMULATED && link_mode() != LINK_LOOPBACK) link_backend_search();
}

void link_prepare_handover(void) {
  if (link_mode() != LINK_SIMULATED && link_mode() != LINK_LOOPBACK) link_backend_handover();
}
