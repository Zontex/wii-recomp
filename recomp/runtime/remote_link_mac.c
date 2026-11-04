// Real Wii Remotes on the Mac (remote_link.h), through IOKit's HID manager. macOS keeps
// the Bluetooth connection itself: pair a remote once in System Settings > Bluetooth,
// holding 1 and 2 (or SYNC), and from then on it appears here as a HID device whose
// reports pass straight through. Dolphin moved to this route in 2021 for the same reason:
// macOS's own Bluetooth stack does the pairing and the radio.
//
// macOS can't be told to switch a remote off, so when the game hangs up, the remote
// sleeps: the core ignores it until a button is pressed (remote_link.c).
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "remote_link.h"

enum { NINTENDO = 0x057e, REMOTE = 0x0306, REMOTE_PLUS = 0x0330, MOST = 8, BUFFER_BYTES = 64 };

typedef struct {
  IOHIDDeviceRef device;
  int id;
  uint8_t buffer[BUFFER_BYTES];
} Remote;

static Remote remotes[MOST];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int remote_write(void *handle, const uint8_t *report, uint32_t bytes) {
  Remote *remote = handle;
  if (!remote->device || !bytes) return -1;
  // The report ID goes first in the data, as with any HID device that numbers its reports.
  IOReturn result = IOHIDDeviceSetReport(remote->device, kIOHIDReportTypeOutput, report[0], report, bytes);
  return result == kIOReturnSuccess ? (int)bytes : -1;
}

static int remote_close(void *handle, int power_off) {
  Remote *remote = handle;
  if (power_off && remote->device) return 0;  // Still connected: it sleeps until a press.
  pthread_mutex_lock(&lock);
  if (remote->device) {
    IOHIDDeviceRegisterInputReportCallback(remote->device, remote->buffer, sizeof remote->buffer, 0, 0);
    IOHIDDeviceClose(remote->device, kIOHIDOptionsTypeNone);
    CFRelease(remote->device);
    remote->device = 0;
  }
  remote->id = -1;
  pthread_mutex_unlock(&lock);
  return 1;
}

static const LinkOps ops = {remote_write, remote_close};

static void on_report(void *context, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id, uint8_t *report,
                      CFIndex bytes) {
  Remote *remote = context;
  (void)sender, (void)type, (void)report_id;
  if (result == kIOReturnSuccess && bytes > 0 && remote->id >= 0) link_input(remote->id, report, (uint32_t)bytes);
}

static void text_property(IOHIDDeviceRef device, CFStringRef key, char *out, size_t size) {
  out[0] = 0;
  CFTypeRef value = IOHIDDeviceGetProperty(device, key);
  if (value && CFGetTypeID(value) == CFStringGetTypeID()) CFStringGetCString(value, out, (CFIndex)size, kCFStringEncodingUTF8);
}

// The serial number is the remote's Bluetooth address, written as text.
static void address_of(IOHIDDeviceRef device, uint8_t address[6]) {
  char serial[64];
  unsigned parts[6] = {0};
  text_property(device, CFSTR(kIOHIDSerialNumberKey), serial, sizeof serial);
  memset(address, 0, 6);
  if (sscanf(serial, "%2x-%2x-%2x-%2x-%2x-%2x", &parts[0], &parts[1], &parts[2], &parts[3], &parts[4], &parts[5]) == 6 ||
      sscanf(serial, "%2x:%2x:%2x:%2x:%2x:%2x", &parts[0], &parts[1], &parts[2], &parts[3], &parts[4], &parts[5]) == 6) {
    for (int i = 0; i < 6; i++) address[i] = (uint8_t)parts[i];
  }
}

static void on_arrival(void *context, IOReturn result, void *sender, IOHIDDeviceRef device) {
  (void)context, (void)result, (void)sender;
  pthread_mutex_lock(&lock);
  Remote *remote = 0;
  for (int i = 0; i < MOST && !remote; i++) {
    if (!remotes[i].device) remote = &remotes[i];
  }
  if (!remote || IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
    pthread_mutex_unlock(&lock);
    fprintf(stderr, "remotes: a Wii Remote is paired, but it couldn't be opened\n");
    return;
  }
  CFRetain(device);
  remote->device = device;
  remote->id = -1;
  IOHIDDeviceRegisterInputReportCallback(device, remote->buffer, sizeof remote->buffer, on_report, remote);
  pthread_mutex_unlock(&lock);
  char name[64];
  uint8_t address[6];
  text_property(device, CFSTR(kIOHIDProductKey), name, sizeof name);
  address_of(device, address);
  // A remote macOS remembers but that's switched off still appears; a status request it
  // can't answer shows that.
  const uint8_t status[2] = {0x15, 0};
  if (remote_write(remote, status, sizeof status) < 0) {
    fprintf(stderr, "remotes: %s is paired but switched off; press a button on it\n", name[0] ? name : "a Wii Remote");
  }
  remote->id = link_attach(address, name, &ops, remote);
}

static void on_removal(void *context, IOReturn result, void *sender, IOHIDDeviceRef device) {
  (void)context, (void)result, (void)sender;
  for (int i = 0; i < MOST; i++) {
    if (remotes[i].device == device && remotes[i].id >= 0) link_lost(remotes[i].id);
  }
}

static CFDictionaryRef matching(int product) {
  CFMutableDictionaryRef dictionary = CFDictionaryCreateMutable(kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks,
                                                                &kCFTypeDictionaryValueCallBacks);
  int vendor = NINTENDO;
  CFNumberRef vendor_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendor);
  CFNumberRef product_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &product);
  CFDictionarySetValue(dictionary, CFSTR(kIOHIDVendorIDKey), vendor_number);
  CFDictionarySetValue(dictionary, CFSTR(kIOHIDProductIDKey), product_number);
  CFRelease(vendor_number);
  CFRelease(product_number);
  return dictionary;
}

// The HID manager must be made, used and closed on one thread, which runs its run loop.
static void *manager_thread(void *unused) {
  (void)unused;
  IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
  CFDictionaryRef both[2] = {matching(REMOTE), matching(REMOTE_PLUS)};
  CFArrayRef list = CFArrayCreate(kCFAllocatorDefault, (const void **)both, 2, &kCFTypeArrayCallBacks);
  IOHIDManagerSetDeviceMatchingMultiple(manager, list);
  IOHIDManagerRegisterDeviceMatchingCallback(manager, on_arrival, 0);
  IOHIDManagerRegisterDeviceRemovalCallback(manager, on_removal, 0);
  IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
  if (IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
    fprintf(stderr, "remotes: macOS refused access to Bluetooth devices; allow Input Monitoring for this program\n");
  }
  CFRunLoopRun();
  return 0;
}

void link_backend_start(int mode) {
  (void)mode;
  for (int i = 0; i < MOST; i++) remotes[i].id = -1;
  pthread_t thread;
  pthread_create(&thread, 0, manager_thread, 0);
  pthread_detach(thread);
}

// Pairing happens in System Settings, so there's nothing to search for here.
void link_backend_search(void) {}

// macOS keeps the connection, and the next title finds the remotes again as it starts.
void link_backend_handover(void) {}
