// Real Wii Remotes and Balance Boards on Linux through the kernel's HID (remote_link.h),
// the other way to the raw sockets in remote_link_linux.c. bluetoothd's input plugin
// takes a remote's connection, as it does a keyboard's or a Switch controller's, and the
// kernel gives it a hidraw device; this reads its reports there and writes the game's.
// The Mac backend works the same way through IOKit.
//
// So the input plugin can stay on, and wireless Switch controllers (pads.c) work at the
// same time. The kernel's own Wii Remote driver, hid-wiimote, must leave remotes alone,
// as it would send them reports of its own: recomp/tools/remotes_setup.sh keeps it from
// loading, and a device it holds is refused here, saying so.
//
// hidraw devices are watched for as they come, with inotify on /dev, so a remote that
// connects mid-game, or is still connected when the console hands over to the next title,
// is taken up at once. The game hanging up can't turn a remote off from here, so it
// sleeps until a button is pressed, as on the Mac.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "remote_link.h"
#include "remote_link_hidraw.h"

enum { MOST = 8, BUFFER_BYTES = 64, BUS_BLUETOOTH_ = 5, NINTENDO = 0x057e, REMOTE = 0x0306, REMOTE_PLUS = 0x0330 };

typedef struct {
  int fd, stop, id, open;
  char node[32];
  pthread_t reader;
} Device;

static Device devices[MOST];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

int link_hidraw_is_remote(int bus, int vendor, int product) {
  return bus == BUS_BLUETOOTH_ && vendor == NINTENDO && (product == REMOTE || product == REMOTE_PLUS);
}

int link_hidraw_address(const char *uniq, uint8_t address[6]) {
  unsigned parts[6];
  char end;
  if (sscanf(uniq, "%2x:%2x:%2x:%2x:%2x:%2x%c", &parts[0], &parts[1], &parts[2], &parts[3], &parts[4], &parts[5], &end) != 6) return 0;
  for (int i = 0; i < 6; i++) address[i] = (uint8_t)parts[i];
  return 1;
}

int link_hidraw_driver(const char *class_folder, const char *node, char *driver, size_t bytes) {
  char path[PATH_MAX], target[PATH_MAX];
  snprintf(path, sizeof path, "%s/%s/device/driver", class_folder, node);
  ssize_t length = readlink(path, target, sizeof target - 1);
  if (length <= 0) return 0;
  target[length] = 0;
  const char *name = strrchr(target, '/');
  snprintf(driver, bytes, "%s", name ? name + 1 : target);
  return 1;
}

static int device_write(void *handle, const uint8_t *report, uint32_t bytes) {
  Device *device = handle;
  if (!device->open) return -1;
  return write(device->fd, report, bytes) == (ssize_t)bytes ? (int)bytes : -1;
}

// The connection stays: the remote sleeps until a button is pressed, and its reports
// carry on arriving here, which is how the core sees it wake.
static int device_close(void *handle, int power_off) {
  (void)handle;
  (void)power_off;
  return 0;
}

static const LinkOps ops = {device_write, device_close};

static void *read_reports(void *handle) {
  Device *device = handle;
  uint8_t report[BUFFER_BYTES];
  struct pollfd waiting[2] = {{device->fd, POLLIN, 0}, {device->stop, POLLIN, 0}};
  for (;;) {
    if (poll(waiting, 2, -1) < 0 && errno != EINTR) break;
    if (waiting[1].revents) return 0;
    if (!(waiting[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
    ssize_t bytes = read(device->fd, report, sizeof report);
    if (bytes <= 0) break;
    link_input(device->id, report, (uint32_t)bytes);  // hidraw starts each report with its ID, as the core wants.
  }
  // Gone: out of range, its batteries, or switched off by its power button.
  pthread_mutex_lock(&lock);
  close(device->fd);
  close(device->stop);
  device->open = 0;
  pthread_mutex_unlock(&lock);
  link_lost(device->id);
  return 0;
}

int link_hidraw_adopt(int fd, const char *node, const uint8_t address[6], const char *name) {
  pthread_mutex_lock(&lock);
  Device *device = 0;
  for (int i = 0; i < MOST && !device; i++) {
    if (!devices[i].open) device = &devices[i];
  }
  if (!device) {
    pthread_mutex_unlock(&lock);
    close(fd);
    return -1;
  }
  device->fd = fd;
  device->stop = eventfd(0, EFD_CLOEXEC);
  device->open = 1;
  device->id = -1;
  snprintf(device->node, sizeof device->node, "%s", node);
  pthread_mutex_unlock(&lock);
  device->id = link_attach(address, name, &ops, device);
  if (device->id < 0) {
    pthread_mutex_lock(&lock);
    close(fd);
    close(device->stop);
    device->open = 0;
    pthread_mutex_unlock(&lock);
    return -1;
  }
  pthread_create(&device->reader, 0, read_reports, device);
  pthread_detach(device->reader);
  return device->id;
}

static int adopted(const char *node) {
  pthread_mutex_lock(&lock);
  int found = 0;
  for (int i = 0; i < MOST; i++) found |= devices[i].open && !strcmp(devices[i].node, node);
  pthread_mutex_unlock(&lock);
  return found;
}

// A hidraw device that's a remote or board over Bluetooth is taken up; anything else is
// left alone. Returns 1 if it should be tried again later, as when udev hasn't yet given
// it the permissions remotes_setup.sh asks for.
static int consider(const char *node) {
  if (strncmp(node, "hidraw", 6) || adopted(node)) return 0;
  char path[PATH_MAX];
  snprintf(path, sizeof path, "/dev/%s", node);
  int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0) return errno == EACCES;
  struct hidraw_devinfo info;
  char name[64] = "", uniq[64] = "";
  uint8_t address[6];
  if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0 || !link_hidraw_is_remote(info.bustype, (uint16_t)info.vendor, (uint16_t)info.product) ||
      ioctl(fd, HIDIOCGRAWNAME(sizeof name), name) < 0 || ioctl(fd, HIDIOCGRAWUNIQ(sizeof uniq), uniq) < 0 ||
      !link_hidraw_address(uniq, address)) {
    close(fd);
    return 0;
  }
  char driver[64];
  if (link_hidraw_driver("/sys/class/hidraw", node, driver, sizeof driver) && !strcmp(driver, "wiimote")) {
    fprintf(stderr, "remotes: the kernel's Wii Remote driver holds %s %s, and would send it reports of its own, so it isn't used: "
                    "run sudo recomp/tools/remotes_setup.sh, then connect it again\n", name, uniq);
    close(fd);
    return 0;
  }
  if (link_hidraw_adopt(fd, node, address, name) < 0) fprintf(stderr, "remotes: %s %s connected, but every slot is taken\n", name, uniq);
  return 0;
}

static int scan(void) {
  DIR *folder = opendir("/dev");
  if (!folder) return 0;
  int retry = 0;
  for (struct dirent *entry; (entry = readdir(folder));) retry |= consider(entry->d_name);
  closedir(folder);
  return retry;
}

static void *watch(void *unused) {
  (void)unused;
  int watcher = inotify_init1(IN_CLOEXEC);
  if (watcher < 0 || inotify_add_watch(watcher, "/dev", IN_CREATE | IN_ATTRIB) < 0) {
    fprintf(stderr, "remotes: can't watch for remotes connecting through HID: %s\n", strerror(errno));
    return 0;
  }
  int retry = scan();
  char events[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
  for (;;) {
    // A device udev hasn't finished with is looked at again shortly.
    struct pollfd waiting = {watcher, POLLIN, 0};
    int ready = poll(&waiting, 1, retry ? 500 : -1);
    if (ready < 0 && errno != EINTR) break;
    if (ready > 0 && read(watcher, events, sizeof events) <= 0) break;
    retry = scan();
  }
  close(watcher);
  return 0;
}

void link_hidraw_start(void) {
  pthread_t thread;
  pthread_create(&thread, 0, watch, 0);
  pthread_detach(thread);
}
