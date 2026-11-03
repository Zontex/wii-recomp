// Loads a program into guest memory and sets what the firmware leaves behind.
#pragma once
#include <stddef.h>
#include <stdint.h>

// What starting a program needs: the program file, the disc header and the disc's file table.
typedef struct {
  uint8_t header[0x440];
  uint8_t *program, *table;
  size_t program_bytes, table_bytes;
} Boot;

// From a folder holding boot.bin, main.dol and fst.bin, as an extracted disc's or a
// system title's, or from the disc in the drive. Both stop the program if they can't.
void boot_from_folder(const char *folder, Boot *boot);
void boot_from_disc(Boot *boot);

uint32_t load_program(const Boot *boot);  // Returns the entry point.
void set_up_low_memory(const Boot *boot);
