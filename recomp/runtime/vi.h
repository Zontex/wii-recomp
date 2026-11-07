// The video interface's timing (vi.c): fields, the beam, and display interrupts.
#pragma once
#include <stdint.h>

void vi_update(uint64_t now);       // Fires what the beam has passed since the last call.
uint64_t vi_next_due(uint64_t now);  // When the beam reaches its next half-line.
void vi_timing_changed(void);       // A timing register was written.
int vi_read(uint32_t address, uint32_t *value);  // The beam position registers.
double vi_field_rate(void);
int vi_is_pal(void);
int vi_is_progressive(void);
