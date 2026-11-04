// Real Wii Remotes on Linux (remote_link.h), over raw Bluetooth L2CAP sockets, as a Wii
// talks to them: a control channel (PSM 0x11) and an interrupt channel (PSM 0x13) that
// carries the HID reports. The kernel's HID layer and its hid-wiimote driver never see
// the remote, so nothing else sends it reports. Dolphin's IOLinux.cpp works the same way.
//
// Remotes reach the console two ways:
//   - A remote paired with this machine (SYNC; see recomp/tools/remotes_setup.sh) calls
//     it when any button is pressed. We listen on both PSMs to answer, which needs
//     bluetoothd's input plugin off, as that setup script does.
//   - A remote in pairing mode (1+2, or SYNC) is found by a search, which runs when the
//     game searches, and once at start. It's linked for this session only.
//
// When the game hangs up, the sockets close and the remote turns off, as on a Wii. Across
// the console's handover to the next title the sockets stay open: their numbers go to the
// next program in WII_RECOMP_LINKS, and the remotes call the new title.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "remote_link.h"

// From the kernel's Bluetooth headers, so there's no library to install.
enum { AF_BT = 31, PROTO_L2CAP = 0, PROTO_HCI = 1, PSM_CONTROL = 0x11, PSM_INTERRUPT = 0x13 };
typedef struct { uint8_t b[6]; } __attribute__((packed)) BdAddress;  // Least significant byte first.
struct l2cap_address {
  sa_family_t family;
  unsigned short psm;
  BdAddress address;
  unsigned short cid;
  uint8_t address_type;
};
struct hci_address { sa_family_t family; unsigned short device, channel; };
struct hci_filter { uint32_t type_mask, event_mask[2]; uint16_t opcode; };
struct inquiry_request { uint16_t device, flags; uint8_t lap[3], length, most; } __attribute__((packed));
struct inquiry_answer { BdAddress address; uint8_t repetition, period, mode, device_class[3]; uint16_t clock_offset; } __attribute__((packed));
#define HCI_INQUIRY _IOR('H', 240, int)
enum { SOL_HCI_ = 0, HCI_FILTER_ = 2, HCI_COMMAND = 1, HCI_EVENT = 4, EVENT_STATUS = 0x0f, EVENT_NAME = 0x07, FLUSH_CACHE = 1 };

enum { MOST = 8, HID_INPUT = 0xa1, HID_OUTPUT = 0xa2, BUFFER_BYTES = 64, NAME_BYTES = 248 };

typedef struct {
  int control, interrupt, stop, id, open;
  uint8_t address[6];
  char name[64];
  pthread_t reader;
} Remote;

static Remote remotes[MOST];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int searching;

static void to_bd(const uint8_t address[6], BdAddress *bd) { for (int i = 0; i < 6; i++) bd->b[i] = address[5 - i]; }
static void from_bd(const BdAddress *bd, uint8_t address[6]) { for (int i = 0; i < 6; i++) address[i] = bd->b[5 - i]; }

static int remote_write(void *handle, const uint8_t *report, uint32_t bytes) {
  Remote *remote = handle;
  uint8_t frame[BUFFER_BYTES] = {HID_OUTPUT};
  if (!remote->open || bytes + 1 > sizeof frame) return -1;
  memcpy(frame + 1, report, bytes);
  return send(remote->interrupt, frame, bytes + 1, MSG_NOSIGNAL) == (ssize_t)(bytes + 1) ? (int)bytes : -1;
}

// Closing both channels ends the connection, and a remote with no connection turns off.
static int remote_close(void *handle, int power_off) {
  Remote *remote = handle;
  (void)power_off;
  pthread_mutex_lock(&lock);
  if (remote->open) {
    uint64_t one = 1;
    if (write(remote->stop, &one, sizeof one) < 0) perror("remotes");
    pthread_join(remote->reader, 0);
    close(remote->interrupt);
    close(remote->control);
    close(remote->stop);
    remote->open = 0;
  }
  remote->id = -1;
  pthread_mutex_unlock(&lock);
  return 1;
}

static const LinkOps ops = {remote_write, remote_close};

static void *read_reports(void *handle) {
  Remote *remote = handle;
  uint8_t frame[BUFFER_BYTES];
  struct pollfd waiting[2] = {{remote->interrupt, POLLIN, 0}, {remote->stop, POLLIN, 0}};
  for (;;) {
    if (poll(waiting, 2, -1) < 0 && errno != EINTR) break;
    if (waiting[1].revents) return 0;  // Closed by the core: it frees the slot itself.
    if (!(waiting[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
    ssize_t bytes = recv(remote->interrupt, frame, sizeof frame, 0);
    if (bytes <= 0) break;
    if (frame[0] == HID_INPUT && bytes > 1) link_input(remote->id, frame + 1, (uint32_t)(bytes - 1));
  }
  link_lost(remote->id);  // Out of range, or its batteries went.
  return 0;
}

// A connected pair of channels becomes a linked device. Also how tests plug in sockets.
int link_linux_adopt(const uint8_t address[6], const char *name, int control, int interrupt) {
  pthread_mutex_lock(&lock);
  Remote *remote = 0;
  for (int i = 0; i < MOST && !remote; i++) {
    if (!remotes[i].open) remote = &remotes[i];
  }
  if (!remote) {
    pthread_mutex_unlock(&lock);
    close(control);
    close(interrupt);
    return -1;
  }
  remote->control = control;
  remote->interrupt = interrupt;
  remote->stop = eventfd(0, EFD_CLOEXEC);
  remote->open = 1;
  remote->id = -1;
  memcpy(remote->address, address, 6);
  snprintf(remote->name, sizeof remote->name, "%s", name ? name : "");
  pthread_mutex_unlock(&lock);
  // The id comes before any report is read, as the reader thread starts after.
  remote->id = link_attach(address, name, &ops, remote);
  if (remote->id < 0) {
    remote->open = 0;
    close(control), close(interrupt), close(remote->stop);
    return -1;
  }
  pthread_create(&remote->reader, 0, read_reports, remote);
  return remote->id;
}

static int l2cap_socket(void) { return socket(AF_BT, SOCK_SEQPACKET | SOCK_CLOEXEC, PROTO_L2CAP); }

static int l2cap_connect(const uint8_t address[6], int psm) {
  int channel = l2cap_socket();
  struct l2cap_address to = {AF_BT, (unsigned short)psm, {{0}}, 0, 0};
  to_bd(address, &to.address);
  for (int attempt = 0; channel >= 0 && attempt < 3; attempt++) {
    if (!connect(channel, (struct sockaddr *)&to, sizeof to)) return channel;
    usleep(500000);
  }
  if (channel >= 0) close(channel);
  return -1;
}

// --- Answering remotes that call ---

static int l2cap_listen(int psm) {
  int listener = l2cap_socket();
  struct l2cap_address at = {AF_BT, (unsigned short)psm, {{0}}, 0, 0};
  if (listener < 0) return -1;
  if (bind(listener, (struct sockaddr *)&at, sizeof at) || listen(listener, 4)) {
    close(listener);
    return -1;
  }
  return listener;
}

static void *answer_calls(void *unused) {
  (void)unused;
  int control = l2cap_listen(PSM_CONTROL), interrupt = l2cap_listen(PSM_INTERRUPT);
  if (control < 0 || interrupt < 0) {
    fprintf(stderr, "remotes: can't answer remotes that call (%s), so press 1 and 2 to link one.\n"
                    "remotes: recomp/tools/remotes_setup.sh lets paired remotes wake the console\n", strerror(errno));
    return 0;
  }
  // A remote opens control, then interrupt. The two are matched by address.
  struct { int control; uint8_t address[6]; } pending[MOST];
  int pending_count = 0;
  for (;;) {
    struct pollfd waiting[2] = {{control, POLLIN, 0}, {interrupt, POLLIN, 0}};
    if (poll(waiting, 2, -1) < 0) continue;
    for (int which = 0; which < 2; which++) {
      if (!(waiting[which].revents & POLLIN)) continue;
      struct l2cap_address from;
      socklen_t size = sizeof from;
      int channel = accept4(waiting[which].fd, (struct sockaddr *)&from, &size, SOCK_CLOEXEC);
      if (channel < 0) continue;
      uint8_t address[6];
      from_bd(&from.address, address);
      if (which == 0) {
        if (pending_count == MOST) close(pending[--pending_count].control);
        pending[pending_count].control = channel;
        memcpy(pending[pending_count++].address, address, 6);
        continue;
      }
      int matched = -1;
      for (int i = 0; i < pending_count; i++) if (!memcmp(pending[i].address, address, 6)) matched = i;
      if (matched < 0) {
        close(channel);
        continue;
      }
      int its_control = pending[matched].control;
      pending[matched] = pending[--pending_count];
      link_linux_adopt(address, 0, its_control, channel);
    }
  }
  return 0;
}

// --- Searching for remotes in pairing mode ---

// Asks a found device its name, which says whether it's a remote or a board. The kernel
// lets any user send this one command.
static int remote_name(int device, const BdAddress *address, const struct inquiry_answer *found, char *name) {
  int hci = socket(AF_BT, SOCK_RAW | SOCK_CLOEXEC, PROTO_HCI);
  struct hci_address at = {AF_BT, (unsigned short)device, 0};
  if (hci < 0 || bind(hci, (struct sockaddr *)&at, sizeof at)) {
    if (hci >= 0) close(hci);
    return -1;
  }
  struct hci_filter filter = {1u << HCI_EVENT, {(1u << EVENT_NAME) | (1u << EVENT_STATUS), 0}, 0};
  setsockopt(hci, SOL_HCI_, HCI_FILTER_, &filter, sizeof filter);
  uint8_t command[4 + 10] = {HCI_COMMAND, 0x19, 0x04, 10};
  memcpy(command + 4, address, 6);
  command[10] = found->repetition;
  memcpy(command + 12, &found->clock_offset, 2);
  int answered = -1;
  if (write(hci, command, sizeof command) == (ssize_t)sizeof command) {
    uint8_t event[3 + 1 + 6 + NAME_BYTES];
    struct pollfd waiting = {hci, POLLIN, 0};
    while (answered < 0 && poll(&waiting, 1, 5000) > 0) {
      ssize_t bytes = read(hci, event, sizeof event);
      if (bytes > 10 && event[1] == EVENT_NAME && !memcmp(event + 4, address, 6)) {
        answered = event[3];
        if (!answered) snprintf(name, 64, "%.*s", (int)(bytes - 10), (const char *)event + 10);
      }
    }
  }
  close(hci);
  return answered == 0 ? 0 : -1;
}

static void *search(void *unused) {
  (void)unused;
  int hci = socket(AF_BT, SOCK_RAW | SOCK_CLOEXEC, PROTO_HCI);
  struct {
    struct inquiry_request request;
    struct inquiry_answer answers[16];
  } inquiry;
  memset(&inquiry, 0, sizeof inquiry);
  // Eight rounds of 1.28 s, asking for devices in pairing mode (the LIAC, 0x9e8b00), as a Wii does.
  inquiry.request = (struct inquiry_request){0, FLUSH_CACHE, {0x00, 0x8b, 0x9e}, 8, 16};
  if (hci < 0 || ioctl(hci, HCI_INQUIRY, &inquiry) < 0) {
    if (hci >= 0) close(hci);
    fprintf(stderr, "remotes: can't search for remotes: %s\n", strerror(errno));
    searching = 0;
    return 0;
  }
  close(hci);
  for (int i = 0; i < inquiry.request.most; i++) {
    char name[64] = "";
    uint8_t address[6];
    from_bd(&inquiry.answers[i].address, address);
    if (remote_name(0, &inquiry.answers[i].address, &inquiry.answers[i], name) || strncmp(name, "Nintendo RVL-", 13)) continue;
    int known = 0;
    for (int r = 0; r < MOST; r++) known |= remotes[r].open && !memcmp(remotes[r].address, address, 6);
    if (known) continue;
    int control = l2cap_connect(address, PSM_CONTROL), interrupt = control >= 0 ? l2cap_connect(address, PSM_INTERRUPT) : -1;
    if (interrupt < 0) {
      if (control >= 0) close(control);
      fprintf(stderr, "remotes: found %s but couldn't connect to it\n", name);
      continue;
    }
    link_linux_adopt(address, name, control, interrupt);
  }
  searching = 0;
  return 0;
}

void link_backend_search(void) {
  if (__atomic_exchange_n(&searching, 1, __ATOMIC_SEQ_CST)) return;
  pthread_t thread;
  pthread_create(&thread, 0, search, 0);
  pthread_detach(thread);
}

// --- Handing over to the next title ---

void link_backend_handover(void) {
  char list[1024] = "";
  size_t used = 0;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < MOST; i++) {
    if (!remotes[i].open || remotes[i].id < 0) continue;
    fcntl(remotes[i].control, F_SETFD, 0);  // Kept open through exec.
    fcntl(remotes[i].interrupt, F_SETFD, 0);
    const uint8_t *a = remotes[i].address;
    used += (size_t)snprintf(list + used, sizeof list - used, "%s%02x:%02x:%02x:%02x:%02x:%02x,%d,%d,%s", used ? ";" : "",
                             a[0], a[1], a[2], a[3], a[4], a[5], remotes[i].control, remotes[i].interrupt, link_name_of(remotes[i].id));
  }
  pthread_mutex_unlock(&lock);
  if (used) setenv("WII_RECOMP_LINKS", list, 1);
}

// Takes up the devices the last title handed over. Returns how many.
int link_linux_take_over(const char *list) {
  int taken = 0;
  while (list && *list) {
    unsigned a[6];
    int control, interrupt, length = 0;
    char name[64] = "";
    if (sscanf(list, "%2x:%2x:%2x:%2x:%2x:%2x,%d,%d,%63[^;]%n", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &control, &interrupt, name, &length) < 8) break;
    uint8_t address[6];
    for (int i = 0; i < 6; i++) address[i] = (uint8_t)a[i];
    fcntl(control, F_SETFD, FD_CLOEXEC);
    fcntl(interrupt, F_SETFD, FD_CLOEXEC);
    if (link_linux_adopt(address, name[0] ? name : 0, control, interrupt) >= 0) taken++;
    list += length;
    if (*list == ';') list++;
    if (!length) break;
  }
  return taken;
}

void link_backend_start(int mode) {
  (void)mode;
  int taken = link_linux_take_over(getenv("WII_RECOMP_LINKS"));
  unsetenv("WII_RECOMP_LINKS");
  pthread_t thread;
  pthread_create(&thread, 0, answer_calls, 0);
  pthread_detach(thread);
  if (!taken) link_backend_search();  // A remote already in pairing mode links at once.
}
