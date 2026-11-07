// The console's own certificate authority, added to Opera's trusted list as a title reads
// it (opera_trust.c).
#pragma once
#include <stdint.h>

// If data, a whole content of size bytes, is a U8 archive with Opera's list of trusted
// authorities, adds the console's own to it, reallocating data and changing size.
// Returns 1 when it did.
int opera_trust_add(uint8_t **data, long *size);
