// Loads the game into guest memory and sets what the firmware leaves behind.
#pragma once
#include <stdint.h>
#include <stddef.h>

size_t read_file(const char *folder, const char *name, uint8_t *target, size_t limit);
void set_up_low_memory(const char *folder);
uint32_t load_program(const char *path);
