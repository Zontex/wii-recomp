// Real remotes through the kernel's HID on Linux (remote_link_hidraw.c).
#pragma once
#include <stddef.h>
#include <stdint.h>

void link_hidraw_start(void);  // Watches for remotes and boards that connect through bluetoothd.

// Exposed for testing.
int link_hidraw_is_remote(int bus, int vendor, int product);
int link_hidraw_address(const char *uniq, uint8_t address[6]);  // From "aa:bb:cc:dd:ee:ff"; 0 if it isn't one.
// The kernel driver holding a hidraw device, from sysfs; 0 if none.
int link_hidraw_driver(const char *class_folder, const char *node, char *driver, size_t bytes);
// A device that's open already, given its hidraw name, address and HID name.
int link_hidraw_adopt(int fd, const char *node, const uint8_t address[6], const char *name);
