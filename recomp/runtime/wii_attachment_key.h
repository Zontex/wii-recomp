// An attachment's encryption tables from the key the game writes to it.
#pragma once
#include <stdint.h>

// From registers 0x40 to 0x4f as written. Returns 1 when the key is one the Wii makes,
// 0 when the fallback tables were used.
int attachment_key_tables(const uint8_t written[16], uint8_t add[8], uint8_t exclusive[8]);
