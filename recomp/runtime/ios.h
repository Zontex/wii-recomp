// Requests the game can send to IOS, and the devices that answer them.
#pragma once
#include <stdint.h>

enum { IOS_OPEN = 1, IOS_CLOSE = 2, IOS_READ = 3, IOS_WRITE = 4, IOS_SEEK = 5, IOS_IOCTL = 6, IOS_IOCTLV = 7 };

int ios_find_device(const char *path);
int ios_device_request(int device, uint32_t command, uint32_t request);
void ios_complete(uint32_t request, int result);

void dvd_start(const char *extracted_system_folder);
int dvd_request(uint32_t command, uint32_t request);
int es_request(uint32_t command, uint32_t request);

// A device answer meaning "no reply yet": the request stays pending.
#define IOS_NO_REPLY (-0x7fffffff)

void files_start(const char *folder);
void files_save_path(const char *path, char *out, unsigned long size);
int files_storage_path(const char *path, char *out, unsigned long size);
void files_make_parents(const char *full);
int files_find(const char *path, char *out, unsigned long size);
int fs_request(uint32_t command, uint32_t request);
int files_open(const char *path);
void files_close(int id);
int files_request(int id, uint32_t command, uint32_t request);
