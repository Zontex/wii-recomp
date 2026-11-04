// Requests the game can send to IOS, and the devices that answer them.
#pragma once
#include <stdint.h>

enum { IOS_OPEN = 1, IOS_CLOSE = 2, IOS_READ = 3, IOS_WRITE = 4, IOS_SEEK = 5, IOS_IOCTL = 6, IOS_IOCTLV = 7 };

// Answers real IOS gives: a bad argument, and a device or file that doesn't exist.
enum { IOS_EINVAL = -4, IOS_ENOENT = -6, FS_ENOENT = -106 };

// This console's number, made up. The real one is never read.
enum { IOS_DEVICE_ID = 0x0403ac68 };

int ios_find_device(const char *path);
const char *ios_device_name(int device);
int ios_device_request(int device, uint32_t command, uint32_t request);
void ios_complete(uint32_t request, int result);

// A device answer meaning "no reply yet": the request stays pending.
#define IOS_NO_REPLY (-0x7fffffff)
// A device answer meaning "not modelled": the game gets IOS_EINVAL, and the trace says so.
#define IOS_UNHANDLED (-0x7ffffffe)

void ios_trace(const char *device, uint32_t command, uint32_t request, int result);
int ios_trace_all(void);

int dvd_request(uint32_t command, uint32_t request);
int es_request(uint32_t command, uint32_t request);
int stm_immediate(uint32_t command, uint32_t request);
int stm_event_hook(uint32_t command, uint32_t request);
void stm_reset_button(void);
int net_kd_request(uint32_t command, uint32_t request);
int net_kd_time(uint32_t command, uint32_t request);
int net_ncd_manage(uint32_t command, uint32_t request);
int sdio_request(uint32_t command, uint32_t request);
int usb_oh0_request(uint32_t command, uint32_t request);
int usb_hid_request(uint32_t command, uint32_t request);
int usb_kbd_request(uint32_t command, uint32_t request);
int aes_request(uint32_t command, uint32_t request);
int sha_request(uint32_t command, uint32_t request);
void wc24_request_user_id(uint8_t *out);
int console_setting(const char *name, char *out, size_t limit);  // From setting.txt; 0 if it isn't there.
// The disc's ticket, beside the extracted system folder. Returns 0 if there's none.

void files_start(const char *folder);
void files_save_path(const char *path, char *out, unsigned long size);
int files_storage_path(const char *path, char *out, unsigned long size);
void files_make_parents(const char *full);
int files_find(const char *path, char *out, unsigned long size);
int fs_request(uint32_t command, uint32_t request);
int files_open(const char *path, uint32_t mode);  // A handle, or an FS error such as FS_ENOENT.
void files_save_all(void);
void files_close(int id);
const char *files_name(int id);  // The path an open file was opened by.
int files_request(int id, uint32_t command, uint32_t request);
